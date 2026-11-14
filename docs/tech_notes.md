# Panda AI 技术积累 / 实测笔记

> 记录真机验证过的细节，避免后续重复踩坑。日期：2026-11-09。

## 1. 当前真机配置

| 项目 | 状态 | 说明 |
|---|---|---|
| 主控 | 已确认 | ESP32-S3 v0.2，16MB Flash，8MB PSRAM，240MHz |
| 串口 | 已确认 | USB-SERIAL-JTAG，`/dev/ttyACM0`，115200 |
| I2C0 | 已确认 | SCL=GPIO10，SDA=GPIO11，400kHz |
| TCA9555 | 已确认 | 0x20，LCD_RST/TP_RST/PA_EN/CAM_EN/CAM_MUX/按键 |
| ES8311 | 已确认 | 0x18，I2S TX，扬声器未接 |
| ES7210 | 已确认 | 0x40，I2S RX，MIC1/MIC2 可用 |
| PCF85063 | 已确认 | 0x51，断电/复位后时间保持 |
| SD 卡 | 已确认 | 1-bit SDMMC，SA02G 1876MB，挂载 `/sdcard` |
| RGB | 已确认 | WS2812 GPIO38 ×7 |
| 屏幕/触摸 | 未连接 | CST816S 探测 NACK，启动容错后继续 |
| 电池 ADC | 未接线 | R53/R78 0R 未焊，GPIO1 悬空；分压 200K/100K（×3） |

## 2. 关键坑位

### TCA9555 上电顺序

TCA9555 的输出锁存器复位值为 `0xFF`。初始化必须先 `set_dir(out_mask, OUTPUT)`，再 `set_level()`；
在输入模式下调用 `set_level()` 会报 `Pin[x] can't set level in input mode`，且最终输出电平不可控。
正确顺序后实测 `level=0xfe8f`：LCD_RST/TP_RST 高、PA_EN/CAM_EN/CAM_MUX 低、按键上拉为高。

### 无屏幕启动

`panda_display_init()` 在没有接屏时仍能完成 QSPI 初始化（面板写命令无回读）。
`panda_touch_init()` 会因 CST816S NACK 失败，不能 `ESP_ERROR_CHECK`，否则重启循环。
`app_main` 现在：显示失败则禁用 UI；触摸失败只告警；`panda_ui` 内部有 `s_ui_ready` 保护。
无屏时 CLI、音频、SD、RTC 仍可用，便于台架验证。

### 时间/RTC

`settimeofday()` 的 boot time 在 `CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER` 下写入内部 RTC 域，
软件复位后系统时间可以保持；PCF85063 外部 RTC 也实测跨烧录保持。
`rtc_regs_valid()` 原来接受 year=0（2000 年），会把未校准 RTC 误判为有效；已改为 `year >= 24`。

### 亮度

`panda_display_set_brightness()` 有 5% 下限：80% 实际映射为 81%，属预期。
初始化前先保存目标值，避免 `set_brightness(0)` 覆盖缓存值。

### 音频

- I2S0 全双工：MCLK=12，BCLK=13，LRCK=14，DIN=15，DOUT=16；16kHz/16bit/mono。
- ES8311 `pa_pin=GPIO_NUM_NC`，PA 由 TCA9555 P1.0（bit8）控制；`panda_audio_init()` 内使能。
- ES7210 选 MIC1+MIC2，录音 slot0（`ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0)`），增益 37.5dB。
- 无喇叭时 `audio test` 仍返回成功；麦克风自检实测 rms 19~60、peak 70~351。
- 播放使用 `esp_codec_dev_write()`，后续 voice 层需要自己加 PSRAM 环形缓冲和淡入淡出。

### 内存

启动完成后内部 RAM 约剩 100KB（PSRAM 约 8.3MB）。LVGL 双缓冲 360×40 在内部 DMA。
接入 WiFi + WebSocket 时，音频/JSON/环形缓冲应优先放 PSRAM，内部 RAM 只留 DMA/协议栈。

### 串口 CLI

`idf.py monitor` 需要 TTY；自动化验证用 pyserial 打开 `/dev/ttyACM0` 并关闭 DTR/RTS。
常用：`status`、`i2c`、`battery`、`sdcard status`、`time`、`audio test`、`rgb`、`settings show`。
`settings show` 只输出脱敏凭证；默认 `app_key` 已清空，必须通过 CLI/NVS 配置。

## 3. WiFi / 配网（2026-11-09 补充）

- `panda_connectivity_init()` 按 NVS 是否已有 SSID 二选一：
  - 有凭证 → `WIFI_MODE_STA`，开机只连路由器，**SoftAP 不启动**；
  - 无凭证 → `WIFI_MODE_APSTA`，SoftAP 名 `CONFIG_PANDA_WIFI_AP_PREFIX-XXXX`，
    密码 `CONFIG_PANDA_WIFI_AP_PASSWORD`（默认 `pandapanda`）。
- `app_provisioning_start()` 无论哪种模式都会起 HTTP 服务，但 AP 未开启时
  `192.168.4.1` 不可达。已有凭证后要进配网页，必须先 `wifi ap on`（或 `wifi forget` 后重启）。
  （2026-11-09 本轮已加 20s 无 IP 自动回退 SoftAP，见第 7 节；此条保留为历史行为。）
- 配网端点：`/` 表单、`POST /save`、`GET /api/status`；另有 4 个 captive portal 探测路径。
- 实测开机 `wifi: STA disconnected, reason=2` 属于首次鉴权超时/环境未就绪，不是驱动错误；
  开启 `wifi ap on` 进入 APSTA 后会重试成功（AP channel 从 1 调整到 STA 的 6）。
- 2026-11-09 实测：STA 连上 <家庭WiFi>，IP `192.168.x.x`、RSSI `-46 dBm`，
  SNTP 日志 `wifi: time synced: 2026-11-09 15:12:35`。
- 配网 HTTP 已从主机直连设备 LAN IP 验证：`/`、`/api/status`、`POST /save`、
  `/generate_204`（302→`http://192.168.4.1/`）。已有凭证时页面仍只在 SoftAP 开启时监听可达。

## 4. 语音链路（2026-11-09 补充）

- 当前只实现 demo 的 JSON 链路：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`，
  请求头仅 `X-Api-Key`；`api_mode=o2` 会打印 `not implemented` 并返回 `ESP_ERR_NOT_SUPPORTED`。
- 默认值已于 2026-11-09 改为 `api_mode="json"`、`model="1.2.1.1"`；
  实现 O2.0 后应切回 `o2` 并复测 NVS 清空场景。
- 缓冲区（全部 PSRAM）：环形播放 256K samples（512KB）、RX 64KB、TX 8KB、
  PCM24 64KB、PCM16 48KB；24k→16k 用步进 1.5 的线性插值，不是多相重采样。
- `app_core_task()` 开机延时 600ms 后直接 `app_voice_start()`，只要有 `api_key` 就建链；
  无网络时 `esp_websocket_client` 固定 5s 重连，日志会持续刷 DNS/TLS 错误。
- 上行在无采集时发送 20ms 静音保活（640B/帧），避免服务端 `NoAudioInputTooLong`。
- 播放/采集互斥（半双工），无 AEC；打断走 `app_voice_cancel()`/`send_cancel`。
- 取消帧为 `{"type":"response.cancel"}`（与 demo 一致，不带 event_id）；服务端不一定回
  `response.canceled`。取消后必须本地丢弃迟到的 `response.output_audio.delta`，否则
  ring 会被重新填满（修复前实测 `tts done ring=3430`，修复后 `ring=0`）。
- `response.output_audio.done` 必须同时把本轮 `s_audio_started` 置 false，
  否则服务端不补 `response.done` 时 UI 会一直停在“说话”（修复前实测 7s 后仍为 Speaking）。
- 测试音频（`voice clip`）的 ASR 结果为空，只能验证会话/TTS 通路，不能验证麦克风识别质量。

## 5. 内存（2026-11-09 复测）

- 启动并启动语音客户端后：`internal=46399`、`psram=7574608`、`min_internal=35623`。
  比早期记录的“约 100KB”更低，WiFi+TLS+WebSocket 并发时需重点观察内部堆。
- 新增缓冲一律用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`；内部 DMA/协议栈余量已不大。

## 6. 自动化串口注意事项

- 本次用 pyserial 打开 `/dev/ttyACM0` 时捕获到完整启动日志，说明打开串口可能触发复位；
  自动化脚本应先丢弃启动段，再发命令，避免把启动日志当成命令输出。
- 打开后立即设置 `dtr=False; rts=False`，否则可能一直处于复位/下载模式。
- `idf.py monitor` 需要 TTY，CI/代理环境用 pyserial 方式。

## 7. WiFi / 音频 / 字体 / TTS 修复要点（2026-11-09 本轮）

### WiFi 稳定性

- ESP-IDF 默认开启 modem sleep（`WIFI_PS_MIN_MODEM`），空闲时周期性关射频，
  表现为“隔一段时间掉线/延迟尖峰”。`panda_connectivity_init()` 在 `esp_wifi_start()`
  后调用 `esp_wifi_set_ps(WIFI_PS_NONE)`；代价是待机电流上升，后续做深睡时需重新评估。
- STA 配置改为 `WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL`，并设
  `pmf_cfg.capable=true`（required=false），避免 WPA3 过渡路由器的关联失败。
- 重连从固定 2s 改为指数退避 1s→30s，带 ±25% 抖动，防止多设备同步重连；
  成功拿到 IP 后复位退避。`wifi status` 打印 `retry`、`reason`、`reason_hint`。
- 有凭证但 20s 未拿到 IP 时启动 `ap_fallback` 定时器自动开 SoftAP（`ap_auto=1`），
  解决“密码写错后配网页永不可达”的死循环；手动 `wifi ap off` 会设 `s_ap_user_off` 抑制自动开启。
- SoftAP 信道跟随最近一次 STA 信道（`s_sta_channel`），避免 AP/STA 异信道导致的共存问题。
- `esp_websocket_client_stop()` 内部对 `STOPPED_BIT` 用 `portMAX_DELAY` 等待，且会 join
  自身任务；**不能在 WS 事件回调里调用**。WiFi 断开时不要 stop 客户端，靠其内置重连。

### 音频锁 / TTS 播放

- 原实现播放与采集共用一个互斥锁：采集 `esp_codec_dev_read()` 阻塞时播放线程被挡住，
  I2S DMA 排空后出现“断一下”。现拆为 `s_tx_lock`/`s_rx_lock`，音量/静音走 TX 锁。
- 24k→16k 线性插值若每个 delta 都重置相位，会在包边界丢/重样本，听感是周期性咔哒。
  现保留 `s_rs_pos` 与上一帧尾样本，跨 delta 连续插值；取消/重连时显式 reset。
- 播放前预缓冲 3200 samples（200ms）再开 I2S，避免首包抖动导致开头被截；
  写 `esp_codec_dev_write()` 的返回值小于请求时循环续写（最多 20 次），超时不再当作写成功。
- 豆包下行会把大 JSON 拆成 WebSocket 续帧（opcode `0x0`）；只处理 `0x1` 会丢掉后半段。
  现同时接受 `0x1`/`0x0`，并在 `connect`/`disconnect`/`cancel` 时复位解析与播放状态。
- RX 缓冲溢出时旧实现整段丢弃，JSON 从此错位。现用 `memchr(data,'}',len)` 找下一个
  完整包边界继续解析；`voice stats` 的 `rx_overflows` 可观察是否仍溢出。
- `voice stats` 计数：`deltas`（收到音频包）、`decode_failures`、`ring_drops`（ring 满）、
  `underruns`（播放中 ring 空）、`write_retries`、`rx_overflows`、`ring_samples`、`tx_frames`。

### 中文字体

- `ui_font_CJK16` 由 `tools/fonts/gen_cjk_font.py` 生成（SourceHanSansSC 16px 4bpp，
  GB2312 + ASCII + 常用标点），并挂到 `Big60/Body18/Small16/Title22` 的 `.fallback`；
  LVGL 9 的 `lv_font_get_glyph_dsc()` 会递归查 fallback。
