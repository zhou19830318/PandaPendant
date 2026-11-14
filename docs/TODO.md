# Panda AI 开发 TODO / 进展

> 协作规则见根目录 [AGENTS.md](../AGENTS.md)：开工前读 [PROGRESS.md](PROGRESS.md)，收工前必须验证并更新
> 本文件、`PROGRESS.md`、`tech_notes.md`。`[x]` 只用于有验证证据的项。
> 当前硬件：串口 `/dev/ttyACM0` 已连接；电池 + SD 卡在位；屏幕、触摸、喇叭已连接
> （2026-11-10 实测：ST77916/CST816S/I2S 均正常）；电池 ADC 的 R53/R78 0R 未焊。
> 构建与验证基线见 `PROGRESS.md` 第 6 节。

## 状态图例

- [ ] 未开始
- [~] 代码完成/编译通过，尚未完整验证（须写明缺口）
- [x] 已完成并有验证证据
- [!] 阻塞

## 当前构建状态（2026-11-14）

- `idf.py build` 通过：`panda_ai.bin` 0x835dd0（约 8.21MiB），factory 10MB 剩余 18%
  （0x1ca230）；`project(panda_ai VERSION 2.0.0)`；已烧录 `/dev/ttyACM0`。
- 新增 `CONFIG_LV_FONT_MONTSERRAT_48=y`（`sdkconfig.defaults`）供音乐页 48px 图标，
  bin 因此增加约 96KB。
- 配网页按需运行：SoftAP 或 STA 任一联网时启动，网络全断时停止；STA 在线可直接
  访问 `http://<设备IP>/`，也可 `wifi ap on` 后访问 `192.168.4.1`。
- 串口 CLI 已接入：status/ui/touch/brightness/volume/battery/time/settings/voice/audio/weather/rgb/i2c/sdcard/rtc/wifi/heap/reboot；
  `status` 现包含 `reset : poweron|sw|panic|int_wdt|task_wdt|wdt|brownout|deepsleep|unknown`；
  `ui error asr|llm|tts|audio|network|auth|generic|clear` 可注入/清除错误屏；
  `ui images` 可无屏检查 37 屏的图片解码（37 屏/71 对象/58 唯一/0 失败）；
  `voice stats` 输出 WS/TTS 计数器，`wifi status` 增加 `ap_auto`/`reason`/`retry`；
  `weather [show|refresh|set <city>]` 查询/刷新/设置天气城市；
  `audio tone [seconds]` 连续 880Hz 音；`voice pump on|off` 开关上行静音泵（诊断用）；
  `mp3 status|list|play [index]|toggle|next|prev|stop`，`mp3 status` 额外输出
  `audio_rate/underruns/buffered/flags`（flags：bit0 primed、bit1 eof、bit2 idle、bit3 abort）。

## 本轮：音乐播放期间不自动深睡（2026-11-14，已烧录/已验证）

- 需求：播放音乐时设备不算空闲，不应进入自动深睡。
- [x] `app_core` 1Hz 逻辑：`panda_mp3_state()==PLAYING` 时刷新 `s_last_active_us`
  并跳过自动休眠判定，避免“放完一首正好超时立刻睡”。
- [x] 已排队的自动休眠在 1.5s 预备期到点时若检测到音乐播放则取消
  （`auto deep sleep deferred: music playing`）；`menu` 立即休眠不受影响。
- [x] 构建 `0x835dd0`（18% free），已烧录。
- [x] 验证 1：`settings set auto_sleep_s 60` + `mp3 play 1`，连续播放 75s，
  `uptime 75 s` 仍在运行，无 `auto sleep`/深睡日志；随后恢复 300s 并停止。
- [x] 验证 2：空闲等满 60s 触发 `auto sleep after 60s idle` + 1.5s 预备，
  期间 `mp3 play 1` → 日志 `auto deep sleep deferred: music playing`，
  设备继续运行（`uptime 162 s`）。

## 本轮：菜单切页卡死诊断与自恢复（2026-11-14，已烧录/待复现）

- 用户现象：多功能键（KEY3）切页时卡在“时间天气”，屏幕与按键无响应。
- 串口复核：卡死时 `ui menu next` 无返回、后续 `status` 不执行，而
  `voice: uplink ... (silence)` 持续打印 → 串口 CLI 阻塞在 `ui_lock`（显示递归锁
  被某任务长期占用）。复位后连翻七页正常，非确定性。
- [x] `panda_display` 增加持锁追踪：`panda_display_lock_at(timeout, site)` 记录
  任务/位置/时长；超时与单次持锁 >500ms 打日志；`panda_display_lock_info()` 可查。
- [x] `panda_ui` 看门狗：3s 拿不到锁打印 `display lock stuck 3s (caller=...):
  holder=... site=... held=...ms; lvgl ticks=N stopped Mms ago`；再等 7s 仍失败
  则 `esp_restart()` 自恢复；50ms 状态定时器维护 LVGL 心跳计数。
- [x] `esp_lcd_st77916.c`：`tx_color` 返回值不再忽略，失败打 ERROR（丢一次颜色
  传输会让 flush 永不完成、屏幕静默冻结）。
- [x] `status` 增加 `reset : ...`，用于判断卡死是否伴随复位。
- [x] 构建 `0x835d30`（18% free），已烧录。
- [x] 压测：`ui menu next` 连翻 100 次；混合 next/prev/home 快速 300 次
  （40–90ms，含音乐页自动起播/离页停止）→ 无卡死；最长锁等待 262ms
  （`panda_ui_is_music_page` 300ms 轮询，属正常）；无 `stuck`/`tx_color failed`。
- [x] `key 3` 注入真实按键处理路径 140 次（30–120ms 连击，混短按/长按回主屏/
  音量键）：页面按序循环，短按处理最长 8ms（混合含音乐页 106ms），无 `stuck`。
- [~] 根因定位：待用户按 KEY3 复现并保留串口日志。判断规则：
  `holder=lvgl-task` 且 `lvgl ticks` 停走 → 查 LVGL 回调/QSPI flush；
  `holder=<任务名> site=<panda_ui 函数>` → 查该函数持锁期间的阻塞调用。
- [~] 触摸幻触复查：早前一次 `status` 见 `gestures=8 taps=0`，但静置 10s 复查
  `gestures=0` 且无新增，暂不作为结论；若复现日志出现无人工操作的
  `touch action swipe_*`，再查 CST816S 坐标抖动与 45px 阈值。

## 本轮：C 阶段情绪标签探针（2026-11-14，已回退/结论不可用）

- 目标：LLM 回复带火山内联 TTS 标签 `{{"audio_params":{"emotion":"happy",...} }}`，
  `output_text.done` 解析后映射到 `panda_emotion_t` 并切表情屏。
- [~] 标签能收到：设备实收为两括号（缺内层 `}`），解析器已做兼容；host 测试 8/8。
- [!] 端到端实时模型（1.2.6.1）把标签当普通文本朗读：同一问题 A/B，
  无标签 `audio=5154/5974ms`，标准三括号标签 `audio=10971/12550ms`。
  → 标签方案不能上线，相关指令与解析代码已移除。
- [x] 回退后构建 `0x832910`（18% free），烧录验证 `audio=5385ms`，回复无标签。
- [x] 保留 C2 状态机能力并验证：`panda_state` 增加 `TURN_EMOTION_MS=60s`
  （语音回合中设置的表情不按 2.5s 过期），`VOICE_SPEAKING` 时表情屏优先；
  `panda_ui` 紧急判定包含 `VOICE_SPEAKING`。真机说话中 `ui emotion happy` →
  `screen speaking -> face_happy`，字幕保持，播放结束回 idle。
- [ ] 正式情绪通道：等待 O2.0 二进制协议或服务端 `emotion` 事件（唯一可靠来源）；
  在此之前 C 阶段暂停，先做现有 `[~]` 项的目视验收。

## 本轮：隐藏设备信息页三个演示入口（2026-11-14，已烧录/待目视）

- 决策：先走 B（隐藏入口），后续再走 C（情绪驱动表情）。调研结论见
  `tech_notes.md` 第 25 节：三页是 SquareLine 演示页，数据写死且点预览会被
  50ms 状态收敛拉回。
- [x] `panda_ui.c` 的 `create_menu_controls()` 隐藏 `ui_link_prev/exp/anim`；
  屏幕对象保留，便于 `ui images` 继续校验 37 屏，后续做真实预览时可复用。
