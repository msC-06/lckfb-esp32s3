/**
 * @file    app_llm.c
 * @brief   DeepSeek 对话实现（含 web_search Function Calling 主循环）
 *
 * 一轮完整对话（最多两次 HTTP 请求）：
 *   第 1 次请求：system + 历史 + user + tools(web_search)
 *     ├─ 模型直接回答        -> 取 choices[0].message.content，结束
 *     └─ 模型返回 tool_calls -> 逐个执行
 *                               web_search -> app_web_search() 联网搜索
 *                               拿到摘要后，第 2 次请求带上
 *                               assistant(tool_calls) + tool(tool_call_id, 摘要)
 *                               （第 2 次不带 tools，避免模型一直搜索不回答）
 *                             -> 取最终答复
 *
 * 对话历史只存 user / assistant 两条：
 *   工具轮次（assistant.tool_calls + tool）**不落历史**。原因是历史满 10 轮会丢弃最老的一对，
 *   一旦把 tool_calls 和对应的 tool 消息拆散，DeepSeek 会因为“tool 消息没有对应的
 *   tool_calls”直接返回 400，之后每轮都失败。工具结果的价值已经体现在最终答复里了。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_llm.h"
#include "app_config.h"
#include "app_chat_history.h"
#include "app_http.h"
#include "app_web_search.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"               /* EXT_RAM_BSS_ATTR：工具结果放 PSRAM */
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "cJSON.h"

static const char *TAG = "APP_LLM";

/* ============================ 工具定义 ============================ */

/** 联网搜索工具名（模型靠这个名字回调；描述见 app_config.h 的 APP_LLM_TOOL_DESC） */
#define LLM_TOOL_WEB_SEARCH     "web_search"
/** 一次最多执行几个 tool_calls */
#define LLM_TOOL_MAX            APP_LLM_MAX_TOOL_CALLS

/**
 * 一次工具执行的结果
 * @note  放静态区（不占网络任务那 8KB 栈）：只有 app_net_task 会调用本模块；
 *        同时用 EXT_RAM_BSS_ATTR 放到 PSRAM，不占内部 RAM
 *        （需开 CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY，没开就退回内部 RAM）。
 */
typedef struct {
    char id[64];                                /*!< tool_call_id（回传时必须原样带回） */
    char name[48];                              /*!< 工具名 */
    char content[APP_WEB_SEARCH_TEXT_MAX + 1];  /*!< 回给模型的文本 */
} llm_tool_result_t;

EXT_RAM_BSS_ATTR static llm_tool_result_t s_tool_results[LLM_TOOL_MAX];

/* ============================ 小工具 ============================ */

/**
 * @brief  去掉字符串首尾的空白字符（原地修改）
 */
static char *llm_trim(char *s)
{
    if (s == NULL) {
        return NULL;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[--len] = '\0';
        } else {
            break;
        }
    }
    return s;
}

/* ============================ 请求体组装 ============================ */

/**
 * @brief  新建请求体根对象和 messages 数组
 * @return 根对象（失败返回 NULL）；messages_out 输出 messages 数组
 */
static cJSON *llm_new_body_root(cJSON **messages_out)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (messages == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    *messages_out = messages;
    return root;
}

/**
 * @brief  往 messages 里放“上下文”：system 提示词 + 历史对话 + 本次提问
 */
static void llm_add_context_messages(cJSON *messages, const char *user_text)
{
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", APP_LLM_SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, sys);

    /* 历史对话（最多 10 轮，只含 user/assistant） */
    app_chat_history_to_json(messages);

    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", user_text);
    cJSON_AddItemToArray(messages, user);
}

/**
 * @brief  往请求体里加 tools 定义（只注册一个 web_search 函数）
 */
static void llm_add_tools(cJSON *root)
{
    cJSON *tools = cJSON_AddArrayToObject(root, "tools");
    if (tools == NULL) {
        return;
    }

    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");

    cJSON *fn = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(fn, "name", LLM_TOOL_WEB_SEARCH);
    cJSON_AddStringToObject(fn, "description", APP_LLM_TOOL_DESC);

    cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *query = cJSON_AddObjectToObject(props, "query");
    cJSON_AddStringToObject(query, "type", "string");
    cJSON_AddStringToObject(query, "description", "搜索关键词，尽量简短具体");

    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("query"));

    cJSON_AddItemToArray(tools, tool);
}

