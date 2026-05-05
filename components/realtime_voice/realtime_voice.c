#include "realtime_voice.h"

#include "airplay_service.h"
#include "receiver_state.h"
#include "screen_ui.h"

#include "audio/audio_output.h"
#include "cJSON.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "voice_playout.h"
#include "voice_timers.h"
#include "voice_tools.h"
#include "afe_bridge.h"
#include "voice_dsp.h"
#include "voice_frontend.h"
#include "voice_reference.h"
#include "voice_request.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_VOICE_SESSION_IDLE_TIMEOUT_MS
#define CONFIG_VOICE_SESSION_IDLE_TIMEOUT_MS 15000
#endif

#ifndef CONFIG_VOICE_CONTEXT_MAX_CHARS
#define CONFIG_VOICE_CONTEXT_MAX_CHARS 256
#endif

#ifndef CONFIG_VOICE_MIC_IN_GAIN_DB
#define CONFIG_VOICE_MIC_IN_GAIN_DB 33
#endif

#ifndef CONFIG_VOICE_DEBUG_TLS_HEAP
#define CONFIG_VOICE_DEBUG_TLS_HEAP 0
#endif

#ifndef CONFIG_VOICE_TOOLS_ENABLE
#define CONFIG_VOICE_TOOLS_ENABLE 0
#endif

#ifndef CONFIG_VOICE_WS_SEND_TIMEOUT_MS
#define CONFIG_VOICE_WS_SEND_TIMEOUT_MS 5000
#endif

#ifndef CONFIG_VOICE_VAD_CONSECUTIVE_FRAMES
#define CONFIG_VOICE_VAD_CONSECUTIVE_FRAMES 2
#endif

#define TAG "realtime_voice"
#define VOICE_TASK_STACK 20480
#define VOICE_TASK_PRIO 4
#define VOICE_MAX_SUMMARY_CHARS 256
#define WS_ACCUM_SIZE 32768
#define VOICE_OWNER_TAG "realtime_voice"
#define VOICE_PLAYBACK_BOOST_DB 0.0f
#define VOICE_HW_CHANNELS 2
#define VOICE_HW_CHANNEL_MASK 0x03
#define VOICE_MIC_WARMUP_MS 120
#define VOICE_MIC_EMPTY_READ_TOLERANCE 16
#define VOICE_MIC_EMPTY_READ_LOG_INTERVAL 8
#define VOICE_MIC_PREREAD_ATTEMPTS 20
#define VOICE_MIC_PREREAD_DELAY_MS 20
#define VOICE_PLAYBACK_PEAK_LOG_INTERVAL 40U
#define VOICE_PLAYBACK_CLIP_RISK_PEAK 30000
#define VOICE_ONESHOT_RECORD_MAX_MS 5000
#define VOICE_ONESHOT_SILENCE_MS 900
#define VOICE_ONESHOT_SAMPLE_RATE 16000

/* Playout-ring tunables now live in voice_playout.h. */

