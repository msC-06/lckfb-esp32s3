/**
 * @file    hzk16.c
 * @brief   HZK16 字库底层实现：fontbin 分区访问 + GB2312 区位码寻址 + 按需读取字模
 *
 * 关键知识点（也是这个字库最容易踩坑的地方，详见文件末尾注释）：
 *   1. GB2312 编码 -> 区位码：区号 = 第一字节 - 0xA0，位号 = 第二字节 - 0xA0；
 *      例如 '中' = 0xD6D0 -> 区 54 位 48；
 *   2. 字模偏移 = ((区号 - 1) * 94 + (位号 - 1)) * 32 字节；
 *   3. 点阵是“高位在前”：每行 2 字节，第 1 字节 bit7 是最左像素，1 表示亮点。
 *
 * 内存策略：只按需读取 32 字节，绝不整库加载；表数据是 const，放 flash。
 */

#include <string.h>

#include "hzk16.h"
#include "hzk16_private.h"
#include "hzk16_unicode_table.h"

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "hzk16";

/* ============================ 内部常量 ============================ */

/** 字库所在分区名，必须和 partitions.csv 保持一致 */
#define HZK16_PART_NAME        "fontbin"
/** 自定义 data 子类型，必须和 partitions.csv 的 subtype 一致 */
#define HZK16_PART_SUBTYPE     ((esp_partition_subtype_t)0x40)

/** 每区 94 个位（GB2312 标准） */
#define HZK16_WEI_PER_QU       94U
/** 标准 HZK16 文件有效区号范围 1~87（1~9 符号，16~87 汉字） */
#define HZK16_QU_MIN           1U
#define HZK16_QU_MAX           87U
/** 标准 HZK16 字库有效数据长度：87 * 94 * 32 = 261696 字节 */
#define HZK16_REQUIRED_BYTES   (HZK16_QU_MAX * HZK16_WEI_PER_QU * HZK16_GLYPH_BYTES)

/* ============================ 内部状态（全部 static，不对外暴露） ============================ */

static const esp_partition_t *s_partition = NULL;   /*!< fontbin 分区句柄 */
static bool                   s_ready     = false;  /*!< 是否初始化成功 */

/* ============================ Unicode -> GB2312 ============================ */

uint16_t hzk16_unicode_to_gb(uint32_t unicode)
{
    /* GB2312 只覆盖 BMP，超出范围直接认为没有 */
    if (unicode > 0xFFFFU) {
        return 0;
    }

    const hzk16_uni_map_t *table = hzk16_uni_table_get();
    uint32_t               count = hzk16_uni_table_count();

    /* 表按 unicode 升序，二分查找 */
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2U;
        uint16_t key = table[mid].unicode;
        if (key == (uint16_t)unicode) {
            return table[mid].gb2312;
        }
        if (key < (uint16_t)unicode) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return 0;   /* 不在 GB2312 字库中（例如 ASCII、生僻字） */
}

/* ============================ UTF-8 -> GB2312 ============================ */

/**
 * @brief  解码一个 UTF-8 字符
 *
 * @param  p  输入指针（指向字符首字节）
 * @param  cp 输出 Unicode 码点
 * @return >0 该字符占用的字节数；<0 非法序列
 */
static int hzk16_utf8_decode(const unsigned char *p, uint32_t *cp)
{
    if (p[0] < 0x80U) {                     /* ASCII */
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0U) == 0xC0U) {          /* 2 字节 */
        if ((p[1] & 0xC0U) != 0x80U) {
            return -1;
        }
        *cp = ((uint32_t)(p[0] & 0x1FU) << 6) | (uint32_t)(p[1] & 0x3FU);
        return 2;
    }
    if ((p[0] & 0xF0U) == 0xE0U) {          /* 3 字节（常用汉字） */
        if (((p[1] & 0xC0U) != 0x80U) || ((p[2] & 0xC0U) != 0x80U)) {
            return -1;
        }
        *cp = ((uint32_t)(p[0] & 0x0FU) << 12) |
              ((uint32_t)(p[1] & 0x3FU) << 6) |
              (uint32_t)(p[2] & 0x3FU);
        return 3;
    }
    if ((p[0] & 0xF8U) == 0xF0U) {          /* 4 字节（emoji 等） */
        if (((p[1] & 0xC0U) != 0x80U) || ((p[2] & 0xC0U) != 0x80U) || ((p[3] & 0xC0U) != 0x80U)) {
            return -1;
        }
        *cp = ((uint32_t)(p[0] & 0x07U) << 18) |
              ((uint32_t)(p[1] & 0x3FU) << 12) |
              ((uint32_t)(p[2] & 0x3FU) << 6) |
              (uint32_t)(p[3] & 0x3FU);
        return 4;
    }
    return -1;
}

