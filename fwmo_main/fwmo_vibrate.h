/**
 * fwmo_vibrate.h — 振动马达驱动
 *
 * GPIO8 = M5StopWatch 振动马达（用户实测确认）。
 * 注意：GPIO46 是 PSRAM 数据线，绝不可用作输出（会花屏）。
 */
#pragma once
#include "fwmo_config.h"
#include <Arduino.h>

class FMO_Vibrate {
public:
    /** 初始化振动马达 GPIO（GPIO8） */
    bool begin() {
        pinMode(PIN_VIBRATE, OUTPUT);
        digitalWrite(PIN_VIBRATE, LOW);
        _active     = false;
        _pattern[0] = _pattern[1] = _pattern[2] = 0;
        _patternIdx = 0;
        _patternEnd = 0;
        _lastTick   = 0;
        return true;
    }

    /** 振动模式 */
    enum Pattern {
        VIBE_NONE       = 0,     // 无振动
        VIBE_SHORT_ONCE,         // 短振1次 → 普通通联提醒
        VIBE_SHORT_TWICE,        // 短振2次 → 今日首次通联
        VIBE_SHORT_THREE,        // 短振3次 → 本机呼号被提及
        VIBE_LONG_ONCE,          // 长振1次 → 新呼号从未通联
    };

    /** 触发振动（非阻塞，立即返回） */
    void trigger(Pattern p) {
        if (_active) return;  // 不打断正在进行的振动

        switch (p) {
        case VIBE_SHORT_ONCE:
            _pattern[0] = VIBRATE_SHORT_MS;
            _pattern[1] = 0;
            _patternEnd = 2;
            break;
        case VIBE_SHORT_TWICE:
            _pattern[0] = VIBRATE_SHORT_MS;
            _pattern[1] = 150;          // 两次振动间隔 150ms
            _pattern[2] = VIBRATE_SHORT_MS;
            _pattern[3] = 0;
            _patternEnd = 4;
            break;
        case VIBE_SHORT_THREE:
            _pattern[0] = VIBRATE_SHORT_MS;
            _pattern[1] = 120;
            _pattern[2] = VIBRATE_SHORT_MS;
            _pattern[3] = 120;
            _pattern[4] = VIBRATE_SHORT_MS;
            _pattern[5] = 0;
            _patternEnd = 6;
            break;
        case VIBE_LONG_ONCE:
            _pattern[0] = VIBRATE_LONG_MS;
            _pattern[1] = 0;
            _patternEnd = 2;
            break;
        default:
            return;
        }

        _patternIdx = 0;
        _active     = true;
        _lastTick   = millis();
        // Pattern[0]开始 → 偶数为ON，奇数为OFF
        digitalWrite(PIN_VIBRATE, (_patternIdx % 2 == 0) ? HIGH : LOW);
    }

    /** 轮询状态机（在 loop() 中调用） */
    void loop() {
        if (!_active) return;

        uint32_t now     = millis();
        uint32_t elapsed = now - _lastTick;
        uint32_t dur     = _pattern[_patternIdx];

        if (elapsed >= (dur > 0 ? dur : 1)) {
            _patternIdx++;
            _lastTick = now;

            // 检查是否结束
            if (_patternIdx >= _patternEnd || _pattern[_patternIdx] == 0) {
                digitalWrite(PIN_VIBRATE, LOW);
                _active = false;
                return;
            }

            // 切换到下一个阶段
            bool on = (_patternIdx % 2 == 0);   // 偶数→ON，奇数→OFF
            digitalWrite(PIN_VIBRATE, on ? HIGH : LOW);
        }
    }

    bool isActive() const { return _active; }

private:
    bool     _active;                          // 振动是否进行中
    uint16_t _pattern[6];                      // 模式数组 [ON_ms, OFF_ms, ON_ms, ...]
    int      _patternIdx;                      // 当前阶段索引
    int      _patternEnd;                      // 模式结束索引
    uint32_t _lastTick;                        // 上次状态切换时间
};
