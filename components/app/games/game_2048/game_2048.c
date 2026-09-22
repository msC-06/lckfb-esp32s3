/**
 * @file    game_2048.c
 * @brief   2048 游戏实现：4x4 棋盘、滑动合并、得分/最高分、胜负弹窗
 *
 * 分层：app 层游戏模块，只用：
 *   - LVGL（界面）+ app 层的存档服务（异步文件读写）
 *   - game_if.h 的统一游戏接口
 * 不接触任何 bsp 底层外设，也不直接做文件 I/O（交给存档服务的独立任务）。
 *
 * 界面布局（320x240）：
 *   ┌────────────────────────────────────────────┐
 *   │ [返回]       2048               [重开]      │  ← 顶部栏
 *   ├───────────────────┬────────────────────────┤
 *   │  4x4 棋盘(172x172) │ 得分 / 最高             │
 *   │                   │      [上]              │
 *   │                   │  [左]     [右]         │
 *   │                   │      [下]              │
 *   └───────────────────┴────────────────────────┘
 *
 * 存档：最高分保存在 TF 卡 /sd/game_save/score_2048.txt（纯文本数字）
 *       上电异步读取、刷新最高分/退出/重开时异步保存，全程不阻塞 LVGL。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_2048.h"

#include "game_launcher/save_service.h"

#include "esp_log.h"

static const char *TAG = "game2048";

/* ============================ 参数 ============================ */

#define BOARD_DIM          4                       /* 4x4 棋盘 */
#define TILE_SIZE          38                      /* 每格像素 */
#define TILE_GAP           4                       /* 格子间距 */
#define BOARD_X            4                       /* 棋盘左上角 */
#define BOARD_Y            30
#define PANEL_X            180                     /* 右侧信息/方向键区 */
#define WIN_VALUE          2048U                   /* 达成即“胜利” */

#define SAVE_FILE_NAME     "score_2048.txt"        /* 存在 /sd/game_save/ 下 */

/** 棋盘滑动方向 */
typedef enum {
    DIR_UP = 0,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT,
} dir_t;

/** 弹窗按钮动作 */
typedef enum {
    ACT_NONE = 0,
    ACT_CONTINUE,
    ACT_RESTART,
    ACT_EXIT,
} dialog_action_t;

/* ============================ 内部状态（全部 static） ============================ */

static uint32_t   s_board[BOARD_DIM][BOARD_DIM];   /* 棋盘数值，0 = 空格 */

static uint32_t   s_score = 0;                     /* 当前得分 */
static uint32_t   s_best  = 0;                     /* 历史最高分 */
static bool       s_won   = false;                 /* 本局是否已经达成 2048 */
static bool       s_over  = false;                 /* 本局是否已结束 */

static game_ctx_t s_ctx;                           /* 管理器给的上下文副本 */
static const lv_font_t *s_font = NULL;             /* 中文字体 */

static lv_obj_t  *s_tile_labels[BOARD_DIM][BOARD_DIM];
static lv_obj_t  *s_score_label = NULL;
static lv_obj_t  *s_best_label  = NULL;
static lv_obj_t  *s_msgbox      = NULL;            /* 胜负弹窗（非页面子对象，需手动删） */

static bool       s_wait_save_load = false;        /* 是否在等最高分读取结果 */
static uint32_t   s_best_saved = 0;                /* 已经写入卡里的最高分（避免重复写） */

/* ============================ 棋盘逻辑 ============================ */

/** 棋盘清空 */
static void board_clear(void)
{
    memset(s_board, 0, sizeof(s_board));
}

/**
 * @brief  在随机空格生成一个新方块（90% 概率 2，10% 概率 4）
 * @return true 生成成功；false 没有空格
 */
static bool board_spawn(void)
{
    int empty[BOARD_DIM * BOARD_DIM][2];
    int count = 0;

    for (int r = 0; r < BOARD_DIM; r++) {
        for (int c = 0; c < BOARD_DIM; c++) {
            if (s_board[r][c] == 0) {
                empty[count][0] = r;
                empty[count][1] = c;
                count++;
            }
        }
    }
    if (count == 0) {
        return false;
    }

    int idx = rand() % count;
    s_board[empty[idx][0]][empty[idx][1]] = ((rand() % 10) == 0) ? 4U : 2U;
    return true;
}

