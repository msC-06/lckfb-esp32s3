/**
 * @file    launcher_ui.c
 * @brief   游戏选择主页实现
 *
 * 布局（320x240 横屏）：
 *   ┌──────────────────────────────┐
 *   │ 游戏中心                      │  ← 标题栏（HZK16 中文）
 *   ├──────────────────────────────┤
 *   │ [图标]  [图标]  [图标] ...     │  ← 游戏列表（flex 自动换行 + 可滚动）
 *   │ 名称    名称    名称           │
 *   ├──────────────────────────────┤
 *   │ 状态：TF卡已挂载 / WiFi ...    │  ← 状态栏（线程安全更新）
 *   └──────────────────────────────┘
 *
 * 内存：整个页面在切页时被 game_manager 一次性删除，这里不额外申请堆内存，
 *       只用一个静态状态文字缓冲（被删除时通过 LV_EVENT_DELETE 清掉标签句柄）。
 */

#include <string.h>

#include "launcher_ui.h"

#include "game_launcher/game_manager.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "launcher";

/* ============================ 内部状态（全部 static） ============================ */

static const lv_font_t *s_cn_font     = NULL;
static lv_obj_t        *s_status_label = NULL;

/** 状态文字缓存：页面重建后还能显示上一次的状态 */
static char            s_status_text[64] = "状态: 正在启动 ...";
static portMUX_TYPE    s_text_lock = portMUX_INITIALIZER_UNLOCKED;

/* ============================ 内部函数 ============================ */

/**
 * @brief  状态标签被删除时清掉句柄，避免线程安全的更新函数访问野指针
 */
static void status_deleted_cb(lv_event_t *e)
{
    (void)e;
    s_status_label = NULL;
}

/**
 * @brief  游戏按钮点击：进入对应游戏
 */
static void game_btn_clicked_cb(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    if (id == NULL) {
        return;
    }
    ESP_LOGI(TAG, "点击游戏: %s", id);
    esp_err_t err = game_manager_enter(id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "进入游戏 %s 失败: %s", id, esp_err_to_name(err));
    }
}

/**
 * @brief  创建一个游戏卡片（图标 + 中文名）
 */
static void create_game_card(lv_obj_t *parent, const game_t *game)
{
    /* 卡片外框用按钮，方便直接点 */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 96, 84);
    lv_obj_set_style_pad_all(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E2A38), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, game_btn_clicked_cb, LV_EVENT_CLICKED, (void *)game->id);

    /* 图标块（没有图片资源，用带底色的小方块 + 文字图标） */
    lv_obj_t *icon = lv_obj_create(btn);
    lv_obj_set_size(icon, 60, 40);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_radius(icon, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(game->icon_color), LV_PART_MAIN);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon_text = lv_label_create(icon);
    lv_label_set_text(icon_text, game->icon_text ? game->icon_text : "?");
    lv_obj_set_style_text_font(icon_text, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon_text, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(icon_text);

    /* 中文名称（HZK16 渲染） */
    lv_obj_t *name = lv_label_create(btn);
    lv_label_set_text(name, game->title ? game->title : game->id);
    if (s_cn_font) {
        lv_obj_set_style_text_font(name, s_cn_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(name, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -2);
}

/* ============================ 对外接口 ============================ */

lv_obj_t *launcher_ui_create(lv_obj_t *parent, const lv_font_t *cn_font)
{
    if (parent == NULL) {
        return NULL;
    }
    s_cn_font = cn_font;

    /* ---------- 页面 ---------- */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_center(page);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(page, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x0B1622), LV_PART_MAIN);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* ---------- 标题栏 ---------- */
    lv_obj_t *header = lv_obj_create(page);
    lv_obj_set_size(header, LV_PCT(100), 30);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x14324F), LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "游戏中心");
    if (s_cn_font) {
        lv_obj_set_style_text_font(title, s_cn_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);

    /* ---------- 游戏列表（flex 自动换行 + 纵向滚动） ---------- */
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_set_size(list, LV_PCT(100), 152);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_pad_all(list, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(list, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0B1622), LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    int count = game_manager_get_count();
    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "暂无游戏");
        if (s_cn_font) {
            lv_obj_set_style_text_font(empty, s_cn_font, LV_PART_MAIN);
        }
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
    } else {
        for (int i = 0; i < count; i++) {
            const game_t *game = game_manager_get(i);
            if (game) {
                create_game_card(list, game);
            }
        }
    }

    /* ---------- 底部状态栏 ---------- */
    lv_obj_t *status = lv_label_create(page);
    if (s_cn_font) {
        lv_obj_set_style_text_font(status, s_cn_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(status, lv_color_hex(0xFFD54F), LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* 复制一份当前状态文字（其它任务可能正在改） */
    char text_copy[sizeof(s_status_text)];
    portENTER_CRITICAL(&s_text_lock);
    strncpy(text_copy, s_status_text, sizeof(text_copy) - 1);
    text_copy[sizeof(text_copy) - 1] = '\0';
    portEXIT_CRITICAL(&s_text_lock);
    lv_label_set_text(status, text_copy);

    /* 页面重建时这个标签会被删掉，注册删除回调把句柄清空 */
    lv_obj_add_event_cb(status, status_deleted_cb, LV_EVENT_DELETE, NULL);
    s_status_label = status;

    ESP_LOGI(TAG, "主页创建完成，共 %d 个游戏", count);
    return page;
}

void launcher_ui_set_status(const char *text)
{
    if (text == NULL) {
        return;
    }

    /* 1. 先缓存文字（加锁，页面重建后也能显示） */
    portENTER_CRITICAL(&s_text_lock);
    strncpy(s_status_text, text, sizeof(s_status_text) - 1);
    s_status_text[sizeof(s_status_text) - 1] = '\0';
    portEXIT_CRITICAL(&s_text_lock);

    /* 2. 当前不在主页（标签已被删）就只缓存 */
    if (s_status_label == NULL) {
        return;
    }

    /* 3. 拿 LVGL 锁后刷新标签（拿到锁期间标签不会被删，可以安全访问） */
    if (lvgl_port_lock(100)) {
        if (s_status_label) {
            lv_label_set_text(s_status_label, s_status_text);
        }
        lvgl_port_unlock();
    }
}