/**
 * @brief  第 1 轮请求体：system + 历史 + user + tools
 * @return 堆内存里的 JSON 字符串（调用者 free）；失败返回 NULL
 */
static char *llm_build_body(const char *user_text)
{
    cJSON *messages = NULL;
    cJSON *root = llm_new_body_root(&messages);
    if (root == NULL) {
        return NULL;
    }

    llm_add_context_messages(messages, user_text);
    llm_add_tools(root);                    /* 第 1 轮把工具定义带上 */

    cJSON_AddStringToObject(root, "model", DEEPSEEK_MODEL);
    cJSON_AddBoolToObject(root, "stream", 0);
    cJSON_AddNumberToObject(root, "max_tokens", APP_LLM_MAX_TOKENS);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/**
 * @brief  第 2 轮请求体：system + 历史 + user + assistant(tool_calls) + tool(...)
 *
 * @param  tool_calls 第 1 轮响应里的 tool_calls 数组（会被深拷贝进去）
 * @param  results    工具执行结果
 * @param  count      结果条数
 * @return 堆内存里的 JSON 字符串（调用者 free）；失败返回 NULL
 *
 * @note   这里**不再带 tools**：已经搜过一次了，让模型基于结果直接作答，
 *         否则模型可能一轮一轮地搜下去。
 */
static char *llm_build_followup(const char *user_text, const cJSON *tool_calls,
                                const llm_tool_result_t *results, int count)
{
    cJSON *messages = NULL;
    cJSON *root = llm_new_body_root(&messages);
    if (root == NULL) {
        return NULL;
    }

    llm_add_context_messages(messages, user_text);

    /* 1. assistant 消息：content 为空，但必须把 tool_calls 原样带回去 */
    cJSON *assistant = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "content", "");
    cJSON *calls = cJSON_Duplicate(tool_calls, 1);
    if (calls != NULL) {
        cJSON_AddItemToObject(assistant, "tool_calls", calls);
    }
    cJSON_AddItemToArray(messages, assistant);

    /* 2. 每个 tool_call 对应一条 tool 消息（tool_call_id 必须匹配，否则接口报 400） */
    for (int i = 0; i < count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "role", "tool");
        cJSON_AddStringToObject(tool, "tool_call_id", results[i].id);
        cJSON_AddStringToObject(tool, "content", results[i].content);
        cJSON_AddItemToArray(messages, tool);
    }

    cJSON_AddStringToObject(root, "model", DEEPSEEK_MODEL);
    cJSON_AddBoolToObject(root, "stream", 0);
    cJSON_AddNumberToObject(root, "max_tokens", APP_LLM_MAX_TOKENS);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* ============================ 响应解析 ============================ */

/**
 * @brief  取 choices[0].message
 * @return 消息对象（属于 root，不要单独 delete）；没有则 NULL
 */
static const cJSON *llm_get_message(const cJSON *root)
{
    const cJSON *choices = cJSON_GetObjectItem(root, "choices");
    const cJSON *first   = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    return (first != NULL) ? cJSON_GetObjectItem(first, "message") : NULL;
}

/**
 * @brief  取出服务端返回的 error.message（例如额度不足、Key 无效）
 */
static void llm_extract_error(const cJSON *root, char *out, size_t out_size)
{
    const cJSON *error = cJSON_GetObjectItem(root, "error");
    const cJSON *msg   = cJSON_IsObject(error) ? cJSON_GetObjectItem(error, "message") : NULL;
    snprintf(out, out_size, "AI服务错误: %.200s",
             (msg != NULL && cJSON_IsString(msg)) ? msg->valuestring : "未知错误");
    ESP_LOGE(TAG, "服务端返回错误：%s", cJSON_IsString(msg) ? msg->valuestring : "未知");
}

/**
 * @brief  把 message.content 复制到 reply（去掉首尾空白）
 * @return ESP_OK 有文字；ESP_FAIL 没有 content（例如纯工具调用）
 */
