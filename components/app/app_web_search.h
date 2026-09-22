/**
 * @file    app_web_search.h
 * @brief   联网搜索工具（Tavily Search API）—— 供 DeepSeek Function Calling 调用
 *
 * 分层：app 层。直接用 ESP-IDF 的 esp_http_client + cJSON，
 *       **不依赖任何外部代理后端**，全部逻辑跑在 ESP32 上。
 *
 * 调用链（本模块只负责中间那一步）：
 *   DeepSeek 返回 tool_calls（function.name = "web_search"）
 *     -> app_llm 从 arguments 里取出 query
 *     -> app_web_search(query, out_buf, out_size)      ← 本模块
 *     -> app_llm 把 out_buf 作为 role=tool 的消息再发给 DeepSeek
 *
 * 特点：
 *   - 响应体用 HTTP_EVENT_ON_DATA 事件回调逐段拼接到 PSRAM 缓冲，长 JSON 不会截断；
 *   - 只提取每条结果的 title + content，最多 3 条，拼成纯文本摘要（默认上限 1200 字符）；
 *   - 每次调用结束都会释放 cJSON 对象、请求体和 PSRAM 缓冲，无内存泄漏；
 *   - 网络异常 / 超时 / 429 限流 / JSON 解析失败 一律返回 ESP_FAIL（日志里有原因）。
 */

#ifndef __APP_WEB_SEARCH_H
#define __APP_WEB_SEARCH_H

#include <stddef.h>

#include "esp_err.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  联网搜索并把结果整理成纯文本摘要
 *
 * @param  query    搜索关键词（UTF-8，一般来自 DeepSeek tool_calls 的 arguments）
 * @param  out_buf  输出缓冲区（由调用者分配，建议大小 >= APP_WEB_SEARCH_TEXT_MAX + 1）
 * @param  out_size 输出缓冲区字节数
 *
 * @return ESP_OK   搜索成功（out_buf 里是摘要文本；搜不到结果时是
 *                  "（未搜索到相关结果）"，也算成功，便于让模型据此回答）
 *         ESP_FAIL 未配置 Key / 参数非法 / 网络异常 / 超时 / 429 限流 /
 *                  服务端错误 / JSON 解析失败（out_buf 会被清空）
 *
 * @note   摘要格式：每条结果一段，形如
 *             [1] 标题
 *             正文...
 *         总长度同时受 APP_WEB_SEARCH_TEXT_MAX 与 out_size 限制，超出部分直接截断。
 */
esp_err_t app_web_search(const char *query, char *out_buf, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* __APP_WEB_SEARCH_H */
