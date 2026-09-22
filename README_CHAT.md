# ESP32-S3 对话掌机（按键录音 → 百度 ASR → DeepSeek LLM → LCD 显示）

立创·实战派 ESP32-S3（LCKFB-SZPI-ESP32-S3-VA，ESP32-S3-WROOM-1-N16R8 / 16MB Flash + 8MB Octal PSRAM）
+ 2.0 寸 IPS 320×240（ST7789，SPI，LVGL9）
+ ES8311（DAC）+ ES7210（ADC，板载双麦）
+ GPIO0 录音按键。

```
┌──────────── ESP32-S3 ────────────┐
│ Core 1（应用/显示核）             │   Core 0（网络核）
│  lvgl_port_task（已有）           │    app_wifi_task  (prio 5)
│  app_key_task   (prio 4)          │    app_net_task   (prio 5, 栈 8KB)
│  app_audio_task (prio 6)          │        │
└───────────┬──────────────────────┘        │
            │ key_event_queue               │ pcm_queue
            │ audio_ctrl_queue              │
  GPIO0 ──> app_key ──> app_audio ──(PCM,PSRAM)──> app_net
                                     app_asr(百度) ─> asr_result_queue
                                     app_llm(DeepSeek) ─> llm_result_queue
            chat_msg_queue ──> app_chat_ui(LVGL 定时器) ──> LCD 中文气泡
```

## 一、填入密钥（不进仓库）

密钥统一放在**私有头文件**里，代码仓库只保留占位符模板：

| 文件 | 是否提交 | 内容 |
| --- | --- | --- |
| `components/app/app_config_template.h` | ✅ 提交 | 只有占位符，作为模板 |
| `components/app/app_config_secret.h` | ❌ 不提交（已在 `.gitignore`） | 真实密钥 |
| `components/app/app_config.h` | ✅ 提交 | 普通设置；`#include "app_config_secret.h"` 拿到密钥 |

**clone 后只需一步**：

```bash
cp components/app/app_config_template.h components/app/app_config_secret.h
# 然后编辑 app_config_secret.h，填入下面 6 个值：
```

| 宏 | 说明 |
| --- | --- |
| `APP_WIFI_SSID` / `APP_WIFI_PASSWORD` | 你的 WiFi 名称 / 密码（2.4GHz） |
| `BAIDU_ASR_API_KEY` / `BAIDU_ASR_SECRET_KEY` | 百度智能云「短语音识别标准版」应用的 API Key / Secret Key |
| `DEEPSEEK_API_KEY` | DeepSeek 开放平台的 API Key |
| `TAVILY_API_KEY` | Tavily 的 API Key（联网搜索工具用） |

其实**忘了复制也不会报错**：`components/app/CMakeLists.txt` 在配置阶段发现
`app_config_secret.h` 不存在时，会自动从模板复制一份（内容是占位符），
运行时日志会明确提示"还没有配置 xxx（填 components/app/app_config_secret.h）"。

也可以完全不用这个文件，直接在编译期覆盖（占位符都在 `#ifndef` 里，
命令行 `-D` 优先级更高）：

```bash
idf.py -DAPP_WIFI_SSID=\"myap\" -DAPP_WIFI_PASSWORD=\"12345678\" build
```

> ⚠️ **踩过的坑（务必知道）**：最初用 `#if __has_include("app_config_secret.h")`
> 让这个 include 变成可选的，结果出现过一个很隐蔽的问题——
> 如果在**还没有密钥文件**的情况下构建过一次，编译依赖文件（`.d`）里不会记录
> `app_config_secret.h`；之后你把密钥文件补上，ninja 认为"没有文件变化"，
> **不会重新编译**，固件里装的还是占位符，表现为一直提示"未配置密钥"，
> 而源码看起来完全正确。
> 现在改成 **无条件 `#include`** + CMake 保证文件存在，依赖关系正常，
> 改密钥一定会触发重新编译（实测：touch 该文件后 app_net/app_wifi/app_asr/
> app_llm/app_web_search 都会重编）。

**自检**：填完密钥后可以看看固件里到底装了什么（占位符构建会比正常小 ~30KB）：

```bash
python -c "d=open('build/empty_proj.bin','rb').read(); print(d.count(b'tvly-dev'), d.count(b'YOUR_TAVILY_API_KEY'))"
# 期望输出：1 0   （1 个真实 key，0 个占位符）
```

