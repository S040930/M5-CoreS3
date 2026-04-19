#include "usb_control_service.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "audio/audio_output.h"
#include "audio/audio_receiver.h"
#include "status_service.h"
#include "settings.h"
#include "wifi.h"
#include "ethernet.h"
#include "system_actions.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if CONFIG_USB_WEB_CONTROL

static const char *TAG = "usb_control";

#define USB_CONTROL_PREFIX     "@usbctl "
#define USB_CONTROL_MAX_LINE   1024
#define USB_CONTROL_MAX_ID_LEN 32
#define USB_CONTROL_TASK_STACK 6144
#define USB_CONTROL_TASK_PRIO  4

static TaskHandle_t s_usb_task = NULL;
static bool s_usb_driver_ready = false;

static void copy_str(char *dst, size_t dst_len, const char *src) {
  if (!dst || dst_len == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = '\0';
}

static void add_error(cJSON *response, const char *code, const char *message) {
  cJSON *error = cJSON_CreateObject();
  if (!error) {
    return;
  }

  cJSON_AddStringToObject(error, "code", code ? code : "internal_error");
  cJSON_AddStringToObject(error, "message",
                          message ? message : "Internal error");
  cJSON_AddItemToObject(response, "error", error);
}

static void send_prefixed_json(cJSON *json) {
  char *payload = cJSON_PrintUnformatted(json);
  if (!payload) {
    return;
  }

  printf(USB_CONTROL_PREFIX "%s\n", payload);
  fflush(stdout);
  free(payload);
}

static void send_error_response(const char *id, const char *code,
                                const char *message) {
  cJSON *response = cJSON_CreateObject();
  if (!response) {
    return;
  }

  if (id && id[0] != '\0') {
    cJSON_AddStringToObject(response, "id", id);
  }
  cJSON_AddBoolToObject(response, "ok", false);
  add_error(response, code, message);
  send_prefixed_json(response);
  cJSON_Delete(response);
}

static void add_status_result(cJSON *result) {
  status_service_snapshot_t snapshot = {0};
  cJSON *status = cJSON_CreateObject();
  cJSON *pipeline = cJSON_CreateObject();
  if (!status || !pipeline) {
    cJSON_Delete(status);
    cJSON_Delete(pipeline);
    return;
  }

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

  cJSON_AddItemToObject(result, "status", status);
  cJSON_AddItemToObject(result, "pipeline", pipeline);
}

static esp_err_t dispatch_wifi_scan(cJSON *result) {
  wifi_ap_record_t *ap_list = NULL;
  uint16_t ap_count = 0;
  esp_err_t err = wifi_scan(&ap_list, &ap_count);
  if (err != ESP_OK) {
    return err;
  }

  cJSON *networks = cJSON_CreateArray();
  if (!networks) {
    free(ap_list);
    return ESP_ERR_NO_MEM;
  }

  for (uint16_t i = 0; i < ap_count; i++) {
    cJSON *net = cJSON_CreateObject();
    if (!net) {
      free(ap_list);
      cJSON_Delete(networks);
      return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(net, "ssid", (const char *)ap_list[i].ssid);
    cJSON_AddNumberToObject(net, "rssi", ap_list[i].rssi);
    cJSON_AddNumberToObject(net, "channel", ap_list[i].primary);
    cJSON_AddItemToArray(networks, net);
  }

  cJSON_AddItemToObject(result, "networks", networks);
  free(ap_list);
  return ESP_OK;
}

static int get_optional_int(const cJSON *args, const char *name, int fallback) {
  if (!args || !name) {
    return fallback;
  }

  const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)args, name);
  if (!cJSON_IsNumber(item)) {
    return fallback;
  }
  return item->valueint;
}

static esp_err_t add_audio_diag_result(cJSON *result) {
  audio_output_diag_t diag = {0};
  esp_err_t err = audio_output_get_diag(&diag);
  if (err != ESP_OK) {
    return err;
  }

  cJSON_AddBoolToObject(result, "speaker_open", diag.speaker_open);
  cJSON_AddNumberToObject(result, "output_rate", diag.output_rate);
  cJSON_AddNumberToObject(result, "bits_per_sample", diag.bits_per_sample);
  cJSON_AddNumberToObject(result, "channels", diag.channels);
  cJSON_AddNumberToObject(result, "channel_mask", diag.channel_mask);
  cJSON_AddNumberToObject(result, "mclk_multiple", diag.mclk_multiple);
  cJSON_AddBoolToObject(result, "mute", diag.muted);
  cJSON_AddNumberToObject(result, "volume", diag.volume);
  cJSON_AddNumberToObject(result, "current_volume_db", diag.current_volume_db);
  cJSON_AddNumberToObject(result, "target_volume_db", diag.target_volume_db);
  cJSON_AddBoolToObject(result, "volume_ramping", diag.ramping);

  cJSON *aw88298 = cJSON_CreateObject();
  if (!aw88298) {
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddNumberToObject(aw88298, "reg04", diag.reg04);
  cJSON_AddNumberToObject(aw88298, "reg05", diag.reg05);
  cJSON_AddNumberToObject(aw88298, "reg06", diag.reg06);
  cJSON_AddNumberToObject(aw88298, "reg0c", diag.reg0c);
  cJSON_AddNumberToObject(aw88298, "reg12", diag.reg12);
  cJSON_AddNumberToObject(aw88298, "reg14", diag.reg14);
  cJSON_AddItemToObject(result, "aw88298", aw88298);
  return ESP_OK;
}

static esp_err_t get_required_string(const cJSON *args, const char *name,
                                     char *buf, size_t buf_len) {
  if (!args || !name || !buf || buf_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)args, name);
  if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  copy_str(buf, buf_len, item->valuestring);
  return ESP_OK;
}

