# Panda AI 熊猫智能挂坠

基于 Waveshare ESP32-S3-AUDIO-Board 的桌面/挂坠式 AI 语音玩具固件。
产品需求见 [Panda_AI_熊猫智能挂坠_PRD_V1.0.md](Panda_AI_熊猫智能挂坠_PRD_V1.0.md)，
可行性结论见 [docs/feasibility.md](docs/feasibility.md)。

**协作开发（多模型接力必读）**：[AGENTS.md](AGENTS.md) 规定了开工/收工流程，
当前进度与交接记录见 [docs/PROGRESS.md](docs/PROGRESS.md)，任务清单见 [docs/TODO.md](docs/TODO.md)，
已验证的坑位见 [docs/tech_notes.md](docs/tech_notes.md)。

## 界面展示

360×360 圆形屏的实际状态机画面（主机仿真渲染，字体与图片直接取自固件生成资源）：

| 待机 | 聆听 | 思考 | 说话（底部 1/4 字幕） |
|:---:|:---:|:---:|:---:|
| <img src="docs/images/ui-idle.png" width="180" alt="待机"> | <img src="docs/images/ui-listening.png" width="180" alt="聆听"> | <img src="docs/images/ui-thinking.png" width="180" alt="思考"> | <img src="docs/images/ui-speaking.png" width="180" alt="说话"> |
| **未听清** | **网络断开** | **低电提示** | **睡眠** |
| <img src="docs/images/ui-error.png" width="180" alt="未听清"> | <img src="docs/images/ui-offline.png" width="180" alt="网络断开"> | <img src="docs/images/ui-lowbat.png" width="180" alt="低电提示"> | <img src="docs/images/ui-sleep.png" width="180" alt="睡眠"> |

> 字幕固定在屏幕下 1/4（y=270..318），状态/Toast 保持在圆形安全半径内。
> 低电屏的百分比当前为示例文案，电池 ADC 补焊后改为实时值。
> 快捷设置、个性化、时间天气等菜单页可在本地浏览器仿真中逐页查看（见下方“浏览器仿真”）。

## 目录

```
AGENTS.md                  多模型协作与交接规则（每次开发前必读）
panda_ai/                  当前固件工程（ESP-IDF v5.5）
  main/                    启动、应用状态机、NVS、串口 CLI、配网网页、语音管线
  components/
    panda_board/           I2C0、TCA9555、按键、SD、RTC、电池、WS2812
    panda_display/         ST77916 QSPI 360x360 + 背光 + LVGL port
    panda_touch/           CST816S 触摸 + 软件手势
    panda_audio/           ES8311/ES7210 + I2S 全双工 + 播放/采集
    panda_connectivity/    WiFi STA/SoftAP、重连、SNTP
    panda_state/           设备/语音/情绪三维状态机（纯 C，无 LVGL 依赖）
    panda_ui/              SquareLine 屏幕包装层 + 生成资产 + 图片自检
    panda_mp3/             SD 卡 Music 目录扫描与 MP3 解码播放
    panda_led/             7x WS2812 状态/播放/事件动画 + 节能调度
  partitions.csv           factory 10MB / assets 5.75MB / coredump
tools/ui_sim/              主机状态机仿真 + 图片资源校验
demo/                      厂商与参考 demo（只读参考，禁止提交真实密钥）
docs/                      可行性、TODO、进度快照、技术积累
```

## 构建

```bash
source /home/conor/esp32/esp-idf/export.sh
cd panda_ai
idf.py build
```

当前产物约 8.20MB（`0x82f350`，固件版本 2.0.0），factory 分区剩余 18%
（`0x1d0cb0`）。其中约 96KB 来自音乐页图标所需的 48px Montserrat 字体
（`CONFIG_LV_FONT_MONTSERRAT_48=y`，固定在 `sdkconfig.defaults`）。

主机侧自检（不需要设备）：

```bash
make -C tools/ui_sim test                        # 状态机 46 项断言 + 58 张图片资源校验
python3 tools/ui_sim/check_images.py --sheet panda_ai/build/ui_face_sheet.png
python3 tools/fonts/check_coverage.py            # UI 中文字符是否都在 ui_font_CJK16 里
```

中文界面使用 SquareLine 字体 + `ui_font_CJK16` 回退。重新导出 UI 后若需重建字体：