- 坑：SquareLine 重新导出会覆盖 `.fallback` 行，必须重跑补丁；`check_coverage.py`
  会检查四个字体是否仍带 `.fallback = &ui_font_CJK16`。
- 坑：`lv_font_conv` 1.5.2 生成的是 LVGL 8 代码（`lv_font_fmt_txt_glyph_cache_t cache`
  与非简化 `lvgl/lvgl.h`）；生成脚本已自动剥离，LVGL 9 下可直接编译。
- 源 OTF 对部分 `0x2000–0x206F`/`0x2100–0x214F` 码位无字形，lv_font_conv 会静默跳过
  （实测 234 个请求码位未生成）；这不是错误，只检查 UI 实际用到的字符即可。
- `python3 tools/fonts/check_coverage.py` 解析生成字体的 cmap，扫描 `main/`、
  `panda_ui/`、`panda_state/` 字符串字面量，缺字或 fallback 丢失时返回非 0。
- 加入 CJK 字体后 `panda_ai.bin` 从 `0x70ccc0` 增至 `0x7f7180`，factory 余量 30%→20%；
  再加入按键逻辑后为 `0x7f7400`，余量仍为 20%。

### 实体按键

- KEY1–3 经 TCA9555 P1.1–P1.3，低有效；BOOT 直连 GPIO0。`app_core_task()` 每 20ms
  轮询，稳定态需连续两次采样一致；KEY1–3 长按阈值 600ms，BOOT 为 4s。
- 当前映射：KEY1 音量 −10%、KEY2 音量 +10%、KEY3 短按循环设置页、长按回 `ui_home`；
  BOOT 短按提示、长按 4s 开/关配网（AP + HTTP 配网页）。PTT 只保留触摸长按。
- BOOT 曾用于按住说话（PTT），4s 配网与它直接冲突，且 GPIO0 是 strapping 脚：
  开机时按住会进 ROM 下载模式，所以配网手势只能在固件运行后检测。经确认改为
  BOOT 专用于配网，避免长说话被 4s 截断；触摸 PTT 不受影响。
- 音量步进同时写 NVS、`panda_audio_set_volume()`、`panda_ui_set_volume()` 并弹 toast；
  每次按键写一次 NVS，日常使用频度远低于擦写寿命，若以后做长按连发应改为松手保存。
- 菜单直接 `lv_screen_load_anim()` 切已创建的生成页，不派发状态机事件；状态机对
  user page 有保护，但语音/错误/低电等紧急屏仍会抢占。菜单索引从当前活动屏反查，
  触摸导航后再按 KEY3 也能接续，不会跳回固定页。
- `key 1|2|3 [long]` CLI 调用同一个 `app_core_key_action()`，无硬件时可验证音量与翻页。

## 8. 状态机 / UI / 图片（2026-11-09 补充）

### 三维状态机

- `panda_ai/components/panda_state/` 是纯 C、无 LVGL/FreeRTOS 依赖，主机和固件共用同一份代码。
- 三个独立维度：`device`（boot/ready/sleep/low_battery）、`voice`
  （disconnected/connecting/ready/listening/thinking/speaking/cancelling）、`emotion`
  （neutral/happy/curious/surprised/sad/shy/sleepy/angry），另有独立 `error` 与 UI 屏幕枚举。
- 屏幕优先级固定为：`sleep > low_battery > error > wake > offline > voice > emotion > idle`。
  网络断但仍开心时，状态机保留 `emotion=happy`，屏幕先显示 Offline，恢复后回到情绪屏。
- 超时：wake 1.2s、情绪 2.5s、错误 4s、连接 20s、聆听 15s、思考 20s、取消 0.4s。
  连接超时转 Offline；聆听超时转 Thinking；思考超时转 LLM Error；取消超时转 Listening。
- `SLEEP` 只改 UI 维度：不改 Wi-Fi/socket，语音维度呈现为 Ready；`WAKE` 后先显示 Wake 1.2s，
  再按当前 voice 回 Idle/Listening/Reconnecting。睡眠中 PTT 会先恢复设备可用态。
- `READY` 表示设备可用，不重新判低电；低电由后续 `BATTERY` 事件重新断言，
  避免残留 10% 电量把“已恢复”又立刻压回 LowBattery。
- 生成 UI 的设置/通知/预览页由手势导航；状态机只在紧急屏幕（语音轮次、错误、低电、睡眠）
  到来时抢占，避免用户停在设置页时被待机状态拉回。

### 浏览器全页面仿真

- `tools/ui_sim/web/index.html` 的“全部页面”读取 `pages.js`：20 个状态屏直接复用
  360×360 图片并在前端重绘波纹/tip，12 个设置页和 6 个扩展表情页来自 `generated/screens`。
- `tools/ui_sim/gen_pages.py` 解析 `ui_*_screen_init()` 的创建顺序、中心点/宽高、颜色、
  圆角、文本/字体/对齐、图片和 `_ui_screen_change()` 回调，输出静态布局；不包含运行时
  才写入的亮度/音量/WiFi 文案，保持生成时的默认值。
- 坑：`lv_obj_set_style_radius(..., 14, (lv_style_selector_t)...)` 的数字后还有 state 参数，
  正则若写成 `\(\d+\)` 会把所有圆角读成 0（快捷设置按钮变方、开关变方块）。
  改为 `\([^,]+,\s*(\d+)` 后恢复圆形/胶囊形状。
- 页面对象数 = `screen_init` 中 `lv_*_create()` 数减 1（根屏幕以 `NULL` 为父对象，不参与布局）。
- 验证：headless Firefox 打开 18 个设置/表情页，33 个页面图片 `naturalWidth>0`；
  `quick_settings→system_settings`、`state_preview→wake`、`character_preview_1→face_sad`
  三条生成 UI 链接均正确切页。

### 图片资源

- 图片全部编译进固件（`generated/images/*.c`），运行时不依赖 SD。58 个描述符共 4.9MiB：
  360×360 RGB565 熊猫脸 19 张、170×170 mini 1 张、60×60 缩略图 19 张、16~30px 图标 19 张。
- 图标含 `RGB565A8`、`I8/I4/I2` 索引格式；索引格式的 `data_size` 包含尾部调色板
  （I8: +256×4，I4: +16×4，I2: +4×4），不能按纯像素字节数判断。
- 主机校验：`python3 tools/ui_sim/check_images.py` 检查 magic=`0x19`、颜色格式、宽高、
  `data_size`、数组实际长度、空白像素，以及所有 `lv_image_set_src(&...)` 引用；
  `--sheet` 可导出熊猫脸接触表人工复核。`make -C tools/ui_sim test` 会同时跑状态机和图片校验。
- 真机校验：`ui images` 遍历 37 屏的所有图片对象，检查 source 类型、描述符字段、
  `data_size` 与 `lv_image_decoder_get_info()` 的解码结果。当前基线：
  `37 screens, 71 image objects, 58 unique images, 0 failures`。
- 无面板时 `panda_display_init()` 仍会创建 QSPI/LVGL 和 37 屏，因此解码与切屏可验证；
  实际像素、色彩、撕裂、手势和帧率必须接屏后再看。
- 错误屏幕映射：`ui error asr|tts` 分别到 `ui_asr_failed`/`ui_tts_error`，
  `network` 到 `ui_network_lost`，`auth|llm|generic` 到 `ui_llm_error`，`audio` 复用 TTS 错误屏；
  `ui error clear` 立即回 Idle（正常错误 4s 自动过期）。实测四种错误屏幕均切换正确。
- 实测 `ui state listening` 后若 WebSocket 恰好连上，服务端 `session.created` 会把状态
  从 Listening 拉回 Ready/Idle；这是语音事件优先于手动注入状态，不是 UI 映射错误。

### 构建尺寸与 LVGL 内存

- 本轮新增状态机/图片检查代码后：`panda_ai.bin = 0x70ccc0`（约 7.0MB），factory 剩余 30%。
- 状态机文件很小，尺寸增长主要来自 `-O0`/调试段与新增字符串；图片像素数据原本已链接。
- 37 屏常驻创建后 LVGL 池：`used=71/85 KB max=73 KB (84%), frag=11%`（配置 96KB）。
  当前 overlay 已创建完，仍有约 12KB 余量；若后续加动画/滚动列表需先扩 `CONFIG_LV_MEM_SIZE_KILOBYTES`
  或改为按需创建屏幕，否则可能出现 `lv_malloc` 失败。

## 9. KEY3 菜单 / 天气 / 自动休眠（2026-11-09 晚）

### 生成 UI 的“假控件”要替换而不是改属性

- SquareLine 导出的亮度/音量滑块是三块静态对象（`ui_*_s_track/fill/knob`），没有
  `lv_slider` 行为。做法：`hide_widget()` 隐藏三件套，再在 `ui_system_settings` 上
  以相同中心/宽度创建真实 `lv_slider`（宽 236、高 8、y=-72/-14）。
- 滑块条本身只有 8px 高，触摸很难命中；`lv_obj_set_ext_click_area(slider, 18)` 把可点区
  扩到上下 18px。拖动过程用 `LV_EVENT_VALUE_CHANGED` 实时预览，`LV_EVENT_RELEASED`
  才写 NVS + toast，避免拖动中频繁擦写。
- 选项行是 `_off` 胶囊 + `_on` 蓝色叠加 + `_t` 文本；三者都要绑点击事件（用户可能点到
  任意一层）。`user_data` 用 `(group<<4)|index` 编码，一个回调处理所有组。
- 蓝牙 tile（`ui_bt_off/on/ic/lbl`）直接隐藏：固件没有 BT 协议栈，留假开关比隐藏更糟。
  隐藏后把亮度 tile 的 x 设为 0 补位。
- 周期刷新（`panda_ui_menu_sync`）会回写控件，可能触发 `LV_EVENT_VALUE_CHANGED`；
  用 `s_menu_syncing` 在同步期间忽略滑块事件，否则会把 UI 回写误当成用户提交。

### 自动休眠 / 唤醒

- 睡眠判定每秒一次：`auto_sleep_s > 0`、当前不在睡眠、语音状态为 IDLE 或 OFFLINE、
  距最后一次活动 ≥ 阈值。活动来源包括触摸按下、按键稳定按下、语音状态事件；
  菜单页也计入“活动”，不强制回主屏才休眠。
- 唤醒输入必须“只唤醒、不执行原动作”：按键/触摸按下时先记录 `wake_if_sleeping()`，
  同一次按下不再触发 PTT/音量/菜单；否则睡梦中按 KEY1 会先唤醒又减音量。
- 当前 SLEEP 只改 UI/状态机维度，Wi-Fi、WS、音频仍在线；深睡与功耗策略未做。

### Wi-Fi 开关 / 勿扰

- 关 Wi-Fi = `panda_connectivity_ap(false)` + `panda_connectivity_disconnect()`；
  `disconnect()` 会置 `s_manual_disconnect`，指数退避重连不会立刻把 Wi-Fi 又打开。
- 开 Wi-Fi：有 NVS SSID 调 `connect()`；没有凭证则开 SoftAP 配网并在 UI toast 说明。
- 勿扰不是“关音量”，而是 `panda_audio_mute(true)`（`esp_codec_dev_set_out_mute`），
  保存的 volume 不变；关闭后恢复原音量。`dnd` 落 NVS，开机由 `app_main` 应用。

### SD 容量 / 版本号

- ESP-IDF 没有 `sys/statvfs.h`；用 `esp_vfs_fat_info(mount, &total, &free)`，单位字节，
  再除 1024 显示 KB/MB/GB。挂载点从 `panda_board_sdcard_mount_point()` 取，不要写死。
- 版本号用 `esp_app_get_description()->version`，需要在组件的 `PRIV_REQUIRES` 加
  `esp_app_format`，否则 `esp_app_desc.h` 找不到。`snprintf` 用 `"v%.31s"` 避免
  `-Werror=format-truncation`。

### 天气（Open-Meteo）

