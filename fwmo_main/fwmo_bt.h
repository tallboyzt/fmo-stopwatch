/**
 * fwmo_bt.h — 蓝牙 A2DP Source（音频发送到音箱/耳机）
 *
 * 将 FMO 接收到的 8kHz/16kHz mono PCM 音频流，
 * 通过蓝牙 A2DP 发送到已配对的蓝牙音箱或耳机。
 *
 * 依赖：ESP32-A2DP 库 (pschatzmann/ESP32-A2DP)
 * 如库未安装 → 编译为空实现，所有函数返回假值。
 */
#pragma once
#include "fwmo_config.h"
#include <vector>

/** 扫描到的蓝牙设备（公共定义） */
struct BtScanDevice {
    uint8_t addr[6];      // MAC地址
    char name[64];        // 设备名
    int  rssi;            // 信号强度
};

#if __has_include("BluetoothA2DPSource.h") && FMO_BT_CLASSIC_SUPPORTED
#define FMO_BT_LIB_PRESENT 1   // ESP32-A2DP 库已安装 且 芯片支持经典蓝牙
#include <BluetoothA2DPSource.h>
#include <esp_gap_bt_api.h>

class FMO_Bluetooth {
public:
    /** 启动A2DP音频源（设备名 "FMO-Watch"） */
    bool begin(const char* devName = BT_DEVICE_NAME) {
        _scanSelf = this;
        _devName = devName ? devName : BT_DEVICE_NAME;
        _a2dp.set_auto_reconnect(true, 3);      // 自动重连，最多3次
        _a2dp.start(_devName);
        _enabled = true;
        Serial.printf("[蓝牙] A2DP 已启动: '%s'\n", _devName);
        return true;
    }

    // ══ 设备搜索（esp_bt_gap，选择要连接的音箱/耳机） ══
    /** 开始搜索附近蓝牙设备（durationSec 秒） */
    bool startScan(int durationSec = 20) {
        if (_scanning) return false;
        _scanDevices.clear();
        _scanning = true;
        if (esp_bt_gap_register_callback(gapCb) != ESP_OK) { _scanning = false; return false; }
        esp_bt_gap_start_discovery(ESP_BT_GAP_DISCOVERABLE_MODE, durationSec, 0);
        Serial.println("[蓝牙] 开始搜索设备...");
        return true;
    }
    bool isScanning() const { return _scanning; }
    void stopScan() { esp_bt_gap_cancel_discovery(); _scanning = false; }
    int  scanCount() const { return (int)_scanDevices.size(); }
    const BtScanDevice* scanDevice(int i) const {
        return (i >= 0 && i < (int)_scanDevices.size()) ? &_scanDevices[i] : nullptr;
    }
    /** 连接扫描到的第i个设备（保存地址，A2DP重连） */
    bool connectToScanDevice(int i) {
        auto* d = scanDevice(i);
        if (!d) return false;
        memcpy(_targetAddr, d->addr, 6);
        _targetSet = true;
        Serial.printf("[蓝牙] 选择设备: %s (%02x:%02x:%02x:%02x:%02x:%02x)\n",
                      d->name, d->addr[0],d->addr[1],d->addr[2],d->addr[3],d->addr[4],d->addr[5]);
        // 重启A2DP以连接新目标
        if (_enabled) { _a2dp.stop(); _a2dp.start(_devName); }
        return true;
    }
    bool hasTarget() const { return _targetSet; }
    const uint8_t* targetAddr() const { return _targetAddr; }

    /** 是否有设备已连接 */
    bool isConnected() {
        if (!_enabled) return false;
        return _a2dp.is_connected();
    }

    /**
     * 写入 PCM 音频数据
     * @param data    PCM 数据指针（int16_t mono）
     * @param len     数据长度（字节）
     * @param inRate  输入采样率（默认8000Hz）
     *
     * 自动升采样到 44100Hz stereo 后发送
     */
    void writePCM(const uint8_t* data, int len, int inRate = 8000) {
        if (!_enabled || !_a2dp.is_connected() || !data || len < 2) return;

        const int16_t* in = (const int16_t*)data;
        int inSamples = len / 2;
        int ratio     = BT_SAMPLE_RATE / inRate;       // 44100/8000 ≈ 5.5
        int totalOut  = inSamples * ratio;
        int bufSize   = totalOut * BT_CHANNELS;        // stereo

        if (bufSize > 4096) bufSize = 4096;

        int16_t out[4096];
        for (int i = 0; i < inSamples && i * ratio * BT_CHANNELS < bufSize; i++) {
            int16_t s = in[i];
            for (int u = 0; u < ratio; u++) {
                int idx = (i * ratio + u) * BT_CHANNELS;
                if (idx + 1 < bufSize) {
                    out[idx]     = s;  // 左声道
                    out[idx + 1] = s;  // 右声道（mono→stereo复制）
                }
            }
        }
        _a2dp.write_data((uint8_t*)out, bufSize * sizeof(int16_t));
    }

