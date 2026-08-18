// fwmo_settings.h — NVS 配置持久化 (5组连接配置)
// 使用 Arduino Preferences 读写 NVS
#pragma once
#include "fwmo_config.h"
#include <Preferences.h>

/* ── 连接配置结构 ───────────────────────────────────────────────── */
struct FMO_Profile {
    char name[PROFILE_NAME_MAX];       // "家里", "车上", ...
    char wifi_ssid[33];                // 802.11 SSID max 32 bytes
    char wifi_password[64];            // WPA2 password max 63 bytes
    char fmo_host[FMO_HOST_MAX];       // "192.168.1.100" 或 "example.com:8080"
    bool ddns_remote;                  // 是否为远程DDNS连接
};

/* ── 全局配置结构 ───────────────────────────────────────────────── */
struct FMO_Settings {
    // 连接配置
    FMO_Profile profiles[PROFILE_MAX];
    uint8_t active_profile;            // 当前激活配置 0-4

    // 音频
    uint8_t audio_volume;              // 0-100
    bool    audio_muted;               // 静音状态
    char    audio_tx_ip[17];           // 语音发射目标 IP（UDP 发送，运行时设置）

    // 显示
    uint8_t backlight_percent;         // 0-100

    // 本机呼号 (可从FMO同步)
    char owner_callsign[16];

    // QSO同步状态
    uint32_t qso_count;
    int32_t  qso_latest_log_id;
    bool     qso_count_valid;

    // 蓝牙音频
    bool     bt_enabled;
    bool     vibrate_enabled;          // 振动提醒开关
    char     bt_target_name[32];
};

/* ── 配置管理器 ─────────────────────────────────────────────────── */
class FMO_SettingsMgr {
public:
    bool begin() {
        _prefs.begin("fmo-watch", false);
        if (!_prefs.isKey("version") || _prefs.getUInt("version") != SETTINGS_VERSION) {
            loadDefaults();
            save();
        } else {
            load();
        }
        return true;
    }

    void loadDefaults() {
        memset(&_cfg, 0, sizeof(_cfg));

        // 配置1: 默认占位
        strncpy(_cfg.profiles[0].name, "Config 1", PROFILE_NAME_MAX - 1);
        strncpy(_cfg.profiles[0].wifi_ssid, DEFAULT_WIFI_SSID, 32);
        strncpy(_cfg.profiles[0].wifi_password, DEFAULT_WIFI_PASSWORD, 63);
        strncpy(_cfg.profiles[0].fmo_host, DEFAULT_FMO_HOST, FMO_HOST_MAX - 1);
        _cfg.profiles[0].ddns_remote = false;

        // 配置2-5: 空
        for (int i = 1; i < PROFILE_MAX; i++) {
            snprintf(_cfg.profiles[i].name, PROFILE_NAME_MAX, "Config %d", i + 1);
        }

        _cfg.active_profile = 0;
        _cfg.audio_volume = 60;
        _cfg.audio_muted = true;
        strncpy(_cfg.audio_tx_ip, FMO_AUDIO_RX_IP, sizeof(_cfg.audio_tx_ip) - 1);
        _cfg.audio_tx_ip[sizeof(_cfg.audio_tx_ip) - 1] = 0;
        _cfg.backlight_percent = 30;
        strncpy(_cfg.owner_callsign, DEFAULT_OWNER_CALLSIGN, 15);
        _cfg.qso_count = 0;
        _cfg.qso_latest_log_id = -1;
        _cfg.qso_count_valid = false;
        _cfg.bt_enabled = BT_ENABLE_DEFAULT;
        _cfg.vibrate_enabled = true;
        _cfg.bt_target_name[0] = '\0';
    }

    bool load() {
        size_t len = _prefs.getBytes("cfg", &_cfg, sizeof(_cfg));
        return (len == sizeof(_cfg));
    }

    bool save() {
        _prefs.putUInt("version", SETTINGS_VERSION);
        size_t written = _prefs.putBytes("cfg", &_cfg, sizeof(_cfg));
        return (written == sizeof(_cfg));
    }

    // ── 访问器 ──
    FMO_Settings*       data()        { return &_cfg; }
    FMO_Profile*        activeProfile() { return &_cfg.profiles[_cfg.active_profile]; }
    uint8_t             activeIndex() { return _cfg.active_profile; }

    bool setActiveProfile(uint8_t idx) {
        if (idx >= PROFILE_MAX) return false;
        _cfg.active_profile = idx;
        return save();
    }

    bool setProfileWiFi(uint8_t idx, const char* ssid, const char* pass) {
        if (idx >= PROFILE_MAX) return false;
        if (ssid) strncpy(_cfg.profiles[idx].wifi_ssid, ssid, 32);
        if (pass) strncpy(_cfg.profiles[idx].wifi_password, pass, 63);
        return save();
    }

    bool setProfileFmoHost(uint8_t idx, const char* host) {
        if (idx >= PROFILE_MAX || !host) return false;
        strncpy(_cfg.profiles[idx].fmo_host, host, FMO_HOST_MAX - 1);
        return save();
    }

    bool setProfileDDNS(uint8_t idx, bool enabled) {
        if (idx >= PROFILE_MAX) return false;
        _cfg.profiles[idx].ddns_remote = enabled;
        return save();
    }

