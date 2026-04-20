#include "rtsp_handlers.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "audio_stream.h"
#include "ntp_clock.h"
#include "plist.h"
#include "rtsp_rsa.h"
#include "settings.h"
#include "socket_utils.h"

#include "rtsp_events.h"
#include "esp_heap_caps.h"

static const char *TAG = "rtsp_handlers";
#define RTSP_MAX_ARTWORK_BYTES ((size_t)2U * 1024U * 1024U)

// ============================================================================
// Codec Registry
// ============================================================================
// To add a new codec, add an entry to codec_registry[] below.

static void configure_codec(audio_format_t *fmt, const char *name, int64_t sr,
                            int64_t spf) {
  strncpy(fmt->codec, name, sizeof(fmt->codec) - 1);
  fmt->codec[sizeof(fmt->codec) - 1] = '\0';
  fmt->sample_rate = (int)sr;
  fmt->channels = 2;
  fmt->bits_per_sample = 16;
  fmt->frame_size = (int)spf;
  fmt->max_samples_per_frame = (uint32_t)spf;
  fmt->sample_size = 16;
  fmt->num_channels = 2;
  fmt->sample_rate_config = (uint32_t)sr;
}

// Codec registry - add new codecs here
// ct values: 2=ALAC, 4=AAC, 8=AAC-ELD, 64=OPUS (based on AirPlay 2 protocol)
static const rtsp_codec_t codec_registry[] = {
    {"ALAC", 2}, {"AAC", 4}, {"AAC-ELD", 8}, {"OPUS", 64}, {NULL, 0}};

bool rtsp_codec_configure(int64_t type_id, audio_format_t *fmt,
                          int64_t sample_rate, int64_t samples_per_frame) {
  for (const rtsp_codec_t *codec = codec_registry; codec->name; codec++) {
    if (codec->type_id == type_id) {
      configure_codec(fmt, codec->name, sample_rate, samples_per_frame);
      ESP_LOGI(TAG, "Configured codec: %s (ct=%lld, sr=%lld, spf=%lld)",
               codec->name, (long long)type_id, (long long)sample_rate,
               (long long)samples_per_frame);
      return true;
    }
  }
  // Default to ALAC if unknown codec type
  ESP_LOGW(TAG, "Unknown codec type %lld, defaulting to ALAC",
           (long long)type_id);
  configure_codec(fmt, "ALAC", sample_rate, samples_per_frame);
  return false;
}