- 先 geocoding：`https://geocoding-api.open-meteo.com/v1/search?name=<urlencoded>&count=1&language=zh&format=json`，
  取 `results[0].latitude/longitude`；再 forecast：
  `https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..&current=temperature_2m,weather_code`。
- 实测 geocoding 北京返回 `39.9075, 116.39723`；forecast 返回
  `temperature_2m=3.6, weather_code=0`。字段与解析代码一致。
- 中文城市名要先做 UTF-8 percent-encoding；不要用 `http://`，设备已启用
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`，配置 `.crt_bundle_attach = esp_crt_bundle_attach`。
- 4KB 响应缓冲和 8KB 任务栈都放 PSRAM（`heap_caps_malloc`/`xTaskCreatePinnedToCoreWithCaps`）；
  每次请求前检查内部堆，低于 28KB 跳过，避免与语音 WS 的 TLS 抢内存。
- 刷新策略：成功 30min；失败 60s；Wi-Fi 未连接 10s 轮询。WMO 码只映射 3 个图标
  （0/1 晴、2 多云、其余阴），雨雪只改文字。
- 城市来源：NVS `wx_city`、配网页输入、CLI `weather set <city>`；保存后立即刷新。

### persona 组合

- 性格（活泼/温柔/安静）与反馈强度（弱/中/强）不是单独发给模型的字段，而是在
  `session.create` 前拼成一段中文提示词，再接用户自定义 `persona`。
- 因此修改后**下一轮对话**才生效，已在 UI toast 说明；不影响当前正在播放的回合。
- 提示词缓冲 800B 在栈上，`json_escape` 后写入会话请求；不要把它放内部堆。

## 10. STT 字幕 / TTS 爆音 / 播放卡死（2026-11-10）

### 转写 delta 不能直接 append

- 豆包 `conversation.item.input_audio_transcription.delta` 的形状不统一：有的只发新增尾巴，
  有的每次都发整句。旧逻辑“delta 全 append + completed 再 append 整句”会得到
  `hellohellohello`，且迟到的 delta 还会追加到已完成的文本后面。
- demo 的做法是不渲染 delta：`DOUBAO_EVT_TRANSCRIPT_DELTA` 只更新内部状态，
  用户气泡在 `TRANSCRIPT_DONE` 时一次性填完整文本。现按此实现；delta 只打前 12 条日志，
  `completed` 设置 `s_asr_done` 并渲染一次，`started` 清空文本/计数。
- 回复文本 (`response.output_text.delta/…) 仍可能重复，用 `merge_text()` 判断三种形状：
  delta 以当前文本开头→替换（累积）；delta 已在文本尾部→忽略（重复）；否则→追加。

### 24k→16k 必须用多相滤波，不能用线性插值

- 线性插值相当于一个很弱的低通，24k 语音里的 8–12kHz 镜像会落进 16k 带内，
  听感是轻微“沙/爆”；TTS 尾音和强辅音处最明显。
- 移植 demo `resampler.c`：32 相 × 32 抽头，Kaiser β=8 窗，Q15 系数，按
  `coeffs[(phase*32)+i]` 与 delay 反转的历史样本卷积；每 3 个输入出 2 个输出，
  相位 0→1→2 循环，跨 WebSocket delta 保持 `history/wr_idx/phase`。
- 系数只在首次 init 时算（`bessel_i0/sinf/sqrtf`），运行期只做乘加；
  结构 `voice_resampler_t` 约 400B，放静态区即可，不必 PSRAM。

### 播放任务的关键阈值（来自 demo，已实测有效）

- 预缓冲 8000 样本（500ms）才启动；低于 1600（100ms）暂停；回到 4800（300ms）恢复。
  旧值 200ms 预缓冲在突发网络下频繁欠载，每次欠载都是一次可听的断裂。
- 启动淡入 800 样本（50ms），续播淡入 160（10ms），停顿/结束前 `play_declick()`
  用 160 样本线性降到 0。I2S `auto_clear=true` 在 DMA 空时输出零，没有淡出就是阶跃→爆音。
- 取消/打断不要立刻 `ring_reset()`：先 `play_declick()` 再把 ring 清空，否则扬声器在
  任意波形幅度上被硬切，产生 pop。
- ring 静止超过 700ms 且未 drain：说明服务器不再发音频，残留尾包永远等不到 300ms 恢复阈值；
  此时 `flushing=true`，把剩余数据按尾块淡出后播完，结束本轮。

### codec 写超时是“整段卡住”的主因

- `esp_codec_dev_write()` 内部等待 I2S DMA 的默认超时是 1000ms；旧代码外层再套
  20 次 × 5ms 重试，最坏单块阻塞可达数秒，播放任务完全停摆，表现为“直接卡住不播”。
- 现在每轮循环只写一个 ≤640 样本块，不加外层重试；写失败/超时直接进入下一轮，
  由 `FEED_GAP_US`（170ms，12×240 帧 DMA ≈180ms 减 10ms）检测断流：
  距上次成功写超过该值就先播 160 样本淡入，避免从静音直接跳到大波形。
- 5s 打一条 `play loop #N: ring=… peak=… nearclip=…`；`peak` 接近 32767 且
  `nearclip` 增长说明 TTS 增益过高，需在服务端降音量或加限幅，而不是继续加淡入。

### CST816S 日志洪泛会拖慢音频

- 触摸未接/接触不良时 `esp_lcd_touch_read_data()` 每 30ms 一条 I2C NACK，
  115200 串口承受不住，日志任务占 CPU 后 TTS 播放出现卡顿。
- 做法：探测阶段失败直接不创建 indev；运行期累计 10 次失败先 `esp_log_level_set()`
  静音 `CST816S`/`lcd_panel.io.i2c`/`i2c.master`，累计 100 次禁用触摸并恢复共享日志级别；
  连续 500 次成功后再解除。注意恢复的级别是“共享总线”的默认值，不能在禁用时改成 ERROR
  后不还原，否则其它 I2C 设备错误也会被吞掉。

### CST816S 自动休眠会让触摸"失效"（2026-11-10 实测）

- 现象：初始化能读到 `IC id: 181`，约 0.5s 后坐标寄存器读取开始持续 NACK，
  100 次后代码禁用触摸；此后任何触摸都不能唤醒/点击。I2C 扫描仍能看到 0x15 —
  **地址 ACK 不代表芯片可用**，扫描结果会误导。
- 根因：CST816S 空闲后自动进入低功耗，休眠状态下 I2C 坐标读取 NACK/超时；
  寄存器 `0xFE`（DisAutoSleep）默认 0=允许自动休眠，写非 0 值关闭。
- 修复：初始化创建 touch handle 后写 `0xFE=0x01`；探测/读失败先硬件复位（TCA9555
  bit1 拉低 20ms 再拉高，等 100ms）再重试；不要永久禁用，保留 indev 供周期恢复。
- `touch diag` 实测：`id=ESP_OK(181)`、`data=ESP_OK`；关闭自动休眠 1s 后仍 OK；
  复位+关闭休眠 1s 后 `data=ESP_OK num=1`。随后长按屏幕成功触发 PTT。
- 注意：写 `0xFE` 本身如果芯片已经睡死也会失败，所以恢复流程必须"先复位、后写"。

### 断网不是配网页造成的（2026-11-10 实测）

- 把配网页改成“仅 SoftAP 生效时运行”后，串口仍出现开机关联失败：
  `reason=2`（auth expire）、`reason=205`（assoc fail），约 13.5–14.7s 才拿到 IP；
  断网期间还有 DNS 失败和 TLS `select() timeout`。此时 HTTP 服务根本没启动。
- 结论：射频问题来自 STA 关联/路由环境，配网 HTTP 服务本身不占带宽、不是掉线原因。
  按需开启的价值是省下常驻 socket/任务与 APSTA 共用射频，而不是“治好掉线”。

### AP 配置必须在 APSTA 模式下设置，且会触发 AP_STOP/AP_START

- STA-only 模式下直接 `esp_wifi_set_config(WIFI_IF_AP, ...)` 返回
  `ESP_ERR_WIFI_MODE`（日志 `softap on: ESP_ERR_WIFI_MODE`）——必须先把模式切到
  `WIFI_MODE_APSTA`，再写 AP 配置。
- 在 AP 已运行时重写配置，驱动会先 AP_STOP 再 AP_START（间隔几 ms），
  事件层若按边沿启停 HTTP 服务就会“停→启”抖动。做法：AP_STOP 用 300ms 防抖定时器，
  AP_START 时取消它；STA 恢复后的自动关闭用 5s 延迟，给在途 HTTP 响应留时间。

### esp_websocket_client 在 recv 期间持客户端锁

- v1.1.0 的 ws 任务在循环顶部取递归锁，然后持锁调用
  `esp_websocket_client_recv()` → `esp_transport_read(..., network_timeout_ms)`；
  发送路径（`send_text`/`send_binary`）用同一个锁，等待上限 500ms。
  因此只要有一个半包在读、且 `network_timeout_ms=5000`，上行会被饿死：
  `Could not lock ws-client within 500 timeout`，随后播放停顿/断流。
- 本地补丁：把 `esp_websocket_client.c` 的 recv 超时从配置值改成 500ms
  （`poll_read` 已确认有数据才读，500ms 足够收一个 TLS record）。文件内加了
  `LOCAL PATCH` 注释；厂商 demo 也是同样的改法。这属于 managed_components 本地修改，
  重新拉依赖会被覆盖，升级 IDF/组件后需复查。
- 注意：改完仍有长回复播放中断（`esp_transport_write() returned 0`、
  `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`），说明锁饥饿不是唯一原因，待继续定位。

### 串口一打开就复位，脚本必须等“拿到 IP”

- `/dev/ttyACM0` 是 USB-SERIAL-JTAG：即使设置 `dtr=False/rts=False`，打开端口本身
  就会复位芯片。自动化脚本不能打开后立刻发命令，也不能按固定秒数判断就绪。
- 稳妥做法：持续读串口，等待 `got IP <addr>`（超时 45–50s）后再发 CLI；
  开机首次关联可能先失败 reason=2/205，约 13–15s 才成功。

### 字幕固定 2 行窗口 + 自下而上滚动（LVGL 9）

- 字幕 pill 高度固定为 `2*20 + 16 = 56px`（行高 20、上下 padding 8），
  超出的 label 由父对象默认裁剪，不再把气泡撑高。
- 判断是否滚动要先 `lv_obj_update_layout(label)` 再 `lv_obj_get_height()`，
  否则刚 `lv_label_set_text()` 时拿到的是旧高度。
- 滚动用 `lv_anim_set_values(span/2, -span/2)`、`exec_cb` 改 `y`、
  `LV_ANIM_REPEAT_INFINITE`；新字幕到来时先 `lv_anim_delete()` 并把 y 归零，
  避免上一段动画继续把新文本拖出窗口。速度 12px/s，`span=h+40`。
- 实测日志：15B → `label=324x20 scroll=0`；167B → `label=324x100 scroll=1`。

### WiFi “假在线”数据通路卡死（2026-11-10 真机复现，已加自动恢复）

- 现象：WiFi 状态一直显示 `connected` 且有 IP，但所有出站流量失败。语音侧先从
  `Could not lock ws-client within 500 timeout` 开始，随后
  `esp_tls_conn_read error errno=No more processes`（EAGAIN）→ `websocket down`；
  重连每次约 5s 后报 `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`、errno=119（EINPROGRESS）。
  天气 HTTPS 同时失败。期间**没有** `WIFI_EVENT_STA_DISCONNECTED`，`wifi status`
  的 reason 不变。
- 主机侧交叉验证：设备自称在线时，主机 `ping` 100% 丢包、`ip neigh` 显示 FAILED；
  说明不是云端或应用层问题。内部堆在故障期间稳定在 ~44KB，也排除内存耗尽。
- 已排除配网页：故障发生时 HTTP 服务处于 `provisioning page idle`（未启动），
  因此“开网页导致掉线”不成立；按需开启只是省资源。
- 触发时机：多次出现在 TTS 开始播放后 1–2s；一次本地 880Hz 测试音未触发，
  说明与无线数据流（播放同时仍有上行/心跳）相关，而非喇叭或 codec。
