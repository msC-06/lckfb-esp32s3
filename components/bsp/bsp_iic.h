#ifndef __I2C_MASTER_H
#define __I2C_MASTER_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// I2C硬件配置
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_SDA_IO       GPIO_NUM_1
#define I2C_MASTER_SCL_IO       GPIO_NUM_2
#define I2C_MASTER_FREQ_HZ      100000U
#define I2C_MASTER_TIMEOUT_MS   1000

/**
 * @brief 初始化I2C主机总线
 */
esp_err_t bsp_i2c_master_init(void);

/**
 * @brief 注销I2C主机总线
 */
esp_err_t bsp_i2c_master_deinit(void);

/**
 * @brief 获取I2C总线句柄，用于挂载设备
 */
i2c_master_bus_handle_t bsp_i2c_get_bus_handle(void);

/**
 * @brief I2C写寄存器
 * @param dev_addr 从机7位地址
 * @param reg_addr 寄存器地址
 * @param data 写入数据缓冲区
 * @param len 数据长度
 */
esp_err_t bsp_i2c_write_reg(uint8_t dev_addr, uint8_t reg_addr, const uint8_t *data, size_t len);

/**
 * @brief I2C读寄存器
 * @param dev_addr 从机7位地址
 * @param reg_addr 寄存器地址
 * @param data 读出缓冲区
 * @param len 读取长度
 */
esp_err_t bsp_i2c_read_reg(uint8_t dev_addr, uint8_t reg_addr, uint8_t *data, size_t len);

/**
 * @brief I2C设备探测，模拟probe
 * @param bus_handle i2c总线句柄
 * @param dev_addr 7位从机地址
 * @param timeout_ms 超时
 * @return ESP_OK:设备存在；其他：不存在
 */
esp_err_t bsp_i2c_probe_device(i2c_master_bus_handle_t bus_handle, uint8_t dev_addr, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
