/**
 * @file    app_config.h
 * @brief   对话掌机全局配置：任务参数、录音参数、界面配色、缓冲尺寸
 *
 * ============================ 密钥不在这里 ============================
 *   WiFi / 百度 / DeepSeek / Tavily 的密钥全部放在**私有头文件**里，本文件只保留占位符：
 *
 *       # 在工程根目录执行（只需一次）
 *       cp components/app/app_config_template.h components/app/app_config_secret.h
 *       # 然后编辑 components/app/app_config_secret.h 填入真实密钥
 *
 *   - app_config_template.h —— 入库，只有占位符
 *   - app_config_secret.h   —— 不入库（已在 .gitignore），放真实密钥
 *   - 本文件用 __has_include 引入 secret：**文件不存在也能编译**，
 *     存在时它的宏优先（下面的占位符都写在 #ifndef 里，不会覆盖它）
 *   - 也支持编译期覆盖：idf.py -DAPP_WIFI_SSID=\"myap\" build
 * ======================================================================
 *
 * 分层：本文件属于 app 层，只有宏定义，不包含任何实现。
 */

#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/* ============================================================
 *  0. 私有密钥（不写进仓库！）
 *     顺序很重要：先引入 secret，再用 #ifndef 兜底占位符
 *
 *     注意：这里是**普通 include**，不做 __has_include 判断。
 *     文件由 components/app/CMakeLists.txt 在配置阶段保证存在
 *     （不存在就用 app_config_template.h 复制一份）。
 *     原因：__has_include 方式有个很坑的副作用——
 *     如果先在没有密钥的情况下构建过一次，编译依赖文件（.d）里
 *     就不会记录 secret 头文件；之后把密钥文件补上，ninja 认为“没变化”，
 *     **不会重新编译**，固件里仍然是占位符（表现就是一直说“未配置密钥”）。
 * ============================================================ */

#include "app_config_secret.h"      /* 真实密钥：git 忽略，不提交 */

/* ---------- 下面 6 个只有 secret 没提供时才生效（占位符会被代码识别为“未配置”） ---------- */

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID           "YOUR_WIFI_SSID"          /* ← 见 app_config_secret.h */
#endif
#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"      /* ← 见 app_config_secret.h */
#endif
#ifndef BAIDU_ASR_API_KEY
#define BAIDU_ASR_API_KEY       "YOUR_BAIDU_API_KEY"      /* ← 见 app_config_secret.h */
#endif
#ifndef BAIDU_ASR_SECRET_KEY
#define BAIDU_ASR_SECRET_KEY    "YOUR_BAIDU_SECRET_KEY"   /* ← 见 app_config_secret.h */
#endif
#ifndef DEEPSEEK_API_KEY
#define DEEPSEEK_API_KEY        "YOUR_DEEPSEEK_API_KEY"   /* ← 见 app_config_secret.h */
#endif
#ifndef TAVILY_API_KEY
#define TAVILY_API_KEY          "YOUR_TAVILY_API_KEY"     /* ← 见 app_config_secret.h */
#endif

/* ============================================================
 *  1. WiFi（STA 模式）
 *     SSID / 密码见第 0 节（私有密钥，不写在这里）
 * ============================================================ */

/** 断线自动重连间隔（毫秒） */
#define APP_WIFI_RECONNECT_INTERVAL_MS  5000
/** 首次连接的重试次数 */
#define APP_WIFI_CONNECT_RETRY          3
/** WiFi 状态轮询周期（毫秒） */
#define APP_WIFI_POLL_MS                10000

/* ============================================================
 *  2. 百度智能云：短语音识别（ASR）
 *     https://console.bce.baidu.com/ai/#/ai/speech/app/list
 * ============================================================ */

/* Baidu API Key / Secret Key 见第 0 节（私有密钥，不写在这里） */

