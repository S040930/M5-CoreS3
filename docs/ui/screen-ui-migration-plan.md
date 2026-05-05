# Screen UI Migration Plan

## 目标
将 `m5-ui` 的 `canvas#m5-display` 屏幕视觉与交互节奏迁移到 `components/screen_ui`，在 CoreS3 320x240 设备侧实现高一致性还原。
不迁移 `index.html` 的网站壳层（控制面板、代码编辑器、设备边框、背景光效与页级排版）。

## 分阶段执行
1. P0 核心屏幕
- 打通统一渲染循环，完成粒子、频谱、歌词、装饰层叠加。
- 保障 `BOOT / READY / STREAMING / FAULT` 状态文案可见。

2. P1 状态与歌词映射
- 状态输入映射到歌词 fallback 文案（`READY/CONNECTING/ERROR` 等）。
- metadata 仅作为 title fallback 来源，不单独显示 artist/progress overlay。

3. P2 视觉精调
- 呼吸光、扫描线、歌词切换过渡细调。
- 色相动态范围与更新速度对齐 `m5-ui`。

## 实施约束
- 仅复用并增强现有 `screen_ui` 模块，不新增第三方依赖。
- 不改部署、密钥、环境配置。
- 不扩展业务语义，仅补全既有 `set_lyrics` / `set_playing` 的实现。
