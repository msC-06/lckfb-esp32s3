/**
 * @file    app_chat_ui.c
 * @brief   聊天界面实现：320x240 横屏，顶部状态栏 + 气泡列表 + 底部状态行
 *
 * 布局：
 *   ┌───────────────────────────────┐ 0
 *   │ WiFi:已连接         ● 录音中   │ 顶部状态栏（26px）
 *   ├───────────────────────────────┤ 26
 *   │                  ┌──────────┐ │
 *   │                  │ 用户消息  │ │  消息区（192px，可上下滚动）
 *   │                  └──────────┘ │
 *   │ ┌──────────┐                  │
 *   │ │ AI 回复   │                  │
 *   │ └──────────┘                  │
 *   ├───────────────────────────────┤ 218
 *   │ 按住按键说话                   │ 底部状态行（22px）
 *   └───────────────────────────────┘ 240
 */

#include <stdio.h>
#include <string.h>

#include "app_chat_ui.h"
#include "app_config.h"
#include "app_gb2312.h"

#include "my_drivers/lcd.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "APP_UI";

/* ============================ 内部常量 ============================ */

/** 消息区高度 = 屏幕高 - 顶部栏 - 底部状态行 */
#define UI_MSG_AREA_H       (BSP_LCD_V_RES - APP_CHAT_TOPBAR_H - APP_CHAT_STATUSBAR_H)
/** 错误提示显示时长（毫秒） */
#define UI_ERROR_SHOW_MS    6000

/* ============================ 内部状态（全部 static） ============================ */

static const lv_font_t *s_font        = NULL;
static lv_obj_t        *s_scr         = NULL;   /*!< 本模块新建的 screen */
static lv_obj_t        *s_msg_area    = NULL;   /*!< 消息区（滚动容器） */
static lv_obj_t        *s_status_lbl  = NULL;   /*!< 底部状态行 */
static lv_obj_t        *s_wifi_lbl    = NULL;   /*!< 顶部 WiFi 状态 */
static lv_obj_t        *s_rec_flag    = NULL;   /*!< 顶部录音指示 */
static lv_timer_t      *s_timer       = NULL;

/** 当前显示的气泡行（用于超量时删除最老的） */
static lv_obj_t        *s_rows[APP_CHAT_MAX_BUBBLES];
static int              s_row_count   = 0;

/** 错误提示到期时间（lv_tick 基准，0 = 没有错误提示） */
static uint32_t         s_error_until = 0;
/** 上一次显示的状态文字，用于判断是否需要刷新 */
static char             s_status_text[128] = "";
/** 顶部指示器上一次反映的状态（避免每 50ms 重复设置文本） */
static app_state_t      s_ind_state   = (app_state_t)-1;

/**
 * 气泡文本裁剪缓冲
 * @note  放静态区（不放栈上）：LVGL 任务栈只有 7KB，1KB 的局部数组容易被挤爆；
 *        本模块只在 LVGL 线程里被调用，用静态缓冲是安全的。
 */
static char             s_bubble_text[APP_CHAT_BUBBLE_TEXT_MAX];

/**
 * 从 chat_msg_queue 取消息用的缓冲
 * @note  同样必须放静态区：chat_msg_t 有 2KB+，而本回调是在 LVGL 任务里跑的，
 *        上面还压着 lv_timer_handler 的一串调用，放局部变量容易爆栈。
 */
static chat_msg_t       s_rx_msg;

/* ============================ 内部函数：样式 ============================ */

/** 给对象套用统一的“无边框无内边距透明”设置 */
static void ui_obj_base(lv_obj_t *obj, lv_color_t bg)
{
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
    lv_obj_set_scrollable(obj, false);
}

/* ============================ 内部函数：气泡 ============================ */

/**
 * @brief  删除最老的一个气泡行
 */
static void ui_drop_oldest_row(void)
{
    if (s_row_count <= 0) {
        return;
    }
    if (s_rows[0] != NULL) {
        lv_obj_delete(s_rows[0]);
    }
    for (int i = 0; i < s_row_count - 1; i++) {
        s_rows[i] = s_rows[i + 1];
    }
    s_row_count--;
    s_rows[s_row_count] = NULL;
}

/**
 * @brief  在消息区追加一个气泡
 *
 * @param  text    文本（UTF-8）
 * @param  is_user true = 用户（右对齐 + 浅蓝），false = AI（左对齐 + 浅灰）
 */
