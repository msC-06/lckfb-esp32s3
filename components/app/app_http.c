/**
 * @file    app_http.c
 * @brief   HTTP 响应体累积与读取实现
 */

#include <stdlib.h>
#include <string.h>

#include "app_http.h"

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "APP_HTTP";

/** 单次 dump 最多完整打印多少字节（超出就只打头尾） */
#define APP_HTTP_DUMP_MAX   4096

/* ============================ 对外接口 ============================ */

void app_http_resp_init(app_http_resp_t *resp, char *buf, size_t cap)
{
    if (resp == NULL) {
        return;
    }
    resp->buf      = buf;
    resp->cap      = cap;
    resp->len      = 0;
    resp->overflow = false;
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
}

esp_err_t app_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        app_http_resp_t *resp = (app_http_resp_t *)evt->user_data;
        if (resp != NULL && resp->buf != NULL && evt->data != NULL && evt->data_len > 0) {
            size_t room = (resp->cap > resp->len + 1) ? (resp->cap - 1 - resp->len) : 0;
            size_t n    = ((size_t)evt->data_len <= room) ? (size_t)evt->data_len : room;

            if (n > 0) {
                memcpy(resp->buf + resp->len, evt->data, n);
                resp->len += n;
                resp->buf[resp->len] = '\0';
            }
            if (n < (size_t)evt->data_len) {
                resp->overflow = true;
            }
        }
    }
    return ESP_OK;
}

int app_http_pump(esp_http_client_handle_t client)
{
    char sink[512];
    int  pumped = 0;

    if (client == NULL) {
        return -1;
    }

    /* 正常情况下 1~2 次就能把 body 抽干；上限只是防御服务端不收尾 */
    for (int i = 0; i < 4096; i++) {
        int r = esp_http_client_read(client, sink, sizeof(sink));
        if (r <= 0) {
            break;      /* 0 = 读完；<0 = 超时/出错，数据已经通过事件回调拿到了 */
        }
        pumped += r;
    }
    return pumped;
}

bool app_http_check_length(const char *tag, esp_http_client_handle_t client,
                           const app_http_resp_t *resp)
{
    if (tag == NULL || client == NULL || resp == NULL) {
        return false;
    }

    int64_t declared = esp_http_client_get_content_length(client);
    if (declared <= 0) {
        return true;        /* chunked 或没有 Content-Length，没法比较 */
    }
    if ((size_t)declared != resp->len) {
        ESP_LOGW(tag, "响应长度不符：服务端声明 %lld 字节，实际收到 %u 字节（可能被截断）",
                 (long long)declared, (unsigned)resp->len);
        return false;
    }
    return true;
}

void app_http_dump(const char *tag, const app_http_resp_t *resp)
{
    /** 打印用的静态缓冲（1KB，刚好够看清响应开头，也不占调用者栈） */
    static char s_head[1024];

    if (resp == NULL) {
        return;
    }
    if (resp->len == 0) {
        ESP_LOGE(tag, "响应体为空（overflow=%d）", (int)resp->overflow);
        return;
    }
    if (resp->overflow) {
        ESP_LOGE(tag, "响应超过缓冲上限（%u 字节），已被截断，请加大接收缓冲！",
                 (unsigned)resp->cap);
    }

    if (resp->len <= APP_HTTP_DUMP_MAX) {
        ESP_LOGE(tag, "响应体共 %u 字节%s：%s",
                 (unsigned)resp->len, resp->overflow ? "（已截断）" : "", resp->buf);
        return;
    }

    /* 太长时打印头尾各一段 */
    size_t head_len = sizeof(s_head) - 1;
    if (head_len > resp->len) {
        head_len = resp->len;
    }
    memcpy(s_head, resp->buf, head_len);
    s_head[head_len] = '\0';
    ESP_LOGE(tag, "响应体共 %u 字节%s，开头：%s ... 结尾：%s",
             (unsigned)resp->len, resp->overflow ? "（已截断）" : "",
             s_head, resp->buf + resp->len - 128);
}

struct cJSON *app_http_parse_json(const app_http_resp_t *resp)
{
    if (resp == NULL || resp->buf == NULL || resp->len == 0) {
        return NULL;
    }

    /* 1. 正常解析 */
    cJSON *root = cJSON_Parse(resp->buf);
    if (root != NULL) {
        return root;
    }

    /* 2. 兜底：掐掉第一个 '{' 之前、最后一个 '}' 之后的杂字符再试 */
    const char *begin = strchr(resp->buf, '{');
    const char *end   = strrchr(resp->buf, '}');
    if (begin == NULL || end == NULL || end <= begin) {
        return NULL;        /* 连大括号都不完整（例如响应被截断），救不回来 */
    }

    size_t len = (size_t)(end - begin) + 1U;
    char  *tmp = (char *)malloc(len + 1U);
    if (tmp == NULL) {
        return NULL;
    }
    memcpy(tmp, begin, len);
    tmp[len] = '\0';

    root = cJSON_Parse(tmp);
    if (root != NULL) {
        ESP_LOGW(TAG, "响应前后有杂字符，已裁剪后解析成功（裁剪掉 %u 字节）",
                 (unsigned)(resp->len - len));
    }
    free(tmp);
    return root;
}
