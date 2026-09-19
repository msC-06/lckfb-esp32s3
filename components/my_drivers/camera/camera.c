/**
 * @file    camera.c
 * @brief   板载 DVP 摄像头驱动实现（GC0308 + esp32-camera）
 */

#include "camera.h"

#include "bsp/bsp_iic.h"
#include "my_drivers/pca9557/pca9557.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "camera";

/* ============================ 引脚定义（与旧工程一致） ============================ */

#define CAMERA_PIN_XCLK        (5)
#define CAMERA_PIN_PCLK        (7)
#define CAMERA_PIN_VSYNC       (3)
#define CAMERA_PIN_HREF        (46)

#define CAMERA_PIN_D0          (16)
#define CAMERA_PIN_D1          (18)
#define CAMERA_PIN_D2          (8)
#define CAMERA_PIN_D3          (17)
#define CAMERA_PIN_D4          (15)
#define CAMERA_PIN_D5          (6)
#define CAMERA_PIN_D6          (4)
#define CAMERA_PIN_D7          (9)

/* SCCB 直接用板载 I2C（GPIO1/GPIO2），所以这里传 -1 让驱动复用已有总线 */
#define CAMERA_PIN_SIOD        (-1)
#define CAMERA_PIN_SIOC        (2)

#define CAMERA_PIN_PWDN        (-1)    /* 摄像头掉电由 PCA9557 的 DVP_PWDN 控制 */
#define CAMERA_PIN_RESET       (-1)

/* XCLK 由 LEDC 生成（沿用旧工程做法） */
#define CAMERA_LEDC_TIMER      LEDC_TIMER_1
#define CAMERA_LEDC_CHANNEL    LEDC_CHANNEL_1

/* ============================ 内部状态（全部 static） ============================ */

static bool           s_initialized = false;
static TaskHandle_t   s_preview_task = NULL;
static volatile bool  s_preview_run = false;
static camera_frame_cb_t s_frame_cb = NULL;
static uint32_t       s_preview_fps = 0;

/* ============================ 供电 ============================ */

esp_err_t camera_power(bool on)
{
    /* DVP_PWDN = 0 时摄像头工作，= 1 时掉电 */
    return pca9557_dvp_pwdn(on ? 0 : 1);
}

/* ============================ 初始化 ============================ */

esp_err_t camera_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "摄像头已经初始化过了");
        return ESP_OK;
    }
    if (bsp_i2c_get_bus_handle() == NULL) {
        ESP_LOGE(TAG, "I2C 总线未初始化（SCCB 复用该总线），请先调用 bsp_i2c_master_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 给摄像头上电 */
    ESP_RETURN_ON_ERROR(camera_power(true), TAG, "摄像头上电失败");
    vTaskDelay(pdMS_TO_TICKS(50));      /* 等传感器内部稳压稳定 */

    /* 2. 配置摄像头 */
    camera_config_t config = {
        .ledc_channel    = CAMERA_LEDC_CHANNEL,
        .ledc_timer      = CAMERA_LEDC_TIMER,
        .pin_d0          = CAMERA_PIN_D0,
        .pin_d1          = CAMERA_PIN_D1,
        .pin_d2          = CAMERA_PIN_D2,
        .pin_d3          = CAMERA_PIN_D3,
        .pin_d4          = CAMERA_PIN_D4,
        .pin_d5          = CAMERA_PIN_D5,
        .pin_d6          = CAMERA_PIN_D6,
        .pin_d7          = CAMERA_PIN_D7,
        .pin_xclk        = CAMERA_PIN_XCLK,
        .pin_pclk        = CAMERA_PIN_PCLK,
        .pin_vsync       = CAMERA_PIN_VSYNC,
        .pin_href        = CAMERA_PIN_HREF,
        .pin_sccb_sda    = CAMERA_PIN_SIOD,        /* -1：复用已经初始化好的 I2C 总线 */
        .pin_sccb_scl    = CAMERA_PIN_SIOC,
        .sccb_i2c_port   = I2C_MASTER_NUM,          /* 与 bsp_i2c 使用同一个端口 */
        .pin_pwdn        = CAMERA_PIN_PWDN,
        .pin_reset       = CAMERA_PIN_RESET,
        .xclk_freq_hz    = CAMERA_XCLK_FREQ_HZ,
        .pixel_format    = CAMERA_DEFAULT_FORMAT,
        .frame_size      = CAMERA_DEFAULT_FRAMESIZE,
        .jpeg_quality    = CAMERA_DEFAULT_QUALITY,
        .fb_count        = 2,                       /* 双缓冲，取图更顺畅 */
        .fb_location     = CAMERA_FB_IN_PSRAM,
        .grab_mode       = CAMERA_GRAB_LATEST,      /* 预览要“最新帧”，不要排队积压 */
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init 失败: 0x%x（检查排线/SCCB 地址/供电）", err);
        camera_power(false);
        return err;
    }

    /* 3. 按传感器型号做镜像修正（与旧工程一致：GC0308 需要水平镜像） */
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGW(TAG, "获取 sensor 句柄失败");
    } else {
        ESP_LOGI(TAG, "传感器 PID=0x%04X VER=0x%02X MIDH=0x%02X MIDL=0x%02X",
                 s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL);
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 1);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "摄像头初始化完成（%dx%d, %s）",
             resolution[config.frame_size].width, resolution[config.frame_size].height,
             config.pixel_format == PIXFORMAT_RGB565 ? "RGB565" : "其他格式");
    return ESP_OK;
}

