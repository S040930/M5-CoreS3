# M5 Core S3 自动亮度调节方案

## Summary

利用 M5 Core S3 板载 LTR-553ALS-WA 环境光传感器，实现屏幕亮度自动调节。创建独立的 `auto_brightness` 组件，包含 LTR-553 裸 I2C 驱动和周期轮询 + 线性映射亮度调节策略。默认开启，无开关。

## Current State Analysis

**硬件**：M5 Core S3 板载 LTR-553ALS-WA（I2C 地址 0x23），支持环境光（ALS）和接近检测（PS）。

**当前亮度控制**：
- `bsp_display_brightness_set(int brightness_percent)` 通过 AXP2101 DLDO1 电压控制，范围 0-100%
- `voice_tools` 中的 `set_screen_brightness` 工具支持手动设置
- **无自动亮度调节代码**
- **无 LTR-553 驱动代码**

**I2C 总线**：BSP 已初始化 I2C 总线（`bsp_i2c_init()`），AXP2101（0x34）和 AW9523（0x5B）已注册。LTR-553（0x23）在同一总线上，可直接添加设备。

**BSP I2C 接口**：`m5stack_core_s3.c` 中 `bsp_i2c_init()` 使用 `i2c_master_bus_add_device()` 添加设备。外部组件需要获取 I2C 总线句柄或自行添加设备。查看 BSP 头文件发现 `bsp_i2c_init()` 返回 `esp_err_t`，I2C 句柄是 static 的。需要确认是否可以外部获取。

## Proposed Changes

### 1. 创建 `components/auto_brightness/` 组件

**新文件**：
- `ltr553.h` — LTR-553ALS-WA 驱动头文件
- `ltr553.c` — LTR-553ALS-WA 裸 I2C 驱动实现
- `auto_brightness.h` — 自动亮度调节接口
- `auto_brightness.c` — 自动亮度调节逻辑
- `CMakeLists.txt` — 组件编译配置
- `idf_component.yml` — 组件依赖

### 2. `ltr553.h` / `ltr553.c` — LTR-553 ALS 驱动

**LTR-553 关键寄存器**（I2C 地址 0x23）：
- `0x80` — ALS_CONTR (ALS 使能/增益/积分时间)
- `0x81` — PS_CONTR (PS 使能，本项目不用)
- `0x85` — ALS_DATA_CH1_0 (ALS 通道 1 低字节)
- `0x86` — ALS_DATA_CH1_1 (ALS 通道 1 高字节)
- `0x87` — ALS_DATA_CH0_0 (ALS 通道 0 低字节)
- `0x88` — ALS_DATA_CH0_1 (ALS 通道 0 高字节)
- `0x8A` — ALS_PS_STATUS (状态寄存器)

**接口设计**：
```c
esp_err_t ltr553_init(i2c_master_bus_handle_t bus);
esp_err_t ltr553_deinit(void);
esp_err_t ltr553_read_als(uint16_t *ch0, uint16_t *ch1);
esp_err_t ltr553_als_enable(bool enable);
```

**实现要点**：
- 使用 `i2c_master_bus_add_device()` 添加 LTR-553 到现有 I2C 总线
- ALS 使能：写 `0x80` 寄存器，设置 gain=1x, integration_time=100ms
- 读取 ALS 数据：读 `0x85-0x88` 寄存器，组合为 16-bit CH0/CH1 值
- CH0 和 CH1 的比值可用于判断光源类型，简化版可直接用 CH0 作为亮度参考

### 3. `auto_brightness.h` / `auto_brightness.c` — 自动亮度调节

**接口设计**：
```c
esp_err_t auto_brightness_start(i2c_master_bus_handle_t i2c_bus,
                                 esp_err_t (*set_brightness_fn)(int));
void auto_brightness_stop(void);
```

