#include "web_server.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "settings.h"
#include "playback_control.h"
#include "audio/audio_receiver.h"
#include "led.h"
#include "wifi.h"
#include "ethernet.h"
#if CONFIG_WEB_OTA_UPDATE
#include "ota.h"
#endif
#if CONFIG_ENABLE_DEV_DIAGNOSTICS
#include "log_stream.h"
#endif
#include "rtsp_server.h"
#include "receiver_state.h"
#include "status_service.h"
#include "system_actions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_DAC_TAS58XX
#include "eq_events.h"
#include "dac_tas58xx_eq.h"
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#define SPIFFS_CHUNK_SIZE 1024
#define JSON_BODY_MAX_SIZE 1024
#define AUTH_TOKEN_BYTES   24
#define AUTH_TOKEN_HEX_LEN (AUTH_TOKEN_BYTES * 2)
#define AUTH_TTL_US        (15ULL * 60ULL * 1000000ULL)
#define BASE_URI_HANDLER_BUDGET 22
#define DIAG_URI_HANDLER_BUDGET 18
#define URI_HANDLER_HEADROOM    6

typedef struct {
  char token[AUTH_TOKEN_HEX_LEN + 1];
  uint64_t expires_at_us;
  bool active;
} auth_session_t;

static auth_session_t s_auth_session = {0};

static void bytes_to_hex(const uint8_t *bytes, size_t byte_len, char *out,
                         size_t out_len) {
  static const char hex[] = "0123456789abcdef";
  if (!bytes || !out || out_len < byte_len * 2 + 1) {
    return;
  }
  for (size_t i = 0; i < byte_len; i++) {
    out[i * 2] = hex[bytes[i] >> 4];
    out[i * 2 + 1] = hex[bytes[i] & 0x0F];
  }
  out[byte_len * 2] = '\0';
}

static void rotate_auth_token(char token[AUTH_TOKEN_HEX_LEN + 1]) {
  uint8_t bytes[AUTH_TOKEN_BYTES];
  esp_fill_random(bytes, sizeof(bytes));
  bytes_to_hex(bytes, sizeof(bytes), token, AUTH_TOKEN_HEX_LEN + 1);
}

static bool secure_streq(const char *lhs, const char *rhs) {
  if (!lhs || !rhs) {
    return false;
  }
  size_t lhs_len = strlen(lhs);
  size_t rhs_len = strlen(rhs);
  if (lhs_len != rhs_len) {
    return false;
  }

  unsigned char diff = 0;
  for (size_t i = 0; i < lhs_len; i++) {
    diff |= (unsigned char)(lhs[i] ^ rhs[i]);
  }
  return diff == 0;
}

static void invalidate_auth_session(void) {
  memset(&s_auth_session, 0, sizeof(s_auth_session));
}

static bool auth_session_is_valid(void) {
  return s_auth_session.active &&
         (uint64_t)esp_timer_get_time() < s_auth_session.expires_at_us;
}

static esp_err_t get_peer_ip(httpd_req_t *req, uint32_t *ip_addr) {
  if (!req || !ip_addr) {
    return ESP_ERR_INVALID_ARG;
  }

  int sockfd = httpd_req_to_sockfd(req);
  struct sockaddr_in peer_addr;
  socklen_t addr_len = sizeof(peer_addr);
  if (getpeername(sockfd, (struct sockaddr *)&peer_addr, &addr_len) != 0) {
    return ESP_FAIL;
  }

  *ip_addr = peer_addr.sin_addr.s_addr;
  return ESP_OK;
}

static bool request_is_ap_setup(httpd_req_t *req) {
  uint32_t peer_ip = 0;
  if (!wifi_is_ap_enabled()) {
    return false;
  }
  if (get_peer_ip(req, &peer_ip) != ESP_OK) {
    return false;
  }
  return wifi_is_ap_client_ip(peer_ip);
}

static bool request_get_token(httpd_req_t *req, char *token, size_t token_len) {
  if (!req || !token || token_len == 0) {
    return false;
  }

  if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", token, token_len) ==
      ESP_OK) {
    return token[0] != '\0';
  }

  char auth_hdr[96] = {0};
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr,
                                  sizeof(auth_hdr)) == ESP_OK) {
    const char *prefix = "Bearer ";
    size_t prefix_len = strlen(prefix);
    if (strncmp(auth_hdr, prefix, prefix_len) == 0) {
      strncpy(token, auth_hdr + prefix_len, token_len - 1);
      token[token_len - 1] = '\0';
      return token[0] != '\0';
    }
  }

  char query[128] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "token", token, token_len) == ESP_OK) {
    return token[0] != '\0';
  }

  return false;
}

static bool request_is_authenticated(httpd_req_t *req) {
#if !CONFIG_WEB_AUTH_REQUIRED
  (void)req;
  return true;
#else
  if (!auth_session_is_valid()) {
    invalidate_auth_session();
    return false;
  }

  char token[AUTH_TOKEN_HEX_LEN + 1] = {0};
  if (!request_get_token(req, token, sizeof(token))) {
    return false;
  }
  return secure_streq(token, s_auth_session.token);
#endif
}

static esp_err_t reject_auth(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", false);
  cJSON_AddStringToObject(json, "error", "Authentication required");
  char *json_str = cJSON_PrintUnformatted(json);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  cJSON_Delete(json);
  return ESP_FAIL;
}

