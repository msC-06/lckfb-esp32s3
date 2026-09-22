# my_drivers 组件：板上外部器件驱动（依赖 bsp 提供的底层外设接口）
#   board/        聚合层：board_init() 一次完成所有硬件初始化
#   pca9557/      IO 扩展芯片（LCD_CS / PA_EN / DVP_PWDN）
#   audio/        ES8311 喇叭 + ES7210 麦克风（I2S）
#   audio/        audio_recorder.c：录音器（PSRAM 缓冲，16k/16bit/单声道）
#   camera/       GC0308 DVP 摄像头
#   sdcard/       TF 卡 + FAT 文件系统挂载（不自动格式化）
#   font_hzk16/   HZK16 中文字库（hzk16_unicode_table.c 由脚本生成）
#   net/          网络服务（对 bsp_wifi 的薄封装，给 app/main 用）
idf_component_register(
    SRCS
        "board/board.c"
        "qmi8658.c"
        "lcd.c"
        "pca9557/pca9557.c"
        "audio/audio.c"
        "audio/audio_recorder.c"
        "camera/camera.c"
        "sdcard/sdcard.c"
        "net/net.c"
        "font_hzk16/hzk16.c"
        "font_hzk16/hzk16_lvgl.c"
        "font_hzk16/hzk16_unicode_table.c"
    INCLUDE_DIRS
        ".."
    PRIV_REQUIRES
        driver
        bsp
        heap
        esp_partition
        esp_driver_sdmmc
        esp_driver_sdspi
        esp_driver_spi
        sdmmc
        fatfs
        vfs
    REQUIRES
        esp_common
        esp_lcd
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2s
        esp_lvgl_port
        esp_lcd_touch
        esp_lcd_touch_ft5x06
        espressif__esp_codec_dev
        espressif__esp32-camera
)
