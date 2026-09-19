/**
 * @file    audio.c
 * @brief   板载音频驱动实现（ES8311 喇叭 + ES7210 麦克风 + PCA9557 功放使能）
 */

#include <string.h>
#include "audio.h"

#include "bsp/bsp_iic.h"
#include "my_drivers/pca9557/pca9557.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio";

/* ============================ 硬件参数 ============================ */

#define AUDIO_I2S_NUM           I2S_NUM_1

#define AUDIO_I2S_MCLK_IO       GPIO_NUM_38
#define AUDIO_I2S_SCLK_IO       GPIO_NUM_14
#define AUDIO_I2S_LRCK_IO       GPIO_NUM_13
#define AUDIO_I2S_DOUT_IO       GPIO_NUM_45
#define AUDIO_I2S_DIN_IO        GPIO_NUM_12

/* ES8311 7 位地址 0x18 -> 8 位 0x30；ES7210 板上 AD1 上拉 -> 8 位 0x82（与旧工程一致） */
#define AUDIO_ES8311_I2C_ADDR   ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_ES7210_I2C_ADDR   0x82

/* ES7210 选用的麦克风通道（板载 2 个麦克风，所以选 MIC1+MIC2） */
#define AUDIO_ES7210_MIC_SEL    (ES7210_SEL_MIC1 | ES7210_SEL_MIC2)

/* ============================ 内部状态（全部 static） ============================ */

static i2s_chan_handle_t              s_tx_chan = NULL;
static i2s_chan_handle_t              s_rx_chan = NULL;
static const audio_codec_data_if_t   *s_i2s_data_if = NULL;
static esp_codec_dev_handle_t         s_speaker = NULL;
static esp_codec_dev_handle_t         s_mic = NULL;

static bool                           s_initialized = false;
static uint32_t                       s_rate = AUDIO_SAMPLE_RATE_DEFAULT;
static uint32_t                       s_bits = AUDIO_BIT_WIDTH_DEFAULT;
static i2s_slot_mode_t                s_channel = I2S_SLOT_MODE_STEREO;

/* ============================ 小工具 ============================ */

/** esp_codec_dev 系列 API 返回 int（0 成功），统一转成 esp_err_t */
static inline esp_err_t codec_check(int ret, const char *what)
{
    if (ret == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s 失败，code=%d", what, ret);
    return ESP_FAIL;
}

/** 位宽数值转 I2S 枚举 */
static i2s_data_bit_width_t bits_to_i2s_width(uint32_t bits)
{
    switch (bits) {
    case 8:  return I2S_DATA_BIT_WIDTH_8BIT;
    case 16: return I2S_DATA_BIT_WIDTH_16BIT;
    case 24: return I2S_DATA_BIT_WIDTH_24BIT;
    case 32: return I2S_DATA_BIT_WIDTH_32BIT;
    default: return I2S_DATA_BIT_WIDTH_16BIT;
    }
}

/* ============================ I2S ============================ */

static esp_err_t audio_i2s_init(void)
{
    if (s_tx_chan && s_rx_chan) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;     /* 自动清空 DMA 里的旧数据，避免播放开始时“噗”声 */

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan),
                        TAG, "创建 I2S 通道失败");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(s_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_to_i2s_width(s_bits), s_channel),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_IO,
            .bclk = AUDIO_I2S_SCLK_IO,
            .ws   = AUDIO_I2S_LRCK_IO,
            .dout = AUDIO_I2S_DOUT_IO,
            .din  = AUDIO_I2S_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    if (s_tx_chan) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
                            TAG, "I2S TX 初始化失败");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "I2S TX 使能失败");
    }
    if (s_rx_chan) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg),
                            TAG, "I2S RX 初始化失败");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "I2S RX 使能失败");
    }

    return ESP_OK;
}

