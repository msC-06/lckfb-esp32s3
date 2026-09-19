/**
 * @file    app_ui.h
 * @brief   应用层界面接口（app 组件对外只暴露这两个函数）
 *
 * 分层约定：
 *   app 层 -> 只调用 my_drivers 暴露的外设接口（屏幕、中文字库等），
 *             不直接碰 bsp 的底层外设 API，也不直接读 flash 分区。
 */

#ifndef __APP_UI_H
#define __APP_UI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  启动界面：初始化中文字库并创建 LVGL 页面
 *
 * @note   调用前需要已经完成 LVGL 与液晶屏初始化（my_drivers 的 lcd_init()，
 *         正常流程里由 board_init() 统一完成）。
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 显示设备未就绪；
 *         ESP_ERR_TIMEOUT 拿不到 LVGL 锁
 */
esp_err_t app_ui_start(void);

/**
 * @brief  更新界面上的状态文字（例如 WiFi 连接状态），可在任意任务里调用
 *
 * @param  text  UTF-8 文本，内部会拷贝一份，调用后可以释放
 */
void app_ui_set_status(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* __APP_UI_H */