size_t hzk16_utf8_to_gb2312(const char *utf8, char *gb, size_t gb_size)
{
    if (utf8 == NULL || gb == NULL || gb_size < 2U) {
        return 0;
    }

    const unsigned char *p  = (const unsigned char *)utf8;
    size_t               out = 0;

    while (*p != '\0') {
        uint32_t cp  = 0;
        int      len = hzk16_utf8_decode(p, &cp);
        if (len < 0) {              /* 非法字节：跳过 */
            p++;
            continue;
        }
        p += len;

        if (cp < 0x80U) {           /* ASCII：1 字节原样输出 */
            if (out + 2U > gb_size) {
                break;
            }
            gb[out++] = (char)cp;
            continue;
        }

        uint16_t code = hzk16_unicode_to_gb(cp);
        if (code == 0U) {           /* 字库里没有：输出 '?' */
            if (out + 2U > gb_size) {
                break;
            }
            gb[out++] = '?';
            continue;
        }

        if (out + 3U > gb_size) {   /* 放不下一个完整汉字就停 */
            break;
        }
        gb[out++] = (char)(code >> 8);
        gb[out++] = (char)(code & 0xFFU);
    }

    gb[out] = '\0';
    return out;
}

/* ============================ GB2312 -> 字模偏移 ============================ */

/**
 * @brief  计算 GB2312 编码对应的字模在字库中的字节偏移
 * @param  gb2312  GB2312 双字节编码（高字节在前）
 * @param  offset  输出偏移
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 编码非法（不是汉字/符号区）
 */
