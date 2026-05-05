# ADR-005: Voice assistant coexistence with AirPlay (duck per reply)

## Status

Accepted — implemented 2026-05-05.

## Context

Earlier firmware treated an active AirPlay streaming session as a **hard gate**: the realtime voice task stopped its active loop, closed the microphone, disconnected the WebSocket, and ignored uplink and `response.audio.delta` while AirPlay was `session_establishing` or `streaming`. That maximized AirPlay resource exclusivity but prevented using the assistant during music playback.

The device has a single speaker path and shared I2S. Assistant audio from DashScope is **native PCM** (`response.audio.delta`), not a separate on-device TTS stage.

## Decision

1. **Listen while AirPlay plays**: `should_run_voice()` no longer returns false solely because AirPlay is active. The voice task keeps the mic pipeline and WebSocket session (subject to existing receiver fault/config gates).

2. **Speaker arbitration per assistant reply**: When the assistant needs to play audio, realtime voice calls `audio_output_acquire_external()` as before. That pauses the AirPlay playback worker for the duration of the reply, then releases ownership—analogous to **ducking** music for a short announcement.

3. **AEC reference for music**: `audio_output_set_ref_tap()` taps PCM immediately before `esp_codec_dev_write()` in the CoreS3 output path. Realtime voice registers a callback that downmixes stereo to mono, resamples to **16 kHz**, and pushes into the same reference ring already fed from assistant playout. ESP-SR **AFE** remains mandatory so **VADNet** and **AEC** see both assistant playback and AirPlay content.

4. **Wake word**: Standalone `wakeword_omi` was removed; WakeNet runs **inside AFE** only (`VOICE_WAKE_MODEL_NAME` in `afe_bridge.h`).

5. **Client VAD**: RMS-based client VAD and the RMS playback echo-guard multiplier were removed. Client mode uses **VADNet** consecutive-frame hits on **AFE-cleaned** audio; barge-in during assistant playback treats VADNet speech detection as genuine user speech.

## Consequences

- Users can say the activation phrase and speak over AirPlay; uplink and tools work during streaming.
- Assistant replies still **preempt** the AirPlay worker briefly (not a simultaneous mix-down).
- Reference-ring pushes may originate from two contexts (voice task + audio worker); the ring uses a critical section around push/pop.
- Documentation and verification scenarios must treat “duck” semantics as above, not “voice fully suspended until AirPlay stops.”

## References

- `components/realtime_voice/realtime_voice.c` — gates, `realtime_voice_on_airplay_state_changed`, reference tap callback
- `components/audio_core/audio/audio_output_cores3.c` — `audio_output_set_ref_tap`
- `docs/voice-interaction.md`
- ADR-001 layered architecture — voice stays in Layer 3; audio_output tap is a downward callback registration from Layer 3 into Layer 2.
