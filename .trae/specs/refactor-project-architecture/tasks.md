# Tasks

## 阶段 1：提取共享基础设施（风险最低）

- [x] Task 1: 创建 `audio_ringbuf` 共享环形缓冲区模块
  - [x] SubTask 1.1: 新建 `components/realtime_voice/audio_ringbuf.c` + `audio_ringbuf.h`
  - [x] SubTask 1.2: 实现 `audio_ringbuf_init/deinit/push/pop/avail/free/reset`，使用两段 memcpy + portMUX_TYPE 自旋锁
  - [x] SubTask 1.3: 修改 `voice_playout.c` — 内部使用 `audio_ringbuf_t` 实例，保留 last_stereo 缓存和 prefill 逻辑
  - [x] SubTask 1.4: 修改 `voice_reference.c` — 内部使用 `audio_ringbuf_t` 实例，消除逐样本拷贝
  - [x] SubTask 1.5: 修改 `CMakeLists.txt` — 添加 `audio_ringbuf.c`
  - [x] SubTask 1.6: 编译验证

- [x] Task 2: 创建 `voice_common` 共享基础设施模块
  - [x] SubTask 2.1: 新建 `components/realtime_voice/voice_common.c` + `voice_common.h`
  - [x] SubTask 2.2: 从 `voice_dsp.c` 移出 `voice_buf_alloc`/`voice_buf_free`/`voice_hw_codec_rate_hz`/`voice_hw_mclk_multiple` 到 `voice_common`
  - [x] SubTask 2.3: 添加 `voice_now_ms()` 统一时间戳函数
  - [x] SubTask 2.4: 替换 `realtime_voice.c` 和 `voice_frontend.c` 中的 `now_ms()` 为 `voice_now_ms()`
  - [x] SubTask 2.5: 修改 `CMakeLists.txt` — 添加 `voice_common.c`
  - [x] SubTask 2.6: 编译验证

- [x] Task 3: 消除 `resampler_bridge.c` 反模式
  - [x] SubTask 3.1: 删除 `resampler_bridge.c`
  - [x] SubTask 3.2: 修改 `CMakeLists.txt` — 在 SRCS 中直接添加 `../audio_core/resampler/resampler.c`
  - [x] SubTask 3.3: 编译验证

- [x] Task 4: `voice_request.c` 模型名参数化
  - [x] SubTask 4.1: 移除 `#define VOICE_REQUEST_MODEL`，改为从 `voice_request_config_t.model` 读取
  - [x] SubTask 4.2: 更新 `realtime_voice.c` 中 `voice_request_config_t` 初始化，填入模型名
  - [x] SubTask 4.3: 编译验证

## 阶段 2：拆分 realtime_voice.c 上帝模块（中等风险）

- [x] Task 5: 提取 `voice_speaker.c` — 扬声器仲裁与音量管理
  - [x] SubTask 5.1: 新建 `voice_speaker.c` + `voice_speaker.h`
  - [x] SubTask 5.2: 移入 `speaker_acquire`/`speaker_release`/`speaker_volume_boost`/`speaker_volume_restore`/`spk_open`/`spk_close`
  - [x] SubTask 5.3: 移入 `playout_workbufs_ensure`/`playout_workbufs_release`
  - [x] SubTask 5.4: 移入 `soft_limit_f32`/`soft_clip_i16`/`voice_peak_abs_i16`
  - [x] SubTask 5.5: 修改 `realtime_voice.c` — 调用 `voice_speaker_*` API
  - [x] SubTask 5.6: 编译验证

- [x] Task 6: 提取 `voice_playout_drain.c` — 播放 drain 逻辑
  - [x] SubTask 6.1: 新建 `voice_playout_drain.c` + `voice_playout_drain.h`
  - [x] SubTask 6.2: 移入 `playout_drain_to_speaker` 函数（含重采样、软限幅、单声道→立体声、间隙掩盖、诊断日志）
  - [x] SubTask 6.3: 修改 `realtime_voice.c` — 调用 `voice_playout_drain()` API
  - [x] SubTask 6.4: 编译验证

- [x] Task 7: 提取 `voice_ws.c` — WebSocket 连接与消息处理
  - [x] SubTask 7.1: 新建 `voice_ws.c` + `voice_ws.h`
  - [x] SubTask 7.2: 移入 `ws_event_handler`/`ws_disconnect`/`ws_send_json`/`ws_retry_reset`
  - [x] SubTask 7.3: 移入 `send_session_update`/`send_response_create`/`send_conversation_function_output`
  - [x] SubTask 7.4: 移入 `log_ws_disconnect_diag`/`record_ws_send_type`
  - [x] SubTask 7.5: 移入 ws_accum 缓冲区管理
  - [x] SubTask 7.6: 修改 `realtime_voice.c` — 调用 `voice_ws_*` API
  - [x] SubTask 7.7: 编译验证