static esp_err_t ensure_access(httpd_req_t *req, bool allow_setup_mode) {
#if !CONFIG_WEB_AUTH_REQUIRED
  (void)req;
  (void)allow_setup_mode;
  return ESP_OK;
#else
  if (allow_setup_mode && request_is_ap_setup(req)) {
    return ESP_OK;
  }
  if (request_is_authenticated(req)) {
    return ESP_OK;
  }
  return reject_auth(req);
#endif
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *json) {
  char *json_str = cJSON_PrintUnformatted(json);
  if (!json_str) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  esp_err_t err = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
  free(json_str);
  return err;
}

static esp_err_t read_request_body(httpd_req_t *req, char *buf, size_t buf_len,
                                   size_t max_allowed, size_t *out_len) {
  if (!req || !buf || buf_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (req->content_len <= 0 || (size_t)req->content_len > max_allowed ||
      (size_t)req->content_len >= buf_len) {
    return ESP_ERR_INVALID_SIZE;
  }

  size_t received = 0;
  while (received < (size_t)req->content_len) {
    int ret = httpd_req_recv(req, buf + received, req->content_len - received);
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (ret <= 0) {
      return ESP_FAIL;
    }
    received += (size_t)ret;
  }

  buf[received] = '\0';
  if (out_len) {
    *out_len = received;
  }
  return ESP_OK;
}

static esp_err_t schedule_restart(void) {
  return system_actions_schedule_restart();
}

static esp_err_t register_uri_handler_checked(httpd_handle_t server,
                                              const httpd_uri_t *uri,
                                              bool required) {
  if (!server || !uri) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = httpd_register_uri_handler(server, uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register %s %s: %s%s", 
             uri->method == HTTP_GET ? "GET" :
             uri->method == HTTP_POST ? "POST" :
             uri->method == HTTP_PUT ? "PUT" :
             uri->method == HTTP_DELETE ? "DELETE" : "HTTP",
             uri->uri ? uri->uri : "(null)", esp_err_to_name(err),
             required ? " [required]" : " [optional]");
  }
  return err;
}

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
static bool log_stream_auth_handler(httpd_req_t *req) {
  return request_is_authenticated(req);
}
#endif

static esp_err_t serve_spiffs_file(httpd_req_t *req, const char *path,
                                   const char *content_type) {
  FILE *f = fopen(path, "r");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open %s", path);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, content_type);
  char buf[SPIFFS_CHUNK_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
      fclose(f);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_FAIL;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// API handlers
static esp_err_t root_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/index.html", "text/html");
}

static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
static esp_err_t logs_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/logs.html", "text/html");
}

static esp_err_t core_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/core.html", "text/html");
}

static const char *playback_source_to_str(playback_source_t src) {
  switch (src) {
  case PLAYBACK_SOURCE_AIRPLAY:
    return "airplay";
  case PLAYBACK_SOURCE_BLUETOOTH:
    return "bluetooth";
  default:
    return "none";
  }
}

static double volume_db_to_percent(float volume_db) {
  if (volume_db <= -30.0f) {
    return 0.0;
  }
  if (volume_db >= 0.0f) {
    return 100.0;
  }
  return ((double)volume_db + 30.0) / 30.0 * 100.0;
}