- [x] 构建：`idf.py build` → 0x8327f0，剩余 18%（0x1cd810），已烧录。
- [x] 串口冒烟：`ui images` → 37 屏/71 对象/57 唯一/0 失败；`ui menu next`×7
  仍按快捷→人格→睡眠→系统→设备信息→时间天气→音乐播放循环，`ui menu home`
  正常；无 panic/assert。
- [~] 目视验收：需用户确认设备信息页下半区三个胶囊已消失；该区域留白，待 C
  阶段填入真实情绪/预览状态。
- [ ] C 阶段（暂停，见上一条“情绪标签探针”）：情绪标签方案已证伪；
  需等 O2.0/服务端情绪事件。预览期间暂停状态机收敛；入口放回也依赖此通道。

## 本轮：天气城市 + 信息去重 + 左右滑动切页（2026-11-14，已烧录/待目视）

- 需求：天气/温度前增加地区；各菜单页不重复显示同一信息；触摸左右滑切换上一页/下一页。
- [x] 时间天气页新增城市：位于天气图标左侧、与温度同一行；新增
  `panda_ui_set_weather_city()`，`app_weather` 取数成功时写入城市，
  `app_core_settings_changed()` 启动/改设置时先用缓存/配置城市填充（离线也显示）。
- [x] 地区中文显示：geocoding 已带 `language=zh`，`parse_geocode()` 现在解析
  `results[0].name`（如 Changzhou→常州市）并作为显示名；英文/拼音查询可命中，
  中文“常州”直接查询 Open-Meteo 无结果，因此保留配置原文、单独缓存中文显示名。
  中文名写入 NVS `wx_label`，重启/离线也显示中文；CLI `weather set`、
  `settings set weather_city` 与配网页改城市时清空缓存，下一次取数重填。
- [x] 整行居中：城市/图标/温度放进透明 flex 行（300×30，y=144，flex center，
  pad_column=6），城市宽度改为 `LV_SIZE_CONTENT`，换城市名时自动重新居中；
  图标与温度 reparent 进该行，温度宽度同样改为自适应。
- [x] 去重：
  - 快捷设置删除“亮度/音量”tile（系统设置已有可操作滑块与百分比；音量另有 KEY1/2），
    剩余 Wi-Fi/勿扰/睡眠 三个 tile 重排为一行（x=-86/0/86，图标 y=-20、文字 y=34）。
  - 系统设置删除“网络 Wi-Fi 已连接”行（快捷设置 Wi-Fi tile 已显示状态），
    “时间同步”上移到原网络行的 y=24。
- [x] 菜单左右滑切页：`panda_ui` 新增 `menu_step(delta)` 与 `panda_ui_menu_prev()`；
  七个菜单页移除 SquareLine 生成的固定手势回调（右滑回首页/回固定页），
  改为统一 `menu_gesture_cb`：左滑=下一页、右滑=上一页（循环）、上滑=回主屏。
- [x] 修复手势分发前置条件（关键）：LVGL 9.5 的 `indev_gesture()` 从按下对象
  向上爬，只要对象带 `LV_OBJ_FLAG_GESTURE_BUBBLE` 就继续，最终把事件发给
  第一个不带 flag 的对象。原 `add_gesture_bubble()` 把屏幕根节点也加上了 flag，
  爬升越过屏幕到 NULL → 所有生成页/菜单页手势实际都收不到。现改为只给子对象
  补 flag、目标屏幕保持不带；音乐页同样由子控件冒泡到页面根节点。
- [x] 修复中文名持久化崩溃：首次实现直接在天气任务里 `panda_settings_save()`，
  该任务栈由 `xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM)` 分配，NVS 读
  flash 时禁用 cache 导致 PSRAM 栈不可访问，触发
  `assert esp_task_stack_is_sane_cache_disabled()` 并 coredump。现改为天气任务只把
  中文名放进 `s_label_pending`，由 app_core 任务（内部栈）每秒消费并通过
  `app_weather_take_city_label()` 持久化。
- [x] 构建：`idf.py build` → 0x8327C0，factory 剩余 18%（0x1cd840），已烧录
  （含中文地名、flex 居中与崩溃修复的最终镜像）。
- [x] 主机校验：`make -C tools/ui_sim test` 46/46 通过；`check_coverage.py` PASS
  （CJK16 8371 字形，UI 非 ASCII 395 字）。
- [x] 串口验证（烧录后）：
  - `ui menu next` ×8：快捷→人格→睡眠→系统→设备信息→时间天气→音乐播放→快捷（循环）；
  - `ui menu prev` ×2：从音乐播放回时间天气、再到设备信息；`ui menu home` → 主屏；
  - `key 3` 仍从主屏进入快捷设置并继续到人格设置（单次 1ms）；
  - 天气在线取数：`weather: Changzhou 18°C 毛毛雨 (code 51)`，城市字符串已送入 UI；
  - `touch left|right` 注入只打印 `touch action swipe_*`，页面不切换
    （`app_core` 不消费滑动，避免与 LVGL 手势双触发）。
- [x] 手势修复后重新烧录冒烟：开机 4.5s 内 `ui ready (37 screens)`、触摸就绪；
  `ui menu next` → `menu: 快捷设置`，`ui menu prev` → `menu: 音乐播放`
  （同时进入音乐页、起播后退出停止），`ui menu home` → `menu: home`；
  `weather show` 返回 `no data yet (city=Changzhou)`（当时 WiFi 未连上，
  城市来源已就位）。
- [x] 中文地名与持久化（最终镜像）：
  - `weather: 常州市 18°C 毛毛雨 (code 51)`；`weather show` →
    `weather: 常州市 18°C 毛毛雨 code=51`；
  - `app: weather city label cached: 常州市`；`settings show` →
    `weather_city=Changzhou label=常州市`；
  - 复位后开机日志 `weather_city=Changzhou label=常州市`（离线/重启仍是中文）；
  - 修复后连续运行 >40s 无 panic/assert。
- [~] 视觉/触控验收：城市/图标/温度整行居中效果、三 tile 布局、系统设置空档、
  真实左右滑是否切页且不误触按钮，需用户目视/上手确认。左右滑目前的证据只到
  “事件能到达屏幕”的源码路径（`lv_indev.c:1781/1815`），真手指滑动待用户验证。
- 未验证/风险：城市用 `ui_font_CJK16`（16px），与右侧 18px 温度有 2px 字号差；
  极长地名（>6 字）在圆边可能贴边，目前不截断；滑动手势依赖 LVGL 手势阈值（约 50px），
  与 `panda_touch` 自身 45px 判定并存，`app_core` 不消费左右滑以避免双触发。
- 阻塞：无（仅剩视觉/触控人工验收）。

## 本轮：真深睡 + BOOT 唤醒（2026-11-14，深睡入口已验证，唤醒待按键）

- 需求：休眠改为真深睡、屏幕灭、只留一颗灯、按键唤醒。
- 硬件结论：KEY1–3 与触摸都接在 TCA9555（I2C 扩展）上，板上没有 INT 线到 ESP32；
  深睡时 I2C 与数字域断电，只能靠 RTC GPIO 唤醒 → **唯一可用唤醒键是 BOOT(GPIO0)**。
  WS2812 在 GPIO38（非 RTC GPIO），深睡期间无法刷新 → 呼吸动画物理上做不到，
  改为睡前锁存第 1 颗极暗绿灯（数据线 hold 低）。
- [x] `panda_board`：`panda_board_rgb_sleep_indicator()` 锁存暗灯、detach RMT、
  GPIO38 hold 低；`rgb_init` 释放上次深睡留下的 hold。
- [x] `panda_display`：`panda_display_prepare_deep_sleep()` 面板 off、背光 0、
  GPIO5 hold 低；`display_init` 释放 hold。
- [x] `panda_led`：`panda_led_prepare_deep_sleep()` 用互斥锁冻结动画任务后再写
  暗灯，避免睡眠态 1s 清屏把指示灯覆盖掉。
- [x] `app_core`：菜单睡眠/自动休眠/`ui sleep` 统一走
  `app_core_request_deep_sleep()`：1.5s 睡眠屏 → 停配网/WiFi/语音/MP3/PA、
  存设置、卸 SD、关屏、锁暗灯、等 BOOT 松开 → `ext0` GPIO0 低唤醒 →
  `esp_deep_sleep_start()`；开机打印 `woke from deep sleep`。
- [x] 睡眠设置页：隐藏“触摸/语音/都可以”，固定显示唯一唤醒键 `BOOT`。
- [x] 构建：`idf.py build` → 0x831f90，factory 剩余 18%（0x1ce070），已烧录。
- [x] 深睡入口真机验证（`ui sleep`）：
  `deep sleep requested (cli)` → `screen idle -> sleep` →
  `provisioning page stopped` → `STA disconnected reason=8 (left AP)` →
  `display off (backlight held low)` → `/dev/ttyACM0` 消失（USB-JTAG 随数字域断电）。
