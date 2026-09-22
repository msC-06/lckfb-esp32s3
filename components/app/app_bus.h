/**
 * @file    app_bus.h
 * @brief   应用总线：业务状态机 + 各任务之间的 FreeRTOS 队列（消息定义与投递接口）
 *
 * 分层：app 层。所有跨任务通信都走这里，模块之间不直接持有对方的队列句柄。
 *
 * 队列一览（对应设计文档“双核任务划分”）：
 *   key_event_queue   按键事件        app_key(esp_timer)     -> app_key_task
 *   audio_ctrl_queue  录音启停命令    app_key_task           -> app_audio_task
 *   pcm_queue         录音 PCM 数据   app_audio_task         -> app_net_task
 *   asr_result_queue  ASR 识别结果    app_net_task           -> app_key_task
 *   llm_result_queue  LLM 回复结果    app_net_task           -> app_key_task
 *   chat_msg_queue    聊天/状态消息   任意任务               -> app_chat_ui(LVGL 定时器)
 *
 * 线程安全：本模块所有投递接口都可以在任意任务/定时器回调里调用。
 */

#ifndef __APP_BUS_H
#define __APP_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 业务状态机 ============================ */

/** 业务状态：IDLE -> RECORDING -> UPLOADING -> LLM_REQUEST -> IDLE */
typedef enum {
    APP_STATE_IDLE = 0,         /*!< 待机：按住按键说话 */
    APP_STATE_RECORDING,        /*!< 录音中：采集 PCM */
    APP_STATE_UPLOADING,        /*!< 识别中：PCM 已投递给网络任务，正在调 ASR */
    APP_STATE_LLM_REQUEST,      /*!< AI 正在回复：正在调 DeepSeek */
} app_state_t;

/* ============================ 消息类型 ============================ */

/** 按键事件 */
typedef enum {
    APP_KEY_EVENT_PRESS = 0,    /*!< 已按下（消抖后） */
    APP_KEY_EVENT_RELEASE,      /*!< 已松开（消抖后） */
    APP_KEY_EVENT_TIMEOUT,      /*!< 录音超时（15 秒）自动结束 */
} app_key_event_t;

/** 录音控制命令 ID */
typedef enum {
    APP_AUDIO_CMD_START = 0,    /*!< 开始录音 */
    APP_AUDIO_CMD_STOP,         /*!< 停止录音 */
} app_audio_cmd_id_t;

/** 录音控制命令 */
typedef struct {
    app_audio_cmd_id_t id;      /*!< 命令 */
    bool               discard; /*!< STOP 时是否丢弃数据（录音太短时用） */
} app_audio_cmd_t;

/** 录音数据（音频任务 -> 网络任务） */
typedef struct {
    int16_t *pcm;               /*!< PCM 首地址（PSRAM，由网络任务用完即止） */
    size_t   bytes;             /*!< 字节数（16bit 单声道） */
    uint32_t duration_ms;       /*!< 录音时长 */
} app_pcm_msg_t;

/** 文本结果（ASR / LLM） */
typedef struct {
    bool ok;                            /*!< true 成功，false 失败 */
    char text[APP_TEXT_MAX_LEN];        /*!< 文本；失败时是错误说明 */
} app_text_msg_t;

/** 聊天消息类型（决定界面上显示在哪儿、什么样式） */
typedef enum {
    CHAT_MSG_USER = 0,          /*!< 用户消息气泡（右对齐，浅蓝） */
    CHAT_MSG_AI,                /*!< AI 回复气泡（左对齐，浅灰） */
    CHAT_MSG_STATUS,            /*!< 底部状态行 */
    CHAT_MSG_ERROR,             /*!< 底部状态行（红色，显示几秒） */
    CHAT_MSG_WIFI,              /*!< 顶部 WiFi 状态 */
} chat_msg_type_t;

/** 聊天消息 */
typedef struct {
    chat_msg_type_t type;                       /*!< 消息类型 */
    char            text[APP_CHAT_TEXT_MAX];    /*!< 文本（UTF-8） */
} chat_msg_t;

/* ============================ 初始化 ============================ */

/**
 * @brief  创建全部队列（在 app_start() 最开始调用，只需一次）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 内存不足；ESP_ERR_INVALID_STATE 重复调用
 */
esp_err_t app_bus_init(void);

/* ============================ 队列句柄 ============================ */

QueueHandle_t app_bus_key_queue(void);      /*!< 按键事件 */
QueueHandle_t app_bus_audio_queue(void);    /*!< 录音启停命令 */
QueueHandle_t app_bus_pcm_queue(void);      /*!< 录音数据 */
QueueHandle_t app_bus_asr_queue(void);      /*!< ASR 结果 */
QueueHandle_t app_bus_llm_queue(void);      /*!< LLM 结果 */
QueueHandle_t app_bus_chat_queue(void);     /*!< 聊天消息 */

/* ============================ 状态机 ============================ */

/** 设置当前业务状态（32 位对齐写，天然原子，任意任务可调用） */
void app_bus_set_state(app_state_t state);

/** 读取当前业务状态 */
app_state_t app_bus_get_state(void);

/** 状态对应的界面提示文字（如 "按住按键说话"） */
const char *app_bus_state_text(app_state_t state);

/* ============================ 投递接口 ============================ */

/** 投递按键事件（app_key 的定时器回调调用） */
esp_err_t app_bus_post_key(app_key_event_t event);

/** 投递录音控制命令 */
esp_err_t app_bus_post_audio_cmd(app_audio_cmd_id_t id, bool discard);

/** 投递录音数据（非阻塞，队列满返回 ESP_ERR_TIMEOUT） */
esp_err_t app_bus_post_pcm(const app_pcm_msg_t *msg);

/** 投递 ASR 结果 */
esp_err_t app_bus_post_asr(bool ok, const char *text);

/** 投递 LLM 结果 */
esp_err_t app_bus_post_llm(bool ok, const char *text);

/** 投递一条聊天消息 */
esp_err_t app_bus_post_chat(chat_msg_type_t type, const char *text);

/** 投递一条聊天消息（printf 风格） */
esp_err_t app_bus_post_chatf(chat_msg_type_t type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* __APP_BUS_H */
