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

## 组件图（目标五层架构）
```
Layer 5: App Entry      → main/main.c
Layer 4: Application    → app_core/           (app_core, settings)
Layer 3: Service        → airplay_core/       (airplay_service, rtsp, plist)
                        → realtime_voice/     (realtime_voice)
Layer 2: Domain         → audio_core/         (pipeline, decoder, dsp, output, timing...)
                        → screen_ui/          (screen_ui, ui_spectrum, ui_lyric...)
                        → network_core/       (wifi, mdns, ntp, socket_utils)
Layer 1: HAL/BSP        → board_cores3/       (board, partitions)
```

### 强制依赖规则
| 规则 | 说明 |
|------|------|
| 禁止向上依赖 | Layer N 不得 #include Layer N+1 或更高的头文件 |
| 禁止跨层跳级 | Layer 3 只能通过 Layer 2 接口访问底层 |
| 同层隔离 | 同层级组件间不得相互依赖 |
| ESP-IDF 无限制 | 所有层均可依赖 esp_*、lvgl、cJSON 等 |

详见: `docs/adr/adr-001-layered-architecture.md`

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