#if CONFIG_VOICE_DEBUG_TLS_HEAP
static void voice_log_heap_tls(const char *where) {
  ESP_LOGI(TAG,
           "heap[%s] int_free=%lu int_largest=%lu dma_largest=%lu spiram_free=%lu",
           where, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
#else
static void voice_log_heap_tls(const char *where) { (void)where; }
#endif

static portMUX_TYPE s_act_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_session_armed;

static void session_arm_set(bool armed) {
  bool changed;
  portENTER_CRITICAL(&s_act_mux);
  changed = s_session_armed != armed;
  s_session_armed = armed;
  portEXIT_CRITICAL(&s_act_mux);
  if (changed) {
    ESP_LOGI(TAG, "activation armed=%d", armed ? 1 : 0);
  }
}

static bool session_arm_get(void) {
  portENTER_CRITICAL(&s_act_mux);
  bool v = s_session_armed;
  portEXIT_CRITICAL(&s_act_mux);
  return v;
}

static char s_err_detail[128];
static realtime_voice_config_t s_voice_config = {0};

static uint64_t now_ms(void);

typedef enum {
  VOICE_SESSION_NONE = 0,
  VOICE_SESSION_ACTIVE,
} voice_session_state_t;

typedef enum {
  VOICE_GATE_OPEN = 0,
  VOICE_GATE_BLOCKED_BY_AIRPLAY,
} voice_gate_state_t;

typedef struct {
  TaskHandle_t task;
  bool running;
  bool enabled;
  bool connected;
  bool session_ready;
  bool ws_disconnected;
  bool waiting_response;
  bool speaking;
  bool interrupt_requested;
  bool user_speech_notified;
  bool speaker_owned;
  bool playback_active;
  bool playout_prefilled;
  bool response_audio_closed;
  bool response_cancelled;
  bool response_create_sent;
  bool speaker_volume_saved;
  float speaker_volume_restore_db;
  uint64_t speaking_ended_ms;
  device_mode_t mode;
  realtime_voice_state_t state;
  voice_session_state_t session_state;
  esp_websocket_client_handle_t ws;
  esp_codec_dev_handle_t mic;
  esp_codec_dev_handle_t spk;
  SemaphoreHandle_t lock;
  char *ws_accum;
  size_t ws_accum_cap;
  size_t ws_accum_len;
  int16_t *playout_pop_buf;
  size_t playout_pop_cap;
  int16_t *playout_hw_buf;
  size_t playout_hw_cap;
  int16_t *playout_stereo_buf;
  size_t playout_stereo_cap;
  char last_user[SCREEN_UI_TEXT_MAX];
  char last_assistant[SCREEN_UI_TEXT_MAX];
  char session_user_summary[VOICE_MAX_SUMMARY_CHARS];
  char session_assistant_summary[VOICE_MAX_SUMMARY_CHARS];
  uint64_t session_started_ms;
  uint64_t session_last_active_ms;
  uint64_t ws_connect_started_ms;
  uint64_t ws_next_retry_ms;
  uint32_t ws_retry_delay_ms;
  uint64_t last_append_ms;
  uint32_t append_ok_count;
  uint64_t last_ws_recv_ms;
  uint64_t last_ws_send_ms;
  char last_ws_send_type[32];
  /** Wall-clock anchor for TTFB logging (set on `response.created`). */
  uint64_t response_latency_anchor_ms;
  bool response_first_audio_logged;
#if CONFIG_VOICE_TOOLS_ENABLE
  bool server_response_active;
  bool tool_call_inflight;
  bool tool_followup_pending;
  char last_tool_call_id[80];
#endif
} realtime_ctx_t;

static realtime_ctx_t s_ctx = {0};

#define VOICE_LOCAL_VAD_CONSECUTIVE_FRAMES ((uint32_t)CONFIG_VOICE_VAD_CONSECUTIVE_FRAMES)

/* Playout ring buffer + last-stereo cache moved to voice_playout.{h,c}. */

static void playout_workbufs_release(void) {
  voice_buf_free(s_ctx.playout_pop_buf);
  s_ctx.playout_pop_buf = NULL;
  s_ctx.playout_pop_cap = 0;
  voice_buf_free(s_ctx.playout_hw_buf);
  s_ctx.playout_hw_buf = NULL;
  s_ctx.playout_hw_cap = 0;
  voice_buf_free(s_ctx.playout_stereo_buf);
  s_ctx.playout_stereo_buf = NULL;
  s_ctx.playout_stereo_cap = 0;
}

static bool playout_workbufs_ensure(size_t pop_samples, size_t hw_samples, size_t stereo_samples) {
  if (s_ctx.playout_pop_cap < pop_samples) {
    voice_buf_free(s_ctx.playout_pop_buf);
    s_ctx.playout_pop_buf = (int16_t *)voice_buf_alloc(pop_samples * sizeof(int16_t));
    s_ctx.playout_pop_cap = s_ctx.playout_pop_buf != NULL ? pop_samples : 0;
  }
  if (s_ctx.playout_hw_cap < hw_samples) {
    voice_buf_free(s_ctx.playout_hw_buf);
    s_ctx.playout_hw_buf = (int16_t *)voice_buf_alloc(hw_samples * sizeof(int16_t));
    s_ctx.playout_hw_cap = s_ctx.playout_hw_buf != NULL ? hw_samples : 0;
  }
  if (s_ctx.playout_stereo_cap < stereo_samples) {
    voice_buf_free(s_ctx.playout_stereo_buf);
    s_ctx.playout_stereo_buf = (int16_t *)voice_buf_alloc(stereo_samples * sizeof(int16_t));
    s_ctx.playout_stereo_cap = s_ctx.playout_stereo_buf != NULL ? stereo_samples : 0;
  }
  return s_ctx.playout_pop_buf != NULL && s_ctx.playout_hw_buf != NULL && s_ctx.playout_stereo_buf != NULL;
}

/* soft-knee limiter: tanh-based, kicks in above VOICE_SOFT_LIMIT_RATIO * 32768 */
static inline float soft_limit_f32(float sample) {
  const float limit = 0.95f;
  if (sample > limit) {
    return limit + (1.0f - limit) * tanhf((sample - limit) / (1.0f - limit));
  }
  if (sample < -limit) {
    return -limit - (1.0f - limit) * tanhf((-sample - limit) / (1.0f - limit));
  }
  return sample;
}

static int16_t soft_clip_i16(int16_t sample) {
  float f = (float)sample / 32768.0f;
  if (f > 0.95f || f < -0.95f) {
    f = soft_limit_f32(f);
    int v = (int)(f * 32768.0f + (f > 0 ? 0.5f : -0.5f));
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
  }
  return sample;
}

static void record_ws_send_type(const char *type) {
  if (type == NULL || type[0] == '\0') {
    return;
  }
  snprintf(s_ctx.last_ws_send_type, sizeof(s_ctx.last_ws_send_type), "%s", type);
  s_ctx.last_ws_send_ms = now_ms();
}

static void log_ws_disconnect_diag(const char *reason) {
  uint64_t now = now_ms();
  uint64_t last_send_age = s_ctx.last_ws_send_ms > 0 ? (now - s_ctx.last_ws_send_ms) : 0;
  uint64_t last_append_age = s_ctx.last_append_ms > 0 ? (now - s_ctx.last_append_ms) : 0;
  ESP_LOGW(TAG,
           "ws diag[%s]: connected=%d ready=%d waiting=%d speaking=%d resp_sent=%d append_ok=%lu "
           "last_send=%s age=%llums last_append_age=%llums",
           reason != NULL ? reason : "unknown", s_ctx.connected ? 1 : 0,
           s_ctx.session_ready ? 1 : 0, s_ctx.waiting_response ? 1 : 0,
           s_ctx.speaking ? 1 : 0, s_ctx.response_create_sent ? 1 : 0,
           (unsigned long)s_ctx.append_ok_count,
           s_ctx.last_ws_send_type[0] != '\0' ? s_ctx.last_ws_send_type : "none",
           (unsigned long long)last_send_age, (unsigned long long)last_append_age);
}

static screen_ui_voice_state_t to_screen_state(realtime_voice_state_t state) {
  switch (state) {
  case REALTIME_VOICE_STATE_STANDBY:
    return SCREEN_UI_VOICE_STANDBY;
  case REALTIME_VOICE_STATE_CONNECTING:
    return SCREEN_UI_VOICE_CONNECTING;
  case REALTIME_VOICE_STATE_LISTENING:
    return SCREEN_UI_VOICE_LISTENING;
  case REALTIME_VOICE_STATE_SENDING:
    return SCREEN_UI_VOICE_SENDING;
  case REALTIME_VOICE_STATE_THINKING:
    return SCREEN_UI_VOICE_THINKING;
  case REALTIME_VOICE_STATE_SPEAKING:
    return SCREEN_UI_VOICE_SPEAKING;
  case REALTIME_VOICE_STATE_ERROR:
    return SCREEN_UI_VOICE_ERROR;
  case REALTIME_VOICE_STATE_OFF:
  default:
    return SCREEN_UI_VOICE_OFF;
  }
}

static const char *prompt_preset_instructions(void) {
#if CONFIG_VOICE_PROMPT_PRESET_CONVERSATIONAL
  return "You are a realtime assistant running on M5 CoreS3. Follow user language (Chinese or English), default to Chinese when unclear. Speak naturally in short 1-3 sentence replies for voice playback. Be warm and conversational while staying accurate. For complex requests give a short answer first then ask if user wants details. If user interrupts, stop the previous answer naturally. Do not claim device control or real-world action execution. Avoid long lists, long numbers, and markdown.";
#elif CONFIG_VOICE_PROMPT_PRESET_FACTUAL
  return "You are a realtime assistant running on M5 CoreS3. Follow user language (Chinese or English), default to Chinese when unclear. Reply in concise 1-3 sentence factual answers suitable for TTS. If unsure, say uncertainty directly and never fabricate. For complex requests give a short answer first then ask whether to continue. If user interrupts, stop prior output naturally. Do not claim device control or real-world action execution. Avoid long lists, long numbers, and markdown.";
#else
  return "You are a realtime assistant running on M5 CoreS3. Follow user language (Chinese or English), default to Chinese when unclear. Reply with natural spoken 1-3 sentence answers suitable for TTS. Prioritize chat Q&A only. If unsure, say uncertainty directly and do not fabricate. For complex requests give a short answer first then ask whether user wants details. If user interrupts, stop prior output naturally. Do not claim device control or real-world action execution. Avoid long lists, long numbers, and markdown.";
#endif
}

static void build_session_instructions(char *dst, size_t cap) {
  const char *base = prompt_preset_instructions();
  snprintf(dst, cap, "%s", base);
#if CONFIG_VOICE_TOOLS_ENABLE
  {
    const char *suffix =
        " You may call tools when needed: set_timer and cancel_timer (relative delays only, not "
        "wall-clock alarms); get_device_status; get_network_status; get_time; get_date; "
        "airplay_status; set_screen_brightness (0-100 percent); play_local_chime (optional tone); "
        "set_volume; get_volume. Use set_volume for louder/quieter/mute. After a tool runs, reply "
        "briefly in voice.";
    size_t n = strlen(dst);
    if (n + strlen(suffix) + 1 < cap) {
      memcpy(dst + n, suffix, strlen(suffix) + 1);
    }
  }
#endif
}

static void set_state(realtime_voice_state_t state, const char *error_text) {
  s_ctx.state = state;
  screen_ui_set_voice_state(to_screen_state(state), s_ctx.last_user,
                            s_ctx.last_assistant, error_text);
}

static void set_voice_ui_idle(void) {
  set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
}

static void mark_session_activity(uint64_t now_ms) {
  if (s_ctx.session_state == VOICE_SESSION_NONE) {
    s_ctx.session_state = VOICE_SESSION_ACTIVE;
    s_ctx.session_started_ms = now_ms;
  }
  s_ctx.session_last_active_ms = now_ms;
}

void realtime_voice_reset_session(void) {
  s_ctx.session_state = VOICE_SESSION_NONE;
  s_ctx.session_started_ms = 0;
  s_ctx.session_last_active_ms = 0;
  s_ctx.response_create_sent = false;
#if CONFIG_VOICE_TOOLS_ENABLE
  s_ctx.server_response_active = false;
  s_ctx.tool_call_inflight = false;
  s_ctx.tool_followup_pending = false;
  s_ctx.last_tool_call_id[0] = '\0';
#endif
  memset(s_ctx.session_user_summary, 0, sizeof(s_ctx.session_user_summary));
  memset(s_ctx.session_assistant_summary, 0, sizeof(s_ctx.session_assistant_summary));
  memset(s_ctx.last_user, 0, sizeof(s_ctx.last_user));
  memset(s_ctx.last_assistant, 0, sizeof(s_ctx.last_assistant));
}

static void append_summary(char *dst, size_t dst_len, const char *text) {
  if (dst == NULL || dst_len == 0 || text == NULL || text[0] == '\0') {
    return;
  }
  size_t cap = CONFIG_VOICE_CONTEXT_MAX_CHARS;
  if (cap > (dst_len - 1)) {
    cap = dst_len - 1;
  }
  size_t used = strlen(dst);
  if (used >= cap) {
    return;
  }
  size_t room = cap - used;
  size_t in_len = strlen(text);
  size_t copy = in_len < room ? in_len : room;
  strncat(dst, text, copy);
  dst[cap] = '\0';
}

static bool config_ready(void) {
  if (s_voice_config.api_key[0] == '\0') {
    return false;
  }
  return true;
}

void realtime_voice_set_config(const realtime_voice_config_t *config) {
  if (config == NULL) {
    return;
  }
  snprintf(s_voice_config.url, sizeof(s_voice_config.url), "%s", config->url);
  snprintf(s_voice_config.api_key, sizeof(s_voice_config.api_key), "%s", config->api_key);
  snprintf(s_voice_config.model, sizeof(s_voice_config.model), "%s", config->model);
}

bool realtime_voice_config_ready(void) { return config_ready(); }

static bool should_run_voice(void) {
  if (!s_ctx.enabled) {
    return false;
  }
  receiver_state_snapshot_t snapshot;
  receiver_state_get_snapshot(&snapshot);
  if (snapshot.faulted || snapshot.config_required || snapshot.recovering) {
    return false;
  }
  /* AirPlay no longer gates the voice pipeline: mic + uplink stay alive while
   * streaming; assistant replies acquire the speaker (duck) per response. */
  return snapshot.network_ready || snapshot.discoverable;
}

static uint64_t now_ms(void) {
  return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static void ws_retry_reset(void) {
  s_ctx.ws_retry_delay_ms = (uint32_t)CONFIG_VOICE_WS_RETRY_BASE_MS;
  s_ctx.ws_next_retry_ms = 0;
}

static void reset_response_state(void) {
  s_ctx.waiting_response = false;
  s_ctx.speaking = false;
  s_ctx.interrupt_requested = false;
  s_ctx.user_speech_notified = false;
  s_ctx.response_audio_closed = false;
  s_ctx.response_cancelled = false;
  s_ctx.response_create_sent = false;
  s_ctx.speaking_ended_ms = 0;
  s_ctx.playback_active = false;
  s_ctx.playout_prefilled = false;
  s_ctx.response_latency_anchor_ms     = 0;
  s_ctx.response_first_audio_logged    = false;
#if CONFIG_VOICE_TOOLS_ENABLE
  s_ctx.server_response_active = false;
  s_ctx.tool_call_inflight = false;
  s_ctx.tool_followup_pending = false;
  s_ctx.last_tool_call_id[0] = '\0';
#endif
}

static void speaker_volume_restore(void) {
  if (!s_ctx.speaker_volume_saved) {
    return;
  }
  audio_output_set_target_volume_db(s_ctx.speaker_volume_restore_db);
  s_ctx.speaker_volume_saved = false;
}

static void speaker_volume_boost(void) {
  if (s_ctx.speaker_volume_saved) {
    return;
  }
  audio_output_diag_t diag = {0};
  float restore_db = -15.0f;
  if (audio_output_get_diag(&diag) == ESP_OK) {
    restore_db = diag.target_volume_db;
  }
  s_ctx.speaker_volume_restore_db = restore_db;
  s_ctx.speaker_volume_saved = true;
  if (restore_db < VOICE_PLAYBACK_BOOST_DB) {
    audio_output_set_target_volume_db(VOICE_PLAYBACK_BOOST_DB);
    ESP_LOGI(TAG, "voice playback volume boosted: restore=%.1f dB boost=%.1f dB",
             restore_db, VOICE_PLAYBACK_BOOST_DB);
  }
}

static void speaker_release(void) {
  if (!s_ctx.speaker_owned) {
    return;
  }
  speaker_volume_restore();
  voice_play_rs_reset();
  esp_err_t err = audio_output_release_external(VOICE_OWNER_TAG, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "speaker ownership release failed: %s", esp_err_to_name(err));
  }
  s_ctx.speaker_owned = false;
  airplay_service_refresh_playback();
}

static bool speaker_acquire(bool stop_worker) {
  if (s_ctx.speaker_owned) {
    if (stop_worker) {
      esp_err_t err = audio_output_acquire_external(VOICE_OWNER_TAG, true);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaker ownership upgrade failed: %s", esp_err_to_name(err));
        return false;
      }
    }
    return true;
  }
  esp_err_t err = audio_output_acquire_external(VOICE_OWNER_TAG, stop_worker);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "speaker ownership acquire failed: %s", esp_err_to_name(err));
    return false;
  }
  s_ctx.speaker_owned = true;
  return true;
}

