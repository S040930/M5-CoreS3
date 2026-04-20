<div align="center">
<img src="docs/logo_airplay_esp32.png" alt="AirPlay ESP32" width="400">

# CoreS3 AirPlay Core

[![GitHub stars](https://img.shields.io/github/stars/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/network)
[![License](https://img.shields.io/badge/license-Non--Commercial-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/hardware-M5Stack_Core_S3-green?style=flat-square)](https://docs.m5stack.com/en/core/CoreS3)

**M5Stack CoreS3 only. AirPlay 1 playback core only. No cloud, no display UI, no USB/HTTP management surface.**

</div>

---

## 项目概述

本仓库已经收口为单一产品线：**M5Stack CoreS3 AirPlay 1 接收器**。默认构建只保留下列能力：

- 启动编排与最小状态机
- NVS 设置读取与音量持久化
- STA Wi‑Fi
- mDNS / RTSP / AirPlay 1 服务
- 音频解码、缓冲、重采样与 CoreS3 板载扬声器输出
- CoreS3 板级初始化

默认产品**不再包含**触屏配网、USB Web Serial 管理、HTTP 管理页、SPIFFS Web 资产、OTA、诊断流、SoftAP、以太网、蓝牙、外部 DAC、按钮/灯效控制。

---

## 硬件要求

| 项目 | 说明 |
|------|------|
| **M5Stack Core S3** | ESP32‑S3 芯片，带 PSRAM，内置扬声器放大器 — [M5 文档](https://docs.m5stack.com/en/core/CoreS3) |
| **USB‑C 线缆** | 用于供电和串行刷写 |

默认构建不需要外部 DAC、PCM5102A、显示屏或额外控制外设。

---

## 刷写固件

推荐直接使用 **PlatformIO** 或 **ESP-IDF**。

### PlatformIO

```bash
pip install platformio
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32

pio run -e m5cores3 -t upload --upload-port /dev/cu.usbmodemXXXX
pio run -e m5cores3 -t monitor --upload-port /dev/cu.usbmodemXXXX
```

唯一预定义环境是 **`m5cores3`**。默认分区表是单应用布局，不需要 `buildfs` 或 `uploadfs`。

### ESP-IDF

```bash
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32
source /path/to/esp-idf/export.sh

idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5cores3" build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

---

## 配置与凭证规则

仓库默认只认这两个正式配置源：

- [`sdkconfig.defaults`](sdkconfig.defaults)：跨板级的正式产品默认值
- [`sdkconfig.defaults.m5cores3`](sdkconfig.defaults.m5cores3)：M5Stack CoreS3 的板级默认值

Wi‑Fi 凭证视为**开发者预置**数据，不由设备上的 UI、SoftAP、USB/HTTP 管理面负责输入。

如果设备启动时没有凭证，固件会：

1. 记录“缺少凭证”
2. 进入 `CONFIG_REQUIRED` / idle 状态
3. 不启动旧的屏幕、USB、HTTP、OTA、SoftAP 回退路径

---

## 启动行为

1. 有凭证时，设备启动后连接 STA Wi‑Fi、发布 `_raop._tcp`、接受 AirPlay 会话并从 CoreS3 板载扬声器输出。
2. 无凭证时，设备保持 idle，不启动任何旧管理面。
3. 断网后，最小网络监视器会停止 AirPlay 服务；恢复联网后重新发布服务。

---

## 功能特性

- **AirPlay 1**（经典 RAOP）— 发现、加密音频路径
- **ALAC & AAC** — 实时播放路径
- **NTP 风格计时** — 多房间友好的同步
- **音频处理** — 包含音频缓冲、解码、重采样和 DSP 处理
- **网络功能** — STA Wi‑Fi、mDNS 服务发现、socket/NTP 辅助

### 限制

- 仅音频（无 AirPlay 视频/屏幕镜像）
- 无设备端 Wi‑Fi 配网页面或 USB/HTTP 管理通道
- 无 OTA、SPIFFS Web UI、以太网、蓝牙、外部 DAC 路径
- 每个设备一次只能有一个活动的 AirPlay 会话；Wi‑Fi 质量会影响稳定性

---

## 技术概述

```text
iPhone / iPad / Mac  ── Wi‑Fi ──►  ESP32‑S3 (Core S3)  ──►  AW88298 / 内部扬声器路径
```

## 文档索引

| 主题 | 摘要 | 路径 |
|------|------|------|
| 核心架构 | 新组件边界、启动流程、保留/移除能力 | `docs/core-architecture.md` |
| 构建与验收 | 构建、快速检查、lint 与本地验证步骤 | `docs/build-and-verify.md` |
| 音频听感 QA | 人耳与运行时指标联合验收方法 | `docs/audio-fidelity-qa.md` |

关键代码区域：

| 区域 | 路径 |
|------|------|
| 启动与设置 | `components/app_core/` |
| RTSP / AirPlay | `components/airplay_core/` |
| 音频处理 | `components/audio_core/` |
| 网络功能 | `components/network_core/` |
| CoreS3 板级支持 | `components/board_cores3/` |
| 程序入口 | `main/main.c` |

---

## 许可证信息

**仅供非商业使用。** 商业使用需要明确许可。请参阅 [LICENSE](LICENSE)。

基于协议研究的独立项目。与 Apple Inc. 无关。不保证与未来 iOS/macOS 更改兼容。按原样提供，不提供任何保证。