- [x] Task 8: 提取 `voice_session.c` — 会话 arm 管理
  - [x] SubTask 8.1: 新建 `voice_session.c` + `voice_session.h`
  - [x] SubTask 8.2: 移入 `session_arm_set`/`session_arm_get`
  - [x] SubTask 8.3: 修改 `realtime_voice.c` — 调用 `voice_session_*` API
  - [x] SubTask 8.4: 编译验证

- [x] Task 9: 将工具函数 JSON Schema 移入 `voice_tools.c`
  - [x] SubTask 9.1: 将 `voice_append_session_tools()` 从 `voice_ws.c` 移入 `voice_tools.c`，重命名为 `voice_tools_append_session_schemas`
  - [x] SubTask 9.2: 在 `voice_tools.h` 中声明新函数
  - [x] SubTask 9.3: 修改 `voice_ws.c` 中的调用
  - [x] SubTask 9.4: 编译验证

## 阶段 3：统一资源仲裁器（中等风险，最高影响）

- [x] Task 10: 创建 `resource_manager` 模块
  - [x] SubTask 10.1: 新建 `components/audio_core/resource/resource_manager.c` + `resource_manager.h`
  - [x] SubTask 10.2: 实现 `resource_manager_init`/`acquire`/`release`/`set_airplay_active`/`get_owner`/`register_callback`
  - [x] SubTask 10.3: 修改 `components/audio_core/CMakeLists.txt` — 添加新源文件
  - [x] SubTask 10.4: 编译验证

- [x] Task 11: 集成 resource_manager 到现有组件
  - [x] SubTask 11.1: 修改 `voice_speaker.c` — 通过 `resource_manager` 请求/释放扬声器
  - [x] SubTask 11.2: 修改 `airplay_service.c` — 通过 `resource_manager` 注册 AirPlay 播放状态
  - [x] SubTask 11.3: 修改 `app_core.c` — 初始化 `resource_manager`，注册回调
  - [x] SubTask 11.4: 编译验证

## 阶段 4：清理跨层依赖（中等风险）

- [x] Task 12: 解耦 realtime_voice 对 network_core (receiver_state) 的直接依赖
  - [x] SubTask 12.1: 在 `realtime_voice.h` 中添加回调注册接口：`realtime_voice_set_network_query_cb`
  - [x] SubTask 12.2: 替换 `realtime_voice.c` 中对 `receiver_state.h` 的直接调用为回调
  - [x] SubTask 12.3: 从 `realtime_voice/CMakeLists.txt` REQUIRES 中移除 `network_core`
  - [x] SubTask 12.4: 编译验证

- [x] Task 13: 解耦 voice_timers 和 realtime_voice 对 screen_ui 的直接依赖
  - [x] SubTask 13.1: 在 `voice_timers.h` 中添加 UI 通知回调注册接口
  - [x] SubTask 13.2: 替换 `voice_timers.c` 中对 `screen_ui.h` 的直接调用为回调
  - [x] SubTask 13.3: 在 `realtime_voice.h` 中添加 UI 状态回调，替换 `realtime_voice.c` 中所有 screen_ui 调用
  - [x] SubTask 13.4: 从 `realtime_voice/CMakeLists.txt` REQUIRES 中移除 `screen_ui`
  - [x] SubTask 13.5: 在 `app_core.c` 中注册回调
  - [x] SubTask 13.6: 编译验证

- [x] Task 14: 解耦 voice_tools 对多组件的直接依赖（已完成 — 使用注入函数模式）
  - [x] SubTask 14.1: 在 `voice_tools.h` 中添加注册接口，允许注入设备状态查询函数
  - [x] SubTask 14.2: 替换 `voice_tools.c` 中对 `airplay_service`/`wifi`/`receiver_state` 的直接调用为注入的查询函数
  - [x] SubTask 14.3: 在 `app_core.c` 中注册查询函数
  - [x] SubTask 14.4: 编译验证

## 阶段 5：全量验证

- [ ] Task 15: 全量编译 + 功能验证（需在 ESP32 环境中执行）
  - [ ] SubTask 15.1: 全量编译，确认零错误
  - [ ] SubTask 15.2: 代码审查 — 确认无内存泄漏、无死代码、无层级违规

# Task Dependencies

- Task 11 依赖 Task 10（resource_manager 已创建）
- Task 12, Task 13 可并行
- Task 15 依赖 Task 11, 12, 13
