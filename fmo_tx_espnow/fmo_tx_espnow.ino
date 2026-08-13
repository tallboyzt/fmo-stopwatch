/**
 * fmo_tx_espnow.ino — StopWatch 麦克风 → ESP-NOW → ESP32-S3 + PCM5102A → FMO 发射
 *
 * 低延迟语音方案（参考 esp32-audio-communication-main）：
 *   M5StopWatch 采集麦克风 → 8bit 压缩 → ESP-NOW 点对点发送
 *   本机接收 ESP-NOW → 8bit→16bit → I2S 输出 PCM5102A → FMO AIN+
 *   ESP-NOW 与 WiFi STA 共存（本机仍需连路由器与 StopWatch 同网，用于配对/广播）
 *
 * 协议（与 fwmo_mic_espnow.h 一致）：
 *   每包 ≤250 字节 = [1字节PTT][2字节序号][240字节 8bit 音频]（240样本=15ms @16kHz）
 *   PTT: 1=发射(A键按住) → 拉高 NET PTT；0=停止 → 拉低
 *
 * 硬件连接（同 fmo_tx_esp32.ino，PCM5102A + 分压 + PTT）：
 *   PCM5102A: VIN→3.3V, GND→GND, BCK→GPIO5, LRC→GPIO6, DIN→GPIO7,
 *             FMT→GND, SCK→GND, XSMT→3.3V, LCK→GND
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

// ═══ 音频缓冲 ═══
#define BLOCK_SAMPLES 240      // 每包样本数（8bit）

WiFiUDP *unusedUdp = nullptr;  // (占位，避免未用警告)

uint8_t rxPkt[250];
bool pttActive = false;
volatile uint32_t lastPktMs = 0;   // 最近收到包时间（回调更新）

// ── I2S 初始化（PCM5102A 输出） ──
void i2sInit() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
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
    Serial.println("[I2S] PCM5102A 就绪 (16kHz/16bit)");
}

// ── ESP-NOW 接收回调 ──
void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 3) return;
    lastPktMs = millis();   // 记录收到包时间
    uint8_t ptt = data[0];
    // PTT 控制
    if (ptt && !pttActive) {
        pttActive = true;
        digitalWrite(PIN_PTT_OUT, HIGH);
        digitalWrite(PIN_LED, HIGH);
        Serial.println("[PTT] 发射中");
    } else if (!ptt && pttActive) {
        pttActive = false;
        digitalWrite(PIN_PTT_OUT, LOW);
        digitalWrite(PIN_LED, LOW);
        Serial.println("[PTT] 停止");
    }
    // 音频数据（len>3 → 8bit 样本）
    if (len > 3) {
        int n = len - 3;   // 8bit 样本数
        // 8bit → 16bit + 双声道
        static int16_t outBuf[250 * 2];
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t)(((int32_t)data[3 + i] - 128) << 8);
            outBuf[i * 2] = v;
            outBuf[i * 2 + 1] = v;
        }
        size_t written = 0;
        i2s_write(I2S_NUM, outBuf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
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
    Serial.println("\n=== FMO TX (ESP32-S3 + ESP-NOW + PCM5102A) ===");

    pinMode(PIN_PTT_OUT, OUTPUT);
    digitalWrite(PIN_PTT_OUT, LOW);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    i2sInit();

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
    // ESP-NOW 由回调驱动，主循环空闲
    // 无数据超时释放 PTT（1秒无包）
    if (pttActive && millis() - lastPktMs > 1000) {
        pttActive = false;
        digitalWrite(PIN_PTT_OUT, LOW);
        digitalWrite(PIN_LED, LOW);
        Serial.println("[PTT] 超时停止");
    }
    delay(1);
}
