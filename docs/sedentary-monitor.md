# Sedentary desk monitor (CoreS3 GC0308, local coarse occupancy)

## Purpose
Optional firmware feature: **low-rate grayscale frames** from the onboard **GC0308**, **local** comparison against an **empty-desk baseline** and a **calibrated ROI**, then a small **state machine** that triggers a **local** reminder (short tone + on-screen line). **No image upload**, no DashScope vision HTTP, and **no dependency on Wi‑Fi** for detection.

Timer behaviour stays **configuration-driven**: defaults such as one hour before the first reminder are only defaults in Kconfig / `config.toml`; change intervals there, not in core logic.

The on-screen / alert line is fixed to product copy **`stand up`** by default (`sedentary.voice_prompt` / `CONFIG_SEDENTARY_VOICE_PROMPT`). An empty prompt still falls back to **`stand up`**.

## Configuration
- **Example TOML**: [`config/config.toml.example`](../config/config.toml.example) — `[sedentary]` and `[sedentary.local]`.
- **Pre-build**: [`scripts/pio_prebuild.py`](../scripts/pio_prebuild.py) maps keys → `CONFIG_SEDENTARY_*` and `CONFIG_SEDENTARY_LOCAL_*` in root `sdkconfig.defaults`.
- **Menu**: `Airplay ESP Configuration -> Sedentary monitor (GC0308 local occupancy)` in [`components/app_core/Kconfig.projbuild`](../components/app_core/Kconfig.projbuild).

Build-time gate: `SEDENTARY_ENABLE` must be **y** for camera and detection code. `sedentary_monitor_set_enabled()` can pause logic at runtime without rebuilding.

### Main keys
| TOML | Kconfig | Role |
|------|---------|------|
| `sedentary.enabled` | `CONFIG_SEDENTARY_ENABLE` | Master switch |
| `sedentary.capture_interval_sec` | `CONFIG_SEDENTARY_CAPTURE_INTERVAL_SEC` | Frame period |
| `sedentary.remind_after_sec` | `CONFIG_SEDENTARY_REMIND_AFTER_SEC` | Seated time before first remind |
| `sedentary.absence_reset_sec` | `CONFIG_SEDENTARY_ABSENCE_RESET_SEC` | Absent streak to end round |
| `sedentary.repeat_interval_sec` | `CONFIG_SEDENTARY_REPEAT_INTERVAL_SEC` | Repeat while overdue |
| `sedentary.detect_fail_suspend_count` | `CONFIG_SEDENTARY_FAIL_SUSPEND_COUNT` | Pipeline fails before `SUSPENDED` |
| `sedentary.suspend_cooldown_sec` | `CONFIG_SEDENTARY_SUSPEND_COOLDOWN_SEC` | Cooldown in `SUSPENDED` |
| `sedentary.camera.xclk_gpio` | `CONFIG_SEDENTARY_CAM_XCLK_GPIO` | LEDC XCLK GPIO (often `15`) |
| `sedentary.voice_prompt` | `CONFIG_SEDENTARY_VOICE_PROMPT` | Reminder line (default `stand up`) |

### Local detection keys (`[sedentary.local]`)
| TOML | Role |
|------|------|
| `present_threshold` / `absent_threshold` | Mean absolute gray difference in ROI vs baseline; present streak when mean ≥ present threshold; absent streak when mean ≤ absent threshold; between → `UNKNOWN` (no streak progress). |
| `present_confirm_count` / `absent_confirm_count` | Consecutive frames required before emitting `PRESENT` or `ABSENT`. |
| `roi_margin_px` | Expands the ROI bounding box derived from the seated calibration step. |
| `enable_calibration` | When **true**, NVS holds empty baseline + ROI; `sedentary_calibration_is_ready()` is false until both steps complete. When **false**, the **first** captured frame is used in RAM as the empty baseline and the ROI is the full QQVGA frame (desk should be empty on first tick). |
| `recalibrate_on_boot` | When **true**, sedentary NVS keys are erased on boot so you can recalibrate. |

## Calibration (NVS, two-step)
When `enable_calibration` is true:

1. **`sedentary_calibration_begin_empty()`** — user leaves the field of view; firmware captures one **QQVGA grayscale** frame and stores it as the **baseline** (namespace `sed_mon`, blob key `cal_v1`).
2. **`sedentary_calibration_begin_present()`** — user sits normally; firmware captures another frame, diffs against the baseline, finds a bounding box of strong differences, expands it by `roi_margin_px`, and writes **ROI** fields into the same blob.

Until both steps succeed, `sedentary_calibration_is_ready()` is false and the monitor task **does not** advance the state machine (no reminders); it does **not** treat “missing calibration” as repeated detect failures.

These APIs are available for a future UI or a debug path; there is no LVGL wizard in-tree yet.

## Runtime pipeline
- **Camera**: `PIXFORMAT_GRAYSCALE`, `FRAMESIZE_QQVGA` (160×120), `sedentary_camera_capture_frame_small()`.
- **Detect**: `sedentary_local_detect_run()` updates a smoothed `present` / `absent` / `unknown` result for the existing state machine in `sedentary_monitor.c`.
- **States**: `IDLE`, `ABSENT`, `PRESENT_TRACKING`, `OVERDUE_REMINDING`, `SUSPENDED` (after repeated capture / processing failures).
- **Speaker**: `sedentary_alert` uses `audio_output_acquire_external("sedentary_monitor", true)`; reminders **defer** if AirPlay is streaming or establishing a session, or if `realtime_voice_is_response_active()` is true. If the speaker cannot be acquired, the UI line is still set first so **`stand up`** (or your prompt) can appear.

## Lifecycle
- Started from `app_core` after realtime voice starts (`sedentary_monitor_start()`); `sedentary_local_detect_on_boot()` runs after camera init (optional NVS erase when `recalibrate_on_boot` is enabled). Stopped before `realtime_voice_stop()`.

## See also
- Voice interaction and arbitration: [`docs/voice-interaction.md`](voice-interaction.md)