static esp_err_t core_status_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  cJSON *json = cJSON_CreateObject();
  cJSON *status = cJSON_CreateObject();
  char device_name[65] = {0};
  receiver_state_snapshot_t snapshot = {0};

  settings_get_device_name(device_name, sizeof(device_name));
  receiver_state_get_snapshot(&snapshot);

  cJSON_AddStringToObject(status, "device_name", device_name);
  cJSON_AddStringToObject(status, "source",
                          playback_source_to_str(playback_control_get_source()));
  cJSON_AddBoolToObject(status, "playing", audio_receiver_is_playing());
  cJSON_AddBoolToObject(status, "wifi_connected", wifi_is_connected());
  cJSON_AddBoolToObject(status, "eth_connected", ethernet_is_connected());
  cJSON_AddNumberToObject(status, "free_heap", esp_get_free_heap_size());
  cJSON_AddBoolToObject(status, "ap_enabled", wifi_is_ap_enabled());
  cJSON_AddStringToObject(status, "system_state",
                          receiver_state_to_str(snapshot.state));

  const esp_app_desc_t *app_desc = esp_app_get_description();
  cJSON_AddStringToObject(status, "firmware_version", app_desc->version);

  cJSON_AddItemToObject(json, "status", status);
  cJSON_AddBoolToObject(json, "success", true);
  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t core_volume_get_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  cJSON *json = cJSON_CreateObject();
  float volume_db = -15.0f;
  esp_err_t err = settings_get_volume(&volume_db);
  if (err != ESP_OK) {
    volume_db = -15.0f;
  }

  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "volume_db", volume_db);
  cJSON_AddNumberToObject(json, "volume_percent", volume_db_to_percent(volume_db));

  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t core_volume_post_handler(httpd_req_t *req) {
  char content[128] = {0};
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  if (read_request_body(req, content, sizeof(content), sizeof(content) - 1,
                        NULL) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *volume_json = cJSON_GetObjectItem(json, "volume_db");
  cJSON *response = cJSON_CreateObject();

  if (!volume_json || !cJSON_IsNumber(volume_json)) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Missing numeric volume_db");
  } else {
    float volume_db = (float)volume_json->valuedouble;
    if (volume_db > 0.0f) {
      volume_db = 0.0f;
    }
    if (volume_db < -30.0f) {
      volume_db = -30.0f;
    }

    esp_err_t err = settings_set_volume(volume_db);
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    if (err == ESP_OK) {
      cJSON_AddNumberToObject(response, "volume_db", volume_db);
      cJSON_AddNumberToObject(response, "volume_percent",
                              volume_db_to_percent(volume_db));
    } else {
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  }

  send_json_response(req, response);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

static esp_err_t core_play_pause_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  (void)req;
  playback_control_play_pause();

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t core_volume_up_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  (void)req;
  playback_control_volume_up();

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t core_volume_down_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  (void)req;
  playback_control_volume_down();

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t core_audio_stats_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  cJSON *json = cJSON_CreateObject();
  cJSON *stats_json = cJSON_CreateObject();
  audio_stats_t stats = {0};
  audio_receiver_get_stats(&stats);

  cJSON_AddNumberToObject(stats_json, "packets_received", stats.packets_received);
  cJSON_AddNumberToObject(stats_json, "packets_decoded", stats.packets_decoded);
  cJSON_AddNumberToObject(stats_json, "packets_dropped", stats.packets_dropped);
  cJSON_AddNumberToObject(stats_json, "decrypt_errors", stats.decrypt_errors);
  cJSON_AddNumberToObject(stats_json, "buffer_underruns", stats.buffer_underruns);
  cJSON_AddNumberToObject(stats_json, "buffer_overruns", stats.buffer_overruns);
  cJSON_AddNumberToObject(stats_json, "late_frames", stats.late_frames);
  cJSON_AddNumberToObject(stats_json, "last_seq", stats.last_seq);
  cJSON_AddNumberToObject(stats_json, "output_latency_us",
                          audio_receiver_get_output_latency_us());
  cJSON_AddNumberToObject(stats_json, "hardware_latency_us",
                          audio_receiver_get_hardware_latency_us());
  audio_pipeline_snapshot_t pipeline = {0};
  audio_pipeline_get_snapshot(&pipeline);
  cJSON_AddNumberToObject(stats_json, "gap_concealment_blocks",
                          pipeline.gap_concealment_blocks);
  cJSON_AddNumberToObject(stats_json, "underrun_bursts",
                          pipeline.underrun_bursts);
  cJSON_AddNumberToObject(stats_json, "dsp_peak_dbfs_x100",
                          pipeline.dsp.peak_dbfs_x100);
  cJSON_AddNumberToObject(stats_json, "dsp_rms_dbfs_x100",
                          pipeline.dsp.rms_dbfs_x100);
  cJSON_AddNumberToObject(stats_json, "dsp_noise_floor_dbfs_x100",
                          pipeline.dsp.noise_floor_dbfs_x100);
  cJSON_AddNumberToObject(stats_json, "dsp_low_band_dbfs_x100",
                          pipeline.dsp.low_band_dbfs_x100);
  cJSON_AddNumberToObject(stats_json, "dsp_mid_band_dbfs_x100",
                          pipeline.dsp.mid_band_dbfs_x100);
  cJSON_AddNumberToObject(stats_json, "dsp_high_band_dbfs_x100",
                          pipeline.dsp.high_band_dbfs_x100);
  cJSON_AddNumberToObject(stats_json, "dsp_gate_gain_pct",
                          pipeline.dsp.gate_gain_pct);
  cJSON_AddNumberToObject(stats_json, "dsp_compressor_gain_pct",
                          pipeline.dsp.compressor_gain_pct);
  cJSON_AddNumberToObject(stats_json, "dsp_limiter_events",
                          pipeline.dsp.limiter_events);

  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddItemToObject(json, "stats", stats_json);

  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t core_led_state_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  cJSON *json = cJSON_CreateObject();
  led_mode_t status_mode = LED_OFF;
  led_mode_t rgb_mode = LED_OFF;
  bool error_active = false;
  uint8_t brightness = 0;

  led_get_snapshot(&status_mode, &rgb_mode, &error_active, &brightness);

  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "status_mode", status_mode);
  cJSON_AddNumberToObject(json, "rgb_mode", rgb_mode);
  cJSON_AddBoolToObject(json, "error", error_active);
  cJSON_AddNumberToObject(json, "brightness", brightness);

  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t core_led_post_handler(httpd_req_t *req) {
  char content[192] = {0};
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  if (read_request_body(req, content, sizeof(content), sizeof(content) - 1,
                        NULL) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *brightness_json = cJSON_GetObjectItem(json, "brightness");
  cJSON *mode_json = cJSON_GetObjectItem(json, "status_mode");
  cJSON *response = cJSON_CreateObject();
  esp_err_t err = ESP_OK;

  if (brightness_json && cJSON_IsNumber(brightness_json)) {
    int brightness = brightness_json->valueint;
    if (brightness < 0) {
      brightness = 0;
    }
    if (brightness > 255) {
      brightness = 255;
    }
    err = led_set_status_brightness((uint8_t)brightness);
  }

  if (err == ESP_OK && mode_json && cJSON_IsNumber(mode_json)) {
    int mode = mode_json->valueint;
    if (mode < LED_OFF || mode > LED_VU) {
      err = ESP_ERR_INVALID_ARG;
    } else {
      err = led_set_status_mode((led_mode_t)mode);
    }
  }

  cJSON_AddBoolToObject(response, "success", err == ESP_OK);
  if (err != ESP_OK) {
    cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
  }

  send_json_response(req, response);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}
#endif

static esp_err_t auth_status_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  receiver_state_snapshot_t snapshot = {0};
  receiver_state_get_snapshot(&snapshot);

  bool authenticated = request_is_authenticated(req);
  bool ap_setup = request_is_ap_setup(req);

  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddBoolToObject(json, "authenticated", authenticated);
  cJSON_AddBoolToObject(json, "ap_setup", ap_setup);
  cJSON_AddStringToObject(json, "system_state",
                          receiver_state_to_str(snapshot.state));
  cJSON_AddBoolToObject(json, "auth_required", CONFIG_WEB_AUTH_REQUIRED);

#if CONFIG_WEB_AUTH_REQUIRED
  if (ap_setup && !authenticated) {
    char secret[33] = {0};
    if (settings_get_management_secret(secret, sizeof(secret)) == ESP_OK) {
      cJSON_AddStringToObject(json, "bootstrap_secret", secret);
    }
  }
#endif

  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t auth_login_handler(httpd_req_t *req) {
  char content[JSON_BODY_MAX_SIZE + 1] = {0};
  if (read_request_body(req, content, sizeof(content), JSON_BODY_MAX_SIZE,
                        NULL) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *secret_json = cJSON_GetObjectItem(json, "secret");
  cJSON *response = cJSON_CreateObject();
  char expected_secret[33] = {0};
  if (!secret_json || !cJSON_IsString(secret_json) ||
      settings_get_management_secret(expected_secret, sizeof(expected_secret)) !=
          ESP_OK ||
      !secure_streq(cJSON_GetStringValue(secret_json), expected_secret)) {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid management key");
    httpd_resp_set_status(req, "403 Forbidden");
  } else {
    rotate_auth_token(s_auth_session.token);
    s_auth_session.expires_at_us = (uint64_t)esp_timer_get_time() + AUTH_TTL_US;
    s_auth_session.active = true;
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "token", s_auth_session.token);
    cJSON_AddNumberToObject(response, "expires_in_sec",
                            (double)(AUTH_TTL_US / 1000000ULL));
  }

  send_json_response(req, response);
  cJSON_Delete(response);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t auth_logout_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  invalidate_auth_session();
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

// Captive portal detection handlers
// These endpoints are requested by various OS to detect captive portals
static esp_err_t captive_portal_redirect(httpd_req_t *req) {
  // Redirect to the configuration page
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  if (ensure_access(req, true) != ESP_OK) {
    return ESP_FAIL;
  }
  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;

  cJSON *json = cJSON_CreateObject();
  esp_err_t err = wifi_scan(&ap_list, &ap_count);

  if (err == ESP_OK && ap_list) {
    cJSON *networks = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_count; i++) {
      cJSON *net = cJSON_CreateObject();
      cJSON_AddStringToObject(net, "ssid", (char *)ap_list[i].ssid);
      cJSON_AddNumberToObject(net, "rssi", ap_list[i].rssi);
      cJSON_AddNumberToObject(net, "channel", ap_list[i].primary);
      cJSON_AddItemToArray(networks, net);
    }
    cJSON_AddItemToObject(json, "networks", networks);
    cJSON_AddBoolToObject(json, "success", true);
    free(ap_list);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", esp_err_to_name(err));
  }

  send_json_response(req, json);
  cJSON_Delete(json);

  return ESP_OK;
}

static esp_err_t wifi_config_handler(httpd_req_t *req) {
  char content[JSON_BODY_MAX_SIZE + 1] = {0};
  if (ensure_access(req, true) != ESP_OK) {
    return ESP_FAIL;
  }
  if (read_request_body(req, content, sizeof(content), JSON_BODY_MAX_SIZE,
                        NULL) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *ssid_json = cJSON_GetObjectItem(json, "ssid");
  cJSON *password_json = cJSON_GetObjectItem(json, "password");

  cJSON *response = cJSON_CreateObject();
  if (ssid_json && cJSON_IsString(ssid_json)) {
    const char *ssid = cJSON_GetStringValue(ssid_json);
    const char *password = password_json && cJSON_IsString(password_json)
                               ? cJSON_GetStringValue(password_json)
                               : "";

    esp_err_t err = settings_set_wifi_credentials(ssid, password);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
      cJSON_AddStringToObject(response, "message",
                              "WiFi credentials saved. Restarting");
      schedule_restart();
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid SSID");
  }

  send_json_response(req, response);
  cJSON_Delete(json);
  cJSON_Delete(response);

  return ESP_OK;
}

static esp_err_t device_name_handler(httpd_req_t *req) {
  char content[JSON_BODY_MAX_SIZE + 1] = {0};
  if (ensure_access(req, true) != ESP_OK) {
    return ESP_FAIL;
  }
  if (read_request_body(req, content, sizeof(content), JSON_BODY_MAX_SIZE,
                        NULL) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *name_json = cJSON_GetObjectItem(json, "name");
  cJSON *response = cJSON_CreateObject();

  if (name_json && cJSON_IsString(name_json)) {
    const char *name = cJSON_GetStringValue(name_json);
    esp_err_t err = settings_set_device_name(name);
    if (err == ESP_OK) {
      cJSON_AddBoolToObject(response, "success", true);
    } else {
      cJSON_AddBoolToObject(response, "success", false);
      cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error", "Invalid name");
  }

  send_json_response(req, response);
  cJSON_Delete(json);
  cJSON_Delete(response);

  return ESP_OK;
}

static esp_err_t wifi_reset_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  esp_err_t err = settings_clear_wifi_credentials();
  cJSON *response = cJSON_CreateObject();

  if (err == ESP_OK) {
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message",
                            "Saved WiFi credentials cleared");
    send_json_response(req, response);
    cJSON_Delete(response);

    ESP_LOGW(TAG, "WiFi credentials cleared from web request, rebooting...");
    schedule_restart();
    return ESP_OK;
  }

  cJSON_AddBoolToObject(response, "success", false);
  cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
  send_json_response(req, response);
  cJSON_Delete(response);
  return ESP_FAIL;
}

static esp_err_t hap_reset_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  // AirPlay 1 has no HAP pairing data to clear
  cJSON *response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "success", true);
  cJSON_AddStringToObject(response, "message",
                          "No pairing data in AirPlay 1 mode");
  send_json_response(req, response);
  cJSON_Delete(response);
  return ESP_OK;
}

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
#if CONFIG_WEB_OTA_UPDATE
static esp_err_t ota_update_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  if (req->content_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No firmware uploaded");
    return ESP_FAIL;
  }

  // Stop AirPlay to free resources during OTA
  ESP_LOGI(TAG, "Stopping AirPlay for OTA update");
  receiver_state_set_ota_in_progress(true);
  rtsp_server_stop();

  esp_err_t err = ota_start_from_http(req);

  if (err != ESP_OK) {
    receiver_state_set_ota_in_progress(false);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        esp_err_to_name(err));
    return ESP_FAIL;
  }

  // Send response before restarting
  httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();

  return ESP_OK;
}
#endif
#endif

