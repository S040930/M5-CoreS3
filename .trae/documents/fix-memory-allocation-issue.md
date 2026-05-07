# 修复内存分配失败问题 - 执行计划

## 问题分析

根据日志：

```
E (11047) esp-aes: Failed to allocate memory 
E (11047) esp-tls-mbedtls: write error :-0x0001 
```

**根本原因**：TLS 加密（AES）在尝试分配内存时失败，导致 websocket 连接断开。

关键观察：
- `Free heap before: 7776332 bytes` - 物理内存还有约 7.4MB
- `activation armed=0` - 语音激活未武装

**可能原因**：
1. **mbedTLS 配置问题** - TLS 需要从内部 RAM 分配内存，但内部 RAM 可能不足
2. **PSRAM 配置问题** - 可能没有正确启用 PSRAM 用于 TLS
3. **内存碎片** - 频繁分配/释放导致内存碎片

---

## 执行步骤

### 步骤 1：检查并修复 mbedTLS 内存配置

修改 `platformio.ini`，添加 TLS 相关配置：

```ini
build_flags =
    ${env.build_flags}
    -DBSP_CONFIG_NO_GRAPHIC_LIB=0
    -DCONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y
    -DCONFIG_LVGL_DOUBLE_BUFFERED=y
    -DCONFIG_LVGL_VDB_SIZE=64
    -DCONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096
    -DCONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
    -DCONFIG_MBEDTLS_SSL_MAX_FRAGMENT_LEN=4096
    -DCONFIG_MBEDTLS_USE_PSRAM=y
```

### 步骤 2：检查 PSRAM 配置

确保 `platformio.ini` 中已经有 PSRAM 配置：

```ini
build_flags =
    -DCONFIG_SPIRAM_SIZE=8388608
    -DCONFIG_SPIRAM_USE=y
    -DCONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
```

### 步骤 3：增加系统内存配置

在 `config/config.toml` 中添加：

```toml
[system.spiram]
spiram_malloc_always_internal = 4096
spiram_use_instead_of_internal = true
```

### 步骤 4：修复 websocket 重连逻辑

检查 `realtime_voice.c` 中的 websocket 重连逻辑，确保正确处理断开后的重连。

### 步骤 5：验证修复

1. 重新构建并烧录
2. 检查日志中是否还有 `Failed to allocate memory` 错误
3. 检查 `activation armed=1` 是否正确显示

---

## 涉及文件清单

| 文件 | 修改内容 |
|------|----------|
| `platformio.ini` | 添加 mbedTLS 和 PSRAM 配置 |
| `config/config.toml` | 添加 SPIRAM 配置 |

---

## 预期结果

修复后日志应显示：
```
I (xxxx) realtime_voice: websocket connected 
I (xxxx) realtime_voice: session.update sent 
I (xxxx) realtime_voice: session ready (session.created)
I (xxxx) realtime_voice: activation armed=1
```