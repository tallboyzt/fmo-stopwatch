// fwmo_font.h — 中文字库加载（思源黑体 24px）
// 字库文件 font_hansan_24.bin 存储在 LittleFS（见 partitions_custom.csv）
// 运行时读取到 PSRAM，构建 LVGL 字体对象
#pragma once
#include <lvgl.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <cstring>

// ── 二进制字库文件格式（由 font_gen.py 生成） ──
// 头 44 字节:
//   [0:4]   magic  "FMOF"
//   [4:8]   version (1)
//   [8:12]  line_height
//   [12:16] base_line
//   [16:20] bpp
//   [20:24] glyph_count
//   [24:28] bitmap_size
//   [28:32] rest_list_len
//   [32:36] ascii_start (0x20)
//   [36:40] ascii_len
//   [40:44] rest_start（汉字+符号段起始码点）
// 之后:
//   位图数据  bitmap_size 字节
//   glyph_dsc glyph_count * 16 字节 (LV_FONT_FMT_TXT_LARGE=1):
//     u32 bitmap_index, u32 adv_w(28.4), u16 box_w, u16 box_h, i16 ofs_x, i16 ofs_y
//   rest_unicode_list rest_list_len * 2 字节 (u16 相对 rest_start 的码点)

static lv_font_t *_g_font = nullptr;

