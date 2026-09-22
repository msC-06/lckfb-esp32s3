/**
 * @file    app_llm.h
 * @brief   DeepSeek 大模型对话（Chat Completions + web_search Function Calling）
 *
 * 分层：app 层，用 esp_http_client + cJSON。
 *
 * 请求体（自动带上对话历史与工具定义）：
 *   {
 *     "model": "deepseek-chat",
 *     "messages": [
 *       {"role":"system","content":"你是一个友好的对话助手，请用简洁中文回答。"},
 *       ... 历史 user/assistant（最多 10 轮）...
 *       {"role":"user","content":"<本次识别文本>"}
 *     ],
 *     "tools": [{"type":"function","function":{"name":"web_search", ...}}],
 *     "stream": false,
 *     "max_tokens": 512
 *   }
 *
 * 当模型返回 tool_calls 时，本模块会自动调用 app_web_search() 联网搜索，
 * 并把搜索结果作为 role=tool 的消息再问一次，最终把答复写进 reply。
 * 调用者（app_net_task）不需要关心是否发生了搜索。
 *
 * 线程：只有网络任务（app_net_task）调用。
 */

#ifndef __APP_LLM_H
#define __APP_LLM_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 LLM 模块（清空对话历史）
 * @return ESP_OK
 */
esp_err_t app_llm_init(void);

/**
 * @brief  发起一轮对话（内部可能包含“联网搜索 + 追问”两次请求）
 *
 * @param  user_text   用户输入（ASR 识别文本，UTF-8）
 * @param  reply       输出：AI 回复文本（UTF-8，已去掉首尾空白）
 * @param  reply_size  输出缓冲区大小
 * @return ESP_OK 成功；
 *         ESP_ERR_INVALID_ARG 参数错；
 *         ESP_ERR_TIMEOUT 超时（30 秒）；
 *         ESP_FAIL 网络错误 / 服务端返回错误（reply 里有中文错误说明）
 *
 * @note   成功时会把 user 与 assistant 两条消息追加进对话历史
 *         （工具轮次不落历史，原因见 app_llm.c 文件头说明）。
 * @note   搜索失败不会让整轮对话失败：会如实把失败告诉模型，让它自己回答。
 */
esp_err_t app_llm_chat(const char *user_text, char *reply, size_t reply_size);

#ifdef __cplusplus
}
#endif

#endif /* __APP_LLM_H */
