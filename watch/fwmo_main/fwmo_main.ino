// #include <nvs_flash.h>
/**
 * fwmo_main.ino — M5StopWatch FMO通联伴侣 主程序
 *
 * 硬件：M5StopWatch C152 (ESP32-S3 + 1.54" AMOLED 466×466)
 * 图形库：LVGL v8.4（双缓冲 PARTIAL 模式 + PSRAM + flush 直接推屏）
 *
 * 显示方式（参考官方固件 hal_display.cpp）：
 *   1. LVGL 双缓冲 PARTIAL 渲染小块区域到行缓冲（PSRAM）
 *   2. flush 回调将每个脏区域直接 writePixels 推屏
 *   3. 不使用整帧延迟推屏，避免 PSRAM 缓存一致性问题（花屏）
 *
 * ⚠️ 关键修复：vibrate PIN=46 实际是 PSRAM 数据线，不是振动马达！
 *   pinMode(46,OUTPUT) 会短路 PSRAM 总线，破坏帧缓冲数据。
 *   振动已禁用，待找到正确 GPIO 后重新启用。
 */

// ── 核心库 ──
#include <lvgl.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#if FMO_BT_CLASSIC_SUPPORTED
#include <BluetoothA2DPSource.h>   // 强制Arduino检测ESP32-A2DP库（否则__has_include不触发库加载）
#endif

// ── 项目模块 ──
#include "fwmo_config.h"   // 引脚定义、颜色常量
#include "fwmo_settings.h" // 设置管理（Flash存储）
#include "fwmo_wifi.h"     // WiFi管理
#include "fwmo_audio.h"    // ES8311音频
#include "fwmo_bt.h"       // 蓝牙串口
#include "fwmo_vibrate.h"  // 振动马达（⚠已禁用）
#include "fwmo_battery.h"  // 电池检测
#include "fwmo_cache.h"    // 音频缓存
#include "fwmo_client.h"   // FMO通联客户端
#include "fwmo_callloc.h"  // 呼号→城市 离线查询
#include "fwmo_ui_lvgl.h"  // LVGL界面
#include "fwmo_weather.h" // 本地天气（wttr.in）
#include "fwmo_mic_wifi.h"   // 麦克风→WiFi UDP 语音发送（备用方案）
#include "fwmo_mic_espnow.h" // 麦克风→ESP-NOW 低延迟语音发送（推荐，与WiFi共存）

// ══════════════════════════════════════════════
// 全局对象
// ══════════════════════════════════════════════
M5GFX *display = nullptr;  // AMOLED显示驱动
FMO_SettingsMgr settings;  // 设置管理器
FMO_WifiManager wifi;      // WiFi管理器
FMO_Audio audio;           // 音频输出
FMO_Bluetooth bt;          // 蓝牙串口
FMO_Vibrate vibrate;       // 振动马达（⚠ GPIO46 待确认）
FMO_Battery battery;       // 电池检测
FMO_Cache cache;           // 音频缓存
FMO_Client fmoClient;      // FMO客户端
FMO_UI_LVGL *ui = nullptr; // LVGL界面对象
FMO_Weather weather;       // 天气（wttr.in免Key）
FMO_MicWifi micWifi;     // 麦克风→WiFi UDP 发送（备用）
FMO_MicEspnow micEspnow; // 麦克风→ESP-NOW 发送（推荐，低延迟）
FMO_AudioFull audioFull; // 全双工音频（单 I2S0 TX+RX，参考 xiaozhi）
FMO_CallLoc callLoc;     // 呼号→城市 离线查询
FMO_CallLoc* _g_callLoc = &callLoc;   // UI 引用（fwmo_ui_lvgl.h extern）

// ── 中文字体（思源黑体，LittleFS→PSRAM加载，见 fwmo_font.h） ──
lv_font_t *_g_font_hansan_24 = nullptr; // 由 setup 中 font_hansan_24_load() 赋值

