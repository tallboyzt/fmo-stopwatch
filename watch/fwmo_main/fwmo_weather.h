/**
 * fwmo_weather.h — 本地天气获取（ipip.net 定位 + Open-Meteo 天气，均免费无Key）
 *
 * 定位：https://myip.ipip.net/json （高精度 IP 库，精确到城市）
 * 天气：https://api.open-meteo.com （ECMWF 数据，按经纬度查 WMO 天气码）
 * 坐标：https://geocoding-api.open-meteo.com （城市名 → 经纬度，只查一次缓存）
 *
 * 架构：FreeRTOS 独立任务获取，不阻塞 LVGL 渲染
 *   任务每30分钟拉取一次，只写共享状态（volatile），UI轮询读取
 *
 * 天气代码（Open-Meteo WMO）→ 图标分类：
 *   0,1晴 / 2多云 / 3阴 / 45,48雾 / 51-67雨冻雨 / 71-86雪 / 95-99雷
 */
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 城市名原样显示（不进行字形规范化处理）

class FMO_Weather {
public:
    /** 天气图标分类 */
    enum Cat {
        W_SUN,      // 晴
        W_PARTLY,   // 多云间晴
        W_CLOUD,    // 阴天
        W_FOG,      // 雾
        W_RAIN,     // 雨
        W_SNOW,     // 雪
        W_THUNDER,  // 雷
        W_SLEET,    // 冻雨/冰粒
        W_UNKNOWN
    };

    /** 强制立即刷新（可WiFi连接成功后调用，加快首次出数据） */
    void forceUpdate() {
        if (_task && !_valid) _lastForce = true;
    }

    /** 启动后台天气任务（FreeRTOS） */
    void begin() {
        if (_task) return;
        xTaskCreatePinnedToCore(task, "weather", 8192, this, 2, &_task, 0);
    }

    // ── 访问器（UI 轮询读取） ──
    bool valid()    const { return _valid;    }  // 数据是否有效
    int  tempC()    const { return _temp_c;   }  // 当前温度（°C）
    int  code()     const { return _code;     }  // 原始天气代码
    Cat  category() const { return _cat;      }  // 图标分类
    const char* city() const { return _city;  }  // 城市名（中文，"河南 安阳"）
    const char* desc() const { return _desc;  }  // 中文天气说明（"晴"/"小雨"等）

    /** 天气码 → 中文说明（Open-Meteo WMO 代码） */
    static const char* descOf(int code) {
        switch (code) {
            case 0: case 1: return "晴";
            case 2: return "多云";
            case 3: return "阴";
            case 45: case 48: return "雾";
            case 51: case 53: case 55: return "毛毛雨";
            case 56: case 57: return "冻毛毛雨";
            case 61: return "小雨";
            case 63: return "中雨";
            case 65: return "大雨";
            case 66: case 67: return "冻雨";
            case 71: return "小雪";
            case 73: return "中雪";
            case 75: return "大雪";
            case 77: return "雪粒";
            case 80: case 81: return "阵雨";
            case 82: return "强阵雨";
            case 85: case 86: return "阵雪";
            case 95: return "雷暴";
            case 96: case 99: return "雷暴冰雹";
            default: return "未知";
        }
    }

    /** 天气代码 → 图标分类（静态，供UI复用） */
    static Cat categorize(int code) {
        switch (code) {
            case 0: case 1: return W_SUN;
            case 2: return W_PARTLY;
            case 3: return W_CLOUD;
            case 45: case 48: return W_FOG;
            // 雷（含雷暴冰雹）
            case 95: case 96: case 99: return W_THUNDER;
            // 雪
            case 71: case 73: case 75: case 77: case 85: case 86: return W_SNOW;
            // 冻雨/冰粒
            case 56: case 57: case 66: case 67: return W_SLEET;
            // 雨（毛毛雨、雨、阵雨）
            case 51: case 53: case 55: case 61: case 63: case 65:
            case 80: case 81: case 82: return W_RAIN;
            default: return W_UNKNOWN;
        }
    }

private:
    static const uint32_t REFRESH_MS = 30UL * 60 * 1000;  // 30分钟刷新

    volatile bool _valid = false;
    volatile int  _temp_c = 0;
    volatile bool _located = false;  // IP定位是否成功
    bool _geoDone = false;           // 城市坐标是否已解析（Open-Meteo geocoding）
    float _lat = 0, _lon = 0;        // 城市经纬度（天气查询用）
    char _city[16] = {0};        // 显示用："省份 城市"（中文）
    char _cityQ[16] = {0};       // 查询用：仅城市名
    char _desc[16] = {0};        // 中文天气说明（Open-Meteo WMO 代码映射）
    volatile int  _code = 0;
    volatile Cat  _cat = W_UNKNOWN;
    TaskHandle_t  _task = nullptr;
    volatile bool _lastForce = false;  // 强制刷新请求

    /** 任务入口：先IP定位→再按城市查天气；未完成30秒重试，成功后30分钟刷新 */
    static void task(void* arg) {
        FMO_Weather* self = (FMO_Weather*)arg;
        for (;;) {
            if (WiFi.status() == WL_CONNECTED) {
                if (!self->_located) {
                    self->fetchLocation();      // 步骤1: IP定位（中文城市）
                }
                if (self->_located) {
                    self->fetchWeather();       // 步骤2: 按城市查天气
                }
            }
            self->_lastForce = false;
            uint32_t wait = (self->_valid && !self->_lastForce) ? REFRESH_MS : 30000UL;
            vTaskDelay(pdMS_TO_TICKS(wait));
        }
    }

