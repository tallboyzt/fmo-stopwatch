// fwmo_wifi.h — WiFi STA 管理 (参考 VibeSDR WifiManager)
// v1.1: 扫描异步化，通过 FreeRTOS 任务执行，避免阻塞主循环
#pragma once
#include "fwmo_config.h"
#include <WiFi.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

enum FMO_WifiState
{
    WF_DISCONNECTED,
    WF_CONNECTING,
    WF_CONNECTED,
    WF_FAILED
};

enum FMO_ScanState
{
    SCAN_IDLE = 0,
    SCAN_RUNNING = 1,
    SCAN_DONE = 2,
    SCAN_ERROR = 3
};

struct FMO_WifiNet
{
    String ssid;
    int rssi;
    bool open;
};

class FMO_WifiManager
{
public:
    FMO_WifiState state() const { return _st; }
    const char *ip() const { return _ip; }
    const char *ssid() const { return _ssid.c_str(); }
    int rssi() const
    {
        return (_st == WF_CONNECTED) ? WiFi.RSSI() : -127;
    }
    bool hasCreds() const { return _ssid.length() > 0; }
    const char *connectedPass() const { return _pass.c_str(); }   // 当前凭证密码（保存记忆用）

    /* ── 设置凭证 ── */
    void setCredentials(const char *s, const char *pass)
    {
        _ssid = s ? s : "";
        _pass = pass ? pass : "";
    }

    /* ── 启动连接 ── */
    void begin(const char *s = nullptr, const char *pass = nullptr)
    {
        if (s && pass)
            setCredentials(s, pass);
        if (_ssid.isEmpty())
        {
            _st = WF_DISCONNECTED;
            return;
        }
        connectAsync();
    }

    /* ── 非阻塞轮询 (在 loop() 中调用) ── */
    void loop()
    {
        if (_st == WF_CONNECTING)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                _st = WF_CONNECTED;
                IPAddress lip = WiFi.localIP();
                snprintf(_ip, sizeof(_ip), "%d.%d.%d.%d",
                         lip[0], lip[1], lip[2], lip[3]);
                Serial.printf("[WiFi] Connected! IP=%s RSSI=%d\n", _ip, WiFi.RSSI());
                return;
            }
            if (millis() - _start > WIFI_TIMEOUT_MS)
            {
                Serial.println("[WiFi] Connection timeout");
                _st = WF_FAILED;
                WiFi.disconnect();
            }
            return;
        }

        // 断开检测
        if (_st == WF_CONNECTED && WiFi.status() != WL_CONNECTED)
        {
            Serial.println("[WiFi] Disconnected");
            _st = WF_DISCONNECTED;
            _start = 0;
        }

        // 重试（仅当有凭证时）
        if (_st == WF_FAILED && millis() - _start > WIFI_RETRY_MS && !_ssid.isEmpty())
        {
            Serial.println("[WiFi] Retrying...");
            _start = millis();
            connectAsync();
        }
    }

    /* ── 强制重连 ── */
    void reconnect()
    {
        WiFi.disconnect();
        delay(200);
        _st = WF_DISCONNECTED;
        _start = millis();
        if (!_ssid.isEmpty())
            connectAsync();
    }

    /* ══════════════════════════════════════════════════════════════
       ░░  异步 WiFi 扫描 (FreeRTOS 任务)  ░░
       ══════════════════════════════════════════════════════════════ */

    /* ── 启动异步扫描 (立即返回，后台扫描) ── */
    void scanAsync()
    {
        if (_scanState == SCAN_RUNNING)
        {
            Serial.println("[WiFi] Scan already running");
            return;
        }

        _scanState = SCAN_RUNNING;
        _scanNets.clear();

        // 确保 WiFi 栈初始化
        if (!_scanInited)
        {
            WiFi.mode(WIFI_STA);
            WiFi.disconnect(true);
            delay(300);
            _scanInited = true;
        }

        BaseType_t ret = xTaskCreatePinnedToCore(
            scanTask,             // 任务函数
            "wifi_scan",          // 任务名
            WIFI_SCAN_TASK_STACK, // 栈大小
            this,                 // 参数
            WIFI_SCAN_TASK_PRIO,  // 优先级
            &_scanTaskHandle,     // 任务句柄
            WIFI_SCAN_TASK_CORE   // 核心
        );

        if (ret != pdPASS)
        {
            Serial.println("[WiFi] Failed to create scan task");
            _scanState = SCAN_ERROR;
            _scanTaskHandle = nullptr;
        }
        else
        {
            Serial.println("[WiFi] Async scan started");
        }
    }

    /* ── 扫描状态 ── */
    FMO_ScanState scanState() const { return _scanState; }

    /* ── 扫描结果 ── */
    const std::vector<FMO_WifiNet> &scanResults() const { return _scanNets; }

    /* ── 同步扫描 (保留兼容，内部调用异步版本 + 阻塞等待) ── */
    int scan()
    {
        scanAsync();

        // 阻塞等待完成 (最多 6 秒)
        uint32_t start = millis();
        while (_scanState == SCAN_RUNNING && millis() - start < 6000)
        {
            delay(10);
        }

        if (_scanState != SCAN_DONE)
        {
            Serial.println("[WiFi] Sync scan timeout");
            _scanState = SCAN_IDLE;
            return 0;
        }

        _scanState = SCAN_IDLE;
        return _scanNets.size();
    }

    const std::vector<FMO_WifiNet> &networks() const { return _scanNets; }

    /* ── 停止 WiFi (省电) ── */
    void stop()
    {
        WiFi.disconnect(true);
        _st = WF_DISCONNECTED;
    }

