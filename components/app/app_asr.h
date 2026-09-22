/**
 * @file    app_asr.h
 * @brief   语音识别（百度智能云 · 短语音识别标准版 REST API）
 *
 * 分层：app 层，用 ESP-IDF 的 esp_http_client + mbedtls(base64) + cJSON。
 *
 * 调用流程：
 *   1. app_asr_recognize() 内部先检查 token 缓存，没有/快过期就先取 token
 *      （POST https://aip.baidubce.com/oauth/2.0/token）；
 *   2. 再 POST 到 https://vop.baidu.com/server_api，
 *      Header: Content-Type: audio/pcm; rate=16000，
 *      Body:   {"format":"pcm","rate":16000,"channel":1,"cuid":"...",
 *               "token":"...","speech":"<base64 PCM>","len":<PCM字节数>}
 *      PCM 数据边编码边发（分块 base64），不在 RAM 里再存一份 640KB 的 body；
 *   3. 解析返回 JSON：err_no == 0 时 result[0] 就是识别文本。
 *
 * 线程：只有网络任务（app_net_task）调用，内部有 token 缓存，不加锁。
 */

#ifndef __APP_ASR_H
#define __APP_ASR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 ASR 模块（清空 token 缓存）
 * @return ESP_OK
 */
esp_err_t app_asr_init(void);

/**
 * @brief  识别一段 16kHz / 16bit / 单声道 PCM
 *
 * @param  pcm       PCM 数据（16bit 小端单声道）
 * @param  bytes     PCM 字节数
 * @param  out       输出缓冲区：成功放识别文本，失败放中文错误说明（供界面显示）
 * @param  out_size  输出缓冲区大小
 * @return ESP_OK 识别成功；
 *         ESP_ERR_INVALID_ARG 参数非法；
 *         ESP_ERR_INVALID_SIZE 录音太短；
 *         ESP_ERR_TIMEOUT 网络超时；
 *         ESP_FAIL 取 token 失败 / 服务端返回错误（详见 out 里的文字）
 */
esp_err_t app_asr_recognize(const int16_t *pcm, size_t bytes, char *out, size_t out_size);

/**
 * @brief  丢弃 token 缓存，下次识别会重新获取
 */
void app_asr_reset_token(void);

/**
 * @brief  当前是否已经缓存了可用的 token
 */
bool app_asr_has_token(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_ASR_H */
