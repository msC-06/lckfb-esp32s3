/**
 * @file    main.c
 * @brief   ESP32-S3 对话掌机程序入口（组装层，保持极简）
 *
 * 分层：
 *     main.c                 —— 只做组装：board_init() -> 开机动画 -> app_start()
 *     components/app         —— 界面、按键、音频、WiFi/ASR/LLM 业务（app_* 模块）
 *     components/my_drivers  —— 外部器件驱动（液晶屏/LVGL/音频 Codec/字库/录音器）
 *     components/bsp         —— ESP32-S3 片上外设（I2C/SPI/GPIO/WiFi）
 *
 * 注意：main.c 不逐个初始化外设（交给 board_init()），也不直接操作 bsp。
 */

#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* my_drivers：板级聚合层（内部完成 I2C/PCA9557/SPI/液晶屏/LVGL/TF 卡初始化） */
#include "my_drivers/board/board.h"

/* app：开机动画 + 对话应用 */
#include "app_splash.h"
#include "app.h"

static const char *TAG = "main";

/* ============================================================
 *  编译期开关
 * ============================================================ */

/**
 * 是否在开机动画后进入原来的“游戏中心”小游戏框架。
 *
 * 本工程的主界面是 app_start() 里的聊天界面（它会新建并加载自己的 screen），
 * 会和游戏主页抢屏，所以默认关闭；想同时保留小游戏时把这里改成 1 即可
 * （游戏主页仍然在旧 screen 上，聊天界面加载后不再可见）。
 */
#define APP_ENABLE_GAME_LAUNCHER    0

#if APP_ENABLE_GAME_LAUNCHER
#include "game_launcher/game_manager.h"
#include "games/game_2048/game_2048.h"
#endif

/* ============================================================
 *  主程序
 * ============================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "================ 程序启动 ================");

    /* 1. 板级初始化（聚合层）：
     *    I2C -> PCA9557 -> QMI8658 -> SPI -> 液晶屏/LVGL -> TF 卡
     *    液晶屏/LVGL 就绪之后，app 层才能建界面。 */
    esp_err_t err = board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "板级初始化失败: %s", esp_err_to_name(err));
        return;
    }

    /* 2. 开机动画（app 层，最多 3 秒，没有动画源时是刷色自检） */
    app_splash_run(3000);

    /* 3. 小游戏框架（可选，默认关闭，见上面开关） */
#if APP_ENABLE_GAME_LAUNCHER
    err = game_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "游戏管理器初始化失败: %s", esp_err_to_name(err));
    } else {
        game_manager_register(game_2048_get());
        game_manager_start();
        game_manager_set_status("对话掌机已启动");
    }
#endif

    /* 4. 启动对话应用：建界面 + 初始化音频/按键/网络 + 创建四个双核任务 */
    err = app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "对话应用启动失败: %s", esp_err_to_name(err));
    }

    /* 5. app_main 的任务到此结束：后面所有工作都由 app 层创建的任务负责，
     *    这里删掉自己，把 8KB 栈和 TCB 还给系统。 */
    ESP_LOGI(TAG, "初始化完成，删除 main 任务");
    vTaskDelete(NULL);
}