static int16_t voice_peak_abs_i16(const int16_t *samples, size_t count) {
  int16_t peak = 0;
  if (samples == NULL) {
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    int32_t v = samples[i];
    int32_t mag = v < 0 ? -v : v;
    if (mag > 32767) {
      mag = 32767;
    }
    if ((int16_t)mag > peak) {
      peak = (int16_t)mag;
    }
  }
  return peak;
}

static bool base64_decode(const char *input, uint8_t **output, size_t *out_len) {
  *output = NULL;
  *out_len = 0;
  size_t needed = 0;
  int rc = mbedtls_base64_decode(NULL, 0, &needed, (const unsigned char *)input,
                                 strlen(input));
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || needed == 0) {
    return false;
  }
  uint8_t *buf = (uint8_t *)voice_buf_alloc(needed);
  if (buf == NULL) {
    return false;
  }
  rc = mbedtls_base64_decode(buf, needed, &needed, (const unsigned char *)input,
                             strlen(input));
  if (rc != 0) {
    voice_buf_free(buf);
    return false;
  }
  *output = buf;
  *out_len = needed;
  return true;
}

static esp_err_t ensure_mic_persistent_open(void) {
  if (s_ctx.mic == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  const uint32_t hw_hz = voice_hw_codec_rate_hz();
  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = VOICE_HW_CHANNELS,
      .channel_mask = VOICE_HW_CHANNEL_MASK,
      .sample_rate = hw_hz,
      .mclk_multiple = voice_hw_mclk_multiple(hw_hz),
  };
  esp_err_t ret = esp_codec_dev_open(s_ctx.mic, &fs);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "persistent mic open failed: %s", esp_err_to_name(ret));
    return ret;
  }
  audio_output_notify_i2s_rx(true);
  (void)esp_codec_dev_set_in_gain(s_ctx.mic, (float)CONFIG_VOICE_MIC_IN_GAIN_DB);
  vTaskDelay(pdMS_TO_TICKS(VOICE_MIC_WARMUP_MS));
  ESP_LOGI(TAG, "persistent mic ready: rate=%lu channels=%d bits=%d",
           (unsigned long)fs.sample_rate, fs.channel, fs.bits_per_sample);
  return ESP_OK;
}

static bool spk_open(void) {
  if (s_ctx.spk == NULL) {
    return false;
  }
  if (s_ctx.playback_active) {
    return true;
  }
  if (!speaker_acquire(true)) {
    return false;
  }
  if (!voice_rs_play_ensure()) {
    ESP_LOGE(TAG, "playout resampler init failed");
    speaker_release();
    return false;
  }
  speaker_volume_boost();
  audio_output_diag_t diag = {0};
  if (audio_output_get_diag(&diag) == ESP_OK) {
    ESP_LOGI(TAG, "voice playback volume diag: target=%.1f current=%.1f hw=%d muted=%d",
             diag.target_volume_db, diag.current_volume_db, diag.volume,
             diag.muted ? 1 : 0);
  }
  {
    const uint32_t output_rate = (uint32_t)CONFIG_VOICE_OUTPUT_SAMPLE_RATE;
    const size_t drain_chunk = output_rate * 10 / 1000;
    size_t hw_mono_cap = drain_chunk;
    if (voice_rs_play_rs() != NULL) {
      hw_mono_cap = voice_rs_output_cap(drain_chunk, voice_rs_play_ratio());
    }
    size_t stereo_cap = hw_mono_cap * VOICE_HW_CHANNELS;
    if (!playout_workbufs_ensure(drain_chunk, hw_mono_cap, stereo_cap)) {
      ESP_LOGE(TAG, "playout workbuf alloc failed");
      speaker_release();
      return false;
    }
  }
  const uint32_t hw_hz = voice_hw_codec_rate_hz();
  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
      .channel = VOICE_HW_CHANNELS,
      .channel_mask = VOICE_HW_CHANNEL_MASK,
      .sample_rate = hw_hz,
      .mclk_multiple = voice_hw_mclk_multiple(hw_hz),
  };
  if (esp_codec_dev_open(s_ctx.spk, &fs) != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "spk: codec open failed");
    speaker_release();
    return false;
  }
  ESP_LOGI(TAG, "spk: codec open success, rate=%d", (int)fs.sample_rate);
  audio_output_notify_i2s_tx(true);
  s_ctx.playout_prefilled = false;
  s_ctx.playback_active = true;
  return true;
}

static void spk_close(void) {
  if (!s_ctx.playback_active) {
    return;
  }
  s_ctx.playback_active = false;
  if (s_ctx.spk != NULL) {
    esp_codec_dev_close(s_ctx.spk);
    audio_output_notify_i2s_tx(false);
  }
  voice_playout_reset();
  voice_play_rs_reset();
  speaker_release();
}

static void playout_drain_to_speaker(void) {
  if (s_ctx.spk == NULL) return;

  const uint32_t output_rate = (uint32_t)CONFIG_VOICE_OUTPUT_SAMPLE_RATE;
  const size_t drain_chunk = output_rate * 10 / 1000; /* 10ms per drain call */

    if (!s_ctx.playout_prefilled) {
      size_t avail = voice_playout_avail();
      size_t prefill = (size_t)output_rate * VOICE_PLAYOUT_PREFILL_MS / 1000;
      if (avail < prefill) return;
      s_ctx.playout_prefilled = true;
      voice_playout_set_last_write_ms(now_ms());
  }

  /* Drain ring → resample → soft-limit → write to codec */
  if (s_ctx.playout_pop_buf == NULL || s_ctx.playout_hw_buf == NULL || s_ctx.playout_stereo_buf == NULL) {
    return;
  }
  int16_t *pop_buf = s_ctx.playout_pop_buf;
  size_t popped = voice_playout_pop(pop_buf, drain_chunk);

  static uint32_t s_drain_diag_count;
  static int16_t s_gap_fade_buf[2048];

  if (popped > 0) {
    voice_playout_set_last_write_ms(now_ms());

    /* Capture reference for AEC: resample model rate → 16k, push to ref ring */
    if (voice_reference_playout_rs() != NULL && voice_reference_ring_is_ready()) {
      size_t ref_out_cap = voice_rs_output_cap(popped, voice_reference_playout_ratio());
      if (s_ctx.playout_hw_cap < ref_out_cap) {
        voice_buf_free(s_ctx.playout_hw_buf);
        s_ctx.playout_hw_buf = (int16_t *)voice_buf_alloc(ref_out_cap * sizeof(int16_t));
        s_ctx.playout_hw_cap = s_ctx.playout_hw_buf != NULL ? ref_out_cap : 0;
      }
      if (s_ctx.playout_hw_buf != NULL) {
        size_t ref_frames =
            voice_rs_process_mono(voice_reference_playout_rs(), voice_reference_playout_ratio(),
                                  pop_buf, popped, s_ctx.playout_hw_buf, s_ctx.playout_hw_cap);
        if (ref_frames > 0) {
          voice_reference_ring_push(s_ctx.playout_hw_buf, ref_frames);
        }
      }
    } else if (voice_reference_ring_is_ready()) {
      /* same rate: direct copy */
      voice_reference_ring_push(pop_buf, popped);
    }

    int16_t *hw_mono = s_ctx.playout_hw_buf;
    size_t hw_mono_cap = s_ctx.playout_hw_cap;

    int16_t input_peak = voice_peak_abs_i16(pop_buf, popped);
    size_t hw_frames = popped;
    if (voice_rs_play_rs() != NULL) {
      hw_frames = voice_rs_process_mono(voice_rs_play_rs(), voice_rs_play_ratio(), pop_buf, popped,
                                        hw_mono, hw_mono_cap);
    } else {
      if (popped <= hw_mono_cap) {
        memcpy(hw_mono, pop_buf, popped * sizeof(int16_t));
      }
      else hw_frames = hw_mono_cap;
    }
    if (hw_frames == 0) {
      return;
    }

    /* Soft-limit in place */
    for (size_t i = 0; i < hw_frames; i++) hw_mono[i] = soft_clip_i16(hw_mono[i]);

    int16_t *stereo = s_ctx.playout_stereo_buf;
    for (size_t i = 0; i < hw_frames; i++) {
      stereo[i * 2] = hw_mono[i];
      stereo[i * 2 + 1] = hw_mono[i];
    }

    /* Save last stereo frame for gap concealment (cached inside voice_playout). */
    voice_playout_save_last_stereo(stereo, hw_frames);

    int16_t output_peak = voice_peak_abs_i16(stereo, hw_frames * VOICE_HW_CHANNELS);
    s_drain_diag_count++;
    if (output_peak >= VOICE_PLAYBACK_CLIP_RISK_PEAK) {
      ESP_LOGW(TAG, "spk: clip-risk input_peak=%d output_peak=%d frames=%lu",
               input_peak, output_peak, (unsigned long)hw_frames);
    } else if ((s_drain_diag_count % VOICE_PLAYBACK_PEAK_LOG_INTERVAL) == 0U) {
      ESP_LOGI(TAG, "spk: pcm peak input=%d output=%d frames=%lu",
               input_peak, output_peak, (unsigned long)hw_frames);
    }
    int wb = (int)(hw_frames * VOICE_HW_CHANNELS * sizeof(int16_t));
    int ret = esp_codec_dev_write(s_ctx.spk, stereo, wb);
    if (ret != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "spk: write failed: %d", ret);

    /* Low-watermark early warning: ring running thin */
    {
      static uint64_t s_last_lowmark_log_ms;
      size_t avail_after = voice_playout_avail();
      size_t low_threshold = (size_t)output_rate * VOICE_PLAYOUT_LOW_MS / 1000;
      if (avail_after < low_threshold && s_ctx.speaking && !s_ctx.response_audio_closed) {
        uint64_t now2 = now_ms();
        if (now2 - s_last_lowmark_log_ms >= 2000ULL) {
          ESP_LOGW(TAG, "playout ring low: avail=%lums threshold=%lums",
                   (unsigned long)(avail_after * 1000 / output_rate),
                   (unsigned long)VOICE_PLAYOUT_LOW_MS);
          s_last_lowmark_log_ms = now2;
        }
      }
    }
  } else {
    /* Gap concealment: repeat last good frame with fade */
    uint64_t now = now_ms();
    {
      static uint64_t s_last_underrun_log_ms;
      if (now - s_last_underrun_log_ms >= 3000ULL) {
        ESP_LOGW(TAG, "playout underrun: avail=%lu speaking=%d audio_closed=%d",
                 (unsigned long)voice_playout_avail(), s_ctx.speaking ? 1 : 0,
                 s_ctx.response_audio_closed ? 1 : 0);
        s_last_underrun_log_ms = now;
      }
    }
    size_t cached_frames = 0;
    const int16_t *cached_stereo = voice_playout_last_stereo(&cached_frames);
    uint64_t last_write = voice_playout_last_write_ms();
    if (cached_stereo != NULL && cached_frames > 0 && last_write > 0 &&
        (now - last_write) < VOICE_PLAYOUT_GAP_TIMEOUT_MS) {
      size_t frames = cached_frames;
      if (frames > 512) frames = 512;
      size_t gap_ms = (size_t)(now - last_write);
      float gain = 1.0f;
      if (gap_ms > 0) {
        float t = (float)gap_ms / (float)VOICE_PLAYOUT_GAP_CONCEAL_MS;
        gain = t < 1.0f ? 1.0f - t * 0.5f : 0.5f;
      }
      size_t sb = frames * VOICE_HW_CHANNELS;
      if (sb <= sizeof(s_gap_fade_buf) / sizeof(s_gap_fade_buf[0])) {
        for (size_t i = 0; i < sb; i++)
          s_gap_fade_buf[i] = (int16_t)((float)cached_stereo[i] * gain);
        esp_codec_dev_write(s_ctx.spk, s_gap_fade_buf, (int)(sb * sizeof(int16_t)));
      }
    } else if (last_write > 0 &&
               (now - last_write) >= VOICE_PLAYOUT_GAP_TIMEOUT_MS) {
      spk_close();
    }
  }
  if (s_ctx.response_audio_closed && s_ctx.playback_active && voice_playout_avail() == 0) {
    spk_close();
    s_ctx.speaking = false;
    set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
  }
}

