/**
 * @file    app_splash.c
 * @brief   开机动画实现：播动画源（钩子）或退回内置刷色自检
 *
 * 说明：
 *   - 动画期间用 lcd_lvgl_pause(true) 暂停 LVGL 刷新，避免直接写屏的画面被
 *     LVGL 的初始刷新覆盖；播完再恢复。
 *   - 只依赖 my_drivers 暴露的 lcd_* 接口，不碰 bsp/分区/寄存器。
 */

#include "app_splash.h"

#include "my_drivers/lcd.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_splash";

/** 默认播放总时长（毫秒） */
#define SPLASH_DEFAULT_TOTAL_MS   3000
/** 内置自检每一色的停留时间（毫秒） */
#define SPLASH_BUILTIN_STEP_MS    400

/* ============================ 内部状态（全部 static） ============================ */

static const app_splash_source_t *s_source = NULL;

/* ============================ 内置自检（原屏幕驱动的开机自检搬到这里） ============================ */

/**
 * @brief  内置刷色自检：白 -> 红 -> 绿 -> 蓝 -> 黑
 * @note   只要能看到颜色变化，就说明 SPI / CS / 背光 / 面板 这条链路是通的；
 *         如果一直是黑的或一直不动，问题在硬件链路上（看串口日志）。
 */
static void splash_builtin_test(void)
{
    static const struct {
        uint16_t    color;
        const char *name;
    } seq[] = {
        { 0xFFFF, "白" },
        { 0xF800, "红" },
        { 0x07E0, "绿" },
        { 0x001F, "蓝" },
        { 0x0000, "黑" },
    };

    ESP_LOGI(TAG, "==== 开机自检（无动画源，刷色确认屏链路）====");
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        ESP_LOGI(TAG, "  显示 %s 色 (0x%04X)", seq[i].name, seq[i].color);
        lcd_set_color(seq[i].color);
        vTaskDelay(pdMS_TO_TICKS(SPLASH_BUILTIN_STEP_MS));
    }
    ESP_LOGI(TAG, "==== 开机自检结束 ====");
}

/* ============================ 播放动画源 ============================ */

/**
 * @brief  播放注册的动画源
 * @return ESP_OK 播放正常结束；其他为失败原因（调用方会退回内置自检）
 */
static esp_err_t splash_play_source(uint32_t max_ms)
{
    const app_splash_source_t *src = s_source;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(max_ms);

    ESP_LOGI(TAG, "播放开机动画源: %s", src->name ? src->name : "(未命名)");

    esp_err_t err = src->open(src->ctx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "动画源打开失败: %s", esp_err_to_name(err));
        return err;
    }

    bool jpeg_warned = false;
    int  frames      = 0;
    while (xTaskGetTickCount() < deadline) {
        app_splash_frame_t frame = { 0 };

        int got = src->read_frame(src->ctx, &frame);
        if (got == 0) {
            ESP_LOGI(TAG, "动画源播放结束（共 %d 帧）", frames);
            break;
        }
        if (got < 0) {
            ESP_LOGW(TAG, "动画源读帧出错，播放中止（已播 %d 帧）", frames);
            if (src->close) {
                src->close(src->ctx);
            }
            return ESP_FAIL;
        }

        if (frame.format == APP_SPLASH_FMT_RGB565 && frame.data && frame.width && frame.height) {
            if (lcd_draw_bitmap(0, 0, frame.width, frame.height, frame.data) != ESP_OK) {
                ESP_LOGW(TAG, "刷帧失败，播放中止");
                break;
            }
        } else if (frame.format == APP_SPLASH_FMT_JPEG) {
            /* JPEG 帧需要先解码：接上 esp_jpeg 后把结果按 RGB565 交回来即可
             * （示例见 app_splash.h 文件头注释） */
            if (!jpeg_warned) {
                ESP_LOGW(TAG, "收到 JPEG 帧但当前未接入解码器，停止播放动画源");
                jpeg_warned = true;
            }
            break;
        } else {
            ESP_LOGW(TAG, "帧数据非法（format=%d, %ux%u, len=%u）",
                     (int)frame.format, frame.width, frame.height, (unsigned)frame.len);
            break;
        }

        frames++;
        vTaskDelay(pdMS_TO_TICKS(frame.duration_ms ? frame.duration_ms
                                                   : APP_SPLASH_FRAME_MS_DEFAULT));
    }

    if (src->close) {
        src->close(src->ctx);
    }
    return (frames > 0) ? ESP_OK : ESP_FAIL;
}

/* ============================ 对外接口 ============================ */

esp_err_t app_splash_register_source(const app_splash_source_t *source)
{
    if (source == NULL) {
        s_source = NULL;                 /* 取消注册，退回内置自检 */
        return ESP_OK;
    }
    if (source->open == NULL || source->read_frame == NULL) {
        ESP_LOGE(TAG, "动画源缺少 open/read_frame 回调");
        return ESP_ERR_INVALID_ARG;
    }

    s_source = source;
    ESP_LOGI(TAG, "已注册开机动画源: %s", source->name ? source->name : "(未命名)");
    return ESP_OK;
}

esp_err_t app_splash_run(uint32_t max_ms)
{
    if (lcd_get_panel() == NULL) {
        ESP_LOGE(TAG, "屏幕未就绪，跳过开机动画");
        return ESP_ERR_INVALID_STATE;
    }
    if (max_ms == 0) {
        max_ms = SPLASH_DEFAULT_TOTAL_MS;
    }

    /* 直接写屏期间暂停 LVGL，避免画面被 LVGL 刷新覆盖 */
    bool paused = (lcd_lvgl_pause(true) == ESP_OK);

    esp_err_t ret = ESP_OK;
    if (s_source != NULL) {
        if (splash_play_source(max_ms) != ESP_OK) {
            ESP_LOGW(TAG, "动画源不可用，退回内置自检");
            splash_builtin_test();
        }
    } else {
        splash_builtin_test();
    }

    if (paused) {
        lcd_lvgl_pause(false);
    }
    return ret;
}
