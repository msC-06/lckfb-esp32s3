/**
 * @file    save_service.h
 * @brief   存档服务（app 公共模块）：独立任务 + 环形缓冲做非阻塞文件读写
 *
 * 为什么要单独一个服务：
 *   LVGL 主线程不能被 TF 卡 I/O 阻塞（一次文件操作可能几十毫秒，会明显卡顿），
 *   所以约定：
 *     - 调用方（LVGL 线程）只做“投递请求”：把请求写进环形缓冲，立刻返回；
 *     - 独立的存档任务从环形缓冲取请求，做真正的文件读写（短块读写，128B 以内）；
 *     - 读结果放在结果槽里，调用方在 update 里 poll 取走（非阻塞）。
 *
 * 目录约定：所有存档都放在 TF 卡的 /sd/game_save/ 下，文件名由调用方给出
 *           （例如 "score_2048.txt"），服务内部会拼成 /sd/game_save/xxx。
 *
 * 注意：
 *   - 内容长度上限 SAVE_DATA_MAX-1 字节（分数这类小文件足够；存大文件请自行扩展）；
 *   - 环形缓冲满时投递会失败并返回 ESP_ERR_NO_MEM（调用方可稍后重试），不会阻塞；
 *   - TF 卡没挂载时，读请求会返回失败，写请求会直接丢弃并告警。
 */

#ifndef __SAVE_SERVICE_H
#define __SAVE_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 配置 ============================ */

/** 存档目录（TF 卡上需要先建好这个目录，服务也会尝试自动创建一次） */
#define SAVE_DIR            "/sd/game_save"

/** 文件名最大长度（含结尾 0） */
#define SAVE_NAME_MAX       24

/** 存档内容最大长度（含结尾 0） */
#define SAVE_DATA_MAX       64

/** 环形缓冲槽位数（满时投递失败，调用方稍后重试） */
#define SAVE_RING_SLOTS     8

/** 单次读写块大小（短块读写，避免一次性大 I/O 阻塞存档任务） */
#define SAVE_IO_CHUNK       16

/* ============================ 接口 ============================ */

/**
 * @brief  初始化存档服务：建环形缓冲、建目录、启动存档任务
 *
 * @return ESP_OK 成功（注意：TF 卡没插也不影响服务初始化，只会影响实际读写）
 *         ESP_ERR_INVALID_STATE 已经初始化
 *         ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t save_service_init(void);

/**
 * @brief  TF 卡是否可用（挂载成功）
 */
bool save_service_is_available(void);

/**
 * @brief  投递一个“保存”请求（非阻塞，立刻返回）
 *
 * @param  name 文件名（相对 /sd/game_save/），例如 "score_2048.txt"
 * @param  text 要写入的文本（以 0 结尾，最长 SAVE_DATA_MAX-1）
 * @return ESP_OK 已投递；ESP_ERR_NO_MEM 环形缓冲满（稍后重试）；ESP_ERR_INVALID_ARG 参数错
 */
esp_err_t save_service_post_save(const char *name, const char *text);

/**
 * @brief  投递一个“读取”请求（非阻塞，立刻返回）
 *
 * @param  name 文件名（相对 /sd/game_save/）
 * @return ESP_OK 已投递；ESP_ERR_NO_MEM 环形缓冲满；ESP_ERR_INVALID_ARG 参数错
 */
esp_err_t save_service_post_load(const char *name);

/**
 * @brief  取走一个读取结果（非阻塞，在游戏 update 里轮询调用）
 *
 * @param  out     输出缓冲
 * @param  out_len 缓冲长度（建议 >= SAVE_DATA_MAX）
 * @return  1 取到内容（已去掉结尾换行）
 *          0 暂时没有结果
 *         -1 最近一次读取失败（文件不存在/卡不可用等）
 */
int save_service_poll(char *out, size_t out_len);

/**
 * @brief  已丢弃的请求数（环形缓冲满导致），用于排查“保存不生效”
 */
uint32_t save_service_get_dropped_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __SAVE_SERVICE_H */
