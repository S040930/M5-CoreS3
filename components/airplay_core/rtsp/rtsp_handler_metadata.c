#include "rtsp_handler_common.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "plist.h"
#include "rtsp_events.h"

void rtsp_handle_get(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                     const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  if (strcmp(req->path, "/info") == 0) {
    char device_id[18];
    char body[4096];
    plist_t p;

    rtsp_get_device_id(device_id, sizeof(device_id));
    uint64_t features =
        ((uint64_t)0x0 << 32) | 0x5C4A00;

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
    plist_dict_string(&p, "name", conn->device_name);

    plist_dict_array_begin(&p, "audioFormats");
    plist_dict_begin(&p);
    plist_dict_int(&p, "type", 96);
    plist_dict_int(&p, "audioInputFormats", 0x01000000);
    plist_dict_int(&p, "audioOutputFormats", 0x01000000);
    plist_dict_end(&p);
    plist_array_end(&p);

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

    ESP_LOGI("rtsp_handlers", "Responding to /info with %zu-byte plist",
             body_len);

    rtsp_send_http_response(socket, conn, 200, "OK", "text/x-apple-plist+xml",
                            body, body_len);
  } else {
    ESP_LOGW("rtsp_handlers", "Unknown GET path: %s", req->path);
    rtsp_send_http_response(socket, conn, 404, "Not Found", "text/plain",
                            "Not Found", 9);
  }
}

void rtsp_handle_post(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                      const uint8_t *raw, size_t raw_len) {
  (void)raw;
  (void)raw_len;

  const uint8_t *body = req->body;
  size_t body_len = req->body_len;

  if (strstr(req->path, "/command")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t cmd_type = 0;
      if (bplist_find_int(body, body_len, "type", &cmd_type)) {
        ESP_LOGI("rtsp_handlers", "/command type=%lld", (long long)cmd_type);
      }
    }
    rtsp_send_ok(socket, conn, req->cseq);

  } else if (strstr(req->path, "/feedback")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        ESP_LOGI("rtsp_handlers", "/feedback has networkTimeSecs=%lld",
                 (long long)value);
      }
    }
    if (conn->stream_type == 103) {
      uint8_t response[128];
      size_t response_len = bplist_build_feedback_response(
          response, sizeof(response), conn->stream_type, 44100.0);
      if (response_len > 0) {
        ESP_LOGD("rtsp_handlers",
                 "/feedback responding with stream status (type=%lld, sr=44100)",
                 (long long)conn->stream_type);
        rtsp_send_response(socket, conn, 200, "OK", req->cseq,
                           "Content-Type: application/x-apple-binary-plist\r\n",
                           (const char *)response, response_len);
      } else {
        rtsp_send_ok(socket, conn, req->cseq);
      }
    } else {
      rtsp_send_ok(socket, conn, req->cseq);
    }

  } else {
    rtsp_send_ok(socket, conn, req->cseq);
  }
}

