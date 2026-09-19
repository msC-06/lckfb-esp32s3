#ifndef BSP_SPI_H
#define BSP_SPI_H

#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#define BSP_LCD_PIXEL_CLOCK_HZ     (80 * 1000 * 1000)
#define BSP_LCD_SPI_NUM            (SPI3_HOST)
#define BSP_LCD_SPI_MOSI           (GPIO_NUM_40)
#define BSP_LCD_SPI_CLK            (GPIO_NUM_41)
#define BSP_LCD_SPI_CS             (GPIO_NUM_NC)

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_spi_init(void);

#ifdef __cplusplus
}
#endif

#endif // BSP_SPI_H