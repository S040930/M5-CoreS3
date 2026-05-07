# 语音功能重做 - 产品需求文档

## Overview
- **Summary**: 对现有项目中问题频发的语音功能进行完全重做，构建一个稳定、清晰、易于维护的语音助手系统。
- **Purpose**: 解决当前语音功能的稳定性问题，简化架构，提高可维护性，确保语音唤醒、录音、请求和播放整个链路的可靠性。
- **Target Users**: 项目维护者和最终用户，包括使用M5 CoreS3设备的开发者和消费者。

## Goals
- 完全重写语音功能模块，移除所有历史遗留的不稳定代码
- 建立清晰、层次分明的架构，每个模块职责单一
- 确保语音唤醒、录音、HTTP请求、音频播放的完整链路稳定运行
- 提高代码可读性和可维护性，便于后续修改和调试
- 保持与现有AirPlay功能的兼容性
- 维持现有API接口不变，最小化对调用方的影响

## Non-Goals (Out of Scope)
- 不添加新的语音功能特性（如新的唤醒词、多语言支持等）
- 不改变硬件配置和底层音频驱动
- 不重构AirPlay相关功能
- 不添加新的依赖库
- 不改变项目的整体架构层次

## Background & Context
当前语音功能存在以下问题：
1. 代码复杂且分散，经过多次修复后变得难以维护
2. 多个模块之间职责不清，存在耦合
3. 缺乏清晰的状态管理
4. 错误处理不够健壮
5. 从PROJECT.md可以看到有大量的历史修复记录，说明问题持续存在

现有语音功能的核心流程：
1. 本地WakeNet唤醒（Hi ESP）
2. 录音
3. 通过HTTP请求发送到DashScope API
4. 接收并播放音频回复
5. AirPlay互斥控制

## Functional Requirements
- **FR-1**: 语音唤醒功能 - 使用本地WakeNet模型"Hi ESP"进行唤醒检测
- **FR-2**: 音频录制 - 唤醒后自动开始录音，检测到静音时自动停止
- **FR-3**: API请求 - 将录音发送到DashScope OpenAI兼容API并处理流式响应
- **FR-4**: 音频播放 - 接收并播放API返回的音频回复
- **FR-5**: AirPlay互斥 - AirPlay活跃时暂停语音功能，AirPlay停止后恢复
- **FR-6**: 系统播报 - 提供独立的文本转语音播报接口，不进入对话模式
- **FR-7**: UI状态通知 - 向UI组件通知语音状态变化

## Non-Functional Requirements
- **NFR-1**: 稳定性 - 语音功能应能连续运行数小时不崩溃或死锁
- **NFR-2**: 内存安全 - 无内存泄漏，无悬空指针，无堆破坏
- **NFR-3**: 响应性 - 从唤醒到开始录音的延迟 < 500ms
- **NFR-4**: 可测试性 - 每个模块应有清晰的接口，便于单元测试
- **NFR-5**: 可观测性 - 完善的日志系统，便于问题定位

## Constraints
- **Technical**: 
  - 必须使用ESP-IDF框架
  - 保持与现有audio_core组件的兼容性
  - 使用现有的配置系统
  - 继续使用DashScope API
- **Business**: 
  - 保持现有功能完整
  - 最小化对用户体验的改变
- **Dependencies**: 
  - ESP-SR (WakeNet, AFE)
  - cJSON
  - mbedtls (base64)
  - esp_http_client

## Assumptions
- 现有硬件配置和音频驱动是正确的
- DashScope API接口保持不变
- AirPlay互斥逻辑的基本需求保持不变
- 配置系统不需要重大修改

## Acceptance Criteria

### AC-1: 语音唤醒功能正常
- **Given**: 系统已启动且语音功能已初始化
- **When**: 用户说"Hi ESP"
- **Then**: 系统应检测到唤醒并进入录音状态，状态更新通知UI
- **Verification**: programmatic
- **Notes**: 验证唤醒词检测的准确性和响应速度

### AC-2: 录音功能正常
- **Given**: 系统已进入录音状态
- **When**: 用户说话然后停止
- **Then**: 系统应录制音频，在检测到静音后自动停止并进入请求状态
- **Verification**: programmatic
- **Notes**: 验证录音质量和静音检测的可靠性

### AC-3: API请求和响应处理正常
- **Given**: 录音已完成
- **When**: 系统发送请求到DashScope
- **Then**: 应成功接收响应，处理流式音频和文本数据
- **Verification**: programmatic
- **Notes**: 验证网络错误处理和重试机制

### AC-4: 音频播放功能正常
- **Given**: 已接收到音频响应
- **When**: 开始播放
- **Then**: 音频应流畅播放，无卡顿或爆音，播放完成后回到唤醒等待状态
- **Verification**: human-judgment + programmatic
- **Notes**: 主观听感测试和播放状态验证

### AC-5: AirPlay互斥功能正常
- **Given**: 语音功能正在运行
- **When**: AirPlay开始播放
- **Then**: 语音功能应暂停，AirPlay停止后语音功能应恢复
- **Verification**: programmatic

### AC-6: 系统播报功能正常
- **Given**: 系统空闲
- **When**: 调用realtime_voice_speak_text()
- **Then**: 应播报指定文本，不进入对话模式
- **Verification**: programmatic

### AC-7: 稳定性测试通过
- **Given**: 系统正常运行
- **When**: 进行20次完整的语音交互循环
- **Then**: 无崩溃、无死锁、无内存泄漏
- **Verification**: programmatic

## Open Questions
- 是否需要调整唤醒词检测的灵敏度？（暂时保持现有配置）
- 是否需要调整录音时长限制？（暂时保持现有配置）
- 是否需要添加更多的诊断日志？（重做过程中会添加必要的日志）
