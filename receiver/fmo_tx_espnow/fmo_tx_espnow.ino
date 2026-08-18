/**
 * fmo_tx_espnow.ino — StopWatch 麦克风 → ESP-NOW → ESP32-S3 + PCM5102A → FMO 发射
 *
 * 低延迟语音方案（参考 esp32-audio-communication-main）：
 *   M5StopWatch 采集麦克风 → 8bit 压缩 → ESP-NOW 点对点发送
 *   本机接收 ESP-NOW → 8bit→16bit → I2S 输出 PCM5102A → FMO AIN+
 *   ESP-NOW 与 WiFi STA 共存（本机仍需连路由器与 StopWatch 同网，用于配对/广播）
 *
 * 协议（与 fwmo_mic_espnow.h 一致）：
 *   每包 ≤250 字节 = [1字节PTT][2字节序号][240字节 8bit 音频]（240样本=30ms @8kHz）
 *   PTT: 1=发射(A键按住) → 拉高 NET PTT；0=停止 → 拉低
 *
 * v2 修复"哒哒哒"：
 *   回调只把音频写入环形缓冲（不阻塞），独立播放任务从缓冲持续写 I2S。
 *   消除 ESP-NOW 回调阻塞导致丢包 → 音频缺口 → 哒哒声。
 *
 * 硬件连接（PCM5102A + 分压 + PTT）：
 *   PCM5102A: VIN→3.3V, GND→GND, BCK→GPIO5, LRC→GPIO6, DIN→GPIO7,
 *             FMT→GND, SCK→GND, XSMT→3.3V, FLT→GND, DMP→GND
 *   OUTL ─┬─[47kΩ]─┬─→ FMO AIN+
 *         └─[4.7kΩ]─┘↓GND
 *   GPIO4 → FMO NET PTT（发射时高）
 *   GPIO2 → LED（发射时亮）
 *
 * 首次使用：烧录后看串口打印 MAC（如 34:85:18:xx:xx:xx），
 *           填入 StopWatch fwmo_main 的 FMO_RX_MAC。
 */
#include <WiFi.h>
#include <esp_now.h>
#include "driver/i2s.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ═══ OLED（0.96寸 SSD1306，显示音频接收状态）═══
#define OLED_SDA  10
#define OLED_SCL  11
#define OLED_RST  -1
Adafruit_SSD1306 oled(128, 64, &Wire, OLED_RST);

// ═══ WiFi 配置（ESP-NOW 在 STA 模式下初始化）═══
const char* WIFI_SSID = "Tallboy";        // ← 改成你的WiFi
const char* WIFI_PASS = "12345678";       // ← 改成你的WiFi密码

// ═══ 引脚 ═══
#define PIN_PTT_OUT   4        // FMO NET PTT（高=发射）
#define PIN_LED       2        // 指示灯

// ═══ I2S (PCM5102A) ═══
#define I2S_NUM       I2S_NUM_0
#define I2S_BCK       5
#define I2S_WS        6
#define I2S_DOUT      7
#define SAMPLE_RATE   8000

// ═══ 音频缓冲（环形队列，16bit 样本，双声道）═══
#define BLOCK_SAMPLES 240      // 每包样本数（8bit）
#define RING_CAPACITY (BLOCK_SAMPLES * 2 * 5)   // 5 包容量 ≈ 150ms（吸收 ESP-NOW 抖动防哒哒）
static int16_t ringBuf[RING_CAPACITY];
static volatile uint32_t ringHead = 0, ringTail = 0;   // 写/读位置（样本计数）
static SemaphoreHandle_t ringSem = nullptr;            // 播放任务唤醒信号

WiFiUDP *unusedUdp = nullptr;  // (占位，避免未用警告)

uint8_t rxPkt[250];
bool pttActive = false;
volatile uint32_t lastPktMs = 0;   // 最近收到包时间（回调更新）
static int pktSkip = 0;            // 丢弃按键噪声包计数（PTT 上升沿后前2包）

