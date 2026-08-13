# M5StopWatch FMO 通联伴侣

基于 M5StopWatch C152（ESP32-S3 + 1.54" 466×466 AMOLED）的 **FMO 通联伴侣** 固件。

## 功能

- **FMO 通联**：WebSocket 接收音频/呼号/台站信息
- **中文字体**：思源黑体 24px 显示呼号、台站、城市地址（LittleFS → PSRAM）
- **离线呼号查询**：呼号 → 城市/省份（FNV-1a 哈希查表，13686 呼号）
- **语音发射**：按住 A 键 → 全双工 I2S 采集麦克风 → ESP-NOW → 接收端 → FMO
- **发射界面**：radio-voice-tx 设计稿（同心圆环 + 呼吸光环 + 均衡器 + 计时器）
- **WiFi 记忆库**：自动保存/重连已连接 WiFi
- **息屏省电**：A+B 长按 3 秒息屏 + 呼号摩斯码动画

## 硬件

| 硬件 | 详情 |
|------|------|
| 主控 | ESP32-S3，8MB PSRAM |
| 屏幕 | 1.54" AMOLED 466×466 |
| 音频 | ES8311 Codec（全双工 I2S0 TX+RX）|
| 按键 | A=GPIO2，B=GPIO1，低电平有效 |

## 目录结构

```
fwmo_main/          # 固件源码（单翻译单元）
  ├── fwmo_main.ino # 主程序
  ├── fwmo_*.h      # 模块头文件
  ├── wicon.c       # 天气图标 A8 位图
  ├── lv_conf.h     # LVGL 配置
  └── *_gen.py      # 字库/呼号表生成脚本
fmo_tx_espnow/      # ESP-NOW 语音接收端（独立 ESP32 + PCM5102A）
demo/               # M5Stack 官方固件参考
xiaozhi-esp32-main/ # ES8311 全双工 I2S 参考
font/HanSan.otf     # 思源黑体源文件
radio-voice-tx.html # 发射界面设计稿
```

## 构建

使用 Arduino IDE（m5stack:esp32 3.3.8 core），板卡 `M5StopWatch`：

```
分区方案: app3M_fat9M_16MB
烧录: 固件 @0x10000, LittleFS @0x610000
```

字库/呼号表生成（修改数据后需重新生成并打包）：
```bash
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
- M5Stack 官方 M5StopWatch 固件
