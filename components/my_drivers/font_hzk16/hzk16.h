/**
 * @file    hzk16.h
 * @brief   HZK16（GB2312 16x16 点阵）中文字库驱动 —— 对外接口
 *
 * 字库来源：自定义 flash 分区 fontbin（见工程根目录 partitions.csv）
 * 读取方式：esp_partition_read 按需读取单个汉字（32 字节），
 *           **不会把整个字库读进 RAM**（字库约 255 KB，ESP32-S3 的 RAM 装不下也不必要）。
 *
 * 典型用法（应用层）：
 *      #include "my_drivers/font_hzk16/hzk16.h"
 *      hzk16_init();                                   // 初始化（找分区）
 *      const lv_font_t *font = hzk16_get_lv_font();     // 拿到 LVGL 字体
 *      lv_obj_set_style_text_font(label, font, 0);      // 给 label 用
 */

#ifndef __HZK16_H
#define __HZK16_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 字模参数 ============================ */

#define HZK16_GLYPH_WIDTH    16                              /*!< 点阵宽度（像素） */
#define HZK16_GLYPH_HEIGHT   16                              /*!< 点阵高度（像素） */
#define HZK16_GLYPH_BYTES    (HZK16_GLYPH_WIDTH * HZK16_GLYPH_HEIGHT / 8)  /*!< 每个汉字 32 字节 */
#define HZK16_FONT_HEIGHT    HZK16_GLYPH_HEIGHT               /*!< LVGL 行高（像素） */

/* ============================ 初始化/反初始化 ============================ */

/**
 * @brief  初始化字库驱动：查找并校验 fontbin 分区
 *
 * @note   可重复调用；分区不存在或长度不足时返回错误码，不会 abort。
 * @return ESP_OK 成功；
 *         ESP_ERR_NOT_FOUND 找不到 fontbin 分区；
 *         ESP_ERR_INVALID_SIZE 分区容量装不下字库
 */
esp_err_t hzk16_init(void);

/**
 * @brief  反初始化（仅释放内部句柄，flash 分区本身不受影响）
 */
esp_err_t hzk16_deinit(void);

/**
 * @brief  字库是否已经初始化成功
 */
bool hzk16_is_ready(void);

/* ============================ 取字模 ============================ */

/**
 * @brief  查询某个字符是否有对应汉字点阵
 *
 * @param  unicode Unicode 码点（UTF-8 解码后的值），例如 '中' = 0x4E2D
 * @return true 字库里有该字
 */
bool hzk16_has_glyph(uint32_t unicode);

/**
 * @brief  按需读取单个汉字的点阵数据（只读 32 字节，不入 RAM 缓存）
 *
 * @param  unicode    Unicode 码点，例如 '中' = 0x4E2D
 * @param  bitmap_out 输出缓冲区，长度必须是 HZK16_GLYPH_BYTES(32)
 *                    数据格式：16 行，每行 2 字节；**每字节高位在左**（bit7 = 最左像素），
 *                    第 1 字节是左边 8 个像素，第 2 字节是右边 8 个像素，1 = 亮点
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数为空；
 *         ESP_ERR_INVALID_STATE 未初始化；ESP_ERR_NOT_FOUND 该字不在字库中；
 *         其他为 esp_partition_read 的返回值
 */
esp_err_t hzk16_get_glyph(uint32_t unicode, uint8_t *bitmap_out);

/* ============================ UTF-8 -> GB2312 ============================ */

/**
 * @brief  把 UTF-8 字符串转换成 GB2312 编码（字库的寻址编码）
 *
 * @param  utf8    输入的 UTF-8 字符串（LLM/ASR 返回的文本就是 UTF-8）
 * @param  gb      输出缓冲区（GB2312 字节流，函数会补 '\0'）
 * @param  gb_size 输出缓冲区长度（字节）
 * @return 实际写入的字节数（不含结尾 '\0'）；
 *         参数非法或缓冲区连一个字符都放不下时返回 0
 *
 * @note   规则：
 *           - ASCII（< 0x80）原样输出 1 字节；
 *           - 汉字查表转成 GB2312 双字节（高字节在前）；
 *           - 字库里没有的字符（生僻字、emoji 等）输出 '?'；
 *           - 缓冲区不够时按“完整字符”截断，不会写出半个汉字；
 *           - 非法 UTF-8 字节直接跳过。
 *
 * @note   用 LVGL 显示时其实不需要本函数（hzk16 的 LVGL 字体内部已经做了
 *         Unicode -> GB2312 转换）；本函数用于按字库编码统计/裁剪文本，
 *         或者直接用 hzk16_read_glyph_by_gb() 手工取模绘制。
 */
size_t hzk16_utf8_to_gb2312(const char *utf8, char *gb, size_t gb_size);

/* ============================ LVGL 接入 ============================ */

/**
 * @brief  获取 LVGL 字体对象（内部静态实例，不要 free）
 *
 * @note   ASCII 等非 GB2312 字符本字库不含点阵，需要通过
 *         hzk16_set_fallback_font() 指定一个西文字体做回退。
 */
const lv_font_t *hzk16_get_lv_font(void);

/**
 * @brief  设置缺字回退字体（例如 &lv_font_montserrat_14），传 NULL 取消
 */
void hzk16_set_fallback_font(const lv_font_t *fallback);

#ifdef __cplusplus
}
#endif

#endif /* __HZK16_H */