    void stop() { if (_enabled) { _a2dp.stop(); _enabled = false; } }

    /** 修改设备名并重启蓝牙 */
    void setDeviceName(const char* name) {
        _devName = name ? name : BT_DEVICE_NAME;
        if (_enabled) { _a2dp.stop(); _a2dp.start(_devName); }
    }

    /** 设置音量 0-100 → 映射到 A2DP 0-127 */
    void setVolume(uint8_t vol) {
        if (vol > 100) vol = 100;
        _volume = vol;
        if (_enabled) { /* A2DP set_volume((vol*127)/100) */ }
    }

    /** 获取已连接设备的名称 */
    const char* connectedDeviceName() {
        if (!_a2dp.is_connected()) return nullptr;
        return _a2dp.get_connected_source_name();
    }

private:
    BluetoothA2DPSource _a2dp;
    const char*         _devName = BT_DEVICE_NAME;
    bool                _enabled = false;
    uint8_t             _volume  = 80;

    volatile bool _scanning = false;
    std::vector<BtScanDevice> _scanDevices;
    esp_bd_addr_t _targetAddr = {};
    bool _targetSet = false;

    /** 蓝牙GAP回调：收集发现的设备 */
    static void gapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
        // 通过user_data取回this（简化：用全局指针）
        FMO_Bluetooth* self = _scanSelf;
        if (!self) return;
        switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            // 从EIR解析设备名（EIR为原始数据）
            char name[64] = {0};
            esp_bt_gap_cb_param_t::disc_res_param_t& r = param->disc_res;
            if (r.eir) {
                // EIR: len,type,name...
                const uint8_t* p = r.eir;
                while (*p) {
                    uint8_t len = *p++;
                    if (len < 2) break;
                    uint8_t type = p[1];
                    if (type == ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME || type == ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME) {
                        uint8_t n = len - 1;
                        if (n > 63) n = 63;
                        memcpy(name, p + 2, n);
                        name[n] = 0;
                    }
                    p += len;
                }
            }
            if (!name[0]) return;   // 无名字设备跳过
            BtScanDevice d;
            memcpy(d.addr, r.bda, 6);
            strncpy(d.name, name, 63);
            d.rssi = r.rssi;
            self->_scanDevices.push_back(d);
            Serial.printf("[蓝牙] 发现: %s rssi=%d\n", name, r.rssi);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                self->_scanning = false;
                Serial.printf("[蓝牙] 搜索结束，共%d个设备\n", (int)self->_scanDevices.size());
            }
            break;
        default: break;
        }
    }

    static FMO_Bluetooth* _scanSelf;   // 简化：静态this指针
};
FMO_Bluetooth* FMO_Bluetooth::_scanSelf = nullptr;

#else
#define FMO_BT_LIB_PRESENT 0   // ESP32-A2DP 库未安装！
/* ──────────────────────────────────────────────────
   无 A2DP 库 → 空实现（编译通过但蓝牙不工作）
   ────────────────────────────────────────────────── */
class FMO_Bluetooth {
public:
    bool begin(const char* = nullptr)       { return false; }
    bool isConnected()                       { return false; }
    void writePCM(const uint8_t*, int, int=8000) {}
    void stop()                                {}
    void setDeviceName(const char*)            {}
    void setVolume(uint8_t)                    {}
    const char* connectedDeviceName()          { return nullptr; }
    bool startScan(int = 20)                   { return false; }
    bool isScanning() const                    { return false; }
    void stopScan()                            {}
    int  scanCount() const                     { return 0; }
    const BtScanDevice* scanDevice(int) const  { return nullptr; }
    bool connectToScanDevice(int)              { return false; }
    bool hasTarget() const                     { return false; }
    const uint8_t* targetAddr() const          { return nullptr; }
};
#endif
