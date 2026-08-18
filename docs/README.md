# M5StopWatch FMO 通联伴侣

基于 M5StopWatch C152（ESP32-S3 + 1.54" 466×466 AMOLED）的 **FMO 通联伴侣** 固件。

系统分两端：**手表端（watch）** 采集/播放音频 + 显示 UI；**接收端（receiver）** 接收 ESP-NOW 语音 → PCM5102A DAC → FMO 电台发射。

## 功能

- **FMO 通联**：WebSocket 接收音频/呼号/台站信息（台站 30s 定时同步）
- **中文字体**：思源黑体 24px 显示呼号、台站、城市地址（LittleFS → PSRAM）
- **离线呼号查询**：呼号 → 城市/省份（FNV-1a 哈希查表，13686 呼号）
- **语音发射**：按住 A 键 → 全双工 I2S 采集麦克风 → ESP-NOW → 接收端 → FMO
- **语音接收端**：ESP32-S3 + GY-PCM5102 DAC → 电台 AIN（重采样 + 噪声门降噪 + OLED 状态显示）
- **发射界面**：radio-voice-tx 设计稿（同心圆环 + 呼吸光环 + 均衡器 + 计时器）
- **天气**：ipip.net 定位 + Open-Meteo 天气（WMO 代码，30 分钟刷新）
- **WiFi 记忆库**：自动保存/重连已连接 WiFi
- **息屏省电**：A+B 长按 3 秒息屏 + 呼号摩斯码动画

## 硬件

### 手表端（watch）
| 硬件 | 详情 |
|------|------|
| 主控 | ESP32-S3，8MB PSRAM |
| 屏幕 | 1.54" AMOLED 466×466 |
| 音频 | ES8311 Codec（全双工 I2S0 TX+RX）|
| 按键 | A=GPIO2，B=GPIO1，低电平有效 |

### 接收端（receiver）
| 硬件 | 引脚 |
|------|------|
| 主控 | ESP32-S3 开发板 |
| DAC | GY-PCM5102：BCK→GPIO5, LCK→GPIO6, DIN→GPIO7 |
| 输出 | OUTL/R → 分压（电位器）→ FMO AIN+ |
| PTT | GPIO4 → FMO NET PTT（高有效）|
| LED | GPIO2（发射指示）|
| OLED | SDA→GPIO10, SCL→GPIO11（接收状态显示）|

## 目录结构

```
m5stopwatch-fmo/
├── watch/               # 手表端固件
│   └── fwmo_main/       # 固件源码（单翻译单元）+ 生成脚本
├── receiver/            # 接收端固件
│   └── fmo_tx_espnow/   # ESP-NOW 语音接收 → PCM5102A → FMO
├── docs/                # 设计文档
│   ├── README.md        # 本文件
│   ├── 刷机教程.md       # Arduino IDE 刷机教程
│   ├── AUDIO_ANALYSIS.md
│   ├── 接收端设计文档.md
│   └── radio-voice-tx.html  # 发射界面设计稿
├── reference/           # 参考项目（本地归档，git 不上传）
├── font/HanSan.otf      # 思源黑体源文件
└── .gitignore
```

## 构建

新手刷机请看 **[刷机教程.md](刷机教程.md)**：Arduino IDE 环境安装、板卡/分区设置、固件 + LittleFS 烧录、ESP-NOW 接收端烧录。

### 手表端（watch/fwmo_main）
Arduino IDE（m5stack:esp32 3.3.8 core），板卡 `M5StopWatch`：
```
分区方案: app3M_fat9M_16MB
烧录: 固件 @0x10000, LittleFS @0x610000
```

### 接收端（receiver/fmo_tx_espnow）
Arduino IDE，板卡 `ESP32S3 Dev Module`（esp32:esp32:esp32s3）：
```
烧录: 固件 @0x10000
```

字库/呼号表生成（修改数据后需重新生成并打包）：
```bash
cd watch/fwmo_main
python3 font_gen.py    # 生成 font_hansan_24.bin
python3 gen_callloc.py # 生成 callloc.bin
python3 fs_pack.py     # 打包 font_fs.bin
```

## 使用

- 按住 **A 键**：语音发射（需 WiFi 已连接）
- **A+B 短按**：打开/关闭菜单
- **A+B 长按 3 秒**：息屏（呼号摩斯码动画）
- 菜单：A 下移 / B 确认

## 参考

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — ES8311 全双工 I2S
- M5Stack 官方 M5StopWatch 固件（reference/）