```bash
python3 tools/fonts/gen_cjk_font.py              # 需 lv_font_conv（或 npx）
python3 tools/fonts/check_coverage.py            # 同时检查四个字体的 .fallback 是否还在
```

浏览器仿真（状态机 + 全部生成页面）：

```bash
python3 -m http.server 8765 --directory tools/ui_sim/web
# 打开 http://127.0.0.1:8765/ ，在“全部页面”里切换 20 个状态屏、
# 12 个设备设置页和 6 个未枚举表情页；页面坐标与图片直接取自 generated/screens。
python3 tools/ui_sim/gen_pages.py  # SquareLine 重新导出后刷新 web/pages.js
```

## 烧录与监视

```bash
cd panda_ai
idf.py -p /dev/ttyACM0 flash monitor
```

开发板使用 USB-SERIAL-JTAG（`/dev/ttyACM0`）。若没有设备节点，先检查 USB 线/驱动。

## 串口 CLI

启动后按回车，提示符 `panda>`，输入 `help` 查看全部命令。常用：

```text
status                          # 固件/UI/电池/堆状态；app 行含主循环 loops/max_gap/age
ui state listening              # idle|connecting|listening|thinking|speaking|offline|error
ui emotion happy                # neutral|happy|surprise|sad|sleepy|curious|shy|angry
ui error asr                    # asr|llm|tts|audio|network|auth|generic|clear
ui device boot|ready|low|charging   # 设备状态屏
ui sleep | ui wake              # 睡眠/唤醒（同时控制 RGB 灯环）
ui images                       # 37 屏图片解码自检（无屏可用）
ui status 正在连接              # 顶部状态 pill
ui subtitle 你好呀              # 字幕 pill（ASR 在最终转写完成后显示一次）
ui toast 测试提示               # toast
ui audio 70                     # 模拟音频电平 0..100
ui volume 70                    # 模拟系统音量显示
touch tap|double|long|left|right|up|down
touch diag                      # 触摸面板原始读检查（ID/坐标寄存器/自动休眠）
key 1|2|3|boot [long]           # 注入按键动作（boot long = 切换配网模式）
brightness [0..100]
volume [0..100]
battery
time [get] | time set <epoch> | time sync
settings show|save|reset|set <key> <value>
# 菜单设置键：personality 0..2 / feedback 0..2 / animation 0..2
# auto_sleep_s 0|60|300|600 / wake_mode 0..2 / sleep_anim 0|1 / dnd 0|1 / weather_city <城市>
voice [start|active|ptt down|ptt up|clip|pump on|off|cancel|stats]
weather [show|refresh|set <city>]   # 天气查询/立即刷新/设置城市
wifi [status|scan|connect <ssid> [pass]|disconnect|forget|ap on|off|sync|recover]
# wifi status 额外输出 ap_auto / reason / reason_hint / retry
# wifi recover 手动执行“断连→重连→(无 IP) stop/start”恢复；WS/天气连续出错会自动触发
# voice stats 输出 TTS 计数：deltas/ring_drops/underruns/write_retries/rx_overflows
# voice pump off 关闭对话间 50ms 静音上行（诊断用；关闭后服务端会因无音频输入关会话）
audio test | audio status | audio tone [seconds]   # tone 连续 880Hz（诊断用）
rgb status | rgb mode off|state|audio|full|manual | rgb brightness [0..100]
rgb <r> <g> <b> | rgb off            # 手动常亮单色 / 关闭灯环
# rgb status 输出 mode/brightness/level/refresh；level 为最近 250ms 的输出峰值 0..100
i2c | sdcard status|ls [path]|mount|unmount | rtc [write <epoch>]
mp3 status|list|play [index]|toggle|next|prev|stop   # SD 卡 Music 目录播放
# mp3 status 额外输出 audio_rate/underruns/buffered/flags；稳定播放应 underruns=0、
# buffered≈32768、flags=0x1（曲中）/0x5（曲尾），flags bit2 表示写入端已排空
# sdcard ls 输出 "FILE <字节数> <文件名>"，可用于核对网页上传是否完整
heap | reboot
```

`settings show` 只打印脱敏后的凭证。真实豆包 Key 通过 CLI、NVS 或后续配网页面写入，
不要提交到仓库。

## 实体按键