static esp_err_t status_handler(httpd_req_t *req) {
  if (ensure_access(req, true) != ESP_OK) {
    return ESP_FAIL;
  }
  cJSON *json = cJSON_CreateObject();
  cJSON *status = cJSON_CreateObject();
  cJSON *pipeline = cJSON_CreateObject();
  status_service_snapshot_t snapshot = {0};

  status_service_get_snapshot(&snapshot);

  cJSON_AddStringToObject(status, "ip", snapshot.ip);
  cJSON_AddStringToObject(status, "mac", snapshot.mac);
  cJSON_AddStringToObject(status, "device_name", snapshot.device_name);
  cJSON_AddStringToObject(status, "firmware_version",
                          snapshot.firmware_version);
  cJSON_AddStringToObject(status, "receiver_state", snapshot.receiver_state);
  cJSON_AddStringToObject(status, "playback_source", snapshot.playback_source);
  cJSON_AddStringToObject(status, "track_title", snapshot.track_title);
  cJSON_AddStringToObject(status, "track_artist", snapshot.track_artist);
  cJSON_AddBoolToObject(status, "wifi_connected", snapshot.wifi_connected);
  cJSON_AddBoolToObject(status, "eth_connected", snapshot.eth_connected);
  cJSON_AddBoolToObject(status, "ap_enabled", snapshot.ap_enabled);
  cJSON_AddBoolToObject(status, "playing", snapshot.playing);
  cJSON_AddNumberToObject(status, "free_heap", snapshot.free_heap);
  cJSON_AddNumberToObject(status, "min_free_heap", snapshot.min_free_heap);
  cJSON_AddNumberToObject(status, "largest_internal_block",
                          snapshot.largest_internal_block);
  cJSON_AddNumberToObject(status, "free_psram", snapshot.free_psram);
  cJSON_AddNumberToObject(status, "largest_psram_block",
                          snapshot.largest_psram_block);
  cJSON_AddNumberToObject(status, "reconnect_count", snapshot.reconnect_count);

  cJSON_AddStringToObject(pipeline, "codec", snapshot.pipeline.codec);
  cJSON_AddNumberToObject(pipeline, "buffer_depth_frames",
                          snapshot.pipeline.buffer_depth_frames);
  cJSON_AddNumberToObject(pipeline, "output_latency_us",
                          snapshot.pipeline.output_latency_us);
  cJSON_AddNumberToObject(pipeline, "hardware_latency_us",
                          snapshot.pipeline.hardware_latency_us);
  cJSON_AddNumberToObject(pipeline, "target_latency_ms",
                          snapshot.pipeline.target_latency_ms);
  cJSON_AddNumberToObject(pipeline, "stream_port", snapshot.pipeline.stream_port);
  cJSON_AddNumberToObject(pipeline, "buffered_port",
                          snapshot.pipeline.buffered_port);
  cJSON_AddNumberToObject(pipeline, "avg_task_load_pct",
                          snapshot.pipeline.avg_task_load_pct);
  cJSON_AddNumberToObject(pipeline, "peak_task_load_pct",
                          snapshot.pipeline.peak_task_load_pct);
  cJSON_AddNumberToObject(pipeline, "packets_received",
                          snapshot.pipeline.stats.packets_received);
  cJSON_AddNumberToObject(pipeline, "packets_decoded",
                          snapshot.pipeline.stats.packets_decoded);
  cJSON_AddNumberToObject(pipeline, "packets_dropped",
                          snapshot.pipeline.stats.packets_dropped);
  cJSON_AddNumberToObject(pipeline, "decrypt_errors",
                          snapshot.pipeline.stats.decrypt_errors);
  cJSON_AddNumberToObject(pipeline, "buffer_underruns",
                          snapshot.pipeline.stats.buffer_underruns);
  cJSON_AddNumberToObject(pipeline, "buffer_overruns",
                          snapshot.pipeline.stats.buffer_overruns);
  cJSON_AddNumberToObject(pipeline, "gap_concealment_blocks",
                          snapshot.pipeline.gap_concealment_blocks);
  cJSON_AddNumberToObject(pipeline, "underrun_bursts",
                          snapshot.pipeline.underrun_bursts);
  cJSON_AddNumberToObject(pipeline, "dsp_peak_dbfs_x100",
                          snapshot.pipeline.dsp.peak_dbfs_x100);
  cJSON_AddNumberToObject(pipeline, "dsp_rms_dbfs_x100",
                          snapshot.pipeline.dsp.rms_dbfs_x100);
  cJSON_AddNumberToObject(pipeline, "dsp_noise_floor_dbfs_x100",
                          snapshot.pipeline.dsp.noise_floor_dbfs_x100);
  cJSON_AddNumberToObject(pipeline, "dsp_low_band_dbfs_x100",
                          snapshot.pipeline.dsp.low_band_dbfs_x100);
  cJSON_AddNumberToObject(pipeline, "dsp_mid_band_dbfs_x100",
                          snapshot.pipeline.dsp.mid_band_dbfs_x100);
  cJSON_AddNumberToObject(pipeline, "dsp_high_band_dbfs_x100",
                          snapshot.pipeline.dsp.high_band_dbfs_x100);
  cJSON_AddNumberToObject(pipeline, "dsp_gate_gain_pct",
                          snapshot.pipeline.dsp.gate_gain_pct);
  cJSON_AddNumberToObject(pipeline, "dsp_compressor_gain_pct",
                          snapshot.pipeline.dsp.compressor_gain_pct);
  cJSON_AddNumberToObject(pipeline, "dsp_limiter_events",
                          snapshot.pipeline.dsp.limiter_events);

  cJSON_AddItemToObject(json, "status", status);
  cJSON_AddItemToObject(json, "pipeline", pipeline);
  cJSON_AddBoolToObject(json, "success", true);

  send_json_response(req, json);
  cJSON_Delete(json);

  return ESP_OK;
}

