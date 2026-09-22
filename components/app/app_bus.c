/**
 * @file    app_bus.c
 * @brief   应用总线实现：队列创建、状态机、消息投递
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_bus.h"

#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "app_bus";

/* ============================ 内部状态（全部 static） ============================ */

static QueueHandle_t key_event_queue   = NULL;  /*!< 按键事件 */
static QueueHandle_t audio_ctrl_queue  = NULL;  /*!< 录音启停命令 */
static QueueHandle_t pcm_queue         = NULL;  /*!< 录音数据 */
static QueueHandle_t asr_result_queue  = NULL;  /*!< ASR 结果 */
static QueueHandle_t llm_result_queue  = NULL;  /*!< LLM 结果 */
static QueueHandle_t chat_msg_queue    = NULL;  /*!< 聊天消息 */

/** 当前业务状态：32 位对齐读写，单写者 + 多读者，无需加锁 */
static volatile app_state_t s_state = APP_STATE_IDLE;

/* ============================ 发送暂存区 ============================ */
/*
 * 为什么不用局部变量发消息：
 *   app_text_msg_t 现在有 4KB+（APP_TEXT_MAX_LEN），如果把它当局部变量放在发送函数里，
 *   这几 KB 就落在**调用者的栈**上。而 app_audio_task / app_key_task 的栈只有 4KB，
 *   一进函数就爆栈——现象就是 “InstrFetchProhibited”、返回地址变成 0xffffffff
 *   （栈被写穿，把相邻任务的栈/TCB 冲掉了）。
 *
 *   所以改成“每个队列一个静态暂存区 + 一把互斥锁”：
 *   锁只包住一次 xQueueSend（它会把内容整体拷进队列），持锁时间极短。
 *   暂存区用 EXT_RAM_BSS_ATTR 放 PSRAM，也不占内部 RAM。
 */
EXT_RAM_BSS_ATTR static app_text_msg_t s_asr_stage;    /*!< ASR 结果暂存 */
EXT_RAM_BSS_ATTR static app_text_msg_t s_llm_stage;    /*!< LLM 结果暂存 */
EXT_RAM_BSS_ATTR static chat_msg_t     s_chat_stage;   /*!< 聊天消息暂存 */
EXT_RAM_BSS_ATTR static chat_msg_t     s_chat_trash;   /*!< 只用来接住被丢掉的最老消息 */
static SemaphoreHandle_t               s_stage_lock = NULL;

/* ============================ 初始化 ============================ */

