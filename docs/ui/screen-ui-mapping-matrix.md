# Screen UI Mapping Matrix

## 组件映射
| Source (`m5-ui`) | Target (`components/screen_ui`) | 允许偏差 | 不可偏离项 |
|---|---|---|---|
| `CanvasRenderer` | `ui_renderer` | 抗锯齿/像素栅格差异 | 320x240 画布、RGB565 输出、基础原语一致 |
| `ParticleSystem` | `ui_particles` | 粒子分布随机差异 | 粒子数量级、速度区间、alpha 脉冲行为 |
| `SpectrumAnalyzer` | `ui_spectrum` | 单帧振幅微差 | 双侧对称结构、柱宽/柱距、渐变+高亮 |
| `LyricDisplay` | `ui_lyric` | 字体 fallback 差异 | 3.5s 切换周期、入出场过渡、中心对齐 |
| `DecorativeElements` | `ui_decor` | 低亮度层细节微差 | 四角 bracket、底部点阵、扫描线节奏 |
| `UIManager` | `screen_ui` | 定时器 jitter | 单一聚合循环、统一状态输入模型 |

## 层级顺序
1. 背景清屏
2. 粒子层
3. 频谱层
4. 歌词层
5. 装饰层
6. 无额外 overlay 层（不绘制 status/artist/progress 控件）