/**
 * @brief  把一条长度为 4 的线向索引 0 方向“压缩 + 合并”
 *
 * @param  line   输入输出：4 个格子（0 表示空）
 * @param  gained 输出：本次合并得到的分数
 * @return true 发生了移动/合并（用于判断是否需要生成新方块）
 */
static bool slide_line(uint32_t *line, uint32_t *gained)
{
    uint32_t out[BOARD_DIM] = { 0 };
    int      n = 0;

    /* 1. 压缩：把非 0 依次前移 */
    for (int i = 0; i < BOARD_DIM; i++) {
        if (line[i] != 0) {
            out[n++] = line[i];
        }
    }

    /* 2. 合并：相邻相同则翻倍，合并后跳过下一格（经典 2048 规则，一格只合并一次） */
    for (int i = 0; i + 1 < n; i++) {
        if (out[i] == out[i + 1]) {
            out[i] *= 2U;
            *gained += out[i];
            for (int j = i + 1; j + 1 < n; j++) {
                out[j] = out[j + 1];
            }
            out[--n] = 0;       /* 尾部补 0 */
        }
    }

    /* 3. 判断是否变化并写回 */
    bool changed = false;
    for (int i = 0; i < BOARD_DIM; i++) {
        if (line[i] != out[i]) {
            changed = true;
        }
        line[i] = out[i];
    }
    return changed;
}

/**
 * @brief  按方向滑动整个棋盘
 * @return true 棋盘发生了变化
 */
static bool board_move(dir_t dir)
{
    bool     changed = false;
    uint32_t gained  = 0;

    for (int i = 0; i < BOARD_DIM; i++) {
        uint32_t line[BOARD_DIM];

        /* 按方向把第 i 条线取成“朝移动方向排列”的 4 个值 */
        for (int j = 0; j < BOARD_DIM; j++) {
            switch (dir) {
            case DIR_UP:    line[j] = s_board[j][i];                break;
            case DIR_DOWN:  line[j] = s_board[BOARD_DIM - 1 - j][i]; break;
            case DIR_LEFT:  line[j] = s_board[i][j];                break;
            case DIR_RIGHT: line[j] = s_board[i][BOARD_DIM - 1 - j]; break;
            default:        line[j] = 0;                            break;
            }
        }

        if (slide_line(line, &gained)) {
            changed = true;
        }

        /* 写回 */
        for (int j = 0; j < BOARD_DIM; j++) {
            switch (dir) {
            case DIR_UP:    s_board[j][i] = line[j];                break;
            case DIR_DOWN:  s_board[BOARD_DIM - 1 - j][i] = line[j]; break;
            case DIR_LEFT:  s_board[i][j] = line[j];                break;
            case DIR_RIGHT: s_board[i][BOARD_DIM - 1 - j] = line[j]; break;
            default:        break;
            }
        }
    }

    if (changed) {
        s_score += gained;
    }
    return changed;
}

/** 棋盘上是否还有空格 */
static bool board_has_empty(void)
{
    for (int r = 0; r < BOARD_DIM; r++) {
        for (int c = 0; c < BOARD_DIM; c++) {
            if (s_board[r][c] == 0) {
                return true;
            }
        }
    }
    return false;
}

/** 是否还能继续移动（有空格，或存在相邻相同） */
static bool board_can_move(void)
{
    if (board_has_empty()) {
        return true;
    }
    for (int r = 0; r < BOARD_DIM; r++) {
        for (int c = 0; c < BOARD_DIM; c++) {
            uint32_t v = s_board[r][c];
            if (c + 1 < BOARD_DIM && s_board[r][c + 1] == v) {
                return true;
            }
            if (r + 1 < BOARD_DIM && s_board[r + 1][c] == v) {
                return true;
            }
        }
    }
    return false;
}