void rtsp_get_device_id(char *device_id, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(device_id, len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

static int rtsp_create_udp_socket(uint16_t *port) {
  uint16_t bound_port = 0;
  int sock = socket_utils_bind_udp(0, 0, 0, &bound_port);
  if (sock < 0) {
    return -1;
  }
  if (port) {
    *port = bound_port;
  }
  return sock;
}

static void ensure_stream_ports(rtsp_conn_t *conn, bool buffered) {
  int temp_socket = 0;
  if (!buffered && conn->data_port == 0) {
    temp_socket = rtsp_create_udp_socket(&conn->data_port);
    if (temp_socket > 0) {
      close(temp_socket);
    }
  }
  if (conn->control_port == 0) {
    temp_socket = rtsp_create_udp_socket(&conn->control_port);
    if (temp_socket > 0) {
      close(temp_socket);
    }
  }
  if (conn->timing_port == 0) {
    // Allocate a timing port for RTSP response (required by protocol)
    // Note: For AirPlay 1, we send timing requests TO the client, not receive
    // them
    temp_socket = rtsp_create_udp_socket(&conn->timing_port);
    if (temp_socket > 0) {
      close(temp_socket);
    }
  }
}

// Forward declarations of handlers
static void handle_options(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req, const uint8_t *raw,
                           size_t raw_len);
static void handle_get(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len);
static void handle_post(int socket, rtsp_conn_t *conn,
                        const rtsp_request_t *req, const uint8_t *raw,
                        size_t raw_len);
static void handle_announce(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len);
static void handle_setup(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_record(int socket, rtsp_conn_t *conn,
                          const rtsp_request_t *req, const uint8_t *raw,
                          size_t raw_len);
static void handle_set_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len);
static void handle_get_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len);
static inline void flush_output_if_active(void) {
  if (audio_output_is_active()) {
    audio_output_flush();
  } else {
    ESP_LOGD(TAG, "Skip audio_output_flush: output inactive");
  }
}

static void handle_pause(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_flush(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len);
static void handle_teardown(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len);

// Dispatch table
static const rtsp_method_handler_t method_handlers[] = {
    {"OPTIONS", handle_options},
    {"GET", handle_get},
    {"POST", handle_post},
    {"ANNOUNCE", handle_announce},
    {"SETUP", handle_setup},
    {"RECORD", handle_record},
    {"SET_PARAMETER", handle_set_parameter},
    {"GET_PARAMETER", handle_get_parameter},
    {"PAUSE", handle_pause},
    {"FLUSH", handle_flush},
    {"TEARDOWN", handle_teardown},
    {NULL, NULL}};

// Parse a named header value from raw RTSP request data (case-insensitive).
// Returns pointer to a static buffer with the trimmed value, or NULL.
static const char *parse_raw_header(const uint8_t *raw, size_t raw_len,
                                    const char *name) {
  static char value_buf[64];
  const char *hdr = strcasestr((const char *)raw, name);
  if (!hdr || (size_t)(hdr - (const char *)raw) >= raw_len) {
    return NULL;
  }
  hdr += strlen(name);
  // Skip optional whitespace
  while (*hdr == ' ' || *hdr == '\t') {
    hdr++;
  }
  // Copy until CR/LF
  size_t i = 0;
  while (i < sizeof(value_buf) - 1 && hdr[i] && hdr[i] != '\r' &&
         hdr[i] != '\n') {
    value_buf[i] = hdr[i];
    i++;
  }
  value_buf[i] = '\0';
  return value_buf;
}

static bool parse_int_token(const char *text, int *value) {
  if (!text || !value) {
    return false;
  }

  errno = 0;
  char *end = NULL;
  long parsed = strtol(text, &end, 10);
  if (end == text || errno != 0 || parsed < INT_MIN || parsed > INT_MAX) {
    return false;
  }

  *value = (int)parsed;
  return true;
}

static bool parse_uint32_token(const char *text, uint32_t *value) {
  if (!text || !value) {
    return false;
  }

  errno = 0;
  char *end = NULL;
  unsigned long parsed = strtoul(text, &end, 10);
  if (end == text || errno != 0 || parsed > UINT32_MAX) {
    return false;
  }

  *value = (uint32_t)parsed;
  return true;
}

static bool parse_double_token(const char *text, double *value) {
  if (!text || !value) {
    return false;
  }

  errno = 0;
  char *end = NULL;
  double parsed = strtod(text, &end);
  if (end == text || errno != 0) {
    return false;
  }

  *value = parsed;
  return true;
}

static bool parse_uint64_triplet(const char *text, uint64_t *first,
                                 uint64_t *second, uint64_t *third) {
  if (!text || !first || !second || !third) {
    return false;
  }

  errno = 0;
  char *end = NULL;
  unsigned long long a = strtoull(text, &end, 10);
  if (end == text || errno != 0 || *end != '/') {
    return false;
  }

  char *cursor = end + 1;
  unsigned long long b = strtoull(cursor, &end, 10);
  if (end == cursor || errno != 0 || *end != '/') {
    return false;
  }

  cursor = end + 1;
  unsigned long long c = strtoull(cursor, &end, 10);
  if (end == cursor || errno != 0) {
    return false;
  }

  *first = (uint64_t)a;
  *second = (uint64_t)b;
  *third = (uint64_t)c;
  return true;
}

static size_t split_whitespace_tokens(char *text, char *tokens[],
                                      size_t max_tokens) {
  size_t count = 0;
  char *cursor = text;

  while (cursor && *cursor != '\0' && count < max_tokens) {
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor == '\0') {
      break;
    }

    tokens[count++] = cursor;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor == '\0') {
      break;
    }
    *cursor++ = '\0';
  }

  return count;
}

static bool parse_rate_channels(const char *text, int *sample_rate,
                                int *channels) {
  if (!text || !sample_rate || !channels) {
    return false;
  }

  int parsed_rate = 0;
  if (!parse_int_token(text, &parsed_rate)) {
    return false;
  }

  const char *slash = strchr(text, '/');
  int parsed_channels = 0;
  if (slash != NULL && !parse_int_token(slash + 1, &parsed_channels)) {
    return false;
  }

  *sample_rate = parsed_rate;
  *channels = parsed_channels;
  return true;
}

static bool parse_fmtp_config(const char *fmtp, audio_format_t *format) {
  if (!fmtp || !format) {
    return false;
  }

  size_t line_len = strcspn(fmtp, "\r\n");
  if (line_len == 0) {
    return false;
  }

  char line[256];
  if (line_len >= sizeof(line)) {
    line_len = sizeof(line) - 1;
  }
  memcpy(line, fmtp, line_len);
  line[line_len] = '\0';

  char *payload = strchr(line, ' ');
  if (!payload || *(payload + 1) == '\0') {
    return false;
  }

  char *tokens[11];
  size_t token_count = split_whitespace_tokens(payload + 1, tokens, 11);
  if (token_count < 7) {
    return false;
  }

  uint32_t values[11] = {0};
  for (size_t i = 0; i < token_count; i++) {
    if (!parse_uint32_token(tokens[i], &values[i])) {
      return false;
    }
  }

  format->max_samples_per_frame = values[0];
  format->sample_size = values[2];
  format->rice_history_mult = values[3];
  format->rice_initial_history = values[4];
  format->rice_limit = values[5];
  format->num_channels = values[6];
  format->channels = (int)values[6];
  format->bits_per_sample = (int)values[2];
  if (token_count >= 8) {
    format->max_run = values[7];
  }
  if (token_count >= 9) {
    format->max_coded_frame_size = values[8];
  }
  if (token_count >= 10) {
    format->avg_bit_rate = values[9];
  }
  if (token_count >= 11) {
    format->sample_rate_config = values[10];
    format->sample_rate = (int)values[10];
  }

  ESP_LOGI(TAG,
           "Parsed ALAC fmtp: frame_len=%" PRIu32 " bit_depth=%" PRIu32
           " pb=%" PRIu32 " mb=%" PRIu32 " kb=%" PRIu32 " channels=%" PRIu32
           " max_run=%" PRIu32 " max_frame=%" PRIu32 " avg_rate=%" PRIu32
           " rate=%" PRIu32,
           values[0], values[2], values[3], values[4], values[5], values[6],
           token_count >= 8 ? values[7] : 0, token_count >= 9 ? values[8] : 0,
           token_count >= 10 ? values[9] : 0, token_count >= 11 ? values[10] : 0);

  return true;
}