static esp_err_t system_restart_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);

  send_json_response(req, json);
  cJSON_Delete(json);

  ESP_LOGI(TAG, "Restart requested via web interface");
  schedule_restart();

  return ESP_OK;
}

/* ================================================================== */
/*  SPIFFS File Management API                                         */
/* ================================================================== */

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
// Allowed path prefixes for file upload (prevent writes outside SPIFFS)
static const char *ALLOWED_PREFIXES[] = {"/spiffs/uploads/"};

static bool has_allowed_extension(const char *path) {
  static const char *extensions[] = {".txt", ".json", ".cfg",
                                     ".bin", ".html", ".css", ".js"};
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }
  for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
    if (strcmp(dot, extensions[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool is_path_allowed(const char *path) {
  for (int i = 0; i < sizeof(ALLOWED_PREFIXES) / sizeof(ALLOWED_PREFIXES[0]);
       i++) {
    if (strncmp(path, ALLOWED_PREFIXES[i], strlen(ALLOWED_PREFIXES[i])) == 0) {
      // Reject path traversal
      if (strstr(path, "..") != NULL) {
        return false;
      }
      return has_allowed_extension(path);
    }
  }
  return false;
}

static esp_err_t fs_upload_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  // Get target path from query string
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  if (req->content_len == 0 || req->content_len > 64 * 1024) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body required (max 64KB)");
    return ESP_FAIL;
  }

  mkdir("/spiffs/uploads", 0777);
  FILE *f = fopen(path, "wb");
  if (!f) {
    ESP_LOGE(TAG, "Failed to create %s", path);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to create file");
    return ESP_FAIL;
  }

  char buf[SPIFFS_CHUNK_SIZE];
  int remaining = req->content_len;
  while (remaining > 0) {
    int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
    int received = httpd_req_recv(req, buf, to_read);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (received <= 0) {
      fclose(f);
      remove(path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receive failed");
      return ESP_FAIL;
    }
    if (fwrite(buf, 1, received, f) != (size_t)received) {
      fclose(f);
      remove(path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Write failed");
      return ESP_FAIL;
    }
    remaining -= received;
  }
  fclose(f);

  ESP_LOGI(TAG, "Uploaded %d bytes to %s", req->content_len, path);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "size", req->content_len);
  cJSON_AddStringToObject(json, "path", path);
  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_delete_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  char query[128] = {0};
  char path[64] = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "path", path, sizeof(path)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing 'path' query parameter");
    return ESP_FAIL;
  }

  if (!is_path_allowed(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_CreateObject();
  if (remove(path) == 0) {
    ESP_LOGI(TAG, "Deleted %s", path);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "File not found");
  }
  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t fs_list_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  char query[128] = {0};
  char dir_path[64] = "/spiffs/uploads";

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "dir", dir_path, sizeof(dir_path));
  }

  if (strcmp(dir_path, "/spiffs/uploads") != 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path not allowed");
    return ESP_FAIL;
  }

  DIR *d = opendir(dir_path);
  cJSON *json = cJSON_CreateObject();
  cJSON *files = cJSON_CreateArray();

  if (d) {
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", entry->d_name);

      char full_path[320];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
      struct stat st;
      if (stat(full_path, &st) == 0) {
        cJSON_AddNumberToObject(item, "size", st.st_size);
      }
      cJSON_AddItemToArray(files, item);
    }
    closedir(d);
    cJSON_AddBoolToObject(json, "success", true);
  } else {
    cJSON_AddBoolToObject(json, "success", false);
    cJSON_AddStringToObject(json, "error", "Cannot open directory");
  }

  cJSON_AddItemToObject(json, "files", files);
  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}