- [x] 唤醒验证（人工轻按 BOOT）：
  `main: woke from deep sleep (cause=2, BOOT)` → `boot complete` →
  `screen boot -> reconnecting` → `wifi connected ip=192.168.1.23`；
  `status` 显示 `display brightness=100%`、`rgb=ok`、`sd=mounted`，
  `max_gap=244ms`（启动基线）。短按未进下载模式。
- [~] 暗灯观感/电流：代码已锁存第 1 颗 (3,8,4) 暗绿并 hold 数据线，
  但“深睡时是否真的常亮”需用户目视确认（取决于灯带供电轨是否保持）。
  若灯灭：说明该轨随深睡断电，改回全灭并在 README 写明。
- [~] BOOT 长按边界：本次为轻按唤醒；按住跨过 ROM strapping 采样是否进下载
  模式未测。若发生，需长按复位或评估飞线 RTC GPIO/浅睡轮询方案。

## 上一轮：KEY3 切页卡死（音乐页阻塞 app_core）（2026-11-14，构建通过/待烧录）

- 根因：进音乐页时 `panda_mp3_scan()`（46 个文件、ID3 标题各读两遍）和
  `panda_mp3_play()` 在 `app_core` 任务里同步执行；离开时要 `panda_mp3_stop()`
  （写端 800ms 收尾 + 解码任务 3s 退出）。该任务同时采样 KEY1/2/3 与触摸，
  于是切页“卡死”数秒且期间按键丢失；此前日志已有“进音乐页约 2s loop stall”。
- [x] `app_core`：新增独立 `music_ctrl` 任务（prio 3、5KB、core1、300ms）负责
  扫描/播放/停止/页面刷新/灯环同步；`app_core` 主循环不再碰 SD 与音频阻塞路径。
- [x] `panda_mp3`：扫描加互斥锁（配网页上传回调与菜单页可能并发重建全局表）；
  ID3 标题改为排序后只读一遍（原先读两遍，扫描耗时减半）。
- [x] `panda_mp3`：停止后清掉 `s_done` 残留 token；`play` 在旧任务未退出时
  返回超时而不是再起一个 worker（两个 worker 共用 `s_stage`/环形缓冲会互相踩）。
- [x] 计时日志：`menu page: <页名> (N ms)`、`music page entered (... scan N ms)`、
  `mp3 play: ... (N ms)`、`music page left, playback stopped (N ms)`。
- [x] `panda_ui`：菜单内维护当前页索引，修复两次按键间隔小于一次动画启动
  tick 时第二按重复选同一页；菜单页改为 `lv_screen_load()` 即时切换（不再淡入），
  快速连按不会叠加屏幕动画、显示锁等待从 100–440ms 降到 0–22ms。
- [x] 构建：`idf.py build` → 0x82f600，factory 剩余 18%（0x1d0a00），已烧录。
- [x] 真机验证（`/dev/ttyACM0`，串口注入 `key 3`）：
  - 30 次 450ms + 40 次 120ms 连按：70 次全部按序落页（快捷→…→音乐→快捷…），
    单次处理 0–22ms；`max_gap=240ms` 为启动期电池阶段，连按期间无新增 stall。
  - 音乐页进入 `scan 821–1324 ms`（后台任务）、`mp3 play: ESP_OK (0 ms)`、
    `audio_rate=44100 underruns=0`；离开 `playback stopped 234 ms`，回到 16kHz。
  - 连按穿过音乐页：`music page left during scan; playback not started` 与
    `playback stopped (0 ms)` 正常，无残留播放。
- [!] 观察：改即时切换前的两轮极速连按中，各出现一次整机静默（串口 0 字节、
  无新 coredump、无 panic），重新复位后恢复；改为即时切换后 70 次连按未再复现。
  若复现：记录 `rst:` 原因与串口原始日志，区分屏幕淡入动画/QSPI 刷新卡死与按键逻辑。
- [x] 文档：`tech_notes.md` §21、`PROGRESS.md` 交接日志、`README.md` 行为说明。

## 上一轮：KEY3 切页去掉页面名 toast（2026-11-14，构建通过/待烧录）

- [x] `app_core` KEY3 短按只切页并保留 `menu page: <名称>` 日志，不再调用
  `panda_ui_toast(page, 800)`；长按“返回主屏”提示保持不变。
- [x] 构建：`idf.py build` → 0x82f350，factory 剩余 18%（0x1d0cb0）。
- [~] 真机验证：连按 KEY3 七个页面应直接切换、前台不再出现页面名胶囊。
  缺口：设备当前未枚举 `/dev/ttyACM0`；补验：插回后烧录，`key 3`×7 观察屏幕，
  日志仍应有 `menu page: 快捷设置…音乐播放`。
- [x] 文档：`tech_notes.md` §20 记录本改动；`PROGRESS.md` 已追加交接日志。

## 本轮：RGB 灯环状态联动（2026-11-13，真机串口验证）

- [x] `panda_board`：新增 `panda_board_rgb_ready()` 与 `panda_board_rgb_write(rgb, count)`，
  整帧 21 字节一次 RMT 刷新；`rgb_set` 复用该接口。
- [x] `panda_audio`：新增 `panda_audio_get_output_level()`（写入块峰值 0..100），
  最近 250ms 无写入即返回 0，暂停/停止后 VU 正常衰减；切采样率重开 codec 后
  重新应用静音状态。
- [x] 新增 `panda_led` 组件（core0、prio 2、栈 3KB）：待机/配网/离线/聆听/思考/
  说话/错误/低电/充电/MP3 播放与暂停各自动画；音量左/右闪、唤醒爆闪、联网扫光。
  VU 经 sqrt 感知曲线（音乐峰值普遍偏低），中点亮起并向外扩散。
- [x] 节能：亮度 gamma 2.0、总功率预算 1100（21 通道字节和）、帧不变跳过刷新、
  周期 40/120/300/1000ms（播放与语音/待机/手动/睡眠），睡眠与关闭时全灭。
  默认 `full` 模式、亮度 45%。
- [x] `app_core` 接线：语音状态、MP3 播放/暂停/曲号、WiFi 连接/AP、电池、
  睡眠/唤醒、音量键；`serial_cli` 扩展
  `rgb status|mode|brightness|<r> <g> <b>|off`，`ui sleep|wake` 同步灯环状态。
- [x] 构建：`idf.py build` → 0x82f350，factory 剩余 18%（0x1d0cb0）；已烧录。
- [x] 真机串口验证：
  - 播放：`rgb mode=full brightness=45% level=6 refresh=41` → 2s 后
    `level=2 refresh=80`（39 次刷新/2s，VU 随音乐起伏）。
  - 暂停：`mp3 toggle` 后 1.2s `level=0`，随后 2s 仅 5 次刷新（待机周期）。
  - 音量键：`key 1/2` 日志 `volume=70%/80% (audio 0–4 ms)`，灯环照常刷新。
  - 睡眠：`ui sleep` 后 3s `refresh` 增量 0、`level=0`（灯灭且停止 RMT）。
  - 唤醒：`ui wake` 后 2s `refresh` +20（呼吸恢复）。
  - 堆：`free=7179452 internal=84071 psram=7140696`，无复位/断言。
- [~] 用户目视验收：默认亮度 45% 下各状态颜色、亮度、VU 居中扩散观感。
  缺口：需人眼确认；补验：正常开机看薄荷绿呼吸 → 播放 MP3 看彩色 VU →
  按音量键看左右闪 → `ui sleep`/按键唤醒看熄灭与白闪。

## 本轮：播放中音量键 + 音乐页图标（2026-11-13 真机验证）

- [x] **播放中音量键失效**：根因是音量写 codec 用了 MP3 写入端持有的 `s_tx_lock`，
  prio 9 持锁方不会让出给 prio 5 的 `app_core` → 饿死。新增独立 `s_ctl_lock`
  （200ms 超时），锁序统一 tx→rx→ctl。真机 44.1k 播放中 `audio 0–7 ms`、
  `dac_reg` 0x8a→0x80→0x8a、`underruns=0 buffered=32768`；
  修复前同一操作 `audio 5799 ms` + `loop stall 5803 ms`。
- [x] KEY1/KEY2 改为按下即生效 + 按住每 250ms 重复 + 长按也算一步
  （原来必须等松手，按住反而完全不动）。