// ── LVGL 显示驱动 ──
static lv_disp_draw_buf_t draw_buf;   // 绘制缓冲描述符
static lv_disp_drv_t disp_drv;        // 显示驱动描述符
static lv_color_t *lv_buf1 = nullptr; // 行缓冲1（PSRAM）
static lv_color_t *lv_buf2 = nullptr; // 行缓冲2（PSRAM，双缓冲）

// ── 定时器变量 ──
unsigned long lastRssiRead = 0;   // 上次读取RSSI时间
unsigned long lastCacheClean = 0; // 上次清理缓存时间
unsigned long lastBtUpdate = 0;   // 上次蓝牙状态更新时间

// ══════════════════════════════════════════════
// LVGL flush 回调：LVGL每渲染一个区域调用一次
// 参考官方固件 hal_display.cpp：直接将该区域推屏（标准 flush 方式）
// ══════════════════════════════════════════════
static void lv_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px_map)
{
    if (!display)
    {
        lv_disp_flush_ready(drv);
        return;
    }

    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    uint32_t pixels = w * h;

    display->startWrite();
    display->setAddrWindow(area->x1, area->y1, w, h);

    // 参考官方固件：分块传输，避免 M5GFX SIMD 优化的 copy_rgb_fast 问题
    const uint32_t SAFE_CHUNK_SIZE = 8192; // 8K 像素/块
    if (pixels > SAFE_CHUNK_SIZE)
    {
        const lgfx::rgb565_t *src = (const lgfx::rgb565_t *)px_map;
        uint32_t remaining = pixels;
        while (remaining > 0)
        {
            uint32_t chunk = (remaining > SAFE_CHUNK_SIZE) ? SAFE_CHUNK_SIZE : remaining;
            display->writePixels(src, chunk);
            src += chunk;
            remaining -= chunk;
        }
    }
    else
    {
        display->writePixels((const lgfx::rgb565_t *)px_map, pixels);
    }

    display->endWrite();
    lv_disp_flush_ready(drv);
}

