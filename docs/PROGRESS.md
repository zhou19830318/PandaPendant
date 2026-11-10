# Panda AI 进度快照与交接日志

> **接手先读本文件**，规则见根目录 `AGENTS.md`，任务清单见 `docs/TODO.md`，
> 已验证的坑位见 `docs/tech_notes.md`。
>
> 最后更新：2026-11-10 · Qoder (Claude) · 圆形屏边缘裁切修复（overlay 重排到安全半径内）

## 1. 当前状态（一句话）

固件可编译、可启动；WiFi STA/SoftAP/SNTP、配网 HTTP、JSON 语音云端闭环（测试音频）
与播放打断均已真机验证。KEY3 六个菜单页已从“只切图层”接到真实后端（Wi-Fi/勿扰/睡眠、
个性化、自动休眠、真实滑块、SD/版本、Open-Meteo 天气）。本轮按厂商 demo 重写 TTS 播放
策略（多相重采样、500ms 预缓冲、启动/续播/打断淡入淡出、100/300ms 暂停回滞、700ms 静音
冲刷、DMA 间隙检测），并让 ASR 只渲染最终转写以消除字幕重复；配网页改为仅 SoftAP
开启时运行，自动 fallback AP 在 STA 恢复后 5s 关闭；字幕固定 2 行、超长自下而上滚动。
已定位“假在线”断网：WiFi 保持 IP 但 ARP/TCP 全断且无 disconnect 事件（ESP-IDF
#8123/#5717/#15704 同款），新增 WS/天气错误聚合触发 `disconnect+connect` →
`stop/start` 恢复阶梯与 `wifi recover` 命令；实测 67s 故障 → 95.5s 触发 → 99.5s
恢复并完成下一轮语音。构建 `0x801a60`（20% free），已烧录 `/dev/ttyACM0`。
屏幕、触摸与喇叭已连接；该卡死仍会偶发，恢复期间在途回复会丢失。
本轮修复对话中途断流：按 demo 对齐 WiFi/TCP 缓冲（TCP 窗口 32KB→5.7KB、WS buffer
4096）；`WEBSOCKET_EVENT_ERROR` 不再被误判为断线（旧代码因此停掉静音泵，服务端
70s 后以无音频输入关会）；天气内部堆门槛 28KB→16KB 且资源不足不再上报网络错误
（旧逻辑误触发 WiFi 恢复、杀掉进行中的会话）；发送 poll 超时不再 abort 连接。
真机 12/12 轮 `voice clip` 全链路通过、0 丢包、0 次 WS 断开。
BOOT 键改为专用配网入口：运行时长按 4s 开/关 SoftAP+配网页，PTT 仅保留触摸长按
（GPIO0 为 strapping 脚，开机时按住会进下载模式，不能用于上电手势）。
电池 ADC 已按原理图改为 GPIO1/ADC1_CH0、分压 ×3，并加上电自检；但本板
R53/R78 两个 0R 选择跳线未焊，GPIO1 实测悬空（内拉低 0 mV），因此当前显示
“未知/未接线”，补焊后才可读数。O2.0 二进制协议未实现，语音唤醒未实现。

## 2. 里程碑快照

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M0 工程骨架/硬件自检 | `[~]` | 构建、启动、I2C/RTC/SD/RGB/MIC/屏/触摸/按键、CLI 已实测；电池待补焊 0R |
| M1 语音闭环 | `[~]` | JSON 云端链路、TTS 数据通路、打断已验证；O2.0 未实现，喇叭与真实语音 ASR 未验证 |
| M2 熊猫角色 | `[~]` | 三维状态机、37 屏映射、图片解码/资源校验已完成；无屏无法验证实际像素、手势与帧率 |
| M3 产品化基础 | `[~]` | 配网页/SNTP/亮度/音量/NVS 已验证；菜单后端、天气、自动休眠已接线待真机；深睡未完成；电池 ADC 固件就绪待补焊 0R |
| M4 结构与可靠性 | `[ ]` | 未开始（依赖硬件团队） |

## 3. 当前会话

### 2026-11-10 · Qoder (Claude) · README 界面展示
- 目标：在 README 增加实际运行界面图，完整展示圆屏效果。
- 结果：完成（文档改动，无固件变更）。
- 改动：
  - `docs/images/`：新增 8 张 360×360 状态机截图（待机/聆听/思考/说话/未听清/
    断网/低电/睡眠），圆屏透明裁切；
  - `README.md`：新增“界面展示”表格与说明，构建尺寸更新为 `0x804590`。
- 验证：截图来自固件实际状态机 + 生成 UI 资源的主机仿真渲染；逐张目视确认
  字幕位于下 1/4、状态/提示不越出圆形可视区。
- 未验证：GitHub 页面上的图片排版待推送后确认。
- 下一步：提交并推送。
- 阻塞：无。

### 2026-11-10 · Qoder (Claude) · 圆形屏边缘裁切修复
- 目标：圆形 360×360 屏上，文本/背景框不得被黑色边框切掉；字幕限定在屏幕下 1/4。
- 结果：完成（几何/构建/真机串口/仿真均有证据），观感待用户目视。
- 改动：
  - `components/panda_ui/panda_ui.c`：状态 pill 200 宽@bottom44；字幕 pill 200 宽
    @bottom42、高 48 → y=270..318（下 1/4，四角 r≈170.4）；toast 240 宽@top64；
    字幕非空或屏自带底部 tip 时隐藏状态 pill，避免压字；新增 `screen_has_tip()`。
  - 6 个生成屏 `ui_{network_lost,reconnecting,asr_failed,llm_error,tts_error,battery_low}.c`：
    tip y 135→127（屏上 298→290），底部角 r≈167。
  - `tools/ui_sim/web/index.html|app.js`：同步 overlay 尺寸/位置与状态抑制；
    `pages.js` 由 `gen_pages.py` 重新生成。
- 验证：
  - `idf.py build` → 通过，`0x804590`，分区剩余 20%；已烧录 `/dev/ttyACM0`。
  - 串口 `ui subtitle` → `I (12006) ui: subtitle 38B label=192x40 box=200x48 scroll=0`。
  - 仿真 DOM（圆形裁切开）：字幕 x=80,y=273,w=200,h=45；状态 pill 被隐藏；
    截图确认 pill 整体在圆内、位于下部 1/4。