- [x] 诊断：`status` 增加 `app: loops/max_gap/age`；循环停顿 >200ms 打印
  `loop stall N ms in <stage>`；UI 拿显示锁 >150ms 打警告。
- [x] 音乐页把“音乐播放”四字换成 88px 圆形徽章 + 48px `LV_SYMBOL_AUDIO`：
  编译通过、烧录后 `key 3`×7 到页无异常/复位，字体 cmap 已确认含 U+F001
  （glyph 48×49px）；**用户真机目视确认图标正常、位置合适**，占位无需再调。
- [x] 构建：`idf.py build` → 0x82db70，factory 剩余 18%（0x1d2490）；已烧录。

## 本轮：多 WiFi / 音乐播放页 / 网页上传 / MP3 音质修复（2026-11-13 真机验证）

- [x] 多 WiFi 凭证：设置层最多保存 4 组 SSID，新增/覆盖/删除均写 NVS；配网页列出
  全部已存网络、支持扫描周边 WiFi 后选择。真机：页面加载/保存/删除正常，设备启动
  即连上已存网络（页面与 STA 共用同一份 NVS 数据）。
- [~] 启动选最强信号：`panda_connectivity` 扫描全信道后，在已存列表内按 RSSI 选最优，
  扫描失败沿用上次成功凭证重试。缺口：未构造“两个已存 AP 同时在线”的 A/B 场景；
  补验：保存两个 SSID，先只开弱 AP 启动，再开强 AP 重启，`wifi status` 应显示
  强 AP 的 BSSID/RSSI。
- [x] 音乐播放页：KEY3 菜单新增“音乐播放”，扫描 `/sdcard/Music`，显示序号/标题/状态，
  上一首/暂停/下一首；退出页面调用 `panda_mp3_stop()` 并恢复 16 kHz 语音时钟。
  真机：`mp3 list` → 46 首；用户通过页面实际播放（听感确认见下）；
  暂停/继续/切歌由串口 `mp3 toggle|next` 注入同一后端验证（日志 `paused`/`resumed`、切歌）。
- [x] 网页上传音乐：`POST /upload/music`（≤20MB、仅 `.mp3`、中文名保留），16KB
  扇区对齐缓冲让 FatFs 走多块 DMA。真机批量上传 42/42 成功，约 330–360KB/s，
  `sdcard ls` 字节数与源文件一致。
- [x] **MP3 卡顿/爆音修复**：解码与 I2S 写入拆成两个任务，中间加 32K 样本 PSRAM
  环形缓冲（16K 预缓冲 ≈0.34s@48k / 0.37s@44.1k，环满 0.67–0.74s）；仅在一首开始、
  写入端空闲且静音时切换一次采样率，播放中不再重配 codec；起播/续播/切歌淡入、
  曲尾淡出；90ms 排空等待覆盖 65ms DMA 深度；欠载计数可查。
  真机（`/dev/ttyACM0`）：
  - 44.1k 曲目：连续播放，`audio_rate=44100 underruns=0 buffered=32768 flags=0x1`。
  - 48k 曲目（`挪威的森林`）：`audio_rate=48000 underruns=0 buffered=32768`；
    `mp3 stop` 后 `audio rate -> 16000 Hz (mic=yes)`、`underruns=0`。
  - 用户听感确认：“没有卡顿和爆破音了”。
- [x] 修复中发现的 WDT：TX 任务在“文件读完且已排空”分支无延时忙等，优先级
  高于解码任务，饿死 core1 IDLE1 并触发任务看门狗；改为 5ms 休眠，并保证
  abort/idle 握手顺序（先清 `s_abort` 再置 `s_tx_idle`）。
- [x] 构建：`idf.py build` → `0x815960`，19% free，已烧录并真机验证。
- [~] SD 卡残留两个测试上传文件（`64k.mp3` 65536B、`test1m.mp3` 1048576B），
  播放约 0.8s/12s 后自动跳下一首；属卡内容，可在配网页删除，不影响固件。

## 本轮：配网页可达性 / 误触语音 / 空语音兜底（2026-11-11 真机验证）

- [x] 配网页自愈：`app_core.c` 周期任务核对 SoftAP 与 HTTP 状态，AP 在而页面
  未运行时重试（2s）；AP 开启边沿 toast 并记录 URL。
- [x] **页面截断根因**：`send_chunk()` 对空字符串发 0 长度块 = 结束响应。
  未填 App ID 或 STA 无 IP 时页面在半个 HTML 处结束。现在空串直接跳过。
  真机 `GET /` → 200、4094B、以 `</html>` 结尾。
- [x] **httpd 任务栈**：AP 启动后内部堆碎片化，8KB 栈创建任务失败
  （`ESP_ERR_HTTPD_TASK`），IDF 该路径不关监听 socket → 之后永久
  `listen (112)`。改为 4KB 栈、3KB 转义缓冲移 PSRAM、启动前检查
  `largest_internal >= stack+2KB`。真机 `largest_internal` 仅 3.5–7.7KB，
  页面仍稳定启动。
- [x] SoftAP 信道：STA 未连接时固定信道 1；AP_START 日志打印真实 IP/信道。
- [x] httpd 容忍慢连接：`max_open_sockets=4`、`backlog_conn=4`、收/发超时 10s。
- [x] 真机配网验收：`wifi ap on` → `SoftAP started ... ip=192.168.4.1 ch=6`、
  `provisioning ready`；`wifi status` → `softap: on`、`provision: on`；
  `/api/status` 200、`POST /save` 200、`/generate_204` 302；
  `wifi ap off` → 页面停止、softap off；连续两轮开关均正常，无端口泄漏。
- [x] 误触/静音 PTT 语音提醒（2026-11-11 真机）：150ms 轻触与 1s 静音长按均为
  `speech_frames=0` → `no speech (no sustained speech)` → `no-speech prompt sent`
  → `tts started voice=chat_tts_text` → `play drain complete` → idle，听到提示音。
- [x] 空语音错误抑制（2026-11-11 真机）：上述两轮均未出现 `error_llm` 错误屏。
- [x] 说话判定：RMS 250 / 8 帧（320ms）+ 按下后 120ms 爆音守卫。实测 150ms 轻触
  由旧版 `peak=9552 speech_frames=6` 降为 `peak=90 speech_frames=0`。
- [x] 提示文本回合改用 demo 的 `speech_text_buffer.append/commit` + `speech_id`
  （旧 `replacement.*` 缺 speech_id，服务端不响应，20s 后状态机弹错误屏）；
  另加 10s 提示无响应回 idle 的看门狗。
- [x] 构建：`idf.py build` → `0x805360`，20% free，已烧录并真机验证。
- [x] 语音 WS 403 已解除：原因是火山账号欠费；充值后主机握手返回
  `101 Switching Protocols`，真机 `websocket connected`，`voice clip` 全链路通过。

## 本轮：圆形屏边缘裁切修复（2026-11-10，已烧录）
- [x] 审计 37 个生成屏：正文/列表/标题字形均在 r≈180 内（仅 11 个标题与 2 个
  透明快捷设置文字靠近 172–180，无背景框，未裁切）；真正越界的是运行时 overlay。
- [x] 重排 overlay（`panda_ui.c`）：toast 240 宽、top 64；状态 pill 200 宽、bottom 44；
  字幕 pill 200 宽、bottom 42 → 盒体 y=270..318，严格落在屏幕下 1/4，四角
  r≈170.4；字幕出现或屏自带底部 tip 时隐藏状态 pill，避免互相压字。
- [x] 6 个带 tip 的生成屏（offline/reconnecting/asr/llm/tts/low_battery）tip 上移
  8px（y 298→290），底部两角 r≈167。
- [x] 主机仿真同步（`tools/ui_sim/web` + `pages.js` 重新生成）；圆形裁切开启后
  字幕/状态/toast 均完整可见。
- [x] 验证：`idf.py build` → 0x804590（20% free），烧录 `/dev/ttyACM0`；串口
  `ui subtitle` → `I (12006) ui: subtitle 38B label=192x40 box=200x48 scroll=0`；
  仿真 DOM 实测字幕 x=80,y=273,w=200,h=45，状态 pill hidden。
- [~] 真机目视：像素级观感（圆角/边距/字幕滚动手感）待用户确认。

## 本轮：电池 ADC 链路核查（2026-11-10，`[~]` 缺硬件补焊）

- [x] 核对官方 1.1 原理图：BAT_ADC 分压为 R2=200K / R11=100K（Vbat=Vadc×3），
  不是代码原先假设的 82K/110K（×1.745）；经 R53/R78 两个 0R 选择跳线接
  GPIO1，默认 NC（注释：“默认不启用电池电压测量……摄像头将不可使用”）。
