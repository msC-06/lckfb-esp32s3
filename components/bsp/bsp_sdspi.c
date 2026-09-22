/**
 * @file    bsp_sdspi.c
 * @brief   TF 卡 SPI 方式接线的底层配置实现
 */

#include "bsp_sdspi.h"

#include "esp_log.h"

static const char *TAG = "bsp_sdspi";

/* 总线只需要初始化一次 */
static bool s_bus_inited = false;

esp_err_t bsp_sdspi_bus_init(void)
{
    if (s_bus_inited) {
        return ESP_OK;
    }

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num     = BSP_SDSPI_MOSI_IO,
        .miso_io_num     = BSP_SDSPI_MISO_IO,
        .sclk_io_num     = BSP_SDSPI_CLK_IO,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(BSP_SDSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF 卡 SPI 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    s_bus_inited = true;
    ESP_LOGI(TAG, "TF 卡 SPI 总线就绪（CLK=%d MOSI=%d MISO=%d CS=%d）",
             BSP_SDSPI_CLK_IO, BSP_SDSPI_MOSI_IO, BSP_SDSPI_MISO_IO, BSP_SDSPI_CS_IO);
    return ESP_OK;
}

void bsp_sdspi_get_host(sdmmc_host_t *host)
{
    if (host == NULL) {
        return;
    }
    *host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    host->slot = BSP_SDSPI_HOST;    /* 用哪条 SPI 总线 */
}

void bsp_sdspi_get_slot(sdspi_device_config_t *slot)
{
    if (slot == NULL) {
        return;
    }
    *slot = (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
    slot->host_id = BSP_SDSPI_HOST;
    slot->gpio_cs = BSP_SDSPI_CS_IO;
}