static bool ws_send_json(cJSON *root) {
  if (s_ctx.ws == NULL || root == NULL || !s_ctx.connected) {
    return false;
  }
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  const char *type_str = (cJSON_IsString(type) && type->valuestring != NULL)
                             ? type->valuestring
                             : "unknown";
  if (cJSON_GetObjectItemCaseSensitive(root, "event_id") == NULL) {
    char eid[48];
    uint64_t t = (uint64_t)(esp_timer_get_time() / 1000ULL);
    snprintf(eid, sizeof(eid), "event_%llu", (unsigned long long)t);
    cJSON_AddStringToObject(root, "event_id", eid);
  }
  char *text = cJSON_PrintUnformatted(root);
  if (text == NULL) {
    return false;
  }
  int rc = esp_websocket_client_send_text(s_ctx.ws, text, strlen(text),
                                        pdMS_TO_TICKS(CONFIG_VOICE_WS_SEND_TIMEOUT_MS));
  free(text);
  if (rc >= 0) {
    record_ws_send_type(type_str);
  } else {
    log_ws_disconnect_diag(type_str);
  }
  return rc >= 0;
}

#if CONFIG_VOICE_TOOLS_ENABLE
static void voice_append_session_tools(cJSON *session) {
  cJSON *tools = cJSON_AddArrayToObject(session, "tools");
  if (tools == NULL) {
    return;
  }
  cJSON *t1 = cJSON_CreateObject();
  cJSON_AddStringToObject(t1, "type", "function");
  cJSON *f1 = cJSON_AddObjectToObject(t1, "function");
  cJSON_AddStringToObject(
      f1, "name", "set_timer");
  cJSON_AddStringToObject(
      f1, "description",
      "Start a one-shot timer after the current moment. Use duration_sec seconds from now; "
      "not wall-clock date/time.");
  cJSON *p1 = cJSON_AddObjectToObject(f1, "parameters");
  cJSON_AddStringToObject(p1, "type", "object");
  cJSON *props1 = cJSON_AddObjectToObject(p1, "properties");
  cJSON *ds = cJSON_AddObjectToObject(props1, "duration_sec");
  cJSON_AddStringToObject(ds, "type", "number");
  cJSON_AddStringToObject(ds, "description", "Delay in seconds before the timer fires (1-86400).");
  cJSON *lb = cJSON_AddObjectToObject(props1, "label");
  cJSON_AddStringToObject(lb, "type", "string");
  cJSON_AddStringToObject(lb, "description", "Optional short label for logs/UI when the timer fires.");
  cJSON *req1 = cJSON_AddArrayToObject(p1, "required");
  cJSON_AddItemToArray(req1, cJSON_CreateString("duration_sec"));
  cJSON_AddItemToArray(tools, t1);

  cJSON *t2 = cJSON_CreateObject();
  cJSON_AddStringToObject(t2, "type", "function");
  cJSON *f2 = cJSON_AddObjectToObject(t2, "function");
  cJSON_AddStringToObject(f2, "name", "cancel_timer");
  cJSON_AddStringToObject(f2, "description", "Cancel a pending timer created by set_timer.");
  cJSON *p2 = cJSON_AddObjectToObject(f2, "parameters");
  cJSON_AddStringToObject(p2, "type", "object");
  cJSON *props2 = cJSON_AddObjectToObject(p2, "properties");
  cJSON *tid = cJSON_AddObjectToObject(props2, "timer_id");
  cJSON_AddStringToObject(tid, "type", "number");
  cJSON_AddStringToObject(tid, "description", "timer_id returned by set_timer.");
  cJSON *req2 = cJSON_AddArrayToObject(p2, "required");
  cJSON_AddItemToArray(req2, cJSON_CreateString("timer_id"));
  cJSON_AddItemToArray(tools, t2);

  cJSON *t3 = cJSON_CreateObject();
  cJSON_AddStringToObject(t3, "type", "function");
  cJSON *f3 = cJSON_AddObjectToObject(t3, "function");
  cJSON_AddStringToObject(f3, "name", "get_device_status");
  cJSON_AddStringToObject(f3, "description", "Summarize WiFi, receiver state, and active timers.");
  cJSON *p3 = cJSON_AddObjectToObject(f3, "parameters");
  cJSON_AddStringToObject(p3, "type", "object");
  (void)cJSON_AddObjectToObject(p3, "properties");
  cJSON_AddItemToArray(tools, t3);

  cJSON *t4 = cJSON_CreateObject();
  cJSON_AddStringToObject(t4, "type", "function");
  cJSON *f4 = cJSON_AddObjectToObject(t4, "function");
  cJSON_AddStringToObject(f4, "name", "set_volume");
  cJSON_AddStringToObject(
      f4, "description",
      "Adjust the local speaker volume. Use percent for absolute 0-100 volume, "
      "delta_percent for relative changes, and muted for mute/unmute.");
  cJSON *p4 = cJSON_AddObjectToObject(f4, "parameters");
  cJSON_AddStringToObject(p4, "type", "object");
  cJSON *props4 = cJSON_AddObjectToObject(p4, "properties");
  cJSON *vp = cJSON_AddObjectToObject(props4, "percent");
  cJSON_AddStringToObject(vp, "type", "number");
  cJSON_AddStringToObject(vp, "description", "Absolute speaker volume percent, 0-100.");
  cJSON *vd = cJSON_AddObjectToObject(props4, "delta_percent");
  cJSON_AddStringToObject(vd, "type", "number");
  cJSON_AddStringToObject(vd, "description", "Relative volume change in percent, e.g. 10 or -10.");
  cJSON *vm = cJSON_AddObjectToObject(props4, "muted");
  cJSON_AddStringToObject(vm, "type", "boolean");
  cJSON_AddStringToObject(vm, "description", "Set true to mute, false to unmute.");
  cJSON_AddItemToArray(tools, t4);

  cJSON *t5 = cJSON_CreateObject();
  cJSON_AddStringToObject(t5, "type", "function");
  cJSON *f5 = cJSON_AddObjectToObject(t5, "function");
  cJSON_AddStringToObject(f5, "name", "get_volume");
  cJSON_AddStringToObject(f5, "description", "Read the current local speaker volume and mute state.");
  cJSON *p5 = cJSON_AddObjectToObject(f5, "parameters");
  cJSON_AddStringToObject(p5, "type", "object");
  (void)cJSON_AddObjectToObject(p5, "properties");
  cJSON_AddItemToArray(tools, t5);

  cJSON *t6 = cJSON_CreateObject();
  cJSON_AddStringToObject(t6, "type", "function");
  cJSON *f6 = cJSON_AddObjectToObject(t6, "function");
  cJSON_AddStringToObject(f6, "name", "get_time");
  cJSON_AddStringToObject(f6, "description", "Current local time (device RTC/NTP) as HH:MM:SS.");
  cJSON *p6 = cJSON_AddObjectToObject(f6, "parameters");
  cJSON_AddStringToObject(p6, "type", "object");
  (void)cJSON_AddObjectToObject(p6, "properties");
  cJSON_AddItemToArray(tools, t6);

  cJSON *t7 = cJSON_CreateObject();
  cJSON_AddStringToObject(t7, "type", "function");
  cJSON *f7 = cJSON_AddObjectToObject(t7, "function");
  cJSON_AddStringToObject(f7, "name", "get_date");
  cJSON_AddStringToObject(f7, "description", "Current local calendar date and weekday.");
  cJSON *p7 = cJSON_AddObjectToObject(f7, "parameters");
  cJSON_AddStringToObject(p7, "type", "object");
  (void)cJSON_AddObjectToObject(p7, "properties");
  cJSON_AddItemToArray(tools, t7);

  cJSON *t8 = cJSON_CreateObject();
  cJSON_AddStringToObject(t8, "type", "function");
  cJSON *f8 = cJSON_AddObjectToObject(t8, "function");
  cJSON_AddStringToObject(f8, "name", "get_network_status");
  cJSON_AddStringToObject(f8, "description", "WiFi/IP and receiver discovery/streaming flags.");
  cJSON *p8 = cJSON_AddObjectToObject(f8, "parameters");
  cJSON_AddStringToObject(p8, "type", "object");
  (void)cJSON_AddObjectToObject(p8, "properties");
  cJSON_AddItemToArray(tools, t8);

  cJSON *t9 = cJSON_CreateObject();
  cJSON_AddStringToObject(t9, "type", "function");
  cJSON *f9 = cJSON_AddObjectToObject(t9, "function");
  cJSON_AddStringToObject(f9, "name", "set_screen_brightness");
  cJSON_AddStringToObject(f9, "description", "Set LCD backlight brightness percent 0-100.");
  cJSON *p9 = cJSON_AddObjectToObject(f9, "parameters");
  cJSON_AddStringToObject(p9, "type", "object");
  cJSON *props9 = cJSON_AddObjectToObject(p9, "properties");
  cJSON *b9 = cJSON_AddObjectToObject(props9, "brightness_percent");
  cJSON_AddStringToObject(b9, "type", "number");
  cJSON_AddStringToObject(b9, "description", "0=dimmest/off-like, 100=brightest.");
  cJSON *req9 = cJSON_AddArrayToObject(p9, "required");
  cJSON_AddItemToArray(req9, cJSON_CreateString("brightness_percent"));
  cJSON_AddItemToArray(tools, t9);

  cJSON *t10 = cJSON_CreateObject();
  cJSON_AddStringToObject(t10, "type", "function");
  cJSON *f10 = cJSON_AddObjectToObject(t10, "function");
  cJSON_AddStringToObject(f10, "name", "play_local_chime");
  cJSON_AddStringToObject(
      f10, "description",
      "Play a short local test tone through the speaker when not in exclusive voice playback.");
  cJSON *p10 = cJSON_AddObjectToObject(f10, "parameters");
  cJSON_AddStringToObject(p10, "type", "object");
  cJSON *props10 = cJSON_AddObjectToObject(p10, "properties");
  cJSON *fq = cJSON_AddObjectToObject(props10, "frequency_hz");
  cJSON_AddStringToObject(fq, "type", "number");
  cJSON_AddStringToObject(fq, "description", "Tone frequency in Hz (default 880).");
  cJSON *dq = cJSON_AddObjectToObject(props10, "duration_ms");
  cJSON_AddStringToObject(dq, "type", "number");
  cJSON_AddStringToObject(dq, "description", "Duration in ms (default 120, max 3000).");
  cJSON *aq = cJSON_AddObjectToObject(props10, "amplitude_pct");
  cJSON_AddStringToObject(aq, "type", "number");
  cJSON_AddStringToObject(aq, "description", "Amplitude percent 1-100 (default 25).");
  cJSON_AddItemToArray(tools, t10);

  cJSON *t11 = cJSON_CreateObject();
  cJSON_AddStringToObject(t11, "type", "function");
  cJSON *f11 = cJSON_AddObjectToObject(t11, "function");
  cJSON_AddStringToObject(f11, "name", "airplay_status");
  cJSON_AddStringToObject(f11, "description", "Whether an AirPlay streaming session is active.");
  cJSON *p11 = cJSON_AddObjectToObject(f11, "parameters");
  cJSON_AddStringToObject(p11, "type", "object");
  (void)cJSON_AddObjectToObject(p11, "properties");
  cJSON_AddItemToArray(tools, t11);
}

