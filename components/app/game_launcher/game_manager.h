/**
 * @file    game_manager.h
 * @brief   游戏管理器：注册游戏、页面切换与销毁、统一更新循环
 *
 * 职责：
 *   1. 维护游戏注册表（主页列表就是从注册表生成的）；
 *   2. 切换页面：进入游戏时销毁主页控件、退出游戏时销毁游戏控件，避免内存泄漏；
 *   3. 用 LVGL 定时器驱动统一 update 循环（在 LVGL 任务上下文里跑，方便操作控件）；
 *   4. 提供中文字体、存档服务可用状态给各个游戏。
 *
 * 线程说明：本模块所有接口默认在 LVGL 线程里调用（按钮回调、定时器回调、
 *           app_main 初始化阶段都属于该上下文）；只有 game_manager_set_status()
 *           可以从别的任务调用（内部会加 LVGL 锁）。
 */

#ifndef __GAME_MANAGER_H
#define __GAME_MANAGER_H

#include "esp_err.h"
#include "lvgl.h"

#include "game_launcher/game_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 最多支持多少个游戏（主页列表容量） */
#define GAME_MANAGER_MAX_GAMES   8

/** update 循环周期（毫秒） */
#define GAME_MANAGER_TICK_MS     50

/**
 * @brief  初始化管理器：初始化中文字库、创建根容器、启动 update 定时器、启动存档服务
 * @return ESP_OK 成功；其它为失败原因
 * @note   必须在液晶屏/LVGL 初始化完成之后调用（board_init() 之后）
 */
esp_err_t game_manager_init(void);

/**
 * @brief  注册一个游戏（新增游戏就靠它，注册后自动出现在主页列表里）
 * @param  game 游戏描述（生命周期必须是静态的，管理器只保存指针）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 注册表已满；ESP_ERR_INVALID_ARG 参数错
 */
esp_err_t game_manager_register(const game_t *game);

/**
 * @brief  显示游戏选择主页
 */
esp_err_t game_manager_start(void);

/**
 * @brief  进入指定游戏（按 id 查找）
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 没注册该游戏；其它为游戏 init 的失败原因
 */
esp_err_t game_manager_enter(const char *id);

/**
 * @brief  退出当前游戏，回到主页（游戏内“返回”按钮会调它）
 */
void game_manager_back(void);

/**
 * @brief  统一更新循环（由内部 LVGL 定时器调用；也可以手动调用一次做测试）
 */
void game_manager_update(void);

/**
 * @brief  统一按键入口（预留：以后接实体按键/摇杆时调用）
 * @return true 表示当前游戏处理了该按键
 */
bool game_manager_post_key(game_key_t key);

/** 获取中文字体（HZK16），供游戏/页面设置文字字体 */
const lv_font_t *game_manager_cn_font(void);

/** 当前是否在游戏中（false = 在主页） */
bool game_manager_in_game(void);

/* ============================ 注册表查询（主页列表用） ============================ */

/** 已注册的游戏数量 */
int game_manager_get_count(void);

/** 按索引取游戏（越界返回 NULL） */
const game_t *game_manager_get(int index);

/* ============================ 状态栏 ============================ */

/**
 * @brief  更新主页状态栏文字（线程安全：可以在别的任务里调用，例如 WiFi 任务）
 */
void game_manager_set_status(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __GAME_MANAGER_H */
