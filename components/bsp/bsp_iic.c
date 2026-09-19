#include "bsp_iic.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bsp_i2c";
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

esp_err_t bsp_i2c_master_init(void)
{
    if (s_i2c_bus_handle != NULL) {
        ESP_LOGW(TAG, "I2C bus already initialized");
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        }
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c new master bus fail, ret=%d", ret);
        s_i2c_bus_handle = NULL;
    }
    return ret;
}

esp_err_t bsp_i2c_master_deinit(void)
{
    if (s_i2c_bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2c_del_master_bus(s_i2c_bus_handle);
    s_i2c_bus_handle = NULL;
    return ret;
}

i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void)
{
    return s_i2c_bus_handle;
}

esp_err_t bsp_i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *data, size_t len)
{
    if (s_i2c_bus_handle == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t dev_handle;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        }
    };

    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &dev_handle);
    if(ret != ESP_OK) return ret;

    uint8_t *tx_buf = malloc(len + 1);
    if(tx_buf == NULL) {
        i2c_master_bus_rm_device(dev_handle);
        return ESP_ERR_NO_MEM;
    }
    tx_buf[0] = reg_addr;
    memcpy(tx_buf + 1, data, len);

    ret = i2c_master_transmit(dev_handle, tx_buf, len + 1, I2C_MASTER_TIMEOUT_MS);

    free(tx_buf);
    i2c_master_bus_rm_device(dev_handle);
    return ret;
}

esp_err_t bsp_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, size_t len)
{
    if (s_i2c_bus_handle == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t dev_handle;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        }
    };

    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus_handle, &dev_cfg, &dev_handle);
    if(ret != ESP_OK) return ret;

    ret = i2c_master_transmit_receive(dev_handle, &reg_addr, 1, data, len, I2C_MASTER_TIMEOUT_MS);

    i2c_master_bus_rm_device(dev_handle);
    return ret;
}

esp_err_t bsp_i2c_probe_device(i2c_master_bus_handle_t bus_handle, uint8_t dev_addr, int timeout_ms)
{
    if (bus_handle == NULL) return ESP_ERR_INVALID_ARG;

    i2c_master_dev_handle_t dev_handle;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        }
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) return ret;

    uint8_t dummy;
    // 发送1字节寄存器地址0，尝试读取1字节，以此判断设备是否应答
    ret = i2c_master_transmit_receive(dev_handle, &(uint8_t){0x00}, 1, &dummy, 1, timeout_ms);

    i2c_master_bus_rm_device(dev_handle);
    return ret;
}



