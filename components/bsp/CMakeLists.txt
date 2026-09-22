# bsp 组件：ESP32-S3 片上外设底层驱动
# 只放芯片自身的外设（I2C/SPI/GPIO/SDMMC/WiFi），只操作 ESP-IDF 原生 API，
# 不包含任何外部芯片的业务逻辑（外部器件驱动都在 my_drivers）
#   bsp_gpio.c   录音按键 GPIO0（输入 + 上拉 + 读电平）
idf_component_register(
    SRCS
        "bsp_iic.c"
        "bsp_spi.c"
        "bsp_gpio.c"
        "bsp_sdmmc.c"
        "bsp_sdspi.c"
        "bsp_wifi.c"
    INCLUDE_DIRS
        ".."
    REQUIRES
        esp_common
        esp_wifi
        esp_driver_gpio
        esp_driver_sdmmc
        esp_driver_sdspi
        esp_driver_spi
        sdmmc
    PRIV_REQUIRES
        driver
        esp_event
        esp_netif
        nvs_flash
)
