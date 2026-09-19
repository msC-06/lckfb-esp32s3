/**
 * @file    hzk16_unicode_table.h
 * @brief   Unicode -> GB2312 映射表访问接口（font_hzk16 模块私有头文件）
 *
 * @note    本文件只给 hzk16.c 内部使用，不对外暴露。
 *          表数据由 tools/gen_hzk16_unicode_table.py 自动生成到
 *          hzk16_unicode_table.c 中，并以 static const 形式保存，外部只能通过
 *          下面两个函数拿到只读指针与表长度。
 */

#ifndef __HZK16_UNICODE_TABLE_H
#define __HZK16_UNICODE_TABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一条映射记录：Unicode 码点 -> GB2312 双字节编码 */
typedef struct {
    uint16_t unicode;   /*!< Unicode 码点，如 U+4E2D */
    uint16_t gb2312;    /*!< GB2312 编码，高字节在前，如 0xD6D0 */
} hzk16_uni_map_t;

/**
 * @brief  获取映射表首地址（按 unicode 字段升序，可二分查找）
 * @return 只读表指针，永不为 NULL
 */
const hzk16_uni_map_t *hzk16_uni_table_get(void);

/**
 * @brief  获取映射表条目数量
 */
uint32_t hzk16_uni_table_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __HZK16_UNICODE_TABLE_H */
