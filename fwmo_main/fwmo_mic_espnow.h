/**
 * fwmo_mic_espnow.h — 麦克风 → ESP-NOW 低延迟语音发送
 *
 * 方案：WiFi STA（连FMO服务器，WebSocket） + ESP-NOW（语音点对点）共存
 * 参考：esp32-audio-communication-main（STA模式下 ESP-NOW 传输音频）
 *
 * 包格式（ESP-NOW 单包 ≤250字节）：
 *   [0]    PTT 标志 (1=发射, 0=停止)
 *   [1:2]  序号 (big-endian)
 *   [3:]   8bit 音频样本（240样本 = 15ms @16kHz）
 *   共 243 字节 ≤ 250 ✓
 *
 * 优势：点对点免路由器、低延迟（单跳 ~5ms）、与 WiFi STA 共存
 * 限制：8bit 量化（语音可懂，音质略降）
 */
#pragma once
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>   // wifi_tx_info_t（新版发送回调参数）
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fwmo_config.h"
#include "fwmo_settings.h"
#include "fwmo_audio_full.h"   // 全双工音频（RX 采集）

// 接收端 MAC 地址（ESP32 接收端，需烧录后查看串口打印）
#ifndef FMO_RX_MAC
#define FMO_RX_MAC  "FF:FF:FF:FF:FF:FF"   // 默认广播（也可填具体 MAC）
#endif

class FMO_MicEspnow {
public:
    void setSettings(FMO_SettingsMgr* s) { _settings = s; }
    void setAudioFull(FMO_AudioFull* a) { _audioFull = a; }   // 绑定全双工音频（RX 采集）

    bool begin() {
        // ESP-NOW 在 STA 模式下初始化（WiFi 已由 fwmo_wifi 连接）
        if (esp_now_init() != ESP_OK) {
            Serial.println("[ESPNow] 初始化失败");
            return false;
        }
        esp_now_register_send_cb(_onSent);
        // 添加接收端 peer
        esp_now_peer_info_t peer = {};
        uint8_t mac[6];
        if (parseMac(_rxMac, mac)) {
            memcpy(peer.peer_addr, mac, 6);
            peer.channel = 0;
            peer.encrypt = false;
            esp_err_t r = esp_now_add_peer(&peer);
            if (r != ESP_OK) {
                Serial.printf("[ESPNow] add_peer 失败: %s（尝试广播）\n", esp_err_to_name(r));
                uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
                memcpy(peer.peer_addr, bc, 6);
                peer.channel = 0; peer.encrypt = false;
                r = esp_now_add_peer(&peer);
            }
            if (r != ESP_OK) {
                Serial.printf("[ESPNow] 广播 peer 也失败: %s\n", esp_err_to_name(r));
                _ready = false;
                return false;
            }
            Serial.printf("[ESPNow] 接收端 MAC: %s\n", _rxMac);
        } else {
            Serial.println("[ESPNow] MAC 解析失败，用默认广播");
            uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            memcpy(peer.peer_addr, bc, 6);
            peer.channel = 0; peer.encrypt = false;
            esp_err_t r = esp_now_add_peer(&peer);
            if (r != ESP_OK) {
                Serial.printf("[ESPNow] 广播 peer 失败: %s\n", esp_err_to_name(r));
                _ready = false;
                return false;
            }
        }
        _ready = true;
        return true;
    }

    void setTransmit(bool on) {
        if (on == _active) return;
        if (on) {
            if (WiFi.status() != WL_CONNECTED) { _active = false; return; }
            if (!_audioFull) { _active = false; Serial.println("[ESPNow] 未绑定 AudioFull"); return; }
            // 全双工：不切换 I2S，直接开始采集发送（RX 通道始终运行）
            _sendCtrl(1);   // PTT=1
            delay(20);
            _running = true;
            _active = true;
            if (_task == nullptr)
                xTaskCreatePinnedToCore(task, "mic_espnow", 16384, this, 5, &_task, 1);
            Serial.println("[ESPNow] TX开启（全双工采集）");
        } else {
            _running = false;
            _sendCtrl(0);   // PTT=0
            if (_task) {
                // 等任务退出：task 退出时置 _task=nullptr（不调 eTaskGetState！）
                uint32_t t0 = millis();
                while (_task != nullptr && millis() - t0 < 200) vTaskDelay(1);
                _task = nullptr;
            }
            _active = false;
            Serial.println("[ESPNow] TX关闭");
        }
    }

private:
    // 全双工音频引用（由主程序绑定，RX 通道采集麦克风）
    FMO_AudioFull* _audioFull = nullptr;

    FMO_SettingsMgr* _settings = nullptr;
    volatile bool _active = false, _running = false;
    TaskHandle_t _task = nullptr;
    uint16_t _seq = 0;
    char _rxMac[18] = FMO_RX_MAC;
    bool _ready = false;
    static volatile bool _sentOk;

    // 新版 ESP32 库：发送回调参数为 wifi_tx_info_t*
    static void _onSent(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
        _sentOk = true;
    }

    static bool parseMac(const char* str, uint8_t* mac) {
        if (!str) return false;
        int vals[6];
        if (sscanf(str, "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2],
                   &vals[3], &vals[4], &vals[5]) != 6) return false;
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)vals[i];
        return true;
    }

    void _sendCtrl(uint8_t ptt) {
        if (!_ready) return;
        uint8_t p[3] = {ptt, 0, 0};
        uint8_t mac[6];
        parseMac(_rxMac, mac);
        esp_now_send(mac, p, 3);
    }

    static void task(void* arg) {
        FMO_MicEspnow* self = (FMO_MicEspnow*)arg;
        uint8_t mac[6];
        if (!self->parseMac(self->_rxMac, mac)) {
            vTaskDelete(NULL);
            return;
        }
        int16_t buf[480];       // 480 int16 = 240 帧（STEREO L+R） = 30ms @8kHz
        uint8_t pkt[243];       // [PTT][seq:2][8bit×240]

        while (self->_running) {
            // 全双工 RX 采集（8kHz/16bit STEREO，带超时）
            if (!self->_audioFull) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            int got = self->_audioFull->readRx(buf, 480, 30);
            if (got < 480) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            // 打包：16bit → 8bit 压缩（取每帧左声道；STEREO 下 L=R，取 L 即单声道）
            uint16_t seq = self->_seq++;
            pkt[0] = 1;                    // PTT=1
            pkt[1] = seq >> 8;
            pkt[2] = seq & 0xFF;
            for (int i = 0; i < 240; i++) {
                int16_t s = buf[i * 2];    // 每帧取 L（右声道相同）
                pkt[3 + i] = (uint8_t)(((uint32_t)(s + 32768)) >> 8);
            }
            // 发送（等上次完成，带超时防卡死）
            uint32_t t0 = millis();
            while (!_sentOk && millis() - t0 < 100) vTaskDelay(1);
            _sentOk = false;
            esp_now_send(mac, pkt, sizeof(pkt));
        }
        self->_task = nullptr;
        vTaskDelete(NULL);
    }
};

volatile bool FMO_MicEspnow::_sentOk = true;
