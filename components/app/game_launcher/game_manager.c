/**
 * @file    game_manager.c
 * @brief   游戏管理器实现：注册表 + 页面切换/销毁 + 统一 update 循环
 *
 * 页面组织：
 *     lv_screen_active()
 *       └── s_root（管理器建的根容器，满屏、无滚动、无边框）
 *             └── s_page（当前页面：主页 或 某个游戏页，切换时整体删除再重建）
 *
 * 内存管理：切换页面时对整棵页面对象调用 lv_obj_delete()，LVGL 会连带删除所有子控件；
 *           游戏自己的非 LVGL 资源由 game->deinit() 负责释放。
 */

#include <string.h>

#include "game_manager.h"

#include "game_launcher/launcher_ui.h"
#include "game_launcher/save_service.h"

#include "my_drivers/font_hzk16/hzk16.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "game_mgr";

/* ============================ 内部状态（全部 static） ============================ */

static const game_t *s_games[GAME_MANAGER_MAX_GAMES];
static int           s_game_count = 0;

static lv_obj_t     *s_root   = NULL;   /* 根容器 */
static lv_obj_t     *s_page   = NULL;   /* 当前页面对象 */
static lv_timer_t   *s_timer  = NULL;

static const game_t *s_current = NULL;  /* 当前游戏（NULL = 在主页） */
static game_ctx_t    s_ctx;             /* 传给游戏的上下文 */
static uint32_t      s_last_tick = 0;

static const lv_font_t *s_cn_font = NULL;

/* ============================ 内部函数 ============================ */

/**
 * @brief  删除当前页面（连带所有子控件），然后建立新页面
 * @param  make_page 新页面的构造函数（返回页面根对象）
 * @return 新页面对象
 */
static lv_obj_t *page_switch(lv_obj_t *(*make_page)(lv_obj_t *parent))
{
    if (s_page) {
        lv_obj_delete(s_page);      /* 整棵树一起删，子控件不会泄漏 */
        s_page = NULL;
    }
    if (make_page == NULL) {
        return NULL;
    }

    s_page = make_page(s_root);
    if (s_page == NULL) {
        ESP_LOGE(TAG, "页面创建失败");
    }
    return s_page;
}

/**
 * @brief  主页构造函数（给 page_switch 用）
 */
static lv_obj_t *make_home_page(lv_obj_t *parent)
{
    return launcher_ui_create(parent, s_cn_font);
}

/**
 * @brief  管理器自己的 update 定时器回调（运行在 LVGL 任务里）
 */
static void manager_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    game_manager_update();
}

/* ============================ 对外接口 ============================ */

