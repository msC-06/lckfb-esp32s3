/**
 * @file    save_service.c
 * @brief   存档服务实现：环形缓冲（单生产者/单消费者）+ 独立任务做短块文件读写
 *
 * 线程模型：
 *   - 生产者：LVGL 线程（游戏/界面）调用 save_service_post_*()，只做环形缓冲入队；
 *   - 消费者：存档任务 save_task 出队并做文件 I/O；
 *   - 结果：存档任务把读到的内容放进“结果槽”，LVGL 线程用 poll 取走。
 *   环形缓冲的读写指针用 portMUX 临界区保护，不做任何阻塞调用。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#include "save_service.h"

#include "my_drivers/sdcard/sdcard.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "save_svc";

/* ============================ 内部类型 ============================ */

typedef enum {
    SAVE_OP_LOAD = 0,
    SAVE_OP_SAVE,
} save_op_t;

/** 定长请求消息：放进环形缓冲，避免动态内存 */
typedef struct {
    save_op_t op;
    char      name[SAVE_NAME_MAX];
    char      data[SAVE_DATA_MAX];
} save_msg_t;

/** 读取结果（单槽 + 就绪/失败标志） */
typedef struct {
    char     data[SAVE_DATA_MAX];
    bool     ready;
    bool     failed;
} save_result_t;

/* ============================ 内部状态（全部 static） ============================ */

static save_msg_t             s_ring[SAVE_RING_SLOTS];   /* 环形缓冲 */
static volatile uint32_t      s_head = 0;                /* 生产者写位置 */
static volatile uint32_t      s_tail = 0;                /* 消费者读位置 */
static portMUX_TYPE           s_ring_lock = portMUX_INITIALIZER_UNLOCKED;

static save_result_t          s_result;
static portMUX_TYPE           s_result_lock = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t           s_task = NULL;
static volatile bool          s_inited = false;
static volatile uint32_t      s_dropped = 0;             /* 环形缓冲满被丢弃的请求数 */

/* ============================ 环形缓冲（单生产者/单消费者） ============================ */

/**
 * @brief  入队（生产者调用，非阻塞）
 * @return true 入队成功；false 缓冲已满
 */
static bool ring_push(const save_msg_t *msg)
{
    bool ok = false;

    portENTER_CRITICAL(&s_ring_lock);
    uint32_t next = (s_head + 1U) % SAVE_RING_SLOTS;
    if (next != s_tail) {               /* 留一个空位区分“满”和“空” */
        s_ring[s_head] = *msg;
        s_head = next;
        ok = true;
    }
    portEXIT_CRITICAL(&s_ring_lock);

    return ok;
}

/**
 * @brief  出队（消费者调用，非阻塞）
 * @return true 取到一条；false 队列空
 */
static bool ring_pop(save_msg_t *msg)
{
    bool ok = false;

    portENTER_CRITICAL(&s_ring_lock);
    if (s_tail != s_head) {
        *msg = s_ring[s_tail];
        s_tail = (s_tail + 1U) % SAVE_RING_SLOTS;
        ok = true;
    }
    portEXIT_CRITICAL(&s_ring_lock);

    return ok;
}

/* ============================ 结果槽 ============================ */

static void result_set(const char *text, bool failed)
{
    portENTER_CRITICAL(&s_result_lock);
    if (text) {
        strncpy(s_result.data, text, sizeof(s_result.data) - 1);
        s_result.data[sizeof(s_result.data) - 1] = '\0';
    } else {
        s_result.data[0] = '\0';
    }
    s_result.failed = failed;
    s_result.ready  = true;
    portEXIT_CRITICAL(&s_result_lock);
}

/* ============================ 文件操作（只在存档任务里调用） ============================ */

/**
 * @brief  拼接完整路径：/sd/game_save/<name>
 */
static void build_path(char *out, size_t out_len, const char *name)
{
    snprintf(out, out_len, "%s/%s", SAVE_DIR, name);
}