## 二、编译与烧写

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

字库：工程根目录的 `hzk16.bin` 会被自动裁剪并烧到 `fontbin` 分区（见根 `CMakeLists.txt`），
没有它中文会显示成占位框。

## 三、操作流程

1. 上电 → 开机动画 → 聊天界面（顶部 WiFi 状态 / 底部状态行）。
2. **按住 GPIO0**：状态行显示「录音中... 松开发送」，音频任务开始 16kHz/16bit 单声道采集，
   PCM 写进 PSRAM 缓冲（最大 15 秒 = 480000 字节）。按住超过 15 秒会自动停止录音。
3. **松开**：状态行「识别中...」，PCM 交给网络任务上传百度 ASR。
4. 识别文本作为用户气泡显示在右侧，状态行「AI 正在回复...」，于是请求 DeepSeek。
5. AI 回复显示为左侧气泡，状态回到「按住按键说话」。
6. 任何一步出错都会在状态行红色显示原因（网络超时 / 密钥未配置 / 音频太短 …），并回到待机。

气泡最多保留 12 条，超出自动删除最老的（LVGL 堆只有 64KB，不能无限堆）。

## 四、目录与分层

```
main/main.c                      组装层：board_init() -> app_splash_run() -> app_start() -> 删除 main 任务
components/bsp/                  ESP32-S3 片上外设（I2C/SPI/GPIO/WiFi）
  bsp_gpio.c/h                   ★新增：录音按键 GPIO0（输入+上拉+读电平）
components/my_drivers/           板上外部器件驱动
  audio/audio.c/h                ES8311/ES7210（已有）
  audio/audio_recorder.c/h       ★新增：PSRAM 环形缓冲录音器（16k/16bit/单声道）
  font_hzk16/hzk16.c/h           HZK16 字库（新增 hzk16_utf8_to_gb2312() 公开接口）
  net/net.c/h, lcd.c, board/…    已有驱动，未修改
components/app/                  应用层
  app.c/h                        ★app_start()：初始化 + 创建四个双核任务
  app_config.h                   ★全部配置宏（WiFi/密钥/任务/录音/界面）
  app_bus.c/h                    ★业务状态机 + 5 个跨任务队列
  app_key.c/h                    ★GPIO0 消抖状态机(esp_timer 20ms) + 业务状态机任务
  app_audio.c/h                  ★音频采集任务
  app_wifi.c/h                   ★WiFi(STA) 任务
  app_net.c/h                    ★网络任务：ASR -> LLM
  app_asr.c/h                    ★百度短语音识别
  app_llm.c/h                    ★DeepSeek 对话
  app_chat_history.c/h           ★最近 10 轮对话历史
  app_chat_ui.c/h                ★LVGL 聊天界面
  app_gb2312.c/h                 ★utf8_to_gb2312() 与文本宽度工具
  app_splash.c, game_launcher/…  已有，保留
```

调用层级：`app/` → `my_drivers/`、`bsp/`；`my_drivers/` → `bsp/`；`bsp/` 不依赖上层。
app 层不直接碰 I2C/I2S 寄存器，WiFi 走 `my_drivers/net`，按键电平走 `bsp/bsp_gpio`。

## 五、任务与队列

| 任务 | 核心 | 优先级 | 栈 | 职责 |
| --- | --- | --- | --- | --- |
| `app_wifi_task` | 0 | 5 | 4096 | WiFi 连接、断线 5 秒重连、状态上报界面 |
| `app_net_task` | 0 | 5 | 8192 | 取录音 → 百度 ASR → DeepSeek → 投递结果 |
| `app_key_task` | 1 | 4 | 4096 | 消费按键事件驱动状态机（IDLE→RECORDING→UPLOADING→LLM_REQUEST→IDLE） |
| `app_audio_task` | 1 | 6 | 4096 | 命令驱动采集 PCM 到 PSRAM，松手后上交数据 |
| `lvgl_port_task` | 1 | 已有 | 已有 | LVGL 刷新；聊天界面靠 lv_timer 在它的上下文里更新 |

