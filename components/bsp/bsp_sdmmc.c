/**
 * @file    bsp_sdmmc.c
 * @brief   SDMMC 主机/槽位底层配置实现（只填结构体，不做卡操作）
 */

#include "bsp_sdmmc.h"

#include "esp_log.h"

static const char *TAG = "bsp_sdmmc";

void bsp_sdmmc_get_host(sdmmc_host_t *host)
{
    if (host == NULL) {
        return;
    }
    /* 默认 SDMMC 主机：20MHz，使用 SDMMC 外设（非 SPI 模式） */
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
}

void bsp_sdmmc_get_slot(sdmmc_slot_config_t *slot)
{
    if (slot == NULL) {
        return;
    }
    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();

    slot->width = 1;                                    /* 板载只有 1 根数据线 */
    slot->clk   = BSP_SDMMC_CLK_IO;
    slot->cmd   = BSP_SDMMC_CMD_IO;
    slot->d0    = BSP_SDMMC_D0_IO;
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;     /* 打开内部上拉（板上有 10k 外部上拉更好） */

    ESP_LOGD(TAG, "SDMMC 槽位: CLK=%d CMD=%d D0=%d 宽度=1", BSP_SDMMC_CLK_IO,
             BSP_SDMMC_CMD_IO, BSP_SDMMC_D0_IO);
}