int rtsp_dispatch(int socket, rtsp_conn_t *conn, const uint8_t *raw_request,
                  size_t raw_len) {
  rtsp_request_t req;
  if (rtsp_request_parse(raw_request, raw_len, &req) < 0) {
    ESP_LOGW(TAG, "Failed to parse RTSP request");
    return -1;
  }

  ESP_LOGI(TAG,
           "RTSP request: method=%s path=%s cseq=%d body_len=%zu",
           req.method, req.path, req.cseq, req.body_len);

  // Find handler in dispatch table
  for (const rtsp_method_handler_t *h = method_handlers; h->method; h++) {
    if (strcasecmp(req.method, h->method) == 0) {
      h->handler(socket, conn, &req, raw_request, raw_len);
      return 0;
    }
  }

  ESP_LOGW(TAG, "Unknown method: %s", req.method);
  rtsp_send_http_response(socket, conn, 501, "Not Implemented", "text/plain",
                          "Not Implemented", 15);
  return 0;
}

// ============================================================================
// Handler implementations
// ============================================================================

static void handle_options(int socket, rtsp_conn_t *conn,
                           const rtsp_request_t *req, const uint8_t *raw,
                           size_t raw_len) {
  const char *public_methods =
      "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, "
      "OPTIONS, POST, GET, SET_PARAMETER, GET_PARAMETER, SETPEERS, "
      "SETRATEANCHORTIME\r\n";

  // Handle Apple-Challenge if present (AirPlay 1 authentication)
  const char *challenge = parse_raw_header(raw, raw_len, "Apple-Challenge:");
  if (challenge) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      uint8_t mac[6];
      esp_read_mac(mac, ESP_MAC_WIFI_STA);

      char response_b64[512];
      if (rsa_apple_challenge_response(challenge, ip_info.ip.addr, mac,
                                       response_b64,
                                       sizeof(response_b64)) == 0) {
        char headers[768];
        snprintf(headers, sizeof(headers), "%sApple-Response: %s\r\n",
                 public_methods, response_b64);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq, headers, NULL,
                           0);
        return;
      }
    }
    ESP_LOGW(TAG, "Failed to build Apple-Challenge response");
  }

  rtsp_send_response(socket, conn, 200, "OK", req->cseq, public_methods, NULL,
                     0);
}

static void handle_get(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (strcmp(req->path, "/info") == 0) {
    // Build info response
    char device_id[18];
    char device_name[65];
    char body[4096];
    plist_t p;

    rtsp_get_device_id(device_id, sizeof(device_id));
    settings_get_device_name(device_name, sizeof(device_name));
    uint64_t features =
        ((uint64_t)AIRPLAY_FEATURES_HI << 32) | AIRPLAY_FEATURES_LO;

    plist_init(&p, body, sizeof(body));
    plist_begin(&p);
    plist_dict_begin(&p);

    plist_dict_string(&p, "deviceid", device_id);
    plist_dict_uint(&p, "features", features);
    plist_dict_string(&p, "model", "AudioAccessory5,1");
    plist_dict_string(&p, "protovers", "1.1");
    plist_dict_string(&p, "srcvers", "377.40.00");
    plist_dict_int(&p, "vv", 1);
    plist_dict_int(&p, "statusFlags", 4);
    plist_dict_string(&p, "name", device_name);

    // Audio formats array
    plist_dict_array_begin(&p, "audioFormats");
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 96);
    plist_dict_int(&p, "audioInputFormats", 0x01000000);
    plist_dict_int(&p, "audioOutputFormats", 0x01000000);
    plist_dict_end(&p);
    plist_array_end(&p);

    // Audio latencies array
    // Type 96 (realtime/UDP): PTP sync + internal hardware compensation
    // means audio exits the speaker at the correct wall-clock time;
    // reporting additional latency would cause the sender to over-delay video.
    // Type 103 (buffered/TCP): no timestamp-based scheduling, so the full
    // jitter-buffer depth + hardware pipeline delay applies.
    plist_dict_array_begin(&p, "audioLatencies");
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 96);
    plist_dict_int(&p, "audioType", 0x64);
    plist_dict_int(&p, "inputLatencyMicros", 0);
    plist_dict_int(&p, "outputLatencyMicros", 0);
    plist_dict_end(&p);
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 103);
    plist_dict_int(&p, "audioType", 0x64);
    plist_dict_int(&p, "inputLatencyMicros", 0);
    plist_dict_int(&p, "outputLatencyMicros",
                   audio_receiver_get_output_latency_us() +
                       audio_receiver_get_hardware_latency_us());
    plist_dict_end(&p);
    plist_array_end(&p);

    plist_dict_end(&p);
    size_t body_len = plist_end(&p);

    ESP_LOGI(TAG, "Responding to /info with %zu-byte plist", body_len);

    rtsp_send_http_response(socket, conn, 200, "OK", "text/x-apple-plist+xml",
                            body, body_len);
  } else {
    ESP_LOGW(TAG, "Unknown GET path: %s", req->path);
    rtsp_send_http_response(socket, conn, 404, "Not Found", "text/plain",
                            "Not Found", 9);
  }
}