- 社区同款：ESP-IDF issues #8123、#5717、#15704 与 esp32.com 的
  “WiFi stops working until esp_wifi_stop/start called” 均描述“有 IP 但 ARP/TCP 全断，
  只有 disconnect/connect 或 stop/start 能恢复”。属于驱动/射频 RX 路径卡死，
  应用层无法直接修复，只能检测并复位。
- 恢复阶梯（`panda_connectivity.c`）：语音 WS 断连事件与天气失败在 90s 内累计 3 次
  → worker 任务执行 `esp_wifi_disconnect()`，等 ≤1s 事件，再 `esp_wifi_connect()`，
  最多等 20s 拿 IP；仍无 IP 则 `esp_wifi_stop()` → 300ms → `esp_wifi_start()` →
  `WIFI_PS_NONE`；45s 冷却避免反复弹 WiFi（手动 `wifi recover` 可绕过冷却）。
- **所有 esp_wifi_* 调用必须在 worker 任务里执行**，esp_timer 回调只投递队列：
  #15704 报告了在 timer 回调里直接操作 WiFi 导致多线程/驱动挂死的风险。
  worker 栈 4096 使用 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`，
  否则内部 RAM 会从 ~44KB 掉到 ~40.6KB，天气 TLS 握手可能失败。
- 验证数据：手动 `wifi recover` 约 4s 恢复；自动路径实测 67s 出故障、95.5s 触发
  （3/3 次失败聚合）、99.5s 拿回 IP，随后一轮语音 `reply(75 bytes)` 与
  `play drain complete` 正常；完整回归中 4/4 次失败 → 193.9s 触发、199.3s 恢复。
- 仍未根除：恢复期间正在播放/等待的那轮回复会丢失；故障本身仍会偶发。
  后续可对照 demo 尝试 `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` 4→6 等射频缓冲差异
  （demo 用 6/静态 TX 16/cache 32；我们已启用 IRAM/RX_IRAM 优化），需单独压测评估。

### 对话中途断流：四个叠加根因（2026-11-10 定位并修复）

- 复现：`voice clip`（内置“你是谁” PCM）在音量 30/15/0 都会概率性中断；主机 1Hz
  ping 记录到连续 3–11s 丢包；失败前 1–2s 出现大量 `Could not lock ws-client`。
  音量不是变量（0% 也复现），持续 12s 本地 880Hz 音 0/45 丢包，可排除喇叭/功放。
- 根因 A（主因，应用层）：`WEBSOCKET_EVENT_ERROR`（event 0）被当成断线。
  `esp_websocket_client_error()` 会为**每次 error 级日志**派发 ERROR 事件，
  包括一次 500ms 发送 poll 超时；旧 handler 与 DISCONNECTED 同分支，清
  `s_ws_connected`、停上行静音泵。TCP 其实还活着（日志可见断开事件后 ASR 仍在
  到达），但服务端约 72s 后以 `AudioServerNoAudioInputTooLongError` 关会。
  修复：ERROR 只记 DEBUG，状态只由 DISCONNECTED/CLOSED 驱动。
- 根因 B（放大）：TCP 接收窗口 32KB（`LWIP_TCP_WND_DEFAULT/SND_BUF_DEFAULT`）
  远大于 demo 的 5760，重 TTS 下行时驱动缓冲被大窗口灌满；配合
  `RECVMBOX=16`、`MAX_SOCKETS=10` 与 WS `buffer_size=8192` 出现 RX 停滞。
  对齐 demo：`STATIC_RX 6`、`RX_BA_WIN 6`、`TCP WND/SND 5760`、`RECVMBOX 6`、
  `MAX_SOCKETS 16`、WS buffer 4096、`network_timeout_ms 10000`。
  修改后 `Could not lock` 完全消失（12/12 轮为 0）。
- 根因 C（误恢复）：demo 缓冲多吃 ~12KB 内部 RAM，内部堆从 ~31KB 降到 ~19KB；
  天气 `WEATHER_MIN_INTERNAL=28KB` 永远不满足 → 每次 fetch 直接 `NO_MEM`，
  却被 `err != ESP_OK` 当成网络故障。90s 内 3 次 → 触发 WiFi `disconnect+connect`，
  主动杀掉进行中的会话。修复：门槛 16KB；只有 `ESP_FAIL` 才上报网络错误。
- 根因 D（二次伤害）：组件在发送 `wlen==0 && errno==0`（poll 超时，非错误）时也
  `abort_connection`；50ms 静音泵下，一次 500ms 抖动就会毁掉整条会话。
  本地补丁：超时只返回失败、丢该帧，不 abort。另：clip 上行单帧入队失败原本
  立即 commit（<0.3s 音频，ASR 无内容），改为重试、连续 200 帧才强制 commit。
- 验证：音量 30%，12/12 轮完整 ASR→TTS→`play drain complete`，0 WS 断开、
  0 丢包、0 `Could not lock`；静置 90s 0/177 丢包且会话保持；天气
  `Beijing 11°C 晴` 正常。内部堆稳定 ~18.7–19.1KB，TLS 峰值最低 ~10.5KB。
- 注意：`managed_components/espressif__esp_websocket_client` 的补丁会在依赖更新
  时被覆盖，升级组件后需重新检查（源码内已有 `LOCAL PATCH` 注释）。

### CLI 不支持非 ASCII 参数（linenoise 过滤高位字节）

- `weather set 北京` 实际执行了 `weather` 状态分支；带引号则保存空字符串；
  `settings set weather_city 北京` 打印用法。原因：esp_console/linenoise 丢弃
  所有 ≥0x80 的输入字节，无法从串口 CLI 输入中文。
- 变通：中文城市用配网页 `POST /save` 写入 NVS；CLI 用拼音/英文（Open-Meteo
  geocoding 支持），例如 `weather set Beijing`。当前 NVS 城市为 `Beijing`，
  实测 `Beijing 8°C 晴`。

### BOOT 长按 4s 进入配网（2026-11-10）

- 冲突排查：GPIO0 只定义给 `PANDA_BOOT_BUTTON`，未与 I2S/SD/RGB 复用；但它是
  strapping 脚，复位时按住会进 ROM 下载模式，因此只能在固件运行后检测长按。
  固件里 BOOT 原本是 PTT（600ms 起按住说话），与 4s 配网直接冲突；经确认改为
  BOOT 专用于配网，PTT 只保留触摸长按。
- 实现：`app_core_task()` 对 BOOT 使用 4s 阈值（`BOOT_PROVISION_HOLD_US`），
  其余键仍 600ms。到阈值调用 `boot_provision_toggle()`：AP 关闭时
  `panda_connectivity_ap(true)` 并启动配网页；AP 已开时再按 4s 关闭。
  短按只 toast “长按 4 秒进入配网”。屏幕休眠时按住 4s 会先唤醒再切配网。
- 验证：`idf.py build` 通过 `0x801640`；CLI `key boot` → `boot short: hold 4 s
  for provisioning` 且无 PTT；`key boot long` → `provisioning page enabled` +
  `provisioning ready`，`wifi status` 显示 `softap: on / provision: on`，
  主机 `curl http://<STA>/api/status` 返回 200；再 `key boot long` →
  `provisioning page disabled` + `softap: off`。
- 待补：物理按住 BOOT 4s 的时序验证（本次监听窗口内未检测到按压）。
- 注意：手动开启的 SoftAP 不会像自动 fallback 那样在 STA 恢复后自动关闭，
  用完需再长按 4s 或 `wifi ap off`。长按关闭与 `wifi ap off` 语义相同，会置
  `s_ap_user_off`，本次运行内不再自动 fallback；重启或再次开启后恢复。

## 11. 电池 ADC 硬件链路（2026-11-10 实查原理图 + 真机）

### 分压与跳线（官方 ESP32-S3-AUDIO-Board 1.1 原理图）

- 电池分压：`BAT_OUT` → R2 `200K ±1%` → `BAT_ADC` → R11 `100K ±1%` → GND，
  C10/C11 各 `100nF`；即 **Vbat = Vadc × 3**，不是早期假设的 82K/110K（×1.745）。
- `BAT_ADC` 不直接进 MCU：经 R53 / R78 两个 **0R 选择跳线**（默认 `0R NC`）。
  - R53：pin1=GPIO1，pin2=CAM_HREF，pin3=中间网络；
  - R78：pin1=BAT_ADC，pin2=中间网络，pin3=GPIO6。
  - 要测电池需桥接 **R53(1-3) + R78(1-2)**；代价是 GPIO1 不再作摄像头 HREF。
  - 原理图注释原文：“默认不启用电池电压测量，若需要使用请注意摄像头将不可使用”。
- `BAT_OUT` 是 USB/电池的选择后系统轨：插 USB 时约 5 V，拔掉才是电池电压。
  因此插着 USB 读电量会得到 ~5 V/100%，真实电池百分比只能在电池供电时看。
  反过来可用 `mv >= 4400` 判断“正在 USB 供电/充电”。

### 悬空判定（实测）

- 0R 未焊时 GPIO1 悬空：20 次快采漂移 588 mV，60 s 慢采从 4.4 V 漂到 4.0 V；
  临时改读 ADC1_CH5(GPIO6) 稳定在 9459 mV（×3），证明 GPIO6 也不是分压节点。
- 固件上电时用内拉低自检：拉低 20 ms 读一次、悬空再读一次；
  分压接通时 66.7K 戴维南只塌到 ~0.5 V，悬空则塌到 0 V，阈值 200 mV。
  实测 `pulldown=0 mV` → `adc=0`，`battery` 显示 `unknown`，不再误报低电。

## 12. 圆形屏 overlay 安全几何（2026-11-10 真机 + 仿真）

- 面板 360×360，圆心 (180,180)，可见半径 180；考虑边框取安全半径 **172**。
- 横向 pill（宽 W、高 H、距底 m）的两个下角在同半径：
  `r = sqrt((W/2)² + (m+H-180)²)`，要求 `r ≤ 172`。旧几何全部违规：
  toast 340@top28 → r≈228；字幕 340@bottom54 → r≈192；状态 300@bottom10 → r≈201，
  视觉上就是被圆弧切掉两端的黑条。
- 现行 overlay（`panda_ui.c:create_overlays`）：
  - 状态：200×~36，`bottom 44` → y≈280..316；
  - 字幕：200×48（2 行 40 + padding 4×2），`bottom 42` → **y=270..318**，
    恰好从下 1/4 线（360×3/4=270）开始，四角 r≈170.4；
  - toast：240×~36，`top 64` → y=64..100，四角 r≈172 内。
- 生成屏自带的 6 个底部 tip（offline/reconnecting/asr/llm/tts/low_battery）
  y 由 298 上移到 290（r≈167），否则和状态 pill 叠在一起并逼近圆弧。
- 状态 pill 与字幕 pill 互斥：字幕非空或当前屏有 tip 时不显示状态，
  否则两个 pill 会在同一底部区域互相压字。
- 仿真器（`tools/ui_sim/web`）要同步这些数值；浏览器会缓存 `app.js`，
  换主机名（`127.0.0.1` ↔ `localhost`）或加 version 参数可强制刷新，
  改完务必用 DOM 矩形核对，不要只看旧截图。

## 13. 配网可达性 / 静默回合（2026-11-10，代码完成待真机）

### 配网页不能只依赖 AP 边沿事件
- 旧逻辑在 WiFi 事件队列收到 `AP_START` 时才启动 httpd；边沿事件被合并、UI 忙或
  httpd 启动失败后，热点在但页面永久不可达。
- 现在 `app_core` 周期任务每秒核对 `panda_connectivity_is_ap()` 与
  `app_provisioning_is_running()`，不一致就重试/停止；失败重试间隔 5s。
- 诊断：日志 `SoftAP started: ... ip=192.168.4.1 ch=1` 与
  `provisioning ready: http://.../` 应成对出现；`wifi status` 有 `softap/provision/ap_auto`。