- 未验证 / 风险：真机实际像素边距与长字幕滚动观感需用户目视。
- 下一步：真机对话一轮，确认真实字幕出现时底部不压脸、不越出圆弧。
- 阻塞：无。

### 2026-11-10 · Qoder (Claude) · 电池 ADC 链路核查与自检
- 目标：电池已接，验证电压/电量代码是否能读出真实值。
- 结果：部分完成。查出原分压比假设错误并修正；实测本板电池采样 0R 未焊，
  GPIO1 悬空，补焊后才能完成数值验证。
- 改动：
  - `components/panda_board/panda_board.c`：ADC 固定为 ADC1_CH0(GPIO1)；
    新增上电自检（内拉低 20ms 读一次、悬空读一次，内拉低 ≥200 mV 才认为
    分压已接通）；`panda_board_battery_mv()` 未接通时返回 0；
    `panda_board_battery_charging()` 改为 `mv >= 4400`（USB 供电时
    BAT_OUT≈5 V，电池自身 ≤4.2 V）。
  - `main/Kconfig.projbuild`、`sdkconfig.defaults`：分压比 1745→3000
    （原理图 R2=200K / R11=100K，Vbat=Vadc×3），帮助文本写明 R53/R78。
  - `main/serial_cli.c`：`status` 未接通时显示 `battery : unknown (ADC not wired)`。
- 验证：
  - 查官方 1.1 原理图：BAT_ADC 经 R53/R78 两个 0R 选择跳线接 GPIO1，
    默认 NC；注释原文“默认不启用电池电压测量……摄像头将不可使用”。
  - 临时改读 ADC1_CH5(GPIO6)：稳定 9459 mV（×3），不是分压节点；
    GPIO1 在 20 次快采中漂移 588 mV，慢采 60 s 从 4.4 V 漂到 4.0 V，
    属典型悬空。
  - 修正后上电自检：`Battery sense not wired: GPIO1 pulldown=0 mV`，
    `status` → `unknown (ADC not wired)`，不再误报低电/假电量。
  - `idf.py build` → 通过，`0x804540`，分区剩余 20%；已烧录 `/dev/ttyACM0`。
- 未验证 / 风险：真实电压/百分比需补焊 R53(1-3)+R78(1-2) 后复测；
  插 USB 时 BAT_OUT 约为 5 V，百分比会显示 100%，拔掉 USB 才测电池本体
  （此时无串口日志，可靠屏幕/触摸查看）。
- 下一步：用户补焊 0R → 重启看 `Battery ADC on GPIO1 (pulldown=... open=...)`
  → `battery` 应稳定在 3.0–4.2 V（电池）或 ~5 V（USB）。
- 阻塞：无（仅缺硬件补焊）。

### 2026-11-10 · Qoder (Claude) · 对话中途断流根因修复
- 目标：查清“两次对话测试都有问题”（TTS 播放中断流）并修复。
- 结果：完成（合成语音全链路 12/12 轮通过）；真人 PTT 待用户复测。
- 改动：
  - `main/app_voice.c`：`WEBSOCKET_EVENT_ERROR` 不再重置会话/停泵，仅 DEBUG；
    WS `buffer_size` 8192→4096、`network_timeout_ms` 5000→10000（对齐 demo）；
    clip 上行入队失败改为重试，连续 200 帧（~4s）才强制 commit；
    新增 `app_voice_set_pump()`（诊断开关）。
  - `panda_ai/sdkconfig.defaults`：`STATIC_RX=6`、`RX_BA_WIN=6`、
    `TCP_SND_BUF/WND=5760`、`RECVMBOX=6`、`MAX_SOCKETS=16`（对齐 demo）。
  - `main/app_weather.c`：`WEATHER_MIN_INTERNAL` 28KB→16KB；仅 `ESP_FAIL`
    上报网络错误，`ESP_ERR_NO_MEM` 只记日志。
  - `managed_components/.../esp_websocket_client.c`：发送 poll 超时
    （`wlen==0 && errno==0`）不调用 `abort_connection`，仅返回失败丢帧。
  - `main/serial_cli.c`：新增 `audio tone [seconds]`、`voice pump on|off` 诊断命令。
- 验证：
  - `idf.py build` → 通过，`0x801a60`，分区剩余 20%；已烧录 `/dev/ttyACM0`。
  - 12/12 轮 `voice clip`（音量 30%）：每轮 ASR→TTS→`play drain complete`；
    日志 0 次 `Could not lock` / `websocket down`（运行期）；主机 1Hz ping 0 丢包。
  - 静置 90s：0/177 丢包，会话保持；之后再 clip 正常。
  - `weather refresh` → `weather: Beijing 11°C 晴 (code 0)`，未误触发恢复。
  - 堆：内部稳定 ~18.7–19.1KB（TLS 峰值最低 ~10.5KB），PSRAM ~7.35MB。
- 未验证 / 风险：真人语音 PTT 未复测；内部堆最低余量 ~10KB 偏紧；
  组件补丁在 `managed_components`，依赖更新可能被覆盖。
- 下一步：用户实际对话复测；若再出现断流，抓 `websocket down` 前的
  `transport_poll_*`/`esp_tls` 行并按 tech_notes 的阶梯排查。
- 阻塞：无。

- 上一会话（保留）：
### 2026-11-10 · Qoder (Claude) · BOOT 长按 4s 进入配网
- 目标：把配网入口改为 BOOT 长按 ≥4s，并检查与 BOOT 现有功能的冲突。
- 结果：完成（CLI 注入验证与构建通过）；物理长按时序待用户实测。
- 冲突结论：GPIO0 仅作 BOOT，未与外设复用；但它是 strapping 脚（复位时按住进入
  ROM 下载模式），只能运行后检测；BOOT 原是 600ms 长按 PTT，与 4s 直接冲突。
  按用户选择：BOOT 专用于配网，PTT 只保留触摸长按。
