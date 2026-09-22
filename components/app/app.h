/**
 * @file    app.h
 * @brief   应用入口（app 层）：初始化外设与界面，创建双核任务
 *
 * 调用关系：
 *     main.c          —— 极简：board_init() -> app_splash_run() -> app_start()
 *     app_start()     —— 本文件：初始化 + 创建任务 + 返回
 *     app_*_task      —— 四个任务分别跑在两个核上
 *
 * 任务划分（详见 app_config.h）：
 *   Core 0（网络核）：app_wifi_task(prio 5) / app_net_task(prio 5, 栈 8KB)
 *   Core 1（应用核）：app_key_task(prio 4)  / app_audio_task(prio 6)
 */

#ifndef __APP_H
#define __APP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  启动对话应用
 *
 * 做的事：
 *   1. 创建跨任务队列（app_bus_init）；
 *   2. 初始化 HZK16 中文字库；
 *   3. 创建聊天界面（新建 screen 并加载，覆盖原来的界面）；
 *   4. 初始化音频（ES8311/ES7210 + 录音器，PCM 缓冲放 PSRAM）；
 *   5. 初始化按键（GPIO0 + 20ms 消抖定时器）；
 *   6. 初始化网络模块（ASR/LLM，清空对话历史）；
 *   7. 用 xTaskCreatePinnedToCore() 创建 4 个任务并绑定核心；
 *   8. 打印内存占用后返回（由 main.c 删除 app_main 任务）。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 板级/液晶屏未初始化；
 *         其它为队列或任务创建失败的错误码
 */
esp_err_t app_start(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_H */
