/**
 * fwmo_callloc.h — 呼号→城市 离线查询（FMO 通联伴侣）
 *
 * 数据来源：callloc.bin（由 gen_callloc.py 从 huhao.csv 生成）
 * 存储：LittleFS /callloc.bin → 运行时加载到 PSRAM
 *
 * 查询逻辑：
 *   1. VR2 → "中国香港"
 *   2. XX9 → "中国澳门"
 *   3. BV  → 查表命中"中国台湾+城市"，未命中 "中国台湾"
 *   4. B + 等级字母 + 分区号 → 哈希查表：
 *       命中 → "中国" + ("省+城市" 或 "直辖市+区")（如"中国河南安阳"、"中国北京海淀"）
 *       未命中 → 分区号主省 → "中国XX"（如 B6 → "中国湖北"）
 *   5. 其他 → 空
 *
 * 二进制格式（callloc.bin）：
 *   头 12 字节: magic "CLOC" + entry_count(u32) + city_count(u32)
 *   call表: entry_count * 8 字节 [hash:u32][city_index:u32]
 *   城市表: city_count 条 [len:u8][utf8城市名]
 */
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

class FMO_CallLoc {
public:
    /**
     * 从 LittleFS 加载呼号表到 PSRAM。
     * @return true 加载成功
     */
    bool begin(const char* path = "/callloc.bin") {
        // 释放旧数据
        freeData();

        File f = LittleFS.open(path, "r");
        if (!f) {
            Serial.printf("[呼号表] 打开失败: %s\n", path);
            return false;
        }
        size_t fsize = f.size();
        if (fsize < 12) { f.close(); return false; }

        // 读整个文件到 PSRAM
        uint8_t* buf = (uint8_t*)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) { f.close(); Serial.println("[呼号表] PSRAM分配失败"); return false; }
        f.read(buf, fsize);
        f.close();

        // 解析头
        if (memcmp(buf, "CLOC", 4) != 0) {
            free(buf);
            Serial.println("[呼号表] magic错误");
            return false;
        }
        memcpy(&_entryCount, buf + 4, 4);
        memcpy(&_cityCount, buf + 8, 4);

        // call 表
        _entries = (Entry*)(buf + 12);
        // 城市表
        size_t cityOff = 12 + (size_t)_entryCount * 8;
        _cityBuf = buf + cityOff;
        _cityOffsets = (uint16_t*)heap_caps_malloc((_cityCount + 1) * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_cityOffsets) { free(buf); return false; }
        // 解析城市偏移
        size_t p = 0;
        for (uint32_t i = 0; i < _cityCount; i++) {
            _cityOffsets[i] = p;
            uint8_t ln = _cityBuf[p++];
            p += ln;
        }
        _cityOffsets[_cityCount] = p;
        _base = buf;  // 保留基址以便 free

        Serial.printf("[呼号表] 加载成功: %u 呼号, %u 城市, %u KB\n",
                      _entryCount, _cityCount, (uint32_t)(fsize / 1024));
        return true;
    }

    /**
     * 查询呼号对应的地区显示文本。
     * @param callsign 呼号（如 "BD6JNF"）
     * @param out      输出缓冲（UTF-8）
     * @param maxLen   缓冲大小
     * @return 是否输出
     */
    bool lookup(const char* callsign, char* out, int maxLen) {
        if (!callsign || !callsign[0] || !out || maxLen < 2 || !_base) return false;
        out[0] = 0;

        char cs[16];
        int i = 0;
        for (const char* p = callsign; *p && i < 15; p++) {
            if (*p >= 'a' && *p <= 'z') cs[i++] = *p - 32;  // 转大写
            else cs[i++] = *p;
        }
        cs[i] = 0;
        if (i < 3) return false;

        // ── 1. 港澳前缀 ──
        if (strncmp(cs, "VR2", 3) == 0) { snprintf(out, maxLen, "中国香港"); return true; }
        if (strncmp(cs, "XX9", 3) == 0) { snprintf(out, maxLen, "中国澳门"); return true; }

        // ── 2. 台湾：先查表（表内为"台湾+城市"→加中国前缀），未命中显示中国台湾 ──
        if (strncmp(cs, "BV", 2) == 0) {
            if (lookupHash(cs, out, maxLen)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "中国%s", out);
                snprintf(out, maxLen, "%s", buf);
                return true;
            }
            snprintf(out, maxLen, "中国台湾");
            return true;
        }

        // ── 3. 大陆：B + 等级字母 + 分区号 ──
        if (cs[0] == 'B' && isAlpha(cs[1]) && cs[2] >= '0' && cs[2] <= '9') {
            // 3a. 哈希查表（命中→加"中国"前缀）
            if (lookupHash(cs, out, maxLen)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "中国%s", out);
                snprintf(out, maxLen, "%s", buf);
                return true;
            }
            // 3b. 分区号主省兜底
            const char* prov = zoneProvince(cs[2]);
            if (prov) { snprintf(out, maxLen, "中国%s", prov); return true; }
        }
        return false;
    }

private:
    struct Entry { uint32_t hash; uint32_t city_index; };

    uint8_t* _base = nullptr;
    Entry* _entries = nullptr;
    uint8_t* _cityBuf = nullptr;
    uint16_t* _cityOffsets = nullptr;
    uint32_t _entryCount = 0;
    uint32_t _cityCount = 0;

    static bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

    // FNV-1a 32bit
    static uint32_t fnv1a(const char* s) {
        uint32_t h = 0x811c9dc5;
        while (*s) {
            h ^= (uint8_t)*s++;
            h *= 0x01000193;
        }
        return h;
    }

    bool lookupHash(const char* cs, char* out, int maxLen) {
        if (!_entries) return false;
        uint32_t h = fnv1a(cs);
        // 线性查找（表不大，13686条可接受；后续可改二分）
        for (uint32_t i = 0; i < _entryCount; i++) {
            if (_entries[i].hash == h) {
                uint32_t ci = _entries[i].city_index;
                if (ci < _cityCount) {
                    int start = _cityOffsets[ci];
                    int len = _cityOffsets[ci + 1] - start;
                    if (len > 0 && len < maxLen) {
                        memcpy(out, _cityBuf + start, len);
                        out[len] = 0;
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    }

    static const char* zoneProvince(char zone) {
        switch (zone) {
            case '0': return "新疆";
            case '1': return "北京";
            case '2': return "辽宁";
            case '3': return "河北";
            case '4': return "江苏";
            case '5': return "浙江";
            case '6': return "湖北";
            case '7': return "广东";
            case '8': return "四川";
            case '9': return "陕西";
            default: return nullptr;
        }
    }

    void freeData() {
        if (_base) { heap_caps_free(_base); _base = nullptr; }
        if (_cityOffsets) { heap_caps_free(_cityOffsets); _cityOffsets = nullptr; }
        _entries = nullptr;
        _cityBuf = nullptr;
        _entryCount = 0;
        _cityCount = 0;
    }
};
