/**
 * @file    board.c
 * @brief   板级聚合层实现：按顺序完成所有硬件初始化
 */

#include "board.h"

/* bsp：片上外设 */
#include "bsp/bsp_iic.h"
#include "bsp/bsp_spi.h"

/* my_drivers：外部器件 */
#include "my_drivers/pca9557/pca9557.h"
#include "my_drivers/qmi8658.h"
#include "my_drivers/lcd.h"
#include "my_drivers/sdcard/sdcard.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "board";

/* ============================ 内部状态（全部 static） ============================ */

static bool s_initialized = false;

/* ============================ 对外接口 ============================ */

esp_err_t board_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "===== 板级初始化开始 =====");

    /* 1. I2C 总线（PCA9557 / QMI8658 / 触摸 / 音频编解码器 / 摄像头 SCCB 都用它） */
    ESP_RETURN_ON_ERROR(bsp_i2c_master_init(), TAG, "I2C 总线初始化失败");
    ESP_LOGI(TAG, "I2C 总线就绪");

    /* 2. PCA9557：LCD_CS=1(不选中) PA_EN=0(功放关) DVP_PWDN=1(摄像头断电)，IO3~IO7 保持输入 */
    ESP_RETURN_ON_ERROR(pca9557_init(), TAG, "PCA9557 初始化失败");
    ESP_LOGI(TAG, "IO 扩展(PCA9557)就绪");

    /* 3. 姿态传感器（可选外设，失败不影响其它功能） */
    esp_err_t err = qmi8658_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 不可用(%s)，跳过", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "姿态传感器就绪");
    }

    /* 4. SPI 总线（液晶屏） */
    ESP_RETURN_ON_ERROR(bsp_spi_init(), TAG, "SPI 总线初始化失败");
    ESP_LOGI(TAG, "SPI 总线就绪");

    /* 5. 液晶屏 + 触摸 + LVGL（内部会拉低 LCD_CS、打开背光、先刷白） */
    lcd_init();
    if (lcd_get_display() == NULL) {
        ESP_LOGE(TAG, "液晶屏未就绪，板级初始化失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "液晶屏/LVGL 就绪");

    /* 6. TF 卡（可选外设：没插卡就不挂载，也绝不格式化） */
    err = sdcard_mount();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TF 卡已挂载到 %s", sdcard_get_mount_point());
    } else {
        ESP_LOGW(TAG, "TF 卡未挂载(%s)，/sdcard 本次不可用", esp_err_to_name(err));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "===== 板级初始化完成 =====");
    return ESP_OK;
}

bool board_is_initialized(void)
{
    return s_initialized;
}