static esp_err_t get_optional_string(const cJSON *args, const char *name,
                                     char *buf, size_t buf_len) {
  if (!buf || buf_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  buf[0] = '\0';
  if (!args || !name) {
    return ESP_OK;
  }

  const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)args, name);
  if (!item) {
    return ESP_OK;
  }
  if (!cJSON_IsString(item) || !item->valuestring) {
    return ESP_ERR_INVALID_ARG;
  }

  copy_str(buf, buf_len, item->valuestring);
  return ESP_OK;
}

esp_err_t usb_control_dispatch(const char *cmd, const cJSON *args,
                               cJSON *result) {
  if (!cmd || !result) {
    return ESP_ERR_INVALID_ARG;
  }

  if (strcmp(cmd, "ping") == 0) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(result, "pong", "ok");
    cJSON_AddStringToObject(result, "firmware_version",
                            app_desc ? app_desc->version : "");
    return ESP_OK;
  }

  if (strcmp(cmd, "get_status") == 0) {
    add_status_result(result);
    return ESP_OK;
  }

  if (strcmp(cmd, "wifi_scan") == 0) {
    return dispatch_wifi_scan(result);
  }

  if (strcmp(cmd, "wifi_config_set") == 0) {
    char ssid[33];
    char password[65];
    esp_err_t err = get_required_string(args, "ssid", ssid, sizeof(ssid));
    if (err != ESP_OK) {
      return ESP_ERR_INVALID_ARG;
    }

    err = get_optional_string(args, "password", password, sizeof(password));
    if (err != ESP_OK) {
      return ESP_ERR_INVALID_ARG;
    }

    err = settings_set_wifi_credentials(ssid, password);
    memset(password, 0, sizeof(password));
    if (err != ESP_OK) {
      return err;
    }

    cJSON_AddStringToObject(result, "message",
                            "WiFi credentials saved. Restarting");
    return system_actions_schedule_restart();
  }

  if (strcmp(cmd, "device_name_set") == 0) {
    char name[65];
    esp_err_t err = get_required_string(args, "name", name, sizeof(name));
    if (err != ESP_OK) {
      return ESP_ERR_INVALID_ARG;
    }
    return settings_set_device_name(name);
  }

  if (strcmp(cmd, "system_restart") == 0) {
    cJSON_AddStringToObject(result, "message", "Restart scheduled");
    return system_actions_schedule_restart();
  }

  if (strcmp(cmd, "audio_diag_get") == 0) {
    return add_audio_diag_result(result);
  }

  if (strcmp(cmd, "audio_test_tone") == 0) {
    int frequency_hz = get_optional_int(args, "frequency_hz", 1000);
    int duration_ms = get_optional_int(args, "duration_ms", 800);
    int amplitude_pct = get_optional_int(args, "amplitude_pct", 20);

    if (frequency_hz <= 0 || duration_ms <= 0 || amplitude_pct <= 0) {
      return ESP_ERR_INVALID_ARG;
    }
    if (amplitude_pct > 100) {
      amplitude_pct = 100;
    }

    cJSON_AddNumberToObject(result, "frequency_hz", frequency_hz);
    cJSON_AddNumberToObject(result, "duration_ms", duration_ms);
    cJSON_AddNumberToObject(result, "amplitude_pct", amplitude_pct);

    if (audio_receiver_is_playing()) {
      cJSON_AddBoolToObject(result, "started", false);
      cJSON_AddStringToObject(result, "error", "busy");
      return ESP_OK;
    }

    esp_err_t err = audio_output_play_test_tone((uint32_t)frequency_hz,
                                                (uint32_t)duration_ms,
                                                (uint8_t)amplitude_pct);
    if (err != ESP_OK) {
      return err;
    }
    cJSON_AddBoolToObject(result, "started", true);
    return ESP_OK;
  }

  if (strcmp(cmd, "airplay_reset_pairing") == 0) {
    // AirPlay 1 has no HAP pairing data to clear
    cJSON_AddStringToObject(result, "message", "No pairing data in AirPlay 1 mode");
    return ESP_OK;
  }

  if (strcmp(cmd, "wifi_reset") == 0) {
    esp_err_t err = settings_clear_wifi_credentials();
    if (err != ESP_OK) {
      return err;
    }
    cJSON_AddStringToObject(result, "message",
                            "WiFi credentials cleared. Restarting");
    return system_actions_schedule_restart();
  }

  return ESP_ERR_NOT_SUPPORTED;
}

