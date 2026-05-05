# Screen UI Maintenance Guide

## 日常调优入口
- 聚合层：`components/screen_ui/screen_ui.c`
- 视觉 token：`components/screen_ui/screen_ui_theme.h`
- 渲染原语：`components/screen_ui/ui_renderer.c`
- 动效模块：`ui_particles.c` / `ui_spectrum.c` / `ui_lyric.c` / `ui_decor.c`

## 变更原则
1. 先改 `screen_ui_theme` 再改模块实现，避免魔法数扩散。
2. 不改变公开 API（`screen_ui.h`）语义边界。
3. 新视觉效果必须在 `docs/ui/screen-ui-mapping-matrix.md` 补充映射关系。

## 回归清单
1. 状态切换文案完整。
2. 歌词轮播与播放状态联动正常。
3. metadata title 只作为歌词 fallback，不出现独立 artist/progress 控件。
4. 频谱、粒子、装饰层未破坏。