static void ui_add_bubble(const char *text, bool is_user)
{
    if (s_msg_area == NULL || text == NULL) {
        return;
    }

    /* 1. 先按字节数截断（不会切断多字节字符），避免超长文本吃满 LVGL 内存 */
    char *clipped = s_bubble_text;
    app_text_truncate_utf8(text, clipped, sizeof(s_bubble_text), 0xFFFFFFFFU);
    if (clipped[0] == '\0') {
        return;
    }

    /* 2. 气泡里文字排不下就换行，排得下就按内容自适应宽度 */
    uint32_t text_w   = app_text_display_width(clipped);
    bool     need_wrap = (text_w > APP_CHAT_BUBBLE_MAX_W);

    /* 3. 行容器：占满宽度，用 flex 把气泡推到左边或右边 */
    lv_obj_t *row = lv_obj_create(s_msg_area);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          is_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    ui_obj_base(row, lv_color_hex(APP_COLOR_BG));

    /* 4. 气泡本体 */
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bubble,
                              lv_color_hex(is_user ? APP_COLOR_USER_BUBBLE : APP_COLOR_AI_BUBBLE),
                              LV_PART_MAIN);
    lv_obj_set_scrollable(bubble, false);

    /* 5. 文字 */
    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, clipped);
    if (s_font != NULL) {
        lv_obj_set_style_text_font(label, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    if (need_wrap) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(label, APP_CHAT_BUBBLE_MAX_W);
    } else {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_width(label, LV_SIZE_CONTENT);
    }

    /* 6. 记录并限制气泡数量 */
    if (s_row_count >= APP_CHAT_MAX_BUBBLES) {
        ui_drop_oldest_row();
    }
    s_rows[s_row_count++] = row;

    /* 7. 新消息自动滚到底部 */
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

/* ============================ 内部函数：状态 ============================ */

/**
 * @brief  设置底部状态行文字
 * @param  is_error true 时用红色显示，并在若干秒后自动恢复状态提示
 */
static void ui_set_status(const char *text, bool is_error)
{
    if (s_status_lbl == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(s_status_lbl, text);
    lv_obj_set_style_text_color(s_status_lbl,
                               lv_color_hex(is_error ? APP_COLOR_ERROR : APP_COLOR_WARN),
                               LV_PART_MAIN);

    strncpy(s_status_text, text, sizeof(s_status_text) - 1);
    s_status_text[sizeof(s_status_text) - 1] = '\0';

    s_error_until = is_error ? (lv_tick_get() + UI_ERROR_SHOW_MS) : 0;
}

/* ============================ 内部函数：定时器 ============================ */

/**
 * @brief  界面刷新定时器（在 LVGL 任务上下文里执行）
 *
 * 做的事：
 *   1. 把 chat_msg_queue 里的消息全部取出来更新界面；
 *   2. 没有错误提示时，底部状态行跟随业务状态机（app_bus 的状态）；
 *   3. 顶部录音指示跟随状态机。
 */
static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 1. 处理消息队列（s_rx_msg 是静态缓冲，不占 LVGL 任务的栈） */
    QueueHandle_t q = app_bus_chat_queue();
    if (q != NULL) {
        chat_msg_t *msg = &s_rx_msg;
        while (xQueueReceive(q, msg, 0) == pdTRUE) {
            switch (msg->type) {
            case CHAT_MSG_USER:
                ui_add_bubble(msg->text, true);
                break;
            case CHAT_MSG_AI:
                ui_add_bubble(msg->text, false);
                break;
            case CHAT_MSG_STATUS:
                ui_set_status(msg->text, false);
                break;
            case CHAT_MSG_ERROR:
                ui_set_status(msg->text, true);
                ESP_LOGW(TAG, "界面错误提示：%s", msg->text);
                break;
            case CHAT_MSG_WIFI:
                if (s_wifi_lbl != NULL) {
                    lv_label_set_text(s_wifi_lbl, msg->text);
                }
                break;
            default:
                break;
            }
        }
    }

    /* 2. 状态行：错误提示优先显示，过期后自动回到状态机的文字 */
    app_state_t state        = app_bus_get_state();
    bool        error_active = (s_error_until != 0) &&
                               ((int32_t)(lv_tick_get() - s_error_until) < 0);
    if (!error_active) {
        const char *want = app_bus_state_text(state);
        if (strcmp(s_status_text, want) != 0) {
            ui_set_status(want, false);
        }
    }

    /* 3. 顶部录音/处理指示（用纯中文，保证 hzk16 字库一定能渲染；只在变化时刷新） */
    if (s_rec_flag != NULL && state != s_ind_state) {
        s_ind_state = state;
        if (state == APP_STATE_RECORDING) {
            lv_label_set_text(s_rec_flag, "录音中");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_ERROR), LV_PART_MAIN);
        } else if (state == APP_STATE_UPLOADING) {
            lv_label_set_text(s_rec_flag, "识别中");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
        } else if (state == APP_STATE_LLM_REQUEST) {
            lv_label_set_text(s_rec_flag, "思考中");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
        } else {
            lv_label_set_text(s_rec_flag, "待机");
            lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_OK), LV_PART_MAIN);
        }
    }
}