- 改动：
  - `main/app_core.c`：BOOT 4s 阈值（`BOOT_PROVISION_HOLD_US`），
    `boot_provision_toggle()` 开/关 AP+配网页；短按 toast 提示；移除 BOOT PTT 分支。
  - `main/serial_cli.c`：`key boot [long]` 注入，`key` 用法更新。
  - `README.md`、`docs/tech_notes.md`、`docs/TODO.md`：按键映射与说明同步。
- 验证：
  - `idf.py build` → 通过，`0x801640`，分区剩余 20%，已烧录 `/dev/ttyACM0`。
  - `key boot` → `boot short: hold 4 s for provisioning`，日志无 `PTT press`。
  - `key boot long` → `provisioning page enabled` + `provisioning ready`，
    `wifi status` 为 `softap: on / provision: on`，主机
    `curl http://192.168.x.x/api/status` → 200。
  - 再 `key boot long` → `provisioning page disabled`、`softap: off`。
  - 串口监听 40s 未检测到物理 BOOT 按压（推测无人操作），保留 `[~]`。
- 未验证 / 风险：物理按住 4s 的时序与去抖需实测；长按关闭等同 `wifi ap off`，
  会置用户主动关闭标志，本次运行不再自动 fallback。
- 下一步：用户按住 BOOT 4s，确认日志 `button 0 long` +
  `boot long: provisioning on`；再用 4s 关闭。
- 阻塞：无。

- 上一会话（保留）：
### 2026-11-10 · Qoder (Claude) · WiFi 假在线检测 + 自动恢复
- 目标：查清“网络又断开”的根因，给出可落地的恢复方案并真机验证。
- 结果：完成（缓解）；根因未根除。
- 改动：
  - `components/panda_connectivity/panda_connectivity.c/.h`：新增错误上报
    `notify_net_error/notify_net_ok` 与 `recover`；90s 窗口聚合 3 次 → worker 执行
    `disconnect+connect`，20s 无 IP → `stop/start`+`WIFI_PS_NONE`，45s 冷却；
    timer 回调改为投递队列；worker 栈 4096 放 PSRAM。
  - `main/app_voice.c`：WS 曾连接过且 WiFi 自称在线时，`WEBSOCKET_EVENT_DISCONNECTED`
    上报错误；连上后 `notify_net_ok`。
  - `main/app_weather.c`：HTTPS 失败上报错误（与 WS 交叉验证）。
  - `main/serial_cli.c`：新增 `wifi recover`（手动，绕过冷却）。
- 验证：
  - `idf.py build` → 通过，`0x801490`，分区剩余 20%。
  - 主机交叉验证：故障期间 `ping` 100% 丢包、`ip neigh` FAILED，设备无 disconnect 事件。
  - 手动 `wifi recover` → 约 4s 恢复。
  - 自动路径（TTS 触发）：67s 故障 → 95.5s 触发（3/3）→ 99.5s 恢复 IP →
    `reply(75 bytes)` + `play drain complete`；完整回归 193.9s 故障 → 199.3s 恢复。
  - 内部堆故障期间 ~44.7KB（最小 ~36.1KB），未因 worker 栈下降。
- 未验证 / 风险：恢复期间在途回复丢失；卡死仍偶发；demo 射频缓冲差异未压测。
- 下一步：对照 demo 单独压测 `STATIC_RX_BUFFER_NUM=6` 等配置；观察真机多轮语音下
  自动恢复频率。
- 阻塞：无。

- 上一会话（保留）：
- **2026-11-10 · Qoder (Claude) · 配网页按需开启 + 字幕 2 行滚动 + WebSocket 锁卡死定位**
- 目标：排查“老是断网是否因网页模式”；配网页改为需要时开；长回复文本最多显示 2 行、
  超出后自下而上滚动。
- 状态：**代码完成、编译通过（`0x800bf0`，20% free）、已烧录验证**；配网页按需开关
  与字幕窗口均已实测；长回复仍会因云端 WS 半帧读超时而中断播放（见风险）。
- 改动：
  - `app_main.c`：有凭证启动时不再开配网页（日志 `provisioning page idle`）。
  - `app_core.c`：按 SoftAP 边沿 start/stop 配网页（`provisioning page enabled/disabled`）。
  - `panda_connectivity.c`：自动 fallback AP 在 STA 拿到 IP 后 5s 关闭；AP_STOP 300ms
    防抖（AP 重新配置会瞬时 STOP/START）；AP 配置必须在 APSTA 模式下设置。
  - `panda_ui.c`：字幕框固定 2 行（340x56），文本超出时 label 以 12px/s 自下而上循环滚动。
  - `managed_components/espressif__esp_websocket_client/esp_websocket_client.c`：本地补丁
    recv 超时 `network_timeout_ms`(5s) → 500ms，避免持锁 5s 饿死上行（与厂商 demo 补丁一致）。
  - `app_provisioning.c`：页面文案改为“平时关闭/恢复 5s 后自动关闭”，stop 增加日志。
- 验证：
  - `idf.py build` → `0x800bf0`，分区剩余 20%。
  - 配网：启动 `provisioning page idle` + `curl :80` rc=7；`wifi ap on` →
    `provisioning ready` + `/api/status` 200；`wifi ap off` → `provisioning page stopped`
    + curl rc=7；AP 开关日志无 provisioning 抖动。
  - 字幕：`ui subtitle` 15B → `label=324x20 box=340x56 scroll=0`；167B →
    `label=324x100 box=340x56 scroll=1`。
  - 长回复（临时长人格）：`reply(372 bytes)` 全量、`subtitle 372B label=324x140`、
    播放启动；约 47.7s 后 WS 读卡住 → `play pause stall` + `DMA gap 690 ms` + WS down。
    补丁后复测仍出现 `esp_transport_write() returned 0`，未完成整段播放。
  - 已通过 `/save` 恢复默认人格：`persona=你是一只温柔、好奇、有点俏皮的熊猫伙伴。回答简短、口语化。`
