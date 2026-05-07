# 添加温度/湿度 Voice Tool

## 问题分析

用户问"室内温度多少"，AI 回复"无法获取室内温度"。原因有二：

1. **硬件连接错误**：ENV.3 Unit 是 I2C 设备（SHT30@0x44 + QMP6988@0x70），但用户插在 PORT.B（UART 端口）。CoreS3 的 Grove 端口定义：
   - PORT.A（红色）= I2C → GPIO 12 (SDA) + GPIO 11 (SCL) ← **应插这里**
   - PORT.B（蓝色）= UART → 不是 I2C
   - PORT.C（黑色）= GPIO
   日志 `i2c.master: I2C transaction unexpected nack detected` + `env_monitor: sensor init retry pending` 正是因为 I2C 设备不在 I2C 总线上。

2. **缺少 voice tool**：`env_monitor` 组件已有完整的 SHT30/QMP6988 读取代码，但只用于季节性提醒（温度过高时主动播报），没有暴露为 voice tool，AI 模型不知道设备有温度传感器。

## 修改计划

### 1. 暴露 env_monitor 的传感器数据读取接口

**文件**: `components/env_monitor/env_monitor.h`

添加一个公共函数，供 voice_tools 调用读取最新温湿度：

```c
bool env_monitor_get_latest(float *temp_c, float *humidity_pct, float *pressure_kpa);
```

**文件**: `components/env_monitor/env_monitor.c`

- 添加 `static float s_latest_temp_c`, `s_latest_humidity_pct`, `s_latest_pressure_kpa` 静态变量
- 在 `env_sample()` 成功后更新这些变量
- 实现 `env_monitor_get_latest()` 读取这些变量
- 当 `s_hw.valid == false` 时返回 `false`

### 2. 添加 `get_temperature` voice tool

**文件**: `components/realtime_voice/voice_tools.c`

- 添加 `#include "env_monitor.h"`
- 在 `voice_tools_dispatch()` 中添加 `get_temperature` 分支：
  - 调用 `env_monitor_get_latest()` 获取 temp/humidity/pressure
  - 返回 JSON: `{"ok":true,"temperature_c":25.3,"humidity_pct":45.2,"pressure_kpa":101.3}` 或 `{"ok":false,"error":"sensor not available"}`
- 在 `voice_tools_append_session_schemas()` 中添加 `get_temperature` 的 function schema：
  - name: `get_temperature`
  - description: "Read the current indoor temperature, humidity, and atmospheric pressure from the onboard ENV sensor."
  - parameters: 无参数（空 object）

### 3. 更新系统提示词

**文件**: `components/realtime_voice/voice_request.c`

在 `build_session_instructions()` 的 suffix 中添加 `get_temperature`：

```
" You may call tools when needed: set_timer and cancel_timer (relative delays only, not "
"wall-clock alarms); get_time; get_date; "
"set_volume; get_volume; get_temperature. Use set_volume for louder/quieter/mute. "
"get_temperature reads indoor temp/humidity/pressure. After a tool runs, reply "
"briefly in voice."
```

### 4. 更新 CMakeLists 依赖

**文件**: `components/realtime_voice/CMakeLists.txt`

添加 `env_monitor` 到 REQUIRES 列表（如果尚未添加）。

## 不做的事

- 不修改 env_monitor 的 I2C 初始化逻辑——硬件连接错误需要用户将传感器从 PORT.B 换到 PORT.A
- 不添加新的 Kconfig 选项——get_temperature 工具跟随 `CONFIG_VOICE_TOOLS_ENABLE` 和 `CONFIG_ENV_MONITOR_ENABLE` 开关

## 验证步骤

1. 编译通过：`pio run -e m5cores3`
2. 用户将 ENV.3 Unit 从 PORT.B 换到 PORT.A
3. 烧录后检查串口日志：
   - 应看到 `ENV.3 sensor ready (SHT30 + QMP6988 addr=0x70)` 而不是 `sensor init retry pending`
   - 应看到 `sample: season=... temp=XX.XC humidity=XX.X%` 定期输出
4. 语音测试："现在室内温度多少" → AI 应调用 `get_temperature` 工具并播报温度