esp_err_t app_bus_init(void)
{
    if (key_event_queue != NULL) {
        ESP_LOGW(TAG, "总线已经初始化过了");
        return ESP_ERR_INVALID_STATE;
    }

    key_event_queue  = xQueueCreate(APP_KEY_QUEUE_LEN,   sizeof(app_key_event_t));
    audio_ctrl_queue = xQueueCreate(APP_AUDIO_QUEUE_LEN, sizeof(app_audio_cmd_t));
    pcm_queue        = xQueueCreate(APP_PCM_QUEUE_LEN,   sizeof(app_pcm_msg_t));
    asr_result_queue = xQueueCreate(APP_ASR_QUEUE_LEN,   sizeof(app_text_msg_t));
    llm_result_queue = xQueueCreate(APP_LLM_QUEUE_LEN,   sizeof(app_text_msg_t));
    chat_msg_queue   = xQueueCreate(APP_CHAT_QUEUE_LEN,  sizeof(chat_msg_t));

    if (key_event_queue == NULL || audio_ctrl_queue == NULL || pcm_queue == NULL ||
        asr_result_queue == NULL || llm_result_queue == NULL || chat_msg_queue == NULL) {
        ESP_LOGE(TAG, "队列创建失败（内部 RAM 不足？）");
        return ESP_ERR_NO_MEM;
    }

    s_stage_lock = xSemaphoreCreateMutex();
    if (s_stage_lock == NULL) {
        ESP_LOGE(TAG, "发送暂存区互斥锁创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_state = APP_STATE_IDLE;
    ESP_LOGI(TAG, "队列就绪：key=%d audio=%d pcm=%d asr=%d llm=%d chat=%d",
             APP_KEY_QUEUE_LEN, APP_AUDIO_QUEUE_LEN, APP_PCM_QUEUE_LEN,
             APP_ASR_QUEUE_LEN, APP_LLM_QUEUE_LEN, APP_CHAT_QUEUE_LEN);
    return ESP_OK;
}

/* ============================ 队列句柄 ============================ */

QueueHandle_t app_bus_key_queue(void)   { return key_event_queue; }
QueueHandle_t app_bus_audio_queue(void) { return audio_ctrl_queue; }
QueueHandle_t app_bus_pcm_queue(void)   { return pcm_queue; }
QueueHandle_t app_bus_asr_queue(void)   { return asr_result_queue; }
QueueHandle_t app_bus_llm_queue(void)   { return llm_result_queue; }
QueueHandle_t app_bus_chat_queue(void)  { return chat_msg_queue; }

/* ============================ 状态机 ============================ */

void app_bus_set_state(app_state_t state)
{
    if (s_state != state) {
        ESP_LOGI(TAG, "状态: %s -> %s", app_bus_state_text(s_state), app_bus_state_text(state));
    }
    s_state = state;
}

app_state_t app_bus_get_state(void)
{
    return s_state;
}

const char *app_bus_state_text(app_state_t state)
{
    switch (state) {
    case APP_STATE_IDLE:        return "按住按键说话";
    case APP_STATE_RECORDING:   return "录音中... 松开发送";
    case APP_STATE_UPLOADING:   return "识别中...";
    case APP_STATE_LLM_REQUEST: return "AI 正在回复...";
    default:                    return "未知状态";
    }
}

/* ============================ 投递接口 ============================ */

esp_err_t app_bus_post_key(app_key_event_t event)
{
    if (key_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(key_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "按键事件队列满，丢弃事件 %d", (int)event);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_audio_cmd(app_audio_cmd_id_t id, bool discard)
{
    if (audio_ctrl_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    app_audio_cmd_t cmd = { .id = id, .discard = discard };
    if (xQueueSend(audio_ctrl_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "录音命令队列满，丢弃命令 %d", (int)id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_pcm(const app_pcm_msg_t *msg)
{
    if (pcm_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(pcm_queue, msg, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "录音数据队列满，本次录音被丢弃");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_asr(bool ok, const char *text)
{
    if (asr_result_queue == NULL || s_stage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 用静态暂存区，避免 4KB 消息落在调用者栈上（见文件上方说明） */
    xSemaphoreTake(s_stage_lock, portMAX_DELAY);

    s_asr_stage.ok = ok;
    if (text != NULL) {
        strncpy(s_asr_stage.text, text, sizeof(s_asr_stage.text) - 1);
        s_asr_stage.text[sizeof(s_asr_stage.text) - 1] = '\0';
    } else {
        s_asr_stage.text[0] = '\0';
    }

    BaseType_t sent = xQueueSend(asr_result_queue, &s_asr_stage, pdMS_TO_TICKS(200));
    xSemaphoreGive(s_stage_lock);

    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "ASR 结果队列满");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_llm(bool ok, const char *text)
{
    if (llm_result_queue == NULL || s_stage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_stage_lock, portMAX_DELAY);

    s_llm_stage.ok = ok;
    if (text != NULL) {
        strncpy(s_llm_stage.text, text, sizeof(s_llm_stage.text) - 1);
        s_llm_stage.text[sizeof(s_llm_stage.text) - 1] = '\0';
    } else {
        s_llm_stage.text[0] = '\0';
    }

    BaseType_t sent = xQueueSend(llm_result_queue, &s_llm_stage, pdMS_TO_TICKS(200));
    xSemaphoreGive(s_stage_lock);

    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "LLM 结果队列满");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_bus_post_chat(chat_msg_type_t type, const char *text)
{
    if (chat_msg_queue == NULL || s_stage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_stage_lock, portMAX_DELAY);

    s_chat_stage.type = type;
    if (text != NULL) {
        strncpy(s_chat_stage.text, text, sizeof(s_chat_stage.text) - 1);
        s_chat_stage.text[sizeof(s_chat_stage.text) - 1] = '\0';
    } else {
        s_chat_stage.text[0] = '\0';
    }

    /* 状态类消息属于“最新值有效”，队列满时丢掉最老的一条再重试，避免状态卡住 */
    BaseType_t sent = xQueueSend(chat_msg_queue, &s_chat_stage, 0);
    if (sent != pdTRUE) {
        if (xQueueReceive(chat_msg_queue, &s_chat_trash, 0) == pdTRUE) {
            sent = xQueueSend(chat_msg_queue, &s_chat_stage, 0);
        }
        ESP_LOGD(TAG, "聊天队列满，丢弃了一条旧消息");
    }

    xSemaphoreGive(s_stage_lock);
    return (sent == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_bus_post_chatf(chat_msg_type_t type, const char *fmt, ...)
{
    if (chat_msg_queue == NULL || s_stage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 直接在静态暂存区里格式化，不用 2KB 的局部 buf */
    xSemaphoreTake(s_stage_lock, portMAX_DELAY);

    s_chat_stage.type = type;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_chat_stage.text, sizeof(s_chat_stage.text), fmt, ap);
    va_end(ap);

    BaseType_t sent = xQueueSend(chat_msg_queue, &s_chat_stage, 0);
    if (sent != pdTRUE) {
        if (xQueueReceive(chat_msg_queue, &s_chat_trash, 0) == pdTRUE) {
            sent = xQueueSend(chat_msg_queue, &s_chat_stage, 0);
        }
    }

    xSemaphoreGive(s_stage_lock);
    return (sent == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