| 按键 | 操作 | 功能 |
|---|---|---|
| BOOT (GPIO0) | 长按 4s | 进入/退出配网模式（开启 SoftAP + 配网页，再按 4s 关闭）；短按提示 |
| 触摸屏 | 长按 600ms | 按住说话（PTT），松开提交；BOOT 不再触发 PTT |
| KEY1 (TCA9555 P1.1) | 短按 / 按住 | 音量 −10%（按下即生效，按住每 250ms 重复；MP3 播放中同样立即生效；保存 NVS） |
| KEY2 (TCA9555 P1.2) | 短按 / 按住 | 音量 +10%（同上） |
| KEY3 (TCA9555 P1.3) | 短按 | 切换菜单：快捷设置→人格设置→睡眠设置→系统设置→设备信息→时间天气→音乐播放 循环 |
| KEY3 | 长按 600ms | 返回主屏（退出菜单） |

KEY1–3 经 TCA9555 轮询（20ms），只有 BOOT 能作为深睡唤醒源。
菜单页与触摸手势共用同一批生成页面；语音/错误/低电等紧急状态仍会抢占菜单。

## RGB 灯环

7 颗 WS2812（GPIO38）默认 `full` 模式、亮度 45%，按设备状态自动变化：

| 状态 | 效果 |
|---|---|
| 待机（联网） | 薄荷绿缓慢呼吸 |
| 配网热点 / 连接中 | AP 蓝色呼吸；STA 连接中琥珀色跑马灯 |
| 未联网 / 离线 | 琥珀色呼吸；离线为 1.6s 周期闪烁 |
| 聆听 / 思考 / 说话 | 青色行波；紫色跑马灯；暖白音量柱（TTS 与 MP3 共用） |
| MP3 播放 / 暂停 | 每首不同色相的音量柱（随电平起伏）/ 暗琥珀呼吸 |
| 错误 | 红色双闪 |
| 低电 / 充电 | 红色呼吸 / 绿色按电量填充 |
| 音量键 | 左/右三颗绿色短闪（按住重复） |
| 唤醒 / 联网成功 | 白色爆闪 / 绿色扫过一圈 |

节能处理：亮度经 gamma 2.0 与总功率预算（21 通道字节和 ≤1100）限制；帧内容
不变时跳过 RMT 刷新；任务周期按状态在 40ms（播放/语音）、120ms（待机）、
300ms（手动常亮）、1000ms（睡眠/关闭）之间切换；睡眠与关闭时灯环全灭且停止刷新。

`rgb mode` / `rgb brightness` / 手动颜色仅对本次运行生效，重启后回到 `full`、45%；
如需更改默认值改 `panda_led.c` 的 `DEFAULT_BRIGHTNESS`。灯环状态与 MP3 曲目、
语音状态、WiFi/电池事件实时联动，无需额外命令。

## 菜单功能

KEY3 打开的七个页面均可触摸操作；设置类选择写入 NVS，音乐页按钮直接控制播放。
除人格提示词在下一轮对话生效外，其余设置立即应用：

| 页面 | 行为 |
|---|---|
| 快捷设置 | Wi-Fi 开/关（无凭证时开配网 AP）、勿扰=扬声器静音、睡眠按钮进入睡眠；蓝牙 tile 隐藏（无 BT 协议栈） |
| 人格设置 | 性格（活泼/温柔/安静）、反馈强度（弱/中/强）、动画活跃度（低/中/高）；性格与反馈拼入下一轮对话提示词，动画映射切屏时长 0/150/250ms |
| 睡眠设置 | 自动休眠 1/5/10 分钟或从不；唤醒方式（触摸/语音/都可以）；睡眠动画开关。语音唤醒尚未实现，选它会提示 |
| 系统设置 | 真实亮度/音量滑块（拖动预览，松手保存）；百分比、Wi-Fi、时间同步状态周期刷新 |
| 设备信息 | 真实 SD 已用/总容量与固件版本 `v2.0.0` |
| 时间天气 | 时钟/日期随 RTC/SNTP 更新；天气来自 Open-Meteo，城市可在配网页或 `weather set` 修改 |
| 音乐播放 | 顶部为圆形音乐图标徽章（48px 音符）；扫描 SD 卡 `Music` 目录，显示歌曲名/序号与播放状态；上一首、暂停/播放、下一首、退出。退出页面即停止播放并释放解码音频资源，恢复 16kHz 语音时钟。播放使用约 0.35s 预缓冲与 10ms 淡入淡出，切换采样率只在每首开始时静音进行一次 |

