/**
 * @file    app_chat_ui.h
 * @brief   聊天界面（LVGL）：顶部状态栏 + 消息气泡区 + 底部状态行
 *
 * 分层：app 层，调用 my_drivers 的 hzk16 中文字体 + esp_lvgl_port 的锁。
 *
 * 线程模型：
 *   - 界面**只在 LVGL 任务里**被创建和修改：
 *       app_chat_ui_init()     在 app_start() 里调用（内部会取 LVGL 锁）；
 *       消息刷新靠一个 lv_timer（50ms），回调在 LVGL 任务上下文里执行；
 *   - 其它任务（WiFi / 网络 / 音频）不直接碰控件，
 *     统一通过 app_bus_post_chat() 把消息投递到 chat_msg_queue，
 *     由 LVGL 定时器取出来再更新界面，天然避免跨线程操作控件。
 *
 * 显示：
 *   用户消息：右对齐、浅蓝气泡；AI 回复：左对齐、浅灰气泡；
 *   中文用 HZK16 字体（内部自动做 Unicode -> GB2312 取模）。
 */

#ifndef __APP_CHAT_UI_H
#define __APP_CHAT_UI_H

#include "esp_err.h"
#include "lvgl.h"

#include "app_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  创建聊天界面（会新建并加载一个全屏 screen，覆盖原来的界面）
 *
 * @param  cn_font 中文字体（一般是 hzk16_get_lv_font()）；传 NULL 用默认西文字体
 * @return ESP_OK 成功；ESP_ERR_NO_MEM LVGL 内存不足；ESP_ERR_INVALID_STATE 未初始化
 * @note   必须在 LVGL（lcd_init()）初始化完成之后调用
 */
esp_err_t app_chat_ui_init(const lv_font_t *cn_font);

/**
 * @brief  清空聊天区所有气泡
 */
void app_chat_ui_clear(void);

/**
 * @brief  更新底部状态行（线程安全：投递到队列，由 LVGL 定时器刷新）
 */
void app_chat_ui_show_status(const char *text);

/**
 * @brief  更新顶部 WiFi 状态（线程安全）
 */
void app_chat_ui_show_wifi(const char *text);

/**
 * @brief  在聊天区加一条消息气泡（线程安全）
 * @param  is_user true = 用户消息（右对齐），false = AI 回复（左对齐）
 */
void app_chat_ui_add_message(bool is_user, const char *text);

/**
 * @brief  界面是否已经创建
 */
bool app_chat_ui_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CHAT_UI_H */
