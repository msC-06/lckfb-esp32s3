/**
 * @file    board.h
 * @brief   板级聚合层：一次调用完成所有硬件初始化
 *
 * 分层位置：my_drivers 里的“组装层”。它按正确的顺序调用各驱动
 *           （bsp 的 I2C/SPI + my_drivers 的 PCA9557/IMU/屏幕/TF 卡），
 *           这样 main.c 和 app 层都不需要再出现 bsp_* 的调用。
 *
 * 初始化顺序（很重要）：
 *   1. I2C 总线            -> PCA9557 / QMI8658 / 触摸 / ES8311 / ES7210 / 摄像头 SCCB 都挂在上面
 *   2. PCA9557(IO 扩展)    -> LCD_CS / PA_EN / DVP_PWDN
 *   3. QMI8658(可选)       -> 失败只告警
 *   4. SPI 总线            -> 液晶屏
 *   5. 液晶屏 + 触摸 + LVGL -> 内部会拉低 LCD_CS、打开背光、刷色自检
 *   6. TF 卡(可选)         -> 没插卡只告警，不挂载、不格式化
 */

#ifndef __BOARD_H
#define __BOARD_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  板级初始化（可重复调用，第二次直接返回 ESP_OK）
 *
 * @return ESP_OK 关键外设都就绪（液晶屏可用）；
 *         ESP_ERR_INVALID_STATE 重复调用且上次失败；
 *         其它为 I2C/SPI/PCA9557/液晶屏 的失败错误码
 * @note   QMI8658 与 TF 卡属于可选外设，初始化失败不影响返回值。
 */
esp_err_t board_init(void);

/**
 * @brief  板级初始化是否成功过
 */
bool board_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_H */