static bool send_conversation_function_output(const char *call_id, const char *output) {
  if (call_id == NULL || output == NULL) {
    return false;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "conversation.item.create");
  cJSON *item = cJSON_AddObjectToObject(root, "item");
  cJSON_AddStringToObject(item, "type", "function_call_output");
  cJSON_AddStringToObject(item, "call_id", call_id);
  cJSON_AddStringToObject(item, "output", output);
  bool ok = ws_send_json(root);
  cJSON_Delete(root);
  return ok;
}

static bool send_response_create_for_tool(void) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "response.create");
  bool ok = ws_send_json(root);
  ESP_LOGI(TAG, "response.create (tool follow-up) %s", ok ? "ok" : "failed");
  if (ok) {
    s_ctx.response_audio_closed = false;
    s_ctx.response_cancelled = false;
    s_ctx.waiting_response = true;
  }
  cJSON_Delete(root);
  return ok;
}

static void handle_function_call_arguments_done(cJSON *root) {
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
  const cJSON *call_id = cJSON_GetObjectItemCaseSensitive(root, "call_id");
  const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(root, "arguments");
  if (!cJSON_IsString(name) || name->valuestring == NULL || name->valuestring[0] == '\0' ||
      !cJSON_IsString(call_id) || call_id->valuestring == NULL || call_id->valuestring[0] == '\0' ||
      !cJSON_IsString(arguments) || arguments->valuestring == NULL) {
    ESP_LOGW(TAG, "function_call_arguments.done missing name/call_id/arguments");
    return;
  }
  const char *cid = call_id->valuestring;
  if (s_ctx.last_tool_call_id[0] != '\0' && strcmp(s_ctx.last_tool_call_id, cid) == 0) {
    ESP_LOGW(TAG, "duplicate tool call_id=%s ignored", cid);
    return;
  }
  s_ctx.tool_call_inflight = true;
  char output[896];
  (void)voice_tools_dispatch(name->valuestring, arguments->valuestring, output, sizeof(output));
  if (output[0] == '\0') {
    snprintf(output, sizeof(output), "%s", "{\"ok\":false,\"error\":\"empty tool output\"}");
  }
  if (!send_conversation_function_output(cid, output)) {
    ESP_LOGE(TAG, "conversation.item.create (function_call_output) failed");
    s_ctx.tool_call_inflight = false;
    return;
  }
  s_ctx.tool_followup_pending = true;
  if (!send_response_create_for_tool()) {
    ESP_LOGE(TAG, "response.create after tool output failed");
    s_ctx.tool_followup_pending = false;
    s_ctx.tool_call_inflight = false;
    return;
  }
  snprintf(s_ctx.last_tool_call_id, sizeof(s_ctx.last_tool_call_id), "%s", cid);
  s_ctx.tool_call_inflight = false;
}
#endif /* CONFIG_VOICE_TOOLS_ENABLE */

static bool send_session_update(void) {
  char instr_buf[2048];
  build_session_instructions(instr_buf, sizeof(instr_buf));
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "session.update");
  cJSON *session = cJSON_AddObjectToObject(root, "session");
  cJSON_AddStringToObject(session, "instructions", instr_buf);
  cJSON *modalities = cJSON_AddArrayToObject(session, "modalities");
  cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
  cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
  /* DashScope realtime: 16-bit mono PCM at session sample rate (see Alibaba Model Studio docs). */
  cJSON_AddStringToObject(session, "input_audio_format", "pcm");
  cJSON_AddStringToObject(session, "output_audio_format", "pcm");
#ifdef CONFIG_VOICE_TTS_VOICE
  if (strlen(CONFIG_VOICE_TTS_VOICE) > 0) {
    cJSON_AddStringToObject(session, "voice", CONFIG_VOICE_TTS_VOICE);
  }
#endif
#if CONFIG_VOICE_ENABLE_INPUT_TRANSCRIPTION
  {
    cJSON *iat = cJSON_AddObjectToObject(session, "input_audio_transcription");
    if (iat != NULL) {
      cJSON_AddStringToObject(iat, "model", CONFIG_VOICE_INPUT_TRANSCRIPTION_MODEL);
    }
  }
#endif
  cJSON_AddItemToObject(session, "turn_detection", cJSON_CreateNull());
  cJSON_AddBoolToObject(session, "smooth_output", true);
#if CONFIG_VOICE_TOOLS_ENABLE
  voice_append_session_tools(session);
#endif

  bool ok = ws_send_json(root);
  if (!ok) {
    ESP_LOGW(TAG, "session.update send failed");
  } else {
    ESP_LOGI(TAG, "session.update sent");
  }
  cJSON_Delete(root);
  return ok;
}

static void handle_realtime_message(const char *msg) {
  cJSON *root = cJSON_Parse(msg);
  if (root == NULL) {
    ESP_LOGW(TAG, "ws json parse failed len=%lu", (unsigned long)(msg ? strlen(msg) : 0));
    return;
  }
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsString(type) || type->valuestring == NULL) {
    cJSON_Delete(root);
    return;
  }

  const char *ev = type->valuestring;

#if CONFIG_VOICE_DEBUG_EVENT_LOG
  ESP_LOGI(TAG, "ws event: %s", ev);
#endif

  if (strcmp(ev, "session.created") == 0 ||
      strcmp(ev, "session.updated") == 0) {
    ESP_LOGI(TAG, "session ready (%s)", ev);
    s_ctx.session_ready = true;
    mark_session_activity(now_ms());
    set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
    screen_ui_set_voice_network_busy(false);
  }
  else if (strcmp(ev, "response.created") == 0) {
    s_ctx.waiting_response = true;
    s_ctx.response_latency_anchor_ms  = now_ms();
    s_ctx.response_first_audio_logged = false;
#if CONFIG_VOICE_TOOLS_ENABLE
    s_ctx.server_response_active = true;
    if (s_ctx.tool_followup_pending) {
      s_ctx.tool_followup_pending = false;
    }
#endif
    s_ctx.response_audio_closed = false;
    s_ctx.response_create_sent = true;
  }
#if CONFIG_VOICE_TOOLS_ENABLE
  else if (strcmp(ev, "response.function_call_arguments.delta") == 0) {
#if CONFIG_VOICE_DEBUG_EVENT_LOG
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    if (cJSON_IsString(delta) && delta->valuestring != NULL) {
      ESP_LOGD(TAG, "function_call_arguments.delta: %s", delta->valuestring);
    }
#endif
  } else if (strcmp(ev, "response.function_call_arguments.done") == 0) {
    handle_function_call_arguments_done(root);
  } else if (strcmp(ev, "input_audio_buffer.committed") == 0) {
    s_ctx.last_tool_call_id[0] = '\0';
  }
