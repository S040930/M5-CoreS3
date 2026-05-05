#include "rtsp_handler_common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "audio_stream.h"
#include "ntp_clock.h"
#include "plist.h"
#include "rtsp_rsa.h"
#include "socket_utils.h"

#include "rtsp_events.h"

static const char *TAG = "rtsp_handlers";

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

void rtsp_ensure_stream_ports(rtsp_conn_t *conn, bool buffered) {
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
    temp_socket = rtsp_create_udp_socket(&conn->timing_port);
    if (temp_socket > 0) {
      close(temp_socket);
    }
  }
}

void rtsp_flush_output_if_active(void) {
  if (audio_output_is_active()) {
    audio_output_flush();
  } else {
    ESP_LOGD(TAG, "Skip audio_output_flush: output inactive");
  }
}

const char *rtsp_parse_raw_header(const uint8_t *raw, size_t raw_len,
                                  const char *name) {
  static char value_buf[64];
  const char *hdr = strcasestr((const char *)raw, name);
  if (!hdr || (size_t)(hdr - (const char *)raw) >= raw_len) {
    return NULL;
  }
  hdr += strlen(name);
  while (*hdr == ' ' || *hdr == '\t') {
    hdr++;
  }
  size_t i = 0;
  while (i < sizeof(value_buf) - 1 && hdr[i] && hdr[i] != '\r' &&
         hdr[i] != '\n') {
    value_buf[i] = hdr[i];
    i++;
  }
  value_buf[i] = '\0';
  return value_buf;
}