static void handle_post(int socket, rtsp_conn_t *conn,
                        const rtsp_request_t *req, const uint8_t *raw,
                        size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  if (strstr(req->path, "/command")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t cmd_type = 0;
      if (bplist_find_int(body, body_len, "type", &cmd_type)) {
        ESP_LOGI(TAG, "/command type=%lld", (long long)cmd_type);
      }
    }
    rtsp_send_ok(socket, conn, req->cseq);

  } else if (strstr(req->path, "/feedback")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        ESP_LOGI(TAG, "/feedback has networkTimeSecs=%lld", (long long)value);
      }
    }

    // For buffered audio streams (type 103), send a proper feedback response
    // with stream status. This acts as a keepalive to prevent iPhone from
    // sending TEARDOWN during extended pause.
    if (conn->stream_type == 103) {
      uint8_t response[128];
      size_t response_len = bplist_build_feedback_response(
          response, sizeof(response), conn->stream_type, 44100.0);

      if (response_len > 0) {
        ESP_LOGD(
            TAG,
            "/feedback responding with stream status (type=%lld, sr=44100)",
            (long long)conn->stream_type);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                           "Content-Type: application/x-apple-binary-plist\r\n",
                           (const char *)response, response_len);
      } else {
        // Fallback to simple OK if response build fails
        rtsp_send_ok(socket, conn, req->cseq);
      }
    } else {
      // For non-buffered streams, simple OK is fine
      rtsp_send_ok(socket, conn, req->cseq);
    }

  } else {
    rtsp_send_ok(socket, conn, req->cseq);
  }
}

typedef enum {
  SDP_PARSE_OK = 0,
  SDP_PARSE_INVALID_ENCRYPTION,
  SDP_PARSE_INVALID_BODY,
} sdp_parse_result_t;

