/**
 * fwmo_client.h — FMO WebSocket 通联客户端
 *
 * 连接架构：
 *   ws://<fmo_host>/audio   → 音频流（8kHz PCM mono，WebSocket binary帧）
 *   ws://<fmo_host>/events  → 通联事件（JSON：呼号/中继台/QSO等）
 *   ws://<fmo_host>/ws      → 中继台列表（JSON）
 *
 * 线程模型：
 *   main loop()   → WebSocket I/O（非阻塞轮询）
 *   JSON解析任务  → FreeRTOS 独立任务（JSON_PARSE_TASK_STACK / 优先级4）
 *
 * 参考： WebSocket 客户端架构
 */
#pragma once
#include "fwmo_config.h"
#include "fwmo_audio.h"
#include "fwmo_bt.h"
#include "fwmo_vibrate.h"
#include "fwmo_cache.h"
#include "fwmo_settings.h"
#include <WiFi.h>
#include <time.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class FMO_UI;

struct FMO_StationInfo {
    int uid; char name[64]; char frequency[24]; bool valid;
    FMO_StationInfo() : uid(0), valid(false) { name[0]=0; frequency[0]=0; }
};

struct JsonParseMsg {
    enum Type { TYPE_EVENT, TYPE_STATION } type;
    int  len;
    char json[0];
};

class FMO_Client {
public:
    FMO_Client() : _ui(nullptr) {}
    void setUI(FMO_UI* ui) { _ui = ui; }

    void setHost(const char* host) {
        if (!host) return;
        snprintf(_audio_url,   128, "ws://%s/audio",  host);
        snprintf(_event_url,   128, "ws://%s/events", host);
        snprintf(_station_url, 128, "ws://%s/ws",     host);
        _host_changed = true;
    }

    void begin(FMO_Audio* a, FMO_Bluetooth* b, FMO_Vibrate* v,
               FMO_Cache* c, FMO_SettingsMgr* s) {
        _audio=a; _bt=b; _vibe=v; _cache=c; _settings=s;
        _state = STATE_DISCONNECTED;

        if (!_jsonQueue)
            _jsonQueue = xQueueCreate(JSON_PARSE_QUEUE_LEN, sizeof(JsonParseMsg*));
        if (!_jsonTaskHandle && _jsonQueue) {
            xTaskCreatePinnedToCore(jsonParseTask,"json_parse",
                JSON_PARSE_TASK_STACK,this,JSON_PARSE_TASK_PRIO,
                &_jsonTaskHandle,JSON_PARSE_TASK_CORE);
        }
        configTzTime("CST-8","ntp.aliyun.com","ntp.tencent.com","pool.ntp.org");
    }

    bool sntpReady() { time_t t; time(&t); return t>1700000000; }

    void loop() {
        if (WiFi.status() != WL_CONNECTED) return;

        if (_state == STATE_DISCONNECTED || _state == STATE_ERROR) {
            // 失败退避重连：3s→6s→12s→24s→30s 封顶，避免服务器不可达时每3秒阻塞主循环
            if (millis() - _last_connect > _reconnect_delay) {
                _last_connect = millis();
                connectAll();
                if (_state == STATE_CONNECTED) _reconnect_delay = 3000;
                else _reconnect_delay = (_reconnect_delay >= 30000) ? 30000 : _reconnect_delay * 2;
            }
            return;
        }
        if (_state != STATE_CONNECTED) return;

        handleEvents();
        handleStation();
        handleAudio();
        handleRequests();

        // 音频连接失败退避重试（3s→30s），避免反复同步连接阻塞主循环
        if (_reconnect_audio && millis() - _last_audio_retry > _audio_retry_delay) {
            _last_audio_retry = millis();
            connectAudio();
            _audio_retry_delay = _reconnect_audio ? ((_audio_retry_delay >= 30000) ? 30000 : _audio_retry_delay * 2) : 3000;
        }
    }

    enum State { STATE_DISCONNECTED, STATE_CONNECTING, STATE_CONNECTED, STATE_ERROR };
    State state()       const { return _state; }
    int  stationUID()   const { return _current_station.uid; }
    const char* stationName()  const { return _current_station.name; }
    const char* currentTalker() const { return _talker_callsign; }
    bool isSpeaking()   const { return _speaking; }

    void audioEnable()  {
        _audio_enabled=true;
        _reconnect_audio=true;
        _last_audio_retry = 0;
        _audio_retry_delay = 3000;   // 重置退避间隔，确保立即重连
        if (_audio) {
            _audio->setOutputMode(
                _bt && _bt->isConnected() ? FMO_Audio::MODE_AUTO : FMO_Audio::MODE_LOCAL);
        }
    }
    void audioDisable() { _audio_enabled=false; _audio_connected=false; }
    bool audioEnabled() const { return _audio_enabled; }

