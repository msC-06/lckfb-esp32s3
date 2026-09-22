/**
 * @file    app_wifi.h
 * @brief   WiFi（STA）连接管理任务
 *
 * 分层：app 层。底层 WiFi 射频属于 bsp（bsp_wifi），
 *       app 层统一通过 my_drivers 的 net_* 接口使用，不直接调 esp_wifi。
 *
 * 行为：
 *   1. net_init() 初始化 STA；SSID/密码取自 app_config.h 的宏；
 *   2. 连接成功后把 IP 投递到界面顶部状态栏；
 *   3. 断线后每 5 秒重连一次（bsp_wifi 自身也开了自动重连，这里是兜底）；
 *   4. 提供 app_wifi_wait_connected() 给网络任务在发请求前等待联网。
 */

#ifndef __APP_WIFI_H
#define __APP_WIFI_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 WiFi（STA 模式），重复调用安全
 * @return ESP_OK 成功；其它为 bsp_wifi 的错误码
 */
esp_err_t app_wifi_init(void);

/**
 * @brief  WiFi 管理任务（Core 0，优先级 5）
 * @param  arg 未使用
 * @note   由 app_start() 用 xTaskCreatePinnedToCore() 创建
 */
void app_wifi_task(void *arg);

/**
 * @brief  当前是否已连接并拿到 IP
 */
bool app_wifi_is_connected(void);

/**
 * @brief  等待联网（在其它任务里调用，用于发 HTTP 请求前等 WiFi）
 *
 * @param  timeout_ms 最长等待时间
 * @return true 已连接；false 超时仍未连接
 */
bool app_wifi_wait_connected(uint32_t timeout_ms);

/**
 * @brief  当前 IP 字符串（未连接返回 "0.0.0.0"）
 */
const char *app_wifi_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WIFI_H */
