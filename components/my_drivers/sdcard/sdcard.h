/**
 * @file    sdcard.h
 * @brief   TF 卡（microSD）驱动 + FAT 文件系统挂载
 *
 * 分层：本文件属于 my_drivers（TF 卡是板上外部器件），
 *       底层用 bsp 的 SDMMC 主机/槽位配置。
 *
 * 行为约定（重要）：
 *   - **绝不自动格式化**：挂载参数固定 format_if_mount_failed = false；
 *   - 没插卡 / 卡无法识别 -> sdcard_mount() 返回 ESP_ERR_NOT_FOUND，不注册文件系统；
 *   - 插了卡但不是 FAT32（例如 exFAT、未格式化）-> 返回 ESP_ERR_INVALID_STATE，
 *     只打印提示，不会动卡上的数据；
 *   - 挂载成功后可以直接用标准 C 文件接口：fopen("/sdcard/xxx.txt", "w")。
 */

#ifndef __SDCARD_H
#define __SDCARD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 挂载点（用标准 C 库文件接口时带上这个前缀） */
#define SDCARD_MOUNT_POINT      "/sdcard"

/** 打开文件数的上限 */
#define SDCARD_MAX_FILES        5

/** 卡信息 */
typedef struct {
    char     name[16];          /*!< 卡名字（CID NAME），失败时为空串 */
    uint64_t total_bytes;       /*!< 总容量（字节） */
    uint64_t free_bytes;        /*!< 剩余容量（字节） */
} sdcard_info_t;

/**
 * @brief  挂载 TF 卡文件系统（先探测卡，再挂 FAT）
 *
 * @note   可重复调用；已经挂载时直接返回 ESP_OK。
 *         开机在 board_init() 里会自动尝试一次；如果当时没插卡，
 *         后面插上卡再调用本函数即可（IDF 在挂载失败时会反初始化 SDMMC 主机，
 *         不会残留半初始化状态，所以可以随时重试）。
 *         失败时不会格式化卡。
 * @return ESP_OK 挂载成功；
 *         ESP_ERR_NOT_FOUND   没检测到卡 / 卡初始化失败（可以认为是没插卡）；
 *         ESP_ERR_INVALID_STATE 卡在，但文件系统不是 FAT32（未格式化/exFAT），未做格式化
 */
esp_err_t sdcard_mount(void);

/**
 * @brief  卸载 TF 卡文件系统（等价于弹出，之后可以安全拔卡）
 */
esp_err_t sdcard_unmount(void);

/**
 * @brief  当前是否已经挂载
 */
bool sdcard_is_mounted(void);

/**
 * @brief  获取挂载点字符串（给上层拼路径用）
 */
const char *sdcard_get_mount_point(void);

/**
 * @brief  读取卡容量信息（需要已经挂载）
 * @param  info 输出参数
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未挂载；ESP_ERR_INVALID_ARG 参数为空
 */
esp_err_t sdcard_get_info(sdcard_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* __SDCARD_H */