// ── OLED 显示状态（回调更新，loop 定时刷新） ──
volatile uint8_t  audioLvl = 0;    // 音频电平 0-100（包内最大幅值）
volatile uint32_t rxPktTotal = 0;  // 总收包数
volatile bool     rxActive = false;// 是否正在接收音频（PTT）
uint32_t pktPerSec = 0;            // 每秒收包数（显示用）
uint32_t pktSecCnt = 0, pktSecT0 = 0;
uint32_t lastOledMs = 0;

// ── OLED 初始化 ──
void oledInit() {
    Wire.begin(OLED_SDA, OLED_SCL);
    if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        oled.ssd1306_command(0x81);
        oled.ssd1306_command(255);   // 对比度拉满
        oled.clearDisplay();
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextSize(1);
        oled.setCursor(20, 25);
        oled.print("RX BOOT...");
        oled.display();
        Serial.println("[OLED] OK");
    } else {
        Serial.println("[OLED] FAIL");
    }
}

// ── OLED 刷新（100ms 一次） ──
void oledUpdate() {
    oled.clearDisplay();
    // 第1行：状态
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(2, 2);
    if (rxActive && millis() - lastPktMs < 500)
        oled.print("RX AUDIO");
    else
        oled.print("RX Ready");
    oled.setCursor(88, 2);
    oled.printf("%u/s", (unsigned)pktPerSec);
    // 中部：12 格电平条
    int bars = (audioLvl * 12) / 100;
    if (bars > 12) bars = 12;
    for (int i = 0; i < 12; i++) {
        int x = 2 + i * 10;
        if (i < bars) oled.fillRect(x, 20, 8, 22, SSD1306_WHITE);
        else          oled.drawRect(x, 20, 8, 22, SSD1306_WHITE);
    }
    // 底部：总包数 + 电平百分比
    oled.setCursor(2, 48);
    oled.printf("pkt:%u", (unsigned)rxPktTotal);
    oled.setCursor(78, 48);
    oled.printf("lv:%u%%", (unsigned)audioLvl);
    oled.display();
}

// ── I2S 初始化（PCM5102A 输出） ──
void i2sInit() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 3,          // 3×256帧 = 96ms DMA（超低延迟）
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    esp_err_t e = i2s_driver_install(I2S_NUM, &cfg, 0, NULL);
    if (e != ESP_OK) Serial.printf("[I2S] install fail: %d\n", e);
    i2s_pin_config_t pin = {
        .bck_io_num = I2S_BCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    e = i2s_set_pin(I2S_NUM, &pin);
    if (e != ESP_OK) Serial.printf("[I2S] set_pin fail: %d\n", e);
    Serial.println("[I2S] PCM5102A 就绪 (8kHz/16bit)");
}

// ── 环形缓冲操作 ──
static inline uint32_t ringCount() {
    return (ringHead + RING_CAPACITY - ringTail) % RING_CAPACITY;
}
static void ringPush(const int16_t* data, uint32_t n) {
    // 空间不足则丢弃最旧数据（保持实时）
    while (ringCount() + n > RING_CAPACITY && ringCount() > 0) {
        ringTail = (ringTail + 2) % RING_CAPACITY;   // 丢弃 1 帧（2 样本）
    }
    for (uint32_t i = 0; i < n; i++) {
        ringBuf[ringHead] = data[i];
        ringHead = (ringHead + 1) % RING_CAPACITY;
    }
    if (ringSem) xSemaphoreGive(ringSem);
}
static uint32_t ringPop(int16_t* out, uint32_t maxN) {
    uint32_t cnt = ringCount();
    uint32_t n = (cnt < maxN) ? cnt : maxN;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = ringBuf[ringTail];
        ringTail = (ringTail + 1) % RING_CAPACITY;
    }
    return n;
}

