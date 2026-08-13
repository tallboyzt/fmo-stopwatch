# -*- coding: utf-8 -*-
"""
gen_callloc.py — 生成离线呼号→城市表
输入: huhao.csv (CallSign,City) + city_map_list.csv (拼音城市,次数,中文)
输出: callloc.bin (二进制, 供 LittleFS 存储 + 运行时加载到 PSRAM)

二进制格式:
  头 12 字节:
    [0:4]  magic "CLOC"
    [4:8]  entry_count (u32)
    [8:12] city_count (u32)
  call 表: entry_count * 8 字节
    [0:4] callsign_hash (u32, FNV-1a)
    [4:8] city_index (u32)
  城市表: city_count 条, 每条 [len:u8][utf8城市名]

查询逻辑(在固件侧):
  VR2→香港, XX9→澳门, BV→台湾分区
  B+字母+分区号 → 哈希查 call 表 → 命中显示城市
  未命中 → 分区号主省表 → "中国XX"
"""
import csv, struct, sys, io, os
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # 项目根目录

def read_csv(path):
    raw = open(path, 'rb').read()
    try:
        return list(csv.reader(raw.decode('utf-8-sig').splitlines()))
    except:
        try:
            return list(csv.reader(raw.decode('gbk').splitlines()))
        except:
            return None

# 1. 读映射表（拼音城市 → 中文）
map_rows = read_csv(os.path.join(BASE, 'city_map_list.csv'))
city_zh = {}
for r in map_rows[1:]:
    if len(r) >= 3 and r[2].strip():
        # 拼音规范化（去空格/符号）
        key = ''.join(c for c in r[0].strip().upper() if c.isalnum())
        city_zh[key] = r[2].strip()

# 分区号 → 主省（用于兜底：未查到城市时显示"中国XX"）
ZONE_PROV = {
    '0': '新疆', '1': '北京', '2': '辽宁', '3': '河北', '4': '江苏',
    '5': '浙江', '6': '湖北', '7': '广东', '8': '四川', '9': '陕西',
}
# 城市 → 省份 精确对照（353 个地级市，含台湾）
from city_prov import CITY_PROV
# 直辖市（不拼省名，保留 直辖市+区）
MUNI = ('北京', '上海', '天津', '重庆')

def city_full(zh):
    """城市中文名 → 完整显示文本（省+城市 / 直辖市+区 / 台湾+城市）"""
    # 直辖市+区：保留原样（如 北京海淀）
    for m in MUNI:
        if zh.startswith(m) and len(zh) > len(m):
            return zh
    # 台湾城市
    if zh in CITY_PROV and CITY_PROV[zh] == '台湾':
        return '台湾' + zh
    # 普通省城市：省+城市
    if zh in CITY_PROV:
        return CITY_PROV[zh] + zh
    # 城市已含省份前缀（如"河南"）或未知：原样
    return zh

# 所有省份名（含直辖市），用于剥离错误前缀（如"陕西榆林"→"榆林"）
ALL_PROV = list(CITY_PROV.values()) + ['北京', '上海', '天津', '重庆']
def strip_province(zh):
    """去掉城市名开头的省份前缀，保留纯城市名"""
    for p in ALL_PROV:
        if zh.startswith(p) and len(zh) > len(p):
            return zh[len(p):]
    return zh

# 2. 读呼号表：生成条目（用精确城市→省份映射）
call_rows = read_csv(os.path.join(BASE, 'huhao.csv'))
entries = {}      # 呼号 -> 城市中文（最终含省前缀）
unmapped = 0

for r in call_rows[1:]:
    if len(r) < 2: continue
    cs = r[0].strip().upper()
    city_raw = r[1].strip().upper()
    if not cs or not city_raw: continue
    ck = ''.join(c for c in city_raw if c.isalnum())
    zh = city_zh.get(ck)
    if not zh:
        unmapped += 1
        continue
    # 台湾呼号（BV）强制 "台湾+城市"：去掉表里的错误省份前缀（如"陕西榆林"→"榆林"）
    if cs.startswith('BV'):
        disp = '台湾' + strip_province(zh)
    else:
        disp = city_full(zh)
    entries[cs] = disp

print(f'总呼号: {len(call_rows)-1}, 已映射: {len(entries)}, 未映射: {unmapped}')

# 3. 城市去重编号
city_list = []
city_index = {}
for zh in entries.values():
    if zh not in city_index:
        city_index[zh] = len(city_list)
        city_list.append(zh)

# 4. FNV-1a 哈希
def fnv1a(s):
    h = 0x811c9dc5
    for b in s.encode('utf-8'):
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h

# 5. 生成二进制
with open('callloc.bin', 'wb') as f:
    f.write(b'CLOC')
    f.write(struct.pack('<II', len(entries), len(city_list)))
    # call 表
    for cs, zh in entries.items():
        f.write(struct.pack('<II', fnv1a(cs), city_index[zh]))
    # 城市表
    for zh in city_list:
        b = zh.encode('utf-8')
        f.write(struct.pack('<B', len(b)))
        f.write(b)

print(f'城市种类: {len(city_list)}')
print(f'文件大小: {8 + len(entries)*8 + sum(1+len(c.encode("utf-8")) for c in city_list)} 字节')
print('=== 城市样例 ===')
for i, c in enumerate(city_list[:20]):
    print(f'  [{i}] {c}')
