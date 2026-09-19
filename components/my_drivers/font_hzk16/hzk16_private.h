/**
 * @file    hzk16_private.h
 * @brief   font_hzk16 模块内部接口（只在模块内的 .c 文件之间使用，不对外暴露）
 *
 * @note    应用层只需要包含 hzk16.h；本文件里的函数属于实现细节，随时可能调整。
 */

#ifndef __HZK16_PRIVATE_H
#define __HZK16_PRIVATE_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  把 Unicode 码点转换成 GB2312 双字节编码（查表 + 二分查找）
 *
 * @param  unicode Unicode 码点
 * @return GB2312 编码（高字节在前）；0 表示该字符不在 GB2312 字库里
 */
uint16_t hzk16_unicode_to_gb(uint32_t unicode);

/**
 * @brief  按 GB2312 编码读取点阵数据（不做 Unicode 转换，LVGL 取字模时直接用它）
 *
 * @param  gb2312     GB2312 双字节编码，高字节在前，例如 0xD6D0 = '中'
 * @param  bitmap_out 输出缓冲区，长度必须为 HZK16_GLYPH_BYTES(32)
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND / esp_partition_read 错误码
 */
esp_err_t hzk16_read_glyph_by_gb(uint16_t gb2312, uint8_t *bitmap_out);

#ifdef __cplusplus
}
#endif

#endif /* __HZK16_PRIVATE_H */