static esp_err_t hzk16_calc_offset(uint16_t gb2312, uint32_t *offset)
{
    uint8_t b1 = (uint8_t)(gb2312 >> 8);        /* 第一字节 */
    uint8_t b2 = (uint8_t)(gb2312 & 0xFFU);     /* 第二字节 */

    /* 区号 = 第一字节 - 0xA0，位号 = 第二字节 - 0xA0 */
    if (b1 < 0xA1U || b1 > 0xF7U || b2 < 0xA1U || b2 > 0xFEU) {
        return ESP_ERR_INVALID_ARG;             /* 不在 GB2312 汉字/符号区 */
    }

    uint32_t qu  = (uint32_t)b1 - 0xA0U;        /* 1 ~ 87 */
    uint32_t wei = (uint32_t)b2 - 0xA0U;        /* 1 ~ 94 */

    if (qu < HZK16_QU_MIN || qu > HZK16_QU_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *offset = ((qu - 1U) * HZK16_WEI_PER_QU + (wei - 1U)) * (uint32_t)HZK16_GLYPH_BYTES;
    return ESP_OK;
}

/* ============================ 按需读取字模 ============================ */

esp_err_t hzk16_read_glyph_by_gb(uint16_t gb2312, uint8_t *bitmap_out)
{
    if (bitmap_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || s_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t offset = 0;
    esp_err_t err = hzk16_calc_offset(gb2312, &offset);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 越界保护：分区可能比标准字库小（比如只烧了一部分） */
    if (offset + (uint32_t)HZK16_GLYPH_BYTES > s_partition->size) {
        ESP_LOGW(TAG, "字模越界: gb=0x%04X offset=%u 分区大小=%u",
                 gb2312, (unsigned)offset, (unsigned)s_partition->size);
        return ESP_ERR_NOT_FOUND;
    }

    /* 只读 32 字节，不缓存整库 */
    return esp_partition_read(s_partition, offset, bitmap_out, HZK16_GLYPH_BYTES);
}

esp_err_t hzk16_get_glyph(uint32_t unicode, uint8_t *bitmap_out)
{
    if (bitmap_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t gb = hzk16_unicode_to_gb(unicode);
    if (gb == 0) {
        return ESP_ERR_NOT_FOUND;               /* 字库里没有这个字符 */
    }
    return hzk16_read_glyph_by_gb(gb, bitmap_out);
}

bool hzk16_has_glyph(uint32_t unicode)
{
    return (s_ready && hzk16_unicode_to_gb(unicode) != 0);
}

/* ============================ 初始化/反初始化 ============================ */

esp_err_t hzk16_init(void)
{
    if (s_ready) {
        return ESP_OK;                          /* 重复调用直接成功 */
    }

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, HZK16_PART_SUBTYPE, HZK16_PART_NAME);
    if (part == NULL) {
        ESP_LOGE(TAG, "找不到分区 \"%s\"（subtype=0x40），请检查 partitions.csv 并重新烧写分区表",
                 HZK16_PART_NAME);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "找到分区 %s: offset=0x%08X size=%u 字节 (%u KB)",
             part->label, (unsigned)part->address, (unsigned)part->size,
             (unsigned)(part->size / 1024U));

    if (part->size < HZK16_REQUIRED_BYTES) {
        ESP_LOGE(TAG, "分区太小: %u < %u 字节，装不下完整 HZK16 字库",
                 (unsigned)part->size, (unsigned)HZK16_REQUIRED_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    s_partition = part;

    /* 自检：读第一个汉字 '啊'(0xB0A1) 的前 2 个字节，确认字库真的烧进去了 */
    uint8_t   probe[2] = { 0 };
    esp_err_t err = esp_partition_read(s_partition,
                                       15U * HZK16_WEI_PER_QU * HZK16_GLYPH_BYTES,
                                       probe, sizeof(probe));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取字库失败: %s（分区表烧了但字库 bin 还没烧？）", esp_err_to_name(err));
        s_partition = NULL;
        return err;
    }
    if (probe[0] == 0xFF && probe[1] == 0xFF) {
        ESP_LOGW(TAG, "fontbin 分区内容是空的（全 0xFF），请先把 hzk16.bin 烧进去，"
                      "否则汉字不会显示");
    }

    s_ready = true;
    ESP_LOGI(TAG, "HZK16 字库就绪（%dx%d，每字 %d 字节，映射表 %u 条）",
             HZK16_GLYPH_WIDTH, HZK16_GLYPH_HEIGHT, HZK16_GLYPH_BYTES,
             (unsigned)hzk16_uni_table_count());
    return ESP_OK;
}

esp_err_t hzk16_deinit(void)
{
    s_partition = NULL;
    s_ready     = false;
    return ESP_OK;
}

bool hzk16_is_ready(void)
{
    return s_ready;
}

/*
 * ============================================================================
 *  坑点备忘（调试时先看这里）
 * ----------------------------------------------------------------------------
 *  1. 取模“高位/低位”问题
 *     HZK16 是“高位在前”(MSB first)：每行 2 字节，第 1 字节的 bit7 是最左边的
 *     像素。如果显示出来是左右镜像或者一堆竖条，通常是这两点之一：
 *       a) 用了 (byte & (1 << x)) 从低位开始取（应该用 0x80 >> x）；
 *       b) 把每行的两个字节写反了（应该先写第 1 字节=左 8 像素）。
 *
 *  2. GB2312 区位码计算
 *     区号 = 第一字节 - 0xA0（不是 -0xA1！），位号 = 第二字节 - 0xA0；
 *     偏移 = ((区号 - 1) * 94 + (位号 - 1)) * 32。
 *     用 -0xA1 会整体偏移一个区，显示出来的全是别的字。另外 94 是“每区 94 个位”，
 *     不要写成 96 或 100。
 *
 *  3. 分区读取错误处理
 *     - 分区不存在（只改了 partitions.csv 但没重新烧分区表）-> ESP_ERR_NOT_FOUND；
 *     - 分区存在但字库没烧 -> 读出来是全 0，汉字不显示（有告警日志）；
 *     - esp_partition_read 的 offset+len 不能超过分区长度，否则报错，
 *       所以本文件在读取前先做越界判断。
 *
 *  4. 不要试图把整库读进 RAM：261,696 字节，直接吃光 ESP32-S3 的可用堆。
 * ============================================================================
 */