// ── 独立播放任务：持续从缓冲读数据写 I2S（节奏由 I2S 时钟驱动，防波动） ──
void playTask(void* arg) {
    (void)arg;
    static int16_t outBuf[480];
    static int16_t zero[64 * 2] = {0};   // 8ms 小静音帧（缓冲空时填充，缺口短防哒哒）
    while (1) {
        uint32_t n = ringPop(outBuf, 480);
        if (n > 0) {
            size_t written = 0;
            i2s_write(I2S_NUM, outBuf, n * sizeof(int16_t), &written, pdMS_TO_TICKS(50));
        } else {
            // 缓冲空：写 30ms 静音保持 I2S 连续（无固定 delay，立即重试取数据）
            size_t written = 0;
            i2s_write(I2S_NUM, zero, sizeof(zero), &written, pdMS_TO_TICKS(20));
        }
    }
}

// ── ESP-NOW 接收回调（新版 esp_now_recv_cb_t 签名：带 esp_now_recv_info*） ──
void onDataRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
    const uint8_t* mac = info ? info->src_addr : nullptr;   // 新版用 info->src_addr
    (void)mac;
    if (len < 3) return;
    lastPktMs = millis();   // 记录收到包时间
    uint8_t ptt = data[0];
    // PTT 控制
    if (ptt && !pttActive) {
        pttActive = true;
        rxActive = true;
        // 开始发射：清空环形缓冲（丢弃上一次残留数据）+ 丢按键噪声包
        // 注意：不能 i2s_stop/start（DAC 重启会产生爆音），只需清空待播缓冲
        ringHead = 0; ringTail = 0;
        pktSkip = 3;           // 丢弃前3包（按键瞬间噪声，~90ms）
        digitalWrite(PIN_PTT_OUT, HIGH);
        digitalWrite(PIN_LED, HIGH);
        Serial.println("[PTT] 发射中");
    } else if (!ptt && pttActive) {
        pttActive = false;
        rxActive = false;
        digitalWrite(PIN_PTT_OUT, LOW);
        digitalWrite(PIN_LED, LOW);
        Serial.println("[PTT] 停止");
    }
    // 音频数据（len>3 → 8bit 样本）→ 转 16bit 双声道 → 入环形缓冲（不阻塞！）
    if (len > 3) {
        // 丢弃按键瞬间的噪声包（PTT 上升沿后的前2包）
        if (pktSkip > 0) { pktSkip--; return; }
        int n = len - 3;   // 8bit 样本数
        rxPktTotal++;
        // 音频电平：包内最大幅值（|s-128|）→ 0-100
        uint8_t maxAbs = 0;
        for (int i = 0; i < n; i++) {
            uint8_t d = (data[3 + i] > 128) ? (uint8_t)(data[3 + i] - 128)
                                            : (uint8_t)(128 - data[3 + i]);
            if (d > maxAbs) maxAbs = d;
        }
        audioLvl = (uint8_t)(((uint32_t)maxAbs * 100) / 128);
        // 接收统计：每秒打印收包数（检测丢包）
        static uint32_t pktCnt = 0, statT0 = 0;
        pktCnt++;
        if (millis() - statT0 >= 1000) {
            Serial.printf("[STAT] pkt/s=%u\n", (unsigned)pktCnt);
            pktCnt = 0; statT0 = millis();
        }
        // OLED 统计（主循环读取，不用volatile竞争过多）
        pktSecCnt++;
        if (millis() - pktSecT0 >= 1000) {
            pktPerSec = pktSecCnt;
            pktSecCnt = 0; pktSecT0 = millis();
        }
        static uint32_t dbgCnt = 0;
        if ((++dbgCnt % 50) == 1)
            Serial.printf("[RX] len=%d n=%d seq=%02x%02x s0=%d buf=%u\n",
                          len, n, data[1], data[2], (int)((int8_t)(data[3] - 128)),
                          (unsigned)ringCount());
        // 8bit → 16bit + 双声道（含降噪：噪声门，滞回+慢平滑防 32Hz 调制）
        static int16_t convBuf[250 * 2];
        // 噪声门（滞回）：低电平静音/衰减，消除背景沙沙
        // 滞回：已静音时需更高阈值才恢复，避免门限附近抖动产生周期性音量调制
        float gate = 1.0f;
        if (maxAbs < 5)       gate = 0.0f;                              // 完全静音（背景噪声）
        else if (maxAbs < 15) gate = (maxAbs - 5) / 10.0f * 0.6f;       // 低电平衰减
        static float gateSmooth = 0.0f;
        if (gateSmooth < 0.05f && maxAbs < 20) gate = 0.0f;            // 静音态滞回：需>20才恢复
        gateSmooth += (gate - gateSmooth) * 0.12f;                      // 慢平滑（0.12，抑制调制）
        if (gateSmooth < 0.0f) gateSmooth = 0.0f;
        // 重采样：发送端实测 32包/s(≈7.66kHz) → 播放 8kHz，每包 240→250 样本（25:24）
        // 放大 4% 使播放速率匹配发送，防缓冲欠载卡顿（保持音调/时长）
        int nOut = n * 25 / 24;
        if (nOut < 1) nOut = 1;
        for (int j = 0; j < nOut; j++) {
            int pos = j * 24;               // 输入位置 ×25
            int i0 = pos / 25;
            int frac = pos % 25;
            int i1 = (i0 + 1 < n) ? (i0 + 1) : i0;
            int v8 = (data[3 + i0] * (25 - frac) + data[3 + i1] * frac) / 25;
            int16_t raw = (int16_t)(((int32_t)v8 - 128) << 8);
            int16_t v = (int16_t)((float)raw * gateSmooth);
            convBuf[j * 2] = v;
            convBuf[j * 2 + 1] = v;
        }
        ringPush(convBuf, nOut * 2);
    }
}