/* ============================ 对外接口 ============================ */

esp_err_t app_chat_ui_init(const lv_font_t *cn_font)
{
    if (s_scr != NULL) {
        ESP_LOGW(TAG, "界面已经创建过了");
        return ESP_OK;
    }

    s_font = cn_font;

    /* 建界面必须持有 LVGL 锁（LVGL 任务在另一个任务里跑） */
    if (!lvgl_port_lock(2000)) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---------- 1. 新建 screen 并加载（覆盖游戏主页，主页对象留在旧 screen 里） ---------- */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(APP_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(s_scr, false);
    lv_screen_load(s_scr);

    /* ---------- 2. 顶部状态栏 ---------- */
    lv_obj_t *topbar = lv_obj_create(s_scr);
    lv_obj_set_size(topbar, LV_PCT(100), APP_CHAT_TOPBAR_H);
    lv_obj_align(topbar, LV_ALIGN_TOP_MID, 0, 0);
    ui_obj_base(topbar, lv_color_hex(APP_COLOR_TOPBAR));

    s_wifi_lbl = lv_label_create(topbar);
    lv_label_set_text(s_wifi_lbl, "WiFi: 连接中...");
    if (s_font) {
        lv_obj_set_style_text_font(s_wifi_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(APP_COLOR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_max_width(s_wifi_lbl, BSP_LCD_H_RES - 80, LV_PART_MAIN);
    lv_label_set_long_mode(s_wifi_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_wifi_lbl, LV_ALIGN_LEFT_MID, 6, 0);

    s_rec_flag = lv_label_create(topbar);
    lv_label_set_text(s_rec_flag, "待机");
    if (s_font) {
        lv_obj_set_style_text_font(s_rec_flag, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_rec_flag, lv_color_hex(APP_COLOR_OK), LV_PART_MAIN);
    lv_obj_align(s_rec_flag, LV_ALIGN_RIGHT_MID, -6, 0);

    /* ---------- 3. 消息区（可滚动） ---------- */
    s_msg_area = lv_obj_create(s_scr);
    lv_obj_set_size(s_msg_area, LV_PCT(100), UI_MSG_AREA_H);
    lv_obj_align(s_msg_area, LV_ALIGN_TOP_MID, 0, APP_CHAT_TOPBAR_H);
    lv_obj_set_style_pad_all(s_msg_area, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_msg_area, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_msg_area, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_msg_area, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_msg_area, lv_color_hex(APP_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_flex_flow(s_msg_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_msg_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_msg_area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_msg_area, LV_SCROLLBAR_MODE_AUTO);

    /* ---------- 4. 底部状态行（整屏宽 + 超长省略号，避免文字跑出屏幕） ---------- */
    s_status_lbl = lv_label_create(s_scr);
    if (s_font) {
        lv_obj_set_style_text_font(s_status_lbl, s_font, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(APP_COLOR_WARN), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_status_lbl, BSP_LCD_H_RES - 8);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s_status_lbl, "正在启动 ...");
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);
    strncpy(s_status_text, "正在启动 ...", sizeof(s_status_text) - 1);

    /* ---------- 5. 欢迎气泡 ---------- */
    ui_add_bubble("你好！按住 GPIO0 按键说话，松开发送。", false);

    /* ---------- 6. 刷新定时器（LVGL 任务里跑） ---------- */
    s_timer = lv_timer_create(ui_timer_cb, APP_CHAT_UI_TICK_MS, NULL);
    if (s_timer == NULL) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "创建界面定时器失败");
        return ESP_ERR_NO_MEM;
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "聊天界面创建完成（%dx%d，消息区 %d 像素高）",
             BSP_LCD_H_RES, BSP_LCD_V_RES, UI_MSG_AREA_H);
    return ESP_OK;
}

void app_chat_ui_clear(void)
{
    if (s_msg_area == NULL) {
        return;
    }
    if (lvgl_port_lock(1000)) {
        for (int i = 0; i < s_row_count; i++) {
            if (s_rows[i] != NULL) {
                lv_obj_delete(s_rows[i]);
                s_rows[i] = NULL;
            }
        }
        s_row_count = 0;
        lvgl_port_unlock();
    }
}

void app_chat_ui_show_status(const char *text)
{
    app_bus_post_chat(CHAT_MSG_STATUS, text);
}

void app_chat_ui_show_wifi(const char *text)
{
    app_bus_post_chat(CHAT_MSG_WIFI, text);
}

void app_chat_ui_add_message(bool is_user, const char *text)
{
    app_bus_post_chat(is_user ? CHAT_MSG_USER : CHAT_MSG_AI, text);
}

bool app_chat_ui_is_ready(void)
{
    return s_scr != NULL;
}
