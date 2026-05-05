#pragma once

#include "esp_afe_sr_iface.h"
#include "esp_err.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * AFE Bridge: wraps ESP-SR Audio Front-End (AEC + NS + AGC + VAD + WakeNet)
 *
 * Feed interleaved [mic, ref, mic, ref, ...] 16-bit PCM at 16 kHz.
 * Fetch returns clean audio + VAD state + wake word detection.
 *
 * AFE is the sole audio front-end; the legacy standalone WakeNet path
 * (wakeword_omi.{c,h}) has been retired.
 */

/* Wake-word model name selector. Picks the built-in ESP-SR wake model based on
 * the Kconfig choice. AFE owns wake-word detection now, so this lives here. */
#if defined(CONFIG_VOICE_WAKE_HIESP)
#define VOICE_WAKE_MODEL_NAME "wn9_hiesp"
#elif defined(CONFIG_VOICE_WAKE_NIHAOXIAOZHI)
#define VOICE_WAKE_MODEL_NAME "wn9_nihaoxiaozhi_tts"
#elif defined(CONFIG_VOICE_WAKE_ALEXA)
#define VOICE_WAKE_MODEL_NAME "wn9_alexa"
#elif defined(CONFIG_VOICE_WAKE_JARVIS)
#define VOICE_WAKE_MODEL_NAME "wn9_jarvis_tts"
#elif defined(CONFIG_VOICE_WAKE_HIJOY)
#define VOICE_WAKE_MODEL_NAME "wn9_hijoy_tts"
#elif defined(CONFIG_VOICE_WAKE_COMPUTER)
#define VOICE_WAKE_MODEL_NAME "wn9_computer_tts"
#else
#define VOICE_WAKE_MODEL_NAME "wn9_hiesp"
#endif

esp_err_t afe_bridge_init(const char *wakenet_model_name);
int      afe_bridge_get_feed_chunksize(void);
int      afe_bridge_get_fetch_chunksize(void);
int      afe_bridge_get_sample_rate(void);

/* feed interleaved mic+ref, samples_per_channel each */
esp_err_t afe_bridge_feed(const int16_t *mic_ref_interleaved, size_t samples_per_channel);

/* fetch processed result; blocks up to ticks. returns NULL on timeout */
afe_fetch_result_t *afe_bridge_fetch(TickType_t ticks);

void afe_bridge_reset(void);
void afe_bridge_deinit(void);
bool afe_bridge_is_ready(void);
