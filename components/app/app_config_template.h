/**
 * @file    app_config_template.h
 * @brief   私有密钥模板（**本文件入库**，里面只有占位符）
 *
 * ============================================================================
 *  clone 之后第一件事（不填密钥跑不起来：WiFi 连不上、ASR/LLM 都会报“未配置密钥”）：
 *
 *      # 在工程根目录执行
 *      cp components/app/app_config_template.h components/app/app_config_secret.h
 *
 *      # 然后用编辑器打开 components/app/app_config_secret.h
 *      # 把下面 6 个占位符换成真实值，保存即可，不需要改别的文件
 *
 *  Windows (cmd / PowerShell) 也可以：
 *      copy components\app\app_config_template.h components\app\app_config_secret.h
 *
 * ============================================================================
 *  机制说明：
 *    - app_config.h 会用 __has_include("app_config_secret.h") 尝试引入它，
 *      所以**文件不存在也能正常编译**（走 app_config.h 里的占位符）；
 *    - app_config.h 里的每个占位符都写在 #ifndef 里，
 *      因此 secret 头文件里定义的宏**优先生效**（不会被占位符覆盖）；
 *    - app_config_secret.h 已写进 .gitignore，不会被提交；
 *    - 也支持编译期覆盖：idf.py -DAPP_WIFI_SSID=\"myap\" build
 *      （命令行 -D 的宏同样优先于占位符）。
 *
 *  安全提醒：
 *    - 千万不要把真实密钥写回本文件或 app_config.h，也不要贴到 issue/聊天记录里；
 *    - 如果密钥曾经提交过 git，即使后来删掉，历史里仍然查得到 —— 请到各家控制台
 *      重新生成（百度“应用密钥”、DeepSeek API Keys、Tavily API Keys）并作废旧密钥。
 * ============================================================================
 */

#ifndef __APP_CONFIG_TEMPLATE_H
#define __APP_CONFIG_TEMPLATE_H

/* ---------- 1. WiFi（2.4GHz，ESP32-S3 不支持 5GHz） ---------- */
#define APP_WIFI_SSID           "YOUR_WIFI_SSID"          /* ← 你的 WiFi 名称 */
#define APP_WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"      /* ← 你的 WiFi 密码 */

/* ---------- 2. 百度智能云：短语音识别标准版 ----------
 * https://console.bce.baidu.com/ai/#/ai/speech/app/list  ->  创建应用后查看 */
#define BAIDU_ASR_API_KEY       "YOUR_BAIDU_API_KEY"      /* ← API Key */
#define BAIDU_ASR_SECRET_KEY    "YOUR_BAIDU_SECRET_KEY"   /* ← Secret Key */

/* ---------- 3. DeepSeek ----------
 * https://platform.deepseek.com/api_keys */
#define DEEPSEEK_API_KEY        "YOUR_DEEPSEEK_API_KEY"   /* ← sk- 开头 */

/* ---------- 4. Tavily（联网搜索，给 Function Calling 用） ----------
 * https://app.tavily.com/home  ->  API Keys */
#define TAVILY_API_KEY          "YOUR_TAVILY_API_KEY"     /* ← tvly- 开头 */

#endif /* __APP_CONFIG_TEMPLATE_H */