- [x] 代码修正：ADC1_CH0(GPIO1)、分压比 3000；新增上电自检（内拉低后
  ≥200 mV 才认为分压接通）；未接通时 `battery` 显示 unknown，不误报低电。
- [x] 验证（未接通状态）：启动日志 `Battery sense not wired: GPIO1
  pulldown=0 mV`；`status` → `battery : unknown (ADC not wired)`；
  构建 0x804540，20% free，已烧录 `/dev/ttyACM0`。
- [~] 真实电压/百分比：需补焊 R53(1-3) + R78(1-2) 后复测。
  预期日志 `Battery ADC on GPIO1 (pulldown=... open=...)`，`battery` 在
  3.0–4.2 V（电池供电）或约 5 V/100%（USB 供电，BAT_OUT 为 5 V）。

## 本轮：对话中途断流根因修复（2026-11-10，已烧录验证）

- [x] 复现：`voice clip` 两轮对话测试均失败；播放 1–5s 后 RX 停滞、`Could not lock`
  连环报错、连接断开。先按 demo 对齐 WiFi/TCP 参数（STATIC_RX 6、RX_BA_WIN 6、
  TCP 窗口 32KB→5.7KB、RECVMBOX 6、MAX_SOCKETS 16、WS buffer 4096、
  network_timeout 10s），播放中断问题消失（4/4 轮完整播放）。
- [x] 根因 1：`WEBSOCKET_EVENT_ERROR`（event 0）被当成断线处理。组件对每次
  error 级日志都会发 ERROR 事件（含 500ms 发送超时），旧代码清 `s_ws_connected`、
  停上行静音泵；服务端约 72s 后以 `AudioServerNoAudioInputTooLongError` 关闭会话。
  修复：ERROR 仅 DEBUG 日志，不断会话；已用日志证据确认（断开后 ASR 数据仍在到达）。
- [x] 根因 2：天气内部堆门槛 28KB 在新 WiFi 缓冲（内部堆稳定 ~19KB）下永远不满足，
  每次 fetch 直接 `ESP_ERR_NO_MEM`，却被上报为“网络错误”；90s 内 3 次误触发
  WiFi 恢复，主动断开并杀掉进行中的语音会话。修复：门槛降到 16KB；仅 `ESP_FAIL`
  计入网络错误，内存不足只记日志。
- [x] 根因 3：clip 上行单帧入队失败即提前 commit（<0.3s 音频），ASR 拿不到内容。
  修复：入队失败重试，连续 ~4s（200 帧）才强制 commit。
- [x] 组件补丁：`esp_websocket_client` 发送 poll 超时（`wlen==0 && errno==0`）
  不再 `abort_connection`，仅丢该帧；避免一次 500ms 抖动杀掉整条会话。
- [x] 验证（真机，音量 30%）：
  - 12/12 轮 `voice clip` 全链路（ASR→TTS→`play drain complete`），0 次 WS 断开、
    0 丢包（主机 1Hz ping）、0 次 `Could not lock`。
  - 静置 90s：0/177 丢包，会话保持；静置后再 clip 仍正常。
  - 天气恢复：`weather refresh` → `weather: Beijing 11°C 晴 (code 0)`，无误触发恢复。
  - 堆：内部稳定 ~18.7–19.1KB，TLS 峰值最低 ~10.5KB；PSRAM ~7.35MB。
- [~] 真人按键/语音 PTT 复测：合成 clip 已覆盖同一链路，仍需用户实际说话确认。
- 风险：内部堆余量偏紧（最低 ~10KB）；组件补丁位于 `managed_components`，
  依赖更新后可能被覆盖；`audio tone`/`voice pump` 为诊断命令，非产品功能。
- `settings set` 新增：`personality`(0–2)、`feedback`(0–2)、`animation`(0–2)、
  `auto_sleep_s`(0/60/300/600)、`wake_mode`(0–2)、`sleep_anim`(0/1)、`dnd`(0/1)、
  `weather_city`（字符串）；保存后同步 UI 与硬件（人格提示词在下一轮对话生效）。
- WiFi STA + SoftAP + 配网 HTTP + SNTP、JSON 语音链路已接入代码（见 M1/M3）。
- SquareLine 37 屏已去重接入；生成 UI 负责导航，`app_core` 负责 PTT、双击电量、状态周期任务；
  KEY3 菜单的控件已全部接到 `app_core` 钩子（NVS/音频/亮度/WiFi/休眠/天气）。
- **真机连接（2026-11-10）**：电池 + SD 卡 + 1.85" 屏 + 喇叭 + 触摸。
  触摸曾因 CST816S 自动休眠持续 NACK，已写 `0xFE=0x01` 关闭自动休眠并实测恢复
  （`touch diag` 全 OK、长按 PTT 可用）。
- 播放诊断：每 5s `play loop #N: ring/peak/nearclip`；事件日志 `play start/pause/resume`、
  `DMA gap`、`playback underrun`、`play pause stall`、`play drain complete`、`playback canceled`。

## 本轮：BOOT 长按 4s 进入配网（2026-11-10，已烧录）

- [x] 冲突排查：GPIO0 只作 BOOT，未与外设复用；但它是 strapping 脚（复位时按住
  会进下载模式），只能在运行后检测。BOOT 原为 600ms 长按 PTT，与 4s 配网冲突；
  经确认改为 BOOT 专用于配网，PTT 只保留触摸长按。
- [x] 实现：BOOT 长按阈值 4s（其余键 600ms）；AP 关→开启 SoftAP+配网页，
  AP 已开→再按 4s 关闭；短按 toast“长按 4 秒进入配网”；休眠时按住会先唤醒再切换。
  `serial_cli` 新增 `key boot [long]` 注入。
- [x] 注入验证（真机）：`key boot` → `boot short: hold 4 s for provisioning`，无 PTT；
  `key boot long` → `provisioning page enabled/ready`，`wifi status` 为 softap on /
  provision on，主机 `curl http://192.168.x.x/api/status` 返回 200；
  再 `key boot long` → `provisioning page disabled`、softap off。构建 `0x801640`。
- [~] 物理按住 BOOT 4s 的时序验证：本次串口监听窗口内未检测到按压（可能无人操作）；
  GPIO0 读取/去抖路径沿用原 PTT 轮询，需用户实际长按一次确认。

## 上一轮：WiFi “假在线”检测与自动恢复（2026-11-10，已烧录验证）

- [x] 定位“断网”根因：驱动保持 connected+IP，但 ARP/TCP 全部失败且无 disconnect 事件
  （主机 ping 100% 丢包、`ip neigh` FAILED；内部堆稳定 ~44KB；配网页当时未运行）。
  与 ESP-IDF #8123/#5717/#15704 同款，属 WiFi 驱动/射频 RX 路径卡死。证据见
  `docs/tech_notes.md`“WiFi 假在线”。
- [x] 自动恢复：WS 断连与天气失败 90s 内聚合 3 次 → `disconnect+connect`，
  20s 无 IP → `stop/start`+`WIFI_PS_NONE`，45s 冷却；`esp_wifi_*` 全部移入 worker
  任务（timer 回调只投队列），worker 栈放 PSRAM。实测故障 67s → 95.5s 触发 →
  99.5s 恢复 IP，随后一轮语音回复与播放正常；完整回归 193.9s→199.3s 恢复。
- [x] 新增 `wifi recover` 手动恢复命令（绕过冷却）；实测约 4s 恢复。
- [~] 根除该卡死：未完成。恢复期间在途的那轮回复会丢失，故障仍偶发；候选后续：
  对照 demo 压测 `STATIC_RX_BUFFER_NUM` 4→6 等射频缓冲差异。
- [x] CLI 中文限制记录：`weather set 北京` 无效（linenoise 丢非 ASCII 字节），
  改用拼音 `Beijing`；中文城市请走配网页 `/save`。

## 上一轮：配网页按需开启 / 字幕 2 行滚动 / WS 锁补丁（2026-11-10，已烧录）

- [x] 配网页按需开启：`app_main.c` 有凭证时不再启动 HTTP；`app_core.c` 在 SoftAP
  上升沿 start、下降沿 stop。实测：启动 `provisioning page idle` 且 `curl :80` 失败；
  `wifi ap on` 后 `/api/status` 返回 200；`wifi ap off` 后 `provisioning page stopped`
  且端口恢复不通。
