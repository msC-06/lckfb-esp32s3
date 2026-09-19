/**
 * @file    pca9557.c
 * @brief   PCA9557 IO 扩展芯片驱动实现
 */

#include "pca9557.h"

#include "bsp/bsp_iic.h"

#include "esp_log.h"

static const char *TAG = "pca9557";

/* ============================ 内部状态（全部 static） ============================ */

/** 输出寄存器影子值，避免每次“读-改-写”引起竞态 */
static uint8_t s_output_shadow = PCA9557_OUTPUT_DEFAULT;
static bool    s_initialized   = false;

/* ============================ 内部函数 ============================ */

/**
 * @brief  写一个寄存器
 */
static esp_err_t pca9557_reg_write(uint8_t reg_addr, uint8_t data)
{
    return bsp_i2c_write_reg(PCA9557_I2C_ADDR, reg_addr, &data, 1);
}

/* ============================ 对外接口 ============================ */

esp_err_t pca9557_init(void)
{
    if (bsp_i2c_get_bus_handle() == NULL) {
        ESP_LOGE(TAG, "I2C 总线未初始化，请先调用 bsp_i2c_master_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 输出默认值：LCD_CS=1(不选中) PA_EN=0(功放关) DVP_PWDN=1(摄像头断电) */
    esp_err_t ret = pca9557_reg_write(PCA9557_REG_OUTPUT_PORT, PCA9557_OUTPUT_DEFAULT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写输出寄存器失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_output_shadow = PCA9557_OUTPUT_DEFAULT;

    /* 2. 只把 IO0~IO2 设为输出，其余保持输入（高阻） */
    ret = pca9557_reg_write(PCA9557_REG_CONFIGURATION_PORT, PCA9557_CONFIG_DEFAULT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写配置寄存器失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 极性不反转 */
    ret = pca9557_reg_write(PCA9557_REG_POLARITY_INVERSION_PORT, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "写极性寄存器失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "PCA9557 初始化完成（LCD_CS=1, PA_EN=0, DVP_PWDN=1，IO3~IO7 保持输入）");
    return ESP_OK;
}

esp_err_t pca9557_set_output(uint8_t mask, uint8_t level)
{
    if (mask == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        ESP_LOGW(TAG, "PCA9557 未初始化，自动初始化一次");
        esp_err_t ret = pca9557_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    uint8_t val = level ? (s_output_shadow | mask) : (s_output_shadow & (uint8_t)~mask);

    esp_err_t ret = pca9557_reg_write(PCA9557_REG_OUTPUT_PORT, val);
    if (ret == ESP_OK) {
        s_output_shadow = val;
    } else {
        ESP_LOGE(TAG, "设置输出失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pca9557_set_pin(uint8_t pin_mask, uint8_t level)
{
    return pca9557_set_output(pin_mask, level ? 1 : 0);
}

esp_err_t pca9557_lcd_cs(uint8_t level)
{
    return pca9557_set_pin(PCA9557_PIN_LCD_CS, level);
}

esp_err_t pca9557_pa_en(uint8_t level)
{
    return pca9557_set_pin(PCA9557_PIN_PA_EN, level);
}

esp_err_t pca9557_dvp_pwdn(uint8_t level)
{
    return pca9557_set_pin(PCA9557_PIN_DVP_PWDN, level);
}
