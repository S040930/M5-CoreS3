# ADR-004: 统一错误处理策略

## 状态
Proposed (2026-05-01)

## 背景
代码库中错误处理不一致：
- 部分函数忽略 `esp_err_t` 返回值（静默失败）
- 部分函数使用 `ESP_ERROR_CHECK()` 在生产环境 abort
- 日志级别不统一（ESP_LOGE vs ESP_LOGW）

## 决策

### 错误传播模式
```c
esp_err_t err = some_function(ctx, &result);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "operation failed: %s (code=0x%x)", 
             esp_err_to_name(err), err);
    return err;
}
```

### 错误码体系
- 模块级错误码：0x1000-0x1FFF（audio），0x2000-0x2FFF（network），0x3000-0x3FFF（rtsp）
- 使用 `esp_err_to_name()` 统一错误信息格式
- 禁止在生产代码中使用 `ESP_ERROR_CHECK`

### 日志级别规范
| 级别 | 使用场景 |
|------|----------|
| ESP_LOGE | 不可恢复错误、硬件故障 |
| ESP_LOGW | 可恢复异常、降级路径触发 |
| ESP_LOGI | 正常状态转换、配置变更 |
| ESP_LOGD | 调试细节、性能数据 |

## 影响
- 所有公共 API 返回 `esp_err_t` 并附带日志
- 编译期启用 `-Wunused-result` 强制检查
- 新增 `audio_errors.h` 定义音频域错误码