private:
    FMO_WifiState _st = WF_DISCONNECTED;
    String _ssid, _pass;
    char _ip[16] = "0.0.0.0";
    uint32_t _start = 0;
    std::vector<FMO_WifiNet> _scanNets;
    bool _scanInited = false;

    // 异步扫描状态
    volatile FMO_ScanState _scanState = SCAN_IDLE;
    TaskHandle_t _scanTaskHandle = nullptr;

    void connectAsync()
    {
        _st = WF_CONNECTING;
        _start = millis();
        WiFi.mode(WIFI_STA);
        WiFi.begin(_ssid.c_str(), _pass.c_str());
        Serial.printf("[WiFi] Connecting to %s...\n", _ssid.c_str());
    }

    /* ── 扫描任务 (FreeRTOS) ── */
    static void scanTask(void *arg)
    {
        FMO_WifiManager *self = (FMO_WifiManager *)arg;

        Serial.println("[WiFi] Scanning (async task)...");

        // ⚠ 关键：扫描前断开当前连接，避免与 WiFi 连接重试冲突导致 scanNetworks 失败(-2)
        // 注意：WiFi.scanNetworks 在连接进行中(WIFI_CONNECTING)调用会返回 WIFI_SCAN_FAILED
        if (WiFi.status() == WL_CONNECTED || WiFi.status() == WL_DISCONNECTED || WiFi.getMode() != WIFI_OFF)
        {
            WiFi.disconnect(true);
            delay(200);
        }

        // 异步扫描
        int n = WiFi.scanNetworks(true, false);
        if (n < 0)
        {
            // WIFI_SCAN_RUNNING = -1，需要等待
            int timeout = 5000;
            int waited = 0;
            while (n < 0 && waited < timeout)
            {
                delay(100);
                waited += 100;
                n = WiFi.scanComplete();
            }
        }

        if (n <= 0)
        {
            Serial.printf("[WiFi] async scan result: %d\n", n);
            if (n < 0)
                WiFi.scanDelete();
            self->_scanState = (n == 0) ? SCAN_DONE : SCAN_ERROR;
            self->_scanTaskHandle = nullptr;
            vTaskDelete(NULL);
            return;
        }

        Serial.printf("[WiFi] Found %d networks\n", n);

        // 填结果
        self->_scanNets.clear();
        for (int i = 0; i < n && i < 20; i++)
        {
            FMO_WifiNet w;
            w.ssid = WiFi.SSID(i);
            w.rssi = WiFi.RSSI(i);
            w.open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
            self->_scanNets.push_back(w);
        }
        WiFi.scanDelete();

        // 按信号强度排序
        size_t sz = self->_scanNets.size();
        for (size_t i = 0; i < sz; i++)
        {
            for (size_t j = i + 1; j < sz; j++)
            {
                if (self->_scanNets[j].rssi > self->_scanNets[i].rssi)
                {
                    auto t = self->_scanNets[i];
                    self->_scanNets[i] = self->_scanNets[j];
                    self->_scanNets[j] = t;
                }
            }
        }

        self->_scanState = SCAN_DONE;
        self->_scanTaskHandle = nullptr;

        // 扫描前断开了连接 → 扫描完成后恢复连接（有凭证时）
        if (!self->_ssid.isEmpty())
        {
            self->_st = WF_FAILED; // 让 loop() 走重试路径重新连接
            self->_start = 0;
            Serial.println("[WiFi] Scan done, reconnecting...");
        }

        Serial.printf("[WiFi] Async scan done, %d networks sorted\n", (int)sz);
        vTaskDelete(NULL);
    }
};
