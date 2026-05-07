# 代码审查：Bug 修复与性能优化 Spec

## Why
项目经过多轮功能迭代后，存在实际可触发的 bug（环形缓冲区数据竞争、降采样混叠、栈空间紧张）和可测量的性能瓶颈（逐样本拷贝、8KB 栈上缓冲区），需要系统性修复。

## What Changes
- 修复 voice_playout 环形缓冲区跨上下文数据竞争（加自旋锁）
- 修复 voice_playout push/pop 逐样本拷贝性能问题（改用两段 memcpy）
- 修复 voice_request 降采样缺少低通滤波导致混叠（改用加权平均）
- 修复 voice_request 8KB line_buf 占用栈空间过多（改为动态分配）
- 修复 playout_workbufs_ensure 部分分配失败时内存泄漏（先 free 旧再 alloc 新，但 alloc 失败时旧指针已丢失）

## Impact
- Affected code: `components/realtime_voice/voice_playout.c`, `components/realtime_voice/voice_request.c`, `components/realtime_voice/realtime_voice.c`

## ADDED Requirements

### Requirement: 环形缓冲区并发安全
voice_playout 的 push/pop/avail/free 操作 SHALL 使用 portMUX_TYPE 自旋锁保护，与 voice_reference.c 保持一致。

#### Scenario: HTTP 回调与 voice_task 并发访问
- **WHEN** oneshot_audio_cb（HTTP 回调上下文）调用 voice_playout_push，同时 playout_drain_to_speaker（voice_task 上下文）调用 voice_playout_pop
- **THEN** 两个操作互斥执行，不会出现数据竞争导致音频爆音

### Requirement: 环形缓冲区批量拷贝
voice_playout_push 和 voice_playout_pop SHALL 使用两段 memcpy 替代逐样本循环拷贝。

#### Scenario: 大块数据写入
- **WHEN** 推入 240 个样本（10ms@24kHz）
- **THEN** 使用至多 2 次 memcpy 完成拷贝（回绕前 + 回绕后），而非 240 次逐样本赋值

### Requirement: 降采样低通滤波
downsample_16k_to_8k SHALL 使用 3 点加权平均（简易 FIR 低通）替代简单两点平均，抑制 4kHz 以上混叠。

#### Scenario: 高频残留输入
- **WHEN** AFE 输出的 16kHz PCM 中包含 4-8kHz 频率分量
- **THEN** 降采样后混叠分量被衰减，不影响语音识别准确率

### Requirement: line_buf 动态分配
voice_request 的 SSE 行缓冲区 SHALL 从 PSRAM 动态分配，不再占用任务栈空间。

#### Scenario: 20KB 栈任务执行 HTTP 请求
- **WHEN** voice_task（20KB 栈）执行 voice_request_send_audio
- **THEN** line_buf（8KB）从 PSRAM 分配，不占用栈空间，避免栈溢出风险

### Requirement: workbufs 原子性分配
playout_workbufs_ensure SHALL 在重新分配某个缓冲区失败时，保留旧缓冲区指针，避免内存泄漏。

#### Scenario: 中间分配失败
- **WHEN** pop_buf 重新分配成功，但 hw_buf 重新分配失败
- **THEN** pop_buf 使用新缓冲区，hw_buf 保留旧缓冲区（不丢失指针），函数返回 false

## MODIFIED Requirements

### Requirement: voice_playout_push 返回值
push 操作在缓冲区满时 SHALL 记录 overflow 警告日志并返回实际推入数量（当前行为保留，但需在自旋锁内完成）。

## REMOVED Requirements
（无）
