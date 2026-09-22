/**
 * @file    bsp_gpio.h
 * @brief   ESP32-S3 片上 GPIO 输入驱动（录音按键）
 *
 * 分层：bsp 层（只操作 ESP-IDF 原生 gpio 驱动），不依赖任何上层。
 *
 * 硬件：
 *   - 录音按键接在 GPIO0（立创·实战派 ESP32-S3 板载 BOOT 键），
 *     按下为低电平，需开启内部上拉；
 *   - 本模块只做“配置 + 读电平”，消抖与状态机在 app 层的 app_key 模块里做，
 *     这样底层保持简单、可复用。
 */

#ifndef __BSP_GPIO_H
#define __BSP_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 录音按键使用的 GPIO（按下为低电平） */
#define BSP_KEY_GPIO                GPIO_NUM_0
/** 按键按下的电平（低电平有效） */
#define BSP_KEY_ACTIVE_LEVEL        (0)
/** 按键松开时的电平 */
#define BSP_KEY_INACTIVE_LEVEL      (1)

/**
 * @brief  初始化录音按键 GPIO（输入 + 内部上拉 + 关闭中断）
 *
 * @note   可重复调用，重复调用直接返回 ESP_OK。
 * @return ESP_OK 成功；其它为 gpio_config() 的错误码
 */
esp_err_t bsp_gpio_key_init(void);

/**
 * @brief  按键是否已初始化
 */
bool bsp_gpio_key_is_ready(void);

/**
 * @brief  读取按键当前电平（0 = 按下，1 = 松开）
 *
 * @note   未初始化时返回 BSP_KEY_INACTIVE_LEVEL，避免误判。
 */
int bsp_gpio_key_get_level(void);

/**
 * @brief  按键当前是否处于按下状态（不做消抖，供 app 层状态机使用）
 */
bool bsp_gpio_key_is_pressed(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_GPIO_H */