esp_err_t camera_deinit(void)
{
    camera_stop_preview();

    if (!s_initialized) {
        return ESP_OK;
    }
    esp_err_t err = esp_camera_deinit();
    s_initialized = false;
    camera_power(false);
    ESP_LOGI(TAG, "摄像头已关闭");
    return err;
}

bool camera_is_initialized(void)
{
    return s_initialized;
}

/* ============================ 取图 ============================ */

camera_fb_t *camera_get_frame(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "摄像头未初始化");
        return NULL;
    }
    return esp_camera_fb_get();
}

void camera_return_frame(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

/* ============================ 预览任务 ============================ */

static void camera_preview_task(void *arg)
{
    TickType_t period = (s_preview_fps > 0) ? pdMS_TO_TICKS(1000 / s_preview_fps) : 0;

    while (s_preview_run) {
        TickType_t start = xTaskGetTickCount();

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            if (s_frame_cb) {
                s_frame_cb(fb);
            }
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGW(TAG, "取帧失败");
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (period > 0) {
            TickType_t used = xTaskGetTickCount() - start;
            if (used < period) {
                vTaskDelay(period - used);
            }
        }
    }

    s_preview_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t camera_start_preview(camera_frame_cb_t cb, uint32_t fps)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "摄像头未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_preview_task) {
        ESP_LOGW(TAG, "预览已经在运行");
        return ESP_OK;
    }

    s_frame_cb    = cb;
    s_preview_fps = fps;
    s_preview_run = true;

    BaseType_t res = xTaskCreatePinnedToCore(camera_preview_task, "cam_prev",
                                             4 * 1024, NULL, 5, &s_preview_task, 1);
    if (res != pdPASS) {
        s_preview_run = false;
        s_preview_task = NULL;
        ESP_LOGE(TAG, "创建预览任务失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "预览已启动（上限 %u fps）", (unsigned)fps);
    return ESP_OK;
}

esp_err_t camera_stop_preview(void)
{
    if (s_preview_task == NULL) {
        return ESP_OK;
    }

    s_preview_run = false;
    for (int i = 0; i < 100 && s_preview_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_frame_cb = NULL;
    ESP_LOGI(TAG, "预览已停止");
    return ESP_OK;
}

/* ============================ 传感器参数 ============================ */

sensor_t *camera_get_sensor(void)
{
    if (!s_initialized) {
        return NULL;
    }
    return esp_camera_sensor_get();
}

#define CAMERA_SET_FUNC(_func, _val, _name)                     \
    do {                                                        \
        sensor_t *s = camera_get_sensor();                      \
        if (s == NULL) return ESP_ERR_INVALID_STATE;            \
        ESP_RETURN_ON_FALSE(s->_func != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "%s 不支持", _name); \
        ESP_RETURN_ON_FALSE(s->_func(s, _val) == 0, ESP_FAIL, TAG, "%s 设置失败", _name);       \
        return ESP_OK;                                          \
    } while (0)

esp_err_t camera_set_framesize(framesize_t framesize)
{
    CAMERA_SET_FUNC(set_framesize, framesize, "分辨率");
}

esp_err_t camera_set_hmirror(bool enable)
{
    CAMERA_SET_FUNC(set_hmirror, enable ? 1 : 0, "水平镜像");
}

esp_err_t camera_set_vflip(bool enable)
{
    CAMERA_SET_FUNC(set_vflip, enable ? 1 : 0, "垂直翻转");
}

esp_err_t camera_set_brightness(int level)
{
    CAMERA_SET_FUNC(set_brightness, level, "亮度");
}

esp_err_t camera_set_contrast(int level)
{
    CAMERA_SET_FUNC(set_contrast, level, "对比度");
}

esp_err_t camera_set_saturation(int level)
{
    CAMERA_SET_FUNC(set_saturation, level, "饱和度");
}
