/**
 * @file    camera.h
 * @brief   板载 DVP 摄像头驱动（GC0308，基于 esp32-camera 组件）
 *
 * 分层：本文件属于 my_drivers（GC0308 是板上外部芯片），
 *       底层用 bsp 的 I2C（SCCB 复用）与 PCA9557（掉电脚）。
 *       原来放在 bsp/bsp_camera.c，按分层规则迁移到这里。
 *
 * 硬件连接（与旧工程一致）：
 *   XCLK=GPIO5  PCLK=GPIO7  VSYNC=GPIO3  HREF=GPIO46
 *   D0=GPIO16 D1=GPIO18 D2=GPIO8  D3=GPIO17
 *   D4=GPIO15 D5=GPIO6  D6=GPIO4  D7=GPIO9
 *   SIOD=GPIO1(共用 I2C SDA)  SIOC=GPIO2(共用 I2C SCL)
 */

#ifndef __CAMERA_H
#define __CAMERA_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 默认输出分辨率与格式 */
#define CAMERA_DEFAULT_FRAMESIZE   FRAMESIZE_QVGA     /* 320x240，和 LCD 分辨率一致 */
#define CAMERA_DEFAULT_FORMAT      PIXFORMAT_RGB565   /* 直接送 LCD；要存图可改成 PIXFORMAT_JPEG */
#define CAMERA_DEFAULT_QUALITY     12                 /* 仅 JPEG 模式有效，越小越清晰 */
#define CAMERA_XCLK_FREQ_HZ        (20 * 1000 * 1000)

/** 一帧图像回调（预览用）：回调返回后驱动会立刻归还帧缓冲，需要保留请自行拷贝 */
typedef void (*camera_frame_cb_t)(camera_fb_t *fb);

/* ============================ 初始化/反初始化 ============================ */

/**
 * @brief  初始化摄像头
 *
 * 内部会：拉低 DVP_PWDN 给摄像头供电 -> esp_camera_init -> 按传感器型号设置镜像
 * @note   失败时返回错误码（不会 abort）；请先初始化 I2C 总线与 PCA9557。
 */
esp_err_t camera_init(void);

/**
 * @brief  反初始化摄像头（并让摄像头掉电）
 */
esp_err_t camera_deinit(void);

/** 摄像头是否已初始化 */
bool camera_is_initialized(void);

/** 摄像头供电控制：true = 工作，false = 掉电 */
esp_err_t camera_power(bool on);

/* ============================ 取图 ============================ */

/**
 * @brief  获取一帧图像（阻塞）
 * @return 帧缓冲指针，失败返回 NULL；用完必须 camera_return_frame()
 */
camera_fb_t *camera_get_frame(void);

/**
 * @brief  归还帧缓冲
 */
void camera_return_frame(camera_fb_t *fb);

/* ============================ 预览（可选） ============================ */

/**
 * @brief  启动预览任务：循环取图 -> 调用回调 -> 归还
 *
 * @param  cb  取到一帧后调用（可用于把图像刷到 LCD），不能为 NULL
 * @param  fps 期望帧率上限（0 表示不限速），例如 15
 */
esp_err_t camera_start_preview(camera_frame_cb_t cb, uint32_t fps);

/** 停止预览任务 */
esp_err_t camera_stop_preview(void);

/* ============================ 传感器参数 ============================ */

/** 获取传感器对象（用于高级设置），未初始化返回 NULL */
sensor_t *camera_get_sensor(void);

/** 设置分辨率 */
esp_err_t camera_set_framesize(framesize_t framesize);

/** 水平镜像 */
esp_err_t camera_set_hmirror(bool enable);

/** 垂直翻转 */
esp_err_t camera_set_vflip(bool enable);

/** 亮度 -2 ~ 2 */
esp_err_t camera_set_brightness(int level);

/** 对比度 -2 ~ 2 */
esp_err_t camera_set_contrast(int level);

/** 饱和度 -2 ~ 2 */
esp_err_t camera_set_saturation(int level);

#ifdef __cplusplus
}
#endif

#endif /* __CAMERA_H */
