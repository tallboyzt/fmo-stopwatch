/**
 * fwmo_battery.h — 电池监测（M5PM1 PMIC I2C 读取，参考官方固件 hal_pmic.cpp）
 *
 * M5StopWatch C152 使用 M5PM1 PMIC，电池电压通过内部 I2C 读取（非 GPIO ADC）。
 * 电池标称容量：450mAh（BAT_CAPACITY_MAH，见 fwmo_config.h）
 *
 * 参考官方固件 demo/main/hal/hal_pmic.cpp：
 *   1. M5PM1 readVbat 读取电池电压（mV）
 *   2. 3300mV → 0%、4200mV → 100% 线性映射
 *   3. 7:1 滑动滤波（_bat_filter_weight_old=7, new=1）
 *   4. 每秒读取一次
 *
 * 说明：M5PM1 无电池电流寄存器，百分比采用电压映射（与官方固件一致），
 *       容量 450mAh 仅用于充电电流匹配与文档记录，不参与百分比计算。
 *
 * 依赖：M5Unified（已内置 M5PM1_Class，board_M5StopWatch 自动识别为 pmic_m5pm1）
 */
#pragma once
#include "fwmo_config.h"
#include <M5Unified.h>
#include <Arduino.h>

class FMO_Battery {
public:
    /** 初始化（M5.begin() 已初始化 PMIC，无需额外操作） */
    bool begin() {
        _lastRead   = 0;
        _voltage_mv = 0;
        _percent    = 0;
        _charging   = false;
        // 立即读取一次真实电量，避免开机瞬间 UI 显示空电量（等1秒才更新）
        loop();
        return true;
    }

    /**
     * 快速充电状态检测（每1秒调用，仅读VBUS电压，不重算电量）
     * 在 loop() 中与慢速电压读取并行使用
     */
    void updateCharging() {
        _charging = (M5.Power.getVBUSVoltage() > 4000);
    }

    /** 周期读取（每10秒，在loop()中调用） */
    void loop() {
        uint32_t now = millis();
        if (now - _lastRead < BAT_READ_INTERVAL_MS) return;
        _lastRead = now;

        // M5PM1 PMIC 通过 I2C 读取电池电压（官方固件同款方式）
        int16_t mv = M5.Power.getBatteryVoltage();
        if (mv <= 0) return;  // 读取失败，保持上次值

        // 7:1 滑动滤波（参考官方固件）
        if (_voltage_mv == 0) {
            _voltage_mv = (uint16_t)mv;
        } else {
            _voltage_mv = (uint16_t)(((uint32_t)_voltage_mv * 7 + mv + 4) / 8);
        }

        // 线性映射：电池空(3300mV) → 0%，满(4200mV) → 100%（官方固件值）
        if (_voltage_mv >= BAT_FULL_MV) {
            _percent = 100;
        } else if (_voltage_mv <= BAT_EMPTY_MV) {
            _percent = 0;
        } else {
            _percent = (uint8_t)((_voltage_mv - BAT_EMPTY_MV) * 100
                                 / (BAT_FULL_MV - BAT_EMPTY_MV));
        }

        // 充电检测：VBUS 电压 > 4000mV 视为外接电源（参考官方固件）
        _charging = (M5.Power.getVBUSVoltage() > 4000);
    }

    // ── 访问器 ──
    uint16_t voltage_mv() const { return _voltage_mv; }  // 电池电压(mV)
    uint8_t  percent()    const { return _percent;    }  // 电量百分比(0-100)
    bool     charging()   const { return _charging;   }  // 是否充电中
    bool     lowBattery() const { return _percent < 10; } // 低电量(<10%)

private:
    uint16_t _voltage_mv;   // 电池电压（毫伏）
    uint8_t  _percent;      // 电量百分比
    bool     _charging;     // 充电状态
    uint32_t _lastRead;     // 上次读取时间
};