- [~] 自动 fallback SoftAP 关闭：`panda_connectivity.c` 在 STA 拿到 IP 后 5s 关闭自动 AP
  （`STA recovered, stopping fallback SoftAP`），并复位 `s_ap_user_off` 以便下次回退；
  AP_STOP 300ms 防抖避免重新配置 AP 时配网页抖动。缺口：需路由器断网/重启才能实测。
- [x] 字幕最多 2 行 + 超出滚动：固定 2 行窗口（340x56），`label` 高度超出时以 12px/s
  自下而上循环滚动。实测 `ui subtitle` 长文本 → `label=324x100 box=340x56 scroll=1`，
  短文本 → `label=324x20 box=340x56 scroll=0`。滚动动画需目视确认。
- [x] 长回复容量：字幕缓冲 512B、回复缓冲 1024B，UTF-8 安全截断；实测 372 字节回复
  完整显示 `subtitle 372B label=324x140`。
- [~] WebSocket 锁饥饿：本地补丁 `esp_websocket_client.c` 的 recv 超时 5s→500ms
  （与厂商 demo 本地补丁一致），避免 ws 任务持锁饿死上行。缺口：长回复中仍出现
  `esp_transport_write() returned 0` 与 WS 重连失败，根因未定位。
- [x] 已通过配网页 `POST /save` 恢复默认人格（CLI 受 linenoise UTF-8 过滤限制无法输入中文）。

## 上一轮修复：STT 重复 / TTS 爆音 / 播放卡死（2026-11-10，已烧录待听感）

> 对照 demo `Doubao_Ver1.1/components/doubao_voice/audio_out.c`；状态 `[~]`，等用户真人语音确认。

1. **STT 字幕重复**
   - 旧逻辑把每个 `input_audio_transcription.delta` 都 append，且 `completed` 后再追加整句，
     服务器发累积文本时出现 `hellohellohello`。
   - 现按 demo：delta 只记日志（前 12 条 LOGI），`completed` 一次性渲染最终文本；
     `response.output_text.delta` 用 `merge_text()` 兼容增量/累积/重复。
2. **爆音 / click**
   - 新增 demo 多相重采样 `voice_resampler.c`（32 相×32 抽头，Kaiser β=8，Q15），
     替换线性插值；跨 delta 保持相位/历史，消除采样不连续。
   - 播放加入 50ms 启动淡入、10ms 续播淡入、10ms 停顿淡出、取消/打断 20ms 淡出后再清环；
     `auto_clear` DMA 不再以阶跃声暴露。
3. **整段卡住不播**
   - 移除 codec 写 20×5ms 重试（一次写内部最长阻塞 1s，累计可达数秒）；
     改为每轮一次写 + 170ms DMA 间隙检测，断流淡入。
   - 预缓冲 200ms→500ms；ring<100ms 暂停、≥300ms 恢复；ring 静止 700ms 冲刷尾音，
     避免“只剩尾包但不够预缓冲”永久等待。
4. **触摸无法唤醒 / 无响应**
   - 根因：CST816S 空闲约 0.5s 自动进入低功耗，I2C 地址仍 ACK，但坐标寄存器读取
     全部 NACK/超时；旧逻辑 100 次失败后永久禁用触摸，于是无法唤醒也无法点击。
   - 修复：初始化写 `0xFE=0x01` 关闭自动休眠；探测失败仍创建 indev，每 ~3s 复位重试；
     保留日志静音防洪泛；新增 `touch diag` 现场读 ID/坐标寄存器。
   - 实测：`touch diag` 三次读均 `ESP_OK`；长按屏幕触发 PTT 并识别 “hello”。
5. **ASR 空最终转写**
   - 本轮实际事件里 `completed.transcript` 为空（日志 `asr:` 空行），但 delta 有内容；
     delta 仍只合并不渲染，`completed` 时若 transcript 为空则用合并结果兜底，保证字幕非空。

## 上一轮修复（WiFi/字体/TTS，2026-11-09，代码完成待真机验证）

1. **WiFi 掉线 / 网页配网**
   - 关 Modem sleep（`WIFI_PS_NONE`）、指数退避+抖动重连、打印 `wifi:reason` 与原因提示、
     全信道按信号扫描、PMF capable、国家码 CN。
   - 有凭证但 20s 未拿到 IP 时自动开 SoftAP（`ap_auto` 标记），首页显示断线原因与 AP 状态；
     配网页新增 `/api/scan`（`scanWifi()` 下拉选择）；只改密码也会触发重连。
2. **中文乱码 / 字体**
   - `ui_font_CJK16`（GB2312+ASCII+标点，16px 4bpp）作为四个 SquareLine 字体的 `.fallback`；
     新增 `tools/fonts/check_coverage.py` 校验 UI 字符串用到的 269 个非 ASCII 字符全部有字形。
3. **TTS 播放断续 / 丢包**
   - 有状态 24k→16k 重采样（跨 delta 保持相位/插值尾样本）、200ms 预缓冲、欠载判定、
     部分写入重试、TX/RX 独立锁、接受 WS 续帧（opcode 0x0）、RX 溢出按 `}` 重同步、
     重连/取消时清 ring 与重采样状态、`voice stats` 暴露计数。
   - 待真机验收：长回复连续播放、`underruns`/`ring_drops`/`decode_failures` 计数。
4. **实体按键（KEY1–3）**
   - KEY1 短按音量 −10%、KEY2 短按 +10%（NVS 保存、音频/UI 同步、toast）；
     KEY3 短按循环设置菜单（快捷→人格→睡眠→系统→设备信息→时间天气），长按回主屏。
   - `app_core_key_action()` 供按键轮询与 `key` CLI 共用；待真机确认三个按键的物理顺序。

## 本轮新增：KEY3 菜单全功能（2026-11-09 晚，代码完成待真机）

> 目标来自上一条 KEY3 审计。以下均为代码 + 编译/主机校验通过，**尚未接屏/触摸/喇叭**，
> 状态标 `[~]`；2026-11-10 真机按文末清单验收。

1. **快捷设置**
   - Wi-Fi 开关接真实连接：开→用 NVS 凭证 `connect()`，无凭证→开配网 AP；
     关→`panda_connectivity_ap(false)` + `disconnect()`（置手动断开，避免退避重连）。
   - 勿扰（DND）改为扬声器静音：`panda_audio_mute()`，保留已存音量；状态落 NVS。
   - 睡眠按钮进入真实 SLEEP 状态（语音播放中先 cancel），不再只切睡眠脸。
   - 蓝牙 tile（`ui_bt_*`）隐藏：固件未链接 BT 协议栈，不做假开关。
   - 亮度 tile 移到原蓝牙位置，系统设置页新增真实亮度滑块。
2. **个性化**
   - 性格/反馈强度/动画活跃度三组选项落 NVS（`pers/fb_lvl/anim_lvl`），互斥高亮随持久化状态恢复；
     性格与反馈拼进 `session.create` 的 persona（下一轮对话生效）；动画活跃度映射
     菜单切屏时长 0/150/250ms。
3. **睡眠设置**
   - 60s/300s/600s/关闭四档落 NVS；`app_core` 每秒检查，设备未睡、语音 IDLE/OFFLINE
     且无触摸/按键/语音事件达到阈值后自动 `panda_ui_sleep()`。
   - 睡眠动画开关、唤醒方式落 NVS；触摸/按键可唤醒并显示 1.2s 唤醒屏，
     **语音唤醒未实现**（选“语音”时 toast 明示，触摸仍可用）。
4. **系统设置 / 存储与版本**
   - 亮度/音量改为真实 `lv_slider`（隐藏 SquareLine 静态 track/fill/knob，原位创建，
     扩大点击区 18px）；拖动实时预览，松手写 NVS + toast。
   - 两处百分比、Wi-Fi 文案、时间同步状态随周期刷新；SD 容量用
     `esp_vfs_fat_info()` 读真实 total/free；版本号读 `esp_app_desc`（v2.0.0）。
5. **时间天气**
   - 新增 `app_weather.c`：Open-Meteo geocoding + forecast（无需新凭证），城市来自
     NVS/配网页/`weather set`；成功 30min 刷新，失败 60s 重试，离线 10s 轮询。
   - 温度与 3 类图标（晴/多云/阴）写入时间天气页；WMO 其余码归到“阴/雨/雪”等文字，
     图标只有 3 种。响应缓冲 4KB 与任务栈均在 PSRAM，内部堆低于 28KB 时跳过本轮。
6. **生效路径**
   - 菜单回调 → `panda_ui_menu_hooks_t` → `app_core`：NVS 保存、音频/亮度/WiFi/休眠动作、
     天气刷新；CLI/配网页改设置后调用 `app_core_settings_changed()` 立即同步 UI 与硬件。