- 未验证 / 风险：
  - “fallback AP 恢复后自动关闭”需路由器断网/重启才能实测，当前只做了代码审查；
  - 2 行滚动动画需目视确认（日志已证明 `scroll=1` 与固定框高）；
  - “断网”不是网页模式导致：本次启动过程配网页全程关闭，仍出现 reason=2/205 关联失败
    与 TLS/DNS 失败；长回复卡死仍待定位（疑似 TLS 半帧读 + 无线丢包）。
- 下一步：真机目视字幕滚动；在路由器恢复场景验证自动 AP 关闭；抓取 WS 卡住时的
  RSSI/重传与 TCP 行为，或评估播放期间上行静音泵降频。
- 上一会话（保留）：
- **2026-11-10 · Qoder (Claude) · 触摸无法唤醒 + STT 重复字幕 + TTS 爆音/播放卡死**
- 目标：修复触摸屏无法唤醒/无响应；说 “hello” 后字幕重复；TTS 爆音与整段卡住。
- 状态：**代码完成、编译通过（`0x7ff8b0`，20% free）、已烧录并启动**；
  触摸已实测恢复（`touch diag` 三条全 OK，长按触发 PTT，`user` 说 hello 成功识别）；
  内置测试音频 TTS 完整播完（`play drain complete`），用户确认本次无卡顿；
  真人长回复的爆音/卡顿仍待继续观察。
- 触摸根因：CST816S 空闲约 0.5s 自动进入低功耗，地址仍 ACK 但坐标寄存器读取全部
  NACK/超时；旧逻辑 100 次失败后永久禁用触摸，所以无法唤醒。修复：初始化写
  `0xFE=0x01` 关闭自动休眠；失败后每 ~3s 复位重试并可自恢复；新增 `touch diag`。
- 改动：
  - `main/voice_resampler.c/.h`（新增）：移植 demo 的 32 相×32 抽头 Kaiser β=8 多相
    重采样（Q15，24k→16k），替换线性插值，跨 delta 保持相位与历史。
  - `app_voice.c`：ASR delta 只记日志不渲染，`completed` 后一次性写入最终文本；
    回复 delta 用 `merge_text()` 兼容“增量/累积/重复”三种形状。
  - `app_voice.c` 播放任务按 demo 重写：500ms 预缓冲、启动 50ms 淡入、续播 10ms 淡入、
    停顿前 10ms 淡出、取消/打断淡出后再清环；100ms 暂停 / 300ms 恢复回滞；
    ring 静止 700ms 冲刷尾音；DMA 间隙 >170ms 视为断流并淡入；每轮仅一次 codec 写，
    移除 20×5ms 重试（单次写内部最多阻塞 1s，是长卡死主因）；5s 输出 ring/peak/nearclip。
  - `panda_touch.c`：初始化关闭 CST816S 自动休眠（`0xFE=0x01`）；探测失败仍创建 indev，
    每 ~3s 复位并重试（不再永久禁用）；保留日志静音防洪泛；新增 `touch diag`
    原始寄存器读/write 检查。
  - `app_voice.c`：ASR delta 合并后仅作兜底，`completed` 的 `transcript` 为空时使用；
    仍只在 `completed` 渲染一次。
- 上一会话（保留）：
- **2026-11-09 晚 · Qoder (Claude) · KEY3 菜单全功能接线 + 天气/自动休眠**
- 目标：把 KEY3 审计中六个页面的未完成功能全部实现并接线，无硬件时编译/主机验证。
- 状态：**代码完成、编译通过（`0x7fe200`，20% free）**，字体 335 字符 PASS、UI 仿真 46/0；
  真机验收 2026-11-10（清单见 `docs/TODO.md` 末节）。
- 改动：
  - `app_settings.c/.h`：新增 `personality/feedback/animation/auto_sleep_s/wake_mode/
    sleep_anim/dnd/weather_city`，默认 1/1/1/300s/2/1/0/北京，NVS 键
    `pers/fb_lvl/anim_lvl/sleep_s/wake_mode/sleep_anim/dnd/wx_city`，含范围归一化。
  - `panda_ui.c/.h`：菜单钩子 `panda_ui_menu_hooks_t` 与 `panda_ui_menu_sync()`；
    隐藏静态亮度/音量图层并原位创建真实 `lv_slider`；选项行/开关/Wi-Fi/DND/睡眠按钮
    事件；蓝牙 tile 隐藏；SD 容量、版本号、日期/天气/Wi-Fi/百分比动态刷新；
    动画活跃度映射 0/150/250ms；语音唤醒选择时 toast 明示不支持。
  - `app_core.c/.h`：注册钩子（音量/亮度/个性化/睡眠/WiFi/DND）、自动休眠（每秒检查，
    语音 IDLE/OFFLINE 且无活动达阈值）、触摸/按键/语音事件刷新活动时间、唤醒输入抑制
    原动作、SD 用 `esp_vfs_fat_info()`、`app_core_settings_changed()` 统一反哺。
  - `app_weather.c/.h`（新增）：Open-Meteo geocoding+forecast，PSRAM 缓冲/任务栈，
    30min 刷新 / 60s 重试 / 离线 10s 轮询，内部堆 <28KB 跳过。
  - `app_voice.c`：`build_persona()` 把性格/反馈拼进 `session.create`。
  - `serial_cli.c`：`weather` 命令、新 `settings set` 键、`status` 增加天气/菜单行。
  - `app_provisioning.c`：天气城市字段；保存后 `app_core_settings_changed()` + 天气刷新。
  - `app_main.c`：启动应用 DND 静音、亮度、启动天气任务；工程版本 2.0.0。
