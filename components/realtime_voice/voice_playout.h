#pragma once

/*
 * voice_playout: SPSC ring buffer that holds assistant audio coming back from
 * the Omni Realtime model (response.audio.delta). Qwen-Omni-Realtime emits PCM
 * natively; there is no separate TTS stage, so this module is named "playout"
 * to reflect what it actually does (queue model PCM for the speaker writer).
 *
 * Lives in Layer 3 (realtime_voice). Uses heap_caps_malloc with SPIRAM
 * preference to leave internal DMA-capable heap for the LCD SPI bus.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Tunables (kept here so both the module and the orchestrator see them
 *      and they belong to the playout concern, not the WebSocket or VAD). ---- */

#define VOICE_PLAYOUT_RING_MS         10000 /* total ring capacity in ms */
#define VOICE_PLAYOUT_PREFILL_MS          800  /* startup watermark before draining */
#define VOICE_PLAYOUT_LOW_MS              500  /* underrun warning threshold */
#define VOICE_PLAYOUT_REBUFFER_RESUME_MS  800  /* resume after underrun */
#define VOICE_PLAYOUT_SILENCE_FILL_MS     20   /* inject silence once when starving */
#define VOICE_PLAYOUT_GAP_TIMEOUT_MS      800  /* close playback after this much silence */

/** Allocate the ring (idempotent). Sample rate is the Omni response audio
 *  rate, used to size the ring so it holds VOICE_PLAYOUT_RING_MS of audio. */
bool voice_playout_init(uint32_t sample_rate);

/** Release the ring and the cached gap-conceal stereo frame. */
void voice_playout_deinit(void);

/** Reset write/read pointers and last-write timestamp without freeing memory. */
void voice_playout_reset(void);

/** Stream format reported by the current assistant audio reply. Defaults to the
 *  speaker-native contract until the first audio chunk arrives. */
void voice_playout_set_stream_format(const voice_pcm_format_t *format);
voice_pcm_format_t voice_playout_stream_format(void);

/** Mono-frame counts available for pop / free for push. */
size_t voice_playout_avail(void);
size_t voice_playout_free(void);

/** Push samples (mono int16) into the ring. Returns number of samples accepted;
 *  drops the tail and logs a warning if the ring is full. */
size_t voice_playout_push(const int16_t *pcm, size_t samples);

/** Pop up to max_samples mono int16 samples. Returns how many were popped. */
size_t voice_playout_pop(int16_t *dst, size_t max_samples);

/** Last write timestamp (in ms; matches realtime_voice's now_ms()). */
uint64_t voice_playout_last_write_ms(void);
void voice_playout_set_last_write_ms(uint64_t ms);

/** Cache the most recent stereo frame block for gap concealment. */
void voice_playout_save_last_stereo(const int16_t *src, size_t frames);

/** Borrow the cached gap-conceal stereo frame; returns NULL if unset.
 *  Output `*frames` is the cached frame count. */
const int16_t *voice_playout_last_stereo(size_t *frames);

#ifdef __cplusplus
}
#endif
