#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
prepare_font_bin.py —— 把字库 bin 裁剪到正好能放进 fontbin 分区的大小。

背景：
    常见 hzk16.bin 文件是 267,616 字节（= 8363 个 32 字节字模），
    而 GB2312 区位码只寻址到 87 区 94 位，也就是前 261,696 字节；
    256KB 的 fontbin 分区装得下这 261,696 字节，但装不下多余的尾部数据。
    直接用 esptool 烧原文件会越过分区边界，所以这里先裁剪再烧。

用法（一般由 CMake 自动调用）：
    python tools/prepare_font_bin.py <src.bin> <out.bin> <partition_size_bytes>

输出：
    裁剪后的文件；标准输出为纯 ASCII，避免 Windows 控制台编码问题。
"""

import sys


def main():
    if len(sys.argv) != 4:
        print("usage: prepare_font_bin.py <src.bin> <out.bin> <partition_size_bytes>")
        return 2

    src, out, part_size = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)

    with open(src, "rb") as f:
        data = f.read()

    keep = min(len(data), part_size)
    with open(out, "wb") as f:
        f.write(data[:keep])

    print("[fontbin] src=%d bytes, partition=%d bytes, flash=%d bytes"
          % (len(data), part_size, keep))
    if keep < len(data):
        print("[fontbin] WARN: %d trailing bytes dropped (beyond 87*94*32=%d "
              "addressable by GB2312, so glyph lookup is unaffected)"
              % (len(data) - keep, 87 * 94 * 32))
    return 0


if __name__ == "__main__":
    sys.exit(main())