    bool setVolume(uint8_t vol) {
        if (vol > 100) vol = 100;
        _cfg.audio_volume = vol;
        return save();
    }

    bool setMuted(bool muted) {
        _cfg.audio_muted = muted;
        return save();
    }

    bool setBacklight(uint8_t pct) {
        if (pct > 100) pct = 100;
        _cfg.backlight_percent = pct;
        return save();
    }

    bool setOwnerCallsign(const char* call) {
        if (!call) return false;
        strncpy(_cfg.owner_callsign, call, 15);
        _cfg.owner_callsign[15] = '\0';
        // 转大写
        for (char* p = _cfg.owner_callsign; *p; p++) {
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        }
        return save();
    }

    // 语音发射目标 IP（运行时设置，不用重新编译）
    bool setAudioTxIp(const char* ip) {
        if (!ip) return false;
        strncpy(_cfg.audio_tx_ip, ip, sizeof(_cfg.audio_tx_ip) - 1);
        _cfg.audio_tx_ip[sizeof(_cfg.audio_tx_ip) - 1] = 0;
        return save();
    }
    const char* audioTxIp() const { return _cfg.audio_tx_ip; }

    bool setQsoState(uint32_t count, int32_t latest_log_id, bool valid) {
        _cfg.qso_count = count;
        _cfg.qso_latest_log_id = latest_log_id;
        _cfg.qso_count_valid = valid;
        return save();
    }

    bool setVibrateEnabled(bool en) {
        _cfg.vibrate_enabled = en;
        return save();
    }

    bool setBTEnabled(bool en) {
        _cfg.bt_enabled = en;
        return save();
    }

    bool setBTTargetName(const char* name) {
        if (name) strncpy(_cfg.bt_target_name, name, 31);
        else _cfg.bt_target_name[0] = '\0';
        return save();
    }

    // ── 构建 WebSocket URL ──
    void buildWsUrls(const FMO_Profile* p, char* audio_url, char* event_url, char* station_url, int max_len) {
        if (!p || !audio_url || !event_url || !station_url) return;
        snprintf(audio_url,   max_len, "ws://%s/audio",   p->fmo_host);
        snprintf(event_url,   max_len, "ws://%s/events",  p->fmo_host);
        snprintf(station_url, max_len, "ws://%s/ws",      p->fmo_host);
    }

    // ── WiFi 记忆库：保存连接过的 WiFi（SSID+密码），断开后自动重连 ──
    // 复用 profiles[1..4] 作为记忆槽（profiles[0] 是当前激活配置）

    /** 连接成功后保存凭证：同 SSID 复用槽位，否则存入空槽（最多4个记忆） */
    bool saveWifiCredential(const char* ssid, const char* pass) {
        if (!ssid || !ssid[0]) return false;
        // 1. 已存在同 SSID 的槽 → 更新密码
        for (int i = 1; i < PROFILE_MAX; i++) {
            if (strcmp(_cfg.profiles[i].wifi_ssid, ssid) == 0) {
                if (pass) strncpy(_cfg.profiles[i].wifi_password, pass, 63);
                _cfg.profiles[i].wifi_ssid[sizeof(_cfg.profiles[i].wifi_ssid)-1] = 0;
                _cfg.profiles[i].wifi_password[sizeof(_cfg.profiles[i].wifi_password)-1] = 0;
                return save();
            }
        }
        // 2. 找空槽
        for (int i = 1; i < PROFILE_MAX; i++) {
            if (!_cfg.profiles[i].wifi_ssid[0]) {
                strncpy(_cfg.profiles[i].wifi_ssid, ssid, 32);
                strncpy(_cfg.profiles[i].wifi_password, pass ? pass : "", 63);
                _cfg.profiles[i].wifi_ssid[sizeof(_cfg.profiles[i].wifi_ssid)-1] = 0;
                _cfg.profiles[i].wifi_password[sizeof(_cfg.profiles[i].wifi_password)-1] = 0;
                return save();
            }
        }
        return false;   // 记忆槽已满
    }

    /** 查找已保存的 SSID 密码（找到返回 true，pass 输出密码） */
    bool findWifiCredential(const char* ssid, char* pass, int passLen) {
        if (!ssid || !ssid[0] || !pass) return false;
        for (int i = 1; i < PROFILE_MAX; i++) {
            if (strcmp(_cfg.profiles[i].wifi_ssid, ssid) == 0) {
                strncpy(pass, _cfg.profiles[i].wifi_password, passLen - 1);
                pass[passLen - 1] = 0;
                return true;
            }
        }
        return false;
    }

    /** 已保存的 WiFi 数量（记忆槽） */
    int wifiCredentialCount() {
        int n = 0;
        for (int i = 1; i < PROFILE_MAX; i++)
            if (_cfg.profiles[i].wifi_ssid[0]) n++;
        return n;
    }

    /** 获取第 idx 个已保存 WiFi 的 SSID */
    const char* wifiCredentialSsid(int idx) {
        int n = 0;
        for (int i = 1; i < PROFILE_MAX; i++) {
            if (_cfg.profiles[i].wifi_ssid[0]) {
                if (n == idx) return _cfg.profiles[i].wifi_ssid;
                n++;
            }
        }
        return nullptr;
    }

private:
    static const uint32_t SETTINGS_VERSION = 2;   // v2: 新增 audio_tx_ip 字段
    Preferences _prefs;
    FMO_Settings _cfg;
};