| 队列 | 方向 |
| --- | --- |
| `key_event_queue` | app_key 定时器 → app_key_task |
| `audio_ctrl_queue` | app_key_task → app_audio_task |
| `pcm_queue` | app_audio_task → app_net_task |
| `asr_result_queue` | app_net_task → app_key_task |
| `llm_result_queue` | app_net_task → app_key_task |
| `chat_msg_queue` | 任意任务 → 聊天界面（LVGL 定时器消费） |

按键扫描用 `esp_timer` 20ms 周期回调（精度不受任务调度影响），
消抖状态机：`IDLE → DEBOUNCE_PRESS → PRESSED → DEBOUNCE_RELEASE → RELEASED`，
连续 2 次（40ms）同电平才算稳定；按下超过 15 秒产生 `TIMEOUT` 事件自动结束录音。

## 六、几个实现要点

- **录音格式**：板上是 ES7210 双麦 + I2S 立体声，录音器读立体声后做 `(L+R)/2` 降混成单声道，
  再以 16kHz/16bit 单声道交给 ASR，正好满足百度接口要求。
- **上传不复制**：识别请求体是 `{"format":"pcm",...,"speech":"<base64>","len":N}`，
  640KB 的 base64 如果整块拼进 JSON 会白吃一份内存，
  所以这里先算好 `Content-Length`，然后「JSON 前缀 + 分块 base64 + JSON 后缀」边编边发，
  只用 4KB 的编码缓冲。
- **token 缓存**：百度 access_token 有效期 30 天，缓存在内存里，提前 1 小时续期；
  服务端返回 `err_no=3302`（token 失效）时自动强制刷新并重试一次。
  ⚠️ 注意：取 token 的响应里带一整串 `scope`（几十项权限），实测约 1.4KB 且是
  **chunked 传输（没有 Content-Length）**，接收缓冲给小于这个长度就会被
  截断成非法 JSON —— 现在给的是 4KB（`s_token_resp` / `s_asr_resp`），
  日志里若出现「响应超过缓冲上限…已被截断」就是这个缓冲需要加大。
- **响应读取**：三个云端请求的响应体统一由 `app_http.c` 的事件回调累积
  （`http_on_body` 每解码出一段 body 就派发一次 `HTTP_EVENT_ON_DATA`，
  同一段只派发一次），再由 `app_http_pump()` 把 socket 抽干；
  解析失败时会把完整响应体打出来，便于定位。
- **对话历史**：内存里保留最近 10 轮（20 条），开机清空；请求时自动拼 `system + 历史 + 新 user`。
- **中文显示**：HZK16 是 GB2312 点阵，LVGL 的 hzk16 字体内部已经做了 Unicode→GB2312 转换，
  所以 label 直接喂 UTF-8 即可；`utf8_to_gb2312()` 用于按点阵宽度估算/截断文本。
- **TLS**：默认用 IDF 内置根证书包校验（`APP_TLS_USE_CA_BUNDLE=1`）。
  开发阶段若要跳过校验，把 `sdkconfig.defaults` 里的
  `CONFIG_ESP_TLS_INSECURE` / `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` 打开，
  并把 `APP_TLS_USE_CA_BUNDLE` 改成 0。

## 七、常见问题排查

| 现象 | 原因 / 处理 |
| --- | --- |
| 状态行「未配置SSID」 | `app_config.h` 里 `APP_WIFI_SSID` 还是占位符 |
| 状态行「获取token失败」 | 看 `APP_ASR` 日志：`error:invalid_client` = Key 填错；若出现「响应超过缓冲上限」= 加大 `s_token_resp` |
| 状态行「识别失败: 音频质量太差(3301)」 | 录音太小/太吵：确认对着板载麦克风说话，或调 `audio_set_mic_gain()`（默认 24dB） |
| 状态行「识别失败: 音频过短(3308)」 | 按住时间太短，多说一会儿（代码里 <400ms 的录音直接丢弃） |
| 状态行「识别失败: 鉴权失败(3302)」 | token 失效，代码会自动重取一次；仍失败说明 Key 无效或服务未开通 |
| 状态行「AI网络连接失败」 | 看 `APP_LLM` 日志；确认能访问 `api.deepseek.com`、Key 有效、账户有余额 |
| 状态行「AI服务错误: ...」 | 直接是 DeepSeek 返回的错误文本（额度/密钥/参数） |
| 中文显示成方框 | `hzk16.bin` 没烧进 `fontbin` 分区，重新 `idf.py flash` |
| 录不到声音 / 全是静音 | 板载麦克风是 ES7210，确认 `audio_init()` 与 `audio_mic_init()` 都成功（看 `audio` 标签日志） |

