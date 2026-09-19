/**
 * @file    app_ui.c
 * @brief   应用层界面实现：用 HZK16 中文字库在 LCD 上显示中文
 *
 * 本文件是“app 层使用中文字库”的示例：
 *   1. 调 my_drivers 的 hzk16_init() 初始化字库（内部自己去读 fontbin 分区）；
 *   2. 调 hzk16_get_lv_font() 拿到 LVGL 字体对象；
 *   3. 用 lv_obj_set_style_text_font() 把字体套到 label 上；
 *   4. 指定西文回退字体，让中英混排也能正常显示。
 *
 * app 层完全不接触 esp_partition、不接触 bsp 外设 API。
 */

#include <string.h>

#include "app_ui.h"

#include "my_drivers/font_hzk16/hzk16.h"   /* my_drivers: HZK16 中文字库 */
#include "my_drivers/lcd.h"                /* my_drivers: 液晶屏 / LVGL 接口 */

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

/* ============================ 内部状态（全部 static） ============================ */

static lv_obj_t *s_label_cn     = NULL;   /*!< 中文示例文本 */
static lv_obj_t *s_label_status = NULL;   /*!< 状态文字（WiFi 等） */

/* ============================ 界面搭建 ============================ */

/**
 * @brief  创建一个使用中文字体的 label
 * @param  parent 父对象
 * @param  text   UTF-8 文本
 * @param  font   字体（HZK16）
 * @param  color  文字颜色
 * @param  y      相对屏幕顶部的 y 偏移
 * @return label 对象
 */
static lv_obj_t *app_ui_create_cn_label(lv_obj_t *parent, const char *text,
                                        const lv_font_t *font, uint32_t color, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);      /* ★ 套用中文字体 */
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, 4, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

esp_err_t app_ui_start(void)
{
    /* ---------- 1. 初始化中文字库（数据在 fontbin 分区里） ---------- */
    esp_err_t err = hzk16_init();
    if (err != ESP_OK) {
        /* 字库不可用时界面仍然要能起来，只是中文显示不出来 */
        ESP_LOGE(TAG, "中文字库初始化失败: %s（检查 partitions.csv 与 hzk16.bin 是否已烧写）",
                 esp_err_to_name(err));
    }

    const lv_font_t *cn_font = hzk16_get_lv_font();
    /* 中英混排：ASCII 由西文字体负责，未收录的字符也走这里 */
    hzk16_set_fallback_font(&lv_font_montserrat_14);

    /* ---------- 2. 拿到显示设备（由 my_drivers 的屏幕驱动提供） ---------- */
    lv_display_t *disp = lcd_get_display();
    if (disp == NULL) {
        ESP_LOGE(TAG, "显示设备未就绪，请先完成液晶屏/LVGL 初始化");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---------- 3. 加锁后操作 LVGL ---------- */
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "获取 LVGL 锁超时");
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x001428), LV_PART_MAIN);

    /* 标题：纯中文 */
    app_ui_create_cn_label(scr, "中文字库测试", cn_font, 0x00E5FF, 8);

    /* 正文：中英数字混排，验证回退字体 */
    s_label_cn = app_ui_create_cn_label(scr,
                                        "ESP32-S3 HZK16 点阵字库\n"
                                        "从 flash 分区按需读取\n"
                                        "每字 32 字节 16x16\n"
                                        "一二三四五六七八九十",
                                        cn_font, 0xFFFFFF, 40);

    /* 状态栏 */
    s_label_status = app_ui_create_cn_label(scr, "状态: 正在启动 ...", cn_font, 0xFFD54F, 170);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "界面创建完成（中文字库%s）", hzk16_is_ready() ? "就绪" : "不可用");
    return ESP_OK;
}

void app_ui_set_status(const char *text)
{
    if (text == NULL || s_label_status == NULL) {
        return;
    }
    if (lvgl_port_lock(100)) {
        lv_label_set_text(s_label_status, text);
        lvgl_port_unlock();
    }
}
