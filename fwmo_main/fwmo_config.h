// fwmo_config.h — M5StopWatch FMO Companion 引脚/配置定义
// 基于 M5StopWatch C152 (ESP32-S3R8, 466×466 AMOLED, CST820B 触摸, ES8311 I2S)
#pragma once
#include <cstdint>

/* ── 固件版本 ─────────────────────────────────────────────────── */
#define FMO_VERSION_TEXT  "v1.5.0"

/* ── 显示 (M5GFX 自动识别 466×466 AMOLED) ──────────────────────── */
#define FMO_LCD_W         466
#define FMO_LCD_H         466
#define FMO_LCD_R         220          // 圆形可视区域半径(中心233,233)

/* ── 实体按键 ──────────────────────────────────────────────────── */
#define PIN_KEY_A         2            // 黄色按键 G2 (下方)
#define PIN_KEY_B         1            // 蓝色按键 G1 (上方)
#define KEY_ACTIVE_LEVEL  0            // 低电平按下

/* ── 触摸屏 CST820B ────────────────────────────────────────────── */
#define TOUCH_I2C_SDA     6            // M5GFX 自动配置，这里仅作文档
#define TOUCH_I2C_SCL     7
#define TOUCH_I2C_ADDR    0x15
#define TOUCH_DEBOUNCE_MS 50

/* ── 音频 I2S (ES8311 编解码器) ────────────────────────────────── */
#define PIN_I2S_MCLK      18
#define PIN_I2S_BCLK      17
#define PIN_I2S_LRCK      15
#define PIN_I2S_DOUT      16
#define PIN_I2S_DIN       21
#define I2S_SAMPLE_RATE   16000

/* ── 音频 I2C (ES8311 @ 0x18) ──────────────────────────────────── */
#define PIN_ES8311_SDA    47
#define PIN_ES8311_SCL    48
#define ES8311_ADDR       0x18

/* ── FMO 音频格式 ──────────────────────────────────────────────── */
#define FMO_AUDIO_IN_RATE   8000      // FMO服务端输入: 8kHz mono 16bit PCM
#define FMO_AUDIO_OUT_RATE  16000     // 本机输出: 升采样到 16kHz
#define FMO_UPSAMPLE_FACTOR 2         // 升采样因子
#define AUDIO_RING_SAMPLES  64000     // RingBuffer 样本数 (8秒 @8kHz, 128KB PSRAM)
#define AUDIO_CHUNK_SAMPLES 128       // 每次处理块大小 (16ms @8kHz)
#define AUDIO_START_SAMPLES 400       // 缓冲达到后开始播放 (50ms, 快速响应)

/* ── 蓝牙音频配置 ──────────────────────────────────────────────── */
#include "soc/soc_caps.h"
// ESP32-S3 等仅 BLE 芯片无经典蓝牙(A2DP)硬件 → 自动禁用 A2DP 编译（代码保留可移植）
#if defined(SOC_BT_CLASSIC_SUPPORTED) && SOC_BT_CLASSIC_SUPPORTED
#define FMO_BT_CLASSIC_SUPPORTED 1
#else
#define FMO_BT_CLASSIC_SUPPORTED 0
#endif

#define BT_ENABLE_DEFAULT   true      // 默认启用蓝牙音频
#define BT_DEVICE_NAME      "FMO-Watch"
#define BT_SAMPLE_RATE      44100     // A2DP 输出采样率
#define BT_CHANNELS         2         // stereo

/* ── 振动马达 ───────────────────────────────────────────────────── */
#define PIN_VIBRATE        8          // M5StopWatch 振动马达 GPIO（用户实测确认）
#define VIBRATE_SHORT_MS   100
#define VIBRATE_LONG_MS    500

/* ── 电池 (M5PM1 PMIC, I2C读取) ────────────────────────────────── */
// 参考官方固件 hal_pmic.cpp：3300mV→0%、4200mV→100%，7:1滑动滤波
// 百分比按电压线性映射（M5PM1 无电池电流寄存器，无法做库仑计积分）
#define BAT_CAPACITY_MAH  450        // 电池标称容量 450mAh（用于充电电流匹配/文档）
#define BAT_EMPTY_MV       3300
#define BAT_FULL_MV        4200
#define BAT_READ_INTERVAL_MS 1000

