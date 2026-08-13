// fwmo_ui_lvgl.h — LVGL v8 UI (touch + physical keys)
#pragma once
#include <lvgl.h>
#include <M5GFX.h>
#include <time.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "fwmo_config.h"
#include "fwmo_settings.h"
#include "fwmo_wifi.h"
#include "fwmo_audio.h"
#include "fwmo_battery.h"
#include <LittleFS.h>
#include "fwmo_client.h"
#include "fwmo_weather.h"
extern const lv_img_dsc_t* wicon_table[];   // 天气图标A8图（wicon.c，按Cat索引）

// 思源黑体（粗体）中文字体：24px，含 ASCII + GB2312 一级汉字
// 字库文件 font_hansan_24.bin 存 LittleFS，运行时加载到 PSRAM
#include "fwmo_font.h"
#include "fwmo_callloc.h"   // 呼号→城市 离线查询
extern FMO_CallLoc* _g_callLoc;   // 主程序中初始化（fwmo_main.ino）

// 呼号用的放大字体（在 setup 中由 font_hansan_24_load() 获取后赋给 _callFont）
extern lv_font_t * _g_font_hansan_24;

// M5UNIFIED_VERSION 是 F() 包装宏（ESP32 的 F() 只接受字符串字面量，直接展开会编译错误），
// 这里用字符串化宏将三个版本号拼接为普通字符串
#define _FMO_STR_HELPER(x) #x
#define _FMO_STR(x) _FMO_STR_HELPER(x)
#define FMO_M5UNIFIED_VER _FMO_STR(M5UNIFIED_VERSION_MAJOR) "." _FMO_STR(M5UNIFIED_VERSION_MINOR) "." _FMO_STR(M5UNIFIED_VERSION_PATCH)

// ═══ 三模式九宫格键盘（字母/数字/符号，参照手机九宫格布局）═══
// ── 字母界面（T9 九宫格）：每键点击依次循环输入字母（a→b→c→2→a...）──
static const char * fmo_kb_map_alpha[] = {
  "1",     "2abc", "3def", "\n",
  "4ghi",  "5jkl", "6mno", "\n",
  "7pqrs", "8tuv", "9wxyz","\n",
  LV_SYMBOL_UP, "0",    LV_SYMBOL_BACKSPACE, "\n",
  "123",   " ",    LV_SYMBOL_OK, NULL
};
// ── 字母界面大写版（↑ 键切换；仅字母变大写，布局与 fmo_kb_map_alpha 完全一致）──
static const char * fmo_kb_map_alpha_upper[] = {
  "1",     "2ABC", "3DEF", "\n",
  "4GHI",  "5JKL", "6MNO", "\n",
  "7PQRS", "8TUV", "9WXYZ","\n",
  LV_SYMBOL_UP, "0",    LV_SYMBOL_BACKSPACE, "\n",
  "123",   " ",    LV_SYMBOL_OK, NULL
};
// ── 数字界面 ──
static const char * fmo_kb_map_num[] = {
  "1", "2", "3", "\n",
  "4", "5", "6", "\n",
  "7", "8", "9", "\n",
  "ABC", "0", LV_SYMBOL_BACKSPACE, "\n",
  "#+=", " ", LV_SYMBOL_OK, NULL
};
// ── 符号界面 ──
static const char * fmo_kb_map_sym[] = {
  ".", ",", "?", "\n",
  "!", "@", "#", "\n",
  "$", "%", "&", "\n",
  "-", "_", LV_SYMBOL_BACKSPACE, "\n",
  "123", " ", LV_SYMBOL_OK, NULL
};
// 与各 map 一一对应的控制标志（每界面 15 键；⌫ 禁连发）。
// 必须传非 NULL，否则 lv_keyboard_update_ctrl_map 内 lv_memcpy 从空指针崩溃
static const lv_btnmatrix_ctrl_t fmo_kb_ctrl_17[] = {
  0,0,0,  0,0,0,  0,0,0,
  0,0, LV_BTNMATRIX_CTRL_NO_REPEAT,
  0,0,0
};

// 判断字符串是否含非 ASCII（中文等），用于决定是否切换中文字体
static bool _strHasNonAscii(const char *s)
{
    if (!s) return false;
    for (const char *p = s; *p; p++)
        if ((unsigned char)*p >= 0x80) return true;
    return false;
}

// 过滤显示文本中的特殊字符（UTF-8）：
// 保留：汉字(0x4E00-0x9FFF)、ASCII 可打印、常见符号(•·、，。！？：；（）《》空格等已在字库)
// 其他（emoji/生僻符号/乱码）替换为空格，避免 LVGL 渲染成方块
static void _sanitizeDisplay(const char *in, char *out, int maxLen)
{
    if (!in || !out || maxLen < 2) { if (out) out[0] = 0; return; }
    int o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o < maxLen - 4) {
        unsigned char c = *p;
        if (c < 0x80) {
            // ASCII 可打印保留，控制符/其他替换为空格
            if (c >= 0x20 && c != 0x7F) out[o++] = (char)c;
            else out[o++] = ' ';
            p++;
        } else if ((c & 0xE0) == 0xC0 && o < maxLen - 2 && (p[1] & 0xC0) == 0x80) {
            uint32_t cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
            if (cp >= 0x4E00 && cp <= 0x9FFF) { out[o++] = (char)0xE4 | ((cp >> 12) & 0x0F); out[o++] = (char)0x80 | ((cp >> 6) & 0x3F); out[o++] = (char)0x80 | (cp & 0x3F); }
            else out[o++] = ' ';
        } else if ((c & 0xE0) == 0xC0) {
            p++; out[o++] = ' ';   // 2字节但非法
        } else if ((c & 0xF0) == 0xE0 && o < maxLen - 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            uint32_t cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
            // 保留：CJK 统一表意文字（含生僻字）、常见全角符号 •·、，。！？：；（）《》【】“”‘’
            if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
                cp == 0x2022 || cp == 0x00B7 || cp == 0x3001 || cp == 0x3002 ||
                cp == 0xFF01 || cp == 0xFF1F || cp == 0xFF1A || cp == 0xFF1B ||
                cp == 0xFF08 || cp == 0xFF09 || cp == 0x300A || cp == 0x300B ||
                cp == 0x3010 || cp == 0x3011 || cp == 0x201C || cp == 0x201D ||
                cp == 0x2018 || cp == 0x2019 || cp == 0xFF0C || cp == 0x3000) {
                out[o++] = (char)(0xE0 | ((cp >> 12) & 0x0F));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[o++] = ' ';
            }
        } else {
            // 4字节(emoji等)或非法：跳过整个序列，替换为空格
            int skip = (c & 0xF8) == 0xF0 ? 4 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 1));
            p += skip;
            out[o++] = ' ';
        }
    }
    out[o] = 0;
}

class FMO_UI_LVGL
{
public:
    FMO_UI_LVGL() {}
    FMO_WifiManager *wifi = nullptr;
    FMO_SettingsMgr *settings = nullptr;
    FMO_Client *fmo = nullptr;
    FMO_Audio *audio = nullptr;
    FMO_Weather *weather = nullptr;  // 天气数据（wttr.in）
    FMO_Bluetooth *bt = nullptr;     // 蓝牙（扫描/连接）
    FMO_Battery *battery = nullptr;  // 电池（设备信息页）
    int _batPercent = 0;
    bool _batCharging = false;   // 充电状态（由主程序更新）
    bool _btConnected = false;
    bool _micTx = false;         // 麦克风发送中（主程序更新）
    bool _wifiPwWait = false;    // WiFi 密码确认后等待连接结果
    lv_timer_t* _wifiPwTimer = nullptr;   // 连接结果检查定时器
    // ── 息屏动画（长按A+B息屏） ──
    bool _screenOff = false;          // 是否息屏中
    lv_obj_t* _offOverlay = nullptr;  // 黑色覆盖层
    lv_obj_t* _offDot = nullptr;      // 摩斯码圆点（绿）
    lv_timer_t* _offTimer = nullptr;  // 息屏动画定时器
    int _offPhase = 0;                // 动画相位
    // 摩斯码状态
    static const int MORSE_MAX = 160;
    uint16_t _morseDur[MORSE_MAX];    // 每段时长（tick 数）
    bool _morseOn[MORSE_MAX];         // 每段亮/灭
    int _morseCnt = 0;                // 总段数
    int _morseIdx = 0;                // 当前段
    int _morseTick = 0;               // 当前段剩余 tick
    int _wifiStatus = 0;
    uint32_t _wifiStatTime = 0;
    enum Page
    {
        PG_HOME,
        PG_SETTINGS,
        PG_WIFI_MENU,
        PG_WIFI_SCAN,
        PG_FMO_HOST,
        PG_AUDIO_SET,
        PG_STATION_LIST,
        PG_CALLSIGN,
        PG_DEVINFO,
        PG_ABOUT,
        PG_VOLUME,
        PG_BT_SCAN,
        PG_LOOKUP_RESULT
    };
    enum InputMode
    {
        INP_NONE,
        INP_SSID,
        INP_PASS,
        INP_HOST,
        INP_PORT,
        INP_CALLSIGN,
        INP_LOOKUP,
        INP_AUDIO_IP
    };

    void setDisplay(M5GFX *d) { _disp = d; }
    bool menuOpen() const { return _menuOpen; }   // 菜单是否打开（供主程序判断）
    bool busy() const { return _menuOpen || _inp != INP_NONE; }  // 非首页状态
    bool onStationList() const { return _pg == PG_STATION_LIST; }  // 当前是否在台站列表页
    bool onWifiScan() const { return _pg == PG_WIFI_SCAN; }       // 当前是否在WiFi扫描页
    bool wifiScanBusy() const { return _scanning && (!wifi || wifi->scanState() == SCAN_RUNNING); }  // WiFi扫描是否进行中（查真实状态）
    // 扫描是否"已完成但结果还没构建到菜单"（供主程序触发一次刷新；不消费_scanning）
    bool wifiScanPending() {
        if (!_scanning) return false;
        if (!wifi) return false;
        FMO_ScanState st = wifi->scanState();
        return (st == SCAN_DONE || st == SCAN_ERROR);
    }
    void refreshMenu() {
        Serial.printf("[UI] refreshMenu: menuOpen=%d pg=%d\n", _menuOpen ? 1 : 0, (int)_pg);
        if (_menuOpen) buildMenu();
        else if (_pg == PG_WIFI_SCAN || _pg == PG_STATION_LIST) buildMenu();  // 扫描/台站页强制刷新
    }             // 重建当前菜单（台站数据到达后刷新）
    void begin()
    {
        createHome();
        _pg = PG_HOME;
        _menuOpen = false;
        _sel = 0;
        _inp = INP_NONE;
    }