/** 检查是否出现 2048 */
static bool board_reached_win(void)
{
    for (int r = 0; r < BOARD_DIM; r++) {
        for (int c = 0; c < BOARD_DIM; c++) {
            if (s_board[r][c] >= WIN_VALUE) {
                return true;
            }
        }
    }
    return false;
}

/* ============================ 界面 ============================ */

/** 给对象及其子控件套上中文字体 */
static void apply_cn_font(lv_obj_t *obj)
{
    if (s_font) {
        lv_obj_set_style_text_font(obj, s_font, LV_PART_MAIN);
    }
}

/** 方块底色（经典 2048 配色） */
static uint32_t tile_color(uint32_t value)
{
    switch (value) {
    case 2:    return 0xEEE4DA;
    case 4:    return 0xEDE0C8;
    case 8:    return 0xF2B179;
    case 16:   return 0xF59563;
    case 32:   return 0xF67C5F;
    case 64:   return 0xF65E3B;
    case 128:  return 0xEDCF72;
    case 256:  return 0xEDCC61;
    case 512:  return 0xEDC850;
    case 1024: return 0xEDC53F;
    case 2048: return 0xEDC22E;
    default:   return 0x3C3A32;      /* >2048 */
    }
}

/** 方块文字颜色（浅底用深色字） */
static uint32_t tile_text_color(uint32_t value)
{
    return (value <= 4) ? 0x776E65 : 0xFFFFFF;
}

/** 刷新 16 个格子的文字与配色 */
static void board_refresh(void)
{
    for (int r = 0; r < BOARD_DIM; r++) {
        for (int c = 0; c < BOARD_DIM; c++) {
            lv_obj_t *tile = s_tile_labels[r][c];
            if (tile == NULL) {
                continue;
            }
            uint32_t v = s_board[r][c];

            if (v == 0) {
                lv_label_set_text(tile, "");
                lv_obj_set_style_bg_color(tile, lv_color_hex(0x20303F), LV_PART_MAIN);
                continue;
            }

            char text[12];
            snprintf(text, sizeof(text), "%u", (unsigned)v);
            lv_label_set_text(tile, text);
            lv_obj_set_style_bg_color(tile, lv_color_hex(tile_color(v)), LV_PART_MAIN);
            lv_obj_set_style_text_color(tile, lv_color_hex(tile_text_color(v)), LV_PART_MAIN);
        }
    }
}

/** 刷新得分/最高分文字 */
static void score_refresh(void)
{
    if (s_score_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "得分 %u", (unsigned)s_score);
        lv_label_set_text(s_score_label, buf);
    }
    if (s_best_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "最高 %u", (unsigned)s_best);
        lv_label_set_text(s_best_label, buf);
    }
}

/* ============================ 存档（异步，不阻塞 LVGL） ============================ */

/** 把最高分写入 TF 卡（非阻塞投递，真正的 I/O 在存档任务里做） */
static void best_score_save(void)
{
    if (s_best == 0 || s_best == s_best_saved) {
        return;                     /* 没有分或和卡里的一样，就不用重复写 */
    }
    char text[16];
    snprintf(text, sizeof(text), "%u\n", (unsigned)s_best);

    esp_err_t err = save_service_post_save(SAVE_FILE_NAME, text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "最高分保存请求投递失败(%s)，存档队列可能已满", esp_err_to_name(err));
    } else {
        s_best_saved = s_best;      /* 投递成功就不再重复投递 */
    }
}

/** 如果当前分数超过历史最高分，则更新显示并异步保存 */
static void best_score_try_update(void)
{
    if (s_score <= s_best) {
        return;
    }
    s_best = s_score;
    score_refresh();
    best_score_save();
}