### SoftAP 信道必须固定 1（STA 断开时）
- APSTA 下 SoftAP 跟随 STA 信道；STA 未连接时若沿用上次关联的 12/13，
  部分手机（尤其把 12/13 视为禁用）会连不上或连上无门户。
- 现为 `ap.channel = (s_connected && s_sta_channel) ? s_sta_channel : 1`。

### 手机门户探测需要更长的 HTTP 超时
- 默认 recv/send 5s 对 Android 的 captive portal 探测偏短，改为 10s；
  `max_open_sockets=4`、`backlog_conn=4`，避免探测连接占满后页面打不开。

### 静音回合不要 commit 上行音频
- PTT 释放时若持续语音帧 < 4（RMS 250/40ms 帧），旧代码仍 `commit`，服务端对
  纯静音回 ASR 失败或“服务器开小差，请稍后再试”，用户看到错误屏。
- 现直接本地收尾：字幕提示 + 不 commit；8s 静默宽限内对兜底文本/TTS/error
  一律取消并抑制。说话阈值 250/4 帧用于滤掉风扇与手持噪声。
- 服务端合成语音提醒走 demo 的 `speech_text_buffer.replacement.append` +
  `replacement.commit`（无音频上传的文本回合）；3s 冷却避免连续误触刷屏，
  10s 超时允许重试；取消旧响应后 400ms 内的 `response.done/canceled` 不算
  提醒回合结束，避免误清 `s_prompt_turn`。
- 残余风险：若静音被误判为语音且服务端兜底 TTS 已开始，取消前可能漏出极短音频，
  但不会再显示错误屏。

### 空字符串 chunk 会提前结束配网页（2026-11-11 真机）
- `httpd_resp_send_chunk(req, "", HTTPD_RESP_USE_STRLEN)` 的长度为 0，按协议就是
  “最后一个块”，响应立即结束。
- 页面里的可选值（未填 App ID、STA 未连接时的 IP）会传入空串，于是 HTML 在
  `<input ... value="` 处被截断；手机看起来就是“页面打不开”。
- 现在 `send_chunk()` 对空串直接返回 ESP_OK。真机整页 4094B，以 `</html>` 结尾。

### httpd 任务栈与监听 socket 泄漏（2026-11-11 真机）
- 实测 AP 开启后内部堆总量约 10–22KB，但最大连续块只有 3.5–7.7KB。
  8KB 栈的 httpd 任务创建失败（`ESP_ERR_HTTPD_TASK`）；IDF 的 `httpd_start`
  在该路径只释放结构体，**不 close 监听 fd**，之后重试 bind 成功但
  `listen` 返回 112（新 libc 的 `EADDRINUSE`），配网页永久起不来。
- 处理：httpd 栈降到 4KB（默认值；大缓冲都已在 PSRAM 堆），
  `index_get` 的 3KB 转义缓冲改为 PSRAM 分配；启动前要求
  `largest_internal >= stack_size + 2KB`，不满足就延后，避免进入泄漏路径；
  自愈重试 2s。`heap` CLI 增加 `largest_internal` 便于诊断。
- 代价：配网页运行时占用约 10KB 内部 RAM，天气刷新会因内部堆门槛跳过（可接受）。

### 语音 WS 403 = 账号欠费（2026-11-11 真机 + 主机复现，已解除）
- `esp-x509-crt-bundle: Certificate validated` 后立即
  `Sec-WebSocket-Accept not found` + `handshake_status_code=403`，每 5s 重试均失败。
- WiFi 有 IP、DNS/SNTP 正常，代码与 2026-11-10 12/12 通过版本一致；JSON 模式
  只用 `X-Api-Key`（与厂商 demo 一致），不是固件改动导致。
- 用同一 Key 在主机侧直接对 `openspeech.bytedance.com:443` 发 WS 握手，复现同样
  403，响应体明确：
  `{"error":{"type":"Forbidden","code":"45000030","message":"[resource_id=volc.speech.dialog] requested resource not granted"}}`。
- 额外测试：加 `X-Api-Resource-Id: volc.speech.dialog`、再加固定
  `X-Api-App-Key: PlgvMymc7f3tQnJ6`，仍是同一 403 → 与请求头无关。
- 结论：账号欠费时服务端把“资源未授权（45000030）”作为拒绝理由，容易误判为
  Key 失效/应用未开通。充值后同一 Key 主机握手立即 `101 Switching Protocols`，
  真机 `websocket connected`，无需重录或换 Key。排查顺序应是：先看账单/欠费，
  再看 Key 所属应用与资源授权。
- 该 Key 曾出现在聊天中，恢复后仍建议轮换。

### JSON 文本回合必须带 speech_id（2026-11-11 真机）
- 无语音提示原用 `speech_text_buffer.replacement.append/commit`，只带 event_id/text，
  服务端不回任何响应；10–20s 后 `panda_state` 的 `THINKING_MS=20000` 超时把
  `PANDA_STATE_ERROR_LLM` 画成“服务开小差了”错误屏。
- demo 的独立文本回合是 `speech_text_buffer.append` + `speech_text_buffer.commit`，
  两条都带同一个 `speech_id`（commit 的 `text` 为空）。改后 `tts started
  voice=chat_tts_text` 在 1–2s 内到达，`play drain complete` 后回 idle。
- 另加看门狗：`s_prompt_turn` 超过 10s 无响应直接回 idle，避免状态机兜底错误屏。

### 误触判定要防按压爆音（2026-11-11 真机）
- 150ms 轻触实测 `peak=9552 rms=2952 speech_frames=6`（RMS 250/4 帧判定成说话），
  说明按压/触摸本身会给麦克风一个瞬态，不能只靠整体 RMS。
- 处理：PTT 按下后 120ms 内的帧不统计也不上行；判定帧数 4→8（320ms）。
  同一轻触变为 `peak=90 rms=28 speech_frames=0`，稳定进入无语音提示分支。
- 代价：PTT 开头约 120ms 被裁掉，正常说话仍可识别；短于 ~440ms 的有效说话会被
  当作误触并得到语音提醒。



## 14. SD 卡音乐上传：栈、扇区对齐与缓冲区（2026-11-13 真机）

### httpd 任务栈 4KB 会在 1MB 左右溢出复位
- `POST /upload/music` 传大文件时出现 `***ERROR*** A stack overflow in task httpd`，
  `rst:0xc (RTC_SW_CPU_RST)`，客户端 `curl (56) Recv failure`，SD 上留下 0 字节文件。
- 打开文件与目录项在 `open()` 时已落盘，崩溃时 `close()` 未执行，所以目录里能看到
  文件名但 size=0；不能把这种文件当成“已上传”。
- `app_provisioning_start()` 的 `config.stack_size` 4KB → 8KB 后，11.2MB 文件稳定通过。
  内部堆已从 ~10KB 恢复到 ~118KB（见 §15），8KB 栈 + 2KB 余量检查可通过。

### FatFs 只有在“文件偏移扇区对齐且整扇区写”时才走多块 DMA
- 现象：11MB 文件上传前 4MB 约 330KB/s，之后掉到 8.7KB/s；串口计时显示
  `recv 194ms / write 117550ms`，单次 `write(8KB)` 最长 1106ms。
- 原因：TCP 窗口 5760 → `httpd_req_recv()` 每次约 5.7KB；第一次写后
  `fp->fptr % 512 != 0`，此后 FatFs 每个 512B 扇区单独 `disk_write(..., 1)`，
  SA02G（SDSC，1-bit/20MHz）单块约 58ms，16 块≈0.9–1.1s。
- 修复：上传循环先填满 16KB（`heap_caps_aligned_alloc(4096, 16384, MALLOC_CAP_DMA)`）
  再 `write()`，只在最后一次写尾块。文件偏移始终是 512 的倍数，FatFs 走多块 DMA。
- 实测（idle）：
  - 8.2MB → 22.1s（372KB/s），`{"ok":true,"bytes":8219359}`
  - 11.2MB → 31.3s（353KB/s），`max write 302ms`
  - 42 首批量约 330KB/s/首，10MB 约 30s。
- 上传日志每 MiB 输出 `@NNN KB/s (recv X ms, write Y ms, max A/B ms)`，可直接区分
  网络慢还是卡慢。

### 缓冲区与名称
- PSRAM 缓冲会被 SDMMC 拒绝（DMA 不能寻址 PSRAM），退化为单扇区 ~8KB/s；
  日志打 `no internal DMA buffer, upload will be slow`。必须用 `MALLOC_CAP_DMA`。
- `httpd_query_key_value()` 返回未解码的百分号串，中文名三字节/字，必须
  `url_decode()`（`%XX` 与 `+`）；网页用 `encodeURIComponent()`，
  主机脚本用 `urllib.parse.quote(name, safe='')`（保留别的 `+` 会被解码成空格）。
- 文件名白名单只保留 basename，过滤 `\ ? & % * : " < > |` 与控制字符，仅接受 `.mp3`。
- `sdcard ls` 现在输出 `FILE <size> <name>`，可直接核对上传字节数；内部 `stat`
  路径缓冲需 ≥512B，否则长中文名被截断、size 显示 -1。

### TCP 窗口不是当前瓶颈
- `CONFIG_LWIP_TCP_WND_DEFAULT=5760`，11.2MB 上传中 recv 累计 16.1s、write 12.1s，
  两者接近；若后续要更快，可把 TCP 窗口/发送缓冲提到 16–32KB 再测。

## 15. 内部 RAM 恢复：LVGL 池与 MP3 表移到 PSRAM（2026-11-13 真机）

- 症状：TLS `esp-aes: Failed to allocate memory`、天气因“内部堆 <28KB”跳过、
  音乐扫描返回 0 首（FATFS/LFN 分配失败）。实测启动后内部堆仅 ~10KB、最大块 4.8KB。
- 根因：LVGL 9.5 内置分配器在 `.bss` 里放 96KB 静态池（`LV_MEM_ADR=0` 的
  `work_mem_int`），MP3 组件还有 2×64×96B 曲目表（12.5KB）也在 `.bss`。
- 修复：
  - `LV_MEM_POOL_INCLUDE` + `LV_MEM_POOL_ALLOC` 把 LVGL 池改从 PSRAM 堆分配，
    实现 `panda_lvgl_pool_alloc()` 放在 `panda_ui`，头文件 `lvgl_mem_pool.h`。
  - `panda_mp3` 的曲目表改为 PSRAM 数组（首次使用时分配）。
- 效果：启动 `internal=118315 min_internal=105555`；上传时 `internal≈94–103KB`；
  TLS 正常握手，`websocket connected`，天气正常。

### CMake / 链接坑位
- 托管组件名是 `lvgl__lvgl`，不是 `lvgl`；
  `idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)`。
- `target_compile_definitions()` 会**静默丢弃**函数式宏（`NAME(x)=...`）。
  必须传普通宏 `LV_MEM_POOL_ALLOC=panda_lvgl_pool_alloc`，再让 LVGL 通过
  `LV_MEM_POOL_INCLUDE` 包含声明该函数的头文件。
- `panda_ui`/`main` 的静态库先于 lvgl 扫描，函数会被丢掉；在 lvgl 目标上加
  `INTERFACE "-u" "panda_lvgl_pool_alloc"` 强制保留。

## 16. MP3 卡顿/爆音：双任务 + 环形缓冲 + 单次采样率切换（2026-11-13 真机）

### 先排除共享通路
- 豆包 TTS 同一 I2S/codec/DMA 通路无卡顿，说明硬件与 `panda_audio_play()` 本身没问题，
  缺陷只在 MP3 播放实现。对照 AIWearable（成熟参考）与 AIWatch_Ver3.0（踩坑记录）。

### 旧实现的三处根因
1. **解码与 I2S 写耦合**：解码任务直接调用 `panda_audio_play()`（持 `s_tx_lock` 阻塞），
   SD 卡单次读抖动（几十 ms）会立刻表现为 DMA 饿死 → 断音/爆点。
   I2S DMA 深度 12×240≈2880 帧，48k 下仅约 60ms，没有缓冲余量。