#endif

/* ================================================================== */
/*  EQ Page + API  (only when TAS58xx DAC is configured)               */
/* ================================================================== */

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
#ifdef CONFIG_DAC_TAS58XX

static esp_err_t eq_page_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/www/eq.html", "text/html");
}

static esp_err_t eq_get_handler(httpd_req_t *req) {
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  cJSON *json = cJSON_CreateObject();
  cJSON *arr = cJSON_CreateArray();

  float gains[SETTINGS_EQ_BANDS];
  if (settings_get_eq_gains(gains) == ESP_OK) {
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)gains[i]));
    }
  } else {
    /* No saved EQ — return all zeros (flat) */
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON_AddItemToArray(arr, cJSON_CreateNumber(0.0));
    }
  }

  cJSON_AddItemToObject(json, "gains", arr);
  cJSON_AddNumberToObject(json, "bands", SETTINGS_EQ_BANDS);
  cJSON_AddBoolToObject(json, "success", true);

  send_json_response(req, json);
  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t eq_post_handler(httpd_req_t *req) {
  char content[512];
  if (ensure_access(req, false) != ESP_OK) {
    return ESP_FAIL;
  }
  if (read_request_body(req, content, sizeof(content), sizeof(content) - 1,
                        NULL) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    return ESP_FAIL;
  }

  cJSON *json = cJSON_Parse(content);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *gains_arr = cJSON_GetObjectItem(json, "gains");

  if (gains_arr && cJSON_IsArray(gains_arr) &&
      cJSON_GetArraySize(gains_arr) == SETTINGS_EQ_BANDS) {

    float gains[SETTINGS_EQ_BANDS];
    for (int i = 0; i < SETTINGS_EQ_BANDS; i++) {
      cJSON *item = cJSON_GetArrayItem(gains_arr, i);
      gains[i] = cJSON_IsNumber(item) ? (float)item->valuedouble : 0.0f;
      /* Clamp */
      if (gains[i] > 15.0f) {
        gains[i] = 15.0f;
      }
      if (gains[i] < -15.0f) {
        gains[i] = -15.0f;
      }
    }

    /* Emit event — listeners (settings + DAC) will handle it */
    eq_event_data_t ev_data;
    memcpy(ev_data.all_bands.gains_db, gains, sizeof(gains));
    eq_events_emit(EQ_EVENT_ALL_BANDS_SET, &ev_data);

    cJSON_AddBoolToObject(response, "success", true);
  } else {
    cJSON_AddBoolToObject(response, "success", false);
    cJSON_AddStringToObject(response, "error",
                            "Expected 'gains' array with 15 values");
  }

  send_json_response(req, response);
  cJSON_Delete(json);
  cJSON_Delete(response);
  return ESP_OK;
}

