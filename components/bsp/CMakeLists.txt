# bsp 组件：ESP32-S3 片上外设底层驱动
# 只放芯片自身的外设（I2C/SPI/SDMMC/WiFi），只操作 ESP-IDF 原生 API，
# 不包含任何外部芯片的业务逻辑（外部器件驱动都在 my_drivers）
idf_component_register(
    SRCS
        "bsp_iic.c"
        "bsp_spi.c"
        "bsp_sdmmc.c"
        "bsp_wifi.c"
    INCLUDE_DIRS
        ".."
    REQUIRES
        esp_common
        esp_wifi
        esp_driver_sdmmc
        sdmmc
    PRIV_REQUIRES
        driver
        esp_event
        esp_netif
        nvs_flash
)