/* ── WiFi 默认 ─────────────────────────────────────────────────── */
#define DEFAULT_WIFI_SSID     "Tallboy"
#define DEFAULT_WIFI_PASSWORD "19890214"
#define WIFI_TIMEOUT_MS       15000
#define WIFI_RETRY_MS         5000

/* ── FMO 默认 ──────────────────────────────────────────────────── */
#define DEFAULT_FMO_HOST       "192.168.3.172"
#define DEFAULT_OWNER_CALLSIGN "BD6JNF"

/* ── 多连接配置 ────────────────────────────────────────────────── */
#define PROFILE_MAX        5
#define PROFILE_NAME_MAX   16
#define FMO_HOST_MAX       96

/* ── 时间/刷新间隔 ─────────────────────────────────────────────── */
#define STATION_REFRESH_MS  10000      // 当前站点刷新
#define QSO_REFRESH_MS      60000      // QSO数量刷新
#define RSSI_REFRESH_MS     5000       // WiFi RSSI刷新
#define UI_DRAW_INTERVAL_MS 200        // UI重绘间隔

/* ── 颜色表 RGB565 ──────────────────────────────────────────────── */
#define COL_BG             0x0000      // 纯黑背景(AMOLED省电)
#define COL_AMBER          0xFD20      // 琥珀色主色调
#define COL_GREEN          0x07E0      // 绿色(连接OK)
#define COL_RED            0xF800      // 红色(断连/通联中)
#define COL_DIM            0x7BEF      // 暗灰
#define COL_MUTED          0x9CD3      // 中等灰
#define COL_BORDER         0x4208      // 边框灰
#define COL_ACTIVE         0x07FF      // 通联活跃亮色
#define COL_HIGHLIGHT      0x3DE0      // 菜单高亮
#define COL_CARD_BG        0x18E3      // 卡片背景
#define COL_TEXT_PRIMARY   0xFFFF      // 白色主文字
#define COL_TEXT_SECONDARY 0x9CD3      // 灰色次文字

/* ══════════════════════════════════════════════════════════════════
   ░░  FreeRTOS 任务配置  ░░
   ══════════════════════════════════════════════════════════════════ */

// ── 音频播放任务 ──
#define AUDIO_TASK_STACK        4096
#define AUDIO_TASK_PRIO         8       // 高优先级，确保音频实时
#define AUDIO_TASK_CORE         0       // Core 0 (Arduino loop在Core 1)

// ── WiFi 扫描任务 ──
#define WIFI_SCAN_TASK_STACK    4096
#define WIFI_SCAN_TASK_PRIO     3
#define WIFI_SCAN_TASK_CORE     0

// ── JSON 解析任务 ──
#define JSON_PARSE_TASK_STACK   6144
#define JSON_PARSE_TASK_PRIO    4
#define JSON_PARSE_TASK_CORE    0

// ── 缓存清理任务 ──
#define CACHE_CLEAN_TASK_STACK  3072
#define CACHE_CLEAN_TASK_PRIO   1
#define CACHE_CLEAN_TASK_CORE   0

// ── JSON 解析队列 ──
#define JSON_PARSE_QUEUE_LEN    8

// ── 内存监控 ──
#define MEM_MONITOR_INTERVAL_MS 30000     // 每30秒打印heap状态

// ── 天气 ──
// 固定城市：填入城市名(如 "Lanzhou" 或 "兰州")则不查IP直接使用；留空=ipwho.is自动定位
#define WEATHER_CITY ""

// ── WiFi 音频流（实验）：麦克风 → UDP → 接收端ESP32播放 ──
#define FMO_AUDIO_RX_IP     "192.168.0.110"   // 测试: 电脑IP (udp_audio_test.py 监听)
#define FMO_AUDIO_RX_PORT   12345             // UDP 端口（两端一致）
#define FMO_DISCOVER_PORT   24680            // 接收端广播发现端口（接收端每2秒广播）
