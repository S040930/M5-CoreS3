# 修复崩溃问题和屏幕闪烁问题 - 执行计划

## 问题概述

### 问题 1：崩溃 (LoadProhibited)
- 现象：设备启动后，`realtime_voice_start()` 打印配置后崩溃
- PC=0x420fe27b，EXCVADDR=0x00000010（访问 NULL 指针偏移 16 字节）
- 崩溃时间线：`realtime_voice_start()` → `xTaskCreate(voice_task)` → `voice_task` 开始执行 → 崩溃

### 问题 2：屏幕每 10 秒闪烁
- 现象：设备启动后，屏幕每约 10 秒闪一次
- 可能原因：LVGL 单缓冲模式 + 周期性全屏刷新

---

## 执行步骤

### 步骤 1：启用崩溃调试信息（定位准确崩溃位置）

修改 `platformio.ini`，在 `build_flags` 中添加：

```ini
build_flags =
    ${env.build_flags}
    -DCONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y
    -DCONFIG_ESP_SYSTEM_PANIC_GDBSTUB=n
    -DCONFIG_ESP_DEBUG_OCDAWARE=y
```

这样崩溃时会打印完整的 backtrace 信息和寄存器状态，方便定位具体函数。

---

### 步骤 2：分析 voice_task 入口处的初始化路径，修复 NULL 指针

检查 `voice_task` (realtime_voice.c 行 1926 起) 的初始化路径：

```
voice_task() → 
  voice_rs_cap_ensure()        # 可能返回 NULL
  realtime_voice_reset_session()
  set_state(INITIALIZING, NULL) → screen_ui_set_voice_state()
  maybe_create_voice_asr()  
  set_voice_ui_idle()
```

**可能的修复点 A：`screen_ui_set_voice_state()`**

在 `screen_ui.c` 中，`screen_ui_set_voice_state()` 在调用 `screen_ui_update_labels()` 前需要确认 UI 对象已创建：

```c
void screen_ui_set_voice_state(screen_ui_voice_state_t state, ...) {
  s_ui.voice_state = state;
  if (!s_ui.initialized) return;
  if (!s_ui.active_screen) return;    // ← 添加：确认屏幕对象已创建
  if (!bsp_display_lock(pdMS_TO_TICKS(80))) return;
  screen_ui_update_labels();
  bsp_display_unlock();
}
```

**可能的修复点 B：`voice_task` 中 buffer 分配后的空指针检查**

在 `voice_task` 中，`voice_rs_cap_ensure()` 和 buffer 分配操作后添加空指针检查。

---

### 步骤 3：修复屏幕闪烁 - 启用双缓冲

修改 `components/screen_ui/screen_ui.c`，启用 LVGL 双缓冲：

```c
// 修改前 (第 79 行)：
.double_buffer = false,

// 修改后：
.double_buffer = true,
```

同时确保 PSRAM 有足够空间分配第二个缓冲区。

---

### 步骤 4：修复屏幕闪烁 - 增加动画刷新间隔

修改 `components/screen_ui/screen_ui.c`，增加动画定时器的刷新间隔：

```c
// 修改前：
#define CONFIG_SCREEN_UI_ANIM_INTERVAL_MS 33

// 修改后：
#define CONFIG_SCREEN_UI_ANIM_INTERVAL_MS 100
```

---

### 步骤 5：修复屏幕闪烁 - 检查 LVGL 缓冲区大小

在 `platformio.ini` 中添加 LVGL 配置：

```ini
build_flags =
    # ... 现有配置 ...
    -DCONFIG_LVGL_DOUBLE_BUFFERED=y
    -DCONFIG_LVGL_VDB_SIZE=64
```

---

### 步骤 6：验证修复

1. 重新构建并烧录：
   ```bash
   pio run -e m5cores3 -t upload
   ```

2. 检查崩溃是否已修复（设备不再重启）

3. 检查屏幕是否不再闪烁

4. 检查串口日志中是否还有错误

---

## 备选方案

如果步骤 2 无法修复崩溃，可以**暂时禁用 voice_task 的屏幕更新逻辑**来验证：

```c
// 在 voice_task 中，注释掉状态相关的 UI 更新调用
// set_state(REALTIME_VOICE_STATE_INITIALIZING, NULL);
// set_voice_ui_idle();
```

如果禁用后不再崩溃，说明崩溃确实在 UI 更新路径中。

---

## 涉及文件清单

| 文件 | 修改内容 |
|------|----------|
| `components/screen_ui/screen_ui.c` | 启用双缓冲、增加刷新间隔 |
| `components/screen_ui/screen_ui.h` | 无需修改 |
| `platformio.ini` | 添加调试 flag 和 LVGL 配置 |
