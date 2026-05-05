# Voice Interaction Integration

## Purpose
Run a dual-mode firmware on CoreS3:
- `AIRPLAY_MODE` for RAOP playback.
- `VOICE_MODE` for persistent realtime voice assistant interaction over WebSocket.

The legacy one-shot HTTP voice pipeline is removed.

## Build-time configuration (PlatformIO)

- **Source file**: [`config/config.toml`](../config/config.toml.example) (copy from `config.toml.example`; keep secrets out of git).
- **Pre-build**: [`scripts/pio_prebuild.py`](../scripts/pio_prebuild.py) runs before each `pio run` and writes **`sdkconfig.defaults` at the project root** — this is what ESP-IDF / PlatformIO merges when CMake configures the app. A duplicate copy is written to **`config/generated/sdkconfig.defaults`** for local diff only; IDF does **not** read that path by default.
- **Stale `sdkconfig`**: if voice-related `CONFIG_VOICE_*` values in `.pio/build/<PIOENV>/sdkconfig` (e.g. `m5cores3`) disagree with the generated defaults, prebuild **deletes** that file so the next configure regenerates from root `sdkconfig.defaults`.
- **Workflow**: edit `config/config.toml` → `pio run` (or `pio run -t fullclean` if options still look wrong) → flash.
- **Self-check**: on `realtime_voice_start`, firmware logs one line **`voice cfg:`** with client VAD settings, mic gain, activation, and WakeNet model name — confirm it matches your TOML after a rebuild.

Current stack uses **ESP-SR AFE** (mandatory): AEC + NS + AGC + **VADNet** + WakeNet in one pipeline. The wake phrase uses the selected WakeNet model (default `wn9_hiesp`). **Alibaba DashScope** `qwen3.5-omni-flash-realtime` is the assistant runtime: user audio uploads after wake, and the model returns **native PCM** in `response.audio.delta` (no separate device-side TTS). The firmware does not send OpenAI Realtime headers or use OpenAI endpoint compatibility paths. `Hi Omi` is retained only as the later custom-model target.

## Runtime Design
- `realtime_voice` runs as an independent FreeRTOS task.
- `app_core` owns mode arbitration; `airplay_core` and `realtime_voice` do not control each other directly.
- Speaker ownership handoff:
  - realtime voice acquires speaker ownership from `audio_output` before **assistant audio playout** (Omni `response.audio.delta` PCM).
  - while voice owns speaker, the AirPlay playback worker is paused to avoid handle contention (**duck per reply**).
  - ownership is released on playback done, disconnect, or voice task teardown.
  - AirPlay RTSP/mDNS remains available in the background, but the playback worker is only started when RTSP enters a real `PLAYING` state.
- **Sedentary monitor (optional, build-time `CONFIG_SEDENTARY_ENABLE`)**: a separate subsystem from Realtime Voice. It does **not** use the voice WebSocket, tools, cloud TTS, or any vision HTTP API. It uses GC0308 grayscale frames and **local** baseline/ROI logic (see [`docs/sedentary-monitor.md`](sedentary-monitor.md)). It acquires the speaker briefly for a local tone plus on-screen copy (default **`stand up`**) when a desk reminder fires, and **defers** that reminder while AirPlay is streaming or establishing a session, or while `realtime_voice_is_response_active()` is true (assistant speaking or waiting on a response). Timer defaults are only defaults; intervals are fully driven by Kconfig / `config.toml`.
- Voice mode entry gate:
  - Wi-Fi connected (or discoverable, per receiver snapshot)
  - voice mode enabled
  - receiver not faulted/recovering/config-required
  - **AirPlay streaming no longer disables the voice loop** — mic + WebSocket stay up during music; assistant replies still acquire the speaker for playout (duck).
- AirPlay coexistence (ducking):
  - `airplay_service_is_active()` remains the signal for “AirPlay session” but **does not** tear down the voice pipeline.
  - `realtime_voice_on_airplay_state_changed()` logs transitions only; it does **not** force-disarm activation or disconnect the socket.
  - While music plays, PCM bound for the codec is also **tapped** (`audio_output_set_ref_tap`) into the AEC reference ring (mono, 16 kHz) so echo cancellation sees AirPlay as well as assistant playout.
  - Assistant playback still uses `audio_output_acquire_external()` so AirPlay’s worker is paused for that reply only.
  - See [`docs/adr/adr-005-voice-airplay-ducking.md`](adr/adr-005-voice-airplay-ducking.md).
