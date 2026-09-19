# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(empty_proj)

# ---------------------------------------------------------------------------
# HZK16 中文字库：把根目录的 hzk16.bin 关联到 partitions.csv 里的 fontbin 分区
#   - 常见 hzk16.bin 为 267,616 字节，而 fontbin 只有 256K(262,144 字节)，
#     直接烧会越界，所以先裁剪到分区大小（GB2312 只寻址前 261,696 字节，不影响取字）；
#   - 裁剪结果放到 build/hzk16_fontbin.bin，然后 `idf.py flash` 会一起烧进去；
#   - 手工烧写命令见 tools/prepare_font_bin.py 顶部注释或 README。
# 若要烧原始未裁剪文件，把 partitions.csv 里 fontbin 的 Size 改成 512K 即可。
# ---------------------------------------------------------------------------
set(HZK16_SRC_BIN   "${CMAKE_CURRENT_LIST_DIR}/hzk16.bin")
set(HZK16_PART_SIZE "262144")   # 必须与 partitions.csv 中 fontbin 的 Size 一致（256K）

if(EXISTS "${HZK16_SRC_BIN}")
    idf_build_get_property(python PYTHON)
    set(HZK16_FLASH_BIN "${CMAKE_BINARY_DIR}/hzk16_fontbin.bin")

    execute_process(
        COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/tools/prepare_font_bin.py"
                "${HZK16_SRC_BIN}" "${HZK16_FLASH_BIN}" "${HZK16_PART_SIZE}"
        RESULT_VARIABLE HZK16_PREP_RC)

    if(HZK16_PREP_RC EQUAL 0)
        esptool_py_flash_to_partition(flash "fontbin" "${HZK16_FLASH_BIN}")
        message(STATUS "fontbin: 已加入烧写列表 (${HZK16_FLASH_BIN})")
    else()
        message(WARNING "fontbin: 准备字库失败 (rc=${HZK16_PREP_RC})，本次不会烧写字库")
    endif()
else()
    message(STATUS "fontbin: 未找到 hzk16.bin，跳过（把字库文件放到工程根目录即可）")
endif()