// ── ESP-NOW 初始化（STA 模式下） ──
bool espNowInit() {
    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNow] init failed");
        return false;
    }
    esp_now_register_recv_cb(onDataRecv);
    Serial.printf("[ESPNow] MAC: %s\n", WiFi.macAddress().c_str());
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== FMO TX (ESP32-S3 + ESP-NOW + PCM5102A) v2 ===");

    pinMode(PIN_PTT_OUT, OUTPUT);
    digitalWrite(PIN_PTT_OUT, LOW);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    oledInit();      // OLED 显示接收状态

    ringSem = xSemaphoreCreateCounting(40, 0);   // 计数信号量：每包 give 一次
    i2sInit();

    // 启动独立播放任务（核心2）
    xTaskCreatePinnedToCore(playTask, "i2s_play", 4096, nullptr, 5, nullptr, 1);

    // WiFi STA（ESP-NOW 需要，同时可打印 MAC）
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WiFi] 连接 %s ...\n", WIFI_SSID);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(200);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] 已连接! IP=%s MAC=%s\n",
                      WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
    } else {
        Serial.println("\n[WiFi] 连接失败（继续用 ESP-NOW 广播接收）");
    }

    if (espNowInit())
        Serial.println("=== 就绪：等待 StopWatch ESP-NOW 语音 ===");
    else
        while (1) delay(1000);
}

void loop() {
    // ESP-NOW 由回调驱动，播放由独立任务驱动
    // 无数据超时释放 PTT（1秒无包）
    if (pttActive && millis() - lastPktMs > 1000) {
        pttActive = false;
        rxActive = false;
        digitalWrite(PIN_PTT_OUT, LOW);
        digitalWrite(PIN_LED, LOW);
        Serial.println("[PTT] 超时停止");
    }
    // OLED 定时刷新（100ms，避免 I2C 频繁写入拖慢主循环）
    if (millis() - lastOledMs >= 100) {
        lastOledMs = millis();
        oledUpdate();
    }
    delay(1);
}