自动休眠默认 300s：设备未睡、语音空闲且 5 分钟无触摸/按键/语音事件时进入睡眠；
睡眠时 RGB 灯环熄灭，触摸或按键唤醒并显示 1.2s 唤醒屏（唤醒的那次输入不会同时
触发原动作，灯环白色爆闪）。
天气成功时 30 分钟刷新，失败 60 秒重试，离线每 10 秒检查网络；图标只有晴/多云/阴三类。

## 配网页、WiFi 与音乐上传

配网页与设备共用同一个 HTTP 服务（端口 80）：设备联网（SoftAP 或 STA 获得 IP）时
自动运行，断网（AP 与 STA 都不可用）时自动停止。STA 在线时直接访问
`http://<设备IP>/`（启动日志 `provisioning ready: http://...` 会打印地址），
也可 `wifi ap on` 或长按 BOOT 4s 开热点后访问 `http://192.168.4.1/`；用完后
`wifi ap off` 关闭热点。

- **多个 WiFi**：页面列出全部已保存网络（最多 4 个），可删除或新增；
  新增时点“扫描附近 WiFi”从列表选择。设备启动/重连时扫描所有信道，
  在已保存网络中选信号最强的接入点连接；扫描失败会沿用上次成功的凭证重试，
  不会误进配网模式。一个都连不上（或无凭证）才开启热点。
- **热点二维码**：热点开启且 STA 未连接时，屏幕显示 `Panda-AI-XXXX` 的二维码
  （含密码），手机扫码加入后访问 `http://192.168.4.1/`；页面顶部也会显示热点名与地址。
- **网页上传音乐**：页面“SD 卡音乐”区列出卡上曲目，可选择 MP3 上传到
  `/sdcard/Music`（单文件 ≤ 20MB，只接受 `.mp3`，中文文件名原样保留），
  带上传进度，完成后自动刷新曲目列表。实测 2GB SDSC 卡约 330–370KB/s，
  10MB 歌曲约 30 秒；播放 MP3 或频繁操作界面时会变慢。
- 页面还可修改音量/亮度/天气城市/人设、豆包语音参数，以及删除已保存的 WiFi。
  凭证只在 NVS 中保存，页面不回显（留空表示保持原值）。

## UI 架构

- `components/panda_ui/generated/` 来自 `demo/ui` 的 SquareLine 导出，已按屏幕重命名冲突符号
  （例如 `ui_home_face`、`ui_listening_event_face`），并修复 `thinkdot1_Animation` 大小写。
- `components/panda_state/` 实现 PRD 8.3 的三维状态机：设备、语音、情绪相互独立，
  屏幕优先级 `sleep > low_battery > error > wake > offline > voice > emotion > idle`，
  并带 wake/情绪/错误/连接/聆听/思考/取消超时。
- `panda_ui.c` 只做映射：把状态机输出的屏幕枚举切到 37 个生成屏之一，并维护
  `lv_layer_top()` 上的状态、字幕、toast overlay；设置/通知/预览页不会被普通状态抢占。
- 字幕 pill 固定显示最多 2 行（40px 文本区）：短文本居中显示，超过 2 行时以 12px/s
  自下而上循环滚动，新字幕到来会先复位动画。字幕框宽 200px、位于屏幕下 1/4
  （y=270..318），四角留在圆形可视区内；字幕出现时状态 pill 自动隐藏，
  带底部提示的屏幕（离线/重连/报错/低电）也不再叠加状态文字。
- 屏幕导航（上下左右滑动、菜单点击）由生成 UI 负责；`app_core` 处理
  双击电量提示、长按 PTT、BOOT 键、菜单钩子、自动休眠与系统周期任务，避免手势重复消费。
- 图片全部编译进固件；`ui images` 遍历 37 屏图片对象并调用 LVGL 解码器校验，
  主机侧 `tools/ui_sim/check_images.py` 逐字节校验描述符与像素数据。

## 已知限制

- 真机当前连接电池、SD 卡、1.85" 屏、喇叭与触摸。CST816S 会自动进入低功耗导致
  坐标读取 NACK，固件已写 `0xFE=0x01` 关闭自动休眠，长按 PTT 与 `touch diag` 实测正常；
  单击/双击/滑动与休眠唤醒仍待逐项验收。
