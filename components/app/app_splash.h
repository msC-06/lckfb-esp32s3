/**
 * @file    app_splash.h
 * @brief   开机动画（app 层）：播动画/自检，并预留“从 TF 卡读 MJPEG”的钩子
 *
 * 为什么放在 app 层：
 *   开机刷什么画面属于产品行为，不该写死在屏幕驱动里。驱动只提供
 *   lcd_set_color() / lcd_draw_bitmap() 这类原语，动画由 app 决定。
 *
 * 播放优先级：
 *   1. 如果通过 app_splash_register_source() 注册了动画源 -> 播动画源的帧；
 *   2. 没有注册、或者动画源打不开/出错 -> 退回内置的刷色自检
 *      （白->红->绿->蓝->黑，用于确认 SPI/CS/背光/面板这条链路是通的）。
 *
 * ---------------------------------------------------------------------------
 * 【以后接 TF 卡 MJPEG 开机动画的做法】
 *   在 board_init() 之后（此时 /sdcard 已挂载）注册一个动画源即可，例如：
 *
 *   static FILE *s_fp;
 *   static uint8_t *s_jpeg;          // 一帧 JPEG 码流缓冲
 *   static uint16_t *s_rgb565;       // 解码后的 RGB565 缓冲
 *
 *   static esp_err_t mjpeg_open(void *ctx) { s_fp = fopen("/sdcard/boot.mjpeg", "rb"); ... }
 *   static int mjpeg_read_frame(void *ctx, app_splash_frame_t *f) {
 *       // 1) 从 s_fp 里按 0xFFD8..0xFFD9 切出一帧 JPEG 到 s_jpeg
 *       // 2) 用 esp_jpeg 组件解码成 RGB565 到 s_rgb565
 *       // 3) f->format = APP_SPLASH_FMT_RGB565; f->data = (const uint8_t *)s_rgb565;
 *       //    f->len = w*h*2; f->width = w; f->height = h; f->duration_ms = 40;
 *       return 1;                    // 1=有帧, 0=播完, <0=出错
 *   }
 *   static void mjpeg_close(void *ctx) { fclose(s_fp); ... }
 *
 *   static const app_splash_source_t s_mjpeg = {
 *       .name = "sdcard-boot.mjpeg", .open = mjpeg_open,
 *       .read_frame = mjpeg_read_frame, .close = mjpeg_close, .ctx = NULL,
 *   };
 *   app_splash_register_source(&s_mjpeg);
 *   app_splash_run(5000);
 *
 * 说明：目前驱动层只实现了 RGB565 帧的直接刷屏；APP_SPLASH_FMT_JPEG 已定义
 *       但未解码，遇到会返回 ESP_ERR_NOT_SUPPORTED 并退回内置自检，
 *       等接入 esp_jpeg 后把解码结果按 RGB565 交回来即可（见上面示例）。
 * ---------------------------------------------------------------------------
 */

#ifndef __APP_SPLASH_H
#define __APP_SPLASH_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 动画帧 ============================ */

/** 帧数据格式 */
typedef enum {
    APP_SPLASH_FMT_RGB565 = 0,  /*!< RGB565 像素（已支持，直接 lcd_draw_bitmap） */
    APP_SPLASH_FMT_JPEG,        /*!< JPEG 码流（MJPEG 用，需先解码，见文件头说明） */
} app_splash_format_t;

/** 一帧动画数据 */
typedef struct {
    app_splash_format_t format;      /*!< 帧格式 */
    const uint8_t      *data;        /*!< 帧数据指针（回调返回后仍要有效直到下帧） */
    size_t              len;         /*!< 数据长度（字节） */
    uint16_t            width;       /*!< RGB565 帧宽（JPEG 帧填 0） */
    uint16_t            height;      /*!< RGB565 帧高（JPEG 帧填 0） */
    uint32_t            duration_ms; /*!< 本帧显示时长，0 = 用默认值 */
} app_splash_frame_t;

/** 默认每帧显示时长（毫秒） */
#define APP_SPLASH_FRAME_MS_DEFAULT   40

/* ============================ 动画源钩子 ============================ */

/** 动画源：由使用方实现（例如从 SD 卡读 MJPEG、从 flash 读图片序列） */
typedef struct {
    const char *name;       /*!< 名字，仅用于日志 */
    /** 打开动画源，返回 ESP_OK 才算可用 */
    esp_err_t (*open)(void *ctx);
    /** 取下一帧：>0 取到一帧，0 播放结束，<0 出错 */
    int       (*read_frame)(void *ctx, app_splash_frame_t *frame);
    /** 关闭动画源（可为 NULL） */
    void      (*close)(void *ctx);
    void       *ctx;        /*!< 使用方自带的上下文 */
} app_splash_source_t;

/**
 * @brief  注册开机动画数据源（钩子函数，注册后由 app_splash_run 调用）
 *
 * @param  source 动画源描述；传 NULL 表示取消注册（回到内置自检）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法（缺少必要回调）
 * @note   必须在 app_splash_run() 之前调用
 */
esp_err_t app_splash_register_source(const app_splash_source_t *source);

/* ============================ 播放 ============================ */

/**
 * @brief  播放开机动画（阻塞，直到动画播完/超时）
 *
 * @param  max_ms 最长播放时间（毫秒），0 = 用默认 3000ms；播动画期间不会被 LVGL 覆盖
 * @return ESP_OK 播放完成（含“退回内置自检”的情况）；
 *         ESP_ERR_INVALID_STATE 屏幕未就绪
 */
esp_err_t app_splash_run(uint32_t max_ms);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SPLASH_H */
