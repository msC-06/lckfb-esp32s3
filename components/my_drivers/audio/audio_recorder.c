/**
 * @file    audio_recorder.c
 * @brief   录音器实现：I2S(ES7210) 立体声采集 -> 降混单声道 -> PSRAM 缓冲
 */

#include <string.h>

#include "audio_recorder.h"
#include "audio.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_rec";

/* ============================ 内部常量 ============================ */

/** I2S 采集是立体声（MIC1+MIC2），读取字节数 = 单声道字节数 * 2 */
#define REC_RAW_BYTES       (AUDIO_RECORDER_BLOCK_BYTES * 2)

/** 开始录音前丢弃的块数（清掉 I2S DMA / 编解码器里的旧数据，避免开头有杂音） */
#define REC_FLUSH_BLOCKS    (3)

/* ============================ 内部状态（全部 static） ============================ */

static int16_t *s_pcm        = NULL;    /*!< PSRAM 里的单声道 PCM 缓冲 */
static size_t   s_capacity   = 0;       /*!< 缓冲容量（字节） */
static size_t   s_bytes      = 0;       /*!< 已录字节数 */
static bool     s_ready      = false;   /*!< 缓冲已分配 */
static bool     s_recording  = false;   /*!< 正在录音 */

/** I2S 原始数据暂存区（立体声 16bit，放内部 RAM 即可，只有 1280 字节） */
static int16_t  s_raw[REC_RAW_BYTES / sizeof(int16_t)];

/* ============================ 内部函数 ============================ */

/**
 * @brief  从 I2S 读一块数据并丢弃（用于开始录音前清空 DMA 残留）
 */
static void rec_flush(uint32_t blocks)
{
    size_t read = 0;
    for (uint32_t i = 0; i < blocks; i++) {
        if (audio_record(s_raw, REC_RAW_BYTES, &read, 50) != ESP_OK) {
            break;
        }
    }
}

/* ============================ 初始化 / 释放 ============================ */

esp_err_t audio_recorder_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (!audio_is_initialized()) {
        ESP_LOGE(TAG, "请先调用 audio_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 1. 大块 PCM 缓冲放 PSRAM（480KB，内部 RAM 装不下） */
    s_capacity = AUDIO_RECORDER_MAX_BYTES;
    s_pcm = (int16_t *)heap_caps_malloc(s_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pcm == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配 %u 字节失败", (unsigned)s_capacity);
        s_capacity = 0;
        return ESP_ERR_NO_MEM;
    }
    memset(s_pcm, 0, s_capacity);

    /* 2. 打开麦克风（ES7210）：16kHz / 16bit / 立体声（板上双麦） */
    esp_err_t err = audio_mic_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风初始化失败: %s", esp_err_to_name(err));
        heap_caps_free(s_pcm);
        s_pcm = NULL;
        s_capacity = 0;
        return err;
    }

    s_bytes     = 0;
    s_recording = false;
    s_ready     = true;

    ESP_LOGI(TAG, "录音器就绪：%d Hz / %d bit / 单声道，缓冲 %u 字节（%d 秒，PSRAM）",
             AUDIO_RECORDER_SAMPLE_RATE, AUDIO_RECORDER_BITS,
             (unsigned)s_capacity, AUDIO_RECORDER_MAX_SECONDS);
    return ESP_OK;
}

esp_err_t audio_recorder_deinit(void)
{
    if (s_pcm != NULL) {
        heap_caps_free(s_pcm);
        s_pcm = NULL;
    }
    s_capacity  = 0;
    s_bytes     = 0;
    s_recording = false;
    s_ready     = false;
    return ESP_OK;
}

bool audio_recorder_is_ready(void)
{
    return s_ready;
}

/* ============================ 录音流程 ============================ */

esp_err_t audio_recorder_start(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "录音器未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    s_bytes     = 0;
    s_recording = true;

    /* 丢掉 I2S 里堆积的旧数据，保证第一帧就是“现在”的声音 */
    rec_flush(REC_FLUSH_BLOCKS);

    ESP_LOGI(TAG, "开始录音（最多 %d 秒）", AUDIO_RECORDER_MAX_SECONDS);
    return ESP_OK;
}

esp_err_t audio_recorder_capture_block(uint32_t timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_recording) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_bytes >= s_capacity) {
        return ESP_OK;      /* 已经满了，什么都不做，由上层判断结束 */
    }

    /* 1. 从 I2S 读一块立体声数据 */
    size_t read = 0;
    esp_err_t err = audio_record(s_raw, REC_RAW_BYTES, &read, timeout_ms);
    if (err != ESP_OK || read == 0) {
        return (err == ESP_OK) ? ESP_ERR_TIMEOUT : err;
    }

    /* 2. (L + R) / 2 降混成单声道，写进 PSRAM 缓冲 */
    size_t frames = read / (2 * sizeof(int16_t));        /* 立体声帧数 */
    size_t room   = (s_capacity - s_bytes) / sizeof(int16_t);
    if (frames > room) {
        frames = room;                                   /* 缓冲放不下就截断 */
    }

    int16_t *dst = &s_pcm[s_bytes / sizeof(int16_t)];
    for (size_t i = 0; i < frames; i++) {
        int32_t mixed = ((int32_t)s_raw[2 * i] + (int32_t)s_raw[2 * i + 1]) / 2;
        dst[i] = (int16_t)mixed;
    }
    s_bytes += frames * sizeof(int16_t);

    if (s_bytes >= s_capacity) {
        ESP_LOGW(TAG, "录音缓冲已满（%d 秒），自动停止", AUDIO_RECORDER_MAX_SECONDS);
    }
    return ESP_OK;
}

esp_err_t audio_recorder_stop(int16_t **pcm_out, size_t *bytes_out)
{
    if (pcm_out == NULL || bytes_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_recording = false;
    *pcm_out    = s_pcm;
    *bytes_out  = s_bytes;

    ESP_LOGI(TAG, "停止录音：%u 字节 / %u ms",
             (unsigned)s_bytes, (unsigned)audio_recorder_recorded_ms());
    return ESP_OK;
}

esp_err_t audio_recorder_abort(void)
{
    s_recording = false;
    s_bytes     = 0;
    return ESP_OK;
}

/* ============================ 状态查询 ============================ */

bool audio_recorder_is_recording(void)
{
    return s_recording;
}

bool audio_recorder_is_full(void)
{
    return (s_capacity > 0) && (s_bytes >= s_capacity);
}

uint32_t audio_recorder_recorded_ms(void)
{
    return (uint32_t)((uint64_t)s_bytes * 1000ULL /
                      (AUDIO_RECORDER_SAMPLE_RATE * (AUDIO_RECORDER_BITS / 8)));
}

size_t audio_recorder_recorded_bytes(void)
{
    return s_bytes;
}

size_t audio_recorder_capacity(void)
{
    return s_capacity;
}
