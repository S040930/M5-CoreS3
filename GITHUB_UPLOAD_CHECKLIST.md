# GitHub 上传清单 - M5 Core S3 AirPlay v1 项目

项目完整扫描结果，列举所有应上传至 GitHub 的源代码、配置、文档和脚本文件。

---

## 应上传的文件（COMMIT）

### 1. 根目录配置文件 (14 个文件)

| 文件名 | 说明 |
|--------|------|
| `.clang-format` | C/C++ 代码格式化规则 |
| `.clang-tidy` | 静态代码分析配置 |
| `.clangd` | Clangd LSP 服务配置 |
| `.gitignore` | Git 忽略规则 |
| `.gitmodules` | Git 子模块定义（u8g2, u8g2-hal-esp-idf） |
| `CMakeLists.txt` | 顶层 CMake 项目文件 |
| `LICENSE` | 许可证文件 |
| `README.md` | 项目说明文档 |
| `platformio.ini` | PlatformIO 工程与环境配置 |
| `version.txt` | 项目版本号 |
| `dependencies.lock` | ESP-IDF 依赖锁文件 |
| `sdkconfig.defaults` | 基础 SDK 默认配置 |
| `sdkconfig.defaults.m5cores3` | M5 Core S3 专用默认配置 |
| `sdkconfig.esp32s3` | ESP32-S3 通用配置示例 |

### 2. 主应用源代码 (main/) — ~80 个文件

#### 主目录 (main/ 顶层)
```
main/
├── CMakeLists.txt
├── Kconfig.projbuild
├── idf_component.yml
├── *.c / *.h (所有源文件)
│   ├── main.c                      # 入口点
│   ├── airplay_service.{c,h}       # AirPlay 服务核心
│   ├── audio_pipeline.{c,h}        # 音频流水线
│   ├── buttons.{c,h}               # 按键驱动
│   ├── dacp_client.{c,h}           # DACP 客户端
│   ├── led.{c,h}                   # LED 控制
│   ├── playback_control.{c,h}      # 播放控制
│   ├── receiver_state.{c,h}        # 接收器状态机
│   ├── settings.{c,h}              # 系统设置存储
│   ├── status_service.{c,h}        # 状态服务
│   ├── system_actions.{c,h}        # 系统动作
│   ├── usb_control_service.{c,h}   # USB 控制服务
│   ├── alac_magic_cookie.{c,h}     # ALAC 解码支持
│   └── spiram_task.h               # 外部 RAM 任务头
```

#### 音频子系统 (main/audio/) — 23 个文件
```
main/audio/
├── audio_decoder.{c,h}             # 音频解码
├── audio_receiver.{c,h}            # AirPlay 音频接收
├── audio_stream.{c,h}              # 音频流管理
├── audio_buffer.{c,h}              # 音频缓冲区
├── audio_output_*.{c,h}            # 输出驱动（Cores3 等）
├── audio_timing.{c,h}              # 时间同步
├── audio_resample.{c,h}            # 重采样
├── audio_crypto.{c,h}              # 音频加密
├── eq_events.{c,h}                 # EQ 事件处理
└── （其他音频相关文件）
```

#### 网络子系统 (main/network/) — 20 个文件
```
main/network/
├── wifi.{c,h}                      # WiFi 管理
├── mdns_airplay.{c,h}              # mDNS 发现
├── ntp_clock.{c,h}                 # NTP 时间同步
├── web_server.{c,h}                # Web 服务器
├── dns_server.{c,h}                # DNS 服务器
├── socket_utils.{c,h}              # Socket 工具
├── log_stream.{c,h}                # 日志流
├── ota.{c,h}                       # OTA 升级
├── ethernet.{c,h}                  # 以太网支持
└── （其他网络模块）
```

#### RTSP 协议 (main/rtsp/) — 12 个文件
```
main/rtsp/
├── rtsp_server.{c,h}               # RTSP 服务核心
├── rtsp_conn.{c,h}                 # RTSP 连接管理
├── rtsp_handlers.{c,h}             # RTSP 命令处理
├── rtsp_message.{c,h}              # RTSP 消息解析
├── rtsp_rsa.{c,h}                  # RSA 密钥交换
├── rtsp_events.{c,h}               # RTSP 事件
└── （其他 RTSP 相关文件）
```