2. **每首开头重配 codec**：文件采样率与当前时钟不同就 `set_rate()`，会关闭/重开
   ES8311/ES7210 与 I2S，必然产生间隙和 pop。AIWatch 的结论是播放中不要重配，
   宁可保持 48k 重采样；本项目曲库只有 44.1k/48k，改为**只在每首开始、写入端
   空闲且静音时切换一次**，播放中不再动时钟。
3. **无预缓冲、无淡入淡出**：起播/欠载处是 PCM 阶跃，直接听成“爆破音”。

### 新结构
- 解码任务 prio 8 只做：读文件 → MP3 解码 → 立体声下混 → 推入 32K 样本环形缓冲
  （PSRAM，互斥锁）。写入任务 prio 9 只做：从环取 1024 样本 → `panda_audio_play()`。
- 预缓冲 16K 样本（≈0.34s@48k、0.37s@44.1k）后才开始写；整环 32K 样本
  （0.67–0.74s），可吸收 SD 卡的长尾延迟。
- 淡入 441 样本（10ms）用于起播/续播/欠载恢复；曲尾最后一块做 10ms 淡出；
  写完后等 90ms（> 65ms DMA 深度）再宣告 idle，保证尾音真正播出。
- 切歌：解码端置 `s_abort`，写入端淡出丢弃环内容并排空，解码端等 `s_tx_idle`
  后重开文件/解码器。**握手顺序必须是先 `s_abort=false` 再 `s_tx_idle=true`**，
  否则解码端可能看到上一轮的 abort，误判为停止。
- 欠载时 `s_underruns++` 并强制下一次写入淡入，避免连续爆点。

### 任务看门狗坑（本次真机复现）
- 现象：播放到文件末尾后 `task_wdt: Task watchdog got triggered ... IDLE1`，
  backtrace 在 `ring_pop`/`mp3_tx_task`；`mp3 status` 显示 `buffered=0`、曲目不再前进。
- 原因：`s_eof && s_tx_idle` 的“环空且已排空”分支没有延时，prio 9 任务在 core1 忙等，
  既饿死 IDLE1，也让 prio 8 解码任务拿不到 CPU（无法切下一首）。
- 规则：任何“等待条件”的分支都必须 `vTaskDelay`（这里 5ms）；优先级只比解码高
  一级，避免写入端把解码端完全压死。

### 观测方法
- `mp3 status` 输出：`audio_rate`、`underruns`、`buffered`（样本数）、
  `flags`（bit0 primed / bit1 eof / bit2 idle / bit3 abort）。
- 稳定播放应为 `buffered=32768`（环满）、`underruns=0`、`flags=0x1`；
  曲尾短暂 `0x5`；停止后 `flags=0x4`。`underruns` 持续增长说明 SD 读取或解码跟不上。
- `panda_audio_set_rate()` 期间的 `i2s_channel_disable: the channel has not been
  enabled yet` 是 esp_codec_dev 关闭数据面的既有噪声，不影响播放。
- 实测：44.1k 与 48k 曲目均 `underruns=0`；用户听感确认无卡顿/爆音。

## 17. 播放中音量键失效：prio 9 写入端饿死 app_core（2026-11-13 真机）

### 现象与证据
- MP3 播放中按 KEY1/KEY2 没反应；暂停后立刻能调。
- 修复前日志：`app: button 1 down` → `app: key volume=30% (audio 5799 ms ESP_OK, ui 0 ms)`
  并伴随 `app: loop stall 5803 ms`。按键确实被处理了，但卡在 codec 音量写入 5.8s。

### 根因
- `panda_audio_set_volume()` 用 `portMAX_DELAY` 等 I2S 写入任务（prio 9, core1）
  几乎持续持有的 `s_tx_lock`。
- 持锁任务优先级高于 `app_core`（prio 5）。FreeRTOS 的 `xSemaphoreGive()` 只在
  等待者优先级更高时才让出 CPU；这里等待者更低，写入端下一轮 `xSemaphoreTake()`
  又先拿回刚释放的锁 → 等待者被无限饿死，直到播放暂停/切歌出现空档。
- 同一坑的另一种表现：播放中连按 `key 3` 翻页到第 5 页会卡住（持显示锁的路径里
  去写音量）。

### 规则
- codec 寄存器访问（音量/静音/回读）与 I2S 数据通路分开加锁；控制锁 200ms 超时，
  超时直接返回 `ESP_ERR_TIMEOUT`（切采样率会重开 codec 并重新套用 `s_volume`，
  丢一次写没有副作用）。
- 全局锁序固定 `s_tx_lock → s_rx_lock → s_ctl_lock`；音量只取 `s_ctl_lock`，不成环。
- 低优先级任务不要用 `portMAX_DELAY` 去等高频高优先级任务持有的锁。

### 按键响应
- 音量键按下即触发，按住每 250ms 重复；松手不再补一次（`s_btn_vol_fired` 抑制）。
- 诊断入口：`status` 的 `app: loops=… max_gap=…ms age=…ms`；停顿时打印
  `loop stall N ms in <stage>`（keys/events/music/voice-ui/1hz/menu-sync/battery），
  可直接定位是哪一段卡住。

## 18. 界面中文字号不一致与音乐页图标（2026-11-13）

### “音乐播放”四个字大小不一
- `ui_font_Title22` 是 SquareLine 按 `--symbols` 清单裁出来的 22px 子集：含 音/播/放，
  **不含 乐**；该字体 `.fallback = &ui_font_CJK16`，缺字回退到 16px，
  于是三个字 22px、一个字 16px。
- 规则：往界面加中文文案前先确认所用字体子集包含全部字符；否则补生成字体，
  或直接用 `ui_font_CJK16` 这类全字库字体。

### 大图标用 Montserrat 符号字体
- `LV_SYMBOL_AUDIO`(U+F001) 在 `lv_font_montserrat_28/48` 的 sparse cmap 里
  （`unicode_list` 存的是相对 `range_start=176` 的偏移：0xef51+176=0xF001）。
- 需要 48px 时在 `sdkconfig.defaults` 加 `CONFIG_LV_FONT_MONTSERRAT_48=y`（+96KB），
  **不要手改 `sdkconfig`**。已有本机 sdkconfig 时先 `idf.py save-defconfig`
  把覆盖项存到临时文件，再用
  `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;/tmp/local.defaults" reconfigure`
  重建，并 diff 确认只多目标一行。

## 19. RGB 灯环联动：亮度 gamma、VU 曲线与节能观测（2026-11-13 真机）

### gamma 会吃掉低幅动画
- `output_frame()` 先乘亮度再平方（`v*v/255`，gamma≈2）。默认亮度 45% 时，
  原呼吸幅度 `0.05–0.16` 的通道值经 gamma 后长期为 0：灯环看似全灭，
  `rgb status` 的 `refresh` 计数也长时间不涨（帧相同被跳过）。
- 规则：按“目标字节”反推幅度。要得到约 5–10 的通道字节，pre-gamma 值约
  `sqrt(byte*255)≈36–50`，即 45% 亮度下幅度约 0.3 起。现网待机/AP/充电/
  低电呼吸均已提到 0.22–0.65；VU 最低幅度 0.45。

### 音乐峰值远低于满刻度
- 实测多首 320kbps MP3，`panda_audio` 每块峰值换算 level 仅 2–19（-34…-14 dBFS）。
  线性映射到 7 颗灯几乎只亮 1 颗；`vu()` 里做 `sqrt(level/100)*100` 感知映射后，
  level 16 → 40 → 亮 3 颗，低音量段也有层次。
- `panda_audio_get_output_level()` 必须带 250ms 过期：暂停/停止后不再写 PCM，
  否则返回最后一块峰值，VU 会“冻结”在暂停瞬间（实测曾停在 5）。

### 节能与可观测性
- 刷新周期：播放或语音 40ms、待机 120ms、手动常亮 300ms、睡眠/关闭 1000ms；
  帧与上一帧相同则跳过 `led_strip_refresh()`。
- `rgb status` 的 `refresh` 是现成功耗探针：播放约 39 次/2s、待机约 10 次/2s、
  `ui sleep` 后 3s 增量为 0（全灭 + 停止 RMT）；`ui wake` 后恢复约 10 次/2s。
- `panda_led` 组件用到 `esp_timer_get_time()`，CMake 需
  `PRIV_REQUIRES esp_timer`（`panda_audio` 新增电平时间戳同理）。
- 串口 `ui sleep|wake` 只调 UI 会与灯环状态脱节；两处都要同步
  `panda_led_set_sleeping()`，唤醒再加 `panda_led_notify_wake()`。

## 20. 菜单切页不要前台胶囊（2026-11-14）

- KEY3 短按原本在 `panda_ui_menu_next()` 后调用 `panda_ui_toast(page, 800)`，
  胶囊压在生成页面上方，和页面自身的标题/内容重复。改为只用 `ESP_LOGI`
  打印 `menu page: <名称>`，屏幕直接呈现目标页；长按“返回主屏”提示保留，
  因为它是离开菜单、没有目标页可看的动作。
- 新增页面时无需再维护 toast 文案；串口日志仍可用于自动验收切页顺序。

## 21. KEY3 切页卡死：不能把 SD/音频阻塞放进 app_core（2026-11-14）

- 现象：连按 `key 3` 翻到第 7 页（音乐播放）时界面“卡死”数秒，期间其他按键
  无响应；旧日志已有 `loop stall ... in music` 约 2s。
- 根因不在 LVGL：所有菜单页在 `ui_init()` 已创建，`lv_screen_load_anim` 重叠调用
  由 LVGL 9.5 处理，`auto_del=false` 也不会删除页面。真正问题在 `app_core`
  主循环——它同时采样 KEY1/2/3 与触摸，却同步调用：
  - 进入音乐页：`panda_mp3_scan()`（每个文件打开两次读 ID3，46 首约数百 ms）
    + `panda_mp3_play()`（建解码/写任务）；
  - 离开音乐页：`panda_mp3_stop()`（写端 abort 等 800ms，解码任务退出等 3s）。
  主循环停摆即 UI 卡死，停顿中的短按还会丢失。
- 规则：`app_core` 只做事件泵，任何可能阻塞数百 ms 以上的 SD/解码/网络操作都放到
  独立任务。现由 `music_ctrl`（prio 3、300ms 轮询）负责音乐页副作用。
- 顺带修的隐患：`s_done` 是二值信号量，超时停止会留下残留 token，让下一次
  `stop()` 立刻“成功”而 worker 仍在跑；现在停止后清 token，且 `play()` 发现旧
  worker 未退出时返回 `ESP_ERR_TIMEOUT`，避免两个 worker 共用 `s_stage`/环形缓冲。
- 扫描本身也被配网页上传回调调用，`s_files/s_titles` 是全局表，必须用互斥锁串行化；
  ID3 标题在排序后只读一遍（原先读两遍），扫描耗时约减半。
- 观测：`menu page: <名称> (N ms)`、`music page entered (... scan N ms)`、
  `mp3 play: ... (N ms)`、`music page left, playback stopped (N ms)`；
  `status` 的 `max_gap` 若再次 >200ms，看 `loop stall` 的 stage 名定位。

### 菜单页即时切换与页索引（同日真机复核后补）
- `lv_screen_load_anim()` 的 `act_scr` 要到动画 start 回调才更新。若两次 KEY3
  间隔小于一个 LVGL tick，第二次按 `lv_screen_active()` 判断会看到旧页，重复选同一
  下一页（日志同一页名出现两次、实际少翻一页）。菜单内改为维护 `s_menu_index`，
  仅在当前屏不是“索引所指的菜单页”时（回主屏/被其他页面接管）重新用屏幕反查。
- 菜单页原先走 150/250ms 淡入。极速连按时多轮淡入重叠，显示锁被渲染长期占用，
  按键处理曾排到 100–440ms；更极端时出现过两次整机静默（串口 0 字节、无 panic、
  无新 coredump、重新复位才恢复），推测与屏幕动画/QSPI 刷新路径卡住有关，
  但未抓到直接证据。菜单页现在用 `lv_screen_load()` 即时切换：处理耗时降到
  0–22ms，70 次 120–450ms 连按全部按序、`max_gap` 保持启动基线。