bool rtsp_parse_int_token(const char *text, int *value) {
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

bool rtsp_parse_uint32_token(const char *text, uint32_t *value) {
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

bool rtsp_parse_double_token(const char *text, double *value) {
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

bool rtsp_parse_uint64_triplet(const char *text, uint64_t *first,
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

size_t rtsp_split_whitespace_tokens(char *text, char *tokens[],
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

bool rtsp_parse_rate_channels(const char *text, int *sample_rate,
                              int *channels) {
  if (!text || !sample_rate || !channels) {
    return false;
  }
  int parsed_rate = 0;
  if (!rtsp_parse_int_token(text, &parsed_rate)) {
    return false;
  }
  const char *slash = strchr(text, '/');
  int parsed_channels = 0;
  if (slash != NULL && !rtsp_parse_int_token(slash + 1, &parsed_channels)) {
    return false;
  }
  *sample_rate = parsed_rate;
  *channels = parsed_channels;
  return true;
}

bool rtsp_parse_fmtp_config(const char *fmtp, audio_format_t *format) {
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
  size_t token_count =
      rtsp_split_whitespace_tokens(payload + 1, tokens, 11);
  if (token_count < 7) {
    return false;
  }
  uint32_t values[11] = {0};
  for (size_t i = 0; i < token_count; i++) {
    if (!rtsp_parse_uint32_token(tokens[i], &values[i])) {
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

sdp_parse_result_t rtsp_parse_sdp(rtsp_conn_t *conn, const char *sdp,
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
      if (rtsp_parse_rate_channels(slash + 1, &sr, &ch)) {
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
    rtsp_parse_fmtp_config(fmtp, &format);
  }

  if ((strstr(format.codec, "AAC") || strstr(format.codec, "aac") ||
       strstr(format.codec, "mpeg4-generic") ||
       strstr(format.codec, "MPEG4-GENERIC")) &&
      format.max_samples_per_frame == 0) {
    format.frame_size = 1024;
    format.max_samples_per_frame = 1024;
  }

  sdp_parse_result_t result = SDP_PARSE_OK;

  const char *rsaaeskey = strcasestr(text, "rsaaeskey:");
  const char *aesiv_str = strcasestr(text, "aesiv:");
  bool has_key = (rsaaeskey != NULL);
  bool has_iv = (aesiv_str != NULL);
  if (has_key != has_iv) {
    ESP_LOGW(TAG, "AirPlay v1 ANNOUNCE invalid: rsaaeskey/aesiv mismatch");
    result = SDP_PARSE_INVALID_ENCRYPTION;
  } else if (rsaaeskey && aesiv_str) {
    rsaaeskey += strlen("rsaaeskey:");
    while (*rsaaeskey == ' ' || *rsaaeskey == '\t') {
      rsaaeskey++;
    }
    char key_b64[512];
    size_t ki = 0;
    for (const char *p = rsaaeskey; *p && ki < sizeof(key_b64) - 1; p++) {
      if (*p == '\r' || *p == '\n') {
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
    uint8_t aes_key[32];
    size_t aes_key_len = 0;
    int rsa_ret =
        rsa_decrypt_aes_key(key_b64, aes_key, sizeof(aes_key), &aes_key_len);
    if (rsa_ret == 0 && aes_key_len == 16) {
      encrypt.type = AUDIO_ENCRYPT_AES_CBC;
      memcpy(encrypt.key, aes_key, aes_key_len);
      encrypt.key_len = aes_key_len;
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

  strncpy(conn->codec, format.codec, sizeof(conn->codec) - 1);
  conn->sample_rate = format.sample_rate;
  conn->channels = format.channels;
  conn->bits_per_sample = format.bits_per_sample;
  audio_receiver_set_format(&format);
  if (encrypt.type != AUDIO_ENCRYPT_NONE) {
    audio_receiver_set_encryption(&encrypt);
  }
  
  // 摘要日志：ANNOUNCE 完成后打印最终的音频格式和加密参数
  const char *encrypt_type_str = "NONE";
  if (encrypt.type == AUDIO_ENCRYPT_AES_CBC) {
    encrypt_type_str = "AES-CBC";
  } else if (encrypt.type == AUDIO_ENCRYPT_CHACHA20_POLY1305) {
    encrypt_type_str = "ChaCha20Poly1305";
  }
  ESP_LOGI(TAG,
           "ANNOUNCE complete: codec=%s sr=%d ch=%d bits=%d encrypt=%s key_len=%zu iv_len=%zu "
           "frame_size=%d max_samples=%u",
           format.codec, format.sample_rate, format.channels, format.bits_per_sample,
           encrypt_type_str, encrypt.key_len,
           (encrypt.type != AUDIO_ENCRYPT_NONE) ? 16 : 0,
           format.frame_size, format.max_samples_per_frame);
  
  free(sdp_buf);
  return result;
}

void rtsp_format_time_mmss(uint32_t seconds, char *out, size_t out_size) {
  if (!out || out_size < 6) {
    return;
  }
  uint32_t mins = seconds / 60;
  uint32_t secs = seconds % 60;
  snprintf(out, out_size, "%" PRIu32 ":%02" PRIu32, mins, secs);
}

void rtsp_parse_dmap_metadata(const uint8_t *data, size_t len,
                              rtsp_metadata_t *meta) {
  size_t pos = 0;
  while (pos + 8 <= len) {
    char tag[5] = {0};
    memcpy(tag, data + pos, 4);
    pos += 4;
    uint32_t item_len = ((uint32_t)data[pos] << 24) |
                        ((uint32_t)data[pos + 1] << 16) |
                        ((uint32_t)data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (pos + item_len > len) {
      break;
    }
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
      rtsp_parse_dmap_metadata(data + pos, item_len, meta);
    }
    pos += item_len;
  }
}

void rtsp_parse_progress(const char *progress_str, uint32_t sample_rate,
                         rtsp_metadata_t *meta) {
  uint64_t start = 0, current = 0, end = 0;
  if (rtsp_parse_uint64_triplet(progress_str, &start, &current, &end)) {
    if (sample_rate == 0) {
      sample_rate = 44100;
    }
    meta->position_secs = (uint32_t)((current - start) / sample_rate);
    meta->duration_secs = (uint32_t)((end - start) / sample_rate);
    char pos_str[16], dur_str[16];
    rtsp_format_time_mmss(meta->position_secs, pos_str, sizeof(pos_str));
    rtsp_format_time_mmss(meta->duration_secs, dur_str, sizeof(dur_str));
    ESP_LOGI(TAG,
             "Progress: %s / %s (raw: %" PRIu64 "/%" PRIu64 "/%" PRIu64 ")",
             pos_str, dur_str, start, current, end);
  }
}