- 上一会话（保留）：
- **2026-11-09 · Qoder (Claude) · WiFi 掉线/网页配网、中文乱码、TTS 断续**
- 目标：修复 WiFi 不稳定与配网页不可达、中文字体缺字、豆包 TTS 播放中断/丢包。
- 状态：**代码完成、编译通过（`0x7f7400`，20% free）**，主机检查 PASS；真机验收 2026-11-10。
- 改动：
  - `panda_connectivity.c/.h`：`WIFI_PS_NONE`、指数退避+抖动（1s→30s）、断线原因提示、
    全信道按信号排序、PMF capable、国家码 CN、20s 无 IP 自动 SoftAP（`ap_auto`）、
    `scan_aps()` 与 `disconnect_reason/hint/retry_delay/ap_auto` getter。
  - `app_provisioning.c`：`/api/scan`、首页扫描下拉/自动回退与断线原因文案、密码变更触发重连、
    `/api/status` 增 `rssi/reason/reason_hint/ap_auto/retry_ms`。
  - `panda_audio.c`：TX/RX 独立锁，采集不再阻塞播放。
  - `app_voice.c`：有状态重采样、200ms 预缓冲、部分写重试、RX 溢出重同步、WS 续帧（0x0）、
    重连/取消清环、`app_voice_get_stats()`；`app_core` 接入 `app_voice_set_network()`。
  - `serial_cli.c`：`voice stats`、`wifi status` 新增字段、`key 1|2|3 [long]` 注入。
  - `app_core.c/.h` + `panda_ui.c/.h`：KEY1/2 音量 ±10%、KEY3 短按循环设置菜单/长按回主屏；
    `app_core_key_action()` 与 CLI 共用。
  - `tools/fonts/check_coverage.py`（新增）：校验 UI 字符串字形覆盖与 `.fallback` 链接。
- 上一会话（保留）：
- **2026-11-09 · Codebuff (DeepSeek Fast) · 三维状态机/UI 映射 + 图片资源验证**
- 目标：实现 PRD 8.3 完整状态机与 UI 映射，主机虚拟跑通，并验证 UI 图片加载。
- 状态：**完成**（真机验证到 LVGL 解码/屏幕切换层；实际面板像素待接屏）。
- 新增：`panda_ai/components/panda_state/`（纯 C 三维状态机）、`tools/ui_sim/`
  （主机仿真 + 图片资源校验）、`tools/ui_sim/check_images.py`。
- 改动：`panda_ui.c`（状态机驱动屏幕/overlay/超时，用户设置页不被抢占，
  `panda_ui_verify_images()`、`panda_ui_set_error()`）、`panda_ui.h`、
  `serial_cli.c`（`ui images`、`ui error`）、
  `app_core.c`（启动/状态事件统一走状态机）。
- 文档改动：`docs/TODO.md`、`docs/PROGRESS.md`、`docs/tech_notes.md`、`README.md`。
- 补充（同一会话）：`tools/ui_sim/web` 增加“全部页面”预览，覆盖 20 个状态屏、
  12 个设备设置页和 6 个未枚举表情页；`gen_pages.py` 从 `generated/screens` 提取坐标、
  颜色、圆角、图片与页面跳转链接，生成 `web/pages.js`，不手写布局。
- 补充验证：`node tools/ui_sim/web/test_state.js` → 46/0；headless Firefox 逐页打开 18 个
  设置/表情页，33 个页面图片全部加载；`quick_settings→system_settings`、
  `state_preview→wake`、`character_preview_1→face_sad` 跳转正确；本轮无固件 C 代码改动，
  构建基线仍为 `0x70ccc0`。

## 4. 接下来做什么（按优先级）

1. **P0 真机验收 KEY3 菜单全功能（2026-11-10）**
   - 按 `docs/TODO.md` 末节清单逐项跑：滑块、Wi-Fi 开关、DND 静音、自动休眠/唤醒、
     个性化持久化、SD/版本、天气取数；记录实际画面与串口日志。
2. **P0 接喇叭/麦克风后验证可听链路**（当前硬件阻塞）
   - 验收：`voice clip` 能听到回复；真人长按 PTT 说话，日志出现非空 ASR 文本；
     音量 0/70/100 无爆音/截断。
3. **P0 语音协议对齐生产（O2.0）**
   - 需要控制台开通与 `app_id`/`access_key`（当前未配置）。实现后把默认 `api_mode` 切回 `o2`。
4. **P1 配网页产品化（代码已完成，待真机闭环）**
   - 待验证“网页扫码改 WiFi→自动重连→重启保持”的闭环（需可临时改动的测试 AP）。
5. **P1 稳定性/功耗**
   - 语音 WebSocket 固定 5s 重连，鉴权错误（401/403）未分类停止；
   - 修复 `voice cancel` 后 UI 偶发被服务端 `transcription.started` 置为“聆听”的状态竞争；
   - 深睡功耗策略未做（当前 SLEEP 只改 UI，Wi-Fi/WS 仍在线），语音唤醒未实现。

## 5. 阻塞项

| 阻塞 | 影响 | 解除条件 |
|---|---|---|
| 无（触摸已恢复） | 手势/唤醒仅做了长按 PTT 与 diag 验证 | 按 `docs/TODO.md` 触摸清单补全手势/休眠唤醒验收 |
| 豆包 O2.0 控制台权限/app_id/access_key 未配置 | 生产二进制协议无法落地 | 控制台开通并写入凭证 |
| 电池 ADC 的 R53/R78 0R 未焊 | 电量/低电策略显示未知 | 补焊 R53(1-3)+R78(1-2) |

## 6. 最近一次验证（2026-11-10，本轮）

### STT 去重 + TTS 抗爆音/卡顿（烧录 + 启动）

- 构建：`idf.py build` → **通过**，`panda_ai.bin binary size 0x7ff8b0`，
  剩余 `0x200750 bytes (20%)`。
- 烧录启动（`/dev/ttyACM0`，USB-SERIAL-JTAG）：`audio ready (16kHz mono, mic=yes, volume=70%)`；
  WiFi 连接、`voice: session created id=…`、`weather: 北京 2°C 晴 (code 0)` 正常。
- 触摸（修复后）：启动 `I (2431) CST816S: IC id: 181`、
  `I (2432) touch: CST816S ready @0x15 (auto-sleep off)`；此后不再刷 NACK。
  `touch diag` → `id=ESP_OK id=181, data=ESP_OK`；关闭自动休眠 1s 后仍 `data=ESP_OK`；
  复位+关闭休眠 1s 后 `data=ESP_OK num=1`。用户长按屏幕成功触发
  `touch action long_press (187,95)` → `PTT press (source=touch)` → 识别 “hello”。