static sdp_parse_result_t parse_sdp(rtsp_conn_t *conn, const char *sdp,
                                    size_t len) {
  if (!conn || !sdp || len == 0) {
    return SDP_PARSE_INVALID_BODY;
  }

  char *sdp_buf = calloc(1, len + 1);
  if (!sdp_buf) {
    return SDP_PARSE_INVALID_BODY;
  }
  memcpy(sdp_buf, sdp, len);
  sdp_buf[len] = '\0';

  const char *text = sdp_buf;

  audio_format_t format = {0};
  audio_encrypt_t encrypt = {0};
  encrypt.type = AUDIO_ENCRYPT_NONE;

  format.sample_rate = 44100;
  format.channels = 2;
  format.bits_per_sample = 16;
  format.frame_size = 352;
  strncpy(format.codec, "AppleLossless", sizeof(format.codec) - 1);
  format.codec[sizeof(format.codec) - 1] = '\0';

  const char *rtpmap = strstr(text, "a=rtpmap:");
  if (rtpmap) {
    sscanf(rtpmap, "a=rtpmap:%*d %31s", format.codec);
    char *slash = strchr(format.codec, '/');
    if (slash) {
      *slash = '\0';
      int sr = 0;
      int ch = 0;
      if (parse_rate_channels(slash + 1, &sr, &ch)) {
        if (sr > 0) {
          format.sample_rate = sr;
        }
        if (ch > 0) {
          format.channels = ch;
        }
      }
    }
  }

  const char *fmtp = strstr(text, "a=fmtp:");
  if (fmtp) {
    parse_fmtp_config(fmtp, &format);
  }

  if ((strstr(format.codec, "AAC") || strstr(format.codec, "aac") ||
       strstr(format.codec, "mpeg4-generic") ||
       strstr(format.codec, "MPEG4-GENERIC")) &&
      format.max_samples_per_frame == 0) {
    format.frame_size = 1024;
    format.max_samples_per_frame = 1024;
  }

  sdp_parse_result_t result = SDP_PARSE_OK;

  // Parse RSA-encrypted AES key and IV from SDP (AirPlay 1 audio encryption)
  const char *rsaaeskey = strcasestr(text, "rsaaeskey:");
  const char *aesiv_str = strcasestr(text, "aesiv:");
  bool has_key = (rsaaeskey != NULL);
  bool has_iv = (aesiv_str != NULL);
  if (has_key != has_iv) {
    ESP_LOGW(TAG, "AirPlay v1 ANNOUNCE invalid: rsaaeskey/aesiv mismatch");
    result = SDP_PARSE_INVALID_ENCRYPTION;
  } else if (rsaaeskey && aesiv_str) {
    // Extract base64 key (may span multiple lines, concatenate until next
    // field or end of SDP). In practice it's a single long base64 line.
    rsaaeskey += strlen("rsaaeskey:");
    while (*rsaaeskey == ' ' || *rsaaeskey == '\t') {
      rsaaeskey++;
    }

    // Collect key characters (skip whitespace/newlines within base64)
    char key_b64[512];
    size_t ki = 0;
    for (const char *p = rsaaeskey; *p && ki < sizeof(key_b64) - 1; p++) {
      if (*p == '\r' || *p == '\n') {
        // Check if next non-space char is start of a new SDP field (e.g. "a=")
        const char *q = p + 1;
        while (*q == '\r' || *q == '\n' || *q == ' ') {
          q++;
        }
        if (*q == 'a' && *(q + 1) == '=') {
          break;
        }
      } else if (*p != ' ' && *p != '\t') {
        key_b64[ki++] = *p;
      }
    }
    key_b64[ki] = '\0';

    // Extract IV
    aesiv_str += strlen("aesiv:");
    while (*aesiv_str == ' ' || *aesiv_str == '\t') {
      aesiv_str++;
    }
    char iv_b64[64];
    size_t ii = 0;
    for (const char *p = aesiv_str;
         *p && *p != '\r' && *p != '\n' && ii < sizeof(iv_b64) - 1; p++) {
      iv_b64[ii++] = *p;
    }
    iv_b64[ii] = '\0';

    // Decrypt AES key using RSA
    uint8_t aes_key[32];
    size_t aes_key_len = 0;
    int rsa_ret =
        rsa_decrypt_aes_key(key_b64, aes_key, sizeof(aes_key), &aes_key_len);
    if (rsa_ret == 0 && aes_key_len == 16) {
      encrypt.type = AUDIO_ENCRYPT_AES_CBC;
      memcpy(encrypt.key, aes_key, aes_key_len);
      encrypt.key_len = aes_key_len;

      // Decode IV
      size_t iv_len = 0;
      int iv_ret =
          rtsp_decode_airplay_base64(iv_b64, encrypt.iv, sizeof(encrypt.iv),
                                     &iv_len);
      if (iv_len != 16) {
        ESP_LOGW(TAG,
                 "AirPlay v1 ANNOUNCE invalid: aesiv decode length=%d "
                 "(expected 16) iv_b64_len=%zu iv_ret=%d",
                 (int)iv_len, strlen(iv_b64), iv_ret);
        memset(&encrypt, 0, sizeof(encrypt));
        encrypt.type = AUDIO_ENCRYPT_NONE;
        result = SDP_PARSE_INVALID_ENCRYPTION;
      } else {
        ESP_LOGI(TAG,
                 "AirPlay v1: AES-CBC encryption configured (key=%zu iv=%d)",
                 aes_key_len, iv_len);
      }
    } else {
      ESP_LOGW(TAG,
               "AirPlay v1 ANNOUNCE invalid: RSA key decrypt failed "
               "(ret=%d key_len=%zu)",
               rsa_ret, aes_key_len);
      result = SDP_PARSE_INVALID_ENCRYPTION;
    }
  }

  // Update connection state
  strncpy(conn->codec, format.codec, sizeof(conn->codec) - 1);
  conn->sample_rate = format.sample_rate;
  conn->channels = format.channels;
  conn->bits_per_sample = format.bits_per_sample;

  audio_receiver_set_format(&format);

  if (encrypt.type != AUDIO_ENCRYPT_NONE) {
    audio_receiver_set_encryption(&encrypt);
  }

  free(sdp_buf);
  return result;
}