- Session lifecycle:
  - **Activation phrase mode** (`CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE`): Omni only arms after local WakeNet detects `Hi ESP`.
  - **Continuous listen mode**: WebSocket stays up while gates allow and keeps listening locally for the wake phrase without requiring a tap.
  - Armed session -> idle timeout disarms, clears context, and returns to wake-word listening.
  - cleanup clears in-memory context only; no NVS writes

## Activation troubleshooting (wake phrase)

Official docs: [ESP-SR WakeNet](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/wake_word_engine/README.html), [Realtime overview](https://www.alibabacloud.com/help/en/model-studio/realtime).

1. **AFE + WakeNet**: activation uses WakeNet **inside** the AFE pipeline. If `afe_bridge_init` fails, realtime voice does not start. There is no standalone `wakeword_omi` path.
2. **Logs to watch** (with `CONFIG_VOICE_DEBUG_EVENT_LOG=y` you also get every inbound event type):
   - `AFE initialized` / `voice cfg: ... wake=wn9_...` — confirms AFE and wake model selection.
   - `activation phrase matched: ...` and `activation armed=1` — confirms the wake phrase was detected locally.
   - `conversation.item.create` / `response.create` — confirms the text handoff to Omni happened.
3. Before wake, no Omni requests should be emitted. If any voice event appears before `activation armed=1`, treat it as a state-machine regression.

## Realtime Event Flow
1. Open `wss` websocket to `CONFIG_VOICE_REALTIME_URL?model=CONFIG_VOICE_MODEL`.
2. Send `Authorization: Bearer <key>` and `session.update` with:
   - `instructions`, `modalities`, string `input_audio_format` / `output_audio_format` set to DashScope `"pcm"`
   - optional `voice` (`CONFIG_VOICE_TTS_VOICE`)
   - optional `input_audio_transcription` model (`CONFIG_VOICE_INPUT_TRANSCRIPTION_MODEL`) for user transcript events
   - optional static `tools` when `CONFIG_VOICE_TOOLS_ENABLE`
   - `turn_detection`: `null`
3. While unarmed, mic PCM feeds **AFE** (WakeNet + VADNet + AEC path); do not send Omni events until armed.
4. After the activation phrase arms the session, **client VAD** uses **VADNet** hits on AFE output to gate `input_audio_buffer.append`. When silence closes the turn, send `input_audio_buffer.commit`, then `response.create`.
5. Consume response events:
   - `response.audio.delta` -> base64 decode -> speaker playback
   - `response.audio_transcript.delta`/`response.audio_transcript.done` and text delta events -> overlay assistant text
   - `conversation.item.input_audio_transcription.completed` -> optional user transcript text for the overlay
   - user barge-in during speaking or thinking (client) -> `response.cancel`
   - `response.done` -> return to listening
   - `error` -> overlay shows server `error.message` when present

## Realtime tools (function calling)

When `CONFIG_VOICE_TOOLS_ENABLE` is on (TOML: `[voice.tools]` `enabled = true`), `session.update` includes a static `tools` list. Per Alibaba [client events](https://help.aliyun.com/zh/model-studio/client-events) and [server events](https://help.aliyun.com/zh/model-studio/server-events):

- **Registered tools**: `set_timer`, `cancel_timer`, `get_device_status`, `get_network_status`, `get_time`, `get_date`, `set_screen_brightness` (`brightness_percent` 0–100), `play_local_chime` (optional `frequency_hz`, `duration_ms`, `amplitude_pct`), `airplay_status`, `set_volume`, `get_volume`. There is **no wall-clock alarm** in this build; timers use `esp_timer` one-shots only.
- **Volume commands**: utterances such as "音量大一点", "音量小一点", "调到 80%", "静音", and "取消静音" should call `set_volume`. Volume percent maps to the existing `-30.0 dB..0.0 dB` output range and is persisted through `audio_volume`.
- **Server**: `response.function_call_arguments.done` carries authoritative `name`, `call_id`, and `arguments` (JSON string). `delta` events are diagnostic only.
- **Client**: send `conversation.item.create` with `item.type = function_call_output`, matching `call_id`, then a **second** `response.create` to continue the assistant reply (see client-events doc). This path does not reuse the per-turn `response.create` gate used for user audio commits.
- **Dedup**: the same `call_id` is ignored if already completed; `input_audio_buffer.committed` clears the last `call_id` so a new user turn can run tools again.
- **Search**: DashScope forbids `tools` and `enable_search` together; this firmware does not send `enable_search`.

Official references: [Realtime overview](https://www.alibabacloud.com/help/en/model-studio/realtime), [Client events](https://www.alibabacloud.com/help/en/model-studio/client-events), [Server-side events](https://www.alibabacloud.com/help/en/document_detail/2922855.html) (tool event details are most complete on the Chinese server-events page).

## Audio Path
- Microphone input: CoreS3 BSP mic (`BOARD_AUDIO_MIC_ID`) through `esp_codec_dev_read`.
- Assistant output: CoreS3 speaker (`BOARD_AUDIO_SPK_ID`) through `esp_codec_dev_write`.
- Capture format sent to DashScope: PCM S16LE mono at `CONFIG_VOICE_INPUT_SAMPLE_RATE` (default 16 kHz).
- Assistant playback stream from DashScope is treated as PCM S16LE mono at `CONFIG_VOICE_OUTPUT_SAMPLE_RATE` (default 24 kHz).
- **I2S clock alignment:** mic and assistant playout speaker opens use `CONFIG_OUTPUT_SAMPLE_RATE_HZ` (same as AirPlay / `audio_output`) so the shared I2S controller does not mix cloud session rates with 44.1 kHz hardware peer mode. Firmware resamples in `realtime_voice` between hardware rate and input/output cloud rates.
- CoreS3 hardware codec format: stereo I2S with 384x MCLK (for 44.1 kHz family); realtime voice downmixes mic stereo to mono before uplink resampling and upmixes assistant PCM to stereo before speaker writes.
- Playback: PCM16 chunks from realtime audio deltas.
- Recovery semantics: reopen is considered successful only after a positive read frame (`read_ok > 0`), and repeated stalls escalate to cooldown instead of tight reopen loops.
- AirPlay worker semantics: discoverable/connected RTSP sessions do not imply active playback. The worker sleeps until RTSP `PLAYING`, and voice ownership release triggers a playback refresh so the next eligible AirPlay session can wake cleanly.

## Configuration
Defined in `Airplay ESP Configuration -> Realtime Voice`:

**DashScope / API**
- `CONFIG_VOICE_REALTIME_URL` — WebSocket base **without** query string. Examples:
  - Mainland (Beijing): `wss://dashscope.aliyuncs.com/api-ws/v1/realtime`
  - International (Singapore): `wss://dashscope-intl.aliyuncs.com/api-ws/v1/realtime`
- `CONFIG_VOICE_API_KEY` — DashScope API key (Bearer). Set in local `sdkconfig` or CI; never commit.
- `CONFIG_VOICE_MODEL` — e.g. `qwen3.5-omni-flash-realtime`

**Session / protocol**
- `CONFIG_VOICE_TTS_VOICE` — assistant voice name (e.g. `Ethan`, `Cherry`); leave empty in menuconfig to omit.
- `CONFIG_VOICE_ENABLE_INPUT_TRANSCRIPTION` — enable optional transcript events for user audio.
- `CONFIG_VOICE_INPUT_TRANSCRIPTION_MODEL` — model used when transcript events are enabled.
- `CONFIG_VOICE_TOOLS_ENABLE` — register DashScope Realtime tools (timers, volume, device/network/time/date, brightness, chime, AirPlay status); default off.
- `CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE` — require local WakeNet activation before `response.create` on commits.
- `CONFIG_VOICE_ACTIVATION_PHRASE` — display/log label for the fixed WakeNet phrase (default `Hi ESP`).

**Other**
- `CONFIG_VOICE_MODE_DEFAULT_ENABLED`
- `CONFIG_VOICE_MIC_IN_GAIN_DB` — codec mic gain after open (AFE capture level).
- `CONFIG_VOICE_CAPTURE_STALL_TIMEOUT_MS`
- `CONFIG_VOICE_CAPTURE_RECOVERY_RETRY_MAX`
- `CONFIG_VOICE_CAPTURE_STALL_CYCLE_MAX`
- `CONFIG_VOICE_CAPTURE_RECOVERY_COOLDOWN_MS`
- `CONFIG_VOICE_WS_CONNECT_TIMEOUT_MS`
- `CONFIG_VOICE_WS_RETRY_BASE_MS`
- `CONFIG_VOICE_WS_RETRY_MAX_MS`
- `CONFIG_VOICE_WS_RETRY_JITTER_MS`
- `CONFIG_VOICE_WS_PING_INTERVAL_SEC`
- `CONFIG_VOICE_APPEND_HEALTH_TIMEOUT_MS`
- `CONFIG_VOICE_INPUT_SAMPLE_RATE`
- `CONFIG_VOICE_OUTPUT_SAMPLE_RATE`
- `CONFIG_VOICE_UPLINK_FRAME_MS`
- `CONFIG_VOICE_VAD_SILENCE_TIMEOUT_MS`
- `CONFIG_VOICE_VAD_MIN_SPEECH_MS`
- `CONFIG_VOICE_SESSION_IDLE_TIMEOUT_MS`
- `CONFIG_VOICE_CONTEXT_MAX_CHARS`
- `CONFIG_VOICE_PROMPT_PRESET_*`

Security rule: API key must be injected via local `sdkconfig` or CI and must never be committed.

Prompt source of truth:
- `docs/voice-prompt-spec.md`

## UI and stability (CoreS3)
- LVGL runs on CPU1 with a configurable animation period (`CONFIG_SCREEN_UI_ANIM_INTERVAL_MS`); each frame ends with `taskYIELD()` so IDLE1 can run under the task watchdog.
- While a DashScope WebSocket is connecting (TLS in flight), `screen_ui_set_voice_network_busy(true)` lengthens the LVGL timer to `CONFIG_SCREEN_UI_ANIM_STRESS_INTERVAL_MS`; it is cleared when the session is ready or the socket disconnects.
- Large `realtime_voice` capture/playout buffers prefer SPIRAM to leave internal DMA-capable heap for LCD SPI.

## UI Behavior
Voice overlay states:
- Voice standby
- Connecting
- Listening
- Sending
- Thinking
- Speaking
- Error

The main screen no longer presents an AirPlay-first front-panel view. AirPlay remains a background capability, while the visible UI stays centered on voice states and wake-word interaction.

Overlay shows latest user/assistant summary text. While the voice overlay is visible and the session is **armed**, a tap calls `realtime_voice_notify_user_speech_start()` (PTT assist only; it does **not** bypass the activation phrase).

## Verification
1. Build: `~/.platformio/penv/bin/pio run -e m5cores3`
2. Flash and monitor:
   - `~/.platformio/penv/bin/pio run -e m5cores3 -t upload`
   - `~/.platformio/penv/bin/pio device monitor`
3. Scenario checks:
   - AirPlay idle/discoverable: continuous mode connects when gates allow and listens for the wake phrase. AirPlay should remain discoverable while its playback worker stays asleep.
   - AirPlay streaming: voice loop **stays active** (mic + socket); log includes `AirPlay active: voice assistant stays armed; duck on reply`.
   - During AirPlay playback: wake phrase still arms; uplink append/commit works; first assistant PCM logs optional `playout ttfb_ms=...`.
   - Assistant speaks over music: speaker acquire pauses AirPlay worker briefly; release restores playback refresh.
   - AirPlay disconnects: log `AirPlay inactive`; voice continues without requiring a full reconnect.
   - Wake phrase: logs `activation phrase matched`, then `activation armed=1`; speech without the phrase must not produce `input_audio_buffer.append` / `commit` / `response.create`.
   - Speech input after wake: client VADNet gates uplink, then `input_audio_buffer.commit` and `response.create`.
   - Assistant returns audio: playout starts and state turns speaking.
   - Missing API key: error overlay appears and AirPlay remains functional.
   - Mic no-data fault: recovery attempts are bounded; cooldown/escalation occurs with no repeating 2s stall spam.
   - Voice playout path: speaker ownership acquire/release logs appear around response playback lifecycle.