static esp_err_t llm_message_content(const cJSON *message, char *reply, size_t reply_size)
{
    const cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!cJSON_IsString(content) || content->valuestring == NULL) {
        return ESP_FAIL;
    }

    strncpy(reply, content->valuestring, reply_size - 1);
    reply[reply_size - 1] = '\0';

    char *clean = llm_trim(reply);
    if (clean != reply) {
        memmove(reply, clean, strlen(clean) + 1);
    }
    return (reply[0] != '\0') ? ESP_OK : ESP_FAIL;
}

/* ============================ 发请求 ============================ */

/**
 * @brief  发一次 chat/completions 请求（响应体由事件回调拼到 resp 的缓冲里）
 *
 * @param  body   请求体
 * @param  resp   响应上下文（每次调用前会被清空复用）
 * @param  status 输出 HTTP 状态码（-1 = 没拿到响应）
 * @return ESP_OK 拿到了 HTTP 响应（状态码看 status）；
 *         其它为网络/超时等传输层错误
 */
static esp_err_t llm_request(const char *body, app_http_resp_t *resp, int *status)
{
    int body_len = (int)strlen(body);

    app_http_resp_init(resp, resp->buf, resp->cap);     /* 清空复用 */

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", DEEPSEEK_API_KEY);

    esp_http_client_config_t cfg = {
        .url            = DEEPSEEK_API_URL,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = APP_LLM_TIMEOUT_MS,
        .buffer_size    = 4096,
        .buffer_size_tx = 2048,
        .event_handler  = app_http_event_handler,       /* 响应体逐段拼接，避免截断 */
        .user_data      = resp,
#if APP_TLS_USE_CA_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .skip_cert_common_name_check = true,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP 客户端创建失败");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, body_len);

    int64_t   t_begin = esp_timer_get_time();
    esp_err_t err     = esp_http_client_perform(client);
    int       code    = esp_http_client_get_status_code(client);
    int       elapsed = (int)((esp_timer_get_time() - t_begin) / 1000);

    if (status != NULL) {
        *status = (err == ESP_OK) ? code : -1;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "响应：HTTP %d，%u 字节，耗时 %d ms", code, (unsigned)resp->len, elapsed);
    } else {
        ESP_LOGE(TAG, "请求失败：%s（耗时 %d ms）", esp_err_to_name(err), elapsed);
    }

    esp_http_client_cleanup(client);
    return (err == ESP_OK) ? ESP_OK : err;
}

/* ============================ 工具执行 ============================ */

/**
 * @brief  从 tool_call.function.arguments 里解析出 query
 *
 * @note   arguments 是一个 JSON **字符串**（形如 "{\"query\":\"今天天气\"}"），
 *         所以要再解析一次。
 * @return ESP_OK 解析到非空 query
 */
static esp_err_t llm_extract_query(const cJSON *tool_call, char *out, size_t out_size)
{
    const cJSON *fn   = cJSON_GetObjectItem(tool_call, "function");
    const cJSON *args = (fn != NULL) ? cJSON_GetObjectItem(fn, "arguments") : NULL;

    if (!cJSON_IsString(args) || args->valuestring == NULL) {
        return ESP_FAIL;
    }

    cJSON *a = cJSON_Parse(args->valuestring);
    if (a == NULL) {
        ESP_LOGW(TAG, "arguments 不是合法 JSON：%.80s", args->valuestring);
        return ESP_FAIL;
    }

    const cJSON *q = cJSON_GetObjectItem(a, "query");
    esp_err_t ret = ESP_FAIL;
    if (cJSON_IsString(q) && q->valuestring != NULL && q->valuestring[0] != '\0') {
        strncpy(out, q->valuestring, out_size - 1);
        out[out_size - 1] = '\0';
        ret = ESP_OK;
    }
    cJSON_Delete(a);
    return ret;
}

/**
 * @brief  执行响应里的 tool_calls（目前只实现了 web_search）
 *
 * @param  tool_calls tool_calls 数组
 * @return 实际执行（处理）的条数
 */
