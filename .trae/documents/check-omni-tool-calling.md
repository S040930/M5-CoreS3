# 检查 Omni 模型本地工具调用

## 结论

**Qwen3.5-Omni-Flash 支持 Function Calling**，DashScope 官方文档明确列出"千问Omni：Qwen3.5-Omni-Flash系列"支持工具调用。

代码中的工具调用链路完整且逻辑正确：

1. **注册**：`voice_tools_append_session_schemas()` 向请求体添加 `tools` 数组 ✅
2. **解析**：`stream_handle_sse_line()` 从 SSE delta 中解析 `tool_calls`（id/name/arguments 增量拼接）✅
3. **检测**：`finish_reason == "tool_calls"` 判断是否需要执行工具 ✅
4. **执行**：`dispatch_tool_calls_and_followup()` 调用 `voice_tools_dispatch()` 执行本地工具 ✅
5. **回传**：`build_tool_followup_body()` 构建 assistant tool_calls + tool result 消息，发起新请求 ✅
6. **递归**：支持多轮工具调用 ✅

## 之前的问题（已修复）

上次对话中系统提示词仍列出已删除的 5 个工具（`get_device_status`、`get_network_status`、`set_screen_brightness`、`play_local_chime`、`airplay_status`），已在上次修改中更新为只列出当前 6 个有效工具。

## 可能的剩余问题

### 1. 流式模式下 tool_calls 的增量拼接

OpenAI 兼容 API 在流式模式下，`tool_calls` 的 `arguments` 是增量返回的（每个 delta 只包含部分 JSON 字符串），需要拼接。当前代码已正确处理增量拼接（L276-L283）。

但有一个边界问题：**`id` 字段在增量 delta 中可能只在第一个 chunk 出现**，后续 chunk 的 `id` 为 null。当前代码在 `id` 为 null 时不更新，这是正确的。但 `already_counted` 的判断依赖 `id` 字段已填充——如果第一个 chunk 中 `id` 还没出现（极端情况），可能导致重复计数。

**风险等级**：低。DashScope 兼容模式通常在第一个 delta 就提供 `id`。

### 2. `tool_call_id` 为空字符串时的去重

当 `ctx->tool_calls[idx].id` 为空字符串（`\0`）时，`already_counted` 的 `strcmp` 会匹配到其他空 id 的条目，导致误判为"已计数"。这意味着如果多个 tool_call 的 id 都还没被填充，只有第一个会被记录。

**风险等级**：低。实际场景中 DashScope 总是在第一个 delta 提供 id。

### 3. `VOICE_REQUEST_TOOL_CALL_MAX = 4` 限制

最多支持 4 个并行工具调用。对于当前 6 个工具的场景，4 个并行调用已足够。

### 4. `VOICE_REQUEST_TOOL_ARGS_MAX = 512` 限制

工具参数 JSON 最大 512 字节。`set_timer` 的参数较小，不会超限。

### 5. `VOICE_REQUEST_TOOL_OUTPUT_MAX = 896` 限制

工具输出最大 896 字节。当前工具的输出都在此范围内。

## 验证方法

无需代码修改。烧录后通过串口日志验证：

1. 说 "10 秒后提醒我上厕所"
2. 查看串口日志：
   - `tool_calls detected: count=1` — 模型返回了工具调用
   - `tool_dispatch: name=set_timer output=...` — 本地工具执行成功
   - `tool_dispatch: name=... output=...` — 如果有后续工具调用
3. 10 秒后应收到定时器提醒

如果日志中没有 `tool_calls detected`，说明模型没有选择调用工具。可能原因：
- 系统提示词不够明确引导工具调用
- 音频转文字后模型理解不准确
- 模型选择直接回答而非调用工具

如果日志中有 `tool_calls detected` 但 `tool_dispatch` 报错，说明本地执行有问题。