#endif
  else if (strcmp(ev, "response.audio.delta") == 0) {
    if (s_ctx.response_cancelled || (s_ctx.response_audio_closed && !s_ctx.speaking)) {
      cJSON_Delete(root);
      return;
    }
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    if (cJSON_IsString(delta) && delta->valuestring != NULL) {
      uint8_t *pcm = NULL;
      size_t pcm_len = 0;
      if (base64_decode(delta->valuestring, &pcm, &pcm_len)) {
        if (!s_ctx.playback_active && !spk_open()) {
          voice_buf_free(pcm);
          cJSON_Delete(root);
          return;
        }
        s_ctx.speaking = true;
        set_state(REALTIME_VOICE_STATE_SPEAKING, NULL);
        size_t pushed = voice_playout_push((const int16_t *)pcm, pcm_len / sizeof(int16_t));
        if (pushed > 0) {
          voice_playout_set_last_write_ms(now_ms());
          if (!s_ctx.response_first_audio_logged &&
              s_ctx.response_latency_anchor_ms > 0) {
            uint64_t ttfb = now_ms() - s_ctx.response_latency_anchor_ms;
            ESP_LOGI(TAG, "playout ttfb_ms=%llu (first response.audio.delta)",
                     (unsigned long long)ttfb);
            s_ctx.response_first_audio_logged = true;
          }
        }
        voice_buf_free(pcm);
      }
    }
  } else if (strcmp(ev, "response.audio.done") == 0) {
    s_ctx.speaking_ended_ms = now_ms();
    s_ctx.response_audio_closed = true;
  } else if (strcmp(ev, "response.audio_transcript.delta") == 0 ||
             strcmp(ev, "response.audio_transcript.done") == 0 ||
             strcmp(ev, "response.output_text.delta") == 0 || strcmp(ev, "response.text.delta") == 0) {
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    if (cJSON_IsString(delta) && delta->valuestring != NULL) {
      strncat(s_ctx.last_assistant, delta->valuestring,
              sizeof(s_ctx.last_assistant) - strlen(s_ctx.last_assistant) - 1);
      append_summary(s_ctx.session_assistant_summary, sizeof(s_ctx.session_assistant_summary),
                     delta->valuestring);
      mark_session_activity((uint64_t)(esp_timer_get_time() / 1000));
      set_state(s_ctx.speaking ? REALTIME_VOICE_STATE_SPEAKING
                               : REALTIME_VOICE_STATE_THINKING,
                NULL);
    }
  } else if (strcmp(ev, "conversation.item.input_audio_transcription.completed") == 0) {
    const cJSON *transcript = cJSON_GetObjectItemCaseSensitive(root, "transcript");
    if (!cJSON_IsString(transcript) || transcript->valuestring == NULL ||
        transcript->valuestring[0] == '\0') {
      transcript = cJSON_GetObjectItemCaseSensitive(root, "text");
    }
    if (cJSON_IsString(transcript) && transcript->valuestring != NULL &&
        transcript->valuestring[0] != '\0') {
      if (strcmp(s_ctx.last_user, "[voice turn]") == 0) {
        memset(s_ctx.session_user_summary, 0, sizeof(s_ctx.session_user_summary));
      }
      snprintf(s_ctx.last_user, sizeof(s_ctx.last_user), "%s", transcript->valuestring);
      append_summary(s_ctx.session_user_summary, sizeof(s_ctx.session_user_summary),
                     transcript->valuestring);
      mark_session_activity((uint64_t)(esp_timer_get_time() / 1000));
      ESP_LOGI(TAG, "user transcript: %s", transcript->valuestring);
    } else {
      ESP_LOGI(TAG, "user transcript completed without text");
    }
  } else if (strcmp(ev, "conversation.item.input_audio_transcription.failed") == 0) {
    ESP_LOGW(TAG, "user transcript failed");
  } else if (strcmp(ev, "response.done") == 0) {
#if CONFIG_VOICE_TOOLS_ENABLE
    s_ctx.server_response_active = false;
#endif
    s_ctx.waiting_response = false;
    s_ctx.response_audio_closed = true;
    s_ctx.response_cancelled = false;
    if (s_ctx.speaking) {
      s_ctx.speaking_ended_ms = now_ms();
    }
    if (!s_ctx.speaking) {
      set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
    }
  } else if (strcmp(ev, "error") == 0) {
    const char *show = "REALTIME API ERROR";
    s_err_detail[0] = '\0';
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(err)) {
      const cJSON *err_msg = cJSON_GetObjectItemCaseSensitive(err, "message");
      if (cJSON_IsString(err_msg) && err_msg->valuestring != NULL) {
        snprintf(s_err_detail, sizeof(s_err_detail), "%s", err_msg->valuestring);
        show = s_err_detail;
      } else {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(err, "code");
        if (cJSON_IsString(code) && code->valuestring != NULL) {
          snprintf(s_err_detail, sizeof(s_err_detail), "%s", code->valuestring);
          show = s_err_detail;
        }
      }
    }
    /* Cancel-vs-completion race: server says no active response — non-fatal */
    if (strstr(show, "none active response") != NULL ||
        strstr(show, "no active response") != NULL) {
      ESP_LOGW(TAG, "cancel race: response already completed; clearing state");
      s_ctx.speaking = false;
      s_ctx.waiting_response = false;
      s_ctx.response_audio_closed = true;
      s_ctx.response_cancelled = true;
      s_ctx.speaking_ended_ms = now_ms();
      if (s_ctx.playback_active) {
        spk_close();
      } else {
        speaker_release();
      }
      set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
      cJSON_Delete(root);
      return;
    }
#if CONFIG_VOICE_TOOLS_ENABLE
    s_ctx.tool_call_inflight = false;
    s_ctx.tool_followup_pending = false;
#endif
    ESP_LOGE(TAG, "realtime error: %s", show);
    set_state(REALTIME_VOICE_STATE_ERROR, show);
  }

  cJSON_Delete(root);
}

static __attribute__((unused)) void ws_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                             void *event_data) {
  (void)arg;
  (void)event_base;
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  if (data == NULL) {
    return;
  }
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "websocket connected");
    voice_log_heap_tls("ws_transport_up");
    s_ctx.connected = true;
    s_ctx.session_ready = false;
    s_ctx.ws_disconnected = false;
    s_ctx.append_ok_count = 0;
    s_ctx.last_append_ms = 0;
    s_ctx.last_ws_recv_ms = now_ms();
    ws_retry_reset();
    set_state(REALTIME_VOICE_STATE_CONNECTING, NULL);
    (void)send_session_update();
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
    ESP_LOGW(TAG, "websocket disconnected");
    log_ws_disconnect_diag("event_disconnected");
    screen_ui_set_voice_network_busy(false);
    s_ctx.connected = false;
    s_ctx.session_ready = false;
    s_ctx.waiting_response = false;
    s_ctx.ws_disconnected = true;
    return;
  }
#if WEBSOCKET_EVENT_ERROR
#endif
  if (event_id != WEBSOCKET_EVENT_DATA || data->data_ptr == NULL || data->data_len <= 0) {
    return;
  }

  s_ctx.last_ws_recv_ms = now_ms();

  if (data->payload_len <= 0) {
    return;
  }
  if (s_ctx.ws_accum == NULL) {
    s_ctx.ws_accum_cap = WS_ACCUM_SIZE;
    s_ctx.ws_accum = (char *)voice_buf_alloc(s_ctx.ws_accum_cap);
    if (s_ctx.ws_accum == NULL) {
      ESP_LOGW(TAG, "ws accum alloc failed cap=%lu", (unsigned long)s_ctx.ws_accum_cap);
      return;
    }
  }
  if (data->payload_offset == 0) {
    s_ctx.ws_accum_len = 0;
    s_ctx.ws_accum[0] = '\0';
  }
  size_t room = s_ctx.ws_accum_cap - s_ctx.ws_accum_len - 1;
  size_t copy = (size_t)data->data_len;
  if (copy > room) {
    ESP_LOGW(TAG, "ws message truncated payload=%d cap=%lu", data->payload_len,
             (unsigned long)s_ctx.ws_accum_cap);
    copy = room;
  }
  if (copy > 0) {
    memcpy(s_ctx.ws_accum + s_ctx.ws_accum_len, data->data_ptr, copy);
    s_ctx.ws_accum_len += copy;
    s_ctx.ws_accum[s_ctx.ws_accum_len] = '\0';
  }
  if ((data->payload_offset + data->data_len) >= data->payload_len) {
    handle_realtime_message(s_ctx.ws_accum);
    s_ctx.ws_accum_len = 0;
    s_ctx.ws_accum[0] = '\0';
  }
}

static void ws_disconnect(void) {
  voice_playout_reset();
  if (s_ctx.playback_active) {
    spk_close();
  } else {
    speaker_release();
  }
  screen_ui_set_voice_network_busy(false);
  if (s_ctx.ws == NULL) {
    s_ctx.connected = false;
    s_ctx.session_ready = false;
    reset_response_state();
    return;
  }
  esp_websocket_client_stop(s_ctx.ws);
  esp_websocket_client_destroy(s_ctx.ws);
  s_ctx.ws = NULL;
  s_ctx.connected = false;
  s_ctx.session_ready = false;
  s_ctx.ws_disconnected = false;
  reset_response_state();
}

static __attribute__((unused)) void send_response_create(void) {
  if (s_ctx.response_create_sent) {
    ESP_LOGI(TAG, "response.create skipped: already sent for this turn");
    return;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "response.create");
  bool ok = ws_send_json(root);
  ESP_LOGI(TAG, "response.create send %s", ok ? "ok" : "failed");
  if (ok) {
    s_ctx.response_audio_closed = false;
    s_ctx.response_create_sent = true;
  }
  cJSON_Delete(root);
}

void realtime_voice_notify_user_speech_start(void) {
#if CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE
  if (!session_arm_get()) {
    return;
  }
#endif
  s_ctx.user_speech_notified = true;
}

void realtime_voice_interrupt_response(void) { s_ctx.interrupt_requested = true; }

