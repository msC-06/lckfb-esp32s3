/**
 * @file    app_net.c
 * @brief   网络请求任务实现（ASR + LLM 串联）
 */

#include <string.h>

#include "app_net.h"
#include "app_bus.h"
#include "app_config.h"
#include "app_asr.h"
#include "app_llm.h"
#include "app_wifi.h"

#include "esp_log.h"
#include "esp_attr.h"               /* EXT_RAM_BSS_ATTR：大缓冲放 PSRAM */
#include "app_mem.h"                /* 关键路径打点：内部堆余量 */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "APP_NET";

/** 发请求前最多等 WiFi 多久（毫秒） */
#define NET_WAIT_WIFI_MS    20000

/* ============================ 内部状态 ============================ */

/**
 * 文本缓冲放静态区，不占任务栈：
 * net 任务栈只有 8KB，TLS 握手本身就要好几 KB。
 * EXT_RAM_BSS_ATTR 把它们放到 PSRAM，不占内部 RAM（需开
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY；没开就退回内部 RAM）。
 */
EXT_RAM_BSS_ATTR static char s_asr_text[APP_TEXT_MAX_LEN];
EXT_RAM_BSS_ATTR static char s_llm_reply[APP_TEXT_MAX_LEN];

/* ============================ 对外接口 ============================ */

esp_err_t app_net_init(void)
{
    esp_err_t err = app_asr_init();
    if (err != ESP_OK) {
        return err;
    }
    return app_llm_init();
}

void app_net_task(void *arg)
{
    (void)arg;

    QueueHandle_t q = app_bus_pcm_queue();
    if (q == NULL) {
        ESP_LOGE(TAG, "录音数据队列未初始化，任务退出");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "网络任务启动（Core %d，优先级 %d）", xPortGetCoreID(), uxTaskPriorityGet(NULL));

    for (;;) {
        app_pcm_msg_t msg;

        /* 1. 等一段录音数据（阻塞，不占 CPU） */
        if (xQueueReceive(q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "收到录音：%u 字节 / %u ms", (unsigned)msg.bytes, (unsigned)msg.duration_ms);

        /* 2. 没联网先等一下（刚开机时用户可能马上说话） */
        if (!app_wifi_is_connected()) {
            app_bus_post_chat(CHAT_MSG_STATUS, "等待 WiFi 连接...");
            if (!app_wifi_wait_connected(NET_WAIT_WIFI_MS)) {
                ESP_LOGE(TAG, "WiFi 未连接，放弃本次识别");
                app_bus_post_asr(false, "WiFi 未连接");
                continue;
            }
        }

        /* 3. 云端识别（百度 ASR） */
        app_mem_log_brief("识别前");        /* 打点：这一轮的内部堆余量 */
        s_asr_text[0] = '\0';
        esp_err_t err = app_asr_recognize((const int16_t *)msg.pcm, msg.bytes,
                                         s_asr_text, sizeof(s_asr_text));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ASR 失败: %s（%s）", esp_err_to_name(err), s_asr_text);
            app_bus_post_asr(false, s_asr_text);
            continue;
        }

        /* 4. 拿到识别结果就**先让用户的话上屏**，再去问大模型。
         *    关键：这里直接投递气泡，不等 app_key_task 轮询转发，
         *    也不等 LLM 请求结束——用户会先看到自己说的话，几秒后才看到回答。 */
        app_bus_post_asr(true, s_asr_text);                     /* 给业务状态机（切状态用） */
        app_bus_post_chat(CHAT_MSG_USER, s_asr_text);           /* 立刻上屏：用户气泡 */

        /* 5. 大模型对话（DeepSeek，自动带上下文） */
        s_llm_reply[0] = '\0';
        err = app_llm_chat(s_asr_text, s_llm_reply, sizeof(s_llm_reply));
        if (err == ESP_OK) {
            app_bus_post_llm(true, s_llm_reply);
        } else {
            ESP_LOGW(TAG, "LLM 失败: %s（%s）", esp_err_to_name(err), s_llm_reply);
            app_bus_post_llm(false, s_llm_reply);
        }
    }
}