static esp_err_t audio_data_if_init(void)
{
    if (s_i2s_data_if) {
        return ESP_OK;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = AUDIO_I2S_NUM,
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_i2s_data_if == NULL) {
        ESP_LOGE(TAG, "创建 I2S 数据接口失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ============================ 编解码器 ============================ */

static esp_err_t audio_speaker_new(void)
{
    if (s_speaker) {
        return ESP_OK;
    }
    if (bsp_i2c_get_bus_handle() == NULL) {
        ESP_LOGE(TAG, "I2C 总线未初始化，请先调用 bsp_i2c_master_init()");
        return ESP_ERR_INVALID_STATE;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "创建 GPIO 接口失败");
        return ESP_FAIL;
    }

    /* 控制接口：复用 bsp_i2c 上已经创建好的 i2c_master 总线 */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = I2C_MASTER_NUM,
        .addr       = AUDIO_ES8311_I2C_ADDR,
        .bus_handle = bsp_i2c_get_bus_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (i2c_ctrl_if == NULL) {
        ESP_LOGE(TAG, "创建 ES8311 I2C 控制接口失败");
        return ESP_FAIL;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage        = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if     = i2c_ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = GPIO_NUM_NC,     /* 功放由 PCA9557 的 PA_EN 控制 */
        .pa_reverted = false,
        .master_mode = false,           /* ESP32 是 I2S 主机 */
        .use_mclk    = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (es8311_dev == NULL) {
        ESP_LOGE(TAG, "ES8311 初始化失败（检查 I2C 地址 0x%02X 是否应答）", AUDIO_ES8311_I2C_ADDR);
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if  = s_i2s_data_if,
    };
    s_speaker = esp_codec_dev_new(&dev_cfg);
    if (s_speaker == NULL) {
        ESP_LOGE(TAG, "创建扬声器设备失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_mic_init(void)
{
    if (s_mic) {
        return ESP_OK;
    }
    if (!s_initialized) {
        ESP_LOGE(TAG, "请先调用 audio_init()");
        return ESP_ERR_INVALID_STATE;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = I2C_MASTER_NUM,
        .addr       = AUDIO_ES7210_I2C_ADDR,
        .bus_handle = bsp_i2c_get_bus_handle(),
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (i2c_ctrl_if == NULL) {
        ESP_LOGE(TAG, "创建 ES7210 I2C 控制接口失败");
        return ESP_FAIL;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if      = i2c_ctrl_if,
        .master_mode  = false,
        .mic_selected = AUDIO_ES7210_MIC_SEL,
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
    if (es7210_dev == NULL) {
        ESP_LOGE(TAG, "ES7210 初始化失败（检查 I2C 地址 0x%02X 是否应答）", AUDIO_ES7210_I2C_ADDR);
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if  = s_i2s_data_if,
    };
    s_mic = esp_codec_dev_new(&dev_cfg);
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "创建麦克风设备失败");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = s_rate,
        .channel         = s_channel,
        .bits_per_sample = s_bits,
    };
    ESP_RETURN_ON_ERROR(codec_check(esp_codec_dev_open(s_mic, &fs), "打开麦克风"), TAG, "");
    esp_codec_dev_set_in_gain(s_mic, AUDIO_ADC_GAIN_DB_DEFAULT);

    ESP_LOGI(TAG, "ES7210 麦克风初始化完成");
    return ESP_OK;
}

/* ============================ 对外接口 ============================ */

esp_err_t audio_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (bsp_i2c_get_bus_handle() == NULL) {
        ESP_LOGE(TAG, "I2C 总线未初始化，请先调用 bsp_i2c_master_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 功放先关掉，避免上电爆音 */
    pca9557_pa_en(0);

    esp_err_t ret = audio_i2s_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_data_if_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_speaker_new();
    if (ret != ESP_OK) {
        return ret;
    }

    s_initialized = true;

    /* 用默认采样率打开扬声器 */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = s_rate,
        .channel         = s_channel,
        .bits_per_sample = s_bits,
    };
    ret = codec_check(esp_codec_dev_open(s_speaker, &fs), "打开扬声器");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "扬声器打开失败，可能是 ES8311 没有应答");
        return ret;
    }
    esp_codec_dev_set_out_vol(s_speaker, AUDIO_VOLUME_DEFAULT);

    ESP_LOGI(TAG, "音频初始化完成（%u Hz / %u bit / %d 声道）",
             (unsigned)s_rate, (unsigned)s_bits, (int)s_channel);
    return ESP_OK;
}

esp_err_t audio_deinit(void)
{
    pca9557_pa_en(0);

    if (s_mic) {
        esp_codec_dev_close(s_mic);
        esp_codec_dev_delete(s_mic);
        s_mic = NULL;
    }
    if (s_speaker) {
        esp_codec_dev_close(s_speaker);
        esp_codec_dev_delete(s_speaker);
        s_speaker = NULL;
    }
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
    }
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
    s_i2s_data_if = NULL;       /* 由 esp_codec_dev 内部释放 */
    s_initialized = false;

    ESP_LOGI(TAG, "音频已关闭");
    return ESP_OK;
}

bool audio_is_initialized(void)
{
    return s_initialized;
}

esp_codec_dev_handle_t audio_get_speaker(void)
{
    return s_speaker;
}

esp_codec_dev_handle_t audio_get_mic(void)
{
    return s_mic;
}

i2s_chan_handle_t audio_get_tx_chan(void)
{
    return s_tx_chan;
}

esp_err_t audio_set_fs(uint32_t rate, uint32_t bits, i2s_slot_mode_t ch)
{
    if (rate == 0 || bits == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_rate    = rate;
    s_bits    = bits;
    s_channel = ch;

    /* 1. 重新配置 I2S 时钟与槽位（需要先关通道） */
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_to_i2s_width(bits), ch);

    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg), TAG, "TX 时钟配置失败");
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_tx_chan, &slot_cfg), TAG, "TX 槽位配置失败");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "TX 使能失败");
    }
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_rx_chan, &clk_cfg), TAG, "RX 时钟配置失败");
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_rx_chan, &slot_cfg), TAG, "RX 槽位配置失败");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "RX 使能失败");
    }

    /* 2. 通知编解码器新的采样参数 */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = rate,
        .channel         = ch,
        .bits_per_sample = bits,
    };

    if (s_speaker) {
        esp_codec_dev_close(s_speaker);
        codec_check(esp_codec_dev_open(s_speaker, &fs), "重新打开扬声器");
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
        codec_check(esp_codec_dev_open(s_mic, &fs), "重新打开麦克风");
    }
    return ESP_OK;
}