#### Plist 解析 (main/plist/) — 5 个文件
```
main/plist/
├── plist.h                         # Plist 头声明
├── plist_xml.c                     # XML Plist 实现
├── bplist_parser.c                 # 二进制 Plist 解析
├── bplist_builder.c                # 二进制 Plist 构建
└── base64.{c,h}                    # Base64 编解码
```

### 3. 可复用组件 (components/) — ~90 个文件

#### audio-resampler/
```
├── CMakeLists.txt
├── license.txt
├── resampler.c / resampler.h
```

#### board_utils/
```
├── CMakeLists.txt
├── board_utils.c / board_utils.h
```

#### boards/ (多板级支持)
```
├── CMakeLists.txt
├── Kconfig.projbuild
├── idf_component.yml
├── board_common.c / board_common.h
├── partitions.csv / partitions-4m.csv
└── 子目录:
    ├── esp32-generic/
    ├── esp32s3-generic/
    ├── m5stack-core-s3/        # M5 Core S3 特定代码
    └── squeezeamp/
```

#### dac/ (通用 DAC 框架)
```
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── dac.c
└── include/dac.h
```

#### dac_tas57xx/ (TAS57xx DAC 驱动)
```
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── dac_tas57xx.{c,h}
├── slau577a.pdf                 # Datasheet
├── tas5754m.pdf                 # Datasheet
└── hybridflows/
    └── hybridflow_convert_cfg.py
```

#### dac_tas58xx/ (TAS58xx DAC 驱动)
```
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── dac_tas58xx.{c,h}
├── dac_tas58xx_eq.h
├── dac_tas58xx_eq_data.h
└── （Datasheet 和其他文件）
```

#### display/ (OLED 显示驱动)
```
├── CMakeLists.txt
├── display.c
├── display_stub.c
└── include/display.h
```

#### spiffs_storage/ (文件系统)
```
├── CMakeLists.txt
├── spiffs_storage.c / spiffs_storage.h
```

#### u8g2/ (占位符，子模块)
```
└── 目录存在，指向 GitHub olikraus/u8g2 仓库
```

#### u8g2-hal-esp-idf/ (u8g2 ESP-IDF HAL 层)
```
├── CMakeLists.txt
├── include/u8g2_esp32_hal.h
└── src/u8g2_esp32_hal.c
```

### 4. Web 资源和数据 (data/)

```
data/www/
├── index.html                  # 主页面
├── eq.html                     # EQ 调节页面
├── logs.html                   # 日志查看
├── core.html                   # 核心控制
```

### 5. 文档 (docs/) — 6 个文件

```
docs/
├── ESP32_PCM_side.png
├── ESP_PCM_back.png
├── ESP_PCM_front.png
├── PCM5102A.png
├── boite esp32.stl             # 3D 外壳设计
└── logo_airplay_esp32.png
```

### 6. 脚本和工具 (scripts/, tools/)

```
scripts/
├── check-fast.sh               # 快速检查脚本
├── format.sh                   # 代码格式化
├── lint.sh                     # 代码静态检查
└── gen_eq_tables.py            # EQ 表生成

tools/
├── provision_wifi.sh           # WiFi 配置脚本
├── agent_serial_debug_bridge.py # 串口调试桥接
└── usb_web/
    ├── README.md
    ├── index.html
    └── server.py
```

### 7. GitHub 工作流和钩子

```
.github/
└── workflows/
    └── ci-release.yml          # CI/CD 工作流配置

.githooks/                       # 提交钩子脚本
├── pre-commit
├── commit-msg
└── （其他钩子）
```

---

## 应忽略的文件（DO NOT COMMIT）

根据 `.gitignore` 规则，以下内容 **不上传**：

