# -*- coding: utf-8 -*-
"""
生成思源黑体（Source Han Sans CN Bold）LVGL 8.4 字库
- 字号: 24px, bpp: 4
- 字符集: ASCII (0x20-0x7E) + GB2312 一级汉字（3755常用字，无中文标点/生僻字）
- cmap: FORMAT0_TINY(ASCII) + SPARSE_TINY(汉字)
输出: font_hansan_24.bin（二进制字库，供 LittleFS 存储 + 运行时加载到 PSRAM）
"""
import os, struct
from PIL import Image, ImageFont, ImageDraw

FONT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "font", "HanSan.otf")
SIZE = 24
BPP = 4
OUT_BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "font_hansan_24.bin")

font = ImageFont.truetype(FONT_PATH, SIZE)
ascent, descent = font.getmetrics()

# LVGL 字体指标约定（参考内置 montserrat）：
#   line_height = 字形所需最大行高（含上下留白）
#   base_line   = 从行底到基线的距离（LVGL 注释: Baseline measured from the bottom of the line）
LINE_HEIGHT = SIZE + 3        # 24+3 = 27（与内置字体一致）
BASE_LINE   = LINE_HEIGHT - ascent  # 27-22 = 5（行底往上 5px 是基线）
# 字形 ofs_y = 字形 box 相对"行顶"的偏移（LVGL: begin_y = pos.y + ofs_y）
#   基线(相对行顶) = line_height - base_line = 22
#   字形顶部(相对基线) = y0 - ascent → 字形顶部(相对行顶) = 22 + (y0-22) = y0
#   所以 ofs_y = y0（PIL bbox 的 y0，相对文本锚点/行顶）
print(f"字体指标: ascent={ascent} descent={descent} line_height={LINE_HEIGHT} base_line={BASE_LINE}")

# ---------- 字符集 ----------
def gb2312_l1_chars():
    """GB2312 一级汉字（区16-55，3755个最常用字），不包含中文标点和生僻字"""
    chars = set()
    for q in range(16, 56):
        for w in range(1, 95):
            gb = bytes([0xA0 + q, 0xA0 + w])
            try:
                ch = gb.decode('gb2312')
                if ch and not ch.isspace() and '\u4e00' <= ch <= '\u9fff':
                    chars.add(ch)
            except Exception:
                pass
    return chars

ascii_chars = set(chr(c) for c in range(0x20, 0x7F))
# 补充特殊字符（不在GB2312一级汉字/ASCII中但界面需要）：
#   • U+2022 中点（列表分隔符号）、圳 U+5733（深圳）、霾 U+973E（雾霾）
#   以下为城市名中的生僻字（GB2312二级）：亳衢泸漯濮滕盱眙醴邳邡禺莞坜坻猗
extra_chars = set("•圳霾亳衢泸漯濮滕盱眙醴邳邡禺莞坜坻猗")
all_chars = sorted(ascii_chars | gb2312_l1_chars() | extra_chars)
print(f"总字符数: {len(all_chars)}")

# ---------- 渲染 ----------
glyphs = {}
for ch in all_chars:
    cp = ord(ch)
    bbox = font.getbbox(ch)
    x0, y0, x1, y1 = (bbox if bbox else (0, 0, 0, 0))
    box_w, box_h = x1 - x0, y1 - y0
    adv_w = font.getlength(ch)
    if box_w <= 0 or box_h <= 0:
        # 空字形（如空格）：生成 1x1 占位位图，保证字形索引连续（重要！否则汉字错位）
        box_w, box_h = 1, 1
        img = Image.new("L", (1, 1), 0)
        ofs_x, ofs_y = 0, 0
    else:
        img = Image.new("L", (box_w, box_h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255)
        ofs_x, ofs_y = x0, y0
    # ofs_y = 字形 box 相对行顶的偏移（LVGL: begin_y = pos.y + ofs_y）
    glyphs[cp] = {"bitmap": img.tobytes(), "box_w": box_w, "box_h": box_h,
                  "adv_w": adv_w, "ofs_x": ofs_x, "ofs_y": ofs_y}
print(f"有效字形: {len(glyphs)}")

# ---------- cmap 分段 ----------
# ASCII (0x20-0x7E): FORMAT0_TINY
# 其余全部（汉字+符号）: 单个 SPARSE_TINY，range_start = 最小码点
cps = sorted(glyphs.keys())
ascii_cps = [c for c in cps if 0x20 <= c <= 0x7E]
rest_cps = [c for c in cps if not (0x20 <= c <= 0x7E)]
rest_start = rest_cps[0] if rest_cps else 0x4E00
rest_len = (rest_cps[-1] - rest_start + 1) if rest_cps else 1
rest_rcp = [c - rest_start for c in rest_cps]
rest_gid = len(ascii_cps)
ascii_len = 0x7E - 0x20 + 1

# ---------- 位图（4bpp） ----------
def pack4bpp(data):
    out = bytearray()
    for i in range(0, len(data), 2):
        b = 0
        for k in range(2):
            v = data[i+k] if i+k < len(data) else 0
            q = (v * 15 + 127) // 255
            b |= (q << (4 - k*4))
        out.append(b)
    return bytes(out)

bitmap_bytes = bytearray()
glyph_dsc = []  # (bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y)
for cp in cps:
    g = glyphs[cp]
    idx = len(bitmap_bytes)
    bitmap_bytes += pack4bpp(g["bitmap"])
    glyph_dsc.append((idx, g["adv_w"], g["box_w"], g["box_h"], g["ofs_x"], g["ofs_y"]))

# ---------- 二进制输出 ----------
# 文件格式:
#   头 44 字节: magic(4) "FMOF", version(4)=1, line_height(4), base_line(4), bpp(4),
#              glyph_count(4), bitmap_size(4), rest_list_len(4),
#              ascii_start(4), ascii_len(4), rest_start(4)
#   位图数据: bitmap_size 字节
#   glyph_dsc: glyph_count * 16 字节 (LV_FONT_FMT_TXT_LARGE=1 格式)
#     u32 bitmap_index, u32 adv_w(28.4), u16 box_w, u16 box_h, i16 ofs_x, i16 ofs_y
#   rest_unicode_list: rest_list_len * 2 字节 (u16 相对 rest_start 的码点)
hdr = struct.pack("<4sIIIIIIIIII", b"FMOF", 1, LINE_HEIGHT, BASE_LINE, BPP,
                  len(glyph_dsc), len(bitmap_bytes), len(rest_rcp), 0x20, ascii_len, rest_start)
with open(OUT_BIN, "wb") as f:
    f.write(hdr)
    f.write(bytes(bitmap_bytes))
    for (idx, adv, bw, bh, ox, oy) in glyph_dsc:
        # LVGL8 LARGE=1 glyph_dsc: u32 bitmap_index + u32 adv_w(28.4) + u16 box_w + u16 box_h + i16 ofs_x + i16 ofs_y = 16字节
        f.write(struct.pack("<IIHHhh", idx, int(adv * 16), bw, bh, ox, oy))
    for v in rest_rcp:
        f.write(struct.pack("<H", v))
print(f"输出: {OUT_BIN} ({os.path.getsize(OUT_BIN)/1024:.1f} KB)")