esp_err_t game_manager_init(void)
{
    if (s_root != NULL) {
        ESP_LOGW(TAG, "管理器已经初始化过了");
        return ESP_OK;
    }

    /* 1. 中文字库（片内 fontbin 分区，按需读取；ASCII 用西文字体回退） */
    esp_err_t err = hzk16_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HZK16 中文字库初始化失败: %s（中文将显示为占位框）", esp_err_to_name(err));
    }
    s_cn_font = hzk16_get_lv_font();
    hzk16_set_fallback_font(&lv_font_montserrat_14);

    /* 2. 存档服务（独立任务 + 环形缓冲，不阻塞 LVGL） */
    if (save_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "存档服务初始化失败，最高分将无法保存");
    }

    /* 3. 根容器：满屏、透明、无内边距、不滚动 */
    lv_obj_t *scr = lv_screen_active();
    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_root);
    lv_obj_set_style_pad_all(s_root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* 4. update 循环定时器（在 LVGL 任务里跑，可以安全操作控件） */
    s_last_tick = lv_tick_get();
    s_timer = lv_timer_create(manager_timer_cb, GAME_MANAGER_TICK_MS, NULL);
    if (s_timer == NULL) {
        ESP_LOGE(TAG, "创建 update 定时器失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "游戏管理器初始化完成（定时器 %d ms）", GAME_MANAGER_TICK_MS);
    return ESP_OK;
}

esp_err_t game_manager_register(const game_t *game)
{
    if (game == NULL || game->id == NULL || game->init == NULL || game->deinit == NULL) {
        ESP_LOGE(TAG, "游戏描述非法");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_game_count >= GAME_MANAGER_MAX_GAMES) {
        ESP_LOGE(TAG, "游戏注册表已满（最多 %d 个）", GAME_MANAGER_MAX_GAMES);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_game_count; i++) {
        if (strcmp(s_games[i]->id, game->id) == 0) {
            ESP_LOGW(TAG, "游戏 %s 已注册，忽略重复注册", game->id);
            return ESP_OK;
        }
    }

    s_games[s_game_count++] = game;
    ESP_LOGI(TAG, "注册游戏: %s (%s)", game->title ? game->title : "?", game->id);
    return ESP_OK;
}

esp_err_t game_manager_start(void)
{
    if (s_root == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_current = NULL;
    page_switch(make_home_page);
    return (s_page != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t game_manager_enter(const char *id)
{
    if (s_root == NULL || id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const game_t *game = NULL;
    for (int i = 0; i < s_game_count; i++) {
        if (strcmp(s_games[i]->id, id) == 0) {
            game = s_games[i];
            break;
        }
    }
    if (game == NULL) {
        ESP_LOGE(TAG, "没有注册名为 %s 的游戏", id);
        return ESP_ERR_NOT_FOUND;
    }

    /* 先把上一个页面清掉（可能是主页，也可能是上一个游戏），防止控件泄漏。
     * 这里不调用 game_manager_back()，避免多建一次主页紧接着又删掉。 */
    if (s_current != NULL) {
        if (s_current->deinit) {
            s_current->deinit((game_t *)s_current);
        }
        s_current = NULL;
    }
    if (s_page != NULL) {
        lv_obj_delete(s_page);
        s_page = NULL;
    }

    /* 游戏页面容器：游戏把自己的控件建在它下面 */
    s_page = lv_obj_create(s_root);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_page);
    lv_obj_set_style_pad_all(s_page, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_page, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_page, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    /* 填上下文并交给游戏去建界面 */
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.parent       = s_page;
    s_ctx.cn_font      = s_cn_font;
    s_ctx.sd_ready     = save_service_is_available();
    s_ctx.request_exit = game_manager_back;

    esp_err_t err = game->init((game_t *)game, &s_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "游戏 %s 初始化失败: %s", id, esp_err_to_name(err));
        /* 失败就整体回滚到主页 */
        if (game->deinit) {
            game->deinit((game_t *)game);
        }
        page_switch(make_home_page);
        s_current = NULL;
        return err;
    }

    s_current = game;
    s_last_tick = lv_tick_get();
    ESP_LOGI(TAG, "进入游戏: %s", game->title ? game->title : id);
    return ESP_OK;
}

void game_manager_back(void)
{
    if (s_current != NULL) {
        if (s_current->deinit) {
            s_current->deinit((game_t *)s_current);     /* 游戏释放自己的资源 */
        }
        s_current = NULL;
    }

    /* 回主页：整页重建（顺带把游戏页面上的所有控件都删掉） */
    page_switch(make_home_page);
}

void game_manager_update(void)
{
    if (s_current == NULL || s_current->update == NULL) {
        s_last_tick = lv_tick_get();
        return;
    }

    uint32_t now     = lv_tick_get();
    uint32_t elapsed = now - s_last_tick;
    s_last_tick = now;

    s_current->update((game_t *)s_current, elapsed);
}

bool game_manager_post_key(game_key_t key)
{
    if (s_current == NULL || s_current->on_key == NULL) {
        return false;
    }
    return s_current->on_key((game_t *)s_current, key);
}

const lv_font_t *game_manager_cn_font(void)
{
    return s_cn_font;
}

bool game_manager_in_game(void)
{
    return (s_current != NULL);
}

int game_manager_get_count(void)
{
    return s_game_count;
}

const game_t *game_manager_get(int index)
{
    if (index < 0 || index >= s_game_count) {
        return NULL;
    }
    return s_games[index];
}

void game_manager_set_status(const char *text)
{
    if (text == NULL) {
        return;
    }
    launcher_ui_set_status(text);
}