- 已实测：I2C0/TCA9555、RTC 读写与跨复位保持、SD 挂载、RGB、ES8311/ES7210 初始化、
  麦克风采集、WiFi STA/SoftAP/SNTP、配网 HTTP（首页/状态/保存）、
  JSON 云端语音（`voice clip`→TTS→回复文本）与 `voice cancel` 打断。
  RGB 灯环的待机/播放/暂停/音量/睡眠/唤醒逻辑已用串口计数验证（播放约 20 次/秒刷新、
  暂停后电平归零、睡眠后刷新停止），默认 45% 亮度下的颜色与亮度观感待用户目视验收。
- 进入音乐页时同步扫描 `Music` 目录（46 首），约 1.5–2s 内界面会停顿一次
  （日志 `loop stall ~1.5–2s in voice-ui`），播放本身不受影响。
- 状态机与 UI 映射已通过主机 46 项断言和真机切屏验证；58 张编译图片经主机解析校验
  与真机 LVGL 解码器校验（37 屏/71 对象/58 唯一/0 失败）。屏已接，实际像素/色彩/帧率待逐屏验收。
- 语音已实现 demo 的 JSON 链路（会话、PCM 上行、TTS 播放、24k→16k 重采样、打断）；
  TTS 播放按 demo 改为多相重采样、500ms 预缓冲、启动/续播淡入、停顿淡出、暂停回滞与
  700ms 静音冲刷，日志每 5s 输出 `play loop: ring/peak/nearclip`；ASR 只渲染最终转写，
  避免 delta 重复叠加。可用 `voice stats` 观察 `underruns`/`ring_drops`/`rx_overflows`；
  官方 O2.0 二进制协议未实现，需要控制台的 app_id/access_key。
- WiFi STA/SoftAP/SNTP 与配网网页已接入：无凭证时开机 SoftAP；有凭证但 20s 未联网时
  自动回退 SoftAP（首页显示断线原因）。配网页在 AP 或 STA 任一联网时运行，
  STA 在线可直接访问 `http://<设备IP>/`，网络全断时自动停止；STA 恢复后自动回退的
  热点会在 5s 后关闭（给在途响应留时间，用户手动 `wifi ap on` 的热点不会自动关）。
  配网页支持扫描周边 WiFi 并直接选择，只改密码也会触发重连。
- 进入配网：热点 SSID 为 `Panda-AI-XXXX`（`Panda-AI-` + MAC 末两字节），默认密码
  `pandapanda`（`CONFIG_PANDA_WIFI_AP_PASSWORD`），手机连上后打开
  `http://192.168.4.1/`。触发方式：无凭证开机、WiFi 20s 连不上自动回退、
  运行时长按 BOOT 4s、或串口 `wifi ap on`。固件每秒自检 AP/HTTP 状态，
  页面意外退出会自动重启；SoftAP 在 STA 未连接时固定信道 1，避免部分手机不支持
  12/13 信道而打不开页面。
- WiFi 已关闭 modem sleep，重连采用 1s→30s 指数退避；长时间掉线仍可能因路由器/信号
  原因发生，`wifi status` 的 `reason` 可辅助定位。
- 已处理“假在线”卡死：驱动仍显示有 IP，但 ARP/TCP 全部失败且没有 disconnect 事件。
  语音 WS 或天气在 90s 内累计 3 次出站失败会自动触发恢复（先 disconnect+connect，
  20s 无 IP 再 `wifi stop/start`，45s 冷却），日志为
  `network wedge suspected: recovery 1/2 ...`；也可手动 `wifi recover`。
  这是缓解而非根除：恢复期间正在进行的那轮语音回复会丢失，需重新说一次。
  另已修复三类“误判断线”导致的会话中断：组件 ERROR 事件不再被当作断线；
  天气在内部堆不足时不再上报网络错误（不会误触发 WiFi 恢复）；
  WS 发送 poll 超时只丢帧、不再 abort 会话。WiFi/TCP 缓冲已与厂商 demo 对齐
  （TCP 窗口 5.7KB、WS buffer 4096）。真机 12/12 轮合成语音全链路通过、0 丢包。
- 喇叭已接；抗爆音/抗卡死策略已烧录。内置 `voice clip` 可完整走通
  ASR→回复→TTS 播放（实测转写“你是谁呀你叫什么名字”），真人语音的听感验收待完成。