void rtsp_handle_set_parameter(int socket, rtsp_conn_t *conn,
                               const rtsp_request_t *req, const uint8_t *raw,
                               size_t raw_len) {
  const uint8_t *body = req->body;
  size_t body_len = req->body_len;
  rtsp_event_data_t event_data;
  memset(&event_data, 0, sizeof(event_data));
  bool has_metadata = false;

  const char *progress_hdr = strstr((const char *)raw, "progress:");
  if (!progress_hdr) {
    progress_hdr = strstr((const char *)raw, "Progress:");
  }
  if (progress_hdr) {
    const char *line_end = strstr(progress_hdr, "\r\n");
    if (line_end) {
      size_t val_start = 9;
      while (progress_hdr[val_start] == ' ') {
        val_start++;
      }
      char progress_val[64];
      size_t val_len = line_end - (progress_hdr + val_start);
      if (val_len < sizeof(progress_val)) {
        memcpy(progress_val, progress_hdr + val_start, val_len);
        progress_val[val_len] = '\0';
        rtsp_parse_progress(progress_val, 44100, &event_data.metadata);
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
          if (rtsp_parse_double_token(vol + 7, &volume)) {
            rtsp_conn_set_volume(conn, (float)volume);
          }
        }
      }
      const char *prog = strstr((const char *)body, "progress:");
      if (prog) {
        prog += 9;
        while (*prog == ' ') {
          prog++;
        }
        rtsp_parse_progress(prog, 44100, &event_data.metadata);
        has_metadata = true;
      }
    }
  } else if (strstr(req->content_type, "application/x-dmap-tagged")) {
    if (body && body_len > 0) {
      ESP_LOGI("rtsp_handlers", "Received DMAP metadata (%zu bytes)", body_len);
      rtsp_parse_dmap_metadata(body, body_len, &event_data.metadata);
      has_metadata = true;
    }
  } else if (strstr(req->content_type, "image/jpeg") ||
             strstr(req->content_type, "image/png")) {
    if (body && body_len > 0) {
      if (body_len > RTSP_MAX_ARTWORK_BYTES) {
        ESP_LOGW("rtsp_handlers",
                 "Artwork dropped: %s (%zu bytes) exceeds limit %u bytes",
                 req->content_type, body_len, (unsigned)RTSP_MAX_ARTWORK_BYTES);
      } else {
        void *artwork_copy = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM);
        if (artwork_copy == NULL) {
          artwork_copy = malloc(body_len);
        }
        if (artwork_copy != NULL) {
          memcpy(artwork_copy, body, body_len);
          event_data.metadata.artwork_data = artwork_copy;
          event_data.metadata.artwork_len = body_len;
          event_data.metadata.has_artwork = true;
          has_metadata = true;
          ESP_LOGI("rtsp_handlers", "Received artwork: %s (%zu bytes)",
                   req->content_type, body_len);
        } else {
          ESP_LOGE("rtsp_handlers", "Artwork allocation failed: %s (%zu bytes)",
                   req->content_type, body_len);
        }
      }
    }
  } else if (strstr(req->content_type, "application/x-apple-binary-plist")) {
    if (body && body_len >= 8 && memcmp(body, "bplist00", 8) == 0) {
      int64_t value;
      if (bplist_find_int(body, body_len, "networkTimeSecs", &value)) {
        ESP_LOGI("rtsp_handlers", "SET_PARAMETER: networkTimeSecs=%lld",
                 (long long)value);
      }
      double rate;
      if (bplist_find_real(body, body_len, "rate", &rate)) {
        ESP_LOGI("rtsp_handlers", "SET_PARAMETER: rate=%.2f", rate);
      }
      char str_val[METADATA_STRING_MAX];
      if (bplist_find_string(body, body_len, "itemName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI("rtsp_handlers", "Metadata: Title = %s", str_val);
        strncpy(event_data.metadata.title, str_val, METADATA_STRING_MAX - 1);
        has_metadata = true;
      }
      if (bplist_find_string(body, body_len, "artistName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI("rtsp_handlers", "Metadata: Artist = %s", str_val);
        strncpy(event_data.metadata.artist, str_val, METADATA_STRING_MAX - 1);
        has_metadata = true;
      }
      if (bplist_find_string(body, body_len, "albumName", str_val,
                             sizeof(str_val))) {
        ESP_LOGI("rtsp_handlers", "Metadata: Album = %s", str_val);
        strncpy(event_data.metadata.album, str_val, METADATA_STRING_MAX - 1);
        has_metadata = true;
      }
      double elapsed = 0, duration = 0;
      if (bplist_find_real(body, body_len, "elapsed", &elapsed)) {
        event_data.metadata.position_secs = (uint32_t)elapsed;
        has_metadata = true;
        char elapsed_str[16];
        rtsp_format_time_mmss((uint32_t)elapsed, elapsed_str,
                              sizeof(elapsed_str));
        if (bplist_find_real(body, body_len, "duration", &duration)) {
          event_data.metadata.duration_secs = (uint32_t)duration;
          char duration_str[16];
          rtsp_format_time_mmss((uint32_t)duration, duration_str,
                                sizeof(duration_str));
          ESP_LOGI("rtsp_handlers", "Progress: %s / %s", elapsed_str,
                   duration_str);
        } else {
          ESP_LOGI("rtsp_handlers", "Progress: %s", elapsed_str);
        }
      }
    }
  }

  if (has_metadata) {
    rtsp_events_emit(RTSP_EVENT_METADATA, &event_data);
  }

  if (event_data.metadata.has_artwork && event_data.metadata.artwork_data) {
    free(event_data.metadata.artwork_data);
    event_data.metadata.artwork_data = NULL;
    event_data.metadata.artwork_len = 0;
    event_data.metadata.has_artwork = false;
  }

  rtsp_send_ok(socket, conn, req->cseq);
}

void rtsp_handle_get_parameter(int socket, rtsp_conn_t *conn,
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
