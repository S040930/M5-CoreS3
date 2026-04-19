<div align="center">
<img src="docs/logo_airplay_esp32.png" alt="AirPlay ESP32" width="400">

# ESP32 AirPlay 2 Receiver

[![GitHub stars](https://img.shields.io/github/stars/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/network)
[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/hardware-M5Stack_Core_S3-green?style=flat-square)](https://docs.m5stack.com/en/core/CoreS3)

**Stream music from your Apple devices over Wi‑Fi to a M5Stack Core S3 — AirPlay 2, no cloud, no extra app.**

</div>

---

## What is this?

This firmware turns a **[M5Stack Core S3](https://docs.m5stack.com/en/core/CoreS3)** (ESP32‑S3) into a wireless **AirPlay 2** speaker. It appears in Control Center on iPhone, iPad, and Mac like any AirPlay receiver. Audio plays on the board’s **built‑in speaker path** (AW88298 amplifier via the official BSP / `esp_codec_dev`).

This repository is maintained for **Core S3 only**: onboard Wi‑Fi, USB‑C power, and captive‑portal setup. **Bluetooth Classic / A2DP is not used** on ESP32‑S3 (no Classic BT controller).

**No cloud. No app. Just tap and play.**

---

## Hardware

| Item | Notes |
|------|--------|
| **M5Stack Core S3** | ESP32‑S3, PSRAM, built‑in speaker amp — [M5 documentation](https://docs.m5stack.com/en/core/CoreS3) |
| **USB‑C cable** | Power and serial flash |

No external DAC or PCM5102 wiring is required for the supported build.

---

## Flash the Firmware

Three options: **Web flasher** (no install), **PlatformIO**, or **ESP-IDF**.

### Option A — Web Flasher (beginners)

1. Download the latest **`airplay2-receiver-m5cores3.bin`** from the [Releases](https://github.com/rbouteiller/airplay-esp32/releases/latest) page.
2. Open the [ESP Web Flasher](https://espressif.github.io/esptool-js/) (Chrome or Edge).
3. Connect the Core S3 via USB, **Connect** → choose the serial port.
4. Flash address **`0x0`**, select the `.bin`, **Program**.
5. Power‑cycle the board; it boots into Wi‑Fi setup mode if not configured.

### Option B — PlatformIO

```bash
pip install platformio
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32

pio run -e m5cores3 -t upload --upload-port /dev/cu.usbmodemXXXX
pio run -e m5cores3 -t buildfs
pio run -e m5cores3 -t uploadfs --upload-port /dev/cu.usbmodemXXXX
pio run -e m5cores3 -t monitor --upload-port /dev/cu.usbmodemXXXX
```

Replace `/dev/cu.usbmodemXXXX` with your actual serial device (macOS: `ls /dev/cu.usb*`; avoid unrelated ports such as `Bluetooth-Incoming-Port`). On Windows use `COMn`.

**Firmware vs SPIFFS:** `upload` writes the application only. The device web UI lives in the **`storage`** SPIFFS partition from [`data/www/`](data/www/). Without **`buildfs` + `uploadfs`**, logs may show `Failed to open /spiffs/www/index.html` and **0 bytes used** for SPIFFS. You can still manage Wi‑Fi and AirPlay pairing over **USB** using [`tools/usb_web/`](tools/usb_web/README.md) (see below).

The only predefined environment is **`m5cores3`** (see [`platformio.ini`](platformio.ini)).

### Option C — ESP-IDF

```bash
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32
source /path/to/esp-idf/export.sh

idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5cores3" build
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## Custom configuration (`user_platformio.ini`)

You can add a **`user_platformio.ini`** (already merged via `extra_configs`) to override pins or features without editing the main config.

1. Extend **`env:m5cores3`**.
2. Add a **`sdkconfig.user.*`** file with Kconfig overrides (GPIOs, optional display, etc.).
3. Chain defaults: `sdkconfig.defaults` → `sdkconfig.defaults.m5cores3` → your file (last wins).

**Example `user_platformio.ini`:**

```ini
[env:my-cores3]
extends = env:m5cores3
board_build.cmake_extra_args =
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.m5cores3;sdkconfig.user.mycores3"
```

Then: `pio run -e my-cores3 -t upload`

`sdkconfig.user.*` files are intended to be gitignored so local tweaks stay private.

---

## Setup (first boot)

1. Power the Core S3 over USB‑C (a **data-capable** USB‑C cable is required for flash and [`tools/usb_web`](tools/usb_web/README.md); USB‑C to USB‑C to a full-featured computer port is fine).
2. **Provisioning Wi‑Fi**
   - **A — Captive portal (needs SPIFFS):** On a phone or PC, join Wi‑Fi **`ESP32-AirPlay-Setup`**, then open **http://192.168.4.1** and set a device name and home Wi‑Fi credentials.
   - **B — USB (works even if SPIFFS was never flashed):** Run `python3 tools/usb_web/server.py`, open the printed URL in **Chrome or Edge**, **Connect Device** to the Core S3 serial port, then use the on-page Wi‑Fi tools. See [`tools/usb_web/README.md`](tools/usb_web/README.md). Close **PlatformIO Serial Monitor** before connecting Web Serial (only one program may open the port).
3. After reboot on your LAN, use **AirPlay** from Control Center or any music app.

### Why you might not see `ESP32-AirPlay-Setup`

The firmware uses **STA-first** boot ([`main/network/wifi.c`](main/network/wifi.c)): if valid STA credentials are stored and the device **connects within ~30s**, it stays in **station-only** mode and **does not** start the setup SoftAP. That is expected when the saved network is reachable.

**Ways to enter setup / SoftAP again:** erase flash/NVS and reflash, move the device out of range of the saved SSID until connection fails, clear saved Wi‑Fi from the device web UI (once SPIFFS works) or from the **USB** page, or wait for repeated connection failures until the firmware re-enables the AP (see Wi‑Fi code paths).

If connection fails repeatedly, the device can return to setup mode so you can reconfigure.

---

## USB Web Serial management (no device web files required)

For M5Stack Core S3, the firmware can speak a JSON **control channel** over the USB Serial/JTAG console (`@usbctl` lines). A small local server serves a Chromium **Web Serial** UI:

```bash
python3 tools/usb_web/server.py
# Open http://127.0.0.1:8765/index.html — use Chrome or Edge
```

This path **does not** depend on SPIFFS or on opening the receiver at `http://<device-ip>/` in a browser. The Core S3 may have **two USB‑C jacks**; use the one that enumerates as your usual flash/monitor serial port. See [`tools/usb_web/README.md`](tools/usb_web/README.md) for scope (Wi‑Fi, device name, restart, AirPlay pairing reset, clear Wi‑Fi).

---

## Updating firmware (OTA)

When the device is on your LAN, you can upload a new firmware image from its web UI (diagnostics/OTA features depend on your `sdkconfig`; see project options).

---

## SPIFFS and `data/`

A **`storage`** SPIFFS partition holds the **on-device** web assets under **`data/www/`** (captive portal and status pages). Layout is defined in [`components/boards/partitions.csv`](components/boards/partitions.csv).

**PlatformIO:** after flashing the app, build and upload the filesystem (same `--upload-port` as `upload`):

```bash
pio run -e m5cores3 -t buildfs
pio run -e m5cores3 -t uploadfs --upload-port /dev/cu.usbmodemXXXX
```

Until SPIFFS is written, the HTTP server may return **404** for `/` and logs may show **SPIFFS … 0 bytes used**. Use **[USB Web Serial management](#usb-web-serial-management-no-device-web-files-required)** to configure the device without the on-device pages.

---

## Optional features (menuconfig)

| Feature | Notes |
|--------|--------|
| **OLED display** | Disabled by default on Core S3 (`CONFIG_DISPLAY_ENABLED`). Enable under *AirPlay ESP Configuration* / display options if wired. |
| **W5500 Ethernet** | Optional; enable `CONFIG_ETH_W5500_ENABLED` and set SPI pins under *Board Configuration* if you add a module. |
| **Hardware buttons** | GPIOs default to disabled; configure under *Button Configuration*. AirPlay 2 vs DACP behavior is documented in older upstream docs; volume still applies locally. |

---

## Features

- **AirPlay 2** — discovery, pairing, encrypted audio path where applicable
- **ALAC & AAC** — realtime and buffered playback paths
- **PTP-style timing** — multi-room friendly buffering
- **Web setup** — captive portal and status (as enabled in config)
- **RGB status LED** — Core S3 uses the configured WS2812 GPIO when set

### Limitations

- Audio only (no AirPlay video/screen mirroring)
- **No Bluetooth Classic A2DP** on this chip/target
- One active AirPlay session per device; Wi‑Fi quality affects stability

---

## Technical overview

```
iPhone / iPad / Mac  ── Wi‑Fi ──►  ESP32‑S3 (Core S3)  ──►  AW88298 / internal speaker path
```

Key code areas:

| Area | Path |
|------|------|
| RTSP / AirPlay | `main/rtsp/` |
| HAP pairing | `main/hap/` |
| Audio pipeline | `main/audio/` |
| Board init | `components/boards/m5stack-core-s3/` |
| Output (Core S3) | `main/audio/audio_output_cores3.c` |
| Wi‑Fi / mDNS / web | `main/network/` |

Legacy **DAC** components (`dac_tas57xx`, `dac_tas58xx`) remain in the tree for reference but are **not** selected in the default **m5cores3** build.

---

## Acknowledgements

- **[Shairport Sync](https://github.com/mikebrady/shairport-sync)** — reference AirPlay behavior
- **[openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver)** — Python AirPlay 2 research
- **[Espressif](https://github.com/espressif)** — ESP-IDF and audio codecs
- **[M5Stack](https://m5stack.com/)** — Core S3 hardware and BSP

---

## Legal

**Non-commercial use only.** Commercial use requires explicit permission. See [LICENSE](LICENSE).

Independent project based on protocol research. Not affiliated with Apple Inc. Not guaranteed against future iOS/macOS changes. Provided as-is without warranty.
