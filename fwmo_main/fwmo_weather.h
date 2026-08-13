/**
 * fwmo_weather.h — 本地天气获取（wttr.in，免API Key，按IP自动定位）
 *
 * 数据源：http://wttr.in/?format=j1 （JSON，免费无注册）
 * 返回：current_condition[0].temp_C（温度°C）+ weatherCode（天气代码）
 *
 * 架构：FreeRTOS 独立任务获取，不阻塞 LVGL 渲染
 *   任务每30分钟拉取一次，只写共享状态（volatile），UI轮询读取
 *
 * 天气代码 → 图标分类（wttr.in 使用 weather.com 代码）：
 *   113晴 / 116多云间晴 / 119-122阴 / 143,248,260雾
 *   176-359雨系 / 179,227-371雪系 / 182-377冻雨冰粒 / 200,386-395雷
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

    /** 天气码 → 中文说明（wttr.in weather.com 代码） */
    static const char* descOf(int code) {
        switch (code) {
            case 113: return "晴";
            case 116: return "多云";
            case 119: case 122: return "阴";
            case 143: return "薄雾";
            case 176: case 293: case 296: case 299: return "小雨";
            case 179: return "小雪";
            case 182: case 185: case 281: case 284: case 311: case 314: return "冻雨";
            case 200: case 386: case 389: return "雷阵雨";
            case 227: case 230: return "暴雪";
            case 248: return "雾";
            case 260: return "冻雾";
            case 263: case 266: return "毛毛雨";
            case 302: case 305: case 306: return "中雨";
            case 308: return "大雨";
            case 317: case 320: return "雨夹雪";
            case 323: case 326: case 329: return "小雪";
            case 332: return "中雪";
            case 335: case 338: return "大雪";
            case 350: case 374: case 377: return "冰粒";
            case 353: case 356: return "阵雨";
            case 359: return "暴雨";
            case 362: case 365: return "阵雨夹雪";
            case 368: case 371: return "阵雪";
            case 392: case 395: return "雷雪";
            default: return "未知";
        }
    }

    /** 天气代码 → 图标分类（静态，供UI复用） */
    static Cat categorize(int code) {
        switch (code) {
            case 113: return W_SUN;
            case 116: return W_PARTLY;
            case 119: case 122: return W_CLOUD;
            case 143: case 248: case 260: return W_FOG;
            // 雷（含雷雨雷雪）
            case 200: case 386: case 389: case 392: case 395: return W_THUNDER;
            // 雪（纯雪、阵雪）
            case 179: case 227: case 230: case 323: case 326: case 329:
            case 332: case 335: case 338: case 368: case 371: return W_SNOW;
            // 冻雨/冰粒/冻雨夹雪
            case 182: case 185: case 281: case 284: case 311: case 314:
            case 317: case 320: case 350: case 362: case 365:
            case 374: case 377: return W_SLEET;
            // 雨（毛毛细雨、中雨、大雨、阵雨）
            case 176: case 263: case 266: case 293: case 296: case 299:
            case 302: case 305: case 308: case 353: case 356: case 359: return W_RAIN;
            default: return W_UNKNOWN;
        }
    }

