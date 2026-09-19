/**
 * @file    pca9557.h
 * @brief   PCA9557 IO 扩展芯片驱动（板载 IO0=LCD_CS, IO1=PA_EN, IO2=DVP_PWDN）
 *
 * 分层：本文件属于 my_drivers（外部芯片驱动），底层通过 bsp 的 I2C 接口访问。
 *       原来放在 bsp/bsp_pca9557.c，按分层规则迁移到这里。
 *
 * 注意：IO3~IO7 一定要保持“输入(高阻)”，不要一起配成输出，否则会误驱动板上其它电路。
 */

#ifndef __PCA9557_H
#define __PCA9557_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 寄存器 ========== */
#define PCA9557_REG_INPUT_PORT              0x00
#define PCA9557_REG_OUTPUT_PORT             0x01
#define PCA9557_REG_POLARITY_INVERSION_PORT 0x02
#define PCA9557_REG_CONFIGURATION_PORT      0x03

/* ========== I2C 从机地址 ========== */
#define PCA9557_I2C_ADDR                    0x19

/* ========== 板载引脚（位掩码） ========== */
#define PCA9557_PIN_LCD_CS                  (1u << 0)  /* LCD 片选，0 = 选中 */
#define PCA9557_PIN_PA_EN                   (1u << 1)  /* 音频功放使能，1 = 打开 */
#define PCA9557_PIN_DVP_PWDN                (1u << 2)  /* 摄像头掉电，0 = 摄像头工作 */

/* 只把 IO0~IO2 配成输出，其余保持输入 */
#define PCA9557_CONFIG_DEFAULT              (0xF8)
/* 上电默认输出：LCD_CS=1(不选中)、PA_EN=0(功放关)、DVP_PWDN=1(摄像头断电) */
#define PCA9557_OUTPUT_DEFAULT              (0x05)

/**
 * @brief  初始化 PCA9557（写输出/配置/极性寄存器）
 * @note   需要先调用 bsp_i2c_master_init()；可重复调用
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE I2C 未初始化；其他为 I2C 错误码
 */
esp_err_t pca9557_init(void);

/**
 * @brief  按位掩码设置输出电平（内部有影子寄存器，避免读改写竞态）
 * @param  mask  要修改的引脚掩码，如 PCA9557_PIN_PA_EN
 * @param  level 0 / 1
 */
esp_err_t pca9557_set_output(uint8_t mask, uint8_t level);

/**
 * @brief  设置单个引脚电平（语义同 pca9557_set_output）
 */
esp_err_t pca9557_set_pin(uint8_t pin_mask, uint8_t level);

/* ========== 板载功能封装 ========== */

/** LCD 片选：0 = 选中（可以和屏通信），1 = 取消选中 */
esp_err_t pca9557_lcd_cs(uint8_t level);

/** 音频功放使能：1 = 打开喇叭输出，0 = 关闭 */
esp_err_t pca9557_pa_en(uint8_t level);

/** 摄像头掉电控制：0 = 摄像头工作，1 = 摄像头掉电 */
esp_err_t pca9557_dvp_pwdn(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* __PCA9557_H */