- 状态机页面（唤醒/聆听/思考/说话/错误/睡眠）仍保留淡入动画，不受影响。
- 若再次遇到整机静默：先确认是否复位（`rst:` 原因、`status` 的 uptime），
  保留原始串口日志；内置 USB-JTAG 的 OpenOCD 需要 udev 权限，当前用户
  `libusb_open()` 报 `LIBUSB_ERROR_ACCESS`，调试前需补 udev 规则或提权。

## 22. 真深睡：哪些东西能留下来（2026-11-14）

- 深睡只保留 RTC 域；数字 GPIO、RMT、I2C、USB-JTAG 全断电。最直观的验证是
  `/dev/ttyACM0` 消失——UI 睡眠不会让端口消失。
- **唤醒源**：KEY1–3 与 TP_INT 都接在 TCA9555(P1.1/1.2/1.3、P0.2)，深睡时 I2C
  不可用；板上也没有把 TCA9555 的 INT 引到 ESP32。可直接唤醒的只有 BOOT
  (GPIO0, RTC GPIO)。触摸、语音唤醒在深睡下都不可用。
- **呼吸灯**：WS2812 数据线是 GPIO38，不属于 RTC GPIO，深睡时无法翻转；
  要“呼吸”只能浅睡轮询（约 1–3mA）或改硬件。当前方案是睡前锁存一颗极暗像素
  `(3,8,4)`，再用 `gpio_hold_en(38)` + `gpio_deep_sleep_hold_en()` 把数据线
  压低；灯保持颜色与否取决于灯带的 5V/3.3V 轨在深睡时是否仍供电。
- **背光**：只把 LEDC 占空比设 0 不够——深睡后 GPIO5 悬空可能把背光拉亮。
  必须先 `gpio_reset_pin` 脱离 LEDC，再输出低并 hold。
- **唤醒后的 hold**：深睡唤醒是复位，hold 位会保留；`rgb_init()` /
  `display_init()` 必须 `gpio_hold_dis()`，否则灯带数据或背光被永久钉住。
- **LED 任务竞争**：睡眠态动画任务仍每 1s 写一帧黑屏，会覆盖睡前暗灯。
  `panda_led_prepare_deep_sleep()` 先置 `s_frozen`，再用同一把输出互斥锁等待
  在途帧结束后写指示灯，避免 1s 后又被清掉。
- **BOOT 的 strapping 风险**：GPIO0 低电平是下载模式 strapping。深睡 `ext0`
  唤醒也是复位，若按住跨过 ROM 采样点可能直接进下载模式（厂商豆包 demo
  同样用 ext0 GPIO0，未给出规避）。所以睡前要等 BOOT 松开再 arm 唤醒；
  唤醒动作建议“轻按一下”。2026-11-14 实测轻按 BOOT 正常唤醒
  （`woke from deep sleep (cause=2, BOOT)`，完整冷启动，未进下载模式）；
  按住跨过复位采样的边界未测，若复现：浅睡轮询、或把按键飞线到
  GPIO2/17/18/21 等空闲 RTC GPIO 后改用 ext1。
- **电流量级**：模块深睡约 10–20µA；一颗暗灯约 1–2mA（灯带静态电流 + 低占空
  比），所以“留灯”会把待机电流抬高两个数量级。若要极低功耗，应把指示灯也关掉。
- 深睡唤醒是冷启动（WiFi、语音、UI 全量初始化，约 1–2s），不是恢复原会话。

## 23. SquareLine 手势与固件菜单轮转冲突（2026-11-14）

- 生成的 37 屏里，菜单页各自绑了 `LV_EVENT_GESTURE` 固定跳转，例如
  时间天气右滑回主屏、系统信息右滑回系统设置、快捷设置上滑回主屏；
  这些顺序与固件 KEY3 的七页轮转（快捷→人格→睡眠→系统→设备信息→时间天气→音乐）
  不一致，直接叠加会出现“左滑下一页、右滑又跳别处”的混乱。
- 做法：`panda_ui_init()` 用 `lv_obj_remove_event_cb(scr, ui_event_xxx)` 摘掉
  这六个生成回调（只含手势跳转，删除不影响按钮），再给七个菜单页统一挂
  `menu_gesture_cb`：左滑 `menu_step(+1)`、右滑 `menu_step(-1)`、上滑回主屏。
- LVGL 9.5 的手势分发不是“发给按下对象再逐级冒泡”，而是
  `indev_gesture()` 从按下对象一路向上，只要当前对象带
  `LV_OBJ_FLAG_GESTURE_BUBBLE` 就继续爬，最终把 `LV_EVENT_GESTURE` 发给
  **第一个不带该 flag 的对象**（通常是屏幕）。注意 `lv_obj_create()` 默认给
  所有有父节点的对象加这个 flag，只有屏幕（parent=NULL）默认没有。
- 因此给屏幕对象本身加 `GESTURE_BUBBLE` 会让爬升越过屏幕到 NULL，事件直接丢失。
  之前的 `add_gesture_bubble()` 把 flag 也加在屏幕/自建音乐页根节点上，
  导致所有生成手势实际都收不到（本次已修：只给子对象补 flag，目标屏幕保持不带）。
  子对象默认已带 flag，自建音乐页的按钮等子控件继承正常，从按钮上起滑也能切页。
- 不要同时在 `app_core` 的 `handle_touch()` 里响应左右滑：`panda_touch` 自己
  也以 45px 阈值发 `PANDA_TOUCH_SWIPE_*`，两套判定会各切一次页。
- 回调运行在 LVGL 任务里，`panda_display_lock()` 是递归互斥，`menu_step()`
  在事件中加锁安全；先 `lv_indev_wait_release()` 再换屏，避免抬手触发原页按钮。
- 城市行放圆屏 y=144 时可视半宽约 `sqrt(180²-144²)=108px`。固定坐标对齐会被
  不同长度的地名带偏，改用透明 flex 行（300×30，居中，`pad_column=6`），把
  `ui_wx_icon`/`ui_wx_temp` reparent 进去，宽度改 `LV_SIZE_CONTENT`，地名变化时
  LVGL 自动重新居中。SquareLine 字体的生僻地名回退到 16px CJK16，与 18px 温度
  有轻微字号差。

## 24. PSRAM 栈任务不能读写 NVS（2026-11-14）

- 天气任务由 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` 创建，
  栈在 PSRAM。若在该任务里调用 `nvs_set_*`/`nvs_commit`，NVS 读 flash 时
  `spi_flash_disable_interrupts_caches_and_other_cpu()` 会关 cache，PSRAM 栈随即
  不可访问，触发 `assert failed: esp_task_stack_is_sane_cache_disabled()`
  并 coredump（实测天气取数成功后必崩）。
- 规则：**PSRAM 栈任务只做 RAM/网络/LVGL 操作，NVS 写入交给内部栈任务**
  （app_core、CLI、HTTP 任务）。本次中文地名缓存即由天气任务置
  `s_label_pending`，app_core 每秒 `app_weather_take_city_label()` 消费并保存。
- Open-Meteo geocoding：请求已带 `language=zh`，`results[0].name` 是本地化名
  （Changzhou→“常州市”）；但用中文“常州”直接查询该 API 返回空结果，所以配置里
  保留英文/拼音原文，另存中文显示名（NVS `wx_label`）供离线/重启使用。
- LVGL `lv_obj_set_parent()` 可把生成页的图标/文字挂到自建容器里，flex 会接管
  位置；`ui images` 的递归遍历仍能找到嵌套图片对象。

## 25. 预览/表情/动画页为何“没用”及隐藏方式（2026-11-14）

- 三页（角色预览 1/2、状态预览 3/3、表情管理、动画管理）来自 SquareLine 设计稿：
  表情管理写死“19 个表情 · 已就绪 18”，动画管理写死“24 帧 · 12 FPS”，
  行不可点，固件也没有序列帧动画管线（真实小动画只有思考三点、聆听/说话电平）。
- 点缩略图“闪一下”的根因：状态定时器 50ms 一次（`panda_ui.c` 的
  `lv_timer_create(state_timer_cb, 50, ...)`），`apply_view_locked()` 只在
  “当前页是用户页或状态紧急”时保留画面。预览页在 `screen_is_user_page()` 里，
  但点进去的表情/错误屏不在，所以下一 tick 立刻按真实状态拉回。
- 这些页还绕过统一状态机直接 `_ui_screen_change` 到表情/断网/错误屏，
  违反 PRD 6.2“禁止 UI 页面各自独立修改角色表情”。
- 处理：保留屏幕对象（`ui images` 仍校验 37 屏），在 `create_menu_controls()`
  用 `hide_widget()` 隐藏 `ui_link_prev/exp/anim` 三个入口。HIDDEN 父对象
  同时屏蔽子对象命中测试，无需逐个删标签。
- 后续若做“情绪驱动表情”（方案 C），需先把预览目标屏纳入用户页或在预览期间
  暂停状态机收敛，再恢复入口；单纯把入口显示回来仍会被 50ms 拉回。

## 26. 火山内联 TTS 情绪标签在实时端到端模型上不可用（2026-11-14）

- 文档里 TTS 2.0/1.0 双向流式支持内联标签
  `{{"audio_params":{"emotion":"happy","emotion_scale":5} }}`（注意是 3 个右括号），
  标签应放在待朗读文本前。实测实时端到端模型 1.2.6.1（`settings` 里
  `model=1.2.6.1`）**不把它当控制指令，而是当普通文本念出来**。
- A/B 证据（同一问题、同一音色）：无标签 `tts done ... audio=5154ms/5974ms`；
  标准三括号标签 `audio=10971ms/12550ms`，多出约 6s，正好是读出
  `audio_params emotion happy emotion_scale` 的长度。控制点：把标签从人格指令
  里去掉后时长立即回到 5–6s。
- `response.output_text.done` 能收到模型输出的标签；但设备实收报文常缺内层
  `}`（`{{"audio_params":{...}}` 只有两个右括号），`{body}` 包裹会解析失败，
  需回退为 `{body}}` 再解析。即使修正格式，朗读问题依旧，所以不要在这条路上
  继续投入，除非换模型/协议。
- `output_text.done` 在 TTS 开始后约 3s 才到；若要在“说话中”尽早切表情，
  需从 `response.output_text.delta` 的头部增量里解析并加只处理一次的标志。
- 表情屏替换说话屏会被 50ms 状态定时器拉回：`screen_is_urgent()` 不够，
  必须把 `s_view.voice == PANDA_STATE_VOICE_SPEAKING` 也算紧急，同时排除
  `OFFLINE`/`RECONNECTING`（这两个仍要让断网提示优先）。
- 语音回合中的表情不能被 `EMOTION_MS=2.5s` 清掉，`panda_state` 在
  `VOICE_THINKING/SPEAKING/CANCELLING` 收到 EVT_EMOTION 时将寿命改为
  `TURN_EMOTION_MS=60s`，回合结束（IDLE/LISTENING/PTT_DOWN）即清。

## 27. O2.0 二进制实时语音协议实测要点（2026-11-14）

- 端点 `wss://openspeech.bytedance.com/api/v3/realtime/dialogue`，头部需
  `X-Api-App-ID`、`X-Api-Access-Key`、`X-Api-Resource-Id: volc.speech.dialog`、
  `X-Api-App-Key`（厂商 demo 公开的固定常量，代码内做默认值，可被设置覆盖）、
  `X-Api-Connect-Id`（随机）。`X-Api-App-Key` 是公开常量，不是用户密钥。
- 帧：4 字节头 `(ver<<4|hdr_words) (msg<<4|flags) (serial<<4|comp) reserved`；
  之后依次是可选的 seq（flags 含 0x01/0x02 时，**在 event 之前**）、event（flags
  0x04）、服务端帧的 session-id（4B 长度 + 内容）、4B payload 长度、payload。
  注意 `SERVER_ERROR` 的字节布局是 `code(4) + len(4) + msg`，且它的消息类型
  在高 4 位（byte1 应为 `0xF0`，不是 `0x0F`）。