/** 取 token 接口 */
#define BAIDU_ASR_TOKEN_URL     "https://aip.baidubce.com/oauth/2.0/token"
/** 短语音识别接口 */
#define BAIDU_ASR_URL           "https://vop.baidu.com/server_api"
/** 用户唯一标识（随便填，用于区分设备） */
#define BAIDU_ASR_CUID          "esp32s3-talking"
/** 识别请求超时（毫秒） */
#define BAIDU_ASR_TIMEOUT_MS    20000
/** 取 token 超时（毫秒） */
#define BAIDU_ASR_TOKEN_TIMEOUT_MS  10000
/** Base64 分块大小（必须是 3 的倍数，避免分块产生 '=' 填充） */
#define APP_ASR_B64_CHUNK       3072
/** token 提前多久续期（秒）：剩余时间小于它就重新取 */
#define APP_ASR_TOKEN_RENEW_SEC 3600
/** 读 token 响应用的静态缓冲：实测响应约 1.4KB 且是 chunked，给足余量防截断 */
#define APP_ASR_TOKEN_RESP_BUF_SIZE (6 * 1024)
/** 读识别响应用的静态缓冲：要装得下 JSON 外壳 + APP_TEXT_MAX_LEN 的识别文本 */
#define APP_ASR_RESP_BUF_SIZE       (8 * 1024)

/* ============================================================
 *  3. DeepSeek：Chat Completions（LLM）
 *     https://platform.deepseek.com/api_keys
 * ============================================================ */

/* DeepSeek API Key 见第 0 节（私有密钥，不写在这里） */

/** 对话接口 */
#define DEEPSEEK_API_URL        "https://api.deepseek.com/chat/completions"
/** 模型名：deepseek-chat（V3 对话模型） */
#define DEEPSEEK_MODEL          "deepseek-chat"
/** 请求超时（毫秒） */
#define APP_LLM_TIMEOUT_MS      30000
/** 系统提示词 */
#define APP_LLM_SYSTEM_PROMPT   "你是一个友好的对话助手，请用简洁中文回答。对话请尽可能简短。"
/** 限制回复长度（token），LCD 也显示不下太长 */
#define APP_LLM_MAX_TOKENS      512
/** 读 HTTP 响应用的缓冲（PSRAM 分配）：回复正文 + tool_calls + usage 都给足 */
#define APP_LLM_RESP_BUF_SIZE   (32 * 1024)

/** web_search 工具的描述：决定模型什么时候去联网搜索 */
#define APP_LLM_TOOL_DESC       \
    "联网搜索工具。遇到实时信息（新闻、天气、价格、比分、人物近况）或你不确定的知识时调用它，" \
    "query 用简短具体的关键词。"
/** 一次对话最多执行几个工具调用（防止模型一次丢一堆把内存吃满） */
#define APP_LLM_MAX_TOOL_CALLS  2

/* ============================================================
 *  3.5 Tavily 联网搜索（DeepSeek Function Calling 的 web_search 工具）
 *      https://app.tavily.com/home  ->  API Keys
 * ============================================================ */

/* Tavily API Key 见第 0 节（私有密钥，不写在这里） */

/** 搜索接口 */
#define TAVILY_SEARCH_URL       "https://api.tavily.com/search"
/** 搜索请求超时（毫秒） */
#define APP_WEB_SEARCH_TIMEOUT_MS       15000
/** 最多取几条结果（写进请求体，解析时也按这个数截断） */
#define APP_WEB_SEARCH_MAX_RESULTS      3
/** 拼给模型的摘要文本上限（字符），减少 token 消耗；受调用者缓冲大小再收一次。
 *  注意：调用者的缓冲按 "APP_WEB_SEARCH_TEXT_MAX + 1" 自动分配，改这里缓冲会跟着变。 */
#define APP_WEB_SEARCH_TEXT_MAX         (5 * 1024)
/** 读 Tavily 响应用的 PSRAM 缓冲（3 条结果的原始 JSON 可能几十 KB，给足） */
#define APP_WEB_SEARCH_RESP_BUF_SIZE    (64 * 1024)

/* ============================================================
 *  4. 对话历史
 * ============================================================ */

/** 保留最近多少轮对话（1 轮 = user + assistant） */
#define APP_CHAT_HISTORY_MAX_ROUNDS     10
/** 历史上限条数（20 条，不含 system） */
#define APP_CHAT_HISTORY_MAX_MSGS       (APP_CHAT_HISTORY_MAX_ROUNDS * 2)
/** 单条历史消息最大字节数 */
#define APP_CHAT_HISTORY_MSG_MAX        1024

