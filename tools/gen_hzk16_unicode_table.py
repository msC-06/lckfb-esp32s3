#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 HZK16 字库驱动用的 Unicode -> GB2312 映射表。

原理：
    GB2312 的“区位码”与 EUC-CN 字节的换算关系是
        byte1 = 0xA0 + 区号 ,  byte2 = 0xA0 + 位号
    例如 啊 = 区位 1601 -> 0xB0 0xA1。
    本脚本用 Python 内置的 gb2312 编解码器枚举所有合法编码，
    反查出对应的 Unicode 码点，生成按 Unicode 升序排列的查找表。

用法（在工程根目录执行）：
    python tools/gen_hzk16_unicode_table.py
输出：
    components/my_drivers/font_hzk16/hzk16_unicode_table.c

注意：
    - 只保留“文件里真实存在点阵”的码位（字节1 <= 0xF7，即区号 <= 87，
      且偏移 + 32 <= 字库文件长度），和常见的 261,696 字节 HZK16 文件一致；
    - 表是 const 数据，放 flash，不占 RAM。
"""

import os

# 标准 HZK16 文件的有效数据长度：区 1~87，每区 94 个位，每字 32 字节
HZK16_VALID_BYTES = 87 * 94 * 32          # = 261696

OUT_C = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "components", "my_drivers", "font_hzk16", "hzk16_unicode_table.c",
)


def build_table():
    """枚举全部 GB2312 合法码位，返回按 Unicode 升序的 (unicode, gb) 列表"""
    entries = {}
    for qu in range(1, 95):               # 区号 1~94
        for wei in range(1, 95):          # 位号 1~94
            b1 = 0xA0 + qu
            b2 = 0xA0 + wei
            if b1 > 0xF7:                 # 区号 > 87：标准字库里没有点阵
                continue
            offset = ((qu - 1) * 94 + (wei - 1)) * 32
            if offset + 32 > HZK16_VALID_BYTES:
                continue
            try:
                ch = bytes([b1, b2]).decode("gb2312")
            except UnicodeDecodeError:
                continue                  # GB2312 未定义的码位
            if len(ch) != 1:
                continue
            entries[ord(ch)] = (b1 << 8) | b2
    return sorted(entries.items())


def main():
    table = build_table()
    print("有效码位数量: %d" % len(table))

    lines = []
    lines.append("/*")
    lines.append(" * @file    hzk16_unicode_table.c")
    lines.append(" * @brief   Unicode -> GB2312 映射表（本文件由脚本自动生成，请勿手工修改）")
    lines.append(" *")
    lines.append(" * 生成脚本: tools/gen_hzk16_unicode_table.py")
    lines.append(" * 码位数量: %d（GB2312 汉字 + 全角符号，按 Unicode 升序排列，供二分查找）" % len(table))
    lines.append(" */")
    lines.append("")
    lines.append('#include "hzk16_unicode_table.h"')
    lines.append("")
    lines.append("/* 表项 = { Unicode 码点, GB2312 双字节编码 }，升序排列 */")
    lines.append("static const hzk16_uni_map_t s_uni_to_gb[] = {")
    for cp, gb in table:
        lines.append("    { 0x%04X, 0x%04X }, /* %s */" % (cp, gb, chr(cp)))
    lines.append("};")
    lines.append("")
    lines.append("/* 私有头文件中声明的访问接口，外部只能拿到只读指针 */")
    lines.append("const hzk16_uni_map_t *hzk16_uni_table_get(void)")
    lines.append("{")
    lines.append("    return s_uni_to_gb;")
    lines.append("}")
    lines.append("")
    lines.append("uint32_t hzk16_uni_table_count(void)")
    lines.append("{")
    lines.append("    return (uint32_t)(sizeof(s_uni_to_gb) / sizeof(s_uni_to_gb[0]));")
    lines.append("}")
    lines.append("")

    os.makedirs(os.path.dirname(OUT_C), exist_ok=True)
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("已生成: %s (%.1f KB)" % (OUT_C, os.path.getsize(OUT_C) / 1024.0))


if __name__ == "__main__":
    main()
