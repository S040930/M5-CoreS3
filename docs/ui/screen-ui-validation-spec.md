# Screen UI Validation Spec

## 视觉一致性
1. 在 `BOOT / READY / STREAMING / FAULT` 分别截图，与 `m5-ui` 对照。
2. 校验项：歌词位置、频谱对称、粒子密度、装饰元素位置、主题色偏移。

## 行为一致性
1. 调用 `screen_ui_set_playing(true/false)` 后，频谱与歌词动画应同步激活/衰减。
2. 调用 `screen_ui_set_lyrics(lines, count)` 后应按 3.5s 轮播，`NULL/0` 回退到 metadata 驱动。
3. 调用 `screen_ui_set_metadata` 后，仅 title 作为歌词 fallback 生效；不应出现 artist/progress overlay。

## 性能与稳定性
1. 连续运行 30 分钟无崩溃、无明显闪烁、无残影。
2. UI 渲染不影响音频主链路稳定性。
3. 高频 metadata 更新（1Hz）下无卡顿与文本异常。

## 分辨率与可读性
1. 320x240 实机为主验收。
2. 不同亮度下中心歌词与频谱高光可读。
3. 长文本滚动时无截断错乱。