- 客户端事件：1 StartConnection（无 sid）、100 StartSession（带 sid + JSON 配置）、
  200 音频（`msg=0x02`、serial NONE、raw s16le 16k，带 sid）、102 FinishSession、
  501 ChatTextQuery（`{"content":"..."}`）。客户端帧不压缩（comp=0）也能被接受。
- 服务端事件：50 连接建立、150 会话建立、350 TTS 句首（`tts_type`）、352 TTS 音频
  （`pcm_s16le` 24k，serial 常为 NONE；处理时不要依赖 serial，352 直接按音频）、
  359 TTS 结束（`status_code`）、450 ASRInfo（打断信号）、451 ASRResponse
  （`results[0].text` 是全量转写）、459 ASREnded、550 ChatResponse（`content`）、
  559 ChatEnded、51/52/153/154 等。
- O2 没有 commit：断句完全由服务端 VAD 决定（`end_smooth_window_ms=1500`），
  客户端 `send_commit()` 在 O2 下属正常空操作。
- 打断语义不同：JSON 链路有 `response.cancel`；O2 只能 `FinishSession`（102），
  然后收到 152 再 `StartSession`（100）。重启会话期间的开头语音会丢，UI 应显示
  CONNECTING。
- WebSocket 分片：`esp_websocket_client` 的 `op_code` 0x2=二进制、0x1=文本、
  0x0=续帧。**续帧不带类型**，必须记住首帧类型；否则 O2 下长 JSON 事件（>4KB）
  的续帧会被送进二进制解析器。已在 `s_ws_binary_msg` 里跟踪。
- gzip 解包：去掉 gzip 10 字节头及 FEXTRA/FNAME/FCOMMENT/FHCRC 后，喂 raw deflate
  给 `tinfl_decompress_mem_to_mem(..., flags=0)`；zlib 流（`78 9c`）则带
  `TINFL_FLAG_PARSE_ZLIB_HEADER`。厂商 demo 的 gzip 分支在 raw deflate 上又加了
  zlib 标志，属于错误用法，别照抄。
- 构建坑：`ESP_ERR_INVALID_DATA` 在 ESP-IDF 5.5 不存在，坏帧用
  `ESP_ERR_INVALID_RESPONSE`。`panda_ai/build` 缓存里可能残留
  `/tmp/sdkconfig.local.defaults`（上次会话为保留本机配置临时生成、已随 /tmp 清理）；
  用 `idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults reconfigure` 重新生成，
  实测 `sdkconfig` 前后零 diff，无需手改。

## 28. 串口打开会复位开发板（2026-11-14）

- 本板走 USB-Serial-JTAG（`/dev/ttyACM0`）。用 pyserial `Serial(...)` 打开时，
  驱动默认拉高 DTR/RTS，即使紧接着置 False，EN 已被触发，设备会重启：
  实测每次打开都能读到 `ESP-ROM:esp32s3-20210327` 与完整启动日志。
- 自动化读写改用 termios 裸开，不碰调制解调器线：
  `os.open(port, O_RDWR|O_NOCTTY|O_NONBLOCK)` + `tty.setraw` +
  `tcsetattr` 设 B115200，之后 `os.read/os.write`。实测不再复位。
- `idf.py flash` 本身会复位一次（预期）；验证日志时先等启动完成再发命令。
- 增量构建后 `app_init: Compile time` 可能仍是旧时间戳（`esp_app_format` 未重编），
  判断是否烧了新固件要看镜像段大小/`ELF SHA256`，不能只看编译时间。

## 29. O2 真机联调：WS 栈溢出与 TTS 突发（2026-11-14）

- **症状**：配好凭证、`api_mode=o2` 重启后，日志刚出现
  `O2 connection started` 就打印
  `***ERROR*** A stack overflow in task websocket_task has been detected.`
  并 `RTC_SW_CPU_RST` 重启，随后每轮连接都复现。
- **原因**：`send_session_create_o2()` 在 WS 事件回调里执行（即
  `websocket_task`），函数内联了 `persona_plain[800] + persona[2200] +
  config[4096] ≈ 7KB` 栈缓冲；WS 任务栈只配了 8192，TLS 握手后余量不够。
  JSON 模式没暴露是因为 `session.create` 多在 PTT/app 任务里构建。
- **修复**：三块缓冲改为 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`，用完
  `free`。注意不要“一块大分配再切三段”传给 `snprintf`，GCC 的
  `-Werror=restrict` 会报参数与目标可能重叠；分成三次独立分配即可。
  修完保持 WS 任务 8192 栈不变。
- **TTS 突发**：O2 服务端按生成速度下发音频，不按实时。实测 258 字回复
  `tts done ... audio=43434ms`，约 10s 内到齐，把 256K 样本（16k≈16s）的
  环形缓冲打满，`ring full, dropped ...` 累计 293933 样本（约 18s）。
  O2 音频帧是 24k，重采样到 16k 后写入环形缓冲；播放端只能实时消费。
- **缓解**：`RING_SAMPLES` 提到 768K（1.5MB PSRAM，≈48s@16k）。PSRAM
  余量约 6.9MB，代价可接受；普通短回复 `ring_drops=0`。更长的回复仍会丢尾，
  根治需要给 WS 接收做背压（阻塞读取会牵连 ping/超时），暂不做。
- **会话生命周期**：O2 每连接一次 `StartConnection`，连接建立后立即
  `StartSession`；取消/打断 = `FinishSession`（服务端回 152）后按需重新
  `StartSession`。`voice cancel` 不会自动重建，下一次 PTT 或 `voice clip`
  才建；`voice clip` 现在会置 `s_clip_pending`，等 `O2 session started`
  自动开始发送，便于自动化验证。O2 下没有 commit 事件，服务端 VAD
  （`end_smooth_window_ms=1500`）决定断句。
- **模型版本**：O2 的 `dialog.extra.model` 固定为 `1.2.1.1`（与厂商 demo 一致），
  `settings set model` 只作用于 JSON 模式；改 O2 模型需改 `app_voice.c` 的
  `O2_MODEL` 并确认控制台已开通对应端到端版本。

## 30. 功放（PA）空闲噪声定位与门控（2026-11-14）

- **症状**：TTS/MP3 播放时有明显“电流滋滋”声；不播放时喇叭也有轻微持续滋滋。
- **分段实验**（串口临时加了 `audio pa on|off` 与 `audio mute on|off`，
  屏幕上用 `ui status` 标 1–5 段，音量 40，用户盲听）：
  1) PA 开 + DAC 开 → 有；2) PA 开 + DAC 静音 → 有；3) PA 关 + DAC 开 → **安静**；
  4) PA 开 + DAC 开 → 有；5) 播放 880Hz 音调 → 有。
  结论：噪声来自 PA 及其供电，不是 ES8311 DAC；DAC 静音也挡不住。
- **修复**（`panda_audio.c`）：功放改为“按需供电”：
  - `codecs_open()` 一律 PA 关 + DAC 静音；
  - `panda_audio_play()` 遇到 `!s_out_on && !s_muted` 时先开 PA → 等 20ms →
    解除 DAC 静音，再写第一块 PCM（只有首块付 20ms 延迟）；
  - `panda_audio_poll()`（`app_core` 20ms 循环调用）在最后写入 800ms 后
    先静音 DAC → 等 15ms → 关 PA；取 `s_tx_lock` 避免与写入/换采样率竞争；
  - `panda_audio_mute(true)`（勿扰）立即执行同样的关断；
  - `panda_audio_set_rate()` 在关闭 codec 前先关断，避免重配瞬间的咔哒。
  DMA `auto_clear` 使 800ms 后数据早已是静音，门控不会截断有效音频。
- **验证**：空闲 `audio status` → `out=off`（用户听感：完全安静）；
  `audio tone 3` 期间 `out=on`，结束后 `out=off mute_reg=0x60`；
  `voice clip` 全链路 `underruns=0 ring_drops=0`。构建 `0x8353d0`（18% free）。
- **播放中残余噪声**：用户反馈“和之前差不多”，与第 2 段一致 —— 只要 PA 通电，
  即使输入静音也有底噪，故播放时叠在语音上无法用软件消除；可尝试的硬件方向是
  PA 供电去耦/滤波、降低 PA 固定增益或换低噪声功放。软件侧仅能缩短通电时间。
- **诊断命令变更**：临时 `audio pa on|off` 已移除（PA 常开会被空闲轮询立即关闭，
  保留会误导）；`audio status` 用 `out=on/off` 反映真实门控状态。

## 31. 菜单切页“卡死”的显示锁看门狗（2026-11-14）

- **现象**：KEY3 切页时卡在“时间天气”。串口侧特征很明确：`ui menu next` 无返回、
  之后的 `status` 也不执行，但 `voice: uplink frames=... (silence)` 仍在打印
  → 串口 CLI 任务卡在 `ui_lock()`，即 LVGL 显示递归锁被某个任务长期占用。
  复位后连翻七页正常，说明是偶发竞态而非固定路径。
- **两可根因**（新日志用于区分）：
  1. 某任务持锁后阻塞在别的资源（如 LVGL 任务在回调里调用阻塞 API，或
     `lv_indev_read` 持锁做 I2C）；
  2. LVGL 任务自己卡在渲染/传输路径（例如 `esp_lcd_panel_io_tx_color` 的
     事务队列满时错误被忽略，`on_color_trans_done` 永不触发，flush 永远
     “在途”，屏幕静默冻结）。
- **已加诊断/恢复**：
  - `panda_display_lock_at(timeout, site)` 记录持锁任务名、`__func__` 位置、
    持锁时长；获取超时与单次持锁 >500ms 打日志；`panda_display_lock_info()`
    输出一行持有者信息。
  - `panda_ui` 的 `ui_lock()`：先等 3s，失败打印
    `display lock stuck 3s (caller=..., holder=..., site=..., held=..., lvgl ticks=N stopped Mms ago)`；
    再等 7s，仍失败则 `vTaskDelay(100)` + `esp_restart()`（自恢复）。
    注意：超时后绝不能“继续用锁”——递归信号量未持有却 `Give` 会破坏计数。
  - LVGL 心跳：50ms 状态定时器里累加计数；`ticks` 停走且 holder=lvgl-task
    说明 LVGL 任务死在回调/渲染里，否则看具体任务名。
  - `esp_lcd_st77916.c` 的 `tx_color` 失败打 ERROR（原来返回值被丢弃）。
- **压测结果**：`ui menu next` 连翻 100 次；next/prev/home 混合快速 300 次
  （40–90ms，含音乐页自动起播/离页停止）→ 全部按序，无 stuck 日志。
  观察到的正常等待上限约 260ms，来自 `panda_ui_is_music_page` 300ms 轮询
  与 LVGL 渲染抢锁；>500ms 才会打提示，不要把它误判成卡死。
- **复位原因**：`status` 新增 `reset : ...`。本板 USB-Serial-JTAG 的复位常报
  `unknown`；若现场出现 `panic`/`task_wdt`/`brownout`，优先查对应路径。
- **仍未坐实**：用户手指复现路径（KEY3 + 触摸手势）未抓到；下一步靠现场日志
  里的 holder/site/lvgl ticks 三要素定性。

## 32. 自动深睡与音乐播放（2026-11-14）

- 规则：`panda_mp3_state()==PANDA_MP3_PLAYING` 时**不算空闲**。1Hz 逻辑里刷新
  `s_last_active_us` 并跳过 `auto_sleep_s` 判定；否则播完一首（或刚好超过计时窗口）
  停歌瞬间就会立即进深睡。
- 预备期复检：自动休眠已排队时，到点执行前若正在播放则取消本次请求并清
  `s_deep_sleep_at_us`（日志 `auto deep sleep deferred: music playing`）。
  “菜单 → 立即休眠”是用户显式动作，不做该取消。
- 实测：`auto_sleep_s=60` + 播放 75s 不休眠；排队后 1.5s 内起播可成功取消。
  暂停（PAUSED）仍按空闲计时。