static void handle_announce(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len) {
  (void)raw;
  (void)raw_len;

  conn->announce_ready = false;
  audio_receiver_clear_encryption();

  if (req->body && req->body_len > 0) {
    sdp_parse_result_t parse_result =
        parse_sdp(conn, (const char *)req->body, req->body_len);
    if (parse_result != SDP_PARSE_OK) {
      ESP_LOGW(TAG,
               "ANNOUNCE rejected: invalid SDP/encryption parameters "
               "(result=%d cseq=%d)",
               (int)parse_result, req->cseq);
      rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                         NULL, 0);
      return;
    }
    conn->announce_ready = true;
  } else {
    ESP_LOGW(TAG, "ANNOUNCE rejected: missing SDP body (cseq=%d)", req->cseq);
    rtsp_send_response(socket, conn, 400, "Bad Request", req->cseq, NULL,
                       NULL, 0);
    return;
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_setup(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw_len;

  if (!conn->announce_ready) {
    ESP_LOGW(TAG, "SETUP rejected before valid ANNOUNCE (cseq=%d)", req->cseq);
    rtsp_send_response(socket, conn, 455, "Method Not Valid in This State",
                       req->cseq, NULL, NULL, 0);
    return;
  }

  // AirPlay 1 SETUP: transport info is in the Transport header, no bplist body.
  ESP_LOGI(TAG, "SETUP: AirPlay v1 stream setup");

  int64_t stream_type = 96; // RTP/UDP realtime
  conn->stream_type = stream_type;

  // Parse client's control and timing ports from Transport header
  rtsp_parse_transport((const char *)raw, &conn->client_control_port,
                       &conn->client_timing_port);
  ESP_LOGI(TAG, "Client ports: control=%u timing=%u",
           conn->client_control_port, conn->client_timing_port);

  // Start NTP timing client
  if (conn->client_timing_port > 0 && conn->client_ip != 0) {
    ntp_clock_start_client(conn->client_ip, conn->client_timing_port);
  }

  ensure_stream_ports(conn, false);
  ESP_LOGI(TAG,
           "SETUP ports: client_control=%u client_timing=%u server_data=%u "
           "server_control=%u server_timing=%u",
           conn->client_control_port, conn->client_timing_port,
           conn->data_port, conn->control_port, conn->timing_port);

  char transport_response[256];
  snprintf(transport_response, sizeof(transport_response),
           "Transport: RTP/AVP/UDP;unicast;mode=record;"
           "server_port=%d;control_port=%d;timing_port=%d\r\n"
           "Session: 1\r\n",
           conn->data_port, conn->control_port, conn->timing_port);
  rtsp_send_response(socket, conn, 200, "OK", req->cseq, transport_response,
                     NULL, 0);

  // Keep the exact format parsed from ANNOUNCE/SDP, especially ALAC fmtp
  // fields. Overwriting it here with a generic 44100/352 profile causes the
  // ALAC decoder cookie to lose stream-specific parameters and can decode to
  // silent PCM even though RTP/RTSP are otherwise healthy.
  ESP_LOGI(TAG,
           "SETUP: using ANNOUNCE audio format codec=%s sr=%d ch=%d bits=%d",
           conn->codec[0] ? conn->codec : "unknown", conn->sample_rate,
           conn->channels, conn->bits_per_sample);
  audio_receiver_set_stream_type((audio_stream_type_t)stream_type);

  // Enable NACK retransmission if we know the client's control port
  if (conn->client_control_port > 0 && conn->client_ip != 0) {
    audio_receiver_set_client_control(conn->client_ip,
                                      conn->client_control_port);
  }

  conn->stream_active = true;
}

static void handle_record(int socket, rtsp_conn_t *conn,
                          const rtsp_request_t *req, const uint8_t *raw,
                          size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (!conn->announce_ready) {
    ESP_LOGW(TAG, "RECORD rejected before valid ANNOUNCE (cseq=%d)", req->cseq);
    rtsp_send_response(socket, conn, 455, "Method Not Valid in This State",
                       req->cseq, NULL, NULL, 0);
    return;
  }

  ESP_LOGI(TAG, "RECORD received - starting playback, stream_paused was %d",
           conn->stream_paused);

  if (conn->stream_paused) {
    // Resuming from PAUSE: the stream listener is still running and the
    // timing anchor has been preserved.  Just re-enable playout; the
    // pause-duration offset in audio_timing will re-align the timestamps.
    ESP_LOGI(TAG, "RECORD: resuming from pause, skipping stream restart");
    audio_receiver_set_playing(true);
  } else {
    // Fresh start or post-teardown reconnect: full stream restart.
    audio_receiver_start_stream(conn->data_port, conn->control_port,
                                conn->buffered_port);
    if (conn->client_control_port > 0 && conn->client_ip != 0) {
      audio_receiver_set_client_control(conn->client_ip,
                                        conn->client_control_port);
    }
    audio_receiver_set_playing(true);
  }
  conn->stream_paused = false;
  rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);

  // Diagnostics: log timing anchor state
  ESP_LOGI(TAG,
           "RECORD: audio_output_active=%d, buffer_frames=%u, anchor_valid=%d, "
           "ntp_locked=%d ntp_tracking=%d reject_streak=%d age_ms=%lld",
           audio_output_is_active(),
           audio_receiver_get_buffered_frames(),
           audio_receiver_anchor_valid(),
           ntp_clock_is_locked(),
           ntp_clock_is_tracking(),
           ntp_clock_get_reject_streak(),
           (long long)ntp_clock_get_last_accept_age_ms());

  // Force playback start if anchor/NTP not yet available
  audio_timing_force_start();

  // AirPlay 1 RECORD is always type 96 (realtime/UDP) with NTP sync.
  // Internal timing already compensates for hardware latency, so report 0.
  char headers[128];
  uint32_t latency_samples = 0;
  snprintf(headers, sizeof(headers),
           "Audio-Latency: %" PRIu32 "\r\n"
           "Audio-Jack-Status: connected\r\n",
           latency_samples);

  rtsp_send_response(socket, conn, 200, "OK", req->cseq, headers, NULL, 0);
}

// ============================================================================
// Metadata and Progress Logging Helpers
// ============================================================================

/**
 * Format time in seconds as mm:ss string
 * @param seconds Time in seconds
 * @param out Output buffer (at least 8 bytes for "999:59\0")
 * @param out_size Size of output buffer
 */
static void format_time_mmss(uint32_t seconds, char *out, size_t out_size) {
  rtsp_format_time_mmss(seconds, out, out_size);
}

/**
 * Parse DMAP-tagged data and extract metadata into struct
 * DMAP format: 4-byte tag, 4-byte BE length, data
 * Common tags:
 *   minm = item name (track title)
 *   asar = artist
 *   asal = album
 *   asgn = genre
 *   asai = album id (64-bit)
 */