- 测试音频一轮 TTS：`play start ring=12288` → `play pause ring=1408` →
  `DMA gap 390 ms`、`DMA gap 219 ms`（淡入处理）→ `tts done deltas=17 ring=14600`
  → `play drain complete (tail)`；用户确认本次播放无卡住。
- 未验证（等真人语音）：字幕是否只显示一份（本次 ASR 最终转写非空：
  `你是谁呀你叫什么名字你是谁呀？你叫什么名字？`）、爆音是否消失、
  长回复是否还卡；继续对照 `asr delta #N`、`play start/pause/resume`、`DMA gap`、
  `playback underrun`、`play pause stall`、`play loop … nearclip`、`tts done`、
  `play drain complete`。

### KEY3 菜单全功能 / 天气（主机侧，无屏）

- 构建：`idf.py build` → **通过**，`panda_ai.bin binary size 0x7fe200`，
  剩余 `0x201e00 bytes (20%)`；`Project build complete`。工程版本 `2.0.0`。
  `idf.py size`：Flash Data 6948828 B、Flash Code 1294148 B、DIRAM 73.0%。
- 字体：`python3 tools/fonts/check_coverage.py` → **PASS**，`ui_font_CJK16.c` 8371 字形，
  UI 源码 335 个不同非 ASCII 字符全部有字形（上轮 269，新增菜单/天气文案）。
- 主机回归：`make -C tools/ui_sim test` → **46 checks, 0 failure(s)**（状态机 + 图片资源）。
- 天气契约（主机 curl，Open-Meteo）：
  - geocoding `北京` → `{'name': '北京', 'latitude': 39.9075, 'longitude': 116.39723, 'country': '中国'}`
  - forecast → `{'time': '2026-11-09T23:00', 'interval': 900, 'temperature_2m': 3.6, 'weather_code': 0}`
  - 字段与 `app_weather.c` 的 `results[0].latitude/longitude`、`current.temperature_2m/weather_code`
    一致；WMO 0/1→晴、2→多云、其余→阴（仅 3 图标）。
- 未验证：所有菜单交互、滑块手感、Wi-Fi 开关真实断连/重连、DND 静音、自动休眠/唤醒、
  天气 TLS 取数与中文显示、SD/版本数值——无屏/触摸/喇叭，2026-11-10 真机逐项验收。

### WiFi / 字体 / 语音修复（上轮，主机侧）

- 构建：`idf.py build` → **通过**，`panda_ai.bin binary size 0x7f7400`，
  剩余 `0x208e80 bytes (20%)`；`Project build complete`。
- 字体：`python3 tools/fonts/check_coverage.py` → **PASS**，`ui_font_CJK16.c` 8371 字形，
  UI 源码字符串 269 个不同非 ASCII 字符全部有字形，`Big60/Body18/Small16/Title22`
  四个字体均含 `.fallback = &ui_font_CJK16`。
- 主机回归：`make -C tools/ui_sim test` → **46 checks, 0 failure(s)**（状态机）。
- 未验证：WiFi 掉线恢复/AP 回退、网页扫码切换、TTS 可听连续性与 `voice stats` 计数、
  中文字幕/提示的实际像素、KEY1–3 物理按键顺序与菜单切换——无硬件，计划 2026-11-10 真机验证。
  无屏时可用 `key 1|2|3 [long]` 走同一套动作做串口验证。

### 状态机 / UI / 图片（上轮）

- 主机仿真：`make -C tools/ui_sim test` → **46 checks, 0 failure(s)**。覆盖启动、唤醒超时、
  PTT 连接/聆听/思考/说话、打断、连接超时、WiFi 断线/重连/配网、低电优先级、
  情绪超时、睡眠/唤醒、ASR/LLM/TTS/Auth/Network 错误与超时等全流程。
- 图片资源：`python3 tools/ui_sim/check_images.py` → **PASS**，58 个描述符、58 个被屏幕引用、
  4.9 MiB 像素数据；逐张校验 magic/颜色格式/尺寸/data_size/数组长度/非空像素。
  另生成 19 张 360×360 熊猫脸接触表，主机解码画面正常（无空白/错位）。
- 真机图片加载：`ui images` → `37 screens, 71 image objects, 58 unique images, 0 failures`，
  连续两次一致；LVGL 解码器接受全部源，未出现 `Image decoder`/尺寸错误日志。
- 真机屏幕解析：`ui state listening/thinking/speaking/idle`、`ui emotion happy`、
  `ui sleep`、`ui wake` 均按预期切到 listening/thinking/speaking/idle/face_happy/sleep/wake；
  wake 1.2s 后自动回 idle。`ui error asr/tts/network/auth` 分别切到
  error_asr/error_tts/offline/error_llm，`ui error clear` 立即回 idle。测试中 WebSocket
  恰好连上，`session.created` 把 listening 拉回 idle，属语音事件优先级而非 UI 映射错误。
- 构建/烧录：`idf.py build` 通过；`panda_ai.bin` = `0x70ccc0`，factory 剩余 30%；
  `idf.py -p /dev/ttyACM0 flash` 成功，新固件运行正常。
- 未验证：实际面板像素、滑动/点击手势、动画帧率与功耗（屏幕未接）。

### WiFi / 配网 / 语音（上轮）

- 构建/烧录：`idf.py build` 通过；`panda_ai.bin` = `0x70b360`，factory 剩余 30%；
  `idf.py -p /dev/ttyACM0 flash` 成功并复位运行新固件。
- WiFi：`wifi ap on` 后 `SoftAP started: Panda-AI-00A1`，STA 连上 <家庭WiFi>，
  IP `192.168.x.x`、RSSI `-46 dBm`；日志 `wifi: time synced: 2026-11-09 15:12:35`。
- 配网（主机直连设备 LAN IP）：`GET /` 返回完整中文页面；`GET /api/status` 返回
  `{"wifi":"connected",...,"heap":...}`；`POST /save` 写入 volume=71 后 `settings show`
  确认，再恢复 70；`GET /generate_204` 返回 302→`http://192.168.4.1/`。
- 语音云端：`voice clip` 发送 47104 samples → `commit acked` → `tts started`
  → 回复文本 → `tts done deltas=12 ring=8269`；队列排空后 `status` 显示“待机”。
