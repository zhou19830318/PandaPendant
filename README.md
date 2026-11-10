# Panda AI 熊猫智能挂坠

基于 Waveshare ESP32-S3-AUDIO-Board 的桌面/挂坠式 AI 语音玩具固件。
产品需求见 [Panda_AI_熊猫智能挂坠_PRD_V1.0.md](Panda_AI_熊猫智能挂坠_PRD_V1.0.md)，
可行性结论见 [docs/feasibility.md](docs/feasibility.md)。

**协作开发（多模型接力必读）**：[AGENTS.md](AGENTS.md) 规定了开工/收工流程，
当前进度与交接记录见 [docs/PROGRESS.md](docs/PROGRESS.md)，任务清单见 [docs/TODO.md](docs/TODO.md)，
已验证的坑位见 [docs/tech_notes.md](docs/tech_notes.md)。

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

当前产物约 8.0MB（`0x804540`，固件版本 2.0.0），factory 分区剩余约 20%。

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
status                          # 固件/UI/电池/堆状态
ui state listening              # idle|connecting|listening|thinking|speaking|offline|error
ui emotion happy                # neutral|happy|surprise|sad|sleepy|curious|shy|angry
ui error asr                    # asr|llm|tts|audio|network|auth|generic|clear
ui images                       # 37 屏图片解码自检（无屏可用）
ui status 正在连接              # 顶部状态 pill
ui subtitle 你好呀              # 字幕 pill（ASR 在最终转写完成后显示一次）
ui toast 测试提示               # toast
ui audio 70                     # 模拟音频电平 0..100
touch tap|double|long|left|right|up|down
touch diag                      # 触摸面板原始读检查（ID/坐标寄存器/自动休眠）
key 1|2|3|boot [long]           # 注入按键动作（boot long = 切换配网模式）
brightness [0..100]
volume [0..100]
battery
time [get] | time set <epoch>
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
rgb <r> <g> <b> | rgb off
i2c | sdcard status|mount|unmount | rtc [write <epoch>]
heap | reboot
```

`settings show` 只打印脱敏后的凭证。真实豆包 Key 通过 CLI、NVS 或后续配网页面写入，
不要提交到仓库。

## 实体按键

| 按键 | 操作 | 功能 |
|---|---|---|
| BOOT (GPIO0) | 长按 4s | 进入/退出配网模式（开启 SoftAP + 配网页，再按 4s 关闭）；短按提示 |
| 触摸屏 | 长按 600ms | 按住说话（PTT），松开提交；BOOT 不再触发 PTT |
| KEY1 (TCA9555 P1.1) | 短按 | 音量 −10%（保存 NVS，播放中立即生效） |
| KEY2 (TCA9555 P1.2) | 短按 | 音量 +10% |
| KEY3 (TCA9555 P1.3) | 短按 | 切换菜单：快捷设置→个性化→睡眠设置→系统设置→存储与版本→时间天气 循环 |
| KEY3 | 长按 600ms | 返回主屏（退出菜单） |

KEY1–3 经 TCA9555 轮询（20ms），只有 BOOT 能作为深睡唤醒源。
菜单页与触摸手势共用同一批生成页面；语音/错误/低电等紧急状态仍会抢占菜单。

## 菜单功能

KEY3 打开的六个页面均可触摸操作，选择会写入 NVS；除人格提示词在下一轮对话生效外，
其余设置立即应用：

| 页面 | 行为 |
|---|---|
| 快捷设置 | Wi-Fi 开/关（无凭证时开配网 AP）、勿扰=扬声器静音、睡眠按钮进入睡眠；蓝牙 tile 隐藏（无 BT 协议栈） |
| 个性化 | 性格（活泼/温柔/安静）、反馈强度、动画活跃度；性格与反馈拼入下一轮对话提示词，动画映射切屏时长 0/150/250ms |
| 睡眠设置 | 自动休眠 60s/300s/600s/关闭；唤醒方式（触摸/语音/两者）；睡眠动画开关。语音唤醒尚未实现，选它会提示 |
| 系统设置 | 真实亮度/音量滑块（拖动预览，松手保存）；百分比、Wi-Fi、时间同步状态周期刷新 |
| 存储与版本 | 真实 SD 总容量/剩余空间与固件版本 `v2.0.0` |
| 时间天气 | 时钟/日期随 RTC/SNTP 更新；天气来自 Open-Meteo，城市可在配网页或 `weather set` 修改 |

自动休眠默认 300s：设备未睡、语音空闲且 5 分钟无触摸/按键/语音事件时进入睡眠；
触摸或按键唤醒并显示 1.2s 唤醒屏（唤醒的那次输入不会同时触发原动作）。
天气成功时 30 分钟刷新，失败 60 秒重试，离线每 10 秒检查网络；图标只有晴/多云/阴三类。

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
- 状态机与 UI 映射已通过主机 46 项断言和真机切屏验证；58 张编译图片经主机解析校验
  与真机 LVGL 解码器校验（37 屏/71 对象/58 唯一/0 失败）。屏已接，实际像素/色彩/帧率待逐屏验收。
- 语音已实现 demo 的 JSON 链路（会话、PCM 上行、TTS 播放、24k→16k 重采样、打断）；
  TTS 播放按 demo 改为多相重采样、500ms 预缓冲、启动/续播淡入、停顿淡出、暂停回滞与
  700ms 静音冲刷，日志每 5s 输出 `play loop: ring/peak/nearclip`；ASR 只渲染最终转写，
  避免 delta 重复叠加。可用 `voice stats` 观察 `underruns`/`ring_drops`/`rx_overflows`；
  官方 O2.0 二进制协议未实现，需要控制台的 app_id/access_key。
- WiFi STA/SoftAP/SNTP 与配网网页已接入：无凭证时开机 SoftAP；有凭证但 20s 未联网时
  自动回退 SoftAP（首页显示断线原因）。配网页服务只在 SoftAP 生效时运行，热点关闭即停止，
  平时不占用射频/任务；STA 恢复后自动回退的热点会在 5s 后关闭（给在途响应留时间，
  用户手动 `wifi ap on` 的热点不会自动关）。配网页支持扫描周边 WiFi 并直接选择，
  只改密码也会触发重连。STA 在线时若要再进配网页，用 `wifi ap on`，用完 `wifi ap off`。
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
- 电池电量：固件已按原理图使用 GPIO1/ADC1_CH0、分压 ×3，并带接线自检；
  但本板 `R53(1-3)` / `R78(1-2)` 两个 0R 选择跳线默认未焊，GPIO1 悬空，
  `status`/`battery` 显示 `unknown`。补焊后即可读数；注意插 USB 时
  `BAT_OUT` 约 5 V，会显示 100%，拔掉 USB 才是真实电池电压。
  补焊后 GPIO1 不能再作摄像头 HREF。配置项为 `CONFIG_PANDA_BATTERY_ADC=y`。
- KEY1–3 的音量/菜单映射已接入并编译通过，但尚未在真机上确认按键物理顺序；
  可先用 `key 1|2|3 [long]` 做串口预演。
- 菜单功能（滑块、Wi-Fi 开关、勿扰静音、自动休眠/唤醒、个性化持久化、SD/版本、天气）
  已编译通过并通过主机检查，但屏幕/触摸未接，实际布局、点击区与交互尚未真机验证。
- 语音唤醒未实现：睡眠设置里的“语音唤醒”只会提示暂不支持，触摸/按键仍可唤醒。
- 固件未链接蓝牙协议栈，快捷设置的蓝牙入口已隐藏；需要 BT 配网/音频需另行评估。
- 天气使用 Open-Meteo（无需 Key），需要联网；只有晴/多云/阴三种图标，尚未取到数据时显示 `--`，
  取数失败保留上一次结果。
- 自动休眠只是 UI 睡眠，Wi-Fi 与语音连接仍在线，不做深睡省电。
- O2.0 控制台权限/模型版本待确认；JSON 链路已用现有 api_key 跑通。
- 内部 RAM 启动后约 19KB 稳定（对齐 demo 的 WiFi 缓冲后比早期 ~31KB 更低，
  TLS 峰值最低约 10KB），音频/JSON/环形缓冲/天气缓冲必须放 PSRAM；
  天气 HTTPS 与语音 WS 并发时内部堆偏低会跳过本轮刷新（日志 `fetch skipped`）。