static int llm_execute_tools(const cJSON *tool_calls)
{
    int n = cJSON_GetArraySize(tool_calls);
    if (n > LLM_TOOL_MAX) {
        ESP_LOGW(TAG, "模型一次给了 %d 个工具调用，只执行前 %d 个", n, LLM_TOOL_MAX);
        n = LLM_TOOL_MAX;
    }

    for (int i = 0; i < n; i++) {
        const cJSON *tc   = cJSON_GetArrayItem(tool_calls, i);
        llm_tool_result_t *r = &s_tool_results[i];
        memset(r, 0, sizeof(*r));

        const cJSON *id   = cJSON_GetObjectItem(tc, "id");
        const cJSON *fn   = cJSON_GetObjectItem(tc, "function");
        const cJSON *name = (fn != NULL) ? cJSON_GetObjectItem(fn, "name") : NULL;

        if (cJSON_IsString(id)) {
            strncpy(r->id, id->valuestring, sizeof(r->id) - 1);
        }
        if (cJSON_IsString(name)) {
            strncpy(r->name, name->valuestring, sizeof(r->name) - 1);
        }

        /* 不认识的工具：如实告诉模型，让它自己回答 */
        if (strcmp(r->name, LLM_TOOL_WEB_SEARCH) != 0) {
            ESP_LOGW(TAG, "模型请求了未实现的工具：%s", r->name);
            /* 注意：这里用 JSON 里的名字而不是 r->name，
             * 否则 snprintf 的目标和参数都落在 s_tool_results 里会被 -Wrestrict 拦下 */
            snprintf(r->content, sizeof(r->content), "工具 %s 不可用，请直接回答。",
                     (name != NULL && cJSON_IsString(name)) ? name->valuestring : "?");
            continue;
        }

        char query[512] = {0};
        if (llm_extract_query(tc, query, sizeof(query)) != ESP_OK) {
            snprintf(r->content, sizeof(r->content), "联网搜索失败：没有解析到搜索关键词。");
            continue;
        }

        ESP_LOGI(TAG, "调用工具 %s：query=\"%s\"", r->name, query);
        if (app_web_search(query, r->content, sizeof(r->content)) != ESP_OK) {
            ESP_LOGW(TAG, "联网搜索失败");
            snprintf(r->content, sizeof(r->content), "联网搜索失败：网络或服务异常。");
        } else {
            ESP_LOGI(TAG, "搜索结果 %u 字符", (unsigned)strlen(r->content));
        }
    }
    return n;
}

/* ============================ 对外接口 ============================ */

esp_err_t app_llm_init(void)
{
    esp_err_t err = app_chat_history_init();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "LLM 模块就绪（模型 %s，接口 %s，已注册工具 %s）",
             DEEPSEEK_MODEL, DEEPSEEK_API_URL, LLM_TOOL_WEB_SEARCH);
    return ESP_OK;
}

