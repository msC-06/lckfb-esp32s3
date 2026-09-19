/**
 * @file    main.c
 * @brief   ESP32-S3 程序入口（组装层）
 *
 * 分层后的结构：
 *     main.c  —— 只做“组装”：board_init() -> app_ui_start() -> 启动各个业务任务
 *     components/my_drivers  —— 外部器件驱动 + board 聚合层（内部会调用 bsp）
 *     components/bsp         —— ESP32-S3 片上外设（I2C/SPI/SDMMC/WiFi）
 *     components/app         —— 界面与业务逻辑
 *
 * 注意：main.c 不再逐个初始化外设，全部交给 board_init()（my_drivers 聚合层）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* my_drivers：聚合层 + 各器件驱动 + 网络服务（app/main 不直接碰 bsp） */
#include "my_drivers/board/board.h"
#include "my_drivers/lcd.h"                 /* 摄像头预览需要拿到面板句柄 */
#include "my_drivers/audio/audio.h"
#include "my_drivers/camera/camera.h"
#include "my_drivers/sdcard/sdcard.h"
#include "my_drivers/net/net.h"

/* app：开机动画 + 界面（含 HZK16 中文字体） */
#include "app_splash.h"
#include "app_ui.h"

static const char *TAG = "main";

/* ============================================================
 *  编译期开关
 * ============================================================ */

/** WiFi 演示：初始化 -> 扫描 -> 连接 -> 状态显示到屏幕 */
#define APP_WIFI_DEMO           1
#define APP_WIFI_SSID           "点击复活特蕾西娅"        /* ← 改成你的 WiFi 名称 */
#define APP_WIFI_PASSWORD       "20060308"          /* ← 改成你的 WiFi 密码 */

/** 音频演示：开机放一小段 1kHz 提示音 */
#define APP_AUDIO_DEMO          1

/** 摄像头演示：LCD 实时预览（会占满屏幕，LVGL 界面会被覆盖） */
#define APP_CAMERA_DEMO         0

/* ============================================================
 *  WiFi 演示任务
 * ============================================================ */
static void app_wifi_task(void *arg)
{
#if APP_WIFI_DEMO
    /* 1. 初始化 */
    esp_err_t err = net_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(err));
        app_ui_set_status("WiFi: 初始化失败");
        vTaskDelete(NULL);
        return;
    }
    app_ui_set_status("WiFi: 初始化完成，开始扫描");

    /* 2. 扫描（缓冲区由调用者提供） */
    net_ap_info_t aps[NET_SCAN_MAX_AP];
    int count = net_scan(aps, NET_SCAN_MAX_AP);
    if (count > 0) {
        ESP_LOGI(TAG, "扫描到 %d 个 AP：", count);
        for (int i = 0; i < count; i++) {
            ESP_LOGI(TAG, "  %-32s %4d dBm  ch%u", aps[i].ssid, aps[i].rssi, aps[i].channel);
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "扫描到 %d 个AP, 连接 %s...", count, APP_WIFI_SSID);
        app_ui_set_status(buf);
    } else {
        ESP_LOGW(TAG, "扫描失败或没有 AP，count=%d", count);
        app_ui_set_status("WiFi: 未扫描到 AP");
    }

    /* 3. 连接指定 WiFi（名称 + 密码） */
    if (strlen(APP_WIFI_SSID) > 0) {
        err = net_connect_with_retry(APP_WIFI_SSID, APP_WIFI_PASSWORD, 3);
        if (err == ESP_OK) {
            char ip[20] = {0};
            net_get_ip(ip, sizeof(ip));
            ESP_LOGI(TAG, "连接成功，IP = %s", ip);
            char buf[64];
            snprintf(buf, sizeof(buf), "WiFi: %s  IP:%s  %d dBm",
                     net_get_ssid(), ip, net_get_rssi());
            app_ui_set_status(buf);
        } else {
            ESP_LOGE(TAG, "连接 %s 失败: %s", APP_WIFI_SSID, esp_err_to_name(err));
            app_ui_set_status("WiFi: 连接失败");
        }
    }

    /* 4. 周期性刷新信号强度 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (net_is_connected()) {
            char ip[20] = {0};
            net_get_ip(ip, sizeof(ip));
            char buf[64];
            snprintf(buf, sizeof(buf), "WiFi: %s  IP:%s  %d dBm",
                     net_get_ssid(), ip, net_get_rssi());
            app_ui_set_status(buf);
        }
    }
#else
    (void)arg;
    vTaskDelete(NULL);
#endif
}

/* ============================================================
 *  音频演示：生成 1kHz 提示音
 * ============================================================ */
