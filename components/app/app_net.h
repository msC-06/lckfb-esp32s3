/**
 * @file    app_net.h
 * @brief   网络请求任务：录音 PCM -> 云端 ASR -> DeepSeek LLM -> 回复文本
 *
 * 分层：app 层，串起 app_asr / app_llm 两个云端模块。
 *
 * 数据流（全部在这个任务里串行执行，避免多个 TLS 连接抢内存）：
 *   pcm_queue(录音数据) -> app_asr_recognize() -> asr_result_queue
 *                       -> app_llm_chat()      -> llm_result_queue
 */

#ifndef __APP_NET_H
#define __APP_NET_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化网络模块（ASR token 缓存 + LLM 对话历史）
 * @return ESP_OK 成功
 */
esp_err_t app_net_init(void);

/**
 * @brief  网络请求任务（Core 0，优先级 5，栈 8KB）
 * @param  arg 未使用
 * @note   由 app_start() 用 xTaskCreatePinnedToCore() 创建；
 *         TLS + JSON 解析需要大栈，所以单独一个任务，不要合并到别的任务里。
 */
void app_net_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* __APP_NET_H */