    void requestCurrentStation() { _req_current_station=true; _forceReconnectNow(); }
    void requestPinnedList(int s, int c) { _req_pinned_start=s; _req_pinned_count=c; _req_pinned_list=true; _forceReconnectNow(); }
    void setCurrentStation(int uid) {
        // 本地立即同步选中台站（主页即时显示），服务器确认前本地选择优先
        for (int i = 0; i < _pinned_list_count; i++) {
            if (_pinned_list[i].uid == uid) { _current_station = _pinned_list[i]; break; }
        }
        _set_station_uid=uid; _req_set_station=true; _pending_set_uid=uid;
        _forceReconnectNow();
    }
    void requestQSOFullScan() { _req_qso_scan=true; _qso_scan_page=0; }
    int  qsoCount() const { return _qso_count; }
    int  pinnedListCount() const { return _pinned_list_count; }
    const FMO_StationInfo* pinnedList() const { return _pinned_list; }

    void updateBtState() {
        if (_audio && _bt) _audio->setBtConnected(_bt->isConnected());
    }

private:
    FMO_UI* _ui; FMO_Audio* _audio=nullptr; FMO_Bluetooth* _bt=nullptr;
    FMO_Vibrate* _vibe=nullptr; FMO_Cache* _cache=nullptr; FMO_SettingsMgr* _settings=nullptr;

    // 用户主动网络操作：未连接时立即触发重连（跳过退避等待）
    void _forceReconnectNow() {
        if (_state != STATE_CONNECTED) { _last_connect = 0; _reconnect_delay = 3000; }
    }

    char _audio_url[128]="", _event_url[128]="", _station_url[128]="";
    bool _host_changed=false;
    State _state=STATE_DISCONNECTED;
    uint32_t _last_connect=0, _last_audio_retry=0;
    uint32_t _last_station_poll=0;   // 台站定时刷新时间戳（30s 同步一次）
    uint32_t _reconnect_delay=3000, _audio_retry_delay=3000;   // 重连退避间隔（失败翻倍，30s封顶）

    bool _audio_enabled=false, _audio_connected=false, _reconnect_audio=false;
    bool _event_connected=false, _station_connected=false;

    char _talker_callsign[16]="", _last_call[16]="";
    bool _speaking=false;

    FMO_StationInfo _current_station;
    int  _pending_set_uid = 0;   // 等待服务器确认的台站uid（确认前本地选择优先）
    FMO_StationInfo _pinned_list[64]; int _pinned_list_count=0;   // 全部台站列表（64个，可滚动）
    uint32_t _qso_count=0; int32_t _qso_latest_id=-1; int _qso_scan_page=0;

    bool _req_current_station=false, _req_pinned_list=false, _req_set_station=false, _req_qso_scan=false;
    int  _req_pinned_start=0, _req_pinned_count=8, _set_station_uid=0;

    QueueHandle_t _jsonQueue = nullptr;
    TaskHandle_t  _jsonTaskHandle = nullptr;

    WiFiClient _event_c, _station_c, _audio_c;

    void connectAll() {
        // WiFi未连接时不尝试FMO重连（connectWS 的同步 connect 会阻塞主循环数秒）
        if (WiFi.status() != WL_CONNECTED) { _state=STATE_DISCONNECTED; return; }
        bool need_event = !_event_connected || !_event_c.connected();
        bool need_station = !_station_connected || !_station_c.connected();
        if (!need_event && !need_station) { _state=STATE_CONNECTED; return; }

        _state=STATE_CONNECTING;
        if (need_event) {
            if (_event_c.connected()) _event_c.stop();
            if (!connectWS(_event_c, _event_url, 1500)) { _state=STATE_ERROR; return; }
            _event_connected=true;
        }
        if (need_station) {
            if (_station_c.connected()) _station_c.stop();
            if (!connectWS(_station_c, _station_url, 1500)) { _state=STATE_ERROR; return; }
            _station_connected=true;
        }
        _state=STATE_CONNECTED;
        if (need_station && _station_connected) requestCurrentStation();
    }

    void connectAudio() {
        if (_audio_c.connected()) _audio_c.stop();
        if (connectWS(_audio_c, _audio_url, 1500))
            _audio_connected=true, _reconnect_audio=false;
    }

