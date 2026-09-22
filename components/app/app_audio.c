/**
 * @file    app_audio.c
 * @brief   音频采集任务实现
 */

#include "app_audio.h"
#include "app_bus.h"
#include "app_config.h"

#include "my_drivers/audio/audio_recorder.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO";

/** 单块采集的 I2S 超时（毫秒） */
#define AUDIO_READ_TIMEOUT_MS   100

/* ============================ 内部函数 ============================ */

/**
 * @brief  停止录音并处理数据
 *
 * @param  discard true = 丢弃本次数据（录音太短）
 * @return 上交的数据字节数（丢弃或失败时为 0）
 */
static size_t audio_finish(bool discard)
{
    int16_t *pcm   = NULL;
    size_t   bytes = 0;

    if (audio_recorder_stop(&pcm, &bytes) != ESP_OK) {
        return 0;
    }

    if (discard) {
        ESP_LOGI(TAG, "丢弃本次录音（%u 字节）", (unsigned)bytes);
        audio_recorder_abort();
        return 0;
    }

    if (pcm == NULL || bytes == 0) {
        ESP_LOGW(TAG, "录音为空，不上传");
        app_bus_post_chat(CHAT_MSG_ERROR, "没有录到声音");
        app_bus_post_asr(false, "没有录到声音");
        return 0;
    }

    app_pcm_msg_t msg = {
        .pcm         = pcm,
        .bytes       = bytes,
        .duration_ms = audio_recorder_recorded_ms(),
    };

    if (app_bus_post_pcm(&msg) != ESP_OK) {
        app_bus_post_chat(CHAT_MSG_ERROR, "网络任务忙，请稍后再试");
        app_bus_post_asr(false, "系统忙，请稍后再试");
        return 0;
    }

    ESP_LOGI(TAG, "录音上交：%u 字节 / %u ms", (unsigned)bytes, (unsigned)msg.duration_ms);
    return bytes;
}

/* ============================ 初始化 ============================ */

esp_err_t app_audio_init(void)
{
    esp_err_t err = audio_recorder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "录音器初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* ============================ 任务 ============================ */

void app_audio_task(void *arg)
{
    (void)arg;

    QueueHandle_t q = app_bus_audio_queue();
    if (q == NULL) {
        ESP_LOGE(TAG, "录音命令队列未初始化，任务退出");
        vTaskDelete(NULL);
        return;
    }

    bool recording = false;
    app_audio_cmd_t cmd;

    ESP_LOGI(TAG, "音频采集任务启动（Core %d，优先级 %d）", xPortGetCoreID(), uxTaskPriorityGet(NULL));

    for (;;) {
        /* ---------------- 空闲：等 START 命令 ---------------- */
        if (!recording) {
            if (xQueueReceive(q, &cmd, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (cmd.id != APP_AUDIO_CMD_START) {
                ESP_LOGD(TAG, "空闲状态忽略命令 %d", (int)cmd.id);
                continue;
            }
            if (audio_recorder_start() != ESP_OK) {
                ESP_LOGE(TAG, "开始录音失败");
                app_bus_post_chat(CHAT_MSG_ERROR, "麦克风启动失败");
                app_bus_post_asr(false, "麦克风启动失败");
                continue;
            }
            recording = true;
            continue;
        }

        /* ---------------- 录音中：先看有没有 STOP ---------------- */
        if (xQueueReceive(q, &cmd, 0) == pdTRUE) {
            if (cmd.id == APP_AUDIO_CMD_STOP) {
                recording = false;
                audio_finish(cmd.discard);
                continue;
            }
            ESP_LOGW(TAG, "录音中收到重复的 START，忽略");
        }

        /* ---------------- 缓冲写满（15 秒）自动收尾 ---------------- */
        if (audio_recorder_is_full()) {
            recording = false;
            audio_finish(false);
            continue;
        }

        /* ---------------- 采集一块 ---------------- */
        esp_err_t err = audio_recorder_capture_block(AUDIO_READ_TIMEOUT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "I2S 暂时没有数据");
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "采集失败: %s", esp_err_to_name(err));
            recording = false;
            audio_finish(true);
            app_bus_post_chat(CHAT_MSG_ERROR, "录音出错，请重试");
            app_bus_post_asr(false, "录音出错，请重试");
        }
    }
}
