/**
 * @file    hzk16_lvgl.c
 * @brief   HZK16 字库的 LVGL 适配层：把 flash 里的 16x16 点阵包装成 lv_font_t
 *
 * LVGL 9 的自定义字体只需要实现两个回调：
 *   get_glyph_dsc()     : 告诉 LVGL 这个字符的字模尺寸/前进宽度/格式
 *   get_glyph_bitmap()  : 把字模数据填进 LVGL 给的 draw_buf（A8 格式）并返回它
 *
 * 说明：
 *   - LVGL 9 只会给“需要的那个字”取字模，所以这里每次只读 32 字节 flash；
 *   - LVGL 给的 draw_buf 是 A8（1 字节 1 像素，0=透明 255=不透明），
 *     而 HZK16 是 1bpp，所以这里做一次展开（256 字节，开销可忽略）；
 *   - ASCII 等非 GB2312 字符由 fallback 字体负责显示。
 */

#include "hzk16.h"
#include "hzk16_private.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "hzk16_lvgl";

/* ============================ 回调前置声明 ============================ */

static bool hzk16_lv_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc_out,
                                   uint32_t letter, uint32_t letter_next);
static const void *hzk16_lv_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc, lv_draw_buf_t *draw_buf);

/* ============================ LVGL 字体实例（static，外部只能拿到只读指针） ============================ */

static lv_font_t s_hzk16_font = {
    .get_glyph_dsc    = hzk16_lv_get_glyph_dsc,
    .get_glyph_bitmap = hzk16_lv_get_glyph_bitmap,
    .release_glyph    = NULL,   /* 没有做内部缓存，不需要释放动作 */
    .line_height      = HZK16_FONT_HEIGHT,
    .base_line        = 0,      /* 汉字点阵占满整个行高，基线贴底 */
    .cap_height       = HZK16_FONT_HEIGHT,
    .x_height         = 0,
    .subpx            = LV_FONT_SUBPX_NONE,
    .kerning          = LV_FONT_KERNING_NONE,
    .static_bitmap    = 0,      /* 不是静态位图字体，字模由回调现取 */
    .underline_position  = -2,
    .underline_thickness = 1,
    .fallback         = NULL,   /* 通过 hzk16_set_fallback_font() 设置 */
};

/* ============================ 回调实现 ============================ */

/**
 * @brief  告诉 LVGL 某个字符的字模信息
 * @note   letter 是 Unicode 码点；只有 GB2312 里有的字才返回 true
 */
static bool hzk16_lv_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc_out,
                                   uint32_t letter, uint32_t letter_next)
{
    (void)font;
    (void)letter_next;              /* 汉字点阵是等宽的，不做字距调整 */

    /* 字库没初始化成功时直接返回 false：LVGL 会走 fallback 字体或画占位框，
     * 这样比“什么都不画”更容易发现是字库没烧进去 */
    if (!hzk16_is_ready()) {
        return false;
    }

    uint16_t gb = hzk16_unicode_to_gb(letter);
    if (gb == 0) {
        return false;               /* 交给 fallback 字体（如果有） */
    }

    /* LVGL 会把这里存的 gid 原样传给 get_glyph_bitmap，用它记住 GB2312 编码，
     * 避免取字模时再查一次表 */
    dsc_out->gid.index = (uint32_t)gb;

    dsc_out->format     = LV_FONT_GLYPH_FORMAT_A8;   /* 我们的回调输出 A8 */
    dsc_out->box_w      = HZK16_GLYPH_WIDTH;
    dsc_out->box_h      = HZK16_GLYPH_HEIGHT;
    /* ★ LVGL 9 的 adv_w 单位是“像素”（不是 1/16 像素！）：
     *   LVGL 在 lv_draw_label 里直接 pos.x += adv_w，
     *   内置字体的 1/16 像素值由 lv_font_fmt_txt 内部 (adv_w + 8) >> 4 转换。
     *   这里如果写成 16*16=256，每个字会前进 256 像素，一行只能显示一个字。 */
    dsc_out->adv_w      = HZK16_GLYPH_WIDTH;
    dsc_out->ofs_x      = 0;
    dsc_out->ofs_y      = 0;
    dsc_out->stride     = 0;                         /* 由 draw_buf 的 stride 决定 */
    dsc_out->is_placeholder = 0;
    dsc_out->req_raw_bitmap = 0;                     /* 请求 LVGL 提供 A8 draw_buf */
    return true;
}

/**
 * @brief  把字模展开成 A8 位图填进 draw_buf（LVGL 9 的约定：返回这个 buffer）
 */
static const void *hzk16_lv_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc, lv_draw_buf_t *draw_buf)
{
    if (g_dsc == NULL || draw_buf == NULL || draw_buf->data == NULL) {
        return NULL;
    }

    /* 只读这一颗字的 32 字节点阵 */
    uint8_t   glyph[HZK16_GLYPH_BYTES];
    esp_err_t err = hzk16_read_glyph_by_gb((uint16_t)g_dsc->gid.index, glyph);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取字模失败: gb=0x%04X err=%s",
                 (unsigned)g_dsc->gid.index, esp_err_to_name(err));
        return NULL;
    }

    const uint16_t box_w  = (uint16_t)g_dsc->box_w;
    const uint16_t box_h  = (uint16_t)g_dsc->box_h;
    const uint32_t stride = draw_buf->header.stride;
    uint8_t       *out    = draw_buf->data;

    /* HZK16：每行 2 字节，第 1 字节是高 8 位像素（最左），1 = 亮点 */
    for (uint16_t y = 0; y < box_h; y++) {
        uint8_t hi = (y * 2U + 0U < HZK16_GLYPH_BYTES) ? glyph[y * 2U + 0U] : 0U;
        uint8_t lo = (y * 2U + 1U < HZK16_GLYPH_BYTES) ? glyph[y * 2U + 1U] : 0U;
        for (uint16_t x = 0; x < 8U; x++) {
            if (x < box_w) {
                out[y * stride + x] = (hi & (uint8_t)(0x80U >> x)) ? 0xFFU : 0x00U;
            }
        }
        for (uint16_t x = 0; x < 8U; x++) {
            if ((x + 8U) < box_w) {
                out[y * stride + x + 8U] = (lo & (uint8_t)(0x80U >> x)) ? 0xFFU : 0x00U;
            }
        }
    }

    /* 写进 draw_buf 的 CPU 缓存要刷一下（draw_buf 可能在 PSRAM） */
    lv_draw_buf_flush_cache(draw_buf, NULL);

    /* LVGL 9 约定：返回 draw_buf 本身，渲染器按 A8 掩码使用 */
    return draw_buf;
}

/* ============================ 对外接口 ============================ */

const lv_font_t *hzk16_get_lv_font(void)
{
    return &s_hzk16_font;
}

void hzk16_set_fallback_font(const lv_font_t *fallback)
{
    s_hzk16_font.fallback = fallback;
}