- MP3 播放已改为“解码任务 + I2S 写入任务 + 32K 样本 PSRAM 环形缓冲”：16K 样本
  预缓冲（44.1k 约 0.37s）、起播/续播淡入、曲尾淡出；文件采样率与语音时钟不同时，
  只在每首开始、写入端空闲且静音时切换一次（支持 22.05/24/32/44.1/48kHz），
  播放中不再重配 codec。实测 44.1k 与 48k 曲目 `underruns=0`，用户确认无卡顿/爆音。
  曲目如有损坏或截断（SD 上残留的 `64k.mp3`/`test1m.mp3` 测试文件），播到末尾会
  自动跳下一首，可在配网页删除。
- 语音连接被服务端以 `403` 拒绝时（日志 `Sec-WebSocket-Accept not found`、
  `handshake_status_code=403`），界面保持离线。除了 Key 失效/轮换，
  **账号欠费**也会以 `45000030 resource not granted` 的形式返回 403；
  已在 2026-11-11 实测：充值后同一 Key 立即恢复（`101 Switching Protocols`）。
- 误触/静音长按：PTT 按下后的 120ms 爆音会被丢弃，且需累计约 320ms 有效语音才
  提交回合，因此轻触仍按误触处理。设备会显示“我在呢～没听到你说话，再说一次好吗？”
  并通过 `speech_text_buffer.append/commit` 请求服务端合成语音提醒；
  连续误触有 3s 冷却，静默期间的服务端兜底文本/错误/TTS 一律抑制。
  提示若 10s 无响应会自动回到待机，不会出现“服务器开小差”错误屏。
- 电池电量：固件已按原理图使用 GPIO1/ADC1_CH0、分压 ×3，并带接线自检；
  但本板 `R53(1-3)` / `R78(1-2)` 两个 0R 选择跳线默认未焊，GPIO1 悬空，
  `status`/`battery` 显示 `unknown`。补焊后即可读数；注意插 USB 时
  `BAT_OUT` 约 5 V，会显示 100%，拔掉 USB 才是真实电池电压。
  补焊后 GPIO1 不能再作摄像头 HREF。配置项为 `CONFIG_PANDA_BATTERY_ADC=y`。
- KEY3 七个菜单页已在真机逐页确认，`key 3` 连按可依次到达“音乐播放”；
  KEY1/2 音量键仍建议用 `key 1|2|3 [long]` 做串口预演确认物理顺序。
- 菜单功能（滑块、Wi-Fi 开关、勿扰静音、自动休眠/唤醒、个性化持久化、SD/版本、天气、
  音乐播放）已在真机操作；屏幕/触摸/喇叭均已连接。
- 语音唤醒未实现：睡眠设置里的“语音唤醒”只会提示暂不支持，触摸/按键仍可唤醒。
- 固件未链接蓝牙协议栈，快捷设置的蓝牙入口已隐藏；需要 BT 配网/音频需另行评估。
- 天气使用 Open-Meteo（无需 Key），需要联网；只有晴/多云/阴三种图标，尚未取到数据时显示 `--`，
  取数失败保留上一次结果。
- 自动休眠只是 UI 睡眠，Wi-Fi 与语音连接仍在线，不做深睡省电。
- O2.0 控制台权限/模型版本待确认；JSON 链路已用现有 api_key 跑通。
- 内部 RAM 启动后约 114KB（实测 `internal=114439`，最大块 63KB）；进入音乐页/播放时
  约 84KB（最大块 48KB），上传时约 94–103KB。LVGL 的 96KB widget 池与 MP3 曲目表
  已移到 PSRAM（`LV_MEM_POOL_ALLOC`），音频/JSON/环形缓冲仍必须放 PSRAM；
  天气 HTTPS 与语音 WS 并发时若内部堆不足会跳过本轮刷新（日志 `fetch skipped`）。
  RGB 灯环任务另占 3KB 内部栈（core0）。
- SD 卡为 2GB SDSC（`SA02G`，1-bit/20MHz，实测写约 330–370KB/s）。上传大文件时
  不要把卡拔出；写到一半复位会留下 0 字节文件（目录项已创建、数据未落盘），
  可用 `sdcard ls` 核对字节数并重新上传。20MB 单文件上限由 `/upload/music` 强制执行。
