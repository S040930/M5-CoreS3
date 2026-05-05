#include "voice_tools.h"

#include "airplay_service.h"
#include "audio/audio_output.h"
#include "audio_volume.h"
#include "bsp/display.h"
#include "realtime_voice.h"
#include "receiver_state.h"
#include "voice_timers.h"
#include "wifi.h"

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define TAG "voice_tools"
#define VOLUME_MIN_DB (-30.0f)
#define VOLUME_MAX_DB (0.0f)
#define VOLUME_DEFAULT_STEP_PERCENT (10.0f)

static void write_json_error(char *output, size_t output_cap, const char *err) {
  snprintf(output, output_cap, "{\"ok\":false,\"error\":\"%s\"}", err);
}

static float clamp_float(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static float volume_percent_to_db(float percent) {
  float p = clamp_float(percent, 0.0f, 100.0f);
  return VOLUME_MIN_DB + (p / 100.0f) * (VOLUME_MAX_DB - VOLUME_MIN_DB);
}

static float volume_db_to_percent(float db) {
  float clamped = clamp_float(db, VOLUME_MIN_DB, VOLUME_MAX_DB);
  return ((clamped - VOLUME_MIN_DB) * 100.0f) / (VOLUME_MAX_DB - VOLUME_MIN_DB);
}

static void write_volume_json(char *output, size_t output_cap, float volume_db,
                              bool muted) {
  audio_output_diag_t diag = {0};
  bool has_diag = audio_output_get_diag(&diag) == ESP_OK;
  float percent = volume_db_to_percent(volume_db);
  snprintf(output, output_cap,
           "{\"ok\":true,\"volume_percent\":%.0f,\"volume_db\":%.2f,"
           "\"muted\":%s,\"target_db\":%.2f,\"current_db\":%.2f,\"hw_volume\":%d}",
           percent, volume_db, muted ? "true" : "false",
           has_diag ? diag.target_volume_db : volume_db,
           has_diag ? diag.current_volume_db : volume_db,
           has_diag ? diag.volume : -1);
}

bool voice_tools_dispatch(const char *name, const char *arguments_json, char *output, size_t output_cap) {
  if (output == NULL || output_cap == 0) {
    return false;
  }
  output[0] = '\0';
  if (name == NULL || name[0] == '\0') {
    write_json_error(output, output_cap, "missing tool name");
    return true;
  }
#if CONFIG_VOICE_TOOLS_ENABLE
  const char *args_in = (arguments_json != NULL) ? arguments_json : "{}";
  if (strcmp(name, "set_volume") == 0) {
    cJSON *root = cJSON_Parse(args_in);
    if (root == NULL) {
      write_json_error(output, output_cap, "invalid arguments json");
      return true;
    }
    const cJSON *percent_j = cJSON_GetObjectItemCaseSensitive(root, "percent");
    const cJSON *delta_j = cJSON_GetObjectItemCaseSensitive(root, "delta_percent");
    const cJSON *muted_j = cJSON_GetObjectItemCaseSensitive(root, "muted");
    bool has_percent = cJSON_IsNumber(percent_j);
    bool has_delta = cJSON_IsNumber(delta_j);
    bool has_muted = cJSON_IsBool(muted_j);
    if (!has_percent && !has_delta && !has_muted) {
      cJSON_Delete(root);
      write_json_error(output, output_cap, "missing percent, delta_percent, or muted");
      return true;
    }

    float db = -15.0f;
    (void)audio_volume_get(&db);
    float percent = volume_db_to_percent(db);
    if (has_percent) {
      percent = (float)cJSON_GetNumberValue(percent_j);
    }
    if (has_delta) {
      percent += (float)cJSON_GetNumberValue(delta_j);
    }
    percent = clamp_float(percent, 0.0f, 100.0f);

    if (has_percent || has_delta) {
      db = volume_percent_to_db(percent);
      (void)audio_volume_save(db);
      (void)audio_volume_persist();
    }
    bool muted = false;
    if (has_muted) {
      muted = cJSON_IsTrue(muted_j);
      audio_output_set_muted(muted);
    } else {
      audio_output_diag_t diag = {0};
      muted = audio_output_get_diag(&diag) == ESP_OK && diag.muted;
    }
    cJSON_Delete(root);
    write_volume_json(output, output_cap, db, muted);
    return true;
  }

  if (strcmp(name, "get_volume") == 0) {
    float db = -15.0f;
    (void)audio_volume_get(&db);
    audio_output_diag_t diag = {0};
    bool muted = audio_output_get_diag(&diag) == ESP_OK && diag.muted;
    write_volume_json(output, output_cap, db, muted);
    return true;
  }

  if (strcmp(name, "set_timer") == 0) {
    cJSON *root = cJSON_Parse(args_in);
    if (root == NULL) {
      write_json_error(output, output_cap, "invalid arguments json");
      return true;
    }
    const cJSON *dur = cJSON_GetObjectItemCaseSensitive(root, "duration_sec");
    if (!cJSON_IsNumber(dur)) {
      cJSON_Delete(root);
      write_json_error(output, output_cap, "missing duration_sec");
      return true;
    }
    double dv = cJSON_GetNumberValue(dur);
    if (dv <= 0 || dv > 86400.0) {
      cJSON_Delete(root);
      write_json_error(output, output_cap, "duration_sec out of range");
      return true;
    }
    uint32_t sec = (uint32_t)(dv + 0.5);
    if (sec == 0) {
      sec = 1;
    }
    const cJSON *lab = cJSON_GetObjectItemCaseSensitive(root, "label");
    const char *lab_s = NULL;
    char lab_buf[48];
    lab_buf[0] = '\0';
    if (cJSON_IsString(lab) && lab->valuestring != NULL) {
      snprintf(lab_buf, sizeof(lab_buf), "%s", lab->valuestring);
      lab_s = lab_buf;
    }
    cJSON_Delete(root);
    uint32_t tid = voice_timers_set(sec, lab_s);
    if (tid == 0) {
      write_json_error(output, output_cap, "no timer slot or timers not initialized");
      return true;
    }
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
      write_json_error(output, output_cap, "oom");
      return true;
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "timer_id", (double)tid);
    cJSON_AddNumberToObject(out, "duration_sec", (double)sec);
    if (lab_s != NULL) {
      cJSON_AddStringToObject(out, "label", lab_s);
    } else {
      cJSON_AddStringToObject(out, "label", "");
    }
    char *printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (printed == NULL) {
      write_json_error(output, output_cap, "oom");
      return true;
    }
    snprintf(output, output_cap, "%s", printed);
    free(printed);
    return true;
  }

  if (strcmp(name, "cancel_timer") == 0) {
    cJSON *root = cJSON_Parse(args_in);
    if (root == NULL) {
      write_json_error(output, output_cap, "invalid arguments json");
      return true;
    }
    const cJSON *tidj = cJSON_GetObjectItemCaseSensitive(root, "timer_id");
    if (!cJSON_IsNumber(tidj)) {
      cJSON_Delete(root);
      write_json_error(output, output_cap, "missing timer_id");
      return true;
    }
    uint32_t tid = (uint32_t)cJSON_GetNumberValue(tidj);
    cJSON_Delete(root);
    bool ok = voice_timers_cancel(tid);
    snprintf(output, output_cap, "{\"ok\":%s,\"timer_id\":%lu}", ok ? "true" : "false",
             (unsigned long)tid);
    return true;
  }

  if (strcmp(name, "get_time") == 0) {
    time_t t = time(NULL);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    char timestr[16];
    strftime(timestr, sizeof(timestr), "%H:%M:%S", &tm_local);
    snprintf(output, output_cap,
             "{\"ok\":true,\"time\":\"%s\",\"unix\":%lld}",
             timestr, (long long)t);
    return true;
  }

  if (strcmp(name, "get_date") == 0) {
    time_t t = time(NULL);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    char datestr[24];
    char dowstr[16];
    strftime(datestr, sizeof(datestr), "%Y-%m-%d", &tm_local);
    strftime(dowstr, sizeof(dowstr), "%A", &tm_local);
    snprintf(output, output_cap,
             "{\"ok\":true,\"date\":\"%s\",\"weekday\":\"%s\",\"unix\":%lld}",
             datestr, dowstr, (long long)t);
    return true;
  }

  if (strcmp(name, "get_network_status") == 0) {
    receiver_state_snapshot_t snap = {0};
    receiver_state_get_snapshot(&snap);
    bool wifi = wifi_is_connected();
    char ip[20] = "";
    if (wifi) {
      (void)wifi_get_ip_str(ip, sizeof(ip));
    }
    snprintf(output, output_cap,
             "{\"ok\":true,\"wifi_connected\":%s,\"ip\":\"%s\","
             "\"receiver_state\":\"%s\",\"network_ready\":%s,"
             "\"streaming\":%s,\"discoverable\":%s}",
             wifi ? "true" : "false", ip, receiver_state_to_str(snap.state),
             snap.network_ready ? "true" : "false",
             snap.streaming ? "true" : "false",
             snap.discoverable ? "true" : "false");
    return true;
  }

  if (strcmp(name, "set_screen_brightness") == 0) {
    cJSON *root = cJSON_Parse(args_in);
    if (root == NULL) {
      write_json_error(output, output_cap, "invalid arguments json");
      return true;
    }
    const cJSON *bp = cJSON_GetObjectItemCaseSensitive(root, "brightness_percent");
    if (!cJSON_IsNumber(bp)) {
      cJSON_Delete(root);
      write_json_error(output, output_cap, "missing brightness_percent");
      return true;
    }
    int pct = (int)cJSON_GetNumberValue(bp);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    esp_err_t err = bsp_display_brightness_set(pct);
    cJSON_Delete(root);
    if (err != ESP_OK) {
      snprintf(output, output_cap,
               "{\"ok\":false,\"error\":\"brightness_set_failed\",\"esp_err\":%d}",
               (int)err);
      return true;
    }
    snprintf(output, output_cap, "{\"ok\":true,\"brightness_percent\":%d}", pct);
    return true;
  }

  if (strcmp(name, "play_local_chime") == 0) {
    uint32_t freq_hz      = 880;
    uint32_t duration_ms  = 120;
    uint32_t amplitude_pct = 25;
    cJSON *root = cJSON_Parse(args_in);
    if (root != NULL) {
      const cJSON *fj = cJSON_GetObjectItemCaseSensitive(root, "frequency_hz");
      const cJSON *dj = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
      const cJSON *aj = cJSON_GetObjectItemCaseSensitive(root, "amplitude_pct");
      if (cJSON_IsNumber(fj)) {
        freq_hz = (uint32_t)cJSON_GetNumberValue(fj);
      }
      if (cJSON_IsNumber(dj)) {
        duration_ms = (uint32_t)cJSON_GetNumberValue(dj);
      }
      if (cJSON_IsNumber(aj)) {
        amplitude_pct = (uint32_t)cJSON_GetNumberValue(aj);
      }
      cJSON_Delete(root);
    }
    if (freq_hz == 0) freq_hz = 880;
    if (freq_hz > 8000) freq_hz = 8000;
    if (duration_ms == 0) duration_ms = 120;
    if (duration_ms > 3000) duration_ms = 3000;
    if (amplitude_pct > 100) amplitude_pct = 100;
    esp_err_t err =
        audio_output_play_test_tone(freq_hz, duration_ms, (uint8_t)amplitude_pct);
    if (err != ESP_OK) {
      snprintf(output, output_cap,
               "{\"ok\":false,\"error\":\"chime_failed\",\"esp_err\":%d}",
               (int)err);
      return true;
    }
    snprintf(output, output_cap,
             "{\"ok\":true,\"frequency_hz\":%lu,\"duration_ms\":%lu,"
             "\"amplitude_pct\":%lu}",
             (unsigned long)freq_hz, (unsigned long)duration_ms,
             (unsigned long)amplitude_pct);
    return true;
  }

  if (strcmp(name, "airplay_status") == 0) {
    bool active = airplay_service_is_active();
    snprintf(output, output_cap, "{\"ok\":true,\"airplay_active\":%s}",
             active ? "true" : "false");
    return true;
  }

  if (strcmp(name, "get_device_status") == 0) {
    receiver_state_snapshot_t snap = {0};
    receiver_state_get_snapshot(&snap);
    bool wifi = wifi_is_connected();
    char ip[20] = "";
    if (wifi) {
      (void)wifi_get_ip_str(ip, sizeof(ip));
    }
    int timers = voice_timers_active_count();
    bool act_armed = realtime_voice_is_activation_armed();
    snprintf(output, output_cap,
             "{\"ok\":true,\"wifi_connected\":%s,\"ip\":\"%s\","
             "\"receiver_state\":\"%s\",\"network_ready\":%s,\"streaming\":%s,"
             "\"active_timers\":%d,\"activation_armed\":%s}",
             wifi ? "true" : "false", ip, receiver_state_to_str(snap.state),
             snap.network_ready ? "true" : "false", snap.streaming ? "true" : "false", timers,
             act_armed ? "true" : "false");
    return true;
  }
#endif

  write_json_error(output, output_cap, "unsupported tool");
  return true;
}
