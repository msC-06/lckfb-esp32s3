/**
 * @file    audio_recorder.h
 * @brief   录音器：16kHz / 16bit / 单声道 PCM 采集，数据存 PSRAM
 *
 * 分层：本文件属于 my_drivers（依赖同层的 audio.h 编解码接口，不直接碰 bsp）。
 *
 * 数据流：
 *   ES7210(ADC) --I2S RX(立体声)--> audio_record() --降混--> PSRAM 缓冲(单声道 PCM)
 *
 * 关键设计：
 *   1. 板上 ES7210 用的是 MIC1 + MIC2 双麦、I2S 立体声模式，
 *      所以这里读取立体声数据后做 (L+R)/2 降混成单声道，正好是 ASR 要的格式；
 *   2. 缓冲一次性从 PSRAM 分配 15 秒（480000 字节），
 *      录音时从 0 开始顺序写入，写满即自动停止（等价于“15 秒超时”）；
 *   3. 因为是顺序写入不回头，stop() 直接返回缓冲首地址 + 实际长度，
 *      上层（ASR）可以整块使用，不需要处理环形回绕。
 *
 * 线程约定：本模块**不加锁**，start/stop/capture 必须在同一个任务里调用
 *           （本工程里统一由 app_audio_task 负责）。
 */

#ifndef __AUDIO_RECORDER_H
#define __AUDIO_RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 录音参数 ============================ */

/** 采样率（ASR 要求 16000Hz） */
#define AUDIO_RECORDER_SAMPLE_RATE      16000
/** 采样位宽（bit） */
#define AUDIO_RECORDER_BITS             16
/** 声道数（降混后单声道） */
#define AUDIO_RECORDER_CHANNELS         1
/** 单次最多录音秒数（防止忘松按键） */
#define AUDIO_RECORDER_MAX_SECONDS      15
/** PSRAM 缓冲容量（字节）：16000 * 2 * 15 = 480000 */
#define AUDIO_RECORDER_MAX_BYTES        \
    (AUDIO_RECORDER_SAMPLE_RATE * (AUDIO_RECORDER_BITS / 8) * AUDIO_RECORDER_MAX_SECONDS)
/** 每次从 I2S 读取的块时长（毫秒），20ms 一块 */
#define AUDIO_RECORDER_BLOCK_MS         20
/** 单块单声道 PCM 字节数：16000*2*20/1000 = 640 */
#define AUDIO_RECORDER_BLOCK_BYTES      \
    (AUDIO_RECORDER_SAMPLE_RATE * (AUDIO_RECORDER_BITS / 8) * AUDIO_RECORDER_BLOCK_MS / 1000)

/* ============================ 初始化 / 释放 ============================ */

/**
 * @brief  初始化录音器：PSRAM 分配缓冲 + 打开麦克风（16kHz/16bit/立体声采集）
 *
 * @note   要求先调用 audio_init()（I2S 与编解码器就绪）。
 * @return ESP_OK 成功；ESP_ERR_NO_MEM PSRAM 分配失败；
 *         ESP_ERR_INVALID_STATE 音频未初始化；其它为麦克风打开失败的错误码
 */
esp_err_t audio_recorder_init(void);

/**
 * @brief  释放录音器（归还 PSRAM 缓冲，不关闭编解码器）
 */
esp_err_t audio_recorder_deinit(void);

/** 录音器是否已就绪（缓冲已分配且麦克风可用） */
bool audio_recorder_is_ready(void);

/* ============================ 录音流程 ============================ */

/**
 * @brief  开始录音：清空已录长度、丢弃 I2S 里的旧数据、进入采集状态
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t audio_recorder_start(void);

/**
 * @brief  采集一块 PCM（默认 20ms）并写入 PSRAM 缓冲
 *
 * @param  timeout_ms 读取 I2S 的超时（毫秒）
 * @return ESP_OK 写入成功；
 *         ESP_ERR_INVALID_STATE 当前不在录音状态；
 *         ESP_ERR_TIMEOUT I2S 读取超时（一般是没数据）；
 *         其它为读取失败
 * @note   缓冲写满后本函数直接返回 ESP_OK 但不再写入，
 *         调用者用 audio_recorder_is_full() 判断并结束录音。
 */
esp_err_t audio_recorder_capture_block(uint32_t timeout_ms);

/**
 * @brief  停止录音并取出 PCM 数据
 *
 * @param  pcm_out   输出：PCM 数据首地址（16bit 小端单声道，指向 PSRAM 缓冲，
 *                   下次 audio_recorder_start() 会被覆盖，用完请及时处理）
 * @param  bytes_out 输出：实际录到的字节数
 * @return ESP_OK 成功（即使 0 字节也算成功）；ESP_ERR_INVALID_ARG 参数为空
 */
esp_err_t audio_recorder_stop(int16_t **pcm_out, size_t *bytes_out);

/**
 * @brief  放弃本次录音（丢弃已录数据，回到空闲）
 */
esp_err_t audio_recorder_abort(void);

/* ============================ 状态查询 ============================ */

/** 当前是否正在录音 */
bool audio_recorder_is_recording(void);

/** 缓冲是否已经写满（15 秒上限） */
bool audio_recorder_is_full(void);

/** 已录时长（毫秒） */
uint32_t audio_recorder_recorded_ms(void);

/** 已录字节数 */
size_t audio_recorder_recorded_bytes(void);

/** 缓冲容量（字节） */
size_t audio_recorder_capacity(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_RECORDER_H */
