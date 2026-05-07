# 音频转写功能实现计划

## 目标
添加用户语音内容的转写功能，在日志中显示用户实际说的话，便于调试和了解发送了什么请求。

## 现状分析
1. **当前流程**：
   - 唤醒后录音
   - 将音频直接发送到 Qwen-Omni API
   - 只记录 AI 的回复内容，不记录用户说的话

2. **Qwen-Omni 能力**：
   - 支持多模态输入（音频）
   - 能识别 113 种语言和方言的语音
   - 响应中应该包含语音转写结果

## 实现方案

### 方案概述
利用 Qwen-Omni API 的能力，在响应中提取用户语音的转写文本，并记录到日志中。

### 修改文件列表
1. `/Users/mac/Desktop/airplay/components/realtime_voice/voice_request.h`
2. `/Users/mac/Desktop/airplay/components/realtime_voice/voice_request.c`
3. `/Users/mac/Desktop/airplay/components/realtime_voice/realtime_voice.c`

## 详细步骤

### 步骤 1: 扩展结果结构体
在 `voice_request.h` 中，修改 `voice_request_result_t` 结构体，添加用户语音转写字段。

### 步骤 2: 修改请求构建
在 `voice_request.c` 中的 `build_request_body` 函数，优化请求消息结构，更明确地提示模型返回语音转写结果。

### 步骤 3: 解析响应获取转写
在 `voice_request.c` 中，添加从 SSE 流式响应或完整响应中提取用户语音转写的逻辑。

### 步骤 4: 记录日志和显示
在 `realtime_voice.c` 中，接收并在日志中打印用户语音的转写内容，同时也可以在屏幕 UI 中显示。

## 关键考虑
1. **API 响应格式**：需要了解 Qwen-Omni 返回的完整响应结构
2. **错误处理**：如果转写失败或模型没有提供转写，要有降级处理
3. **性能影响**：确保转写过程不影响现有功能的性能

## 风险应对
1. **API 返回格式变化**：处理响应时使用安全的空值检查
2. **转写失败**：如果无法提取转写，只记录 "无法获取转写" 日志

## 预期结果
- 日志中会显示：`用户语音转写: "您今天的天气怎么样？"`
- 屏幕 UI 上也能看到用户说的话
- 调试时更清楚知道发送了什么内容给 API