## 真机验证记录（2026-11-09）

- 启动：ESP32-S3 v0.2，16MB Flash + 8MB PSRAM，USB-SERIAL-JTAG `/dev/ttyACM0`。
- 通过：I2C0 扫描到 0x18/0x20/0x40/0x51；TCA9555 初始化无报错；RTC 读写并跨复位保持；SD 卡挂载（1876MB）；RGB 灯环设置/关闭；ES8311/ES7210 初始化；麦克风采集 rms=19..60。
- 无屏容错：显示初始化成功、LVGL/37 屏创建成功；CST816S 探测 NACK 仅告警，系统继续启动并进入 CLI。
- 未验证：屏幕显示、触摸手势、喇叭播放、电池电压（0R 未焊，见本轮电池 ADC 核查）、豆包云端。
- 已修复：TCA9555 方向/电平顺序、亮度 NVS 应用、`rgb` 参数判断、`app_key` 默认值/明文打印、RTC 2000 年误判有效。

### 2026-11-09 晚（建立交接流程时复验）

- `idf.py build` 通过：0x70b1e0，剩余 30%。
- 启动：ES8311/ES7210 正常，`audio ready (16kHz mono, mic=yes, volume=70%)`。
- `heap`：internal≈46KB、PSRAM≈7.57MB、min_internal≈35KB。
- `wifi status`：首次 STA 连接失败（`reason=2`）；`wifi ap on` 进入 APSTA 后 STA 成功连上
  <家庭WiFi>，IP 192.168.x.x、RSSI -46dBm，并出现 `wifi: time synced: 2026-11-09 15:12:35`。
- 配网：主机直连 `http://192.168.x.x/` 验证首页、`/api/status`、`POST /save`（音量 71 写入后恢复 70）、
  captive portal 404→302；SoftAP `Panda-AI-00A1` 正常启动。
- 语音：JSON 云端链路打通。`voice clip` → `commit acked` → `tts started` → 回复文本 →
  `tts done deltas=12 ring=8269`，播完回到“待机”；连续两轮正常。
- 打断：`voice cancel` 后 `tts done deltas=2 ring=0`（迟到音频被丢弃），下一轮可正常对话。
- 状态机/UI：`make -C tools/ui_sim test` 46 checks/0 fail；`ui state listening/thinking/speaking/idle`、
  `ui emotion happy`、`ui sleep`、`ui wake` 真机切屏正确，wake 1.2s 后回 idle。
- 图片加载：主机 58/58 描述符校验 PASS；真机 `ui images` 37 屏/71 对象/58 唯一/0 失败。
- 错误屏：`ui error asr/tts/network/auth` 分别切到 error_asr/error_tts/offline/error_llm，
  `ui error clear` 回 idle。
- 未验证：喇叭可听、麦克风真实语音 ASR、网页切换 WiFi、断网恢复、屏幕实际像素/触摸/帧率。

### 2026-11-09 上轮（主机侧，无硬件）

- `idf.py build` → 通过：`panda_ai.bin 0x7f7400`，剩余 `0x208c00 (20%)`。
- `python3 tools/fonts/check_coverage.py` → PASS：字体 8371 字形，UI 字符串 269 个非 ASCII 字符全覆盖，
  4 个 SquareLine 字体均已挂 `.fallback`。
- `make -C tools/ui_sim test` → 46 checks, 0 failure(s)。
- 未验证：以上三项修复均未接硬件；真机验收计划 2026-11-10（掉线恢复/RSSI、配网页扫码切换 WiFi、
  TTS 连续播放与 `voice stats`、中文字幕/提示）。

## M0 工程骨架与硬件自检

- [x] 阅读 PRD、demo、原理图、屏幕/豆包官方文档
- [x] 输出可行性/引脚冲突清单（docs/feasibility.md）
- [x] ESP-IDF 工程骨架（CMake / partitions / sdkconfig / 组件，编译通过）
- [~] panda_board：I2C0/TCA9555/RTC/SD/RGB 已实测；KEY1–3 已接音量/菜单动作，
  物理按键顺序与实际触发待真机验证；电池待补焊 0R 后验证
- [~] panda_display：ST77916 QSPI + LVGL9 + 背光；屏已接并正常显示，
  实际像素/色彩/帧率与 37 屏逐屏观感待验收
- [~] panda_touch：CST816S + 软件手势；自动休眠已关闭，长按 PTT/diag 实测通过；
  单击/双击/滑动、休眠唤醒、误触待逐项验收
- [~] 串口 CLI：status / ui / touch / brightness / volume / battery / time / settings / voice / audio / weather / rgb / i2c / sdcard / rtc / wifi / heap / reboot
- [ ] 真机验证：启动日志、屏幕测试图案、触摸坐标、按键
- [x] 记录构建/flash/monitor 命令和结果（README）

## M1 语音闭环

- [~] panda_audio：ES8311/ES7210、I2S 全双工；麦克风与喇叭均已接通，
  `voice clip` 12/12 轮播放日志正常，爆音/卡死的听感待用户确认
- [x] 功放空闲噪声（2026-11-14）：分段实验定位为 PA 通电底噪
  （PA 关安静；PA 开+DAC 静音仍滋滋）。改为“写 PCM 时才开 PA，空闲 800ms
  先静音 DAC 再关 PA，勿扰立即关断”；真机 `audio status` 空闲 `out=off`、
  播放 `out=on`，用户确认空闲完全安静。播放中残余底噪与修复前相近，来自
  功放通电本身，软件无解（需硬件改善供电/滤波）。构建 `0x8353d0`（18% free）。
- [ ] 音频自检：tone、loopback、mic RMS、音量（`audio test` 仅有弱校验）
- [~] panda_settings：NVS 配置（WiFi/豆包/音量/亮度/人格/反馈/动画/睡眠/勿扰/天气城市）
  已接入；默认 `api_mode` 已改为 `json`（NVS 清空场景待复测）
- [~] panda_connectivity：STA/AP/SNTP 已实测（192.168.x.x、-46dBm、时间同步）；
  已加入 ps off/指数退避/全信道扫描/AP 自动回退，fallback AP 在 STA 恢复后 5s 自动关闭
  （关闭分支待路由器断网实测）；配网页随 SoftAP 启停
- [x] panda_voice（JSON）：测试音频→commit→ASR/TTS→回复文本已验证，两轮连续可用
- [x] panda_voice（O2.0）：二进制协议层与 `app_voice` 分支已实现，构建
  `0x835000`（18% free），主机帧/gzip 测试通过。2026-11-14 配置凭证后真机验证：
  `O2 connection started` → `O2 StartSession sent (632B)` → `O2 session started`
  → `asr: 你是谁呀？你叫什么名字？` → `tts done ... status=0`；真机触摸 PTT
  （“唱首歌听听”）通过；`voice cancel`（FinishSession）后 `voice clip` 自动
  重建会话并完成下一轮；`voice stats underruns=0 ring_drops=0 rx_overflow=0`。
  - 修掉 WS 任务栈溢出：建会话的 7KB 临时缓冲改 PSRAM（连上即崩溃重启）。
  - 修掉长回复音频丢失：`RING_SAMPLES` 256K→768K（约 48s），43s TTS 修复前
    丢 293933 样本；>48s 仍会丢尾。
  - 未验：新环形缓冲下的真实超长回复；O2 说话中再按 PTT 的真机听感。
- [~] 触摸 PTT：长按已接 `app_voice`（BOOT 改为配网专用，不再触发 PTT）；
  真人语音识别已验证，触摸时序仍需逐项验收
- [~] 流式 TTS 播放 + 24k→16k：已按 demo 换成多相重采样 + 500ms 预缓冲 + 淡入淡出 +
  暂停回滞 + 静音冲刷 + DMA 间隙检测；已烧录，待真机验听爆音/卡死与播放诊断日志
- [x] 播放打断：`voice cancel` 已验证；取消后迟到音频丢弃（ring=0），下一轮正常
- [~] 错误处理：WiFi 已指数退避；语音 WS 仍固定 5s 重连，鉴权错误（401/403）未分类停止
- [~] 真机验证：连续两轮+打断已过；断网恢复、真实语音 ASR 未验证

## M2 熊猫角色

- [x] 接入 demo/ui SquareLine 37 屏资产（冲突符号去重、动画符号修复、编译通过）
- [x] 设备/语音/情绪三维状态机 + 优先级/超时 + 顶部状态/字幕/toast overlay
  （`panda_state.c`；主机仿真 46 项断言通过；真机 `ui screen` 切屏验证）