static void handle_request_line(const char *line) {
  char id[USB_CONTROL_MAX_ID_LEN + 1] = {0};
  cJSON *request = cJSON_Parse(line);
  if (!request) {
    send_error_response(NULL, "invalid_json", "Invalid JSON request");
    return;
  }

  cJSON *id_item = cJSON_GetObjectItemCaseSensitive(request, "id");
  if (cJSON_IsString(id_item) && id_item->valuestring) {
    copy_str(id, sizeof(id), id_item->valuestring);
  }

  cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(request, "cmd");
  cJSON *args_item = cJSON_GetObjectItemCaseSensitive(request, "args");
  if (!cJSON_IsString(cmd_item) || !cmd_item->valuestring) {
    send_error_response(id, "invalid_request", "Missing command");
    cJSON_Delete(request);
    return;
  }

  if (args_item && !cJSON_IsObject(args_item)) {
    send_error_response(id, "invalid_request", "args must be an object");
    cJSON_Delete(request);
    return;
  }

  cJSON *response = cJSON_CreateObject();
  cJSON *result = cJSON_CreateObject();
  if (!response || !result) {
    cJSON_Delete(request);
    cJSON_Delete(response);
    cJSON_Delete(result);
    send_error_response(id, "internal_error", "Out of memory");
    return;
  }

  if (id[0] != '\0') {
    cJSON_AddStringToObject(response, "id", id);
  }

  esp_err_t err = usb_control_dispatch(cmd_item->valuestring, args_item, result);
  if (err == ESP_OK) {
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddItemToObject(response, "result", result);
  } else {
    cJSON_AddBoolToObject(response, "ok", false);
    cJSON_Delete(result);
    add_error(response,
              err == ESP_ERR_NOT_SUPPORTED ? "unknown_command"
              : err == ESP_ERR_INVALID_ARG ? "invalid_args"
              : "command_failed",
              err == ESP_ERR_NOT_SUPPORTED ? "Unknown command"
              : err == ESP_ERR_INVALID_ARG ? "Invalid command arguments"
              : esp_err_to_name(err));
  }

  send_prefixed_json(response);
  cJSON_Delete(response);
  cJSON_Delete(request);
}

static void flush_oversize_line(void) {
  int ch = 0;
  while ((ch = getchar()) != '\n' && ch != EOF) {
  }
}

static void usb_control_task(void *arg) {
  (void)arg;
  char line[USB_CONTROL_MAX_LINE + 2];

  while (1) {
    if (!fgets(line, sizeof(line), stdin)) {
      vTaskDelay(pdMS_TO_TICKS(50));
      clearerr(stdin);
      continue;
    }

    size_t len = strlen(line);
    if (len == 0) {
      continue;
    }

    if (line[len - 1] != '\n' && !feof(stdin)) {
      flush_oversize_line();
      send_error_response(NULL, "line_too_long",
                          "Request exceeds maximum line length");
      continue;
    }

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (len == 0) {
      continue;
    }

    handle_request_line(line);
  }
}

esp_err_t usb_control_service_init(void) {
  if (!s_usb_driver_ready) {
    fflush(stdout);
    fsync(fileno(stdout));

    /* Web Serial (Chromium) sends LF-terminated lines; LF mode passes them
     * through unchanged (see usb_serial_jtag_vfs_set_rx_line_endings). */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);

    if (!usb_serial_jtag_is_driver_installed()) {
      usb_serial_jtag_driver_config_t config =
          USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
      config.rx_buffer_size = USB_CONTROL_MAX_LINE + 64;
      config.tx_buffer_size = USB_CONTROL_MAX_LINE + 128;

      esp_err_t err = usb_serial_jtag_driver_install(&config);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB Serial/JTAG driver: %s",
                 esp_err_to_name(err));
        return err;
      }
    }

    usb_serial_jtag_vfs_use_driver();
    s_usb_driver_ready = true;
  }

  setvbuf(stdin, NULL, _IONBF, 0);
  setvbuf(stdout, NULL, _IONBF, 0);
  ESP_LOGI(TAG, "USB Serial/JTAG control channel initialized");
  return ESP_OK;
}

esp_err_t usb_control_service_start(void) {
  if (s_usb_task) {
    return ESP_OK;
  }

  BaseType_t ok = xTaskCreate(usb_control_task, "usb_control",
                              USB_CONTROL_TASK_STACK, NULL,
                              USB_CONTROL_TASK_PRIO, &s_usb_task);
  if (ok != pdPASS) {
    s_usb_task = NULL;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "USB web control ready on console serial");
  return ESP_OK;
}

void usb_control_service_stop(void) {
  if (!s_usb_task) {
    return;
  }
  vTaskDelete(s_usb_task);
  s_usb_task = NULL;
}

#else

esp_err_t usb_control_service_init(void) { return ESP_OK; }
esp_err_t usb_control_service_start(void) { return ESP_OK; }
void usb_control_service_stop(void) {}
esp_err_t usb_control_dispatch(const char *cmd, const cJSON *args,
                               cJSON *result) {
  (void)cmd;
  (void)args;
  (void)result;
  return ESP_ERR_NOT_SUPPORTED;
}

#endif