    /** URL编码（城市名含中文/空格） */
    static String urlencode(const char* s) {
        String out;
        for (const char* p = s; *p; p++) {
            unsigned char cc = (unsigned char)*p;
            if (isalnum(cc) || cc == '-' || cc == '_' || cc == '.' || cc == '~')
                out += (char)cc;
            else {
                char b[4];
                snprintf(b, 4, "%%%02X", cc);
                out += b;
            }
        }
        return out;
    }

    /** 步骤1: IP定位（ipip.net 高精度，精确到城市）。固定城市 > ipip.net */
    void fetchLocation() {
#ifdef WEATHER_CITY
        if (WEATHER_CITY[0]) {
            snprintf(_cityQ, sizeof(_cityQ), "%s", WEATHER_CITY);
            snprintf(_city, sizeof(_city), "%s", WEATHER_CITY);
            _located = true;
            Serial.printf("[天气] 固定城市: %s\n", _city);
            return;
        }
#endif
        // ipip.net — 返回 {"ret":"ok","data":{"ip":"x","location":["中国","河南","安阳","","联通"]}}
        HTTPClient http;
        http.setTimeout(6000);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.addHeader("User-Agent", "Mozilla/5.0");
        if (!http.begin("https://myip.ipip.net/json")) {
            Serial.println("[天气] ipip.net begin失败");
            return;
        }
        int code = http.GET();
        if (code != 200) { http.end(); Serial.printf("[天气] ipip.net HTTP失败: %d\n", code); return; }
        String body = http.getString();
        http.end();
        JsonDocument doc;
        if (deserializeJson(doc, body)) { Serial.println("[天气] ipip.net JSON解析失败"); return; }
        if (strcmp(doc["ret"].as<const char*>() ?: "", "ok") != 0) return;
        JsonArray loc = doc["data"]["location"].as<JsonArray>();
        if (loc.isNull() || loc.size() < 3) return;
        const char* province = loc[1].as<const char*>();   // "河南" 或直辖市名
        const char* city     = loc[2].as<const char*>();   // "安阳"
        const char* district = loc[3].as<const char*>();   // 区县（可能为空）
        if (!city || !city[0]) return;
        snprintf(_cityQ, sizeof(_cityQ), "%s", city);
        if (province && province[0]) {
            if (strcmp(province, city) == 0) {
                // 直辖市：省==市 → 用区县细分（如 北京 朝阳区），无区县则仅市名
                if (district && district[0])
                    snprintf(_city, sizeof(_city), "%s %s", city, district);
                else
                    snprintf(_city, sizeof(_city), "%s", city);
            } else {
                snprintf(_city, sizeof(_city), "%s %s", province, city);   // "河南 安阳"
            }
        } else {
            snprintf(_city, sizeof(_city), "%s", city);
        }
        _located = true;
        Serial.printf("[天气] 定位(ipip.net): %s\n", _city);
    }

    /** 步骤2: 按城市查天气（Open-Meteo：先 geocoding 查坐标，再按坐标查天气） */
    void fetchWeather() {
        // 2a: 城市坐标（只解析一次并缓存）
        if (!_geoDone) {
            HTTPClient http;
            http.setTimeout(8000);
            // 城市名+"市" 提高匹配准确率（"安阳"→"安阳市"，避免匹配广西安阳镇）
            String q = _cityQ;
            if (q.length() < 8) q += "市";
            String url = "https://geocoding-api.open-meteo.com/v1/search?name=" +
                         urlencode(q.c_str()) + "&count=1&language=zh&format=json";
            if (!http.begin(url)) return;
            int c = http.GET();
            if (c == 200) {
                String b = http.getString();
                JsonDocument d;
                if (!deserializeJson(d, b)) {
                    JsonArray res = d["results"].as<JsonArray>();
                    if (!res.isNull() && res.size() > 0) {
                        _lat = res[0]["latitude"].as<float>();
                        _lon = res[0]["longitude"].as<float>();
                        _geoDone = true;
                        Serial.printf("[天气] 坐标(%s): %.3f,%.3f\n", _cityQ, _lat, _lon);
                    }
                }
            }
            http.end();
            if (!_geoDone) { Serial.printf("[天气] geocoding失败(HTTP %d)\n", c); return; }  // 下轮重试
        }

        // 2b: 按坐标查当前天气（Open-Meteo，ECMWF 数据）
        HTTPClient http;
        http.setTimeout(8000);
        char url[160];
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f&current=temperature_2m,weather_code",
                 _lat, _lon);
        if (!http.begin(url)) return;
        int httpCode = http.GET();
        if (httpCode == 200) {
            String body = http.getString();
            JsonDocument doc;
            if (!deserializeJson(doc, body)) {
                JsonObject cur = doc["current"].as<JsonObject>();
                if (!cur.isNull()) {
                    int t  = cur["temperature_2m"].as<int>();
                    int cd = cur["weather_code"].as<int>();
                    _temp_c = t;
                    _code   = cd;
                    _cat    = categorize(cd);
                    _valid  = true;
                    strncpy(_desc, descOf(cd), sizeof(_desc) - 1);
                    Serial.printf("[天气] %s %d°C code=%d cat=%d desc=%s\n", _city, t, cd, (int)_cat, _desc);
                }
            }
        } else {
            Serial.printf("[天气] HTTP失败: %d (WiFi=%d)\n", httpCode, WiFi.status());
        }
        http.end();
    }
};
