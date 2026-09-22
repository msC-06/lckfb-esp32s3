/**
 * @file    game_2048.h
 * @brief   2048 游戏模块（实现 game_if.h 的统一接口）
 *
 * 用法（在 main.c 里注册即可，主页会自动出现它的图标）：
 *      game_manager_register(game_2048_get());
 */

#ifndef __GAME_2048_H
#define __GAME_2048_H

#include "game_launcher/game_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  获取 2048 游戏对象（静态实例，注册到游戏管理器）
 */
const game_t *game_2048_get(void);

#ifdef __cplusplus
}
#endif

#endif /* __GAME_2048_H */