__attribute__((unused)) static void app_audio_demo(void)
{
    if (audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败（ES8311 无应答？）");
        return;
    }

    audio_set_volume(70, NULL);

    /* 200ms 1kHz 正弦波，16bit 双声道 */
    const int      sample_rate = AUDIO_SAMPLE_RATE_DEFAULT;
    const int      duration_ms = 200;
    const int      frames      = sample_rate * duration_ms / 1000;
    int16_t       *buf         = malloc(frames * 2 * sizeof(int16_t));
    if (buf == NULL) {
        ESP_LOGE(TAG, "提示音缓冲分配失败");
        return;
    }
    for (int i = 0; i < frames; i++) {
        int16_t v = (int16_t)(sinf(2.0f * (float)M_PI * 1000.0f * i / sample_rate) * 8000);
        buf[2 * i]     = v;
        buf[2 * i + 1] = v;
    }

    size_t written = 0;
    ESP_LOGI(TAG, "播放提示音 ...");
    audio_play(buf, frames * 2 * sizeof(int16_t), &written, 1000);
    ESP_LOGI(TAG, "播放完成，写入 %u 字节", (unsigned)written);

    free(buf);
    vTaskDelay(pdMS_TO_TICKS(300));     /* 等声音播完再关功放 */
    audio_pa_enable(false);
}

/* ============================================================
 *  摄像头演示：把 RGB565 帧直接刷到 LCD
 * ============================================================ */
static void app_camera_frame_cb(camera_fb_t *fb)
{
    esp_lcd_panel_handle_t panel = lcd_get_panel();
    if (panel == NULL || fb == NULL || fb->format != PIXFORMAT_RGB565) {
        return;
    }
    esp_lcd_panel_draw_bitmap(panel, 0, 0, fb->width, fb->height, fb->buf);
}

__attribute__((unused)) static void app_camera_demo(void)
{
    if (camera_init() != ESP_OK) {
        ESP_LOGW(TAG, "摄像头初始化失败");
        return;
    }
    camera_start_preview(app_camera_frame_cb, 15);
}

/* ============================================================
 *  主程序
 * ============================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "================ 程序启动 ================");

    /* 1. 板级初始化（聚合层：I2C -> PCA9557 -> IMU -> SPI -> 液晶屏/LVGL -> TF 卡） */
    esp_err_t err = board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "板级初始化失败: %s", esp_err_to_name(err));
        return;
    }

    /* 2. 开机动画（app 层）：
     *    没注册动画源时就是原来的“白/红/绿/蓝/黑”刷色自检；
     *    以后注册了 TF 卡 MJPEG 源就会自动播动画（见 app_splash.h）。 */
    err = app_splash_run(3000);

    /* 3. 界面（app 层：内部会初始化 HZK16 中文字库并使用它显示中文） */
    err = app_ui_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "界面创建失败: %s", esp_err_to_name(err));
    }

    /* 4. TF 卡状态（board_init 里已经尝试挂载，这里只做展示/日志） */
    if (sdcard_is_mounted()) {
        sdcard_info_t info;
        if (sdcard_get_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "TF 卡: %s，总 %llu KB / 剩余 %llu KB", info.name,
                     (unsigned long long)(info.total_bytes / 1024),
                     (unsigned long long)(info.free_bytes / 1024));
        }
    } else {
        ESP_LOGW(TAG, "TF 卡未挂载：/sdcard 不可用（没插卡时属正常，不会自动格式化）");
#if !APP_WIFI_DEMO
        app_ui_set_status("TF卡: 未检测到");
#endif
    }

    /* 5. 音频 / 摄像头演示（按需打开上面的宏开关） */
#if APP_AUDIO_DEMO
    app_audio_demo();
#endif
#if APP_CAMERA_DEMO
    app_camera_demo();
#endif

    /* 6. WiFi 演示任务 */
    xTaskCreate(app_wifi_task, "app_wifi", 8 * 1024, NULL, 5, NULL);

    /* LVGL 自己有一个任务在跑（lvgl_port_init 里创建），这里只做打印 */
    while (1) {
        ESP_LOGI(TAG, "运行中 ... 空闲内存: %u 字节 (内部 %u / PSRAM %u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