## 八、小游戏框架

原来的游戏中心（`game_launcher/`、`games/game_2048/`）代码完整保留。
聊天界面会新建并加载自己的 screen，会和游戏主页抢屏，
所以 `main/main.c` 里用 `APP_ENABLE_GAME_LAUNCHER` 开关控制，默认 `0`（不启动游戏框架）。
想同时保留，把它改成 1 即可。

## 九、内存布局与缓冲大小

### 9.1 大文本缓冲全部放 PSRAM

板子有 8MB PSRAM，而内部 DIRAM 只有 ~334KB，还要留给任务栈、队列、WiFi/TLS 的运行时分配。
所以**大块文本缓冲一律用 `EXT_RAM_BSS_ATTR` 放进 PSRAM**（`sdkconfig` 里开了
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`）：

| 缓冲 | 大小 | 位置 | 说明 |
| --- | --- | --- | --- |
| `s_asr_resp` / `s_token_resp` | 8KB / 6KB | PSRAM | 百度识别 / token 响应（token 响应实测 1.4KB 且是 chunked） |
| `s_asr_prefix` / `s_token_url` | 1KB / 512B | PSRAM | 拼请求用 |
| `s_asr_text` / `s_llm_reply` | 4KB / 4KB | PSRAM | ASR 文本 / AI 回复（一轮回复的硬上限 `APP_TEXT_MAX_LEN`） |
| `s_tool_results[2]` | 10.5KB | PSRAM | Function Calling 工具结果（每条 5KB） |
| `s_gb_tmp` | 4KB | PSRAM | UTF-8→GB2312 宽度估算 |
| `APP_LLM_RESP_BUF_SIZE` | 32KB | PSRAM（堆） | DeepSeek 响应，每次调用分配/释放 |
| `APP_WEB_SEARCH_RESP_BUF_SIZE` | 64KB | PSRAM（堆） | Tavily 响应，每次调用分配/释放 |
| `APP_CHAT_BUBBLE_TEXT_MAX` | 1KB/气泡 | **LVGL 内存池** | 只有它必须在 LVGL 池里（`CONFIG_LV_MEM_SIZE`） |

队列是运行时堆内存（内部 RAM），也按文本长度算过账：
`asr 2×4KB + llm 2×4KB + chat 8×2KB ≈ 33KB`。

### 9.2 内存监控：空闲任务触发 + 监控任务上报（`app_mem.c`）

分工（这点很重要，见下面的“坑”）：

| 角色 | 干什么 | 栈 |
| --- | --- | --- |
| 空闲任务（IDLE0）钩子 | 只做一次时间判断，到点 `xTaskNotifyGive()` 通知监控任务 | 1536 字节（IDF 默认，改不得） |
| 监控任务 `app_mem`（优先级 1） | 抢 LVGL 锁读池、读堆统计、打印 | 4096 字节（`APP_MEM_TASK_STACK`） |

开机先打一条基线，之后空闲任务每 30 秒触发一次：

```
I (xxx) APP_MEM: [启动基线] 内部空闲 160 KB（最大块 145 KB）| 历史最低 158 KB
I (xxx) APP_MEM: [启动基线] PSRAM 空闲 7.4 MB（最大块 7.4 MB）| LVGL池 12%（碎片 1%）
I (xxx) APP_MEM: [空闲任务] ...
W (xxx) APP_MEM: 内部 RAM 只剩 20 KB（阈值 24 KB）：建议调小 APP_TEXT_MAX_LEN ...
```

- **内部空闲 / 最大块**：最大块决定还能不能分配大缓冲（碎片多了就看它）；
- **历史最低**：开机以来最低水位，用来判断 TLS 握手那一下够不够；
- **LVGL 池**：气泡文字用的池子占用率与碎片率（读之前会先抢 LVGL 锁，拿不到就跳过这次统计）；
- 低于阈值会额外告警，并提示该调哪个宏。

> ⚠️ **踩过的坑**：第一版是直接在空闲任务钩子里 `ESP_LOGI`，结果开机就
> `A stack overflow in task IDLE0 has been detected`。原因是
> `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE` 只有 1536 字节，而一次带多个参数的
> `ESP_LOGI`（vsnprintf + UART 写）就要几百字节，再加上读堆统计和 LVGL 池链表，
> 直接冲爆。而且空闲任务还负责回收被删除任务的 TCB/栈，绝不能在里面阻塞。
> 所以改成“空闲任务只发通知、监控任务干重活”。
> 如果你更想让空闲任务自己打印，就得把 `CONFIG_FREERTOS_IDLE_TASK_STACKSIZE`
> 提到 4KB 以上——但不推荐。

调参入口都在 `app_config.h`：`APP_MEM_LOG_INTERVAL_MS` 改周期、
`APP_MEM_LOG_ENABLE=0` 直接关掉、`APP_MEM_WARN_*` 改告警阈值、
`APP_MEM_TASK_PRIO` / `APP_MEM_TASK_STACK` 调监控任务本身。

### 9.3 任务栈监控与"栈炸弹"（踩过的大坑）

第二条日志里会多一行各任务栈剩余量（历史最小值，来自 `uxTaskGetStackHighWaterMark`）：

```
I (xxx) APP_MEM: 任务栈剩余(字节，历史最小值): app_wifi=3210 app_net=4520 app_key=2800
                 app_audio=3100 app_mem=2900 taskLVGL=3600