inline lv_font_t *font_hansan_24_load()
{
    if (_g_font)
        return _g_font;

    File f = LittleFS.open("/font_hansan_24.bin", "r");
    if (!f)
    {
        Serial.println("[字体] 打开失败：/font_hansan_24.bin 不存在");
        Serial.println("[字体] 请先烧录 LittleFS 镜像（分区 0x610000）");
        return nullptr;
    }

    // 读头部
    uint8_t hdr[44];
    if (f.read(hdr, 44) != 44 || memcmp(hdr, "FMOF", 4) != 0)
    {
        Serial.println("[字体] 文件格式错误");
        f.close();
        return nullptr;
    }
    uint32_t line_h, base_l, bpp, glyph_cnt, bitmap_sz, cjk_len;
    uint32_t ascii_start, ascii_len, rest_start;
    memcpy(&line_h, hdr + 8, 4);
    memcpy(&base_l, hdr + 12, 4);
    memcpy(&bpp, hdr + 16, 4);
    memcpy(&glyph_cnt, hdr + 20, 4);
    memcpy(&bitmap_sz, hdr + 24, 4);
    memcpy(&cjk_len, hdr + 28, 4);
    memcpy(&ascii_start, hdr + 32, 4);
    memcpy(&ascii_len, hdr + 36, 4);
    memcpy(&rest_start, hdr + 40, 4);

    Serial.printf("[字体] 头: bpp=%lu glyph=%lu bitmap=%luKB rest=%lu\n",
                  (unsigned long)bpp, (unsigned long)glyph_cnt,
                  (unsigned long)(bitmap_sz / 1024), (unsigned long)cjk_len);

    // 在 PSRAM 分配
    uint8_t *pb = (uint8_t *)heap_caps_malloc(bitmap_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // LARGE=1 时 lv_font_fmt_txt_glyph_dsc_t 为 16 字节（u32+u32+u16+u16+i16+i16）
    size_t dsc_size = glyph_cnt * sizeof(lv_font_fmt_txt_glyph_dsc_t);
    auto *pd = (lv_font_fmt_txt_glyph_dsc_t *)heap_caps_malloc(dsc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *pu = (uint16_t *)heap_caps_malloc(cjk_len * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *pc = (lv_font_fmt_txt_cmap_t *)heap_caps_malloc(2 * sizeof(lv_font_fmt_txt_cmap_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *pfd = (lv_font_fmt_txt_dsc_t *)heap_caps_malloc(sizeof(lv_font_fmt_txt_dsc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *pf = (lv_font_t *)heap_caps_malloc(sizeof(lv_font_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if LVGL_VERSION_MAJOR == 8
    auto *pfc = (lv_font_fmt_txt_glyph_cache_t *)heap_caps_malloc(sizeof(lv_font_fmt_txt_glyph_cache_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!pb || !pd || !pu || !pc || !pfd || !pf
#if LVGL_VERSION_MAJOR == 8
        || !pfc
#endif
    )
    {
        Serial.println("[字体] PSRAM 分配失败");
        if (pb) free(pb); if (pd) free(pd); if (pu) free(pu);
        if (pc) free(pc); if (pfd) free(pfd); if (pf) free(pf);
#if LVGL_VERSION_MAJOR == 8
        if (pfc) free(pfc);
#endif
        f.close();
        return nullptr;
    }

    // 读位图
    if (f.read(pb, bitmap_sz) != (int)bitmap_sz)
    {
        Serial.println("[字体] 位图读取失败");
        f.close();
        return nullptr;
    }
    // 读 glyph_dsc（16 字节/个，直接映射结构）
    f.read((uint8_t *)pd, dsc_size);
    // 读 cjk_unicode_list
    f.read((uint8_t *)pu, cjk_len * 2);
    f.close();

    // 构建 cmaps
    memset(pc, 0, 2 * sizeof(lv_font_fmt_txt_cmap_t));
    // ASCII: FORMAT0_TINY
    pc[0].range_start = ascii_start;
    pc[0].range_length = ascii_len;
    pc[0].glyph_id_start = 0;
    pc[0].unicode_list = NULL;
    pc[0].glyph_id_ofs_list = NULL;
    pc[0].list_length = 0;
    pc[0].type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY;
    // 汉字+符号: SPARSE_TINY（range 覆盖 rest_start 到最大可能码点 0x9FFF）
    pc[1].range_start = rest_start;
    pc[1].range_length = 0x9FFF - rest_start + 1;
    pc[1].glyph_id_start = ascii_len;
    pc[1].unicode_list = pu;
    pc[1].glyph_id_ofs_list = NULL;
    pc[1].list_length = cjk_len;
    pc[1].type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY;

    // font_dsc
    memset(pfd, 0, sizeof(lv_font_fmt_txt_dsc_t));
    pfd->glyph_bitmap = pb;
    pfd->glyph_dsc = pd;
    pfd->cmaps = pc;
    pfd->kern_dsc = NULL;
    pfd->kern_scale = 0;
    pfd->cmap_num = 2;
    pfd->bpp = bpp;
    pfd->kern_classes = 0;
    pfd->bitmap_format = 0;
#if LVGL_VERSION_MAJOR == 8
    pfd->cache = pfc;
#endif

    // lv_font_t
    memset(pf, 0, sizeof(lv_font_t));
    pf->get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    pf->get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    pf->line_height = line_h;
    pf->base_line = base_l;
    pf->subpx = LV_FONT_SUBPX_NONE;
    pf->underline_position = -2;
    pf->underline_thickness = 1;
    pf->dsc = pfd;
    pf->fallback = NULL;
    pf->user_data = NULL;

    _g_font = pf;
    Serial.printf("[字体] 加载成功：%u 字形，位图 %.1f KB\n", glyph_cnt, bitmap_sz / 1024.0);

    // ── 自检：用 LVGL API 查找字形，验证字体对象有效 ──
    {
        lv_font_glyph_dsc_t gd;
        bool r1 = pf->get_glyph_dsc(pf, &gd, 'N', 'O');
        Serial.printf("[字体] 自检 'N': %s adv_w=%u box=%dx%d\n",
                      r1 ? "OK" : "FAIL",
                      (unsigned)gd.adv_w, (unsigned)gd.box_w, (unsigned)gd.box_h);
        bool r2 = pf->get_glyph_dsc(pf, &gd, 0x8BF7 /*请*/, ' ');
        Serial.printf("[字体] 自检 '请': %s box=%dx%d\n",
                      r2 ? "OK" : "FAIL", (unsigned)gd.box_w, (unsigned)gd.box_h);
    }
    return pf;
}