    void initTouch()
    {
        if (_disp)
        {
            lv_indev_drv_init(&_indev_drv);
            _indev_drv.type = LV_INDEV_TYPE_POINTER;
            _indev_drv.read_cb = _touchRead;
            _indev_drv.user_data = this;
            lv_indev_drv_register(&_indev_drv);
        }
    }
    void showBtWarn(bool on)
    {
        if (_btWarn) {
            if (on) lv_obj_clear_flag(_btWarn, LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(_btWarn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void setStatus(const char *m)
    {
        if (m)
            snprintf(_status, 32, "%s", m);
        if (_bottom)
            lv_label_set_text(_bottom, _status);
    }
    void drawAll()
    {
        if (_menuOpen || _inp != INP_NONE)
            return;
        updateHome();
    }

    // ── 摩斯码表（A-Z, 0-9） ──
    static const char* _morseCode(char c)
    {
        static const char* AZ[] = {".-","-...","-.-.","-..",".","..-.","--.","....","..",".---","-.-",".-..","--","-.","---",".--.","--.-",".-.","...","-","..-","...-",".--","-..-","-.--","--.."};
        static const char* NU[] = {"-----",".----","..---","...--","....-",".....","-....","--...","---..","----."};
        if (c >= 'A' && c <= 'Z') return AZ[c-'A'];
        if (c >= 'a' && c <= 'z') return AZ[c-'a'];
        if (c >= '0' && c <= '9') return NU[c-'0'];
        return nullptr;
    }

    // ── 生成呼号摩斯码序列（点=1tick 亮，划=3tick 亮，符号间隔1，字符间隔3，呼号间隔7） ──
    void _buildMorse(const char* call)
    {
        _morseCnt = 0; _morseIdx = 0; _morseTick = 0;
        if (!call) return;
        for (const char* p = call; *p && _morseCnt < MORSE_MAX - 8; p++) {
            const char* code = _morseCode(*p);
            if (!code) continue;
            for (int i = 0; code[i] && _morseCnt < MORSE_MAX - 8; i++) {
                // 亮段（点=1, 划=3）
                if (_morseCnt < MORSE_MAX) { _morseDur[_morseCnt] = (code[i]=='.') ? 1 : 3; _morseOn[_morseCnt++] = true; }
                // 符号间间隔 1（最后一个符号后不加）
                if (code[i+1] && _morseCnt < MORSE_MAX) { _morseDur[_morseCnt] = 1; _morseOn[_morseCnt++] = false; }
            }
            // 字符间间隔 3
            if (_morseCnt < MORSE_MAX) { _morseDur[_morseCnt] = 3; _morseOn[_morseCnt++] = false; }
        }
        // 呼号循环间隔 7
        if (_morseCnt < MORSE_MAX) { _morseDur[_morseCnt] = 7; _morseOn[_morseCnt++] = false; }
        // 初始 tick = 第 0 段时长：让第一段（亮）立即显示，避免动画回调首帧跳过
        if (_morseCnt > 0) _morseTick = _morseDur[0];
    }

    // ── 语音发射界面动画（radio-voice-tx）：呼吸光环 + 均衡器跳动 + 计时器 ──
    static void _txAnimCb(lv_timer_t* t)
    {
        FMO_UI_LVGL* self = (FMO_UI_LVGL*)t->user_data;
        if (!self->_txOverlay || lv_obj_has_flag(self->_txOverlay, LV_OBJ_FLAG_HIDDEN)) return;
        // ① 呼吸光环：交替透明度（模拟 glowBlink）
        if (self->_txRings[3]) {
            self->_txGlowOn = !self->_txGlowOn;
            lv_obj_set_style_border_opa(self->_txRings[3],
                self->_txGlowOn ? 200 : 90, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        // ② 均衡器：7 根条随机高度（0-46px 容器）
        if (self->_txEq[0]) {
            for (int i = 0; i < 7; i++) {
                int h = 6 + (esp_random() % 41);  // 6~46px
                self->_txEqLevel[i] = h;
                lv_obj_set_size(self->_txEq[i], 8, h);
                lv_obj_set_pos(self->_txEq[i], 181 + i * 16, 228 + 46 - h);  // 底部对齐
            }
        }
        // ③ 计时器：发射时长 mm:ss
        if (self->_txTimer && self->_txStartMs) {
            uint32_t elapsed = (millis() - self->_txStartMs) / 1000;
            char b[8];
            snprintf(b, sizeof(b), "%02u:%02u", elapsed / 60, elapsed % 60);
            lv_label_set_text(self->_txTimer, b);
        }
    }

    // ── 息屏动画定时器（呼号摩斯码闪烁） ──
    static void _offAnimCb(lv_timer_t* t)
    {
        FMO_UI_LVGL* self = (FMO_UI_LVGL*)t->user_data;
        if (!self->_offDot || self->_morseCnt <= 0) return;
        // 推进摩斯序列（每 tick = 120ms）
        if (self->_morseTick > 0) {
            self->_morseTick--;
        } else {
            self->_morseIdx = (self->_morseIdx + 1) % self->_morseCnt;
            self->_morseTick = self->_morseDur[self->_morseIdx] - 1;
        }
        bool on = self->_morseOn[self->_morseIdx];
        lv_obj_set_style_bg_opa(self->_offDot, on ? 220 : 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(self->_offDot, on ? 255 : 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    // ── 息屏（黑覆盖层 + 呼号摩斯码绿色圆点闪烁，保留 WiFi/音频进程） ──
    void startScreenOff()
    {
        if (_screenOff) return;
        _screenOff = true;
        _offPhase = 0;
        // 用本机呼号生成摩斯码
        const char* call = (settings && settings->data()->owner_callsign[0]) ? settings->data()->owner_callsign : "CQ";
        _buildMorse(call);
        if (!_offOverlay) {
            _offOverlay = lv_obj_create(lv_scr_act());
            lv_obj_set_size(_offOverlay, 466, 466);
            lv_obj_set_pos(_offOverlay, 0, 0);
            lv_obj_set_style_bg_color(_offOverlay, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(_offOverlay, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(_offOverlay, 0, 0);
            lv_obj_clear_flag(_offOverlay, LV_OBJ_FLAG_SCROLLABLE);
            // 摩斯码圆点（绿色，50px，亮度60可见）
            _offDot = lv_obj_create(_offOverlay);
            lv_obj_set_size(_offDot, 50, 50);
            lv_obj_align(_offDot, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_color(_offDot, lv_color_make(0, 255, 80), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(_offDot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(_offDot, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(_offDot, 0, 0);
            lv_obj_clear_flag(_offDot, LV_OBJ_FLAG_SCROLLABLE);
        }
        lv_obj_move_foreground(_offOverlay);
        if (!_offTimer)
            _offTimer = lv_timer_create(_offAnimCb, 120, this);   // 120ms/tick（摩斯节奏）
    }

    // ── 唤醒（恢复显示） ──
    void wakeScreen()
    {
        if (!_screenOff) return;
        _screenOff = false;
        // 先删定时器，再删覆盖层：避免定时器回调访问已删除的 _offDot（LVGL assert 卡死）
        if (_offTimer) { lv_timer_del(_offTimer); _offTimer = nullptr; }
        if (_offOverlay) lv_obj_del(_offOverlay);
        _offOverlay = nullptr;
        _offDot = nullptr;
    }
    bool isScreenOff() const { return _screenOff; }

    void drawWifiOverlay() {}
    void keyA()
    {
        if (_inp != INP_NONE)
        {
            Serial.printf("[keyA] inp=%d 忽略\n", (int)_inp);
            return;
        }
        if (_pg == PG_VOLUME) { volAdjust(+5); return; }   // 音量+
        if (_pg == PG_DEVINFO) { scrollList(+1); return; } // Device Info: A键内容向上滚动
        Serial.printf("[keyA] menuOpen=%d pg=%d sel=%d\n", _menuOpen ? 1 : 0, (int)_pg, _sel);
        if (_menuOpen)
            menuDown();
    }
    void keyB()
    {
        if (_inp != INP_NONE)
        {
            Serial.printf("[keyB] inp=%d 忽略\n", (int)_inp);
            return;
        }
        if (_pg == PG_VOLUME) { volAdjust(-5); return; }   // 音量-
        Serial.printf("[keyB] menuOpen=%d pg=%d sel=%d\n", _menuOpen ? 1 : 0, (int)_pg, _sel);
        if (_menuOpen)
            menuSelect();
    }
    void keyAB()
    {
        if (_inp != INP_NONE)
        {
            endInput();
            return;
        }
        if (_pg == PG_VOLUME) { closeVolume(); return; }   // 返回音频菜单
        if (_menuOpen)
            closeMenu();
        else
            openMenu();
    }
    void handleTouch(int, int) {}
    void onTouchDown(int, int) {}
    void onTouchUp(int, int) {}

private:
    static lv_color_t H(uint32_t c) { return lv_color_hex(c); }
    static const uint32_t C_BG = 0x050508, C_SURFACE = 0x1A1A22, C_ACCENT = 0xFF6B2B,
                          C_GREEN = 0x22C55E, C_WHITE = 0xFFFFFF, C_GRAY = 0xC8CAD0, C_MUTED = 0xA8AAB0;
    M5GFX *_disp = nullptr;
    lv_indev_drv_t _indev_drv;

    static void _touchRead(lv_indev_drv_t *drv, lv_indev_data_t *data)
    {
        auto *self = (FMO_UI_LVGL *)drv->user_data;
        if (!self || !self->_disp)
        {
            data->state = LV_INDEV_STATE_REL;
            return;
        }
        int tx = -1, ty = -1;
        if (self->_disp->getTouch(&tx, &ty) > 0)
        {
            data->point.x = tx;
            data->point.y = ty;
            data->state = LV_INDEV_STATE_PR;
        }
        else
            data->state = LV_INDEV_STATE_REL;
    }

    lv_obj_t *_scrHome = nullptr, *_scrMenu = nullptr, *_scrInput = nullptr, *_scrVolume = nullptr;
    lv_obj_t *_date = nullptr, *_time = nullptr, *_mode = nullptr, *_call = nullptr,
             *_ctxt = nullptr, *_bottom = nullptr;
    lv_obj_t *_callLoc = nullptr;    // 呼号所在地（城市，中文字体小字）
    lv_obj_t *_batPct = nullptr;     // 电池百分比文字
    lv_obj_t *_batBolt = nullptr;    // 充电闪电图标
    lv_obj_t *_sigBar[3] = {};       // 信号强度条（3格）
    lv_obj_t *_wthrIcon = nullptr;   // 天气图标（A8图）
    lv_obj_t *_wthrTemp = nullptr;   // 温度文字
    lv_obj_t *_wthrCity = nullptr;   // 城市名
    lv_obj_t *_wthrDesc = nullptr;   // 中文天气说明
    lv_obj_t *_wthrDeg = nullptr;    // 温度°号（右上角上标）
    lv_obj_t *_txInd = nullptr;      // 语音发送TX指示
    lv_obj_t *_txOverlay = nullptr;  // 语音发送整屏叠加层
    lv_obj_t *_txCall = nullptr;     // 叠加层中央呼号
    lv_obj_t *_btWarn = nullptr;     // 蓝牙未连接提示
    // ── radio-voice-tx 设计稿：发射界面元素 ──
    lv_obj_t *_txRings[4] = {};      // 同心圆环 [0]bezel [1]inner [2]level-track [3]glow(呼吸)
    lv_obj_t *_txEq[7] = {};         // 均衡器电平条（7 根）
    lv_obj_t *_txTimer = nullptr;    // 发射计时器
    lv_obj_t *_txHint = nullptr;     // 提示语"松开A键结束通联"
    uint32_t _txStartMs = 0;         // 发射开始时间戳（计时用）
    bool _txGlowOn = false;          // 呼吸光环当前状态（闪烁动画）
    lv_timer_t* _txAnimTimer = nullptr;  // 呼吸/电平动画定时器
    int _txEqLevel[7] = {};          // 均衡器当前电平（0-100）
    Page _pg = PG_HOME, _prevPg = PG_HOME;
    bool _menuOpen = false;
    int _sel = 0;
    lv_obj_t *_menuBtns[24] = {};   // 菜单项引用（Device Info 页最多20行）
    lv_obj_t *_menuList = nullptr;  // 当前菜单列表容器（选中滚动/A键滚动用）
    int _menuCount = 0;
    InputMode _inp = INP_NONE;
    lv_obj_t *_kb = nullptr, *_ta = nullptr;
    int _kbMode = 0;               // 键盘模式: 0=字母(T9), 1=数字, 2=符号
    bool _kbUpper = false;         // 字母键盘是否大写（↑ 键切换）
    uint32_t _kbLastBtn = 0xFFFFFFFF;  // 上次按下的T9键索引（用于多击循环）
    uint32_t _kbLastTime = 0;      // 上次按键时间
    int _kbT9Idx = 0;              // T9 当前连击序号（a=0,b=1,c=2,数字=3）
    bool _kbPending = false;       // 是否有待确认的T9字符（连击时替换它）
    lv_obj_t *_volLabel = nullptr, *_volUp = nullptr, *_volDown = nullptr;
    char _status[32] = "";
    // updateHome 文本缓存：文本未变化时不重复 set_text（避免每5ms无谓重绘）
    char _lastCtxt[64] = "";
    char _lastCity[32] = "";
    char _lastDesc[32] = "";
    char _lastTemp[8]  = "";
    char _lastDate[16] = "";
    char _lastTime[8]  = "";
    int  _lastMode     = -1;
    char _lastCall[32] = "";
    uint32_t _lastCallColor = 0;
    char _lastCallLoc[32] = "";   // 呼号所在地（避免重复刷新）
    char _lookupCall[16] = "";    // 呼号查询页：呼号
    char _lookupLoc[48] = "";     // 呼号查询页：地址
    int  _lastSig      = -1;
    char _lastBatSym[8] = "";
    uint32_t _lastBatColor = 0;
    bool _lastBatBolt  = false;
    char _lastStatus[32] = "";
    bool _lastTxInd    = false;
    bool _lastTxOv     = false;
    char _lastTxCall[16] = "";
    struct NetItem
    {
        String ssid;
        int rssi;
        bool open;
    };
    std::vector<NetItem> _nets;
    bool _scanning = false;

    static void _gearClick(lv_event_t *e) { ((FMO_UI_LVGL *)lv_event_get_user_data(e))->openMenu(); }
    static void _menuItemClick(lv_event_t *e)
    {
        auto *self = (FMO_UI_LVGL *)lv_event_get_user_data(e);
        self->_sel = (int)(intptr_t)lv_event_get_target(e)->user_data;
        self->menuSelect();
    }
    static void _kbReady(lv_event_t *e) { ((FMO_UI_LVGL *)lv_event_get_user_data(e))->endInput(); }
    static void _kbValueChanged(lv_event_t *e) { ((FMO_UI_LVGL *)lv_event_get_user_data(e))->onKbValueChanged(); }
    static void _volTapUp(lv_event_t *e)   { ((FMO_UI_LVGL *)lv_event_get_user_data(e))->volAdjust(+5); }
    static void _volTapDown(lv_event_t *e) { ((FMO_UI_LVGL *)lv_event_get_user_data(e))->volAdjust(-5); }

    // Helper
    lv_obj_t *mk(lv_obj_t *p, const char *t, uint32_t c, int x, int y, int w, const lv_font_t *f)
    {
        auto l = lv_label_create(p);
        lv_obj_set_style_text_font(l, f, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(l, H(c), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(l, w > 0 ? w : 466);
        lv_obj_set_pos(l, x, y);
        lv_label_set_text(l, t);
        return l;
    }

    /** 天气中文字符（思源黑体，可靠渲染） */
    const char* _wthrIconChar(FMO_Weather::Cat cat) {
        switch (cat) {
            case FMO_Weather::W_SUN:     return "晴";
            case FMO_Weather::W_PARTLY:  return "云";
            case FMO_Weather::W_CLOUD:   return "阴";
            case FMO_Weather::W_FOG:     return "雾";
            case FMO_Weather::W_RAIN:    return "雨";
            case FMO_Weather::W_SNOW:    return "雪";
            case FMO_Weather::W_THUNDER: return "雷";
            case FMO_Weather::W_SLEET:   return "冻";
            default: return "--";
        }
    }

    /** QWeather 图标码点（U+F101-F130，qwi 字体备用） */
    uint32_t _wthrIconCp(FMO_Weather::Cat cat) {
        switch (cat) {
            case FMO_Weather::W_SUN:     return 0xF101;  // 晴
            case FMO_Weather::W_PARTLY:  return 0xF102;  // 多云
            case FMO_Weather::W_CLOUD:   return 0xF105;  // 阴
            case FMO_Weather::W_FOG:     return 0xF12F;  // 雾
            case FMO_Weather::W_RAIN:    return 0xF10F;  // 小雨
            case FMO_Weather::W_SNOW:    return 0xF120;  // 小雪
            case FMO_Weather::W_THUNDER: return 0xF10C;  // 雷阵雨
            case FMO_Weather::W_SLEET:   return 0xF11D;  // 冰雨
            default: return '?';
        }
    }

    /** 码点 → UTF-8 字符串（供 lv_label_set_text） */
    static void _cpToUtf8(uint32_t cp, char* out) {
        if (cp < 0x80) { out[0]=cp; out[1]=0; }
        else if (cp < 0x800) {
            out[0]=0xC0|(cp>>6); out[1]=0x80|(cp&0x3F); out[2]=0;
        } else {
            out[0]=0xE0|((cp>>12)&0xF); out[1]=0x80|((cp>>6)&0x3F); out[2]=0x80|(cp&0x3F); out[3]=0;
        }
    }

    /** 天气图标颜色（按分类着色） */
    uint32_t _wthrIconColor(FMO_Weather::Cat cat) {
        switch (cat) {
            case FMO_Weather::W_SUN:     return 0xFFD700;  // 黄-晴
            case FMO_Weather::W_PARTLY:  return 0xFFD700;  // 黄-多云
            case FMO_Weather::W_CLOUD:   return 0xA8AAB0;  // 灰-阴
            case FMO_Weather::W_FOG:     return 0xA8AAB0;  // 灰-雾
            case FMO_Weather::W_RAIN:    return 0x4FA8FF;  // 蓝-雨
            case FMO_Weather::W_SNOW:    return 0xFFFFFF;  // 白-雪
            case FMO_Weather::W_THUNDER: return 0xFFD700;  // 黄-雷
            case FMO_Weather::W_SLEET:   return 0x4FA8FF;  // 蓝-冻雨
            default: return 0xA8AAB0;
        }
    }

    /** 天气行自适应布局：图标+说明+温度按实际宽度整体居中 */
    void _layoutWeatherRow() {
        if (!_wthrIcon || !_wthrDesc || !_wthrTemp) return;
        lv_obj_update_layout(_wthrDesc);
        lv_obj_update_layout(_wthrTemp);
        int dw = lv_obj_get_width(_wthrDesc);   // 说明宽度（1~3字可变）
        int tw = lv_obj_get_width(_wthrTemp);   // 温度宽度（1~3位数字）
        int total = 28 + 8 + dw + 6 + tw;       // 图标+间距+说明+间距+温度
        int sx = 233 - total / 2;               // 整组居中
        lv_obj_set_pos(_wthrIcon, sx, 315);
        lv_obj_set_pos(_wthrDesc, sx + 36, 319);
        lv_obj_set_pos(_wthrTemp, sx + 36 + dw + 6, 319);
        if (_wthrDeg) {
            lv_obj_update_layout(_wthrTemp);
            lv_obj_align_to(_wthrDeg, _wthrTemp, LV_ALIGN_TOP_RIGHT, 8, -10);
        }
    }

    // ═══ HOME ═══
    void createHome()
    {
        _scrHome = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scrHome, H(C_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(_scrHome, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(_scrHome, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scroll_dir(_scrHome, LV_DIR_NONE);
        lv_scr_load(_scrHome);
        lv_obj_scroll_to_y(_scrHome, 0, LV_ANIM_OFF);
        int CX = 233;
        _date = mk(_scrHome, "--.--.--", C_GRAY, 0, 50, 466, &lv_font_montserrat_18);
        _time = mk(_scrHome, "--:--", C_GRAY, 0, 73, 466, &lv_font_montserrat_18);
        _mode = mk(_scrHome, "Standby", C_WHITE, 0, 117, 466, &lv_font_montserrat_24);
        // TX指示（按住A语音发送时显示）
        _txInd = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_txInd, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_txInd, H(C_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_txInd, 380, 129);
        lv_label_set_text(_txInd, "TX");
        lv_obj_add_flag(_txInd, LV_OBJ_FLAG_HIDDEN);
        // 呼号：英文 Montserrat 48px（呼号是英文/数字，用英文字体更合适）
        _call = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_call, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_call, H(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(_call, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(_call, 466);
        lv_obj_set_height(_call, 52);   // Montserrat48 line_height=52，确保完整显示
        lv_obj_set_pos(_call, 0, 160);
        lv_label_set_text(_call, "NOCALL");

        // 呼号所在地（城市）：中文字体小字，显示在呼号下方
        _callLoc = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_callLoc,
            _g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_24,
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_callLoc, H(C_MUTED), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(_callLoc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(_callLoc, 466);
        lv_obj_set_pos(_callLoc, 0, 214);
        lv_label_set_text(_callLoc, "");

        lv_obj_t *cbg = lv_obj_create(_scrHome);
        lv_obj_set_size(cbg, 380, 52);
        lv_obj_set_pos(cbg, CX - 190, 251);
        lv_obj_set_style_bg_color(cbg, H(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cbg, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cbg, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scroll_dir(cbg, LV_DIR_NONE);
        // 允许子内容超出容器边界显示（避免文字被裁剪）
        lv_obj_add_flag(cbg, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        _ctxt = lv_label_create(cbg);
        // station 名称：思源黑体 24px（支持中文），容器内垂直居中
        lv_obj_set_width(_ctxt, 380);
        lv_obj_center(_ctxt);
        lv_obj_set_style_text_align(_ctxt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(_ctxt, _g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_24,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_ctxt, H(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT); // 站名黑色（琥珀底色上高对比）
        lv_obj_set_style_pad_top(_ctxt, 2, LV_PART_MAIN | LV_STATE_DEFAULT);              // 补偿字形顶部裁切
        lv_label_set_text(_ctxt, "请稍后");

        // ── 天气（频道条下方）：城市 + A8图标 + 温度 ──
        _wthrCity = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_wthrCity, _g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_24,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_wthrCity, H(C_GRAY), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(_wthrCity, 466);
        lv_obj_set_style_text_align(_wthrCity, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_wthrCity, 0, 365);   // 城市行（天气行下方）
        lv_obj_set_style_pad_top(_wthrCity, 2, LV_PART_MAIN | LV_STATE_DEFAULT);          // 补偿字形顶部裁切
        lv_label_set_text(_wthrCity, "--");
        // 天气行居中： [图标] [说明] [温度]  说明与温度同字号(思源黑体24)
        _wthrIcon = lv_img_create(_scrHome);
        lv_img_set_src(_wthrIcon, wicon_table[FMO_Weather::W_SUN]);
        lv_obj_set_style_img_recolor(_wthrIcon, H(0xFFD700), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(_wthrIcon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_wthrIcon, 163, 315);
        _wthrDesc = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_wthrDesc, _g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_24,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_wthrDesc, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_wthrDesc, 199, 319);
        lv_obj_set_style_pad_top(_wthrDesc, 2, LV_PART_MAIN | LV_STATE_DEFAULT);          // 补偿字形顶部裁切
        lv_label_set_text(_wthrDesc, "--");
        _wthrTemp = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_wthrTemp, _g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_24,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_wthrTemp, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_wthrTemp, 255, 319);
        lv_obj_set_style_pad_top(_wthrTemp, 2, LV_PART_MAIN | LV_STATE_DEFAULT);          // 补偿字形顶部裁切
        lv_label_set_text(_wthrTemp, "--");
        // °上标（右侧，montserrat24；位置在updateHome动态对齐，跟随数字宽度）
        _wthrDeg = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_wthrDeg, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_wthrDeg, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text(_wthrDeg, "°");

        // ── 电池（底部）：百分比文字 + 充电闪电紧邻 ──
        _batPct = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_batPct, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_batPct, H(C_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_batPct, 207, 415);   // 电池图标（按电量切换 5 级符号）
        lv_label_set_text(_batPct, LV_SYMBOL_BATTERY_FULL);
        lv_obj_set_width(_batPct, 24);
        lv_obj_set_style_text_align(_batPct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

        // 充电闪电图标：紧贴电量左侧（-2px 间隙）
        _batBolt = lv_label_create(_scrHome);
        lv_obj_set_style_text_font(_batBolt, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_batBolt, H(C_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align_to(_batBolt, _batPct, LV_ALIGN_LEFT_MID, -14, 0);
        lv_label_set_text(_batBolt, LV_SYMBOL_CHARGE);
        lv_obj_add_flag(_batBolt, LV_OBJ_FLAG_HIDDEN);

        // ── WiFi 图标（左下方）+ 信号强度条 ──
        // 3 格信号强度条（底部对齐 y=428，替代WiFi图标）
        for (int i = 0; i < 3; i++) {
            _sigBar[i] = lv_obj_create(_scrHome);
            lv_obj_set_size(_sigBar[i], 4, 6 + i * 4);
            lv_obj_set_pos(_sigBar[i], 237 + i * 8, 428 - (6 + i * 4));
            lv_obj_set_style_bg_color(_sigBar[i], H(C_MUTED), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(_sigBar[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(_sigBar[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        }

        _bottom = mk(_scrHome, "", C_MUTED, 0, 440, 466, &lv_font_montserrat_14);

        // ── A键语音发送叠加层（radio-voice-tx 设计稿）──
        _txOverlay = lv_obj_create(_scrHome);
        lv_obj_set_size(_txOverlay, 466, 466);
        lv_obj_set_pos(_txOverlay, 0, 0);
        lv_obj_set_style_bg_color(_txOverlay, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);  // 纯黑不透明
        lv_obj_set_style_bg_opa(_txOverlay, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(_txOverlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(_txOverlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(_txOverlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scroll_dir(_txOverlay, LV_DIR_NONE);
        // 同心圆环：bezel(466,2px) → inner(438,2px) → level-track(406,10px) → glow(406,4px 呼吸)
        const uint32_t RING_W[4] = {466, 438, 406, 406};
        const uint32_t RING_BW[4] = {2, 2, 10, 4};
        const uint32_t RING_C[4]  = {0x2A2A2E, 0x161618, 0x0B2E1A, 0x00E676};
        for (int i = 0; i < 4; i++) {
            _txRings[i] = lv_obj_create(_txOverlay);
            uint32_t off = (466 - RING_W[i]) / 2;
            lv_obj_set_size(_txRings[i], RING_W[i], RING_W[i]);
            lv_obj_set_pos(_txRings[i], off, off);
            lv_obj_set_style_bg_opa(_txRings[i], LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(_txRings[i], H(RING_C[i]), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(_txRings[i], RING_BW[i], LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(_txRings[i], LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_opa(_txRings[i], (i == 3) ? 180 : 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(_txRings[i], LV_OBJ_FLAG_SCROLLABLE);
        }
        // 大呼号（46px 效果用 48 近似，y=130 居中）
        _txCall = lv_label_create(_txOverlay);
        lv_obj_set_style_text_font(_txCall, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_txCall, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(_txCall, 466);
        lv_obj_set_style_text_align(_txCall, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_txCall, 0, 130);
        lv_label_set_text(_txCall, "NOCALL");
        // 均衡器（7 根绿色竖条，底部对齐，y=228 容器）
        for (int i = 0; i < 7; i++) {
            _txEq[i] = lv_obj_create(_txOverlay);
            lv_obj_set_size(_txEq[i], 8, 6);
            lv_obj_set_pos(_txEq[i], 181 + i * (8 + 8), 228 + 46 - 6);  // 104px 容器内，底部对齐
            lv_obj_set_style_bg_color(_txEq[i], H(C_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(_txEq[i], 220, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(_txEq[i], 4, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(_txEq[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_clear_flag(_txEq[i], LV_OBJ_FLAG_SCROLLABLE);
            _txEqLevel[i] = 10 + (i * 7) % 40;
        }
        // 计时器（绿色 32px，y=288）
        _txTimer = lv_label_create(_txOverlay);
        lv_obj_set_style_text_font(_txTimer, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_txTimer, H(C_GREEN), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(_txTimer, 466);
        lv_obj_set_style_text_align(_txTimer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_txTimer, 0, 288);
        lv_label_set_text(_txTimer, "00:00");
        // 提示语（灰色，思源黑体 24px，y=360；加固定行高+置顶防被遮挡）
        _txHint = lv_label_create(_txOverlay);
        lv_obj_set_style_text_font(_txHint, _g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_14,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_txHint, H(C_MUTED), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(_txHint, 466);
        lv_obj_set_height(_txHint, 40);   // 固定行高，容纳 ascent，防止文字顶部被裁剪
        lv_obj_set_style_text_align(_txHint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_txHint, 0, 352);
        lv_label_set_text(_txHint, "松开A键结束通联");
        lv_obj_move_foreground(_txHint);
        // 蓝牙未连接提示（提示语下方，橙色）
        _btWarn = lv_label_create(_txOverlay);
        lv_obj_set_style_text_font(_btWarn, _g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_24,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_btWarn, H(0xFF6B2B), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(_btWarn, 466);
        lv_obj_set_style_text_align(_btWarn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_btWarn, 0, 395);
        lv_label_set_text(_btWarn, "WiFi 未连接");
        lv_obj_add_flag(_btWarn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_txOverlay, LV_OBJ_FLAG_HIDDEN);
    }

    void updateHome()
    {
        time_t now_t = time(nullptr);
        struct tm ti;
        localtime_r(&now_t, &ti);
        if (now_t > 1000000000)
        {
            char b[16];
            strftime(b, 16, "%Y-%m-%d", &ti);
            if (strcmp(b, _lastDate) != 0) { strncpy(_lastDate, b, sizeof(_lastDate) - 1); _lastDate[sizeof(_lastDate) - 1] = 0; lv_label_set_text(_date, b); }
            strftime(b, 8, "%H:%M", &ti);
            if (strcmp(b, _lastTime) != 0) { strncpy(_lastTime, b, sizeof(_lastTime) - 1); _lastTime[sizeof(_lastTime) - 1] = 0; lv_label_set_text(_time, b); }
        }
        else
        {
            // 未同步时间（1970年）→ 显示占位符
            if (_lastDate[0]) { _lastDate[0] = 0; lv_label_set_text(_date, "--.--.--"); }
            if (_lastTime[0]) { _lastTime[0] = 0; lv_label_set_text(_time, "--:--"); }
        }
        bool spk = fmo ? fmo->isSpeaking() : false;
        {
            int mode = spk ? 1 : 0;
            if (mode != _lastMode) {
                _lastMode = mode;
                lv_label_set_text(_mode, spk ? "ON AIR" : "Standby");
                lv_obj_set_style_text_color(_mode, H(spk ? C_GREEN : C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }
        // 呼号：只在正在通联（isSpeaking）且对方呼号非空时显示，否则 NOCALL
        {
            const char *tkr = fmo ? fmo->currentTalker() : "";
            const char* callTxt = (spk && tkr[0]) ? tkr : "NOCALL";
            uint32_t callCol = (spk && tkr[0]) ? C_ACCENT : C_MUTED;
            if (strcmp(callTxt, _lastCall) != 0 || callCol != _lastCallColor) {
                strncpy(_lastCall, callTxt, sizeof(_lastCall) - 1); _lastCall[sizeof(_lastCall) - 1] = 0;
                _lastCallColor = callCol;
                lv_label_set_text(_call, callTxt);
                lv_obj_set_style_text_color(_call, H(callCol), LV_PART_MAIN | LV_STATE_DEFAULT);
            }
            // 呼号所在地：通联中且有呼号时查询城市，否则显示提示"Location"
            char locBuf[32] = "";
            if (spk && tkr[0] && _g_callLoc) {
                _g_callLoc->lookup(tkr, locBuf, sizeof(locBuf));
            } else if (!(spk && tkr[0])) {
                strcpy(locBuf, "Location");   // NOCALL 时提示位置区
            }
            if (strcmp(locBuf, _lastCallLoc) != 0) {
                strncpy(_lastCallLoc, locBuf, sizeof(_lastCallLoc) - 1); _lastCallLoc[sizeof(_lastCallLoc) - 1] = 0;
                // 字体：中文城市用中文字体，英文(Location)用英文字体（同为24px视觉一致）
                bool hasCJK = ((uint8_t)locBuf[0] >= 0x80);
                lv_obj_set_style_text_font(_callLoc,
                    hasCJK ? (_g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_24)
                           : &lv_font_montserrat_24,
                    LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_label_set_text(_callLoc, locBuf);
            }
        }
        {
            char _sb[64];
            if (fmo && fmo->stationName()[0]) {
                _sanitizeDisplay(fmo->stationName(), _sb, sizeof(_sb));   // 过滤特殊字符，防方块
            }
            else                              { snprintf(_sb, sizeof(_sb), "%s", "请稍后..."); }
            if (strcmp(_sb, _lastCtxt) != 0) {   // 文本未变化则不重复 set_text（避免每5ms无谓重绘）
                strncpy(_lastCtxt, _sb, sizeof(_lastCtxt) - 1); _lastCtxt[sizeof(_lastCtxt) - 1] = 0;
                lv_label_set_text(_ctxt, _sb);
            }
        }
        // ── 信号强度（替代WiFi图标） ──
        bool wifiOn = (wifi && wifi->state() == WF_CONNECTED);
        // 信号强度：RSSI 越强亮的格数越多（-127=断开）
        int sig = 0;
        if (wifiOn) {
            int rssi = wifi->rssi();
            if (rssi >= -60) sig = 3;
            else if (rssi >= -70) sig = 2;
            else if (rssi >= -80) sig = 1;
        }
        int sigKey = (wifiOn ? 8 : 0) + sig;   // 含wifiOn（颜色随连接状态变）
        if (sigKey != _lastSig) {
            _lastSig = sigKey;
            for (int i = 0; i < 3; i++) {
                if (_sigBar[i]) {
                    lv_obj_set_style_bg_color(_sigBar[i], H(i < sig ? C_GREEN : (wifiOn ? 0x333333 : C_MUTED)),
                                              LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        // ── 天气：城市 + 图标 + 温度 ──
        if (_wthrTemp && _wthrIcon) {
            if (weather && weather->valid()) {
                char _cb[32], _db[32];
                snprintf(_cb, sizeof(_cb), "%s", weather->city()[0] ? weather->city() : "--");
                snprintf(_db, sizeof(_db), "%s", weather->desc()[0] ? weather->desc() : "--");
                char wb[8];
                snprintf(wb, 8, "%d", weather->tempC());
                // 文本未变化则不重复 set_text / 重排（避免每5ms无谓重绘）
                bool cChanged = (strcmp(_cb, _lastCity) != 0);
                bool dChanged = (strcmp(_db, _lastDesc) != 0);
                bool tChanged = (strcmp(wb, _lastTemp) != 0);
                if (cChanged) { strncpy(_lastCity, _cb, sizeof(_lastCity) - 1); _lastCity[sizeof(_lastCity) - 1] = 0; if (_wthrCity) lv_label_set_text(_wthrCity, _cb); }
                if (dChanged) { strncpy(_lastDesc, _db, sizeof(_lastDesc) - 1); _lastDesc[sizeof(_lastDesc) - 1] = 0; if (_wthrDesc) lv_label_set_text(_wthrDesc, _db); }
                if (tChanged) { strncpy(_lastTemp, wb, sizeof(_lastTemp) - 1); _lastTemp[sizeof(_lastTemp) - 1] = 0; lv_label_set_text(_wthrTemp, wb); }
                if (cChanged || dChanged || tChanged)
                    _layoutWeatherRow();                  // 整组自适应居中（仅在内容变化时重排）
                static int lastCat = -1;
                int cat = (int)weather->category();
                if (cat != lastCat) {
                    lastCat = cat;
                    lv_img_set_src(_wthrIcon, wicon_table[cat]);
                    lv_obj_set_style_img_recolor(_wthrIcon, H(_wthrIconColor(weather->category())), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_img_recolor_opa(_wthrIcon, LV_OPA_COVER,
                                                     LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            } else {
                if (strcmp("--", _lastTemp) != 0) {
                    strncpy(_lastTemp, "--", sizeof(_lastTemp) - 1); _lastTemp[sizeof(_lastTemp) - 1] = 0;
                    lv_label_set_text(_wthrTemp, "--");
                    _layoutWeatherRow();
                }
            }
        }
        int p = _batPercent;
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        // 电池图标（按电量切换 5 级符号）+ 充电闪电
        if (_batPct) {
            const char *sym;
            if (p >= 75)      sym = LV_SYMBOL_BATTERY_FULL;
            else if (p >= 50) sym = LV_SYMBOL_BATTERY_3;
            else if (p >= 25) sym = LV_SYMBOL_BATTERY_2;
            else if (p >= 10) sym = LV_SYMBOL_BATTERY_1;
            else              sym = LV_SYMBOL_BATTERY_EMPTY;
            uint32_t batCol = _batCharging ? 0x19C25F : C_GREEN;
            if (strcmp(sym, _lastBatSym) != 0 || batCol != _lastBatColor) {
                strncpy(_lastBatSym, sym, sizeof(_lastBatSym) - 1); _lastBatSym[sizeof(_lastBatSym) - 1] = 0;
                _lastBatColor = batCol;
                lv_label_set_text(_batPct, sym);
                lv_obj_set_style_text_color(_batPct, H(batCol), LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }
        if (_batBolt) {
            if (_batCharging != _lastBatBolt) {
                _lastBatBolt = _batCharging;
                if (_batCharging) lv_obj_clear_flag(_batBolt, LV_OBJ_FLAG_HIDDEN);
                else              lv_obj_add_flag(_batBolt, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (strcmp(_status, _lastStatus) != 0) {
            strncpy(_lastStatus, _status, sizeof(_lastStatus) - 1); _lastStatus[sizeof(_lastStatus) - 1] = 0;
            lv_label_set_text(_bottom, _status);
        }
        // TX指示 + 整屏叠加层：按住A发送时显示
        if (_txInd) {
            if (_micTx != _lastTxInd) {
                _lastTxInd = _micTx;
                if (_micTx) lv_obj_clear_flag(_txInd, LV_OBJ_FLAG_HIDDEN);
                else        lv_obj_add_flag(_txInd, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (_txOverlay && _txCall) {
            // 蓝牙未连接提示是否开启（叠加层内的橙色标签）
            bool warnOn = _btWarn && !lv_obj_has_flag(_btWarn, LV_OBJ_FLAG_HIDDEN);
            bool show = _micTx || warnOn;
            const char* cs = settings ? settings->data()->owner_callsign : "";
            const char* csTxt = (cs && cs[0]) ? cs : "NOCALL";
            if (show != _lastTxOv || strcmp(csTxt, _lastTxCall) != 0) {
                _lastTxOv = show;
                strncpy(_lastTxCall, csTxt, sizeof(_lastTxCall) - 1); _lastTxCall[sizeof(_lastTxCall) - 1] = 0;
                if (show) {
                    lv_label_set_text(_txCall, csTxt);
                    lv_obj_clear_flag(_txOverlay, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(_txOverlay);
                    // 启动动画（呼吸光环 + 均衡器 + 计时器）
                    _txStartMs = millis();
                    if (_txTimer) lv_label_set_text(_txTimer, "00:00");
                    if (!_txAnimTimer)
                        _txAnimTimer = lv_timer_create(_txAnimCb, 110, this);  // 110ms（同设计稿）
                } else {
                    lv_obj_add_flag(_txOverlay, LV_OBJ_FLAG_HIDDEN);
                    // 停止动画
                    if (_txAnimTimer) { lv_timer_del(_txAnimTimer); _txAnimTimer = nullptr; }
                    _txStartMs = 0;
                }
            }
        }
    }

    // ═══ MENU ═══
    void openMenu()
    {
        _menuOpen = true;
        _sel = 0;
        if (_pg == PG_HOME)
            _pg = PG_SETTINGS;
        buildMenu();
        lv_scr_load(_scrMenu);
    }
    void closeMenu()
    {
        _menuOpen = false;
        _pg = PG_HOME;
        _sel = 0;
        lv_scr_load(_scrHome);
        lv_obj_scroll_to_y(_scrHome, 0, LV_ANIM_OFF);
        updateHome();
    }
    int itemCount()
    {
        switch (_pg)
        {
        case PG_SETTINGS:
            return 10;
        case PG_WIFI_MENU:
            return 4;
        case PG_WIFI_SCAN:
            return (int)_nets.size() < 16 ? (int)_nets.size() : 16;
        case PG_AUDIO_SET:
            return 5;
        case PG_STATION_LIST:
            return fmo ? fmo->pinnedListCount() : 0;
        case PG_DEVINFO:
            return 19;
        case PG_ABOUT:
            return 1;
        case PG_BT_SCAN:
            return bt ? bt->scanCount() : 0;
        case PG_LOOKUP_RESULT:
            return 1;   // 查询结果页：1个"返回"项
        default:
            return 0;
        }
    }
    const char *pageTitle()
    {
        switch (_pg)
        {
        case PG_SETTINGS:
            return "SETTINGS";
        case PG_WIFI_MENU:
            return "WiFi";
        case PG_WIFI_SCAN:
            return "WiFi Scan";
        case PG_AUDIO_SET:
            return "Audio";
        case PG_FMO_HOST:
            return "FMO Server";
        case PG_CALLSIGN:
            return "Callsign";
        case PG_STATION_LIST:
            return "Stations";
        case PG_DEVINFO:
            return "Device Info";
        case PG_ABOUT:
            return "About";
        case PG_VOLUME:
            return "Volume";
        case PG_BT_SCAN:
            return "BT Devices";
        case PG_LOOKUP_RESULT:
            return "Search Result";
        default:
            return "";
        }
    }
    void getMenuText(int i, char *t, int m)
    {
        FMO_Settings *cfg = settings ? settings->data() : nullptr;
        t[0] = 0;
        switch (_pg)
        {
        case PG_SETTINGS:
            switch (i)
            {
            case 0:
                strcpy(t, "Stations");
                break;
            case 1:
                snprintf(t, m, "Audio %s", cfg && !cfg->audio_muted ? "(ON)" : "(OFF)");
                break;
            case 2:
                strcpy(t, "WiFi");
                break;
            case 3:
                snprintf(t, m, "FMO IP: %s", cfg ? cfg->profiles[cfg->active_profile].fmo_host : "--");
                break;
            case 4:
                snprintf(t, m, "MyCallSign: %s", cfg ? cfg->owner_callsign : "--");
                break;
            case 5:
                snprintf(t, m, "AudioTX IP: %s", cfg ? cfg->audio_tx_ip : "--");
                break;
            case 6:
                strcpy(t, "CallSignSearch");
                break;
            case 7:
                strcpy(t, "Device Info");
                break;
            case 8:
                strcpy(t, "About");
                break;
            case 9:
                strcpy(t, "Exit");
                break;
            }
            break;
        case PG_WIFI_MENU:
            switch (i)
            {
            case 0:
                strcpy(t, "Scan");
                break;
            case 1:
                snprintf(t, m, "SSID: %s", cfg ? cfg->profiles[cfg->active_profile].wifi_ssid : "--");
                break;
            case 2:
                strcpy(t, "Password: ****");
                break;
            case 3:
                strcpy(t, "< Back");
                break;
            }
            break;
        case PG_AUDIO_SET:
            switch (i)
            {
            case 0:
                snprintf(t, m, "%s", cfg && !cfg->audio_muted ? "Mute" : "Unmute");
                break;
            case 1:
                snprintf(t, m, "BT %s", cfg && cfg->bt_enabled ? "ON" : "OFF");
                break;
            case 2:
                snprintf(t, m, "Volume: %d", cfg ? cfg->audio_volume : 60);
                break;
            case 3:
                snprintf(t, m, "Vibrate %s", cfg && cfg->vibrate_enabled ? "ON" : "OFF");
                break;
            case 4:
                strcpy(t, "< Back");
                break;
            }
            break;
        case PG_CALLSIGN:
            snprintf(t, m, "Set: %s", cfg ? cfg->owner_callsign : "--");
            break;
        case PG_FMO_HOST:
            snprintf(t, m, "Host: %s", cfg ? cfg->profiles[cfg->active_profile].fmo_host : "--");
            break;
        case PG_STATION_LIST:
            if (fmo && i < fmo->pinnedListCount())
            {
                auto *l = fmo->pinnedList();
                char clean[64];
                _sanitizeDisplay(l[i].name, clean, sizeof(clean));   // 过滤特殊字符，防方块
                snprintf(t, m, "%s%s", clean, l[i].uid == fmo->stationUID() ? " *" : "");
            }
            break;
        case PG_DEVINFO:
            switch (i)
            {
            case 0:  snprintf(t, m, "Chip: %s", ESP.getChipModel()); break;
            case 1:  snprintf(t, m, "Cores: %d @ %dMHz", ESP.getChipCores(), ESP.getCpuFreqMHz()); break;
            case 2:  snprintf(t, m, "Flash: %uMB", (uint32_t)(ESP.getFlashChipSize() / (1024 * 1024))); break;
            case 3:  snprintf(t, m, "PSRAM: %uKB free", (uint32_t)(ESP.getFreePsram() / 1024)); break;
            case 4:  snprintf(t, m, "Heap: %uKB free", (uint32_t)(ESP.getFreeHeap() / 1024)); break;
            case 5:  snprintf(t, m, "SDK: %s", ESP.getSdkVersion()); break;
            case 6:  snprintf(t, m, "Board: M5StopWatch"); break;
            case 7:  snprintf(t, m, "Display: %dx%d", FMO_LCD_W, FMO_LCD_H); break;
            case 8:  snprintf(t, m, "M5Unified: %s", FMO_M5UNIFIED_VER); break;
            case 9:  snprintf(t, m, "MAC: %s", WiFi.macAddress().c_str()); break;
            case 10: snprintf(t, m, "Firmware: %s", FMO_VERSION_TEXT); break;
            case 11: snprintf(t, m, "Built: %s %s", __DATE__, __TIME__); break;
            case 12: snprintf(t, m, "Sketch: %u/%uKB", (uint32_t)(ESP.getSketchSize() / 1024), (uint32_t)(ESP.getFreeSketchSpace() / 1024)); break;
            case 13: snprintf(t, m, "Battery: %dmV %d%%%s", battery ? battery->voltage_mv() : 0, _batPercent, _batCharging ? " chg" : ""); break;
            case 14: snprintf(t, m, "SSID: %s", wifi && wifi->ssid()[0] ? wifi->ssid() : "--"); break;
            case 15: snprintf(t, m, "IP: %s", wifi ? wifi->ip() : "--"); break;
            case 16: snprintf(t, m, "RSSI: %ddBm", wifi ? wifi->rssi() : 0); break;
            case 17: snprintf(t, m, "FS: %u/%uKB", (uint32_t)(LittleFS.usedBytes() / 1024), (uint32_t)(LittleFS.totalBytes() / 1024)); break;
            case 18: { uint32_t up = millis() / 1000; snprintf(t, m, "Running time: %02u:%02u:%02u", up / 3600, (up % 3600) / 60, up % 60); break; }
            }
            break;
        case PG_ABOUT:
            snprintf(t, m, "FMO Watch " FMO_VERSION_TEXT);
            break;
        case PG_BT_SCAN:
            if (bt) {
                auto* d = bt->scanDevice(i);
                if (d) snprintf(t, m, "%s (%ddBm)", d->name, d->rssi);
            }
            break;
        default:
            break;
        }
    }

    void buildMenu()
    {
        FMO_Settings *cfg = settings ? settings->data() : nullptr;
        if (_scrMenu)
            lv_obj_clean(_scrMenu);
        else
        {
            _scrMenu = lv_obj_create(nullptr);
            lv_obj_set_style_bg_color(_scrMenu, H(C_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(_scrMenu, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(_scrMenu, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        memset(_menuBtns, 0, sizeof(_menuBtns));
        _menuCount = 0;
        lv_obj_t *title = lv_label_create(_scrMenu);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(title, H(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(title, 466);
        lv_obj_set_pos(title, 0, 28);   // 标题整体下移20px（含各子页标题）
        lv_label_set_text(title, pageTitle());

        // ── WiFi 扫描页：先填充结果再计算菜单项数（必须在 itemCount() 之前！） ──
        if (_pg == PG_WIFI_SCAN)
        {
            Serial.printf("[UI] buildMenu WIFI_SCAN: scanning=%d nets=%d\n", _scanning ? 1 : 0, (int)_nets.size());
            if (wifi)
            {
                FMO_ScanState st = wifi->scanState();
                Serial.printf("[UI]   scanState=%d\n", (int)st);
                if (st == SCAN_DONE)
                {
                    if (_nets.empty()) {
                        auto &r = wifi->scanResults();
                        for (size_t i = 0; i < r.size() && i < 16; i++)
                            _nets.push_back({r[i].ssid, r[i].rssi, r[i].open});
                        Serial.printf("[UI]   fill: %d nets\n", (int)_nets.size());
                    }
                    _scanning = false;
                }
                else if (st == SCAN_ERROR)
                    _scanning = false;
            }
        }

        int n = itemCount();
        if (n == 0)
        {
            lv_scr_load(_scrMenu);
            return;
        }
        // ── 呼号查询结果页：标题 + 呼号(英文) + 地址(中文)，居中两行 ──
        if (_pg == PG_LOOKUP_RESULT)
        {
            // 第一行：呼号（英文，琥珀色，大字）
            lv_obj_t *lc = lv_label_create(_scrMenu);
            lv_obj_set_style_text_font(lc, &lv_font_montserrat_32, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(lc, H(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(lc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_width(lc, 466);
            lv_obj_set_pos(lc, 0, 190);
            lv_label_set_text(lc, _lookupCall[0] ? _lookupCall : "--");

            // 第二行：地址（中文）
            lv_obj_t *ll = lv_label_create(_scrMenu);
            lv_obj_set_style_text_font(ll,
                _g_font_hansan_24 ? _g_font_hansan_24 : &lv_font_montserrat_24,
                LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(ll, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(ll, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_width(ll, 466);
            lv_obj_set_pos(ll, 0, 235);
            lv_label_set_text(ll, _lookupLoc[0] ? _lookupLoc : "未找到");

            // 返回提示
            mk(_scrMenu, "B: Back", C_MUTED, 0, 290, 466, &lv_font_montserrat_18);
            lv_scr_load(_scrMenu);
            return;
        }
        if (_pg == PG_BT_SCAN)
        {
            if (bt && bt->isScanning())
            {
                mk(_scrMenu, "Scanning...", C_WHITE, 0, 200, 466, &lv_font_montserrat_24);
                lv_scr_load(_scrMenu);
                return;
            }
            if (!bt || bt->scanCount() == 0)
            {
                mk(_scrMenu, "No devices", C_MUTED, 0, 200, 466, &lv_font_montserrat_24);
                mk(_scrMenu, "A+B: back", C_ACCENT, 0, 240, 466, &lv_font_montserrat_24);
                lv_scr_load(_scrMenu);
                return;
            }
        }
        if (_pg == PG_WIFI_SCAN)
        {
            // 结果已在前面的填充块处理，这里只处理 Scanning/No networks 显示
            if (_scanning)
            {
                mk(_scrMenu, "Scanning...", C_WHITE, 0, 200, 466, &lv_font_montserrat_24);
                lv_scr_load(_scrMenu);
                return;
            }
            if (_nets.empty())
            {
                Serial.println("[UI] WiFi no networks");
                mk(_scrMenu, "No networks", C_MUTED, 0, 200, 466, &lv_font_montserrat_24);
                mk(_scrMenu, "A+B: back", C_ACCENT, 0, 240, 466, &lv_font_montserrat_24);
                lv_scr_load(_scrMenu);
                return;
            }
        }
        if (_pg == PG_ABOUT)
        {
            mk(_scrMenu, "M5 FMO Watch", C_WHITE, 0, 150, 466, &lv_font_montserrat_24);
            mk(_scrMenu, "Develop by BD6JNF", C_GRAY, 0, 210, 466, &lv_font_montserrat_24);
            mk(_scrMenu, "Ver 1.5.0", C_ACCENT, 0, 270, 466, &lv_font_montserrat_24);
            lv_scr_load(_scrMenu);
            return;
        }
        _menuList = lv_obj_create(_scrMenu);
        lv_obj_set_size(_menuList, 440, 380);
        lv_obj_set_pos(_menuList, 13, 60);   // 随标题下移20px（避免与标题重叠）
        lv_obj_set_style_bg_opa(_menuList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(_menuList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(_menuList, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_flex_flow(_menuList, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(_menuList, LV_DIR_VER);
        lv_obj_set_style_pad_row(_menuList, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        for (int i = 0; i < n; i++)
        {
            char t[72];
            getMenuText(i, t, 72);
            if (_pg == PG_WIFI_SCAN && i < (int)_nets.size())
            {
                snprintf(t, 72, "%s %s", _nets[i].open ? " " : "*", _nets[i].ssid.c_str());
            }
            lv_obj_t *item = lv_obj_create(_menuList);
            lv_obj_set_size(item, 440, 48);
            lv_obj_set_style_bg_opa(item, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(item, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(item, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            item->user_data = (void *)(intptr_t)i;
            lv_obj_add_event_cb(item, _menuItemClick, LV_EVENT_CLICKED, this);
            lv_obj_t *lbl = lv_label_create(item);
            // 台站列表：始终用思源黑体（站名主要是中文）
            // WiFi扫描结果：SSID 含非ASCII（中文）才用思源黑体，纯英文用 Montserrat 保持原样
            const lv_font_t *rowFont = &lv_font_montserrat_24;
            if (_g_font_hansan_24) {
                if (_pg == PG_STATION_LIST) rowFont = _g_font_hansan_24;
                else if (_pg == PG_WIFI_SCAN && i < (int)_nets.size()) {
                    // WiFi扫描结果：SSID 含中文（非ASCII）才用思源黑体
                    if (_strHasNonAscii(_nets[i].ssid.c_str())) rowFont = _g_font_hansan_24;
                }
                else if (_pg == PG_WIFI_MENU && i == 1 && cfg) {
                    // WiFi设置页 SSID 行：SSID 含中文（非ASCII）才用思源黑体
                    if (_strHasNonAscii(cfg->profiles[cfg->active_profile].wifi_ssid)) rowFont = _g_font_hansan_24;
                }
                else if (_pg == PG_DEVINFO && i == 14 && wifi) {
                    // 设备信息页 SSID 行：已连接 SSID 含中文（非ASCII）才用思源黑体
                    const char *ss = wifi->ssid()[0] ? wifi->ssid() : "--";
                    if (_strHasNonAscii(ss)) rowFont = _g_font_hansan_24;
                }
            }
            lv_obj_set_style_text_font(lbl, rowFont, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(lbl, i == _sel ? H(C_ACCENT) : H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_center(lbl);
            lv_obj_set_width(lbl, 430);
            lv_label_set_text(lbl, t);
            _menuBtns[i] = lbl;
        }
        _menuCount = n;
        lv_scr_load(_scrMenu);
    }
    void menuDown()
    {
        int n = itemCount();
        if (n == 0)
            return;
        int oldSel = _sel;
        _sel++;
        if (_sel >= n)
            _sel = 0;
        // 动态内容页（扫描/台站列表）仍需重建以刷新内容
        if (_pg == PG_WIFI_SCAN || _pg == PG_BT_SCAN || _pg == PG_STATION_LIST)
        {
            buildMenu();
            ensureSelVisible();
            return;
        }
        // 固定内容页：轻量更新高亮（按住A持续滚动时避免每250ms全量重建，防卡顿）
        if (_menuBtns[oldSel]) lv_obj_set_style_text_color(_menuBtns[oldSel], H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        if (_menuBtns[_sel])   lv_obj_set_style_text_color(_menuBtns[_sel],   H(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
        ensureSelVisible();
    }
    // 选中项超出可视区时滚动，使选中项始终可见（设置菜单8项超出列表可视高度）
    void ensureSelVisible()
    {
        if (!_menuList) return;
        const int rowH = 52;                     // 行高48 + 间距4
        const int listH = 380;
        if (itemCount() * rowH <= listH) { lv_obj_scroll_to_y(_menuList, 0, LV_ANIM_OFF); return; }
        int target = _sel * rowH - (listH - 48);
        if (target < 0) target = 0;
        lv_obj_scroll_to_y(_menuList, target, LV_ANIM_OFF);   // 无动画：避免动画逐帧全量重绘导致系统卡顿
    }
    // Device Info 页：按A向上滚动一行（dir=+1 内容上移）
    // max = 当前滚动位置 + 剩余可滚量（scroll_bottom 是"剩余"量，滚到底=0，不能直接当最大值）
    void scrollList(int dir)
    {
        if (!_menuList) return;
        lv_coord_t max = lv_obj_get_scroll_y(_menuList) + lv_obj_get_scroll_bottom(_menuList);
        lv_coord_t y = lv_obj_get_scroll_y(_menuList) + dir * 52;
        if (y > max) y = max;
        if (y < 0) y = 0;
        lv_obj_scroll_to_y(_menuList, y, LV_ANIM_OFF);   // 无动画：避免动画逐帧全量重绘导致系统卡顿
    }
    void menuSelect()
    {
        FMO_Settings *cfg = settings ? settings->data() : nullptr;
        switch (_pg)
        {
        case PG_SETTINGS:
            switch (_sel)
            {
            case 0:
                _pg = PG_STATION_LIST;
                _sel = 0;
                if (fmo)
                    fmo->requestPinnedList(0, 16);
                buildMenu();
                break;
            case 1:
                _pg = PG_AUDIO_SET;
                _sel = 0;
                buildMenu();
                break;
            case 2:
                _pg = PG_WIFI_MENU;
                _sel = 0;
                buildMenu();
                break;
            case 3:
                startInput(INP_HOST, cfg ? cfg->profiles[cfg->active_profile].fmo_host : "");
                break;
            case 4:
                startInput(INP_CALLSIGN, cfg ? cfg->owner_callsign : "");
                break;
            case 5:
                startInput(INP_AUDIO_IP, cfg ? cfg->audio_tx_ip : "");   // 语音发射IP
                break;
            case 6:
                startInput(INP_LOOKUP, "");   // 呼号查询
                break;
            case 7:
                _pg = PG_DEVINFO;
                _sel = 0;
                buildMenu();
                break;
            case 8:
                _pg = PG_ABOUT;
                _sel = 0;
                buildMenu();
                break;
            case 9:
                closeMenu();
                break;
            }
            break;
        case PG_WIFI_MENU:
            switch (_sel)
            {
            case 0:
                _pg = PG_WIFI_SCAN;
                _sel = 0;
                _nets.clear();
                _scanning = true;
                if (wifi)
                    wifi->scanAsync();
                buildMenu();
                break;
            case 1:
                startInput(INP_SSID, cfg ? cfg->profiles[cfg->active_profile].wifi_ssid : "");
                break;
            case 2:
                startInput(INP_PASS, cfg ? cfg->profiles[cfg->active_profile].wifi_password : "");
                break;
            case 3:
                _pg = PG_SETTINGS;
                _sel = 3;
                buildMenu();
                break;
            }
            break;
        case PG_WIFI_SCAN:
            if (_sel >= 0 && _sel < (int)_nets.size() && settings && cfg)
            {
                settings->setProfileWiFi(cfg->active_profile, _nets[_sel].ssid.c_str(), nullptr);
                startInput(INP_PASS, "");
            }
            else
            {
                // 无网络可选 → 返回 WiFi 菜单（按 B 返回上一级）
                _pg = PG_WIFI_MENU;
                _sel = 0;
                buildMenu();
            }
            break;
        case PG_AUDIO_SET:
            switch (_sel)
            {
            case 0:
                toggleMute();
                buildMenu();
                break;
            case 1:
                toggleBT();              // 切换BT开关
                if (settings && settings->data()->bt_enabled) {
                    _pg = PG_BT_SCAN;    // 开启 → 进入搜索界面
                    _sel = 0;
                    if (bt) bt->startScan(20);
                }
                buildMenu();
                break;
            case 2:
                openVolume();   // 音量触摸调节页
                break;
            case 3:
                toggleVibrate();
                buildMenu();
                break;
            case 4:
                _pg = PG_SETTINGS;
                _sel = 2;
                buildMenu();
                break;
            }
            break;
        case PG_STATION_LIST:
            if (fmo && _sel < fmo->pinnedListCount())
            {
                fmo->setCurrentStation(fmo->pinnedList()[_sel].uid);
                closeMenu();
            }
            else
            {
                // 无有效台站 → 返回设置菜单（按 B 返回上一级）
                _pg = PG_SETTINGS;
                _sel = 0;
                buildMenu();
            }
            break;
        case PG_ABOUT:
            _pg = PG_SETTINGS;
            _sel = 6;
            buildMenu();
            break;
        case PG_DEVINFO:
            _pg = PG_SETTINGS;
            _sel = 6;   // 返回 Device Info 位置
            buildMenu();
            break;
        case PG_LOOKUP_RESULT:
            _pg = PG_SETTINGS;
            _sel = 5;   // 返回 CallSignSearch 位置
            buildMenu();
            break;
        case PG_BT_SCAN:
            if (bt && bt->connectToScanDevice(_sel))
            {
                Serial.println("[蓝牙] 连接目标已设置");
            }
            _pg = PG_AUDIO_SET;
            _sel = 1;
            buildMenu();
            break;
        case PG_FMO_HOST:
            startInput(INP_HOST, cfg ? cfg->profiles[cfg->active_profile].fmo_host : "");
            break;
        case PG_CALLSIGN:
            startInput(INP_CALLSIGN, cfg ? cfg->owner_callsign : "");
            break;
        default:
            break;
        }
    }
    void toggleMute()
    {
        if (!settings)
            return;
        bool m = !settings->data()->audio_muted;
        settings->setMuted(m);
        if (audio)
            audio->setMuted(m);
        if (fmo)
        {
            if (m)
                fmo->audioDisable();
            else
                fmo->audioEnable();
        }
    }
    void toggleBT()
    {
        if (!settings)
            return;
        settings->setBTEnabled(!settings->data()->bt_enabled);
    }
    void toggleVibrate()
    {
        if (!settings)
            return;
        settings->setVibrateEnabled(!settings->data()->vibrate_enabled);
    }

    // ═══ VOLUME ═══
    void openVolume()
    {
        _menuOpen = true;
        _pg = PG_VOLUME;
        if (_scrVolume)
            lv_obj_clean(_scrVolume);
        else
        {
            _scrVolume = lv_obj_create(nullptr);
            lv_obj_set_style_bg_color(_scrVolume, H(C_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(_scrVolume, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(_scrVolume, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        lv_scr_load(_scrVolume);

        // 标题
        mk(_scrVolume, "Volume", C_ACCENT, 0, 8, 466, &lv_font_montserrat_24);

        // ── 上半区(0-233)：音量+（+号居中，点击+5） ──
        _volUp = lv_obj_create(_scrVolume);
        lv_obj_set_size(_volUp, 466, 233);
        lv_obj_set_pos(_volUp, 0, 0);
        lv_obj_set_style_bg_opa(_volUp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(_volUp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(_volUp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(_volUp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scroll_dir(_volUp, LV_DIR_NONE);
        lv_obj_add_event_cb(_volUp, _volTapUp, LV_EVENT_CLICKED, this);
        // +号居中于上半区（子对象，点击冒泡到容器）
        lv_obj_t* plus = lv_label_create(_volUp);
        lv_obj_set_style_text_font(plus, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(plus, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(plus);
        lv_label_set_text(plus, "+");

        // ── 中部：当前音量值（两半交界处） ──
        _volLabel = lv_label_create(_scrVolume);
        lv_obj_set_style_text_font(_volLabel, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_volLabel, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(_volLabel, 466);
        lv_obj_set_style_text_align(_volLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(_volLabel, 0, 205);
        updateVolLabel();

        // ── 下半区(233-466)：音量-（-号居中，点击-5） ──
        _volDown = lv_obj_create(_scrVolume);
        lv_obj_set_size(_volDown, 466, 233);
        lv_obj_set_pos(_volDown, 0, 233);
        lv_obj_set_style_bg_opa(_volDown, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(_volDown, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(_volDown, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(_volDown, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scroll_dir(_volDown, LV_DIR_NONE);
        lv_obj_add_event_cb(_volDown, _volTapDown, LV_EVENT_CLICKED, this);
        // -号居中于下半区
        lv_obj_t* minus = lv_label_create(_volDown);
        lv_obj_set_style_text_font(minus, &lv_font_montserrat_48, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(minus, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(minus);
        lv_label_set_text(minus, "-");
    }

    void closeVolume()
    {
        _pg = PG_AUDIO_SET;
        _sel = 2;               // 回到"Volume"项
        buildMenu();            // 重建音频菜单（显示最新音量值）
        lv_scr_load(_scrMenu);
    }

    void volAdjust(int delta)
    {
        if (!settings) return;
        int v = settings->data()->audio_volume + delta;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        settings->setVolume(v);      // 持久化
        if (audio) audio->setVolume(v);  // 应用
        updateVolLabel();
    }

    void updateVolLabel()
    {
        if (!_volLabel || !settings) return;
        char b[8];
        snprintf(b, 8, "%d", settings->data()->audio_volume);
        lv_label_set_text(_volLabel, b);
    }

    // ═══ 通用反馈弹窗（3秒自动关闭） ═══
    static void _feedbackTimerCb(lv_timer_t* t)
    {
        if (t && t->user_data) lv_msgbox_close((lv_obj_t*)t->user_data);
        lv_timer_del(t);
    }
    void showFeedback(const char* msg)
    {
        if (!msg) return;
        static lv_obj_t* fb = nullptr;
        if (fb) lv_msgbox_close(fb);
        fb = lv_msgbox_create(_scrMenu, "WiFi", msg, nullptr, 0);
        lv_obj_set_style_text_font(lv_msgbox_get_text(fb), &lv_font_montserrat_18,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(fb, 300);
        lv_obj_center(fb);
        lv_timer_create(_feedbackTimerCb, 3000, fb);
    }

    // ═══ WiFi 密码连接结果检查（定时器回调） ═══
    static void _wifiPwCheckCb(lv_timer_t* t)
    {
        FMO_UI_LVGL* self = (FMO_UI_LVGL*)t->user_data;
        self->_wifiPwTimer = nullptr;
        self->_wifiPwWait = false;
        lv_timer_del(t);
        if (!self->wifi) return;
        // 连接结果弹窗
        if (self->wifi->state() == WF_CONNECTED)
            self->showFeedback("已连接");
        else
            self->showFeedback("WiFi 密码不正确");
    }

    // ═══ INPUT ═══
    void startInput(InputMode m, const char *cur)
    {
        _inp = m;
        _prevPg = _pg;
        if (_scrInput)
            lv_obj_clean(_scrInput);
        else
        {
            _scrInput = lv_obj_create(nullptr);
            lv_obj_set_style_bg_color(_scrInput, H(C_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_pad_all(_scrInput, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        lv_obj_clear_flag(_scrInput, LV_OBJ_FLAG_SCROLLABLE);  // 输入界面禁止滑动
        lv_scr_load(_scrInput);
        const char *ti = "Input";
        switch (_inp)
        {
        case INP_SSID:
            ti = "WiFi SSID";
            break;
        case INP_PASS:
            ti = "WiFi Password";
            break;
        case INP_HOST:
            ti = "Server Host";
            break;
        case INP_CALLSIGN:
            ti = "Callsign";
            break;
        case INP_LOOKUP:
            ti = "CallSignSearch";
            break;
        case INP_AUDIO_IP:
            ti = "AudioTX IP";
            break;
        default:
            break;
        }
        // 圆形屏幕适配（圆心233,233，半径220）：
        // 标题在圆顶、文本框居中于圆上部，键盘整体缩入圆内并水平居中，
        // 键盘背景透明 + 胶囊按钮，避免矩形底色和按钮被圆边裁切
        mk(_scrInput, ti, C_ACCENT, 0, 25, 466, &lv_font_montserrat_24);
        _ta = lv_textarea_create(_scrInput);
        lv_obj_set_size(_ta, 266, 48);
        lv_obj_set_pos(_ta, 100, 62);
        lv_textarea_set_max_length(_ta, 63);
        lv_textarea_set_one_line(_ta, true);
        // SSID 输入框：当前值为中文（非ASCII）时用思源黑体，否则 Montserrat
        const lv_font_t *taFont = &lv_font_montserrat_18;
        if (m == INP_SSID && _g_font_hansan_24 && cur && _strHasNonAscii(cur))
            taFont = _g_font_hansan_24;
        lv_obj_set_style_text_font(_ta, taFont, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(_ta, H(C_SURFACE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_ta, H(C_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(_ta, H(C_MUTED), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(_ta, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(_ta, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        if (m == INP_PASS)
            lv_textarea_set_password_mode(_ta, true);
        lv_textarea_set_text(_ta, cur && cur[0] ? cur : "");
        _kb = lv_keyboard_create(_scrInput);
        _kbMode = 0;             // 每次打开输入界面从字母(T9)模式开始
        _kbUpper = false;        // 从大写状态开始（便于输入呼号）
        _kbPending = false;      // 清除T9待确认状态
        _kbLastBtn = 0xFFFFFFFF; // 清除T9多击状态
        // 九宫格 5行×3列：加宽按钮更易点按（x 63~403, y 111~381，四角距圆心≤223在圆内）
        lv_obj_set_size(_kb, 340, 270);
        // lv_keyboard 构造时默认对齐 BOTTOM_MID，必须用 lv_obj_align 覆盖，
        // 否则 set_pos 不生效，键盘会被拉到底部中间而超出圆形屏幕
        lv_obj_align(_kb, LV_ALIGN_TOP_LEFT, 63, 111);
        lv_obj_clear_flag(_kb, LV_OBJ_FLAG_SCROLLABLE);  // 键盘自身禁止滑动
        // 键盘容器透明，避免圆形屏幕上出现矩形底色
        lv_obj_set_style_bg_opa(_kb, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(_kb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(_kb, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_row(_kb, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_column(_kb, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        // 按钮：圆角、深灰底、白字、按下高亮
        lv_obj_set_style_radius(_kb, 12, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(_kb, H(C_SURFACE), LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(_kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(_kb, H(C_WHITE), LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(_kb, &lv_font_montserrat_16, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(_kb, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(_kb, H(C_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_keyboard_set_map(_kb, LV_KEYBOARD_MODE_USER_1, fmo_kb_map_alpha, fmo_kb_ctrl_17);
        lv_keyboard_set_mode(_kb, LV_KEYBOARD_MODE_USER_1);
        lv_keyboard_set_textarea(_kb, _ta);
        // 移除 LVGL 默认键盘事件，改用自定义处理（支持 T9 多击/模式切换）
        lv_obj_remove_event_cb(_kb, lv_keyboard_def_event_cb);
        lv_obj_add_event_cb(_kb, _kbValueChanged, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(_kb, _kbReady, LV_EVENT_READY, this);
        lv_obj_move_foreground(_ta);
    }
    void onKbValueChanged()
    {
        if (!_kb || !_ta) return;
        uint16_t btn = lv_btnmatrix_get_selected_btn(_kb);
        if (btn == LV_BTNMATRIX_BTN_NONE) return;
        const char *txt = lv_btnmatrix_get_btn_text(_kb, btn);
        if (!txt) return;

        // ── 模式切换键 ──
        if (strcmp(txt, "ABC") == 0)  { _switchKbMode(0); return; }  // 切到字母
        if (strcmp(txt, "#+=") == 0)  { _switchKbMode(2); return; }  // 切到符号
        if (strcmp(txt, "123") == 0)  { _switchKbMode(1); return; }  // 切到数字

        // ── 大小写切换（↑ 键，仅字母模式） ──
        if (strcmp(txt, LV_SYMBOL_UP) == 0)
        {
            _kbUpper = !_kbUpper;
            _kbPending = false;      // 确认当前待确认字符
            _kbLastBtn = 0xFFFFFFFF;   // 切换后重置T9连击
            lv_keyboard_set_map(_kb, LV_KEYBOARD_MODE_USER_1,
                                _kbUpper ? fmo_kb_map_alpha_upper : fmo_kb_map_alpha,
                                fmo_kb_ctrl_17);
            return;
        }

        // ── 功能键 ──
        if (strcmp(txt, LV_SYMBOL_OK) == 0)        { _kbPending=false; _kbLastBtn=0xFFFFFFFF; endInput(); return; }
        if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) { _kbPending=false; _kbLastBtn=0xFFFFFFFF; lv_textarea_del_char(_ta); return; }
        if (strcmp(txt, " ") == 0)                 { _kbPending=false; _kbLastBtn=0xFFFFFFFF; lv_textarea_add_text(_ta, " "); return; }

        // ── 字母模式：T9 多击循环（2abc → a→b→c→2→a...） ──
        if (_kbMode == 0)
        {
            const uint32_t NOW = millis();
            // T9 键组：小写/大写两套（与 fmo_kb_map_alpha[_upper] 前三行对应）
            static const char *t9[]  = { "2abc", "3def", "4ghi", "5jkl", "6mno", "7pqrs", "8tuv", "9wxyz" };
            static const char *t9Up[] = { "2ABC", "3DEF", "4GHI", "5JKL", "6MNO", "7PQRS", "8TUV", "9WXYZ" };
            const char **t9s = _kbUpper ? t9Up : t9;
            int idx = -1;
            for (int i = 0; i < 8; i++)
                if (strcmp(txt, t9s[i]) == 0) { idx = i; break; }

            if (idx >= 0)
            {
                // 老式手机 T9 交互：点按循环"选择"字符，换键/停顿后"确认"
                // 例："2abc" → 点1下显示'2'、点2下变'a'、点3下变'b'、点4下变'c'（替换文本框末尾）
                //   - 点不同键 或 停顿>1.5秒 → 确认当前字符，开始新的选择
                const uint32_t NOW2 = millis();
                bool sameKey = (btn == _kbLastBtn) && (NOW2 - _kbLastTime < 1500);
                _kbLastBtn = btn;
                _kbLastTime = NOW2;
                // t9s[idx] = "2abc"/"2ABC"（索引0=数字, 1..=字母，顺序即键上显示）
                const char *grp = t9s[idx];
                int grpLen = (int)strlen(grp);       // 4
                int cycle = sameKey ? _kbT9Idx + 1 : 0;
                if (cycle >= grpLen) cycle = 0;      // 循环回第1个字符
                _kbT9Idx = cycle;
                char ch = grp[cycle];                // 当前选中的字符
                if (sameKey && _kbPending) {
                    // 同键连击：删掉上一个待确认字符（只删1个），再输入新字符替换
                    lv_textarea_del_char(_ta);
                }
                char buf[2] = { ch, 0 };
                lv_textarea_add_text(_ta, buf);
                _kbPending = true;                   // 当前字符待确认（换键/停顿后确认）
                return;
            }
            // 数字键 1/0 或纯数字：直接输入（确认上一个待确认字符）
            if (txt[0] >= '0' && txt[0] <= '9' && txt[1] == 0)
            {
                _kbPending = false;
                _kbLastBtn = 0xFFFFFFFF;
                lv_textarea_add_text(_ta, txt);
                return;
            }
            _kbPending = false;
            _kbLastBtn = 0xFFFFFFFF;
            lv_textarea_add_text(_ta, txt);
            return;
        }

        // ── 数字/符号模式：直接输入 ──
        _kbPending = false;
        _kbLastBtn = 0xFFFFFFFF;
        lv_textarea_add_text(_ta, txt);
    }

    // 切换键盘模式（0=字母T9, 1=数字, 2=符号）
    void _switchKbMode(int mode)
    {
        if (mode == _kbMode) return;
        _kbMode = mode;
        _kbLastBtn = 0xFFFFFFFF;
        const char **map;
        if (mode == 0)      map = _kbUpper ? fmo_kb_map_alpha_upper : fmo_kb_map_alpha;  // 字母（按大小写状态）
        else if (mode == 1) map = fmo_kb_map_num;
        else                map = fmo_kb_map_sym;
        lv_keyboard_set_map(_kb, LV_KEYBOARD_MODE_USER_1, map, fmo_kb_ctrl_17);
    }

    void endInput()
    {
        const char *text = lv_textarea_get_text(_ta);
        if (text && text[0])
        {
            FMO_Settings *c = settings ? settings->data() : nullptr;
            if (c)
            {
                switch (_inp)
                {
                case INP_SSID:
                    settings->setProfileWiFi(c->active_profile, text, nullptr);
                    break;
                case INP_PASS:
                    settings->setProfileWiFi(c->active_profile, nullptr, text);
                    if (wifi)
                    {
                        auto *p = &c->profiles[c->active_profile];
                        wifi->setCredentials(p->wifi_ssid, p->wifi_password);
                        wifi->reconnect();
                        _wifiStatus = 1;
                        _wifiStatTime = millis();
                        // 6 秒后检查连接结果并弹窗反馈
                        if (_wifiPwTimer) lv_timer_del(_wifiPwTimer);
                        _wifiPwWait = true;
                        _wifiPwTimer = lv_timer_create(_wifiPwCheckCb, 6000, this);
                    }
                    break;
                case INP_HOST:
                    settings->setProfileFmoHost(c->active_profile, text);
                    if (fmo)
                        fmo->setHost(text);
                    break;
                case INP_CALLSIGN:
                    settings->setOwnerCallsign(text);
                    break;
                case INP_AUDIO_IP:
                    settings->setAudioTxIp(text);   // 语音发射目标IP（下次发送生效）
                    break;
                case INP_LOOKUP: {
                    // 呼号查询：查离线表 → 切到查询结果页
                    strncpy(_lookupCall, text, sizeof(_lookupCall) - 1); _lookupCall[sizeof(_lookupCall) - 1] = 0;
                    _lookupLoc[0] = 0;
                    if (_g_callLoc && text[0]) {
                        if (!_g_callLoc->lookup(text, _lookupLoc, sizeof(_lookupLoc)))
                            strncpy(_lookupLoc, "未找到", sizeof(_lookupLoc) - 1);
                    } else {
                        strncpy(_lookupLoc, "未找到", sizeof(_lookupLoc) - 1);
                    }
                    _inp = INP_NONE;
                    _pg = PG_LOOKUP_RESULT;
                    _sel = 0;
                    _menuOpen = true;
                    buildMenu();
                    lv_scr_load(_scrMenu);
                    return;   // 已切换到结果页，跳过末尾通用返回处理
                }
                default:
                    break;
                }
            }
        }
        _inp = INP_NONE;
        _pg = _prevPg;
        _sel = 0;
        _menuOpen = true;
        buildMenu();
        lv_scr_load(_scrMenu);
    }
};