private:
    static const uint32_t REFRESH_MS = 30UL * 60 * 1000;  // 30分钟刷新

    volatile bool _valid = false;
    volatile int  _temp_c = 0;
    volatile bool _located = false;  // IP定位是否成功
    char _city[16] = {0};        // 显示用："省份 城市"（中文）
    char _cityQ[16] = {0};       // 查询用：仅城市名
    char _desc[16] = {0};        // 中文天气说明（wttr.in weatherDesc, lang=zh）
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

    /** 步骤1: IP定位。多源尝试：固定城市 > ipchaxun(中文) > ipwho.is(英文) */
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
        // 源1: uapis.cn — 中文定位 "中国 河南 安阳"
        if (tryUapisCn()) return;
        // 源2: ipchaxun.com — 中文结构化数据
        if (tryIpchaxun()) return;
        // 源3: ipwho.is — 英文，翻译为中文
        if (tryIpwhoIs()) return;
        Serial.println("[天气] 所有定位源失败");
    }

    /** uapis.cn：region="中国 河南 安阳"（中文，取"省 市"） */
    bool tryUapisCn() {
        HTTPClient http;
        http.setTimeout(6000);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        if (!http.begin("https://uapis.cn/api/v1/network/myip")) return false;
        int code = http.GET();
        if (code != 200) { http.end(); return false; }
        String body = http.getString();
        http.end();
        JsonDocument doc;
        if (deserializeJson(doc, body)) return false;
        const char* region = doc["region"].as<const char*>();
        if (!region || !region[0]) return false;
        // "中国 河南 安阳" → 跳过国家"中国"，取 省=河南 市=安阳
        String r = region;
        int p1 = r.indexOf(' ');
        int p2 = p1 >= 0 ? r.indexOf(' ', p1 + 1) : -1;
        int p3 = p2 >= 0 ? r.indexOf(' ', p2 + 1) : -1;
        String prov = (p2 >= 0) ? r.substring(p1 + 1, p2) : (p1 >= 0 ? r.substring(p1 + 1) : "");
        String city = (p3 >= 0) ? r.substring(p2 + 1, p3) : (p2 >= 0 ? r.substring(p2 + 1) : "");
        if (city.length() == 0) {  // "中国 北京" 直辖市
            city = prov; prov = "";
        }
        if (city.length() == 0) return false;
        snprintf(_cityQ, sizeof(_cityQ), "%s", city.c_str());
        if (prov.length() > 0)
            snprintf(_city, sizeof(_city), "%s %s", prov.c_str(), city.c_str());   // "河南 安阳"
        else
            snprintf(_city, sizeof(_city), "%s", city.c_str());
        _located = true;
        Serial.printf("[天气] 定位(uapis.cn): %s\n", _city);
        return true;
    }

    /** ipchaxun.com：返回 ["中国","湖南","长沙","岳麓","联通",...] */
    bool tryIpchaxun() {
        HTTPClient http;
        http.setTimeout(6000);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.addHeader("User-Agent", "Mozilla/5.0");
        http.addHeader("Referer", "https://www.ipchaxun.com/");
        if (!http.begin("https://2024.ipchaxun.com/")) return false;
        int code = http.GET();
        if (code != 200) { http.end(); return false; }
        String body = http.getString();
        http.end();
        JsonDocument doc;
        if (deserializeJson(doc, body)) return false;
        if (strcmp(doc["ret"].as<const char*>() ?: "", "ok") != 0) return false;
        JsonArray data = doc["data"].as<JsonArray>();
        if (data.isNull() || data.size() < 3) return false;
        const char* country = data[0].as<const char*>();
        const char* province = data[1].as<const char*>();
        const char* city = data[2].as<const char*>();
        if (!city || !city[0]) return false;
        snprintf(_cityQ, sizeof(_cityQ), "%s", city);
        snprintf(_city, sizeof(_city), "%s %s", province && province[0] ? province : (country ? country : ""), city);
        _located = true;
        Serial.printf("[天气] 定位(ipchaxun): %s\n", _city);
        return true;
    }

    /** ipwho.is：英文，翻译为中文 */
    bool tryIpwhoIs() {
        HTTPClient http;
        http.setTimeout(6000);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        if (!http.begin("https://ipwho.is/")) return false;
        int code = http.GET();
        if (code != 200) { http.end(); return false; }
        String body = http.getString();
        http.end();
        JsonDocument doc;
        if (deserializeJson(doc, body)) return false;
        if (!doc["success"].as<bool>()) return false;
        const char* city = doc["city"].as<const char*>();
        const char* region = doc["region"].as<const char*>();
        if (!city || !city[0]) return false;
        snprintf(_cityQ, sizeof(_cityQ), "%s", city);
        snprintf(_city, sizeof(_city), "%s %s", zhName(region ? region : ""), zhName(city));
        _located = true;
        Serial.printf("[天气] 定位(ipwho.is): %s\n", _city);
        return true;
    }

    /** 英文地名 → 中文（省份+常用城市，未知保持原文） */
    static const char* zhName(const char* en) {
        static const struct { const char* en; const char* zh; } tab[] = {
            {"Gansu","甘肃"},{"Henan","河南"},{"Henan Sheng","河南"},{"Beijing","北京"},
            {"Shanghai","上海"},{"Tianjin","天津"},{"Chongqing","重庆"},{"Hebei","河北"},
            {"Shanxi","山西"},{"Shaanxi","陕西"},{"Inner Mongolia","内蒙古"},{"Liaoning","辽宁"},
            {"Jilin","吉林"},{"Heilongjiang","黑龙江"},{"Jiangsu","江苏"},{"Zhejiang","浙江"},
            {"Anhui","安徽"},{"Fujian","福建"},{"Jiangxi","江西"},{"Shandong","山东"},
            {"Hubei","湖北"},{"Hunan","湖南"},{"Guangdong","广东"},{"Guangxi","广西"},
            {"Hainan","海南"},{"Sichuan","四川"},{"Guizhou","贵州"},{"Yunnan","云南"},
            {"Tibet","西藏"},{"Xizang","西藏"},{"Xinjiang","新疆"},{"Qinghai","青海"},
            {"Ningxia","宁夏"},{"Taiwan","台湾"},{"Hong Kong","香港"},{"Macau","澳门"},
            {"Lanzhou","兰州"},{"Zhengzhou","郑州"},{"Jiaozuo","焦作"},{"Yinchuan","银川"},
            {"Xining","西宁"},{"Urumqi","乌鲁木齐"},{"Hohhot","呼和浩特"},{"Chengdu","成都"},
            {"Xi'an","西安"},{"Wuhan","武汉"},{"Jinan","济南"},{"Shijiazhuang","石家庄"},
            {"Taiyuan","太原"},{"Shenyang","沈阳"},{"Changchun","长春"},{"Harbin","哈尔滨"},
            {"Nanjing","南京"},{"Hangzhou","杭州"},{"Hefei","合肥"},{"Fuzhou","福州"},
            {"Nanchang","南昌"},{"Changsha","长沙"},{"Guangzhou","广州"},{"Nanning","南宁"},
            {"Haikou","海口"},{"Guiyang","贵阳"},{"Kunming","昆明"},{"Lhasa","拉萨"},
        };
        if (!en || !en[0]) return "";
        for (auto& t : tab)
            if (!strcmp(en, t.en)) return t.zh;
        return en;
    }

    /** 步骤2: 按城市拉取天气（wttr.in，中文城市名URL编码） */
    void fetchWeather() {
        HTTPClient http;
        http.setTimeout(8000);       // 8秒超时
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // wttr.in 可能重定向
        String url = "http://wttr.in/" + urlencode(_cityQ) + "?format=j1&lang=zh";
        if (!http.begin(url)) {
            Serial.println("[天气] begin失败");
            return;
        }

        int httpCode = http.GET();
        if (httpCode == 200) {
            String body = http.getString();
            JsonDocument doc;
            if (!deserializeJson(doc, body)) {
                JsonArray cc = doc["current_condition"].as<JsonArray>();
                if (!cc.isNull() && cc.size() > 0) {
                    JsonObject c = cc[0];
                    int t = c["temp_C"].as<int>();
                    int cd = c["weatherCode"].as<int>();
                    if (cd != 0) {  // 有效天气码
                        _temp_c = t;
                        _code   = cd;
                        _cat    = categorize(cd);
                        _valid  = true;
                        // 中文天气说明（按天气码本地映射，wttr.in lang=zh无效）
                        strncpy(_desc, descOf(cd), sizeof(_desc)-1);
                        Serial.printf("[天气] %s %d°C code=%d cat=%d desc=%s\n", _city, t, cd, (int)_cat, _desc);
                    }
                }
            }
        } else {
            Serial.printf("[天气] HTTP失败: %d (WiFi=%d)\n", httpCode, WiFi.status());
        }
        http.end();
    }
};