/**
 * @brief  把常见的文件系统 errno 翻译成排查建议（省得只看一个数字）
 */
static const char *fs_err_hint(int err)
{
    switch (err) {
    case ENOENT:  return "：路径不存在；如果目录/文件名超过 8.3（例如 game_save、score_2048.txt），"
                         "必须打开 menuconfig 的 FATFS 长文件名支持（CONFIG_FATFS_LFN_HEAP=y）";
    case EACCES:  return "：权限不足或卡处于只读（检查 TF 卡侧面的写保护开关）";
    case EROFS:   return "：文件系统只读";
    case ENOSPC:  return "：卡空间不足";
    case EMFILE:  return "：打开的文件数超过上限（SDCARD_MAX_FILES）";
    case EIO:     return "：卡读写错误（接触不良或卡损坏）";
    default:      return "";
    }
}

/**
 * @brief  存档目录 + 卡根目录自检（在存档任务启动时跑一次，方便排查路径问题）
 */
static void storage_check(void)
{
    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "TF 卡未挂载，存档目录暂不可用");
        return;
    }

    /* 1. 存档目录是否存在，不存在就建 */
    struct stat st;
    if (stat(SAVE_DIR, &st) != 0) {
        ESP_LOGW(TAG, "存档目录 %s 不存在(errno=%d)，尝试创建 ...", SAVE_DIR, errno);
        if (mkdir(SAVE_DIR, 0777) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "创建存档目录失败(errno=%d)%s", errno, fs_err_hint(errno));
            return;
        }
    }
    ESP_LOGI(TAG, "存档目录就绪: %s", SAVE_DIR);

    /* 2. 列一下卡根目录，确认 ESP32 看到的和电脑上看到的一致（长文件名问题一眼能看出来） */
    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "无法打开卡根目录 %s (errno=%d)%s",
                 SDCARD_MOUNT_POINT, errno, fs_err_hint(errno));
        return;
    }

    char   listing[160] = { 0 };
    size_t used = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        int n = snprintf(listing + used, sizeof(listing) - used, "%s%s",
                         (used > 0) ? ", " : "", ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(listing) - used) {
            break;
        }
        used += (size_t)n;
    }
    closedir(dir);

    ESP_LOGI(TAG, "%s 根目录: %s", SDCARD_MOUNT_POINT, (used > 0) ? listing : "(空)");
}

/**
 * @brief  短块读取：每次最多读 SAVE_IO_CHUNK 字节，避免长时间占用
 * @return ESP_OK 成功；其它失败
 */
static esp_err_t read_file_short(const char *path, char *out, size_t out_len)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        ESP_LOGD(TAG, "打开文件失败(errno=%d)%s", errno, fs_err_hint(errno));
        return ESP_ERR_NOT_FOUND;
    }

    size_t total = 0;
    while (total < out_len - 1) {
        size_t want = out_len - 1 - total;
        if (want > SAVE_IO_CHUNK) {
            want = SAVE_IO_CHUNK;
        }
        size_t got = fread(out + total, 1, want, fp);
        if (got == 0) {
            break;
        }
        total += got;
    }
    out[total] = '\0';
    fclose(fp);

    /* 去掉结尾的换行/回车/空格 */
    while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r' ||
                         out[total - 1] == ' ')) {
        out[--total] = '\0';
    }
    return ESP_OK;
}

/**
 * @brief  短块写入：每次最多写 SAVE_IO_CHUNK 字节
 * @return ESP_OK 成功；其它失败
 */
static esp_err_t write_file_short(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "打开文件失败: %s (errno=%d)%s", path, errno, fs_err_hint(errno));
        return ESP_FAIL;
    }

    size_t len = strlen(text);
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        if (want > SAVE_IO_CHUNK) {
            want = SAVE_IO_CHUNK;
        }
        size_t put = fwrite(text + done, 1, want, fp);
        if (put != want) {
            ESP_LOGE(TAG, "写文件失败: %s（已写 %u/%u 字节）",
                     path, (unsigned)done, (unsigned)len);
            fclose(fp);
            return ESP_FAIL;
        }
        done += put;
    }

    fclose(fp);
    ESP_LOGI(TAG, "已保存 %s: %s", path, text);
    return ESP_OK;
}