/** 读取最高分结果（在 update 里轮询，非阻塞） */
static void best_score_poll(void)
{
    if (!s_wait_save_load) {
        return;
    }
    char buf[SAVE_DATA_MAX];
    int  ret = save_service_poll(buf, sizeof(buf));
    if (ret == 0) {
        return;                     /* 还没结果，下次再问 */
    }
    s_wait_save_load = false;

    if (ret > 0) {
        long v = strtol(buf, NULL, 10);
        if (v > 0) {
            s_best = (uint32_t)v;
            s_best_saved = s_best;      /* 卡里就是这个值，不用再写回去 */
            ESP_LOGI(TAG, "读取历史最高分: %u", (unsigned)s_best);
            score_refresh();
        }
    } else {
        ESP_LOGI(TAG, "没有历史最高分记录（首次运行或 TF 卡不可用）");
    }
}

/* ============================ 弹窗 ============================ */

static void dialog_close(void)
{
    if (s_msgbox) {
        lv_msgbox_close(s_msgbox);
        s_msgbox = NULL;
    }
}

static void game_reset(void);       /* 前置声明 */

static void dialog_btn_cb(lv_event_t *e)
{
    dialog_action_t act = (dialog_action_t)(intptr_t)lv_event_get_user_data(e);
    dialog_close();

    switch (act) {
    case ACT_CONTINUE:
        break;                      /* 继续玩（已经关掉弹窗） */
    case ACT_RESTART:
        game_reset();
        break;
    case ACT_EXIT:
        if (s_ctx.request_exit) {
            s_ctx.request_exit();
        }
        break;
    default:
        break;
    }
}

/**
 * @brief  弹出中文提示框
 * @param  title      标题（HZK16 渲染）
 * @param  text       正文
 * @param  ok_text    主按钮文字（可为 NULL）
 * @param  ok_act     主按钮动作
 * @param  alt_text   次按钮文字（可为 NULL）
 * @param  alt_act    次按钮动作
 */
static void dialog_show(const char *title, const char *text,
                        const char *ok_text, dialog_action_t ok_act,
                        const char *alt_text, dialog_action_t alt_act)
{
    dialog_close();

    /* parent = NULL：弹窗放在顶层，不随游戏页面一起删除，所以退出时要手动关 */
    s_msgbox = lv_msgbox_create(NULL);
    if (s_msgbox == NULL) {
        ESP_LOGE(TAG, "创建弹窗失败");
        return;
    }
    apply_cn_font(s_msgbox);        /* 字体样式会继承到标题/正文/按钮 */

    lv_msgbox_add_title(s_msgbox, title);
    lv_msgbox_add_text(s_msgbox, text);

    if (ok_text) {
        lv_obj_t *btn = lv_msgbox_add_footer_button(s_msgbox, ok_text);
        lv_obj_add_event_cb(btn, dialog_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ok_act);
    }
    if (alt_text) {
        lv_obj_t *btn = lv_msgbox_add_footer_button(s_msgbox, alt_text);
        lv_obj_add_event_cb(btn, dialog_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)alt_act);
    }
}

/* ============================ 游戏流程 ============================ */

/** 重开一局 */
static void game_reset(void)
{
    dialog_close();
    board_clear();
    s_score = 0;
    s_over  = false;
    s_won   = false;

    board_spawn();
    board_spawn();
    board_refresh();
    score_refresh();
    ESP_LOGI(TAG, "重开一局");
}

/**
 * @brief  执行一次移动（方向键/按键都走这里）
 */
static void game_move(dir_t dir)
{
    if (s_over || s_msgbox != NULL) {
        return;                     /* 结束或弹窗期间不接受移动 */
    }

    if (!board_move(dir)) {
        return;                     /* 没变化就不生成新方块 */
    }

    board_spawn();
    board_refresh();
    score_refresh();

    /* 胜利判定（只弹一次，可以继续玩） */
    if (!s_won && board_reached_win()) {
        s_won = true;
        best_score_try_update();
        dialog_show("恭喜", "达成 2048 ！", "继续", ACT_CONTINUE, "重开", ACT_RESTART);
        return;
    }

    /* 失败判定：没有空格也不能合并 */
    if (!board_can_move()) {
        s_over = true;
        best_score_try_update();            /* 游戏结束，保存最高分 */
        dialog_show("游戏结束", "没有可以移动的方块了", "重开", ACT_RESTART, "返回", ACT_EXIT);
    }
}

