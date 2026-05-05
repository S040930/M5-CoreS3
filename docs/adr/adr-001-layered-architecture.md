# ADR-001: 五层分层架构与依赖规则

## 状态
Accepted (2026-05-01)

## 背景
重构前的代码库存在多处跨层依赖违规：
- Layer 2 (audio_core) 直接包含 Layer 3 (airplay_core) 头文件
- Layer 2 (network_core) 直接包含 Layer 4 (app_core) 头文件
- Layer 3 (airplay_core) 直接包含 Layer 4 (app_core) 头文件
- realtime_voice 绕过立面直接依赖 5 个子系统

这些问题导致紧耦合、难以测试和维护。

## 决策
采用严格的五层分层架构：

| 层级 | 组件 | 职责 |
|------|------|------|
| Layer 5 | main/ | 硬件上电、一次性初始化 |
| Layer 4 | app_core/ | 应用生命周期编排、状态机 |
| Layer 3 | airplay_core/, realtime_voice/ | 业务协议和功能实现 |
| Layer 2 | audio_core/, screen_ui/, network_core/ | 核心领域逻辑 |
| Layer 1 | board_cores3/ | 硬件抽象 |

### 强制依赖规则
1. **禁止向上依赖**：Layer N 不得 #include Layer N+1 或更高的头文件
2. **禁止跨层跳级**：Layer 3 只能通过 Layer 2 接口访问底层
3. **同层隔离**：同层级组件间不得相互依赖
4. **ESP-IDF 无限制**：所有层均可依赖 esp_* 等 IDF 组件

## 影响
- 所有跨层依赖需通过参数传递或桥接 API 解耦
- 每个组件只暴露单一职责的公共 API
- 新增组件需明确放置层级并遵循规则
