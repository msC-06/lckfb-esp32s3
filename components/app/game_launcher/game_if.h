/**
 * @file    game_if.h
 * @brief   游戏统一抽象接口（所有游戏都实现这一套，方便新增游戏）
 *
 * 分层：app 层内部接口。游戏实现只允许调用：
 *   - my_drivers 暴露的接口（lcd / font_hzk16 / sdcard / board 等）
 *   - LVGL 与 esp_lvgl_port
 *   - app 层内的公共模块（存档服务、游戏管理器）
 * 严禁直接操作 bsp 的底层外设。
 *
 * 新增一个游戏只要三步：
 *   1. 在 app/games/ 下新建目录实现 struct game 的 4 个回调；
 *   2. 提供一个 `const game_t *game_xxx_get(void);`
 *   3. 在 main.c 里 game_manager_register(game_xxx_get());（管理器会自动出现在主页列表里）
 */

#ifndef __GAME_IF_H
#define __GAME_IF_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct game game_t;

/* ============================ 运行上下文 ============================ */

/** 由游戏管理器填充，游戏只读使用 */
typedef struct {
    lv_obj_t        *parent;        /*!< 游戏页面根对象，游戏所有控件都建在它下面 */
    const lv_font_t *cn_font;       /*!< 中文字体（HZK16，已设置好西文回退） */
    bool             sd_ready;      /*!< 存档是否可用（TF 卡挂载成功） */
    void           (*request_exit)(void);  /*!< 请求返回主页（供游戏内“返回”按钮调用） */
} game_ctx_t;

/* ============================ 按键（预留） ============================ */

/** 统一按键定义：现在主要用屏幕按钮，将来接实体按键/摇杆时直接复用 */
typedef enum {
    GAME_KEY_UP = 0,
    GAME_KEY_DOWN,
    GAME_KEY_LEFT,
    GAME_KEY_RIGHT,
    GAME_KEY_OK,
    GAME_KEY_BACK,
} game_key_t;

/* ============================ 游戏描述 + 生命周期 ============================ */

struct game {
    /* ---- 静态描述（主页列表用） ---- */
    const char *id;             /*!< 唯一标识，如 "2048"，用于 game_manager_enter() */
    const char *title;          /*!< 中文名（HZK16 渲染），如 "2048" */
    const char *icon_text;      /*!< 图标文字（没有图片资源时用文字图标） */
    uint32_t    icon_color;     /*!< 图标底色（0xRRGGBB） */

    /* ---- 生命周期回调 ---- */

    /**
     * @brief  初始化并创建游戏页面
     * @param  self 游戏对象自身
     * @param  ctx  上下文（parent 已经创建好，直接把控件加在 ctx->parent 下）
     * @return ESP_OK 成功；其它值表示失败，管理器会放弃进入并回到主页
     */
    esp_err_t (*init)(game_t *self, const game_ctx_t *ctx);

    /**
     * @brief  销毁游戏：释放自己申请的资源
     * @note   页面上的 LVGL 控件由管理器统一删除，这里只清理非 LVGL 资源
     *         （例如打开的弹窗句柄、定时器、malloc 的缓冲）。
     */
    void (*deinit)(game_t *self);

    /**
     * @brief  周期更新（可选）：由管理器在 LVGL 任务里按 ~50ms 调用
     * @param  elapsed_ms 距上次调用的毫秒数
     * @note   必须是非阻塞的：文件读写请用存档服务异步接口，不要在这里做耗时操作
     */
    void (*update)(game_t *self, uint32_t elapsed_ms);

    /**
     * @brief  按键事件（可选，预留）：返回 true 表示已处理
     */
    bool (*on_key)(game_t *self, game_key_t key);

    /** 游戏私有数据（可选）：单实例游戏直接用文件内 static 即可，多实例才需要它 */
    void *priv;
};

#ifdef __cplusplus
}
#endif

#endif /* __GAME_IF_H */