W (xxx) APP_MEM: 有任务栈剩余不足 1024 字节，随时可能爆栈（查局部大数组/大结构体）: ...
```

**踩过的坑**：把文本缓冲从 1152 放大到 4096 之后，
`app_bus_post_asr()` / `app_bus_post_chat()` 里的 `app_text_msg_t msg;` / `chat_msg_t msg;`
变成了 **4KB / 2KB 的局部变量**——它们落在**调用者的栈**上。
而 `app_audio_task`/`app_key_task` 的栈只有 4KB、`app_key_task` 里还有一个 4104 字节的
接收缓冲，于是栈被写穿，把相邻任务的栈/TCB 冲掉，表现为**另一个核上的任务**
莫名 `InstrFetchProhibited`（PC=0xffffffff，返回地址被写成 0xffffffff）。

修复方式（现在代码里就是这么写的）：

1. `app_bus_post_asr/llm/chat/chatf` 全部改用**静态暂存区 + 互斥锁**发送，
   函数自身栈帧降到 ~100 字节（锁只包住一次 `xQueueSend`）；
2. 任务里接收大消息的缓冲（`app_key_task` 的 ASR/LLM 结果、`app_chat_ui` 的聊天消息）
   一律改成 `static`；
3. `app_key`/`app_audio` 栈从 4096 提到 5120，留出余量。

**审计工具**：`components/app/CMakeLists.txt` 里加了 `-fstack-usage`，
每个源文件都会生成 `.su`，能看到每个函数的栈帧：

```bash
idf.py build
# 列出 app 组件里栈帧最大的函数（单位：字节）
grep -h -P '\t\d+\t' build/esp-idf/app/CMakeFiles/__idf_app.dir/*.su \
  | awk -F'\t' '{print $2, $1}' | sort -rn | head -20
```

修复前后对比：最大栈帧从 **4104 字节** 降到 **576 字节**，
各任务入口函数（`app_net_task`/`app_audio_task`/`app_key_task`/`ui_timer_cb`）都在 64 字节以内。
新增代码后建议跑一次这个命令，只要没有几百字节以上的栈帧就很安全。

### 9.4 文本缓冲尺寸怎么定（稳定优先）

内部堆只有 ~139KB，还要养任务栈（~20KB）、WiFi/LWIP（~40KB）、TLS 连接（~22KB）。
**队列是内部堆内存**，所以"把文本缓冲改大"最容易踩的坑就是把内部堆挤干——
挤干之后不是"分配失败报个错"，而是 WiFi 驱动拿不到收包缓冲、TLS 写阻塞，
表现成上传卡 20 秒后 `Poll timeout … 写请求体失败`。

当前配置（稳定优先，实测内部堆余量充足）：

| 项 | 值 | 在哪里占内存 |
| --- | --- | --- |
| `s_token_resp` | 4096（百度实际响应 ~1.4KB） | PSRAM（`EXT_RAM_BSS_ATTR`） |
| `s_asr_resp` | 4096 | PSRAM |
| `APP_TEXT_MAX_LEN` | 2048（≈1000 汉字） | asr/llm 队列，内部堆 2×2KB |
| `APP_CHAT_TEXT_MAX` | 1024 | chat 队列，内部堆 6×1KB |
| `APP_CHAT_BUBBLE_TEXT_MAX` | 1024 | LVGL 池（64KB，实测占用 ~14%） |
| 三个队列合计 | **14.4KB** | 内部堆（改大前激进版是 32.8KB） |
| `APP_LLM_RESP_BUF_SIZE` | 32KB | PSRAM 堆（每次请求分配/释放） |
| `APP_WEB_SEARCH_RESP_BUF_SIZE` | 64KB | PSRAM 堆 |
| 工具结果 `s_tool_results[2]` | 2×5KB | PSRAM |

**想再放大文本**：优先动 PSRAM 上的（`APP_LLM_RESP_BUF_SIZE`、`APP_WEB_SEARCH_*`、
`APP_TEXT_MAX_LEN` 之外那几个 `EXT_RAM_BSS_ATTR` 缓冲）；
动队列尺寸前先看日志里的"内部空闲 / 最大块"，留出 30KB 以上余量再动。

**ASR 上传失败会自动重试**：传输层失败（没拿到服务端响应，例如上传中途链路卡死）
会换一条新连接重试一次，避免一次网络抖动报废整轮对话；日志里能看到
`没拿到服务端响应（上传失败），1 秒后换新连接重试一次`。

### 9.5 双核任务分配（含 LVGL 任务，别让它被网络任务饿死）

| 核心 | 任务（优先级） |
| --- | --- |
| Core 0（网络核） | `app_wifi(5)`、`app_net(5)`、`app_mem(1)`、WiFi/LWIP 内部任务 |
| Core 1（应用核） | `app_audio(6)` > `taskLVGL(5)` > `app_key(4)` |

**踩过的坑**：`ESP_LVGL_PORT_INIT_CONFIG()` 默认 `task_affinity = -1`（不绑核），
而 `lcd_init()` 是在 **main 任务**里跑的、main 被钉在 CPU0，
于是 LVGL 任务也落在 CPU0，优先级 4 —— **比 `app_wifi`/`app_net`(5) 低**。
后果：只要网络任务在跑 TLS/HTTP，LVGL 就被饿死，界面不刷新，
表现为"识别结果和 AI 回复两个气泡一起冒出来""录音中的状态字不动""整机反应变慢"。

现在在 `lcd.c` 里显式指定：`lvgl_cfg.task_affinity = 1; lvgl_cfg.task_priority = 5;`
（`LCD_LVGL_TASK_CORE` / `LCD_LVGL_TASK_PRIO`）。

**对话上屏时序**（改完后）：

```
松手 → 状态“识别中...”          （app_key_task 切状态，30ms 内上屏）
百度返回识别结果
     → 用户气泡立刻上屏 + 状态“AI 正在回复...”（app_net_task 直接投递，不等转发）
几秒后 DeepSeek 返回
     → AI 气泡上屏 + 状态回到“按住按键说话”
```

注意：网络本身的耗时没变（TLS 握手 + 上传 + 模型推理），
变的是**界面不再等网络**，所以"用户说的话"能先看到。

### 9.6 注意事项

- 打开 `.bss` 放 PSRAM 后，**PSRAM 变成硬依赖**：IDF 会把 `lwip`/`net80211` 等的
  `.bss` 一并放到 PSRAM（这是该选项的官方行为，也正是它省出 ~50KB DIRAM 的原因）。
  板子上的 PSRAM 一直正常就没问题；想退回原来的内部 RAM 布局，把这个选项关成 `n` 即可
  （`EXT_RAM_BSS_ATTR` 会自动变成空属性，代码不用改，只是缓冲回到内部 RAM）。
- `CONFIG_LV_MEM_SIZE` 是**内部 RAM 的静态数组**，别盲目加大：它的池子是给
  LVGL 控件和气泡文字用的，气泡数 × 单个气泡文字长度别超过池子的一半。
- 想再放大文本能力，优先动 PSRAM 上的那几个宏（`APP_TEXT_MAX_LEN`、
  `APP_LLM_RESP_BUF_SIZE`、`APP_WEB_SEARCH_*`），不要动 LVGL 池。