/* ============================================================
 *  5. 按键（GPIO0）
 * ============================================================ */

/** 扫描周期（毫秒），app_key 内部用 esp_timer 周期回调 */
#define APP_KEY_SCAN_MS             20
/** 消抖需要连续采到几次相同电平（2 * 20ms = 40ms） */
#define APP_KEY_DEBOUNCE_SAMPLES    2
/** 最长录音时间（毫秒），到点自动停止，防止忘松按键 */
#define APP_KEY_MAX_RECORD_MS       15000
/** 最短录音时间（毫秒），太短的录音直接丢弃 */
#define APP_KEY_MIN_RECORD_MS       400

/* ============================================================
 *  6. 双核任务划分与优先级
 *     Core 0 = PRO_CPU（网络核），Core 1 = APP_CPU（应用/显示核）
 * ============================================================ */

#define APP_CPU_NET_CORE            0
#define APP_CPU_APP_CORE            1

/** 网络任务（ASR 上传 + LLM 请求）：TLS + JSON 解析需要大栈 */
#define APP_NET_TASK_PRIO           5
#define APP_NET_TASK_STACK          8192
/** WiFi 任务 */
#define APP_WIFI_TASK_PRIO          5
#define APP_WIFI_TASK_STACK         (5 * 1024)
/** 按键/业务状态机任务
 *  @note 注意：消息结构体（app_text_msg_t 有 4KB+）绝不能放成局部变量，
 *        否则这个栈立刻爆掉；app_bus_post_* 已经改成用静态暂存区发送。 */
#define APP_KEY_TASK_PRIO           4
#define APP_KEY_TASK_STACK          (5 * 1024)
/** 音频采集任务：实时性要求高，优先级最高 */
#define APP_AUDIO_TASK_PRIO         6
#define APP_AUDIO_TASK_STACK        (5 * 1024)

/* ============================================================
 *  7. 队列长度与文本长度
 *
 *  ⚠️ 内存账（队列是**内部堆**，不是 PSRAM！）：
 *       asr 队列 2 × (APP_TEXT_MAX_LEN+8)   = 4.1KB
 *       llm 队列 2 × (APP_TEXT_MAX_LEN+8)   = 4.1KB
 *       chat 队列 6 × (APP_CHAT_TEXT_MAX+8) = 6.2KB
 *     内部堆总共才 ~139KB，还要养任务栈（~20KB）、WiFi/LWIP（~40KB）、
 *     TLS 连接（~22KB）。队列占太多会把内部堆挤到 20KB 以下，
 *     后果不是“分配失败报个错”，而是 WiFi 驱动拿不到收包缓冲、TLS 写阻塞
 *     （实测现象：上传卡 20 秒后 “Poll timeout”，errno 还是上一次 connect 的残留值）。
 *
 *     所以想放大文本请优先动 **PSRAM 上的**那几个：
 *     APP_LLM_RESP_BUF_SIZE / APP_WEB_SEARCH_* / EXT_RAM_BSS 标记的缓冲。
 *     运行时空闲任务会打印内部堆余量与最大可分配块，照着数据调。
 * ============================================================ */

#define APP_KEY_QUEUE_LEN           8       /* 按键事件 */
#define APP_AUDIO_QUEUE_LEN         4       /* 录音启停命令 */
#define APP_PCM_QUEUE_LEN           2       /* 录音数据 -> 网络任务 */
#define APP_ASR_QUEUE_LEN           2       /* ASR 结果 */
#define APP_LLM_QUEUE_LEN           2       /* LLM 结果 */
#define APP_CHAT_QUEUE_LEN          6       /* 聊天消息 -> 界面 */

/** ASR 识别文本 / LLM 回复文本消息的缓冲长度（也是一轮回复的硬上限）
 *  @note 2048 字节能装约 1000 个汉字，对“简洁回答”的系统提示足够了 */
