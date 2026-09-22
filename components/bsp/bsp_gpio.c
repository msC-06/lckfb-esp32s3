/**
 * @file    bsp_gpio.c
 * @brief   片上 GPIO 输入驱动实现（录音按键 GPIO0）
 */

#include "bsp_gpio.h"

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp_gpio";

/* ============================ 内部状态（全部 static） ============================ */

static bool s_key_ready = false;

/* ============================ 对外接口 ============================ */

esp_err_t bsp_gpio_key_init(void)
{
    if (s_key_ready) {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BSP_KEY_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* 按下接地，必须内部上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,      /* 软件定时器轮询，不用中断 */
    };

    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "录音按键 GPIO%d 配置失败", (int)BSP_KEY_GPIO);

    s_key_ready = true;
    ESP_LOGI(TAG, "录音按键就绪：GPIO%d（低电平有效，内部上拉）", (int)BSP_KEY_GPIO);
    return ESP_OK;
}

bool bsp_gpio_key_is_ready(void)
{
    return s_key_ready;
}

int bsp_gpio_key_get_level(void)
{
    if (!s_key_ready) {
        return BSP_KEY_INACTIVE_LEVEL;
    }
    return gpio_get_level(BSP_KEY_GPIO);
}

bool bsp_gpio_key_is_pressed(void)
{
    return bsp_gpio_key_get_level() == BSP_KEY_ACTIVE_LEVEL;
}