static void parse_dmap_metadata(const uint8_t *data, size_t len,
                                rtsp_metadata_t *meta) {
  size_t pos = 0;

  while (pos + 8 <= len) {
    // Read 4-byte tag
    char tag[5] = {0};
    memcpy(tag, data + pos, 4);
    pos += 4;

    // Read 4-byte big-endian length
    uint32_t item_len = ((uint32_t)data[pos] << 24) |
                        ((uint32_t)data[pos + 1] << 16) |
                        ((uint32_t)data[pos + 2] << 8) | data[pos + 3];
    pos += 4;

    if (pos + item_len > len) {
      break; // Malformed
    }

    // Extract known metadata tags
    if (strcmp(tag, "minm") == 0 && item_len > 0) {
      size_t copy_len = item_len < METADATA_STRING_MAX - 1
                            ? item_len
                            : METADATA_STRING_MAX - 1;
      memcpy(meta->title, data + pos, copy_len);
      meta->title[copy_len] = '\0';
      ESP_LOGI(TAG, "  Title  = %s", meta->title);
    } else if (strcmp(tag, "asar") == 0 && item_len > 0) {
      size_t copy_len = item_len < METADATA_STRING_MAX - 1
                            ? item_len
                            : METADATA_STRING_MAX - 1;
      memcpy(meta->artist, data + pos, copy_len);
      meta->artist[copy_len] = '\0';
      ESP_LOGI(TAG, "  Artist = %s", meta->artist);
    } else if (strcmp(tag, "asal") == 0 && item_len > 0) {
      size_t copy_len = item_len < METADATA_STRING_MAX - 1
                            ? item_len
                            : METADATA_STRING_MAX - 1;
      memcpy(meta->album, data + pos, copy_len);
      meta->album[copy_len] = '\0';
      ESP_LOGI(TAG, "  Album  = %s", meta->album);
    } else if (strcmp(tag, "asgn") == 0 && item_len > 0) {
      size_t copy_len = item_len < METADATA_STRING_MAX - 1
                            ? item_len
                            : METADATA_STRING_MAX - 1;
      memcpy(meta->genre, data + pos, copy_len);
      meta->genre[copy_len] = '\0';
      ESP_LOGI(TAG, "  Genre  = %s", meta->genre);
    } else if (strcmp(tag, "mlit") == 0 || strcmp(tag, "cmst") == 0 ||
               strcmp(tag, "mdst") == 0) {
      // Container tags - recurse into them
      parse_dmap_metadata(data + pos, item_len, meta);
    }

    pos += item_len;
  }
}

/**
 * Parse progress string and populate metadata fields
 * Progress format: "start/current/end" in RTP timestamp units
 * Sample rate is typically 44100
 */
static void parse_progress(const char *progress_str, uint32_t sample_rate,
                           rtsp_metadata_t *meta) {
  uint64_t start = 0, current = 0, end = 0;

  if (parse_uint64_triplet(progress_str, &start, &current, &end)) {
    if (sample_rate == 0) {
      sample_rate = 44100; // Default sample rate
    }

    meta->position_secs = (uint32_t)((current - start) / sample_rate);
    meta->duration_secs = (uint32_t)((end - start) / sample_rate);

    char pos_str[16], dur_str[16];
    format_time_mmss(meta->position_secs, pos_str, sizeof(pos_str));
    format_time_mmss(meta->duration_secs, dur_str, sizeof(dur_str));

    ESP_LOGI(TAG,
             "Progress: %s / %s (raw: %" PRIu64 "/%" PRIu64 "/%" PRIu64 ")",
             pos_str, dur_str, start, current, end);
  }
}