| 目录/文件 | 说明 |
|-----------|------|
| `.pio/` | PlatformIO 本地缓存和依赖副本 |
| `.vscode/` | VS Code 个人工作区设置 |
| `.cache/` | 随机缓存文件 |
| `.claude/` | Claude 相关缓存 |
| `managed_components/` | ESP-IDF 自动管理的第三方组件 |
| `build*/` | 所有构建输出目录 |
| `cleanup_reports/` | 临时清理报告 |
| `sdkconfig` | 动态生成的运行时配置（git 追踪 defaults，不追踪生成值） |
| `user_platformio.ini` | 用户个人 PlatformIO 覆盖配置 |
| `*.o`, `*.a`, `*.so` | 编译中间产物 |
| `__pycache__/` | Python 字节码缓存 |
| `.DS_Store` | macOS 元数据 |

---

## 上传执行清单

### 前置检查
- [ ] 确认 `.gitignore` 规则已正确定义（已完成）
- [ ] 验证 `components/u8g2/` 包含真实源码（从 git submodule 初始化）
- [ ] 检查 `.git` 目录是否存在（或需要新建仓库）
- [ ] 确认没有本地机密（API Key, WiFi 密码等）在源文件里

### 执行步骤

#### 方案 A: 现有 Git 仓库（已初始化）
```bash
cd /Users/mac/Desktop/airplay
git status                  # 验证当前状态
git add .                   # 暂存所有改动
git commit -m "Initial commit: M5 Core S3 AirPlay v1 - Full project"
git branch -M main          # 重命名为 main（如需要）
git push -u origin main     # 推送到远端
```

#### 方案 B: 新建 GitHub 仓库
```bash
cd /Users/mac/Desktop/airplay

# 初始化本地仓库（如果还没有 .git）
git init

# 配置远端
git remote add origin https://github.com/<your-username>/<repo-name>.git

# 初始提交
git add .
git commit -m "Initial commit: M5 Core S3 AirPlay v1 - Full project implementation"

# 首次推送
git branch -M main
git push -u origin main
```

### 上传后验证
- [ ] 确认所有源文件已推送（主要关健文件可见）
- [ ] 验证子模块 `components/u8g2` 已正确配置
- [ ] 检查 `.github/workflows` 中的 CI 流程是否自动触发
- [ ] 查看 GitHub Release 是否自动生成（若有 CI 配置）

---

## 文件统计摘要

| 区域 | 文件数 | 备注 |
|-----|--------|------|
| 根目录配置 | 14 | 包括 CMake、PlatformIO、sdkconfig 等 |
| 主应用源码 (main/) | 80+ | 包含 audio/, network/, rtsp/, plist/ 子系统 |
| 可复用组件 (components/) | 90+ | 包含 DAC 驱动、显示、存储等 |
| Web 资源 (data/) | 4 | HTML 文件 |
| 文档 (docs/) | 6 | 图片和 3D 设计文件 |
| 脚本和工具 | 7+ | 构建、配置和调试脚本 |
| GitHub 工作流和钩子 | 5+ | CI/CD 和开发流程 |
| **总计** | **~210+ 文件** | 完整的 AirPlay 实现 |

---

## 备注

1. **子模块依赖**: 项目依赖于 Git 子模块（`components/u8g2` 和 `u8g2-hal-esp-idf`）。  
   克隆时需使用：`git clone --recursive <repo-url>`

2. **PSRAM 配置**: 最近修改了 `sdkconfig.defaults.esp32s3` 以适配无 PSRAM 的 ESP32-S3 通用板  
   （已禁用 `CONFIG_SPIRAM` 和 `CONFIG_SPIRAM_MODE_OCT`）

3. **依赖 Lock 文件**: `dependencies.lock` 记录了 ESP-IDF 依赖的精确版本，应与源代码一起提交

4. **板级适配**: 支持多种硬件（M5 Core S3、SqueezeAMP、Esparagus 等），各有相应 sdkconfig 默认配置

5. **许可证**: 项目使用 LICENSE 文件（建议查看具体内容）

---

**生成时间**: 2026-04-19  
**文档位置**: `/Users/mac/Desktop/airplay/GITHUB_UPLOAD_CHECKLIST.md`
