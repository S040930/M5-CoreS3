<div align="center">
<img src="docs/logo_airplay_esp32.png" alt="AirPlay ESP32" width="400">

# ESP32 AirPlay 1 Receiver

[![GitHub stars](https://img.shields.io/github/stars/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/network)
[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/hardware-M5Stack_Core_S3-green?style=flat-square)](https://docs.m5stack.com/en/core/CoreS3)

**Stream music from your Apple devices over Wi‑Fi to a M5Stack Core S3 — AirPlay 1, no cloud, no extra app.**

</div>

---

## 项目概述

本项目将 **[M5Stack Core S3](https://docs.m5stack.com/en/core/CoreS3)** (ESP32‑S3) 开发板转变为无线 **AirPlay 1** 音箱。它会像其他 AirPlay 接收器一样出现在 iPhone、iPad 和 Mac 的控制中心中。音频通过开发板的 **内置扬声器路径**（通过官方 BSP / `esp_codec_dev` 驱动的 AW88298 放大器）播放。

本仓库专门维护 **Core S3** 平台：板载 Wi‑Fi、USB‑C 供电和 captive‑portal 配置。由于 ESP32‑S3 没有 Classic BT 控制器，**不使用 Bluetooth Classic / A2DP**。

**无需云服务。无需额外应用。只需点击播放。**

---

## 硬件要求

| 项目 | 说明 |
|------|--------|
| **M5Stack Core S3** | ESP32‑S3 芯片，带 PSRAM，内置扬声器放大器 — [M5 文档](https://docs.m5stack.com/en/core/CoreS3) |
| **USB‑C 线缆** | 用于供电和串行闪存 |

支持的构建版本不需要外部 DAC 或 PCM5102 接线。

---

## 刷写固件

三种选项：**Web 刷写器**（无需安装）、**PlatformIO** 或 **ESP-IDF**。

### 选项 A — Web 刷写器（初学者）

1. 从 [Releases](https://github.com/rbouteiller/airplay-esp32/releases/latest) 页面下载最新的 **`airplay-receiver-m5cores3.bin`** 文件。
2. 打开 [ESP Web Flasher](https://espressif.github.io/esptool-js/)（Chrome 或 Edge 浏览器）。
3. 通过 USB 连接 Core S3，点击 **Connect** → 选择串口。
4. 刷写地址设置为 **`0x0`**，选择下载的 `.bin` 文件，点击 **Program**。
5. 重启开发板；如果未配置，它会进入 Wi‑Fi 设置模式。

### 选项 B — PlatformIO

```bash
pip install platformio
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32

pio run -e m5cores3 -t upload --upload-port /dev/cu.usbmodemXXXX
pio run -e m5cores3 -t buildfs
pio run -e m5cores3 -t uploadfs --upload-port /dev/cu.usbmodemXXXX
pio run -e m5cores3 -t monitor --upload-port /dev/cu.usbmodemXXXX
```

将 `/dev/cu.usbmodemXXXX` 替换为实际的串行设备（macOS：`ls /dev/cu.usb*`；避免使用无关端口，如 `Bluetooth-Incoming-Port`）。在 Windows 上使用 `COMn`。

**固件与 SPIFFS：** `upload` 仅写入应用程序。设备 Web UI 存储在 **`storage`** SPIFFS 分区中，位于 [`data/www/`](data/www/)。如果不执行 **`buildfs` + `uploadfs`**，日志可能会显示 `Failed to open /spiffs/www/index.html` 和 SPIFFS **0 bytes used**。您仍然可以通过 **USB** 使用 [`tools/usb_web/`](tools/usb_web/README.md) 管理 Wi‑Fi（AirPlay 1 不需要配对）。

唯一预定义的环境是 **`m5cores3`**（见 [`platformio.ini`](platformio.ini)）。

### 选项 C — ESP-IDF

```bash
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32
source /path/to/esp-idf/export.sh

idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5cores3" build
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## 配置规则

本仓库只保留两份正式默认配置文件：

- [`sdkconfig.defaults`](sdkconfig.defaults)：跨板级的正式产品默认值
- [`sdkconfig.defaults.m5cores3`](sdkconfig.defaults.m5cores3)：M5Stack CoreS3 的板级默认值

默认构建、发布和文档都只认这两份文件。修改正式产品行为时，请直接更新 `sdkconfig.defaults*`，不要引入额外的 `sdkconfig.<board>`、`sdkconfig.<profile>` 或 `sdkconfig.user.*` 文件作为仓库接口。

`sdkconfig` 自动生成快照可在本地实验时临时出现，但它们不是正式配置源，不应提交，也不应成为 README 或构建流程的一部分。

---

## 首次启动设置

1. 通过 USB‑C 为 Core S3 供电（需要 **支持数据传输** 的 USB‑C 线缆用于刷写和 [`tools/usb_web`](tools/usb_web/README.md)；USB‑C 到 USB‑C 连接到全功能计算机端口即可）。
2. **配置 Wi‑Fi**
   - **A — Captive portal（需要 SPIFFS）：** 在手机或电脑上，连接 Wi‑Fi **`ESP32-AirPlay-Setup`**，然后打开 **http://192.168.4.1** 并设置设备名称和家庭 Wi‑Fi 凭证。
   - **B — USB（即使未刷写 SPIFFS 也可工作）：** 运行 `python3 tools/usb_web/server.py`，在 **Chrome 或 Edge** 中打开打印的 URL，**Connect Device** 到 Core S3 串口，然后使用页面上的 Wi‑Fi 工具。请参阅 [`tools/usb_web/README.md`](tools/usb_web/README.md)。在连接 Web Serial 之前关闭 **PlatformIO Serial Monitor**（只有一个程序可以打开端口）。
3. 在 LAN 上重启后，从控制中心或任何音乐应用程序使用 **AirPlay**。

### 为什么您可能看不到 `ESP32-AirPlay-Setup`

固件使用 **STA-first** 启动（[`main/network/wifi.c`](main/network/wifi.c)）：如果存储了有效的 STA 凭证且设备在 **~30 秒内** 连接，它会保持在 **仅 station** 模式，**不会** 启动设置 SoftAP。当保存的网络可达时，这是预期行为。

**重新进入设置 / SoftAP 的方法：** 擦除闪存/NVS 并重新刷写，将设备移出保存的 SSID 范围直到连接失败，从设备 Web UI（一旦 SPIFFS 工作）或从 **USB** 页面清除保存的 Wi‑Fi，或等待重复连接失败直到固件重新启用 AP（见 Wi‑Fi 代码路径）。

如果连接反复失败，设备可以返回设置模式以便您重新配置。

---

## USB Web Serial 管理（无需设备 Web 文件）

对于 M5Stack Core S3，固件可以通过 USB Serial/JTAG 控制台（`@usbctl` 行）使用 JSON **控制通道**。一个小型本地服务器提供 Chromium **Web Serial** UI：

```bash
python3 tools/usb_web/server.py
# 打开 http://127.0.0.1:8765/index.html — 使用 Chrome 或 Edge
```

此路径 **不** 依赖于 SPIFFS 或在浏览器中打开接收器的 `http://<device-ip>/`。Core S3 可能有 **两个 USB‑C 接口**；使用枚举为通常的刷写/监控串口的接口。请参阅 [`tools/usb_web/README.md`](tools/usb_web/README.md) 了解范围（Wi‑Fi、设备名称、重启、清除 Wi‑Fi）。

---

## 固件更新（OTA）

当设备在您的 LAN 上时，您可以从其 Web UI 上传新的固件映像（诊断/OTA 功能取决于您的 `sdkconfig`；请参阅项目选项）。

---

## SPIFFS 和 `data/`

**`storage`** SPIFFS 分区存储 **设备上** 的 Web 资产，位于 **`data/www/`**（captive portal 和状态页面）。布局在 [`components/boards/partitions.csv`](components/boards/partitions.csv) 中定义。

**PlatformIO：** 刷写应用程序后，构建并上传文件系统（与 `upload` 使用相同的 `--upload-port`）：

```bash
pio run -e m5cores3 -t buildfs
pio run -e m5cores3 -t uploadfs --upload-port /dev/cu.usbmodemXXXX
```

在写入 SPIFFS 之前，HTTP 服务器可能会为 `/` 返回 **404**，日志可能会显示 **SPIFFS … 0 bytes used**。使用 **[USB Web Serial 管理](#usb-web-serial-management-no-device-web-files-required)** 在没有设备页面的情况下配置设备。

---

## 可选功能（menuconfig）

| 功能 | 说明 |
|--------|--------|
| **OLED 显示** | 在 Core S3 上默认禁用 (`CONFIG_DISPLAY_ENABLED`)。如果连接了显示设备，在 *AirPlay ESP Configuration* / 显示选项下启用。 |
| **W5500 以太网** | 可选；如果添加模块，启用 `CONFIG_ETH_W5500_ENABLED` 并在 *Board Configuration* 下设置 SPI 引脚。 |
| **硬件按钮** | GPIO 默认禁用；在 *Button Configuration* 下配置。AirPlay 1 与 DACP 行为在较早的上游文档中有记录；音量仍在本地应用。 |

---

## 功能特性

- **AirPlay 1**（经典 RAOP）— 发现、加密音频路径
- **ALAC & AAC** — 实时播放路径
- **NTP 风格计时** — 多房间友好的同步
- **Web 设置** — captive portal 和状态（按配置启用）
- **RGB 状态 LED** — Core S3 在设置时使用配置的 WS2812 GPIO
- **音频处理** — 包含音频缓冲、解码、重采样和 DSP 处理
- **网络功能** — Wi‑Fi 管理、mDNS 服务发现、Web 服务器
- **USB 控制** — 通过 USB Serial 进行设备管理
- **OTA 更新** — 通过 Web UI 进行固件更新

### 限制

- 仅音频（无 AirPlay 视频/屏幕镜像）
- **此芯片/目标上无 Bluetooth Classic A2DP**
- 每个设备一次只能有一个活动的 AirPlay 会话；Wi‑Fi 质量会影响稳定性

---

## 技术概述

```
iPhone / iPad / Mac  ── Wi‑Fi ──►  ESP32‑S3 (Core S3)  ──►  AW88298 / 内部扬声器路径
```

## 文档索引

| 主题 | 摘要 | 路径 |
|------|------|------|
| 音频质量优化 | 播放路径根本原因模型、实现的 DSP 强化、新诊断以及 SNR/THD/频率响应测试的验证指南 | `docs/audio_quality_optimization.md` |

关键代码区域：

| 区域 | 路径 |
|------|------|
| RTSP / AirPlay | `main/rtsp/` |
| 音频处理 | `main/audio/` |
| 开发板初始化 | `components/boards/m5stack-core-s3/` |
| 输出（Core S3） | `main/audio/audio_output_cores3.c` |
| 网络功能 | `main/network/` |
| USB 控制 | `main/usb_control_service.c` |
| 状态管理 | `main/receiver_state.c` |
| 播放控制 | `main/playback_control.c` |
| 系统操作 | `main/system_actions.c` |

传统 **DAC** 组件（`dac_tas57xx`、`dac_tas58xx`）仍在代码树中作为参考，但在默认 **m5cores3** 构建中 **未** 被选择。

---

## 贡献指南

我们欢迎社区贡献！如果您想为项目做出贡献，请遵循以下步骤：

1. Fork 本仓库
2. 创建您的特性分支 (`git checkout -b feature/amazing-feature`)
3. 提交您的更改 (`git commit -m 'Add some amazing feature'`)
4. 推送到分支 (`git push origin feature/amazing-feature`)
5. 打开一个 Pull Request

请确保您的代码符合项目的代码风格和质量标准。

---

## 致谢

- **[Shairport Sync](https://github.com/mikebrady/shairport-sync)** — AirPlay 行为参考
- **[Espressif](https://github.com/espressif)** — ESP-IDF 和音频编解码器
- **[M5Stack](https://m5stack.com/)** — Core S3 硬件和 BSP

---

## 许可证信息

**仅供非商业使用。** 商业使用需要明确许可。请参阅 [LICENSE](LICENSE)。

基于协议研究的独立项目。与 Apple Inc. 无关。不保证与未来 iOS/macOS 更改兼容。按原样提供，不提供任何保证。
