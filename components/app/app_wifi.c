/**
 * @file    app_wifi.c
 * @brief   WiFi（STA）连接管理实现
 */

#include <stdio.h>
#include <string.h>

#include "app_wifi.h"
#include "app_bus.h"
#include "app_config.h"

#include "my_drivers/net/net.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_WIFI";

/* ============================ 内部状态（全部 static） ============================ */

static char s_ip[20]      = "0.0.0.0";
/** 上一次投递到界面的 WiFi 状态文字（避免重复刷屏） */
static char s_last_status[64] = "";

/* ============================ 内部函数 ============================ */

/**
 * @brief  把 WiFi 状态投递到界面（内容没变就不投）
 */
static void wifi_post_status(const char *text)
{
    if (text == NULL || strcmp(text, s_last_status) == 0) {
        return;
    }
    strncpy(s_last_status, text, sizeof(s_last_status) - 1);
    s_last_status[sizeof(s_last_status) - 1] = '\0';

    ESP_LOGI(TAG, "%s", text);
    app_bus_post_chat(CHAT_MSG_WIFI, text);
}

/**
 * @brief  刷新本机 IP 缓存
 */
static void wifi_refresh_ip(void)
{
    char ip[20] = {0};
    if (net_get_ip(ip, sizeof(ip)) == ESP_OK) {
        strncpy(s_ip, ip, sizeof(s_ip) - 1);
        s_ip[sizeof(s_ip) - 1] = '\0';
    } else {
        strncpy(s_ip, "0.0.0.0", sizeof(s_ip) - 1);
    }
}

/* ============================ 对外接口 ============================ */

esp_err_t app_wifi_init(void)
{
    return net_init();
}

bool app_wifi_is_connected(void)
{
    return net_is_connected();
}

const char *app_wifi_ip(void)
{
    return s_ip;
}

bool app_wifi_wait_connected(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (net_is_connected()) {
            wifi_refresh_ip();
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return net_is_connected();
}

void app_wifi_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "WiFi 任务启动（Core %d），SSID = %s", xPortGetCoreID(), APP_WIFI_SSID);

    app_bus_post_chat(CHAT_MSG_WIFI, "WiFi: 连接中...");

    /* 1. 初始化 STA */
    esp_err_t err = app_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败: %s", esp_err_to_name(err));
        wifi_post_status("WiFi: 初始化失败");
        vTaskDelete(NULL);
        return;
    }
    net_set_auto_reconnect(true);

    /* 2. SSID 没填就别连了，直接提示 */
    if (strcmp(APP_WIFI_SSID, "YOUR_WIFI_SSID") == 0 || strlen(APP_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "还没有配置 WiFi 名称/密码（填 components/app/app_config_secret.h）");
        wifi_post_status("WiFi: 未配置SSID");
        vTaskDelete(NULL);
        return;
    }

    /* 3. 连接 + 断线重连循环 */
    for (;;) {
        if (!net_is_connected()) {
            wifi_post_status("WiFi: 连接中...");

            err = net_connect(APP_WIFI_SSID, APP_WIFI_PASSWORD);
            if (err == ESP_OK) {
                wifi_refresh_ip();
                char buf[64];
                snprintf(buf, sizeof(buf), "WiFi: 已连接 %s", s_ip);
                wifi_post_status(buf);
            } else {
                ESP_LOGW(TAG, "连接 %s 失败(%s)，%d 秒后重试",
                         APP_WIFI_SSID, esp_err_to_name(err),
                         APP_WIFI_RECONNECT_INTERVAL_MS / 1000);
                wifi_post_status("WiFi: 连接失败 重试中");
                vTaskDelay(pdMS_TO_TICKS(APP_WIFI_RECONNECT_INTERVAL_MS));
            }
            continue;
        }

        /* 已连接：定期刷新状态栏上的信号强度 */
        wifi_refresh_ip();
        char buf[64];
        snprintf(buf, sizeof(buf), "WiFi: %s %d dBm", s_ip, net_get_rssi());
        wifi_post_status(buf);

        vTaskDelay(pdMS_TO_TICKS(APP_WIFI_POLL_MS));
    }
}
