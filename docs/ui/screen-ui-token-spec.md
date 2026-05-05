# Screen UI Token Spec

## 颜色 Tokens（来源：`m5-ui/css/style.css`）
| Token | Hex | RGB565(approx) | 用途 |
|---|---|---|---|
| `--bg-app` | `#06070A` | `0x0021` | 背景近黑层 |
| `--accent-primary` | `#FFB800` | `0xFDC0` | 高亮强调色 |
| `--accent-secondary` | `#3B82F6` | `0x3C1E` | 次强调色 |
| `--accent-tertiary` | `#00D1FF` | `0x069F` | 频谱/辉光色 |
| `--text-primary` | `#E2E4EA` | `0xE71D` | 主文本 |
| `--text-secondary` | `#9CA3AF` | `0x9D35` | 次文本 |

## 排版 Tokens
- Lyric font size: 15~16 px（设备侧采用 CJK 主字体 + Montserrat fallback）
- Center alignment for lyric layer

## 动画与节奏 Tokens
- Lyrics switch interval: 3500 ms
- Lyrics transition: 600 ms
- Spectrum animation speed: 0.15
- Particle speed cap: 0.3
- Hue base/swing/speed: 190 / 60 / 0.4

## 布局 Tokens
- Canvas: 320x240
- Spectrum baseY: centerY + 75
- Spectrum bar offset: 130
- Dot row Y: centerY + 45
- Lyric Y: centerY - 10