- 打断：TTS 开始后 0.6s 发送 `voice cancel` → `voice cancel: ESP_OK`，
  随后 `tts done deltas=2 ring=0`（迟到音频被丢弃）；紧接着再 `voice clip`
  得到 `deltas=11 ring=6924`，会话可继续。
- 已知测试盲区：测试音频的 ASR 文本为空（`asr:`），且无喇叭，声音质量未验证。

## 7. 环境与关键参数

| 项 | 值 |
|---|---|
| ESP-IDF | v5.5-dirty，`/home/conor/esp32/esp-idf/export.sh` |
| 工程 | `panda_ai/`（`idf.py` 需在此目录执行） |
| 芯片 | ESP32-S3 v0.2，16MB Flash + 8MB PSRAM |
| 串口 | `/dev/ttyACM0`，115200，USB-SERIAL-JTAG |
| 分区 | factory 10MB / assets 5.75MB / coredump |
| 配置 | `CONFIG_PANDA_PROVISIONING=y`、`PANDA_BATTERY_ADC` 关闭、UI FPS 30、版本 2.0.0 |
| 网络 | 设备 STA `192.168.x.x`，SoftAP `192.168.4.1`（上轮实测） |
| 天气 | Open-Meteo geocoding + forecast，HTTPS + `esp_crt_bundle`，默认城市北京 |

## 8. 已知风险 / 技术债

- **安全**：`voice cancel` 后若服务端迟到 `transcription.started`，UI 可能从“待机”变“聆听”；
  播放已停止，属状态竞争，需下一轮回合前复位。
- **内部 RAM 偏紧**：启动后 internal 约 36–46KB，TLS+WebSocket 并发时可能不足，
  新缓冲必须放 PSRAM。天气 HTTPS 与语音 WS 会同时占用 TLS 内部堆；已加
  “内部堆 <28KB 跳过本轮、60s 重试”的保护，真机需观察 `heap` 与天气是否会长期取不到。
- **语音唤醒未实现**：`wake_mode` 已落盘并在 UI 中可选，但只有触摸/按键会真正唤醒；
  选“语音”时只 toast 提示，后续需要常开低功耗麦克风与关键词检测。
- **无蓝牙**：固件未链接 BT 协议栈，快捷设置里的蓝牙 tile 已隐藏；若产品需要 BT 配网/音频
  需另立任务评估 Flash/RAM 与共存。
- **天气图标只有 3 类**（晴/多云/阴），雨雪/雷暴只改文字；城市经 Open-Meteo geocoding，
  从未取到数据时页面显示 `--`，之后失败保留上次结果，不会崩溃。
- **自动休眠只是 UI 睡眠**：Wi-Fi 与语音 WS 仍在线，不做深睡省电；真机需测待机电流。
- **菜单交互未接屏**：滑块拖动、点击区扩展 18px、选项行高亮与布局遮挡均需 2026-11-10 真机确认。
- **LVGL 池接近上限**：37 屏常驻后 `used=71/85KB (84%)`、frag=11%（配置 96KB）；
  再加动画/列表前需扩容或改按需创建屏幕。
- 语音 WebSocket 开机即启动并每 5s 重连，无退避/鉴权错误分类，长期空转会刷日志
  （WiFi 层本轮已指数退避，WS 层未改）。
- 配网页在无凭证或 20s 无 IP 时自动可达；若 STA 在线且只想改配网，仍需 `wifi ap on`。
- 24k→16k 为线性插值（步进 1.5），跨 delta 已保持相位，但非多相重采样；
  音质需真机接喇叭评估。
- TTS 播放依赖 200ms 预缓冲与 PSRAM ring；真机需看 `voice stats` 的
  `underruns/ring_drops/decode_failures` 判断是否仍有断续。
- 无 AEC，播放时需保持半双工（停采集），否则模型会听到自身 TTS。
- 仓库当前不是 git 工作区（文件系统直接修改），改动前建议自行备份/初始化版本管理。

## 9. 交接日志（新的在最上面）

### 2026-11-10 · Qoder (Claude) · 触摸恢复 + STT 重复字幕 + TTS 爆音/卡死
- 目标：修掉触摸无法唤醒/无响应、“hello” 字幕重复、TTS 爆音与整段卡住不播。
- 结果：代码完成、编译通过、已烧录；触摸已实测恢复并成功长按 PTT；TTS 测试音频完整播完。
- 改动：`panda_touch.c` 写 `0xFE=0x01` 关闭 CST816S 自动休眠、失败后周期复位自恢复、
  新增 `touch diag`；新增 `voice_resampler.c/.h`（32 相多相 24k→16k）；`app_voice.c`
  播放任务按 demo 重写（预缓冲/淡入淡出/暂停回滞/静音冲刷/DMA 间隙/单次写），
  ASR 只渲染最终转写并用 delta 兜底空 transcript。
- 验证：`idf.py build` → `0x7ff8b0`（20% free）；启动 `CST816S ready (auto-sleep off)`；
  `touch diag` 三条读均 ESP_OK；长按 (187,95) → PTT → ASR “hello”；
  测试音频 `play start ring=12288` → 2 次 DMA gap（390/219ms，淡入）→
  `play drain complete`，用户确认无卡顿。
- 未验证 / 风险：真人长回复的爆音/卡死仍需复测（上一条 hello 回复卡过一次，未抓到日志）；
  手势/休眠唤醒全量、字幕单份显示待确认；多相重采样未做频谱对比。
- 下一步：保持串口监听，复现卡顿时抓 `play pause/resume`、`DMA gap`、`underrun`、
  `play pause stall`、`play loop nearclip`；必要时调整暂停/恢复阈值。
- 阻塞：无。

