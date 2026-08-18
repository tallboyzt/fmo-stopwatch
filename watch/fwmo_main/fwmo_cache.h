/**
 * fwmo_cache.h — QSO 呼号缓存（通联历史记录）
 *
 * 功能：
 *   1. 记录每次通联的呼号和时间
 *   2. 判断呼号提醒级别（新呼号/今日未通联/15分钟内通联/普通）
 *   3. 定期清理过期记录（>7天）→ 异步 FreeRTOS 任务
 *   4. 保持缓存不超过 200 条
 *
 * 提醒级别：
 *   ALERT_NEVER      → 从未通联过的新呼号 → 长振1次
 *   ALERT_NOT_TODAY  → 今天还没有通联    → 短振2次
 *   ALERT_RECENT_15M → 15分钟内刚通联    → 无提醒
 *   ALERT_NORMAL     → 普通通联          → 短振1次
 */
#pragma once
#include "fwmo_config.h"
#include <Arduino.h>
#include <time.h>
#include <map>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class FMO_Cache {
public:
    /** 呼号提醒级别 */
    enum CallAlert {
        ALERT_NEVER    = 0,   // 从未通联过 → 新呼号
        ALERT_NOT_TODAY,      // 今日首次通联
        ALERT_RECENT_15M,     // 15分钟内通联过
        ALERT_NORMAL,         // 普通（今天已通联）
    };

    /** 记录一次通联（呼号 + 时间戳） */
    void recordCall(const char* callsign) {
        if (!callsign || !callsign[0]) return;
        std::string cs(callsign);
        _calls[cs] = millis();
    }

    /**
     * 判断呼号提醒级别
     * @param callsign  目标呼号
     * @param ownerCall 本机呼号（忽略匹配）
     */
    CallAlert classify(const char* callsign, const char* ownerCall) {
        if (!callsign || !callsign[0]) return ALERT_NORMAL;

        // 忽略本机呼号
        if (ownerCall && strcasecmp(callsign, ownerCall) == 0)
            return ALERT_NORMAL;

        std::string cs(callsign);
        auto it = _calls.find(cs);

        if (it == _calls.end()) {
            return ALERT_NEVER;                    // 从未通联
        }

        uint32_t lastCall = it->second;
        uint32_t now      = millis();

        if (now - lastCall < 15 * 60 * 1000UL) {
            return ALERT_RECENT_15M;               // 15分钟内
        }

        if (isToday(lastCall)) {
            return ALERT_NORMAL;                   // 今天已通联
        }

        return ALERT_NOT_TODAY;                    // 今天未通联
    }

    /** 检查毫秒时间戳是否在今天 */
    bool isToday(uint32_t ts_ms) {
        time_t now_time; time(&now_time);
        struct tm now_tm, ts_tm;
        localtime_r(&now_time, &now_tm);
        time_t ts = ts_ms / 1000;
        localtime_r(&ts, &ts_tm);

        return (now_tm.tm_year == ts_tm.tm_year &&
                now_tm.tm_mon  == ts_tm.tm_mon  &&
                now_tm.tm_mday == ts_tm.tm_mday);
    }

    int size() const { return _calls.size(); }

    /** 获取指定呼号的最后通联时间（毫秒），0=未找到 */
    uint32_t lastCallTime(const char* callsign) {
        auto it = _calls.find(std::string(callsign));
        return (it != _calls.end()) ? it->second : 0;
    }

    // ═══════════════════════════════════════════════
    // 异步清理（FreeRTOS 任务）
    // ═══════════════════════════════════════════════

    /** 启动异步清理 → 立即返回，后台清理 */
    void startCleanup() {
        if (_cleanupRunning) return;
        _cleanupRunning = true;
        xTaskCreatePinnedToCore(
            cleanupTask, "cache_clean",
            CACHE_CLEAN_TASK_STACK,
            this,
            CACHE_CLEAN_TASK_PRIO,
            nullptr,
            CACHE_CLEAN_TASK_CORE);
    }

    /** 同步清理（阻塞，保留兼容） */
    void cleanup() {
        uint32_t now    = millis();
        uint32_t cutoff = 7 * 24 * 60 * 60 * 1000UL;  // 7天过期

        // 收集过期条目（>7天未通联）
        std::vector<std::string> toRemove;
        toRemove.reserve(_calls.size());
        for (auto& kv : _calls) {
            if (now - kv.second > cutoff) toRemove.push_back(kv.first);
        }
        for (auto& cs : toRemove) _calls.erase(cs);

        // 保持缓存不超过200条（删除最旧记录）
        while (_calls.size() > 200) {
            auto oldest = _calls.begin();
            for (auto it = _calls.begin(); it != _calls.end(); ++it) {
                if (it->second < oldest->second) oldest = it;
            }
            _calls.erase(oldest);
        }
    }

private:
    std::map<std::string, uint32_t> _calls;           // 呼号 → 最后通联时间(ms)
    volatile bool _cleanupRunning = false;

    /** 清理任务入口 */
    static void cleanupTask(void* arg) {
        FMO_Cache* self = (FMO_Cache*)arg;
        self->cleanup();
        self->_cleanupRunning = false;
        vTaskDelete(NULL);
    }
};