#endif /* CONFIG_DAC_TAS58XX */
#endif /* CONFIG_ENABLE_DEV_DIAGNOSTICS */

esp_err_t web_server_start(uint16_t port) {
  if (s_server) {
    ESP_LOGW(TAG, "Web server already running");
    return ESP_OK;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.uri_match_fn = httpd_uri_match_wildcard;
#ifdef CONFIG_BT_ENABLED
  config.max_open_sockets = 2;   // BT: tighter socket budget (LWIP 12)
  config.send_wait_timeout = 10; // BT/WiFi coexistence slows TCP drain
#else
  config.max_open_sockets = 3; // Limit to save lwIP socket slots for AirPlay
#endif
  config.lru_purge_enable = true; // Reclaim stale sockets when all are in use
  config.max_uri_handlers = BASE_URI_HANDLER_BUDGET + URI_HANDLER_HEADROOM;
  config.max_resp_headers = 8;
  config.stack_size = 6144;

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
  config.max_uri_handlers =
      BASE_URI_HANDLER_BUDGET + DIAG_URI_HANDLER_BUDGET + URI_HANDLER_HEADROOM;
  config.stack_size = 8192;
#endif

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
    return err;
  }

  // Register handlers
  httpd_uri_t root_uri = {
      .uri = "/", .method = HTTP_GET, .handler = root_handler};
  if ((err = register_uri_handler_checked(s_server, &root_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t favicon_uri = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler};
  if ((err = register_uri_handler_checked(s_server, &favicon_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t auth_status_uri = {.uri = "/api/auth/status",
                                 .method = HTTP_GET,
                                 .handler = auth_status_handler};
  if ((err = register_uri_handler_checked(s_server, &auth_status_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t auth_login_uri = {.uri = "/api/auth/login",
                                .method = HTTP_POST,
                                .handler = auth_login_handler};
  if ((err = register_uri_handler_checked(s_server, &auth_login_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t auth_logout_uri = {.uri = "/api/auth/logout",
                                 .method = HTTP_POST,
                                 .handler = auth_logout_handler};
  if ((err = register_uri_handler_checked(s_server, &auth_logout_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t wifi_scan_uri = {.uri = "/api/wifi/scan",
                               .method = HTTP_GET,
                               .handler = wifi_scan_handler};
  if ((err = register_uri_handler_checked(s_server, &wifi_scan_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t wifi_config_uri = {.uri = "/api/wifi/config",
                                 .method = HTTP_POST,
                                 .handler = wifi_config_handler};
  if ((err = register_uri_handler_checked(s_server, &wifi_config_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t device_name_uri = {.uri = "/api/device/name",
                                 .method = HTTP_POST,
                                 .handler = device_name_handler};
  if ((err = register_uri_handler_checked(s_server, &device_name_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t wifi_reset_uri = {.uri = "/api/wifi/reset",
                                .method = HTTP_POST,
                                .handler = wifi_reset_handler};
  if ((err = register_uri_handler_checked(s_server, &wifi_reset_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t hap_reset_uri = {.uri = "/api/airplay/reset-pairing",
                               .method = HTTP_POST,
                               .handler = hap_reset_handler};
  if ((err = register_uri_handler_checked(s_server, &hap_reset_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t status_uri = {.uri = "/api/status",
                            .method = HTTP_GET,
                            .handler = status_handler};
  if ((err = register_uri_handler_checked(s_server, &status_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t legacy_status_uri = {.uri = "/api/system/info",
                                   .method = HTTP_GET,
                                   .handler = status_handler};
  if ((err = register_uri_handler_checked(s_server, &legacy_status_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t system_restart_uri = {.uri = "/api/system/restart",
                                    .method = HTTP_POST,
                                    .handler = system_restart_handler};
  if ((err = register_uri_handler_checked(s_server, &system_restart_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  // Captive portal detection endpoints
  static const char *captive_uris[] = {
      "/hotspot-detect.html", "/library/test/success.html",
      "/generate_204",        "/connecttest.txt",
      "/ncsi.txt",            "/gen_204",
      "/success.txt",         "/canonical.html",
      "/fwlink/*",
  };
  for (size_t i = 0; i < sizeof(captive_uris) / sizeof(captive_uris[0]); i++) {
    httpd_uri_t captive = {.uri = captive_uris[i],
                           .method = HTTP_GET,
                           .handler = captive_portal_redirect};
    if ((err = register_uri_handler_checked(s_server, &captive, true)) != ESP_OK) {
      httpd_stop(s_server);
      s_server = NULL;
      return err;
    }
  }

#if CONFIG_ENABLE_DEV_DIAGNOSTICS
  httpd_uri_t logs_uri = {
      .uri = "/logs", .method = HTTP_GET, .handler = logs_page_handler};
  if ((err = register_uri_handler_checked(s_server, &logs_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_uri = {
      .uri = "/core", .method = HTTP_GET, .handler = core_page_handler};
  if ((err = register_uri_handler_checked(s_server, &core_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_status_uri = {.uri = "/api/core/status",
                                 .method = HTTP_GET,
                                 .handler = core_status_handler};
  if ((err = register_uri_handler_checked(s_server, &core_status_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_volume_get_uri = {.uri = "/api/core/volume",
                                     .method = HTTP_GET,
                                     .handler = core_volume_get_handler};
  if ((err = register_uri_handler_checked(s_server, &core_volume_get_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_volume_post_uri = {.uri = "/api/core/volume",
                                      .method = HTTP_POST,
                                      .handler = core_volume_post_handler};
  if ((err = register_uri_handler_checked(s_server, &core_volume_post_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_play_pause_uri = {.uri = "/api/core/play-pause",
                                     .method = HTTP_POST,
                                     .handler = core_play_pause_handler};
  if ((err = register_uri_handler_checked(s_server, &core_play_pause_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_volume_up_uri = {.uri = "/api/core/volume/up",
                                    .method = HTTP_POST,
                                    .handler = core_volume_up_handler};
  if ((err = register_uri_handler_checked(s_server, &core_volume_up_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_volume_down_uri = {.uri = "/api/core/volume/down",
                                      .method = HTTP_POST,
                                      .handler = core_volume_down_handler};
  if ((err = register_uri_handler_checked(s_server, &core_volume_down_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_audio_stats_uri = {.uri = "/api/core/audio/stats",
                                      .method = HTTP_GET,
                                      .handler = core_audio_stats_handler};
  if ((err = register_uri_handler_checked(s_server, &core_audio_stats_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_led_state_uri = {.uri = "/api/core/led/state",
                                    .method = HTTP_GET,
                                    .handler = core_led_state_handler};
  if ((err = register_uri_handler_checked(s_server, &core_led_state_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t core_led_post_uri = {.uri = "/api/core/led",
                                   .method = HTTP_POST,
                                   .handler = core_led_post_handler};
  if ((err = register_uri_handler_checked(s_server, &core_led_post_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t legacy_hap_reset_uri = {.uri = "/api/hap/reset",
                                      .method = HTTP_POST,
                                      .handler = hap_reset_handler};
  if ((err = register_uri_handler_checked(s_server, &legacy_hap_reset_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

#if CONFIG_WEB_OTA_UPDATE
  httpd_uri_t ota_uri = {.uri = "/api/ota/update",
                         .method = HTTP_POST,
                         .handler = ota_update_handler};
  if ((err = register_uri_handler_checked(s_server, &ota_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }
#endif

  httpd_uri_t fs_upload_uri = {.uri = "/api/fs/upload",
                               .method = HTTP_POST,
                               .handler = fs_upload_handler};
  if ((err = register_uri_handler_checked(s_server, &fs_upload_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t fs_delete_uri = {.uri = "/api/fs/delete",
                               .method = HTTP_POST,
                               .handler = fs_delete_handler};
  if ((err = register_uri_handler_checked(s_server, &fs_delete_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t fs_list_uri = {
      .uri = "/api/fs/list", .method = HTTP_GET, .handler = fs_list_handler};
  if ((err = register_uri_handler_checked(s_server, &fs_list_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

#ifdef CONFIG_DAC_TAS58XX
  httpd_uri_t eq_page_uri = {
      .uri = "/eq", .method = HTTP_GET, .handler = eq_page_handler};
  if ((err = register_uri_handler_checked(s_server, &eq_page_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t eq_get_uri = {
      .uri = "/api/eq", .method = HTTP_GET, .handler = eq_get_handler};
  if ((err = register_uri_handler_checked(s_server, &eq_get_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  httpd_uri_t eq_post_uri = {
      .uri = "/api/eq", .method = HTTP_POST, .handler = eq_post_handler};
  if ((err = register_uri_handler_checked(s_server, &eq_post_uri, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }
#endif
#endif

  httpd_uri_t catch_all = {.uri = "/*",
                           .method = HTTP_GET,
                           .handler = captive_portal_redirect};
  if ((err = register_uri_handler_checked(s_server, &catch_all, true)) != ESP_OK) {
    httpd_stop(s_server);
    s_server = NULL;
    return err;
  }

  // Registration of /ws/logs is now handled safely within log_stream_register
#if CONFIG_ENABLE_DEV_DIAGNOSTICS
  log_stream_set_auth_callback(log_stream_auth_handler);
  log_stream_register(s_server);
#endif

  ESP_LOGI(TAG, "Web server started on port %d", port);
  return ESP_OK;
}

void web_server_stop(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "Web server stopped");
  }
}