### 2026-11-09 晚 · Qoder (Claude) · KEY3 菜单全功能 + 天气/休眠
- 目标：把 KEY3 六个菜单页的未完成功能全部实现并接线（审计项 P1/P2）。
- 结果：完成（代码层）；无屏/触摸/喇叭，真机验收待 2026-11-10。
- 改动：`app_settings`（8 个新字段 + NVS）、`panda_ui`（钩子/真实滑块/事件/动态信息）、
  `app_core`（钩子注册/自动休眠/活动跟踪/SD/统一反哺）、新增 `app_weather`、
  `app_voice`（persona 组合）、`serial_cli`（weather/新 settings 键）、
  `app_provisioning`（天气城市）、`app_main`（启动应用）、版本 2.0.0。
- 验证：`idf.py build` → `0x7fe200`，20% free；`check_coverage.py` → 335 字符 PASS；
  `make -C tools/ui_sim test` → 46/0；Open-Meteo 字段用主机 curl 核对。
- 未验证 / 风险：所有菜单交互、Wi-Fi 开关闭环、DND 静音、自动休眠/唤醒、天气 TLS、
  SD/版本显示；语音唤醒未实现；BT tile 隐藏。
- 下一步：按 `docs/TODO.md` 末节清单做 2026-11-10 真机验收并回填结果。
- 阻塞：硬件（屏/触摸/喇叭）。

### 2026-11-09 · Qoder (Claude) · KEY1–3 音量/菜单按键
- 目标：KEY1/KEY2 调音量，KEY3 切换多功能菜单。
- 结果：代码完成、编译通过；待真机确认按键物理顺序与触发。
- 改动：`app_core.c/.h`（`app_core_key_action`、音量步进/保存/toast）、
  `panda_ui.c/.h`（`panda_ui_menu_next/menu_home`）、`serial_cli.c`（`key` 注入）。
- 验证：`idf.py build` → `0x7f7400`，20% free；无硬件，未做运行验证。
- 下一步：真机按 KEY1/2 看 `volume` 与 toast、按 KEY3 逐页切换、长按回主屏。
- 阻塞：硬件（按键在 TCA9555 上，需真机）。

### 2026-11-09 · Qoder (Claude) · WiFi/字体/TTS 三项修复
- 目标：修 WiFi 掉线与网页配网、中文缺字、豆包 TTS 断续/丢包。
- 结果：代码完成、编译通过；主机检查 PASS；真机验收待 2026-11-10。
- 改动：`panda_connectivity`（ps off/退避/AP 回退/扫描/getter）、`app_provisioning`
  （`/api/scan`、状态字段、重连）、`panda_audio`（TX/RX 锁分离）、`app_voice`
  （重采样/预缓冲/写重试/续帧/溢出重同步/stats）、`serial_cli`、新增字体覆盖检查脚本。
- 验证：`idf.py build` → `0x7f7180`，20% free；`check_coverage.py` → PASS（266 字符）；
  `make -C tools/ui_sim test` → 46/0。
- 未验证 / 风险：三项修复均未接硬件；WS 仍固定 5s 重连；线性插值音质未评估。
- 下一步：真机跑掉线恢复、网页扫码切换、长 TTS + `voice stats`、中文字幕。
- 阻塞：硬件（屏/喇叭/触摸）。

### 2026-11-09 · Codebuff (DeepSeek Fast) · 三维状态机/UI 映射 + 图片资源验证
- 目标：按 PRD 8.3 实现设备/语音/情绪三维状态机，映射 37 屏并虚拟跑通；同时验证图片加载。
- 结果：完成（状态机仿真 46/46，通过真机 `ui images` 与屏幕切换验证；实际面板待接屏）。
- 改动：新增 `panda_state` 组件、`tools/ui_sim` 仿真与图片校验；`panda_ui` 改为状态机统一
  决策（优先级：睡眠 > 低电 > 错误 > 唤醒 > 离线 > 语音 > 情绪 > 待机，含超时）；
  `serial_cli` 增加 `ui images`；`app_core` 启动/事件接入状态机。
- 验证：`make -C tools/ui_sim test` 46 checks/0 fail；图片 58/58、4.9MiB、PASS；
  `ui images` 37 屏/71 对象/58 唯一/0 失败；构建 `0x70ccc0`，30% free。
- 未验证 / 风险：无屏看不到实际像素和手势；图片校验是解码/元数据层，不含面板色彩/撕裂；
  cancel 后迟到 `transcription.started` 的状态竞争仍在。
- 下一步：接屏后跑完整 37 屏视觉与手势回归，再补 SD 动画 manifest。

### 2026-11-09 · Codebuff (DeepSeek Fast) · WiFi/配网/语音云端验证与打断修复
- 目标：跑通上一轮列出的 P0 验证，并修掉验证中暴露的问题。
- 结果：完成（WiFi/配网/JSON 云端/打断均已真机验证）。
- 改动：`app_settings.c` 默认 `api_mode=json`；`app_voice.c` 增加取消丢包标志、
  TTS 播完回待机、cancel 复位状态；`serial_cli.c` 增加 `voice cancel`。
- 验证：build `0x70b360`（30% free）→ flash → 串口/HTTP/语音全链路（详见第 6 节）。
- 未验证 / 风险：无喇叭与真实语音、O2.0、网页切换 WiFi；cancel 后 UI 有状态竞争。
- 下一步：接喇叭/麦克风验证可听链路；再评估 O2.0 与配网自动回退。
- 阻塞：硬件（屏/触摸/喇叭）、O2.0 凭证。

### 2026-11-09 · Codebuff (DeepSeek Fast) · 建立多模型交接流程
- 目标：统一“完成即验证、验证即更新文档”的接力规则。
- 结果：完成（文档）。
- 改动：新增 `AGENTS.md`、`docs/PROGRESS.md`；更新 `docs/TODO.md`（校正
  connectivity/provisioning/voice 状态与构建尺寸）、`docs/tech_notes.md`
  （WiFi/语音/内存实测）、`README.md`（文档索引与当前限制）、`.gitignore`（密钥文件）。
- 验证：`idf.py build` 通过（`0x70b1e0`，剩余 30%）；串口启动/`status`/`wifi status`/`heap` 实测
  （当次结果见 `docs/TODO.md` 的真机验证记录）。
- 未验证：见第 5 节阻塞项。
- 下一步：按第 4 节第 1 项做配网闭环验证。
- 阻塞：无（文档任务本身）。
