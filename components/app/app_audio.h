/**
 * @file    app_audio.h
 * @brief   音频采集任务：按键按下后把 I2S(ES7210) 的 PCM 采集到 PSRAM 缓冲
 *
 * 分层：app 层，录音能力来自 my_drivers 的 audio_recorder。
 *
 * 工作方式（命令驱动，避免跨任务操作同一块缓冲）：
 *   app_key_task --(audio_ctrl_queue: START/STOP)--> app_audio_task
 *       START：调用 audio_recorder_start()，然后循环采集 20ms 一块；
 *              采集过程中每块都看一眼队列里有没有 STOP，并检查缓冲是否写满；
 *       STOP ：audio_recorder_stop() 拿到 PCM 指针和长度，
 *              discard=true 时直接丢弃（录音太短），否则投递到 pcm_queue 交给网络任务。
 *
 * 缓冲只有一份，且“必须等上一轮 IDLE 才能开始新录音”，
 * 所以网络任务使用期间不会被新的录音覆盖。
 */

#ifndef __APP_AUDIO_H
#define __APP_AUDIO_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化录音器（PSRAM 缓冲 + ES7210 麦克风）
 * @return ESP_OK 成功；其它为 audio_init/audio_recorder_init 的错误码
 * @note   必须在 audio_init() 之后调用
 */
esp_err_t app_audio_init(void);

/**
 * @brief  音频采集任务（Core 1，优先级 6）
 * @param  arg 未使用
 * @note   由 app_start() 用 xTaskCreatePinnedToCore() 创建
 */
void app_audio_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* __APP_AUDIO_H */
