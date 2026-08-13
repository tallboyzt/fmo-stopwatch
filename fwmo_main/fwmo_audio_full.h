/**
 * fwmo_audio_full.h — ES8311 全双工音频（参考 xiaozhi-esp32）
 *
 * 核心：单个 I2S_NUM_0 创建 TX+RX 双通道（全双工）！
 *   TX → DOUT=21（扬声器），RX ← DIN=16（麦克风）
 *   时钟 18/17/15 只由一个 I2S 外设驱动 → 彻底解决 M5Unified 分开管理(I2S0/I2S1)的冲突
 *
 * 引脚（M5StopWatch C152）：
 *   MCLK=18, BCLK=17, WS=15, DOUT=21(扬声器), DIN=16(麦克风)
 *   PA 使能 = M5IOE1_G10（IO 扩展器引脚 9）
 *
 * ES8311 I2C 地址 0x18，寄存器序列参考 M5Unified（DAC+ADC 合并全双工）
 */
#pragma once
#include <M5Unified.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ES8311 I2C 地址
#define ES8311_ADDR  0x18

class FMO_AudioFull {
public:
    // ── 初始化：单 I2S0 全双工 + ES8311 配置 ──
    bool begin() {
        if (_ready) return true;
        _ready = CreateDuplexChannels() && CodecInit();
        if (_ready) setPa(true);   // 默认开功放（扬声器有声）
        return _ready;
    }

    // ── 全双工通道创建（参考 xiaozhi CreateDuplexChannels） ──
    bool CreateDuplexChannels() {
        i2s_chan_config_t chan_cfg = {
            .id = I2S_NUM_0,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = 8,
            .dma_frame_num = 256,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
        };
        esp_err_t err = i2s_new_channel(&chan_cfg, &_tx, &_rx);
        if (err != ESP_OK) { Serial.printf("[AudioFull] i2s_new_channel: %d\n", err); return false; }

        i2s_std_config_t std_cfg = {
            .clk_cfg = {
                .sample_rate_hz = 8000,      // 全链路 8kHz（WS 音频直出，不做升采样，音质最保真）
                .clk_src = I2S_CLK_SRC_PLL_160M,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = I2S_STD_SLOT_BOTH,
                .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false,
            },
            .gpio_cfg = {
                .mclk = GPIO_NUM_18,
                .bclk = GPIO_NUM_17,
                .ws = GPIO_NUM_15,
                .dout = GPIO_NUM_21,
                .din = GPIO_NUM_16,
                .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
            },
        };
        err = i2s_channel_init_std_mode(_tx, &std_cfg);
        if (err != ESP_OK) { Serial.printf("[AudioFull] tx init: %d\n", err); return false; }
        err = i2s_channel_init_std_mode(_rx, &std_cfg);
        if (err != ESP_OK) { Serial.printf("[AudioFull] rx init: %d\n", err); return false; }
        err = i2s_channel_enable(_tx);
        if (err != ESP_OK) { Serial.printf("[AudioFull] tx enable: %d\n", err); return false; }
        err = i2s_channel_enable(_rx);
        if (err != ESP_OK) { Serial.printf("[AudioFull] rx enable: %d\n", err); return false; }
        Serial.println("[AudioFull] 全双工 I2S0 TX+RX 就绪");
        return true;
    }

    // ── ES8311 全双工配置（合并 DAC+ADC 寄存器，参考 M5Unified） ──
    bool CodecInit() {
        // 开音频电源 (M5IOE1_G3=2)
        _ioe(true, 2);   // Audio Power
        delay(10);
        // ES8311 寄存器序列（0x00 RESET 开始，全双工 DAC+ADC）
        static const uint8_t regs[][3] = {
            {0x00, 0x80},  // RESET / CSM POWER ON
            {0x01, 0xB5},  // CLOCK_MANAGER / MCLK=BCLK（Speaker 配置，DAC 优先）
            {0x02, 0x18},  // CLOCK_MANAGER / MULT_PRE=3
            {0x0D, 0x01},  // SYSTEM / Power up analog
            {0x0E, 0x02},  // SYSTEM / Enable PGA + ADC
            {0x12, 0x00},  // SYSTEM / power-up DAC
            {0x13, 0x10},  // SYSTEM / Enable output to HP
            {0x14, 0x10},  // ADC / Mic1p-Mic1n / PGA GAIN min
            {0x17, 0xFF},  // ADC / ADC_VOLUME max
            {0x1C, 0x6A},  // ADC / EQ bypass + DC offset cancel
            {0x32, 0xEF},  // DAC / DAC volume (+6dB，M5Unified 默认；全双工无 M5GFX magnification 需靠 DAC 增益补足)
            {0x37, 0x08},  // DAC / Bypass DAC equalizer
        };
        bool allOk = true;
        for (auto& r : regs) {
            if (!_i2cWrite(r[0], r[1])) {
                Serial.printf("[AudioFull] ES8311 reg 0x%02X 写失败\n", r[0]);
                allOk = false;
            }
        }
        if (!allOk) return false;

        // ── 读回关键寄存器验证（ADC/DAC 配置诊断日志） ──
        delay(20);
        Serial.println("[AudioFull] ── ES8311 寄存器读回 ──");
        static const uint8_t keys[] = {0x00,0x01,0x02,0x0D,0x0E,0x12,0x13,0x14,0x17,0x1C,0x32,0x37};
        for (uint8_t reg : keys) {
            uint8_t v = _i2cRead(reg);
            Serial.printf("[AudioFull]   reg 0x%02X = 0x%02X%s\n", reg, v,
                          (v == 0xFF) ? "  (READ ERR?)" : "");
        }
        Serial.println("[AudioFull] ── ES8311 配置完成 ──");
        return true;
    }

    // ── PA 控制（M5IOE1_G10=9） ──
    void setPa(bool en) { _ioe(en, 9); }

    // ── 播放（TX 通道） ──
    bool writeTx(const int16_t* data, int samples) {
        if (!_tx) return false;
        size_t written = 0;
        esp_err_t e = i2s_channel_write(_tx, data, samples * sizeof(int16_t), &written, 100 / portTICK_PERIOD_MS);
        if (e != ESP_OK || written < samples * sizeof(int16_t)) return false;   // 超时/未写完 → 丢帧标记
        return true;
    }

    // ── 采集（RX 通道） ──
    int readRx(int16_t* data, int samples, int timeoutMs = 20) {
        if (!_rx) return 0;
        size_t read = 0;
        esp_err_t e = i2s_channel_read(_rx, data, samples * sizeof(int16_t), &read, timeoutMs / portTICK_PERIOD_MS);
        if (e != ESP_OK) return 0;
        return read / sizeof(int16_t);
    }

private:
    i2s_chan_handle_t _tx = nullptr;
    i2s_chan_handle_t _rx = nullptr;
    bool _ready = false;

    // IO 扩展器写（M5IOE1）
    void _ioe(bool level, uint8_t pin) {
        auto& ioe = M5.getIOExpander(0);
        ioe.digitalWrite(pin, level);
    }

    // ES8311 I2C 写（内部 I2C，SDA=47/SCL=48）
    bool _i2cWrite(uint8_t reg, uint8_t val) {
        // 用 M5Unified 的 I2C 实例（In_I2C）写 ES8311（writeRegister8 返回 bool）
        return M5.In_I2C.writeRegister8(ES8311_ADDR, reg, val, 100000);
    }

    // ES8311 I2C 读回（诊断用）
    uint8_t _i2cRead(uint8_t reg) {
        return M5.In_I2C.readRegister8(ES8311_ADDR, reg, 100000);
    }
};
