/**
 * @file    bsp_sdspi.h
 * @brief   TF 卡 SPI 方式接线的底层配置（片上 SPI 外设，可选）
 *
 * 分层：属于 bsp（ESP32-S3 的 SPI 外设），只做“总线 + 引脚”配置，不做卡/文件系统逻辑。
 *
 * ★ 注意：本开发板的 TF 座是接在 **SDMMC(SDIO 1 线)** 上的（CLK=47 / CMD=48 / D0=21，
 *   官方例程也是 SDMMC 方式），并没有按 SPI 四线（CS/MOSI/MISO/CLK）接线。
 *   所以默认走 bsp_sdmmc，本文件只有在“你的板子按 SPI 接线”时才会被用到：
 *       my_drivers/sdcard/sdcard.h 里把 SDCARD_TRANSPORT_SPI 改成 1，
 *       并按实际接线修改下面的引脚宏。
 */

#ifndef __BSP_SDSPI_H
#define __BSP_SDSPI_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== SPI 方式接线时的引脚（按实际板子修改） ========== */
#define BSP_SDSPI_HOST          SPI2_HOST
#define BSP_SDSPI_CLK_IO        GPIO_NUM_47     /* 卡的 CLK */
#define BSP_SDSPI_MOSI_IO       GPIO_NUM_48     /* 卡的 CMD/DI */
#define BSP_SDSPI_MISO_IO       GPIO_NUM_21     /* 卡的 D0/DO */
#define BSP_SDSPI_CS_IO         GPIO_NUM_NC     /* SPI 模式需要一根独立的片选脚 */

/**
 * @brief  初始化 TF 卡用的 SPI 总线（只需调用一次，重复调用直接返回 ESP_OK）
 */
esp_err_t bsp_sdspi_bus_init(void);

/**
 * @brief  取出 SPI 模式的 SD 主机配置（SDSPI_HOST_DEFAULT）
 */
void bsp_sdspi_get_host(sdmmc_host_t *host);

/**
 * @brief  取出 SPI 模式的设备（槽位）配置
 */
void bsp_sdspi_get_slot(sdspi_device_config_t *slot);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_SDSPI_H */
