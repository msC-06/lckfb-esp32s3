/**
 * @file    launcher_ui.h
 * @brief   游戏选择主页（LVGL 页面）：游戏图标 + 中文名称列表
 *
 * 分层：app 层界面模块。中文用 my_drivers 的 HZK16 字库渲染，
 *       游戏列表来自 game_manager 的注册表（新增游戏不用改本文件）。
 */

#ifndef __LAUNCHER_UI_H
#define __LAUNCHER_UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  创建主页页面
 *
 * @param  parent   父对象（管理器提供的页面容器）
 * @param  cn_font  中文字体（HZK16），可为 NULL（则用 LVGL 默认字体）
 * @return 页面根对象；失败返回 NULL
 * @note   由 game_manager 调用，切页时整页会被删除
 */
lv_obj_t *launcher_ui_create(lv_obj_t *parent, const lv_font_t *cn_font);

/**
 * @brief  更新主页底部状态栏文字（线程安全，可在任意任务调用）
 *
 * @param  text UTF-8 文本（内部会拷贝保存，页面重建后仍会显示）
 * @note   当前不在主页时只记录文字，等回到主页再显示
 */
void launcher_ui_set_status(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __LAUNCHER_UI_H */
