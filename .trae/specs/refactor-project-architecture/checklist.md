# 重构验收检查清单

## 阶段 1：共享基础设施

- [x] `audio_ringbuf.c/h` 存在，提供 init/deinit/push/pop/avail/free/reset API
- [x] `audio_ringbuf_push` 和 `audio_ringbuf_pop` 使用两段 memcpy + portMUX_TYPE 自旋锁
- [x] `voice_playout.c` 内部使用 `audio_ringbuf_t` 实例，保留 last_stereo 和 prefill 逻辑
- [x] `voice_reference.c` 内部使用 `audio_ringbuf_t` 实例，不再有逐样本拷贝循环
- [x] `voice_common.c/h` 存在，提供 voice_buf_alloc/voice_buf_free/voice_now_ms/voice_hw_codec_rate_hz/voice_hw_mclk_multiple
- [x] `voice_dsp.c` 不再包含 voice_buf_alloc/voice_buf_free/voice_hw_codec_rate_hz/voice_hw_mclk_multiple 定义
- [x] `realtime_voice.c` 和 `voice_frontend.c` 不再有本地 `now_ms()` 定义，改用 `voice_now_ms()`
- [x] `resampler_bridge.c` 已删除，resampler 通过 CMakeLists.txt 正确链接
- [x] `voice_request.c` 不再有 `#define VOICE_REQUEST_MODEL`，改从 config 读取模型名
- [ ] 阶段 1 全量编译零错误

## 阶段 2：拆分上帝模块

- [x] `voice_speaker.c/h` 存在，包含 speaker_acquire/release/volume_boost/restore/spk_open/close/workbufs
- [x] `voice_playout_drain.c/h` 存在，包含 playout_drain_to_speaker 及其所有子逻辑
- [x] `voice_ws.c/h` 存在，包含 ws_event_handler/disconnect/send_json/handle_realtime_message/session_update
- [x] `voice_session.c/h` 存在，包含会话状态管理、录音逻辑、VAD 判断、oneshot 流程
- [x] `realtime_voice.c` 行数 < 500 行，仅包含 voice_task 主循环和公共 API — **490 行**
- [x] 工具函数 JSON Schema（voice_append_session_tools）已移入 `voice_tools.c`
- [x] `realtime_voice.c` 不再包含 cJSON Schema 构建代码
- [ ] 阶段 2 全量编译零错误

## 阶段 3：资源仲裁器

- [x] `resource_manager.c/h` 存在于 `components/audio_core/resource/`
- [x] resource_manager 提供 acquire_speaker/release_speaker/set_airplay_active/set_voice_active/get_state/register_callback API — **NOTE: API 名称略有差异（`resource_manager_acquire/release` 而非 `acquire_speaker/release_speaker`），`set_voice_active` 未单独提供但通过 acquire/release 功能覆盖**
- [x] `voice_speaker.c` 通过 resource_manager 请求/释放扬声器
- [x] `airplay_service.c` 通过 resource_manager 注册 AirPlay 播放状态
- [x] `app_core.c` 初始化 resource_manager 并注册回调
- [ ] 阶段 3 全量编译零错误

## 阶段 4：跨层依赖清理

- [x] `realtime_voice/CMakeLists.txt` REQUIRES 不包含 `airplay_core`
- [x] `realtime_voice/CMakeLists.txt` REQUIRES 不包含 `screen_ui`
- [x] `realtime_voice.c` 不再 #include "airplay_service.h"
- [x] `realtime_voice.c` 不再 #include "screen_ui.h"
- [x] `voice_tools.c` 不再直接 #include "airplay_service.h"、"wifi.h"、"receiver_state.h"
- [ ] AirPlay + 语音互斥功能正常（通过回调桥接）
- [ ] UI 状态显示正常（通过回调桥接）
- [ ] 阶段 4 全量编译零错误

## 阶段 5：全量验证

- [ ] ninja 全量编译零错误零警告
- [ ] firmware.elf 生成成功
- [ ] 无内存泄漏（voice_buf_alloc/voice_buf_free 配对正确）
- [ ] 无死代码（未使用的 static 函数已移除）
- [x] 无层级违规（CMakeLists.txt 依赖关系符合五层架构） — **voice_tools.c 不再 #include "bsp/display.h"；realtime_voice/ 下所有 .c/.h 文件无跨层 include**
