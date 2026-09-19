/**
 * @file    bsp_sdmmc.h
 * @brief   SDMMC 主机/槽位底层配置（片上外设，只配置引脚与主机参数）
 *
 * 分层：本文件属于 bsp（ESP32-S3 的 SDMMC 外设），只做“引脚 + 主机参数”配置，
 *       卡初始化、文件系统挂载等业务逻辑在 my_drivers/sdcard 里。
 *
 * 板载 TF 卡走 1 线 SDIO 模式（CLK=47 CMD=48 D0=21），需要打开内部上拉。
 */

#ifndef __BSP_SDMMC_H
#define __BSP_SDMMC_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 板载引脚（与原理图/旧工程一致） ========== */
#define BSP_SDMMC_CLK_IO        GPIO_NUM_47
#define BSP_SDMMC_CMD_IO        GPIO_NUM_48
#define BSP_SDMMC_D0_IO         GPIO_NUM_21

/**
 * @brief  取出 SDMMC 主机配置（SDMMC_HOST_DEFAULT，20MHz）
 * @param  host 输出参数
 */
void bsp_sdmmc_get_host(sdmmc_host_t *host);

/**
 * @brief  取出 SDMMC 槽位配置（1 线模式 + 板载引脚 + 内部上拉）
 * @param  slot 输出参数
 */
void bsp_sdmmc_get_slot(sdmmc_slot_config_t *slot);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_SDMMC_H */