static void voice_loop_drain_and_monitor(uint64_t loop_now_ms) {
  int drain_iters = 0;
  while (drain_iters < 5 && s_ctx.playback_active) {
    size_t avail_before = voice_playout_avail();
    playout_drain_to_speaker();
    drain_iters++;
    if (voice_playout_avail() == 0 || voice_playout_avail() == avail_before) break;
  }

  {
    static uint64_t s_last_stack_log_ms;
    uint64_t now_st = now_ms();
    if (now_st - s_last_stack_log_ms >= 30000) {
      s_last_stack_log_ms = now_st;
      ESP_LOGI(TAG, "voice_task stack: watermark=%lu",
               (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    }
  }

  static uint64_t s_last_append_health_log_ms;
  uint64_t append_age_start_ms =
      s_ctx.last_append_ms > 0 ? s_ctx.last_append_ms : s_ctx.session_last_active_ms;
  if (append_age_start_ms > 0 &&
      (s_ctx.append_ok_count > 0 || s_ctx.waiting_response) &&
      (loop_now_ms - append_age_start_ms) >=
          (uint64_t)CONFIG_VOICE_APPEND_HEALTH_TIMEOUT_MS &&
      (loop_now_ms - s_last_append_health_log_ms) >=
          (uint64_t)CONFIG_VOICE_APPEND_HEALTH_TIMEOUT_MS) {
    ESP_LOGW(TAG, "uplink append idle: last_ok=%llums count=%lu",
             (unsigned long long)(loop_now_ms - append_age_start_ms),
             (unsigned long)s_ctx.append_ok_count);
    s_last_append_health_log_ms = loop_now_ms;
  }

  /* WS receive watchdog: warn if no data received for > 2x ping interval */
  if (s_ctx.connected && s_ctx.last_ws_recv_ms > 0) {
    uint64_t recv_age_ms = loop_now_ms - s_ctx.last_ws_recv_ms;
    uint64_t watchdog_ms = (uint64_t)CONFIG_VOICE_WS_PING_INTERVAL_SEC * 2000ULL;
    static uint64_t s_last_ws_watchdog_log_ms;
    if (recv_age_ms >= watchdog_ms &&
        (loop_now_ms - s_last_ws_watchdog_log_ms) >= watchdog_ms) {
      ESP_LOGW(TAG, "ws receive watchdog: no data for %llums (ping_interval=%lus)",
               (unsigned long long)recv_age_ms,
               (unsigned long)CONFIG_VOICE_WS_PING_INTERVAL_SEC);
      s_last_ws_watchdog_log_ms = loop_now_ms;
    }
  }

  {
    static uint64_t s_last_fe_health_log_ms;
    voice_frontend_health_t feh = {0};
    if (voice_frontend_get_health(&feh) && feh.last_feed_ok_ms > 0) {
      uint64_t feed_age_ms = 0;
      if (loop_now_ms >= feh.last_feed_ok_ms) {
        feed_age_ms = loop_now_ms - feh.last_feed_ok_ms;
      }
      uint64_t feed_warn_threshold_ms = 500ULL;
      if (feed_age_ms > feed_warn_threshold_ms &&
          (loop_now_ms - s_last_fe_health_log_ms) >= 1000ULL) {
        ESP_LOGW(TAG,
                 "fe health warn: last_feed_ok_age=%llums ws_connected=%d ws_ready=%d "
                 "playback_active=%d speaking=%d waiting=%d feed_ok=%lu fetch_ok=%lu "
                 "read_ok=%lu read_fail=%lu pending=%lu fetch_timeout=%lu fetch_yield=%lu "
                 "yield_p=%lu yield_np=%lu loop_hz=%lu wake=%lu/%lu",
                 (unsigned long long)feed_age_ms, s_ctx.connected ? 1 : 0,
                 s_ctx.session_ready ? 1 : 0, s_ctx.playback_active ? 1 : 0,
                 s_ctx.speaking ? 1 : 0, s_ctx.waiting_response ? 1 : 0,
                 (unsigned long)feh.feed_ok, (unsigned long)feh.fetch_ok,
                 (unsigned long)feh.mic_read_ok, (unsigned long)feh.mic_read_fail,
                 (unsigned long)feh.feed_pending_frames,
                 (unsigned long)feh.fetch_timeout_count,
                 (unsigned long)feh.fetch_yield_count,
                 (unsigned long)feh.yield_progress_count,
                 (unsigned long)feh.yield_no_progress_count,
                 (unsigned long)feh.last_loop_hz,
                 (unsigned long)feh.wake_detect_count,
                 (unsigned long)feh.wake_forward_count);
        s_last_fe_health_log_ms = loop_now_ms;
      }
    }
  }
}

static void voice_task(void *arg) {
  (void)arg;
  int16_t *fe_pcm = (int16_t *)voice_buf_alloc(VOICE_FE_MAX_PCM_FRAMES * sizeof(int16_t));
  if (fe_pcm == NULL) {
    set_state(REALTIME_VOICE_STATE_ERROR, "VOICE OOM");
    s_ctx.running = false;
    s_ctx.task = NULL;
    vTaskDelete(NULL);
    return;
  }
  const size_t record_cap_frames =
      ((size_t)VOICE_ONESHOT_SAMPLE_RATE * (size_t)VOICE_ONESHOT_RECORD_MAX_MS) / 1000;
  int16_t *record_pcm = (int16_t *)voice_buf_alloc(record_cap_frames * sizeof(int16_t));
  if (record_pcm == NULL) {
    set_state(REALTIME_VOICE_STATE_ERROR, "VOICE REC OOM");
    voice_buf_free(fe_pcm);
    s_ctx.running = false;
    s_ctx.task = NULL;
    vTaskDelete(NULL);
    return;
  }
  voice_frontend_config_t fe_cfg = {.mic = s_ctx.mic};
  bool frontend_started = false;
  voice_gate_state_t gate_state = VOICE_GATE_OPEN;
  bool gate_paused_logged = false;
  bool standby_skip_logged = false;
  uint64_t speech_start_ms = 0;
  uint64_t last_voice_ms = 0;
  bool recording = false;
  size_t record_frames = 0;
  uint32_t vad_consecutive_hits = 0;
  uint32_t voiced_frames_in_turn = 0;
  realtime_voice_reset_session();
  ws_retry_reset();
  voice_playout_init((uint32_t)CONFIG_VOICE_OUTPUT_SAMPLE_RATE);
  set_voice_ui_idle();

  while (s_ctx.running) {
    uint64_t loop_now_ms = now_ms();
    if (!config_ready()) {
      set_state(REALTIME_VOICE_STATE_ERROR, "VOICE CFG MISSING");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!should_run_voice()) {
      if (!standby_skip_logged) {
        ESP_LOGI(TAG, "voice standby: waiting for network/gates");
        standby_skip_logged = true;
      }
      if (frontend_started) {
        voice_frontend_stop();
        frontend_started = false;
      }
      session_arm_set(false);
      ws_disconnect();
      realtime_voice_reset_session();
      s_ctx.mode = DEVICE_MODE_AIRPLAY;
      set_voice_ui_idle();
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    standby_skip_logged = false;
    s_ctx.mode = DEVICE_MODE_VOICE;

    bool output_active = audio_output_is_active();
    voice_gate_state_t want_gate =
        output_active ? VOICE_GATE_BLOCKED_BY_AIRPLAY : VOICE_GATE_OPEN;
    if (want_gate != gate_state) {
      if (want_gate == VOICE_GATE_BLOCKED_BY_AIRPLAY) {
        ESP_LOGI(TAG, "voice gate -> blocked_by_airplay (output_active=1)");
      } else {
        ESP_LOGI(TAG, "voice gate -> open (output_active=0)");
      }
      gate_state = want_gate;
      gate_paused_logged = false;
    }
    if (gate_state == VOICE_GATE_BLOCKED_BY_AIRPLAY) {
      if (frontend_started) {
        voice_frontend_stop();
        frontend_started = false;
      }
      if (!gate_paused_logged) {
        ESP_LOGI(TAG, "voice frontend paused by gate");
        gate_paused_logged = true;
      }
      if (s_ctx.ws != NULL) {
        ws_disconnect();
      }
      session_arm_set(false);
      realtime_voice_reset_session();
      if (!s_ctx.waiting_response && !s_ctx.speaking) {
        set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (!frontend_started) {
      esp_err_t fe_err = voice_frontend_start(&fe_cfg);
      if (fe_err != ESP_OK) {
        ESP_LOGE(TAG, "voice_frontend start failed in task: %s", esp_err_to_name(fe_err));
        set_state(REALTIME_VOICE_STATE_ERROR, "VOICE FRONTEND START FAILED");
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      frontend_started = true;
      gate_paused_logged = false;
    }

    voice_loop_drain_and_monitor(loop_now_ms);

    voice_fe_event_t fev = {0};
    bool have_event = voice_frontend_read_event(&fev, pdMS_TO_TICKS(20));
    uint64_t read_now_ms = now_ms();
    bool afe_speech_active = false;
    size_t api_frames = 0;
    const int16_t *pcm = NULL;

    if (have_event) {
      if (fev.type == VOICE_FE_EVENT_ERROR) {
        set_state(REALTIME_VOICE_STATE_ERROR, "VOICE FRONTEND ERROR");
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      if (fev.type == VOICE_FE_EVENT_WAKE) {
        static uint64_t s_last_wake_rx_log_ms;
        if (read_now_ms - s_last_wake_rx_log_ms >= 1000ULL) {
          ESP_LOGI(TAG, "wake_event received");
          s_last_wake_rx_log_ms = read_now_ms;
        }
#if CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE
        if (!session_arm_get()) {
          session_arm_set(true);
          mark_session_activity(read_now_ms);
          recording = true;
          record_frames = 0;
          speech_start_ms = read_now_ms;
          last_voice_ms = read_now_ms;
          vad_consecutive_hits = 0;
          voiced_frames_in_turn = 0;
          snprintf(s_ctx.last_user, sizeof(s_ctx.last_user), "%s", "[recording]");
          set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
          ESP_LOGI(TAG, "activation phrase matched: %s", CONFIG_VOICE_ACTIVATION_PHRASE);
          ESP_LOGI(TAG, "record_start max_ms=%d", VOICE_ONESHOT_RECORD_MAX_MS);
        }
#endif
      } else if (fev.type == VOICE_FE_EVENT_AUDIO) {
        size_t copied = 0;
        if (voice_frontend_read_slot_pcm(fev.slot_id, fev.slot_seq, fe_pcm,
                                         VOICE_FE_MAX_PCM_FRAMES, &copied)) {
          afe_speech_active = fev.vad_speech;
          pcm = fe_pcm;
          api_frames = copied;
          if (afe_speech_active) {
            if (vad_consecutive_hits < UINT32_MAX) vad_consecutive_hits++;
          } else {
            vad_consecutive_hits = 0;
          }
        }
      }
    }

#if CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE
    if (!session_arm_get()) {
      if (!s_ctx.waiting_response && !s_ctx.speaking) {
        set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
      }
      continue;
    }
#endif

    bool voice = vad_consecutive_hits >= VOICE_LOCAL_VAD_CONSECUTIVE_FRAMES;
    bool force_voice = s_ctx.user_speech_notified;
    if (force_voice) s_ctx.user_speech_notified = false;

    if (recording && pcm != NULL && api_frames > 0) {
      size_t room = record_cap_frames - record_frames;
      size_t copy = api_frames < room ? api_frames : room;
      if (copy > 0) {
        memcpy(record_pcm + record_frames, pcm, copy * sizeof(int16_t));
        record_frames += copy;
      }
      mark_session_activity(read_now_ms);
    }
    if (recording && (voice || force_voice)) {
      voiced_frames_in_turn++;
      last_voice_ms = read_now_ms;
    }

    bool record_full = recording && record_frames >= record_cap_frames;
    bool record_timeout = recording &&
                          (read_now_ms - speech_start_ms) >=
                              (uint64_t)VOICE_ONESHOT_RECORD_MAX_MS;
    bool record_silence = recording && record_frames >= (VOICE_ONESHOT_SAMPLE_RATE / 2) &&
                          (read_now_ms - last_voice_ms) >=
                              (uint64_t)VOICE_ONESHOT_SILENCE_MS;
    if (recording && (record_full || record_timeout || record_silence)) {
      bool keep_reply_text = false;
      char reply_text[SCREEN_UI_TEXT_MAX] = {0};
      bool min_dur_ok =
          (read_now_ms - speech_start_ms) >= (uint64_t)CONFIG_VOICE_VAD_MIN_SPEECH_MS;
      bool voiced_enough = voiced_frames_in_turn >= VOICE_LOCAL_VAD_CONSECUTIVE_FRAMES;
      if (min_dur_ok && voiced_enough) {
        ESP_LOGI(TAG, "record_end frames=%lu reason=%s", (unsigned long)record_frames,
                 record_full ? "full" : (record_timeout ? "timeout" : "silence"));
        set_state(REALTIME_VOICE_STATE_SENDING, NULL);
        if (frontend_started) {
          voice_frontend_stop();
          frontend_started = false;
        }
        if (audio_output_is_active()) {
          ESP_LOGI(TAG, "oneshot cancelled: airplay output active");
        } else {
          s_ctx.waiting_response = true;
          set_state(REALTIME_VOICE_STATE_THINKING, NULL);
          voice_request_config_t req_cfg = {
              .url = s_voice_config.url,
              .api_key = s_voice_config.api_key,
              .model = "qwen3.5-omni-flash",
          };
          voice_request_result_t result = {0};
          esp_err_t req_err = voice_request_send_audio(&req_cfg, record_pcm, record_frames,
                                                       VOICE_ONESHOT_SAMPLE_RATE, &result);
          s_ctx.waiting_response = false;
          if (audio_output_is_active()) {
            ESP_LOGI(TAG, "oneshot result discarded: airplay output active");
          } else if (req_err == ESP_OK) {
            size_t n = strlen(result.text);
            if (n >= sizeof(reply_text)) n = sizeof(reply_text) - 1;
            memcpy(reply_text, result.text, n);
            reply_text[n] = '\0';
            keep_reply_text = true;
          } else {
            snprintf(s_err_detail, sizeof(s_err_detail), "REQ FAILED %d", result.status_code);
            set_state(REALTIME_VOICE_STATE_ERROR, s_err_detail);
            vTaskDelay(pdMS_TO_TICKS(1000));
            set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
          }
        }
      } else {
        ESP_LOGI(TAG, "record dropped: duration_ok=%d voiced_frames=%lu need=%u",
                 min_dur_ok ? 1 : 0, (unsigned long)voiced_frames_in_turn,
                 VOICE_LOCAL_VAD_CONSECUTIVE_FRAMES);
      }
      recording = false;
      record_frames = 0;
      vad_consecutive_hits = 0;
      voiced_frames_in_turn = 0;
      session_arm_set(false);
      realtime_voice_reset_session();
      if (keep_reply_text) {
        snprintf(s_ctx.last_assistant, sizeof(s_ctx.last_assistant), "%s", reply_text);
        set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
      }
    } else if (!s_ctx.waiting_response && !s_ctx.speaking) {
      set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
    }

    if (s_ctx.session_state == VOICE_SESSION_ACTIVE &&
        s_ctx.session_last_active_ms > 0 &&
        (read_now_ms - s_ctx.session_last_active_ms) >=
            (uint64_t)CONFIG_VOICE_SESSION_IDLE_TIMEOUT_MS) {
      session_arm_set(false);
      realtime_voice_reset_session();
      set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
    }
  }

#if CONFIG_VOICE_TOOLS_ENABLE
  voice_timers_deinit();
#endif
  ws_disconnect();
  playout_workbufs_release();
  voice_rs_destroy_play();
  audio_output_set_ref_tap(NULL, NULL);
  voice_reference_airplay_rs_destroy();
  voice_reference_airplay_scratch_free();
  if (frontend_started) {
    voice_frontend_stop();
  }
  afe_bridge_deinit();
  voice_reference_playout_rs_destroy();
  voice_reference_ring_deinit();
  voice_playout_deinit();
  voice_rs_free_float_bufs();
  voice_buf_free(record_pcm);
  voice_buf_free(fe_pcm);
  set_voice_ui_idle();
  s_ctx.task = NULL;
  s_ctx.mode = DEVICE_MODE_AIRPLAY;
  vTaskDelete(NULL);
}

esp_err_t realtime_voice_start(void) {
  if (s_ctx.running) {
    return ESP_OK;
  }
  if (s_ctx.lock == NULL) {
    s_ctx.lock = xSemaphoreCreateMutex();
  }

  if (s_voice_config.url[0] == '\0' && strlen(CONFIG_VOICE_REALTIME_URL) > 0) {
    snprintf(s_voice_config.url, sizeof(s_voice_config.url), "%s", CONFIG_VOICE_REALTIME_URL);
  }
  if (s_voice_config.api_key[0] == '\0' && strlen(CONFIG_VOICE_API_KEY) > 0) {
    snprintf(s_voice_config.api_key, sizeof(s_voice_config.api_key), "%s", CONFIG_VOICE_API_KEY);
  }
  if (s_voice_config.model[0] == '\0' && strlen(CONFIG_VOICE_MODEL) > 0) {
    snprintf(s_voice_config.model, sizeof(s_voice_config.model), "%s", CONFIG_VOICE_MODEL);
  }

  /* AFE is the sole audio front-end (AEC + NS + AGC + VADNet + WakeNet).
     The legacy standalone WakeNet path has been retired, so AFE init is
     mandatory: failure puts realtime_voice in REALTIME_VOICE_STATE_ERROR. */
  {
    esp_err_t afe_err = afe_bridge_init(VOICE_WAKE_MODEL_NAME);
    if (afe_err != ESP_OK) {
      ESP_LOGE(TAG, "AFE init failed: model=%s err=%s",
               VOICE_WAKE_MODEL_NAME, esp_err_to_name(afe_err));
      set_state(REALTIME_VOICE_STATE_ERROR, "AFE INIT FAILED");
      return afe_err;
    }
    ESP_LOGI(TAG, "AFE initialized (AEC+NS+AGC+VADNet+WakeNet model=%s)",
             VOICE_WAKE_MODEL_NAME);
    voice_reference_ring_init();
    voice_reference_playout_rs_ensure();
    audio_output_set_ref_tap(voice_reference_airplay_tap, NULL);
  }

  void *mic_handle = NULL;
  void *spk_handle = NULL;
  if (audio_output_get_mic_handle(&mic_handle) != ESP_OK ||
      audio_output_get_spk_handle(&spk_handle) != ESP_OK) {
    return ESP_ERR_INVALID_STATE;
  }
  s_ctx.mic = (esp_codec_dev_handle_t)mic_handle;
  s_ctx.spk = (esp_codec_dev_handle_t)spk_handle;

  /* Prevent mic close from disabling I2S RX channel, which can conflict
     with the speaker's TX channel sharing the same I2S bus on CoreS3. */
  esp_codec_set_disable_when_closed(s_ctx.mic, false);
  {
    esp_err_t mic_err = ensure_mic_persistent_open();
    if (mic_err != ESP_OK) {
      set_state(REALTIME_VOICE_STATE_ERROR, "MIC OPEN FAILED");
      return mic_err;
    }
  }

#if CONFIG_VOICE_TOOLS_ENABLE
  voice_timers_init();
#endif

  s_ctx.running = true;
  s_ctx.mode = DEVICE_MODE_AIRPLAY;
  s_ctx.enabled = CONFIG_VOICE_MODE_DEFAULT_ENABLED;
  const char *vad_label = "client-vadnet";
#ifdef CONFIG_VOICE_ACTIVATION_PHRASE
  const char *phrase_cfg = CONFIG_VOICE_ACTIVATION_PHRASE;
#else
  const char *phrase_cfg = "";
#endif
  ESP_LOGI(TAG,
           "voice cfg: vad=%s afe=on gain_db=%d activation=%s phrase=\"%s\" wake=%s",
           vad_label, CONFIG_VOICE_MIC_IN_GAIN_DB,
           CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE ? "on" : "off", phrase_cfg,
           VOICE_WAKE_MODEL_NAME);
  realtime_voice_reset_session();
  if (xTaskCreate(voice_task, "realtime_voice", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIO,
                  &s_ctx.task) != pdPASS) {
    s_ctx.running = false;
    voice_frontend_stop();
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "realtime voice started");
  return ESP_OK;
}

void realtime_voice_stop(void) {
  s_ctx.running = false;
  voice_frontend_stop();
}

void realtime_voice_set_enabled(bool enabled) { s_ctx.enabled = enabled; }

bool realtime_voice_is_enabled(void) { return s_ctx.enabled; }

void realtime_voice_on_airplay_state_changed(bool active) {
  static bool s_prev_airplay;
  if (active == s_prev_airplay) {
    return;
  }
  s_prev_airplay = active;
  if (active) {
    ESP_LOGI(TAG, "AirPlay active: voice frontend will pause");
  } else {
    ESP_LOGI(TAG, "AirPlay inactive: voice frontend can resume");
  }
}

device_mode_t realtime_voice_get_mode(void) { return s_ctx.mode; }

bool realtime_voice_is_activation_armed(void) {
#if CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE
  return session_arm_get();
#else
  return true;
#endif
}

bool realtime_voice_is_response_active(void) { return s_ctx.speaking || s_ctx.waiting_response; }
