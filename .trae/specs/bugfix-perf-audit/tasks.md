# Tasks

- [x] Task 1: voice_playout.c — 添加 portMUX_TYPE 自旋锁 + 两段 memcpy
  - [x] SubTask 1.1: 添加 `static portMUX_TYPE s_playout_spin` 全局变量
  - [x] SubTask 1.2: voice_playout_push — 在 portENTER_CRITICAL/portEXIT_CRITICAL 内使用两段 memcpy 替代逐样本循环
  - [x] SubTask 1.3: voice_playout_pop — 在 portENTER_CRITICAL/portEXIT_CRITICAL 内使用两段 memcpy 替代逐样本循环
  - [x] SubTask 1.4: voice_playout_reset — 在锁内重置 s_w/s_r
  - [x] SubTask 1.5: 编译验证

- [x] Task 2: voice_request.c — line_buf 动态分配 + 降采样低通滤波
  - [x] SubTask 2.1: stream_ctx_t 中将 `char line_buf[8192]` 改为 `char *line_buf` + `size_t line_buf_cap`
  - [x] SubTask 2.2: voice_request_send_audio 中用 voice_buf_alloc 分配 line_buf，函数末尾 voice_buf_free
  - [x] SubTask 2.3: http_event_handler 中将 `sizeof(ctx->line_buf)` 改为 `ctx->line_buf_cap`
  - [x] SubTask 2.4: downsample_16k_to_8k 改用 3 点加权平均 FIR 低通（边界处理：首尾样本用 2 点加权）
  - [x] SubTask 2.5: 编译验证

- [x] Task 3: realtime_voice.c — playout_workbufs_ensure 原子性修复
  - [x] SubTask 3.1: 修改 playout_workbufs_ensure：先 alloc 新缓冲区到临时变量，成功后再 free 旧缓冲区并赋值；失败则保留旧缓冲区
  - [x] SubTask 3.2: 编译验证

- [x] Task 4: 全量编译 + 烧录验证
  - [x] SubTask 4.1: ninja 全量编译，确认零错误
  - [x] SubTask 4.2: 检查 firmware.elf 生成成功

# Task Dependencies
- Task 2 依赖 Task 1（无依赖，可并行）
- Task 3 依赖 Task 1（无依赖，可并行）
- Task 4 依赖 Task 1, Task 2, Task 3