// ══════════════════════════════════════════════
// setup() — 上电初始化
// ══════════════════════════════════════════════
void setup()
{
    // ── 串口 ──
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== FMO Watch 启动 ===\n");



    // ── M5硬件初始化（AMOLED/电源管理等） ──
    // ⚠ 禁用内置 Speaker/Mic：I2S0 由 FMO_AudioFull 全双工接管（避免 i2s_new_channel 冲突）
    {
        auto m5cfg = M5.config();
        m5cfg.internal_spk = false;
        m5cfg.internal_mic = false;
        M5.begin(m5cfg);
    }
    display = &M5.Display;
    display->setBrightness(130); // 亮屏亮度（降低省电）

    // ── NTP时间同步（UTC+8北京时区） ──
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");

    // ── LVGL 图形库初始化 ──
    lv_init();

    // ── 显示驱动配置（466×466 AMOLED） ──
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = lv_flush;
    disp_drv.hor_res = 466;
    disp_drv.ver_res = 466;

    // ── 行缓冲分配（参考官方固件：双缓冲 PARTIAL 模式，PSRAM） ──
    const uint32_t LINE = 120; // 缓冲行数（官方固件 LV_BUFFER_LINE=120）
    lv_buf1 = (lv_color_t *)heap_caps_calloc(
        1, 466 * LINE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_buf2 = (lv_color_t *)heap_caps_calloc(
        1, 466 * LINE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lv_buf1 || !lv_buf2)
    {
        Serial.println("[错误] 帧缓冲分配失败");
        return;
    }
    Serial.printf("[内存] PSRAM:%d KB  空闲:%d KB\n",
                  ESP.getPsramSize() / 1024, ESP.getFreeHeap() / 1024);

    // 双缓冲 PARTIAL 模式：LVGL 渲染小块 → flush 回调直接推屏
    lv_disp_draw_buf_init(&draw_buf, lv_buf1, lv_buf2, 466 * LINE);
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // ── LittleFS 文件系统初始化（中文字库存储区，使用 ffat 分区） ──
    if (LittleFS.begin(false, "/littlefs", 8, "ffat"))
        Serial.println("[FS] LittleFS 挂载成功 (ffat 分区)");
    else
        Serial.println("[FS] LittleFS 挂载失败（需要先烧录文件系统镜像）");

    // ── 中文字体加载（思源黑体 24px，从 LittleFS 读入 PSRAM） ──
    Serial.println("[字体] 正在加载思源黑体到 PSRAM...");
    _g_font_hansan_24 = font_hansan_24_load();
    if (_g_font_hansan_24)
    {
        Serial.println("[字体] 中文字体加载成功");
        // °等符号回退到montserrat（中文字库无U+00B0字形）
        _g_font_hansan_24->fallback = &lv_font_montserrat_24;
    }
    else
        Serial.println("[字体] 中文字体加载失败(文件缺失或PSRAM不足)");

    // ── 呼号→城市 离线表加载（从 LittleFS 读入 PSRAM） ──
    callLoc.begin();

    // ── 创建界面 + 初始渲染（flush 回调会自动推屏） ──
    // 先初始化电池（立即读真实电量），让 UI 首帧就显示真实电量而非空图标
    battery.begin();
    ui = new FMO_UI_LVGL();
    ui->setDisplay(display);
    ui->_batPercent = battery.percent();   // 首帧用真实电量
    ui->_batCharging = battery.charging();
    ui->begin();

    // 强制多轮渲染确保首页完整
    lv_timer_handler();
    delay(5);
    lv_timer_handler();
    delay(5);
    lv_timer_handler();

    // ── 按键引脚 ──
    pinMode(PIN_KEY_A, INPUT_PULLUP); // 黄色按键 G2（下方）
    pinMode(PIN_KEY_B, INPUT_PULLUP); // 蓝色按键 G1（上方）

    // ── 设置管理器 ──
    // TODO: 下一次烧录删除下面3行 → 仅首次清空NVS
    // nvs_flash_erase();
    // nvs_flash_init();
    delay(100);
    settings.begin();
    auto *cfg = settings.data();
    Serial.printf("[配置] 配置文件:%d/%d  呼号:%s\n",
                  cfg->active_profile + 1, PROFILE_MAX, cfg->owner_callsign);

    ui->wifi = &wifi;
    ui->settings = &settings;
    ui->fmo = &fmoClient;
    ui->audio = &audio;
    ui->weather = &weather;
    ui->bt = &bt;
    ui->battery = &battery;

    // ── 天气任务启动（后台拉取wttr.in） ──
    weather.begin();
    

    // ── 振动马达（GPIO8，用户确认） ──
    vibrate.begin();

    // ── 音频初始化（全双工 ES8311，单 I2S0 TX+RX，参考 xiaozhi） ──
    if (audioFull.begin())
    {
        audio.setAudioFull(&audioFull);   // 播放走全双工 TX
        audio.begin();                    // 只准备 ring buffer
        audio.setMuted(false);
        audio.setVolume(cfg->audio_volume);
        audio.start();
        Serial.println("[音频] 全双工就绪");
    }
    else
    {
        Serial.println("[音频] 失败");
    }

    // ── 蓝牙 ──
#if FMO_BT_LIB_PRESENT
    Serial.println("[蓝牙] ESP32-A2DP 库已安装，蓝牙功能可用");
#else
    Serial.println("[蓝牙] 当前芯片不支持经典蓝牙(A2DP)，蓝牙不可用");
#endif
    if (cfg->bt_enabled)
    {
        if (bt.begin())
            Serial.println("[蓝牙] 就绪");
        else
            cfg->bt_enabled = false;
    }

    // ── 麦克风（A键按住→WiFi UDP 语音发送到 FMO 发射端） ──
    micWifi.setSettings(&settings);   // 发射目标IP从设置读取（菜单可改，不用刷固件）
    micWifi.begin();

    // ── WiFi 连接：先扫描已保存的 WiFi 列表，自动连匹配的（不用固定 SSID） ──
    auto *profile = settings.activeProfile();
    {
        Serial.println("[WiFi] 扫描已保存的 WiFi...");
        int nNet = wifi.scan();   // 同步扫描（最多6秒）

        // 匹配顺序：active_profile → 记忆库(profiles[1..4]) → 默认
        const char* matchSsid = nullptr;
        const char* matchPass = nullptr;
        char tmpPass[64] = "";

        if (nNet > 0) {
            auto &nets = wifi.scanResults();
            // 1. active_profile
            if (profile->wifi_ssid[0]) {
                for (auto &net : nets) {
                    if (net.ssid == profile->wifi_ssid) { matchSsid = profile->wifi_ssid; matchPass = profile->wifi_password; break; }
                }
            }
            // 2. 记忆库 profiles[1..4]
            if (!matchSsid) {
                for (int i = 1; i < PROFILE_MAX && !matchSsid; i++) {
                    const char* ssid = settings.wifiCredentialSsid(i - 1);
                    if (!ssid) continue;
                    for (auto &net : nets) {
                        if (net.ssid == ssid && settings.findWifiCredential(ssid, tmpPass, sizeof(tmpPass))) {
                            matchSsid = ssid; matchPass = tmpPass; break;
                        }
                    }
                }
            }
        }

        if (matchSsid && matchPass) {
            wifi.setCredentials(matchSsid, matchPass);
            Serial.printf("[WiFi] 匹配已保存: %s\n", matchSsid);
        } else if (profile->wifi_ssid[0]) {
            // 无匹配 → 用 active_profile 直接连（可能网络暂时扫不到）
            wifi.setCredentials(profile->wifi_ssid, profile->wifi_password);
            Serial.printf("[WiFi] 未匹配，用当前配置: %s\n", profile->wifi_ssid);
        }
        wifi.begin();
        WiFi.setSleep(false);   // 默认实时模式（语音/WS 低延迟）；息屏时才开启省电
    }

    // ── FMO 通联客户端 ──
    fmoClient.setHost(profile->fmo_host);
    fmoClient.begin(&audio, &bt, &vibrate, &cache, &settings);
    fmoClient.audioEnable();

    // ── ESP-NOW 低延迟语音（与 WiFi 共存，点对点发到接收端） ──
    micEspnow.setAudioFull(&audioFull);   // RX 通道采集麦克风
    micEspnow.begin();   // 内部 esp_now_init（STA 模式）

    // ── 电池检测（已在 UI 创建前初始化，此处仅再次同步，确保最新值） ──
    if (ui) ui->_batPercent = battery.percent();
    if (ui) ui->_batCharging = battery.charging();

    // ── 触摸屏（CST820B，I2C） ──
    // ⚠ 必须在所有硬件初始化之后注册，避免I2C冲突
    ui->initTouch();
    Serial.println("[触摸] 已注册");

    delay(500);
    Serial.println("=== 就绪 ===\n");
}

// ══════════════════════════════════════════════
// loop() — 主循环（每5ms执行一次）
// ══════════════════════════════════════════════
void loop()
{
    uint32_t now = millis();

    // ── 各模块轮询 ──
    wifi.loop();
    micWifi.loop();        // 监听接收端广播（自动发现 IP）
    fmoClient.loop();
    audio.update();
    battery.loop();

    // ── 台站列表页：数据异步到达后自动重建菜单显示 ──
    static int lastPinCnt = -1;
    if (ui->onStationList()) {
        int cnt = fmoClient.pinnedListCount();
        if (cnt != lastPinCnt) { lastPinCnt = cnt; ui->refreshMenu(); }
    } else {
        lastPinCnt = fmoClient.pinnedListCount();
    }

    // ── WiFi 扫描页：扫描完成后自动重建菜单显示结果 ──
    // 检测：扫描标志仍为true但底层状态已是 DONE/ERROR → 刷新一次并消费标志
    if (ui->onWifiScan()) {
        if (ui->wifiScanPending()) {
            Serial.println("[UI] WiFi scan done, refresh menu");
            ui->refreshMenu();
        }
    }

    // ── 快速充电状态检测（每1秒，闪电图标及时响应） ──
    static uint32_t lastChgCheck = 0;
    if (now - lastChgCheck > 1000) {
        lastChgCheck = now;
        battery.updateCharging();
        ui->_batCharging = battery.charging();
    }

    // ── 蓝牙状态（每2秒更新） ──
    if (now - lastBtUpdate > 2000)
    {
        lastBtUpdate = now;
        fmoClient.updateBtState();
    }

    // ── RSSI/电池/WiFi状态（定时更新） ──
    if (now - lastRssiRead > RSSI_REFRESH_MS)
    {
        lastRssiRead = now;
        ui->_batPercent = battery.percent();
        ui->_batCharging = battery.charging();
        ui->_btConnected = bt.isConnected();

        // WiFi 状态由首页图标显示，不再写文字
    }

    // ── 音频缓存清理（每小时） ──
    if (now - lastCacheClean > 3600000UL)
    {
        lastCacheClean = now;
        cache.startCleanup();
    }

    // ═══════════════════════════════
    // 物理按键处理
    // ═══════════════════════════════
    static bool la = HIGH, lb = HIGH;                                                                                   // 上次键值
    // 消抖读取：连续两次采样（间隔200µs）一致才采用，滤除机械抖动毛刺（避免快速按动漏检/A键误判A+B）
    auto readKeyStable = [](int pin) -> bool {
        bool v1 = digitalRead(pin);
        delayMicroseconds(200);
        bool v2 = digitalRead(pin);
        if (v1 == v2) return v1;
        delayMicroseconds(200);
        return digitalRead(pin);
    };
    bool a = readKeyStable(PIN_KEY_A);                                                                                  // 当前A键
    bool b = readKeyStable(PIN_KEY_B);                                                                                  // 当前B键
    bool aP = (a == KEY_ACTIVE_LEVEL && la != KEY_ACTIVE_LEVEL);                                                        // A键按下
    bool bP = (b == KEY_ACTIVE_LEVEL && lb != KEY_ACTIVE_LEVEL);                                                        // B键按下
    bool both = (a == KEY_ACTIVE_LEVEL && b == KEY_ACTIVE_LEVEL && (la != KEY_ACTIVE_LEVEL || lb != KEY_ACTIVE_LEVEL)); // 双键同时

    // ── 息屏状态：A/B 任意键唤醒 ──
    static bool screenOff = false;
    if (screenOff)
    {
        if (aP || bP || both)
        {
            screenOff = false;
            ui->wakeScreen();
            display->setBrightness(130);   // 恢复亮屏亮度
            setCpuFrequencyMhz(240);       // 恢复 CPU 高频
            WiFi.setSleep(false);          // 关闭 WiFi 省电（恢复实时性）
            Serial.println("[屏幕] 唤醒");
        }
        la = a; lb = b;
        // 息屏时不处理其他按键逻辑（保持省电）
    }
    else
    {
        // ── A+B 短按/长按区分：按下不立即触发，松开<3s=菜单，按住≥3s=息屏 ──
        static uint32_t bothDownMs = 0;
        static bool bothPending = false;   // A+B 按下未判定
        static bool bothLongDone = false;  // 已触发长按（息屏）
        bool bothHeld = (a == KEY_ACTIVE_LEVEL && b == KEY_ACTIVE_LEVEL);

        if (both) { bothPending = true; bothLongDone = false; bothDownMs = now; }

        if (bothPending && !bothLongDone)
        {
            if (now - bothDownMs >= 3000)
            {
                // 长按 3 秒 → 息屏（不触发菜单）
                bothLongDone = true;
                bothPending = false;
                screenOff = true;
                ui->startScreenOff();
                display->setBrightness(45);   // 低亮度（摩斯码绿点可见，降低省电）
                setCpuFrequencyMhz(160);      // 息屏降频省电（音频/摩斯码动画仍流畅）
                WiFi.setSleep(true);          // 开启 WiFi 省电（modem sleep）
                Serial.println("[屏幕] 息屏");
            }
        }

        // ── 正常状态按键处理（排除 A+B 按住期间的单键误触） ──
        if (!bothHeld)
        {
            // 单键按下：清除残留的 A+B 待判定状态（防止单键被误判为 A+B）
            if (aP && !bP) bothPending = false;
            if (bP && !aP) bothPending = false;
            // A+B 松开：若未触发长按 → 短按菜单
            if (bothPending && !bothLongDone)
            {
                bothPending = false;
                ui->keyAB();   // 短按：打开/关闭菜单
                Serial.println("[按键] A+B 短按");
            }
            if (aP && !bP) { ui->keyA(); Serial.println("[按键] A 单按"); }   // A键按下沿且B无按下沿
            else if (bP && !aP) { ui->keyB(); Serial.println("[按键] B 单按"); } // B键按下沿且A无按下沿
        }
        else if (bothPending && bothLongDone)
        {
            bothPending = false;   // 长按已处理，忽略松开
        }
        la = a;
        lb = b;
    }

    // ── A键按住 → 蓝牙语音发送（仅首页；150ms去抖排除A+B组合） ──
    bool aHeld = (a == KEY_ACTIVE_LEVEL);
    bool bHeld = (b == KEY_ACTIVE_LEVEL);
    bool homeMode = ui && !ui->isScreenOff() && !ui->busy();   // 仅首页（息屏/菜单/输入页不触发）

    // A+B 刚松开抑制：300ms 内不触发 TX（避免关菜单后 A 残留误触发送）
    // ⚠ 只在“任一键从按下变松开”的沿记录，不能每循环刷新（否则 A 键按住时
    //   bothReleaseMs 持续更新，now-bothReleaseMs 永远 <300ms，A 键 TX 永不触发）
    static uint32_t bothReleaseMs = 0;
    static bool laHeld = false, lbHeld = false;
    if ((laHeld && !aHeld) || (lbHeld && !bHeld)) bothReleaseMs = now;  // 松开沿
    laHeld = aHeld; lbHeld = bHeld;

    // A键需单独按住150ms才生效；期间B键按下则取消（避免A+B误触发TX）
    static uint32_t aHoldStart = 0;
    static bool aHoldArmed = false;
    if (homeMode && aHeld && !bHeld && (now - bothReleaseMs > 300)) {
        if (!aHoldArmed) { aHoldArmed = true; aHoldStart = now; }
    } else {
        aHoldArmed = false;
    }
    bool txReq = aHoldArmed && (now - aHoldStart >= 150) && !ui->isScreenOff();
    static bool prevTx = false;   // 上一次发送状态（只在变化时启停音频）

    if (txReq && (wifi.state() == WF_CONNECTED)) {
        // 进入发送状态（全双工：I2S TX+RX 并行，无需暂停播放）：
        //  - 静音 PA（扬声器无声，播放任务仍跑 TX 无影响）
        //  - 暂停 WS 音频接收（发送时不收）
        if (!prevTx) {
            audioFull.setPa(false);      // PA 静音
            fmoClient.audioDisable();    // 停止 WS 音频流（发送时不收）
            prevTx = true;
        }
        micEspnow.setTransmit(true);            // ESP-NOW 采集 RX 发送
        ui->_micTx = true;
    } else {
        if (txReq) Serial.println("[TX] A键按住+WiFi未连 → 显示提示");
        micEspnow.setTransmit(false);   // 停 ESP-NOW 发送
        ui->_micTx = false;
        if (prevTx) {
            audioFull.setPa(true);       // 恢复 PA（扬声器有声）
            fmoClient.audioEnable();     // 恢复 WS 音频（自动重连）
            prevTx = false;
        }
    }
    // WiFi 未连接且按A → 屏幕提示（沿用蓝牙提示文案，改为 WiFi 语境）
    ui->showBtWarn(txReq && (wifi.state() != WF_CONNECTED));

    // ═══════════════════════════════
    // UI + LVGL 渲染管线
    // 标准 flush 方式：LVGL 渲染 → flush 回调直接推屏（参考官方固件）
    // ═══════════════════════════════
    if (!ui->isScreenOff())
        ui->drawAll();      // 更新UI数据（时间/呼号/电池等）；息屏时跳过（省电）
    lv_timer_handler(); // LVGL渲染脏区域 → lv_flush 推屏（息屏动画仍需要）

    // ═══════════════════════════════
    // WiFi状态变化处理
    // ═══════════════════════════════
    static FMO_WifiState lastWifiState = WF_DISCONNECTED;
    FMO_WifiState curWifiState = wifi.state();

    if (curWifiState != lastWifiState)
    {
        lastWifiState = curWifiState;
        // 图标已显示状态，仅记录状态值
        if (curWifiState == WF_CONNECTED)
        {
            ui->_wifiStatus = 2;
            ui->_wifiStatTime = now;
            weather.forceUpdate();   // 联网后立即拉天气
            // 连接成功 → 保存 WiFi 记忆（自动重连用）
            if (wifi.ssid()[0] && wifi.hasCreds()) {
                Serial.printf("[WiFi] 保存记忆: %s\n", wifi.ssid());
                settings.saveWifiCredential(wifi.ssid(), wifi.connectedPass() ? wifi.connectedPass() : "");
            }
        }
        else if (curWifiState == WF_FAILED)
        {
            ui->_wifiStatus = wifi.hasCreds() ? 3 : 4;
            ui->_wifiStatTime = now;
        }
    }

    // ═══════════════════════════════
    // WiFi 断开自动重连记忆库
    // 当前 WiFi 连接失败 → 尝试从已保存的 WiFi 记忆中自动重连
    // ═══════════════════════════════
    if (curWifiState == WF_FAILED || curWifiState == WF_DISCONNECTED)
    {
        static uint32_t lastAutoReconn = 0;
        static int autoReconnIdx = 0;
        // 每 5 秒尝试一个记忆中的 WiFi
        if (millis() - lastAutoReconn > 5000)
        {
            lastAutoReconn = millis();
            int nSaved = settings.wifiCredentialCount();
            if (nSaved > 0)
            {
                const char* ssid = settings.wifiCredentialSsid(autoReconnIdx % nSaved);
                char pass[64] = "";
                if (ssid && settings.findWifiCredential(ssid, pass, sizeof(pass)))
                {
                    // 避免重复连接同一 SSID（当前凭证已是它且刚失败）
                    if (strcmp(ssid, wifi.ssid()) != 0 || !wifi.hasCreds())
                    {
                        Serial.printf("[WiFi] 自动重连记忆: %s\n", ssid);
                        wifi.setCredentials(ssid, pass);
                        wifi.reconnect();
                    }
                }
                autoReconnIdx++;
            }
        }
    }

    // ═══════════════════════════════
    // 通话状态 → 音频开关
    // ═══════════════════════════════
    static bool lastSpeaking = false;
    bool speaking = fmoClient.isSpeaking();

    if (speaking != lastSpeaking)
    {
        lastSpeaking = speaking;
        if (speaking && !settings.data()->audio_muted && !fmoClient.audioEnabled())
        {
            fmoClient.audioEnable();
        }
    }

    // FreeRTOS 任务切换（让出CPU给WiFi/音频等后台任务）
    vTaskDelay(pdMS_TO_TICKS(5));
}
