# Core Architecture

## Product Scope

This firmware now targets exactly one product: **M5Stack CoreS3** running an **AirPlay 1 audio receiver core**.

Retained runtime responsibilities:

- boot orchestration
- NVS-backed settings
- STA Wi-Fi
- mDNS / RTSP / AirPlay service lifecycle
- audio decode / buffer / resample / output
- minimal receiver state
- CoreS3 speaker-path initialization

Removed runtime responsibilities:

- display and touchscreen UX
- SoftAP and in-device provisioning flows
- USB / HTTP management surfaces
- OTA and diagnostic stream features
- Ethernet and Bluetooth
- external DAC families
- local button, LED, and playback-control paths

## Component Map

### `components/app_core`

- Owns startup sequencing.
- Initializes NVS and retained settings.
- Decides whether the device should enter idle `CONFIG_REQUIRED` or start networking.
- Runs the small network monitor task that starts and stops AirPlay with connectivity.

### `components/airplay_core`

- Owns AirPlay service bootstrap.
- Hosts RTSP request handling and plist helpers.
- Publishes session state transitions into the retained receiver-state model.

### `components/audio_core`

- Owns the playable audio path only.
- Contains decode, timing, buffering, DSP/EQ defaults, resampling, and CoreS3 speaker output.
- Talks to the board layer for the single retained speaker handle.

### `components/network_core`

- Owns STA Wi-Fi, mDNS registration, NTP clock, and socket helpers.
- No SoftAP, no provisioning UI helpers, no HTTP management surface.

### `components/board_cores3`

- Owns the single supported board.
- Initializes the CoreS3 BSP audio path and speaker codec handle.
- Carries the single-app partition table used by the default build.

### `main/main.c`

- Thin entrypoint only.
- Calls `app_core_run()` and contains no product logic.

## Startup Flow

1. `main/main.c` calls `app_core_run()`.
2. `app_core` initializes receiver state, NVS, settings, and audio preallocation.
3. If Wi-Fi credentials are missing, the device logs the condition and stays idle.
4. If credentials exist, `network_core` starts STA Wi-Fi.
5. Once connected, `airplay_core` initializes the CoreS3 board path, audio pipeline, mDNS, and RTSP server.

## No-Credentials Behavior

The refactored product deliberately does **not** fall back to:

- display/touch setup
- SoftAP setup mode
- USB Web Serial tools
- HTTP management pages
- OTA upload surfaces

Missing credentials are treated as a deployment/configuration issue outside the runtime UX.