esp_err_t audio_play(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_speaker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 播放前打开功放 */
    pca9557_pa_en(1);

    int ret = esp_codec_dev_write(s_speaker, (void *)data, (int)len);
    if (bytes_written) {
        *bytes_written = (ret == ESP_CODEC_DEV_OK) ? len : 0;
    }
    (void)timeout_ms;   /* esp_codec_dev_write 内部阻塞写，无超时参数 */

    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "播放失败，code=%d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_record(void *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "麦克风未初始化，请先调用 audio_mic_init()");
        return ESP_ERR_INVALID_STATE;
    }

    int ret = esp_codec_dev_read(s_mic, data, (int)len);
    if (bytes_read) {
        *bytes_read = (ret == ESP_CODEC_DEV_OK) ? len : 0;
    }
    (void)timeout_ms;

    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "录音失败，code=%d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_set_volume(int volume, int *volume_set)
{
    if (s_speaker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;

    ESP_RETURN_ON_ERROR(codec_check(esp_codec_dev_set_out_vol(s_speaker, volume), "设置音量"), TAG, "");
    if (volume_set) {
        *volume_set = volume;
    }
    return ESP_OK;
}

esp_err_t audio_get_volume(int *volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_speaker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec_check(esp_codec_dev_get_out_vol(s_speaker, volume), "读取音量");
}

esp_err_t audio_mute(bool enable)
{
    if (s_speaker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec_check(esp_codec_dev_set_out_mute(s_speaker, enable), "设置静音");
}

esp_err_t audio_set_mic_gain(float db)
{
    if (s_mic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec_check(esp_codec_dev_set_in_gain(s_mic, db), "设置麦克风增益");
}

esp_err_t audio_pa_enable(bool enable)
{
    return pca9557_pa_en(enable ? 1 : 0);
}