/* ============================ 存档任务 ============================ */

static void save_task(void *arg)
{
    save_msg_t msg;

    storage_check();        /* 开机先自检目录 + 打印卡根目录，排查长文件名等问题 */

    while (1) {
        if (!ring_pop(&msg)) {
            vTaskDelay(pdMS_TO_TICKS(20));      /* 没请求就睡一会，不占 CPU */
            continue;
        }

        if (!sdcard_is_mounted()) {
            ESP_LOGW(TAG, "TF 卡不可用，忽略 %s 请求(%s)",
                     msg.op == SAVE_OP_LOAD ? "读取" : "保存", msg.name);
            if (msg.op == SAVE_OP_LOAD) {
                result_set(NULL, true);         /* 让调用方能收到失败 */
            }
            continue;
        }

        char path[64];
        build_path(path, sizeof(path), msg.name);

        if (msg.op == SAVE_OP_LOAD) {
            char buf[SAVE_DATA_MAX];
            esp_err_t err = read_file_short(path, buf, sizeof(buf));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "已读取 %s: %s", path, buf);
                result_set(buf, false);
            } else {
                ESP_LOGI(TAG, "读取 %s 失败(%s)，按默认值处理", path, esp_err_to_name(err));
                result_set(NULL, true);
            }
        } else {
            write_file_short(path, msg.data);
        }
    }
}

/* ============================ 对外接口 ============================ */

esp_err_t save_service_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(s_ring, 0, sizeof(s_ring));
    memset(&s_result, 0, sizeof(s_result));
    s_head = s_tail = 0;
    s_dropped = 0;

    if (xTaskCreate(save_task, "save_task", 4 * 1024, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "创建存档任务失败");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "存档服务就绪（目录 %s，环形缓冲 %d 槽，短块 %d 字节）",
             SAVE_DIR, SAVE_RING_SLOTS, SAVE_IO_CHUNK);
    return ESP_OK;
}

bool save_service_is_available(void)
{
    return sdcard_is_mounted();
}

esp_err_t save_service_post_save(const char *name, const char *text)
{
    if (name == NULL || text == NULL || strlen(name) >= SAVE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    save_msg_t msg = { 0 };
    msg.op = SAVE_OP_SAVE;
    strncpy(msg.name, name, sizeof(msg.name) - 1);
    strncpy(msg.data, text, sizeof(msg.data) - 1);

    if (!ring_push(&msg)) {
        s_dropped++;
        ESP_LOGW(TAG, "环形缓冲已满，保存请求被丢弃（累计 %u 次）", (unsigned)s_dropped);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t save_service_post_load(const char *name)
{
    if (name == NULL || strlen(name) >= SAVE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    save_msg_t msg = { 0 };
    msg.op = SAVE_OP_LOAD;
    strncpy(msg.name, name, sizeof(msg.name) - 1);

    if (!ring_push(&msg)) {
        s_dropped++;
        ESP_LOGW(TAG, "环形缓冲已满，读取请求被丢弃（累计 %u 次）", (unsigned)s_dropped);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int save_service_poll(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return -1;
    }

    int ret = 0;
    portENTER_CRITICAL(&s_result_lock);
    if (s_result.ready) {
        if (s_result.failed) {
            ret = -1;
        } else {
            strncpy(out, s_result.data, out_len - 1);
            out[out_len - 1] = '\0';
            ret = 1;
        }
        s_result.ready  = false;
        s_result.failed = false;
    }
    portEXIT_CRITICAL(&s_result_lock);

    return ret;
}

uint32_t save_service_get_dropped_count(void)
{
    return s_dropped;
}
