/**
 * fwmo_mic_wifi.h — 麦克风采集 → WiFi UDP 音频流发送（FMO 发射链路）
 *
 * 用途：按住A键讲话，麦克风(16kHz/16bit) 通过 WiFi UDP 发送到
 *       ESP32 发射端（PCM5102A I2S DAC → FMO AIN+）。
 *
 * 协议：UDP 音频流
 *   - 每包 1024 字节 = [1字节PTT标志][2字节序号][1021字节PCM]（≈32ms 音频）
 *   - PTT 标志: 1=发射(A键按住), 0=停止(A键松开) —— ESP32 收到立即控制 NET PTT
 *   - 目标: 接收端 IP 的 UDP 12345 端口
 *
 * 依赖：WiFi 已连接（fwmo_wifi.h），配置里设置接收端 IP（FMO_AUDIO_RX_IP）
 */
#pragma once
#include <WiFi.h>
#include <WiFiUdp.h>
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fwmo_config.h"
#include "fwmo_settings.h"   // 运行时读取发射目标IP（设置菜单可改）

#ifndef FMO_AUDIO_RX_IP
#define FMO_AUDIO_RX_IP   "192.168.1.100"   // 接收端 ESP32 的 IP
#endif
#ifndef FMO_AUDIO_RX_PORT
#define FMO_AUDIO_RX_PORT 12345
#endif

class FMO_MicWifi {
public:
    void setSettings(FMO_SettingsMgr* s) { _settings = s; }   // 运行时读取发射目标IP

    void begin() {
        _udp.begin(FMO_AUDIO_RX_PORT);       // 绑定本地端口（发送用）
        _discoverUdp.begin(FMO_DISCOVER_PORT);  // 监听接收端广播
    }

    /**
     * 自动发现：监听接收端广播（每2秒一次 "FMO-RX" 包），记录来源 IP。
     * 在主循环 loop() 中调用。优先使用发现的 IP，其次用手动设置。
     */
    void loop() {
        int sz = _discoverUdp.parsePacket();
        if (sz > 0) {
            char buf[16];
            int n = _discoverUdp.read(buf, sizeof(buf) - 1);
            buf[n] = 0;
            // 校验标识
            if (n >= 5 && memcmp(buf, "FMO-RX", 6) == 0) {
                IPAddress src = _discoverUdp.remoteIP();
                snprintf(_discoveredIp, sizeof(_discoveredIp), "%d.%d.%d.%d",
                         src[0], src[1], src[2], src[3]);
                Serial.printf("[麦克风WiFi] 发现接收端: %s\n", _discoveredIp);
                _lastDiscoverMs = millis();
            }
        }
    }

    /** 当前发射目标 IP：优先自动发现，其次手动设置，最后默认值 */
    const char* currentTxIp() {
        // 自动发现有效（10秒内有广播）
        if (_discoveredIp[0] && millis() - _lastDiscoverMs < 10000)
            return _discoveredIp;
        if (_settings && _settings->audioTxIp()[0])
            return _settings->audioTxIp();
        return FMO_AUDIO_RX_IP;
    }

    /**
     * 设置发送状态：
     *   on=true  → 启麦克风 + UDP 任务开始发送
     *   on=false → 停麦克风 + 停任务
     */
    void setTransmit(bool on) {
        if (on == _active) return;
        if (on) {
            if (WiFi.status() != WL_CONNECTED) {  // WiFi未连接不启动
                _active = false;
                return;
            }
            M5.Mic.setSampleRate(16000);   // 与接收端 DAC 一致（16kHz 语音足够）
            if (M5.Mic.begin()) {
                _sendCtrl(1);              // 立即发 PTT=1，让 ESP32 马上拉高（不等首个音频包）
                if (_task == nullptr) {
                    xTaskCreatePinnedToCore(task, "mic_wifi", 16384, this, 5, &_task, 1);
                }
                _running = true;
                Serial.printf("[麦克风WiFi] TX开启 → %s:%d (16kHz)\n", FMO_AUDIO_RX_IP, FMO_AUDIO_RX_PORT);
            } else {
                Serial.println("[麦克风WiFi] 启动失败");
                _active = false;
            }
        } else {
            _running = false;
            // 发送显式 PTT 停止包，让 ESP32 立即释放 PTT（不等超时）
            _sendCtrl(0);
            if (_task) { vTaskDelete(_task); _task = nullptr; }
            M5.Mic.end();
            Serial.println("[麦克风WiFi] TX关闭");
        }
    }

    /** 是否正在发送 */
    bool isTransmitting() const { return _active && _running; }

private:
    FMO_SettingsMgr* _settings = nullptr;
    volatile bool _active = false;
    volatile bool _running = false;
    TaskHandle_t _task = nullptr;
    WiFiUDP _udp;
    WiFiUDP _discoverUdp;        // 接收端广播监听
    uint16_t _seq = 0;
    char _discoveredIp[17] = ""; // 自动发现的接收端 IP
    uint32_t _lastDiscoverMs = 0;
    // 当前发射目标 IP（每次发送前从 currentTxIp() 读取）
    char _txIp[17] = "";

    const char* txIp() {
        return currentTxIp();
    }

    // 发送 PTT 状态包（供 ESP32 立即控制 FMO PTT）
    void _sendCtrl(uint8_t ptt) {
        IPAddress target;
        if (!target.fromString(txIp())) return;
        uint8_t p[3];
        p[0] = ptt;                       // PTT 标志: 1=发射, 0=停止
        p[1] = 0; p[2] = 0;
        _udp.beginPacket(target, FMO_AUDIO_RX_PORT);
        _udp.write(p, sizeof(p));
        _udp.endPacket();
    }

    static void task(void* arg) {
        FMO_MicWifi* self = (FMO_MicWifi*)arg;
        IPAddress target;
        target.fromString(self->txIp());
        int16_t buf[512];                  // 512样本 ≈ 32ms @16kHz
        uint8_t pkt[1024];                 // [1字节PTT][2字节序号][1021字节PCM]

        while (self->_running) {
            if (!M5.Mic.isEnabled()) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (!M5.Mic.record(buf, 512)) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            // 打包：PTT标志(1=发射) + 序号 + PCM
            uint16_t seq = self->_seq++;
            pkt[0] = 1;                    // PTT=1（A键按住发射中）
            pkt[1] = seq >> 8;
            pkt[2] = seq & 0xFF;
            memcpy(pkt + 3, buf, 1024 - 3);
            self->_udp.beginPacket(target, FMO_AUDIO_RX_PORT);
            self->_udp.write(pkt, sizeof(pkt));
            self->_udp.endPacket();
        }
        vTaskDelete(NULL);
    }
};