static void handle_set_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  rtsp_event_data_t event_data;
  memset(&event_data, 0, sizeof(event_data));
  bool has_metadata = false;

  // Check for progress header in raw request
  const char *progress_hdr = strstr((const char *)raw, "progress:");
  if (!progress_hdr) {
    progress_hdr = strstr((const char *)raw, "Progress:");
  }
  if (progress_hdr) {
    // Find end of header line
    const char *line_end = strstr(progress_hdr, "\r\n");
    if (line_end) {
      size_t val_start = 9; // Length of "progress:"
      while (progress_hdr[val_start] == ' ') {
        val_start++;
      }
      char progress_val[64];
      size_t val_len = line_end - (progress_hdr + val_start);
      if (val_len < sizeof(progress_val)) {
        memcpy(progress_val, progress_hdr + val_start, val_len);
        progress_val[val_len] = '\0';
        parse_progress(progress_val, 44100, &event_data.metadata);
        has_metadata = true;
      }
    }
  }

  if (strstr(req->content_type, "text/parameters")) {
    if (body) {
      if (strstr((const char *)body, "volume:")) {
        const char *vol = strstr((const char *)body, "volume:");
        if (vol) {
          double volume = 0.0;
          if (parse_double_token(vol + 7, &volume)) {
            rtsp_conn_set_volume(conn, (float)volume);
          }
        }
      }
      // Check for progress in body (AirPlay 1 style)
      const char *prog = strstr((const char *)body, "progress:");
      if (prog) {
        prog += 9;
        while (*prog == ' ') {
          prog++;
        }
        parse_progress(prog, 44100, &event_data.metadata);
        has_metadata = true;
      }
    }
  } else if (strstr(req->content_type, "application/x-dmap-tagged")) {
    // DMAP-tagged metadata (AirPlay 1)
    if (body && body_len > 0) {
      ESP_LOGI(TAG, "Received DMAP metadata (%zu bytes)", body_len);
      parse_dmap_metadata(body, body_len, &event_data.metadata);
      has_metadata = true;
    }
  } else if (strstr(req->content_type, "image/jpeg") ||
             strstr(req->content_type, "image/png")) {
    // Artwork - allocate PSRAM and save data
    if (body && body_len > 0) {
      if (body_len > RTSP_MAX_ARTWORK_BYTES) {
        ESP_LOGW(TAG,
                 "Artwork dropped: %s (%zu bytes) exceeds limit %u bytes",
                 req->content_type, body_len, (unsigned)RTSP_MAX_ARTWORK_BYTES);
      } else {
        ESP_LOGI(TAG, "Received artwork: %s (%zu bytes)", req->content_type,
                 body_len);
        void *art = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
        if (art) {
          memcpy(art, body, body_len);
          event_data.metadata.artwork_data = art;
          event_data.metadata.artwork_len = body_len;
          event_data.metadata.has_artwork = true;
          has_metadata = true;
        } else {
          ESP_LOGE(TAG, "Failed to allocate PSRAM for artwork");
        }
      }
    }
  } else if (strstr(req->content_type, "application/x-apple-binary-plist")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        ESP_LOGI(TAG, "SET_PARAMETER: networkTimeSecs=%lld", (long long)value);
      }
      double rate;
      if (bplist_find_real(body, body_len, "rate", &rate)) {
        ESP_LOGI(TAG, "SET_PARAMETER: rate=%.2f", rate);
      }
      // Try to extract metadata from bplist (AirPlay 2)
      char str_val[METADATA_STRING_MAX];
      if (bplist_find_string(body, body_len, "itemName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI(TAG, "Metadata: Title = %s", str_val);
        strncpy(event_data.metadata.title, str_val, METADATA_STRING_MAX - 1);
        has_metadata = true;
      }
      if (bplist_find_string(body, body_len, "artistName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI(TAG, "Metadata: Artist = %s", str_val);
        strncpy(event_data.metadata.artist, str_val, METADATA_STRING_MAX - 1);
        has_metadata = true;
      }
      if (bplist_find_string(body, body_len, "albumName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI(TAG, "Metadata: Album = %s", str_val);
        strncpy(event_data.metadata.album, str_val, METADATA_STRING_MAX - 1);
        has_metadata = true;
      }
      // Progress info from bplist
      double elapsed = 0, duration = 0;
      if (bplist_find_real(body, body_len, "elapsed", &elapsed)) {
        event_data.metadata.position_secs = (uint32_t)elapsed;
        has_metadata = true;
        char elapsed_str[16];
        format_time_mmss((uint32_t)elapsed, elapsed_str, sizeof(elapsed_str));
        if (bplist_find_real(body, body_len, "duration", &duration)) {
          event_data.metadata.duration_secs = (uint32_t)duration;
          char duration_str[16];
          format_time_mmss((uint32_t)duration, duration_str,
                           sizeof(duration_str));
          ESP_LOGI(TAG, "Progress: %s / %s", elapsed_str, duration_str);
        } else {
          ESP_LOGI(TAG, "Progress: %s", elapsed_str);
        }
      }
    }
  }

  if (has_metadata) {
    rtsp_events_emit(RTSP_EVENT_METADATA, &event_data);
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_get_parameter(int socket, rtsp_conn_t *conn,
                                 const rtsp_request_t *req, const uint8_t *raw,
                                 size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (req->body && req->body_len > 0) {
    if (strstr((const char *)req->body, "volume")) {
      char vol_response[32];
      int vol_len = snprintf(vol_response, sizeof(vol_response),
                             "volume: %.2f\r\n", conn->volume_db);
      rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                         "Content-Type: text/parameters\r\n", vol_response,
                         vol_len);
      return;
    }
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_pause(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw;
  (void)raw_len;

  ESP_LOGI(TAG, "PAUSE received");

  // Stop the audio consumer but leave the buffer filling.  The phone will
  // send a fresh SETRATEANCHORTIME (rate=1) anchor on resume that re-aligns
  // the buffered frames to the correct wall-clock position.
  audio_receiver_pause();
  flush_output_if_active();
  conn->stream_paused = true;

  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_flush(int socket, rtsp_conn_t *conn,
                         const rtsp_request_t *req, const uint8_t *raw,
                         size_t raw_len) {
  (void)raw;
  (void)raw_len;

  // Plain AirPlay 1 FLUSH — always immediate.
  ESP_LOGI(TAG, "FLUSH received");
  audio_receiver_seek_flush();
  flush_output_if_active();
  rtsp_send_ok(socket, conn, req->cseq);
}

static void handle_teardown(int socket, rtsp_conn_t *conn,
                            const rtsp_request_t *req, const uint8_t *raw,
                            size_t raw_len) {
  (void)raw;
  (void)raw_len;

  // AirPlay 1 TEARDOWN — always a full session teardown.
  // Keep DACP session alive so the grace period in rtsp_server can probe mDNS
  // to differentiate pause from real disconnect.
  ESP_LOGI(TAG, "TEARDOWN received");
  audio_receiver_stop();
  flush_output_if_active();
  conn->stream_active = false;
  conn->stream_paused = false;
  ntp_clock_stop();
  conn->timing_port = 0;

  rtsp_send_ok(socket, conn, req->cseq);
}