#define APP_TEXT_MAX_LEN            2048
/** 聊天消息（投递给界面的文本）长度
 *  @note 只要 >= APP_CHAT_BUBBLE_TEXT_MAX 就不会丢显示内容 */
#define APP_CHAT_TEXT_MAX           1024
/** 单个气泡最多显示多少字节（再长只影响显示，不影响交给模型的历史）
 *  @note 这段文字会占 LVGL 自己的内存池（CONFIG_LV_MEM_SIZE=64KB，实测占用 ~14%） */
#define APP_CHAT_BUBBLE_TEXT_MAX    1024
/** 聊天区最多保留多少个气泡（超出后删除最老的）
 *  @note 每个气泡约占 LVGL 池 2KB + 文字长度 */
#define APP_CHAT_MAX_BUBBLES        10
/** 气泡内文字最大像素宽度（320 宽的屏，留出内边距） */
#define APP_CHAT_BUBBLE_MAX_W       210

/* ============================================================
 *  8. TLS 证书校验
 *     APP_TLS_USE_CA_BUNDLE=1：用 IDF 内置根证书包校验（推荐，联网正常）
 *     APP_TLS_USE_CA_BUNDLE=0：开发阶段跳过校验，
 *                              需要 sdkconfig 打开
 *                              CONFIG_ESP_TLS_INSECURE=y 与
 *                              CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
 * ============================================================ */

#define APP_TLS_USE_CA_BUNDLE       1

/* ============================================================
 *  8.5 空闲内存监控
 *      空闲任务只负责“到点触发”，真正读堆统计 + 打印放在监控任务里：
 *      空闲任务栈只有 CONFIG_FREERTOS_IDLE_TASK_STACKSIZE（本工程 1536 字节），
 *      在里面打印日志会把它的栈冲爆，而且空闲任务还要回收被删任务的资源，不能阻塞。
 * ============================================================ */

/** 1 = 打开空闲内存定期上报 */
#define APP_MEM_LOG_ENABLE          1
/** 上报间隔（毫秒）：空闲任务里按时间限流，不会刷屏也不会占 CPU */
#define APP_MEM_LOG_INTERVAL_MS     30000
/** 内部 RAM 空闲低于这个值时额外告警（字节） */
#define APP_MEM_WARN_INTERNAL       (24 * 1024)
/** PSRAM 空闲低于这个值时额外告警（字节） */
#define APP_MEM_WARN_PSRAM          (256 * 1024)
/** 任务栈剩余低于这个值时额外告警（字节，历史最小值） */
#define APP_MEM_WARN_STACK          1024

/** 监控任务优先级（比空闲任务高就行，别抢业务任务的 CPU） */
#define APP_MEM_TASK_PRIO           1
/** 监控任务栈：要装得下一次带不少参数的 ESP_LOGI（vsnprintf + UART） */
#define APP_MEM_TASK_STACK          4096

/* ============================================================
 *  9. 界面
 * ============================================================ */

/** 界面刷新/队列轮询周期（毫秒），在 LVGL 定时器里跑
 *  @note 决定“识别结果 / AI 回复”上屏的最大延迟，30ms 足够跟手又不费 CPU */
#define APP_CHAT_UI_TICK_MS         30
/** 顶部状态栏高度 */
#define APP_CHAT_TOPBAR_H           26
/** 底部状态行高度 */
#define APP_CHAT_STATUSBAR_H        22
/** 界面配色（RGB888，内部转 RGB565） */
#define APP_COLOR_BG                0x0B1622    /* 背景 */
#define APP_COLOR_TOPBAR            0x14324F    /* 顶栏 */
#define APP_COLOR_TEXT              0xE6E6E6    /* 普通文字 */
#define APP_COLOR_USER_BUBBLE       0x2E6DA4    /* 用户气泡（浅蓝） */
#define APP_COLOR_AI_BUBBLE         0x3A3A3A    /* AI 气泡（浅灰） */
#define APP_COLOR_ERROR             0xFF5252    /* 错误文字 */
#define APP_COLOR_OK                0x66BB6A    /* 正常/已连接 */
#define APP_COLOR_WARN              0xFFD54F    /* 提示文字 */

#endif /* __APP_CONFIG_H */