- [x] 图片加载校验：58 个描述符主机逐项解析校验，真机 LVGL 解码器 37 屏/71 对象/0 失败
- [~] 状态屏映射：Idle/Listening/Thinking/Speaking/Offline/Reconnecting/ASR/LLM/TTS/LowBattery/Sleep
  （`ui screen` 映射已验证，屏已接；实际显示与状态/音频一致性待逐屏验收）
- [~] 情绪屏映射：Happy/Surprise/Sad/Sleepy/Curious/Shy/Angry（已验证 happy；其余待真机逐项确认）
- [x] 统一交互：生成 UI 负责导航，app_core 负责 PTT、双击电量、BOOT 键；设置页不被状态机抢占
- [x] 浏览器全页面预览：20 个状态屏 + 12 个设备设置页 + 6 个未枚举表情页，按生成代码坐标/图片渲染
  （`tools/ui_sim/web`；`gen_pages.py` 生成 `pages.js`；headless Firefox 逐页检查 18 个设置/表情页图片加载与 3 条页面链接跳转）
- [~] 字幕（P1，可选开关）：ASR 最终转写与回复文本已上屏；已修掉 delta 重复叠加，
  并限制为 2 行窗口、超长自下而上滚动；待真机目视滚动效果
- [ ] SD 动画 manifest + 帧播放 + 缺卡兜底（P1）
- [ ] 真机验证：状态与音频一致、实际像素/色彩/无明显掉帧
- [ ] 真机验证：滑动手势、点击不误触发、亮度/音量入口

## M3 产品化基础

- [~] 配网网页（WiFi/豆包凭证/音色/音量/亮度/人格/天气城市）：首页/`/api/status`/
  `POST /save`/captive portal 已端到端验证；新增 `/api/scan`、自动 AP 回退（20s）、
  断线原因展示、密码变更重连、保存后立即应用并刷新天气。
  网页服务已改为**按需开启**：仅在 SoftAP 生效（无凭证开机 / `wifi ap on` / 20s 回退）时
  启动，热点关闭即停止；STA 恢复后自动回退热点在 5s 后关闭（防在途请求被截断）。
  已实测 `wifi ap on` → `provisioning page enabled`、`wifi ap off` → `disabled`；
  回退热点“恢复后自动关闭”仍需真实路由器断网复测
- [~] 电量显示（硬件允许时）、低电降级（UI/周期任务已接入，ADC 阻塞）
- [~] 时间显示、RTC 保持；SNTP 已验证 `time synced`；时间天气页已接真实天气，
  屏幕时间显示与断电保持待补测
- [~] 亮度/音量/静音：CLI + NVS + 快捷/系统设置 UI 已接线（真实滑块、DND=扬声器静音），
      待真机触摸拖动与静音恢复验证
- [~] 睡眠/唤醒：自动休眠（0/60/300/600s）、睡眠动画、触摸/按键唤醒已接；
      深睡功耗策略与语音唤醒未实现
- [~] 天气：Open-Meteo geocoding + forecast，城市可经 CLI/配网页设置；
  真机 TLS 取数已验证（`weather refresh` → Beijing 11°C 晴）；图标仅 3 类，
  30min 刷新与断网重试的长时间行为待观察
- [ ] 隐私：未对话不上传、录音状态可见、日志不存原始音频
- [~] 诊断页/CLI 统计（CLI status/heap 已完成；status 增加天气与菜单状态行）
- [ ] 真机验证：断网本地功能、低电、睡眠唤醒、菜单全功能

## M4 结构与可靠性

- [ ] 整机装配/佩戴舒适度（硬件团队）
- [ ] 声学/回声/风噪测试
- [ ] WiFi 天线净空/衰减
- [ ] 温升/续航/充电测试
- [ ] 24h 稳定性、内存泄漏、100 轮对话

## KEY3 菜单功能审计（2026-11-09）

KEY3 循环的 6 个页面均已在 `ui_init()` 创建、导航与手势可用，但功能接线不完整：

| 页面 | 已可用 | 缺口 |
|---|---|---|
| 快捷设置 | 音量文字随 KEY1/2/CLI 更新；Wi-Fi 状态灯/文字只读刷新 | Wi-Fi 开关只切图层，不连接/断开；蓝牙、勿扰无后端；亮度入口无控件；睡眠按钮只切睡眠脸，不进 SLEEP 状态 |
| 个性化 | 三组选项互斥高亮 | 选择不落盘、不改变 `settings.persona`；反馈强度/动画活跃度无设置字段 |
| 睡眠设置 | 选项互斥高亮 | `idle_timeout_s` 已有但未消费；无自动休眠定时器；唤醒方式/睡眠动画无字段 |
| 系统设置 | 可跳转“存储与版本” | 亮度/音量滑块只是静态图层，无事件；两处百分比、网络、时间同步文案从不刷新 |
| 存储与版本 | 跳转角色预览/表情/动画可用 | SD 容量与版本号为写死文案；未读 SD 卡与 `esp_app_desc` |
| 时间天气 | 时钟/日期每秒随 RTC/SNTP 更新 | 天气图标/26°C 为写死占位，无数据源 |

接线结果（2026-11-09 晚，详见上一节）：

| 页面 | 状态 | 实现 / 仍需验证 |
|---|---|---|
| 快捷设置 | `[~]` | Wi-Fi 开关、勿扰静音、睡眠按钮、蓝牙隐藏、亮度入口已接；待真机触摸/视觉 |
| 个性化 | `[~]` | 三组选项落 NVS、persona 组合、动画时长已接；待真机确认高亮与对话变化 |
| 睡眠设置 | `[~]` | 自动休眠/唤醒方式/睡眠动画已接；语音唤醒未实现；待真机验证唤醒输入 |
| 系统设置 | `[~]` | 真实滑块、百分比/Wi-Fi/时间同步动态刷新；待真机拖动与布局确认 |
| 存储与版本 | `[~]` | SD 容量、固件版本改为真实读取；待真机核对数值 |
| 时间天气 | `[~]` | Open-Meteo 数据、城市可配；待真机联网取数与中文显示 |

- [~] 菜单功能接线（P1）：音量/亮度滑块、个性化落盘、睡眠定时、系统信息动态化
      已实现并编译通过；缺口是无屏/触摸，滑块手感、布局遮挡与实际显示未验证。
- [~] 快捷设置降级处理（P2）：蓝牙 tile 隐藏，勿扰=扬声器静音，Wi-Fi 开关接真实连接；
      缺口是真机确认 Wi-Fi 关→开（含 AP 回退）与静音恢复。
- [~] 天气数据源（P2）：采用 Open-Meteo（geocoding + forecast，无需凭证）；
      缺口是真机 TLS 取数、30min 刷新与断网重试，另外图标只有晴/多云/阴 3 种。

## 2026-11-10 真机验收清单（本轮菜单功能）

- [ ] 系统设置：拖亮度/音量滑块，标签实时变化，松手后重启仍保持；滑块无遮挡。
- [ ] 快捷设置：Wi-Fi 关→状态灯灭/文案“未连接”→开→重新连上（无凭证时开 AP）；
      勿扰开→TTS 静音，关→恢复原音量；睡眠按钮进入睡眠屏；蓝牙 tile 不出现。
- [ ] 个性化：三组选项重启后保持高亮；换性格后下一轮 `voice clip` 的回复风格变化。
- [ ] 睡眠设置：60s 档静置自动入睡；触摸/KEY 唤醒并显示 1.2s 唤醒屏；
      选“语音唤醒”出现“暂不支持”提示；睡眠动画开关有可见差异。
- [ ] 存储与版本：SD 容量与主机 `df` 一致；版本显示 `v2.0.0`。
- [ ] 时间天气：联网后显示真实温度/图标；`weather set 上海` 后刷新；断网不崩溃。
- [ ] 串口辅助：`status` 的 weather/menu 行、`settings show`、`weather`、`key 1|2|3`。

## 阻塞项

- [!] 豆包 O2.0 控制台权限/模型版本未确认：仅 JSON 链路可联调，生产协议无法落地。
- [!] 电池 ADC 需补焊 R53(1-3)+R78(1-2) 两个 0R：GPIO1 目前悬空，电压/电量
  无法读数（固件已就绪并会自动识别）。补焊后摄像头 HREF 不可用。
- [!] WiFi “假在线”卡死未根除：已有自动恢复阶梯（67s 故障→95.5s 触发→99.5s 恢复），
  恢复期间在途回复会丢失，故障仍偶发；候选方向是对齐 demo 的射频缓冲参数。