    bool connectWS(WiFiClient& c, const char* url, int timeout_ms=2000) {
        if (!url||!url[0]) return false;
        String u(url+5);
        String host=u, path="/"; int port=80;
        int si=u.indexOf('/'); if(si>0){ host=u.substring(0,si); path=u.substring(si); }
        int ci=host.indexOf(':'); if(ci>0){ port=host.substring(ci+1).toInt(); host=host.substring(0,ci); }

        // 限制 TCP 连接阻塞时间：服务器不可达时默认 connect 会卡数秒，拖死主循环（按键无响应）
        if (!c.connect(host.c_str(), port, 2000)) return false;

        char key[25];
        for (int i=0; i<24; i++) key[i]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[esp_random()%64];
        key[24]=0;
        c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                 "Connection: Upgrade\r\nSec-WebSocket-Key: %s==\r\n"
                 "Sec-WebSocket-Version: 13\r\n\r\n", path.c_str(), host.c_str(), key);
        c.flush();

        uint32_t t=millis();
        while (!c.available() && millis()-t<(uint32_t)timeout_ms) { delay(5); yield(); }
        if (!c.available()) { c.stop(); return false; }

        String ln; bool ok=false;
        while (c.available()) {
            ln=c.readStringUntil('\n'); ln.trim();
            if (ln.isEmpty()) break;
            if (ln.startsWith("HTTP/") && ln.indexOf("101")>0) ok=true;
        }
        if (!ok) { c.stop(); return false; }
        return true;
    }

    bool wsSend(WiFiClient& c, const char* json) {
        if (!c.connected()) return false;
        int len=strlen(json);
        uint8_t h[10]; int hl=0; h[0]=0x81;
        if (len<126)      { h[1]=len; hl=2; }
        else if (len<65536){ h[1]=126; h[2]=len>>8; h[3]=len&0xFF; hl=4; }
        else              { h[1]=127; for(int i=0;i<8;i++) h[2+i]=(len>>(56-i*8))&0xFF; hl=10; }
        return c.write(h,hl)==hl && c.write((const uint8_t*)json,len)==len;
    }

    // 等待TCP数据就绪 (避免帧反同步)
    static bool _w8(WiFiClient& c, int need, int to_ms=100) {
        uint32_t t = millis();
        while (c.connected() && c.available() < need && millis()-t < (uint32_t)to_ms)
            { delay(1); yield(); }
        return c.available() >= need;
    }

    int wsRead(WiFiClient& c, uint8_t* buf, int max, bool=false) {
        if (!c.connected()) return -1;
        if (!_w8(c, 2)) return -1;

        uint8_t h[2];
        if (c.read(h,2)!=2) return -1;
        uint8_t op=h[0]&0x0F;

        if (op==0x8) return -2;
        if (op==0x9) {
            uint8_t po[6]={0x8A,0x80,
                (uint8_t)(esp_random()&0xFF),(uint8_t)(esp_random()&0xFF),
                (uint8_t)(esp_random()&0xFF),(uint8_t)(esp_random()&0xFF)};
            c.write(po,6);
            return -1;
        }

        uint32_t len=h[1]&0x7F;
        bool masked=(h[1]&0x80)!=0;

        if (len==126) {
            if (!_w8(c,2,500)) return -1;
            uint8_t e[2]; c.read(e,2);
            len=((uint32_t)e[0]<<8)|e[1];
        } else if (len==127) {
            if (!_w8(c,8,500)) return -1;
            uint8_t e[8]; c.read(e,8);
            len=0; for(int i=0;i<8;i++) len=(len<<8)|e[i];
        }

        uint8_t mk[4]={0};
        if (masked) {
            if (!_w8(c,4,500)) return -1;
            c.read(mk,4);
        }

        uint32_t orig_len=len;
        if (len>(uint32_t)max) len=max;
        if (!_w8(c,(int)len,1000)) return -1;

        int rd=c.read(buf,len);
        if (rd<=0) return -1;
        for(int i=0;i<rd;i++) buf[i]^=mk[i%4];

        if (orig_len>(uint32_t)rd) {
            uint32_t remain=orig_len-rd;
            uint8_t d[256];
            while (remain>0) {
                if (!_w8(c,1,200)) break;
                uint32_t n=remain>256?256:remain;
                int got=c.read(d,n);
                if (got<=0) break;
                remain-=got;
            }
        }
        return rd;
    }

    void sendStationMsg(const char* sub, const char* data) {
        char j[512]; snprintf(j,sizeof(j),"{\"type\":\"station\",\"subType\":\"%s\",\"data\":%s}",sub,data);
        wsSend(_station_c,j);
    }
    void sendEventMsg(const char* type, const char* data) {
        char j[512]; snprintf(j,sizeof(j),"{\"type\":\"%s\",\"data\":%s}",type,data);
        wsSend(_event_c,j);
    }

    void postJsonToParse(JsonParseMsg::Type t, const char* json, int len) {
        if (!_jsonQueue || !json || len <= 0) return;
        JsonParseMsg* msg = (JsonParseMsg*)malloc(sizeof(JsonParseMsg) + len + 1);
        if (!msg) return;
        msg->type=t; msg->len=len;
        memcpy(msg->json, json, len); msg->json[len]=0;
        if (xQueueSend(_jsonQueue, &msg, 0) != pdTRUE) free(msg);
    }

    static void jsonParseTask(void* arg) {
        FMO_Client* self = (FMO_Client*)arg;
        while (true) {
            JsonParseMsg* msg = nullptr;
            if (xQueueReceive(self->_jsonQueue, &msg, portMAX_DELAY) == pdTRUE && msg) {
                if (msg->type == JsonParseMsg::TYPE_EVENT)
                    self->parseEventJson(msg->json, msg->len);
                else if (msg->type == JsonParseMsg::TYPE_STATION)
                    self->parseStationJson(msg->json, msg->len);
                free(msg);
            }
        }
    }

    void handleEvents() {
        if (!_event_c.connected()) { _event_c.stop(); _event_connected=false; _state=STATE_ERROR; return; }
        if (!_event_c.available()) return;
        uint8_t buf[2048]; int len=wsRead(_event_c,buf,sizeof(buf));
        if (len<=0) { if(len==-2){_event_c.stop();_event_connected=false;_state=STATE_ERROR;} return; }
        buf[len]=0;
        postJsonToParse(JsonParseMsg::TYPE_EVENT, (const char*)buf, len);
    }

    void handleStation() {
        if (!_station_c.connected()) { _station_c.stop(); _station_connected=false; _state=STATE_ERROR; return; }
        if (!_station_c.available()) return;
        uint8_t buf[4096]; int len=wsRead(_station_c,buf,sizeof(buf));
        if (len<=0) { if(len==-2){_station_c.stop();_station_connected=false;_state=STATE_ERROR;} return; }
        buf[len]=0;
        postJsonToParse(JsonParseMsg::TYPE_STATION, (const char*)buf, len);
    }

    void handleAudio() {
        if (!_audio_connected || !_audio_c.connected()) {
            if (_audio_c.connected()) _audio_c.stop();
            _audio_connected=false; _reconnect_audio=true;
            return;
        }
        if (!_audio_c.available()) return;

        uint32_t deadline = millis() + 30;
        uint8_t buf[4096];
        int totalFrames = 0;

        while (_audio_c.available() && millis() < deadline && totalFrames < 8) {
            int len = wsRead(_audio_c, buf, sizeof(buf));
            if (len == -2) {
                _audio_c.stop(); _audio_connected=false; _reconnect_audio=true;
                break;
            }
            if (len <= 0) break;

            if (_audio_enabled && _audio && len >= 2)
                _audio->pushPCM((const int16_t*)buf, len/2);
            if (_audio_enabled && _bt && _bt->isConnected() && _audio && !_audio->localActive())
                _bt->writePCM(buf, len, 8000);
            totalFrames++;
        }
    }

    void handleRequests() {
        // 台站定时刷新：每30秒同步一次当前台站（服务器端变化能及时反映到主页面）
        if (_station_connected && millis() - _last_station_poll > 30000UL) {
            _last_station_poll = millis();
            sendStationMsg("getCurrent", "{}");
        }
        if (_req_current_station) { _req_current_station=false; sendStationMsg("getCurrent","{}"); }
        if (_req_pinned_list) { _req_pinned_list=false;
            char d[64]; snprintf(d,sizeof(d),"{\"start\":%d,\"count\":%d}",_req_pinned_start,_req_pinned_count);
            sendStationMsg("getPinnedList",d); }   // 收藏台站列表（服务器暂不支持 getListRange 全部列表）
        if (_req_set_station) { _req_set_station=false;
            char d[32]; snprintf(d,sizeof(d),"{\"uid\":%d}",_set_station_uid);
            sendStationMsg("setCurrent",d); _req_current_station=true; }
        if (_req_qso_scan) {
            char d[64]; snprintf(d,sizeof(d),"{\"page\":%d,\"pageSize\":50}",_qso_scan_page);
            sendEventMsg("qso/getList",d);
            if (_qso_scan_page==0) _qso_count=0;
        }
    }

    void parseEventJson(const char* json, int len) {
        JsonDocument doc; if (deserializeJson(doc, json, len)) return;
        const char* type=doc["type"];
        const char* sub=doc["subType"];
        if (!type) return;

        if (strcmp(type,"qso")==0 && sub && strcmp(sub,"callsign")==0) {
            const char* call=doc["data"]["callsign"];
            if (call) { strncpy(_talker_callsign, call, 15); _talker_callsign[15]=0; }
            _speaking=doc["data"]["isSpeaking"].as<bool>();
            if (_speaking && call && call[0]) {
                if (_vibe && _cache && _settings && _settings->data()->vibrate_enabled) {
                    auto a=_cache->classify(call, _settings->data()->owner_callsign);
                    switch (a) {
                    case FMO_Cache::ALERT_NEVER:     _vibe->trigger(FMO_Vibrate::VIBE_LONG_ONCE); break;
                    case FMO_Cache::ALERT_NOT_TODAY:  _vibe->trigger(FMO_Vibrate::VIBE_SHORT_TWICE); break;
                    case FMO_Cache::ALERT_RECENT_15M: _vibe->trigger(FMO_Vibrate::VIBE_SHORT_THREE); break;
                    default: _vibe->trigger(FMO_Vibrate::VIBE_SHORT_ONCE); break;
                    }
                }
                if (_cache) _cache->recordCall(call);
                strncpy(_last_call, call, 15); _last_call[15]=0;
            }
        }
        else if (strcmp(type,"qso")==0 && sub && strcmp(sub,"history")==0) {
            JsonArray arr=doc["data"].as<JsonArray>();
            if (!arr.isNull() && _cache) {
                for (JsonObject h: arr) {
                    const char* c=h["callsign"];
                    if (c && c[0]) _cache->recordCall(c);
                }
            }
        }
        else if (strcmp(type,"qso/callsign")==0) {
            const char* call=doc["data"]["callsign"];
            if (call) { strncpy(_talker_callsign, call, 15); _talker_callsign[15]=0; }
            _speaking=doc["data"]["isSpeaking"].as<bool>();
            if (_speaking && call && call[0]) {
                if (_cache) _cache->recordCall(call);
                strncpy(_last_call, call, 15); _last_call[15]=0;
            }
        }
        else if (strcmp(type,"qso/getListResponse")==0) {
            JsonObject d=doc["data"];
            int cnt=d["count"].as<int>(), page=d["page"].as<int>(), lid=d["latestLogId"].as<int>();
            if (_req_qso_scan) {
                _qso_count+=cnt; _qso_latest_id=lid;
                if (cnt>=50 && _qso_scan_page<50) { _qso_scan_page++; _req_qso_scan=true; }
                else { _req_qso_scan=false; if (_settings) _settings->setQsoState(_qso_count,_qso_latest_id,true); }
            }
        }
    }

    void parseStationJson(const char* json, int len) {
        JsonDocument doc; if (deserializeJson(doc, json, len)) return;
        const char* sub=doc["subType"]; if (!sub) return;
        JsonObject d=doc["data"];

        if (strstr(sub,"getCurrent") || strstr(sub,"setCurrent")) {
            if (d["uid"].is<int>()) {
                int uid = d["uid"].as<int>();
                // 用户刚本地选择了新台站且服务器尚未确认（返回旧uid）→ 保留本地选择，忽略旧值
                if (_pending_set_uid != 0 && uid != _pending_set_uid) return;
                _pending_set_uid = 0;   // 服务器已确认
                _current_station.uid=uid;
                const char* n=d["name"].as<const char*>(); if(!n) n=""; strncpy(_current_station.name,n,63);
                const char* f=d["frequency"].as<const char*>(); if(!f) f=d["rxFrequency"].as<const char*>(); if(!f) f="";
                strncpy(_current_station.frequency,f,23); _current_station.valid=true;
            }
        }
        else if (strstr(sub,"getPinnedListResponse") || strstr(sub,"getListResponse")) {
            // getPinnedListResponse = 收藏台站列表（当前服务器支持）；getListResponse = 全部（兼容保留）
            JsonArray items=d["items"].as<JsonArray>(); if(items.isNull()) items=d["list"].as<JsonArray>();
            if (!items.isNull()) {
                _pinned_list_count=0;
                for (JsonObject it: items) {
                    if (_pinned_list_count>=64) break;
                    auto& s=_pinned_list[_pinned_list_count];
                    s.uid=it["uid"].as<int>();
                    const char* nm=it["name"].as<const char*>(); if(!nm) nm=""; strncpy(s.name,nm,63);
                    s.valid=true; _pinned_list_count++;
                }
            }
        }
    }
};
