# ADR-003: 上下文结构体模式替代全局变量

## 状态
Proposed (2026-05-01)

## 背景
代码库中存在 45+ 处分散的静态全局可变状态，分布在 16+ 个 .c 文件中。这种模式：
- 导致测试困难（需要 mock 全局状态）
- 存在多任务竞态条件风险
- 降低代码可读性

## 决策
对包含 3+ 全局变量的模块，引入上下文结构体模式：

```c
// 定义上下文
typedef struct {
    int field1;
    float field2;
} module_ctx_t;

// 单一静态实例
static module_ctx_t s_ctx = {0};

// 通过 s_ctx.field1 访问
```

### 目标模块
- audio_output_common (10 个变量 → audio_out_ctx_t)
- audio_pipeline (5 个 → audio_pipeline_ctx_t)
- realtime_voice (26 个字段 → 已有 realtime_ctx_t)
- rtsp_server (16 个 → rtsp_server_ctx_t)
- wifi (8 个 → net_wifi_ctx_t)
- ntp_clock (6 个 → ntp_clock_ctx_t)

## 影响
- 每个模块独立重构，降低风险
- 结构体内存布局更清晰，便于调试
- 方便将来扩展到多实例场景
