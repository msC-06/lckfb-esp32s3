/**
 * @file    sdcard.c
 * @brief   TF 卡驱动实现：探测卡 -> 挂 VFS FatFS（不自动格式化）
 *
 * 关键点：
 *   1. mount_config.format_if_mount_failed 固定为 false —— 避免把 exFAT 卡/别人的卡误格式化；
 *   2. 通过挂载返回值区分两种失败：
 *        ESP_FAIL            -> 卡在，但文件系统不是 FAT（未格式化/exFAT）
 *        其它错误(超时等)     -> 基本可以认为没插卡
 *   3. 失败时 IDF 内部会调用 host deinit，所以不会残留半初始化状态，
 *      之后重新插卡再调一次 sdcard_mount() 即可；
 *   4. 传输方式由 SDCARD_TRANSPORT_SPI 选择（默认 SDMMC，本板接法），
 *      两种方式对上层是同一套接口（挂载点 /sd）。
 */

#include <string.h>
#include <stdio.h>

#include "sdcard.h"

#include "bsp/bsp_sdmmc.h"
#if SDCARD_TRANSPORT_SPI
#include "bsp/bsp_sdspi.h"
#endif

#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "sdcard";

/* ============================ 内部状态（全部 static） ============================ */

static sdmmc_card_t *s_card    = NULL;
static bool          s_mounted = false;

/* ============================ 挂载/卸载 ============================ */

esp_err_t sdcard_mount(void)
{
    if (s_mounted) {
        return ESP_OK;                      /* 已经挂载，直接返回 */
    }

    /* 1. 挂载参数：★ 绝对不允许自动格式化 */
    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,    /* ★ 挂载失败也不格式化，避免误擦卡上数据 */
        .max_files              = SDCARD_MAX_FILES,
        .allocation_unit_size   = 16 * 1024,
    };

    ESP_LOGI(TAG, "正在检测/挂载 TF 卡 ...");

    esp_err_t err;

#if SDCARD_TRANSPORT_SPI
    /* ---- SPI 方式：需要板子按 CS/MOSI/MISO/CLK 接线 ---- */
    esp_err_t bus_err = bsp_sdspi_bus_init();
    if (bus_err != ESP_OK) {
        return bus_err;
    }

    sdmmc_host_t          host;
    sdspi_device_config_t slot;
    bsp_sdspi_get_host(&host);
    bsp_sdspi_get_slot(&slot);

    err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
#else
    /* ---- SDMMC(SDIO 1 线) 方式：本开发板的接法 ---- */
    sdmmc_host_t        host;
    sdmmc_slot_config_t slot;
    bsp_sdmmc_get_host(&host);
    bsp_sdmmc_get_slot(&slot);

    err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
#endif

    if (err == ESP_FAIL) {
        /* 卡有应答，但 FAT 挂不上：多半是 exFAT 或者没格式化 */
        ESP_LOGE(TAG, "TF 卡文件系统无法挂载（不是 FAT32 或未格式化）；"
                      "已按配置跳过格式化，卡上数据未做任何改动");
        s_card = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        /* 没插卡 / 接触不良 / 卡损坏，都会走到这里 */
        ESP_LOGW(TAG, "未检测到 TF 卡或卡初始化失败(%s)，跳过挂载", esp_err_to_name(err));
        s_card = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "TF 卡挂载成功: %s", SDCARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);      /* 打印卡名字/容量/速率 */

    /* 顺便打印剩余空间，方便确认挂的是哪张卡 */
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        ESP_LOGI(TAG, "容量: 总 %llu KB / 剩余 %llu KB",
                 (unsigned long long)(total / 1024), (unsigned long long)(free_b / 1024));
    }

    return ESP_OK;
}

esp_err_t sdcard_unmount(void)
{
    if (!s_mounted || s_card == NULL) {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TF 卡已卸载，可以安全拔卡");
    } else {
        ESP_LOGE(TAG, "TF 卡卸载失败: %s", esp_err_to_name(err));
    }

    s_card    = NULL;
    s_mounted = false;
    return err;
}

bool sdcard_is_mounted(void)
{
    return s_mounted;
}

const char *sdcard_get_mount_point(void)
{
    return SDCARD_MOUNT_POINT;
}

esp_err_t sdcard_get_info(sdcard_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(info, 0, sizeof(sdcard_info_t));

    if (s_card != NULL) {
        /* CID 里的 NAME 是卡的名字（例如 SD16G） */
        memcpy(info->name, s_card->cid.name, sizeof(s_card->cid.name));
        info->name[sizeof(info->name) - 1] = '\0';
    }

    esp_err_t err = esp_vfs_fat_info(SDCARD_MOUNT_POINT, &info->total_bytes, &info->free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取容量失败: %s", esp_err_to_name(err));
    }
    return err;
}