**实现要点**：
- 创建一个 FreeRTOS 任务，每 2 秒读取一次 ALS 数据
- lux → brightness% 映射使用分段线性函数：
  - 0-10 lux → 10-30%（暗室/夜间）
  - 10-100 lux → 30-60%（室内正常光）
  - 100-500 lux → 60-85%（明亮室内）
  - 500-1000+ lux → 85-100%（直射阳光/室外）
- 平滑处理：亮度变化超过 5% 才实际调节，避免频繁 I2C 写入
- 调用 `set_brightness_fn`（即 `bsp_display_brightness_set`）设置亮度
- 当用户通过 `set_screen_brightness` 工具手动设置亮度时，auto_brightness 暂停 30 秒后恢复

### 4. `CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "ltr553.c" "auto_brightness.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c
)
```

### 5. `idf_component.yml`

无外部依赖，仅依赖 ESP-IDF 内置的 I2C 驱动。

### 6. 修改 `components/app_core/app_core.c`

- 在 `app_core_init()` 中调用 `auto_brightness_start()`
- 需要获取 I2C 总线句柄。BSP 的 I2C 句柄是 static 的，需要添加一个获取函数，或者让 `auto_brightness` 自行初始化 I2C 设备

**I2C 总线句柄问题**：BSP 中 `i2c_handle` 是 static 变量。解决方案：
- 方案 A：在 BSP 中添加 `bsp_i2c_get_bus_handle()` 函数
- 方案 B：`auto_brightness` 使用 `I2C_NUM_0` 自行创建总线设备（ESP-IDF 5.x 支持同一总线上添加多个设备）

选择方案 B：使用 `i2c_master_bus_add_device()` 在已有总线上添加设备，只需知道 I2C 端口号。但 ESP-IDF 5.x 的新 I2C Master API 需要总线句柄。

实际方案：在 `board_common.h` 中添加 `bsp_i2c_get_bus_handle()` 声明，在 `m5stack_core_s3.c` 中实现，返回 `i2c_handle`。

### 7. 修改 `components/realtime_voice/voice_tools.c`

当 `set_screen_brightness` 工具被调用时，通知 `auto_brightness` 暂停 30 秒：
- 添加 `auto_brightness_notify_manual_override()` 函数
- 在 `set_screen_brightness` dispatch 中调用

### 8. lux 计算说明

LTR-553 的 ALS 数据不是直接 lux 值。CH0 和 CH1 的组合用于计算 lux：
- 简化公式：`lux = (CH0 * 0.8) - (CH1 * 0.2)`（近似值，增益和积分时间归一化后）
- 更精确的公式需要根据 LTR-553 数据手册中的 lux 计算表

对于自动亮度调节，不需要精确的 lux 值，可以直接使用 CH0 原始值作为"亮度等级"参考，配合实验调整映射表。

## Assumptions & Decisions

1. **裸 I2C 驱动**：不依赖第三方组件，直接读写 LTR-553 寄存器
2. **周期轮询 + 线性映射**：每 2 秒读取 ALS，分段线性映射到亮度百分比
3. **默认开启无开关**：自动亮度始终生效，手动设置亮度时暂停 30 秒后自动恢复
4. **新组件 auto_brightness**：模块化设计，独立于 realtime_voice
5. **BSP I2C 句柄**：通过新增 `bsp_i2c_get_bus_handle()` 获取
6. **不使用 PS（接近检测）**：仅使用 ALS 功能
7. **平滑处理**：亮度变化超过 5% 才实际调节

## Verification Steps

1. **编译验证**：`~/.platformio/penv/bin/pio run -e m5cores3`
2. **I2C 扫描验证**：启动后日志应显示 LTR-553 在 0x23 地址被检测到
3. **ALS 数据验证**：日志中应显示周期性的 CH0/CH1 读数
4. **亮度调节验证**：
   - 遮挡传感器 → 亮度降低
   - 手电筒照射 → 亮度升高
   - 手动设置亮度 → 自动调节暂停 30 秒
5. **与 voice_tools 集成**：说"屏幕亮一点"后自动亮度暂停，30 秒后恢复