esp_err_t app_llm_chat(const char *user_text, char *reply, size_t reply_size)
{
    if (user_text == NULL || reply == NULL || reply_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reply[0] = '\0';

    if (strcmp(DEEPSEEK_API_KEY, "YOUR_DEEPSEEK_API_KEY") == 0 || strlen(DEEPSEEK_API_KEY) == 0) {
        ESP_LOGE(TAG, "还没有配置 DeepSeek API Key（填 components/app/app_config_secret.h）");
        snprintf(reply, reply_size, "未配置DeepSeek密钥");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t       err      = ESP_FAIL;
    char           *body     = NULL;
    char           *resp_buf = NULL;
    cJSON          *root     = NULL;
    app_http_resp_t resp;
    int             status   = 0;

    /* ---------- 1. 第 1 轮请求体：system + 历史 + user + tools ---------- */
    body = llm_build_body(user_text);
    if (body == NULL) {
        snprintf(reply, reply_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "第1轮请求：%u 字节，历史 %d 条",
             (unsigned)strlen(body), app_chat_history_count());

    /* ---------- 2. 接收缓冲放 PSRAM ---------- */
    resp_buf = (char *)heap_caps_malloc(APP_LLM_RESP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resp_buf == NULL) {
        resp_buf = (char *)malloc(APP_LLM_RESP_BUF_SIZE);       /* 退化到内部 RAM */
    }
    if (resp_buf == NULL) {
        free(body);
        snprintf(reply, reply_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    app_http_resp_init(&resp, resp_buf, APP_LLM_RESP_BUF_SIZE);

    /* ---------- 3. 第 1 轮请求 ---------- */
    err = llm_request(body, &resp, &status);
    if (err != ESP_OK) {
        snprintf(reply, reply_size, "AI网络连接失败");
        goto done;
    }
    if (status != 200) {
        root = app_http_parse_json(&resp);
        if (root != NULL && cJSON_GetObjectItem(root, "error") != NULL) {
            llm_extract_error(root, reply, reply_size);
        } else {
            snprintf(reply, reply_size, "AI服务异常(HTTP %d)", status);
        }
        err = ESP_FAIL;
        goto done;
    }

    root = app_http_parse_json(&resp);
    if (root == NULL) {
        app_http_dump(TAG, &resp);
        snprintf(reply, reply_size, "AI回复解析失败");
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    const cJSON *message = llm_get_message(root);
    if (message == NULL) {
        snprintf(reply, reply_size, "AI响应里没有 message");
        err = ESP_FAIL;
        goto done;
    }

    const cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");

    /* ---------- 4. 没有工具调用：这就是最终答复 ---------- */
    if (!cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0) {
        if (llm_message_content(message, reply, reply_size) != ESP_OK) {
            snprintf(reply, reply_size, "AI没有返回内容");
            err = ESP_FAIL;
        } else {
            err = ESP_OK;
        }
        goto done;
    }

    /* ---------- 5. 有工具调用：先执行搜索 ---------- */
    ESP_LOGI(TAG, "模型要求调用 %d 个工具", cJSON_GetArraySize(tool_calls));

    /* 有的模型会同时给出文字，这里先留着当兜底 */
    (void)llm_message_content(message, reply, reply_size);

    int tool_count = llm_execute_tools(tool_calls);
    if (tool_count <= 0) {
        snprintf(reply, reply_size, "AI工具调用异常");
        err = ESP_FAIL;
        goto done;
    }

    char *body2 = llm_build_followup(user_text, tool_calls, s_tool_results, tool_count);
    cJSON_Delete(root);
    root = NULL;
    if (body2 == NULL) {
        snprintf(reply, reply_size, "内存不足");
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    free(body);
    body = body2;

    /* ---------- 6. 第 2 轮请求：带上工具结果追问 ---------- */
    ESP_LOGI(TAG, "带搜索结果追问：%u 字节", (unsigned)strlen(body));
    err = llm_request(body, &resp, &status);
    if (err != ESP_OK || status != 200) {
        if (reply[0] != '\0') {
            ESP_LOGW(TAG, "追问失败（HTTP %d），改用第 1 轮的文字", status);
            err = ESP_OK;
        } else {
            snprintf(reply, reply_size, "搜索后追问失败(HTTP %d)", status);
            err = ESP_FAIL;
        }
        goto done;
    }

    root = app_http_parse_json(&resp);
    if (root == NULL) {
        app_http_dump(TAG, &resp);
        if (reply[0] == '\0') {
            snprintf(reply, reply_size, "AI回复解析失败");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            err = ESP_OK;
        }
        goto done;
    }

    message = llm_get_message(root);
    if (message == NULL || llm_message_content(message, reply, reply_size) != ESP_OK) {
        if (reply[0] == '\0') {
            snprintf(reply, reply_size, "AI没有返回内容");
            err = ESP_FAIL;
        } else {
            err = ESP_OK;       /* 兜底用第 1 轮文字 */
        }
        goto done;
    }
    err = ESP_OK;

done:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (body != NULL) {
        free(body);
    }
    if (resp_buf != NULL) {
        free(resp_buf);
    }

    /* 成功才写历史：user + assistant 成对进历史（工具轮次不落历史，见文件头说明） */
    if (err == ESP_OK) {
        app_chat_history_add("user", user_text);
        app_chat_history_add("assistant", reply);
        ESP_LOGI(TAG, "AI 回复：%s", reply);
    }
    return err;
}
