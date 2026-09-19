/**
 * @file    audio.h
 * @brief   板载音频驱动（ES8311 喇叭 + ES7210 麦克风 + PCA9557 功放使能）
 *
 * 分层：本文件属于 my_drivers（ES8311/ES7210 是板上外部芯片），
 *       底层用 bsp 的 I2C 接口 + ESP-IDF 的 I2S 外设驱动（esp_codec_dev 组件）。
 *       原来放在 bsp/bsp_audio.c，按分层规则迁移到这里。
 *
 * 硬件连接（与旧工程一致）：
 *   MCLK=GPIO38  SCLK=GPIO14  LRCK=GPIO13  DOUT=GPIO45  DIN=GPIO12
 *   ES8311 I2C 地址 0x30(8bit)、ES7210 I2C 地址 0x82(8bit)
 */

#ifndef __AUDIO_H
#define __AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 默认参数 ============================ */

#define AUDIO_SAMPLE_RATE_DEFAULT   16000   /*!< 默认采样率 */
#define AUDIO_BIT_WIDTH_DEFAULT     16      /*!< 默认位宽 */
#define AUDIO_CHANNELS_DEFAULT      2       /*!< 默认声道数 */
#define AUDIO_VOLUME_DEFAULT        60      /*!< 默认音量 0~100 */
#define AUDIO_ADC_GAIN_DB_DEFAULT   24.0f   /*!< 麦克风输入增益(dB) */

/* ============================ 初始化/反初始化 ============================ */

/**
 * @brief  初始化音频：I2S 总线 + ES8311 扬声器 + 功放控制
 *
 * @note   可重复调用；失败时返回错误码（不会 abort），上层可忽略音频继续跑。
 */
esp_err_t audio_init(void);

/**
 * @brief  反初始化音频（关闭功放、释放 I2S 与编解码器）
 */
esp_err_t audio_deinit(void);

/**
 * @brief  音频是否已初始化
 */
bool audio_is_initialized(void);

/* ============================ 句柄 ============================ */

/** 扬声器设备句柄（未初始化返回 NULL） */
esp_codec_dev_handle_t audio_get_speaker(void);

/** 麦克风设备句柄（未初始化返回 NULL） */
esp_codec_dev_handle_t audio_get_mic(void);

/** 原始 I2S 发送通道句柄（需要绕过编解码器直接写 I2S 时使用，可为 NULL） */
i2s_chan_handle_t audio_get_tx_chan(void);

/**
 * @brief  初始化麦克风（ES7210）
 * @note   不用麦克风可以不调用，节省内存与启动时间
 */
esp_err_t audio_mic_init(void);

/* ============================ 播放/录音 ============================ */

/**
 * @brief  设置采样参数（会重新配置 I2S 与编解码器）
 *
 * @param  rate  采样率，如 16000 / 44100
 * @param  bits  位宽，如 16 / 24
 * @param  ch    声道模式：I2S_SLOT_MODE_MONO / I2S_SLOT_MODE_STEREO
 */
esp_err_t audio_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t ch);

/**
 * @brief  播放 PCM 数据（自动打开功放）
 *
 * @param  data           PCM 数据（16bit 小端，双声道交织）
 * @param  len            字节数
 * @param  bytes_written  实际写入字节数，可为 NULL
 * @param  timeout_ms     超时
 */
esp_err_t audio_play(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms);

/**
 * @brief  录音（读取 PCM 数据，需先 audio_mic_init()）
 */
esp_err_t audio_record(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/* ============================ 音量/静音/功放 ============================ */

/** 设置喇叭音量 0~100，返回实际生效值（可为 NULL） */
esp_err_t audio_set_volume(int volume, int *volume_set);

/** 读取当前音量 */
esp_err_t audio_get_volume(int *volume);

/** 喇叭静音开关 */
esp_err_t audio_mute(bool enable);

/** 麦克风输入增益(dB) */
esp_err_t audio_set_mic_gain(float db);

/** 功放使能（PCA9557 PA_EN），播放前打开、播完可以关掉省电 */
esp_err_t audio_pa_enable(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_H */