/* ============================ 界面构造 ============================ */

/** 方向按钮点击 */
static void dir_btn_cb(lv_event_t *e)
{
    dir_t dir = (dir_t)(intptr_t)lv_event_get_user_data(e);
    game_move(dir);
}

/** 顶部小按钮（返回/重开） */
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    best_score_try_update();        /* 退出前保存一次最高分 */
    dialog_close();
    if (s_ctx.request_exit) {
        s_ctx.request_exit();
    }
}

static void reset_btn_cb(lv_event_t *e)
{
    (void)e;
    best_score_try_update();
    game_reset();
}

/**
 * @brief  创建一个方向键按钮
 */
static void create_dir_button(lv_obj_t *parent, const char *text, int x, int y, dir_t dir)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C4B6E), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, dir_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)dir);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    apply_cn_font(label);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(label);
}

/**
 * @brief  创建游戏页面（由 game_manager 通过 init 回调间接调用）
 */
static esp_err_t game_2048_build_ui(lv_obj_t *parent)
{
    /* ---------- 页面底色 ---------- */
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0B1622), LV_PART_MAIN);

    /* ---------- 顶部栏：返回 / 标题 / 重开 ---------- */
    lv_obj_t *btn_back = lv_button_create(parent);
    lv_obj_set_pos(btn_back, 4, 2);
    lv_obj_set_size(btn_back, 58, 24);
    lv_obj_set_style_radius(btn_back, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x37474F), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "返回");
    apply_cn_font(lbl_back);
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(lbl_back);

    lv_obj_t *lbl_title = lv_label_create(parent);
    lv_label_set_text(lbl_title, "2048");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, -8, 6);

    lv_obj_t *btn_reset = lv_button_create(parent);
    lv_obj_set_pos(btn_reset, 258, 2);
    lv_obj_set_size(btn_reset, 58, 24);
    lv_obj_set_style_radius(btn_reset, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_reset, lv_color_hex(0x37474F), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_reset, reset_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_reset = lv_label_create(btn_reset);
    lv_label_set_text(lbl_reset, "重开");
    apply_cn_font(lbl_reset);
    lv_obj_set_style_text_color(lbl_reset, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(lbl_reset);

    /* ---------- 棋盘：4x4 个 label 作为方块 ---------- */
    lv_obj_t *board = lv_obj_create(parent);
    lv_obj_set_pos(board, BOARD_X, BOARD_Y);
    lv_obj_set_size(board, BOARD_DIM * TILE_SIZE + (BOARD_DIM + 1) * TILE_GAP,
                    BOARD_DIM * TILE_SIZE + (BOARD_DIM + 1) * TILE_GAP);
    lv_obj_set_style_pad_all(board, TILE_GAP, LV_PART_MAIN);
    lv_obj_set_style_radius(board, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(board, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(board, lv_color_hex(0x14202B), LV_PART_MAIN);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE);

    for (int r = 0; r < BOARD_DIM; r++) {
        for (int c = 0; c < BOARD_DIM; c++) {
            lv_obj_t *tile = lv_label_create(board);
            lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
            lv_obj_set_pos(tile, c * (TILE_SIZE + TILE_GAP), r * (TILE_SIZE + TILE_GAP));
            lv_obj_set_style_radius(tile, 4, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(tile, lv_color_hex(0x20303F), LV_PART_MAIN);
            lv_obj_set_style_text_align(tile, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_font(tile, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(tile, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_pad_top(tile, 10, LV_PART_MAIN);
            s_tile_labels[r][c] = tile;
        }
    }

    /* ---------- 右侧：得分 / 最高 / 方向键 ---------- */
    s_score_label = lv_label_create(parent);
    lv_obj_set_pos(s_score_label, PANEL_X, 34);
    apply_cn_font(s_score_label);
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    s_best_label = lv_label_create(parent);
    lv_obj_set_pos(s_best_label, PANEL_X, 56);
    apply_cn_font(s_best_label);
    lv_obj_set_style_text_color(s_best_label, lv_color_hex(0xFFD54F), LV_PART_MAIN);

    /* 3x3 方向键盘（只放上下左右四格） */
    create_dir_button(parent, "上", PANEL_X + 44, 84,  DIR_UP);
    create_dir_button(parent, "左", PANEL_X,      130, DIR_LEFT);
    create_dir_button(parent, "右", PANEL_X + 88, 130, DIR_RIGHT);
    create_dir_button(parent, "下", PANEL_X + 44, 176, DIR_DOWN);

    score_refresh();
    board_refresh();
    return ESP_OK;
}

/* ============================ 统一游戏接口实现 ============================ */

static esp_err_t game_2048_init(game_t *self, const game_ctx_t *ctx)
{
    (void)self;
    if (ctx == NULL || ctx->parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ctx  = *ctx;                      /* 保存上下文（parent/字体/退出回调） */
    s_font = ctx->cn_font;

    memset(s_tile_labels, 0, sizeof(s_tile_labels));
    s_msgbox = NULL;
    s_over   = false;
    s_won    = false;

    game_reset();                       /* 清盘 + 生成两个方块 */

    esp_err_t err = game_2048_build_ui(ctx->parent);
    if (err != ESP_OK) {
        return err;
    }
    board_refresh();
    score_refresh();

    /* 异步读取历史最高分（不阻塞 LVGL，结果在 update 里取） */
    s_wait_save_load = false;
    if (ctx->sd_ready) {
        if (save_service_post_load(SAVE_FILE_NAME) == ESP_OK) {
            s_wait_save_load = true;
        } else {
            ESP_LOGW(TAG, "存档队列满，最高分读取请求未投递");
        }
    } else {
        ESP_LOGW(TAG, "TF 卡不可用，最高分不会读写（游戏本身可正常玩）");
    }

    ESP_LOGI(TAG, "2048 初始化完成");
    return ESP_OK;
}

static void game_2048_deinit(game_t *self)
{
    (void)self;

    /* 弹窗是顶层对象，不随页面删除，必须手动关掉 */
    dialog_close();

    if (s_score > s_best) {
        s_best = s_score;
    }
    /* 退出前保存一次最高分（非阻塞投递） */
    best_score_save();

    memset(s_tile_labels, 0, sizeof(s_tile_labels));
    s_score_label = NULL;
    s_best_label  = NULL;

    ESP_LOGI(TAG, "2048 已退出（得分 %u，最高 %u）", (unsigned)s_score, (unsigned)s_best);
}

static void game_2048_update(game_t *self, uint32_t elapsed_ms)
{
    (void)self;
    (void)elapsed_ms;

    /* 只做非阻塞的存档结果轮询 */
    best_score_poll();
}

static bool game_2048_on_key(game_t *self, game_key_t key)
{
    (void)self;
    switch (key) {
    case GAME_KEY_UP:    game_move(DIR_UP);    return true;
    case GAME_KEY_DOWN:  game_move(DIR_DOWN);  return true;
    case GAME_KEY_LEFT:  game_move(DIR_LEFT);  return true;
    case GAME_KEY_RIGHT: game_move(DIR_RIGHT); return true;
    case GAME_KEY_OK:    game_reset();         return true;
    case GAME_KEY_BACK:  best_score_try_update(); dialog_close();
                         if (s_ctx.request_exit) { s_ctx.request_exit(); } return true;
    default:             return false;
    }
}

/* ============================ 对外的游戏对象 ============================ */

static const game_t s_game_2048 = {
    .id         = "2048",
    .title      = "2048",
    .icon_text  = "2048",
    .icon_color = 0xEDC22E,
    .init       = game_2048_init,
    .deinit     = game_2048_deinit,
    .update     = game_2048_update,
    .on_key     = game_2048_on_key,
    .priv       = NULL,
};

const game_t *game_2048_get(void)
{
    return &s_game_2048;
}
