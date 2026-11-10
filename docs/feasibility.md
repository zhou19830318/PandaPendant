# Panda AI 挂坠 —— 可行性评估与技术调研（V0.1）

- 评估日期：2026-11-08
- 目标硬件：Waveshare ESP32-S3-AUDIO-Board + Waveshare 1.85inch Touch LCD Module
- 软件基线：ESP-IDF v5.5 + LVGL 9.5 + 火山引擎豆包端到端实时语音
- 参考工程：
  - `demo/1.85-S3-audio-board-test`：1.85" ST77916 QSPI 屏 + CST816S 触摸驱动
  - `demo/Doubao_Ver1.1`：豆包实时语音、ES8311/ES7210、WiFi、串口/网页配置
  - `demo/AIWatch_Ver2.0`：LVGL 9 UI、ADC/按键/电源、串口 CLI、稳定性经验
  - `demo/Doubao_Ver1.1/1.2.1.1豆包语音_端到端实时语音大模型API接入文档_1787178779.pdf`

## 1. 结论摘要

| 模块 | 可行性 | 结论 |
|---|---|---|
| 1.85" 360×360 ST77916 QSPI 显示 | 高 | 音频板 18PIN 接口与模块线序一致；demo 已验证 QSPI 引脚；需通过 TCA9555 控制 LCD_RST |
| CST816S 触摸 | 高 | I2C0 与音频 codec/RTC/IO 扩展共用，地址无冲突；无独立 INT 到 SoC，需轮询；手势需软件识别 |
| ES8311 播放 + ES7210 麦克风 | 高 | demo 已有完整 esp_codec_dev 链路；需注意 MIC 槽位、PA 使能、TTS 24k→16k 重采样 |
| 豆包端到端实时语音 | 中高 | demo 已跑通 JSON 与 O2.0 二进制两条链路；生产以官方 O2.0/SC2.0 为准，需控制台开通对应模型/资源 |
| 流式 TTS 播放 + 打断 | 高 | demo 的环形缓冲 + 播放任务 + 淡入淡出方案成熟；首版按键半双工即可 |
| LVGL 9 熊猫表情 | 高 | 用矢量对象可先实现全部 P0 表情；SD 序列帧播放为增强项，带宽/解码器是主要风险 |
| SD 动画资源 | 中 | SDMMC 1-bit 可读；360×360 RGB565 原始帧 253KB/帧，10fps≈2.5MB/s，需 RLE/调色板/低帧率或矢量方案 |
| WiFi 配网/网页配置 | 高 | demo 的 AP+Captive Portal+HTTP 配置页可复用；注意首次启动流程与凭证安全 |
| 电量检测 | 低（当前硬件） | 音频板 BAT_ADC 接 GPIO1，默认未焊接 0R，启用后摄像头不可用；充电状态无 GPIO 回读 |
| RTC / 时间 | 高 | PCF85063 在 I2C0 0x51；需自写轻量驱动 + SNTP 校时 |
| 低功耗/深睡 | 中 | 仅 GPIO0(BOOT) 可深睡唤醒；TCA9555 按键不能唤醒；无 PMU，需软件策略 |
| 结构/声学/热/续航 | 待硬件实测 | 与本固件并行验证；不能以开发板结果替代整机结论 |

## 2. 硬件兼容性与引脚冲突清单

### 2.1 音频板 18PIN 屏幕接口 ↔ 1.85" 模块

1.85" 模块 18PIN 定义（官方文档）：

| 模块引脚 | 功能 | 音频板网络 | ESP32 GPIO |
|---|---|---|---|
| 1 | VCC | 3V3 | - |
| 2 | LCD_BL | BL_PWM | GPIO5 |
| 3 | GND | GND | - |
| 4 | LCD_SCLK | LCD_SCK | GPIO4 |
| 5 | LCD_IO0 | LCD_SDA0/LCD_MOSI | GPIO9 |
| 6 | LCD_IO1 | LCD_SDA1/LCD_MISO | GPIO8 |
| 7 | LCD_IO2 | LCD_SDA2/LCD_DC | GPIO7 |
| 8 | LCD_RST | Extend_IO0 | TCA9555 P0.0 |
| 9 | LCD_CS | LCD_CS | GPIO3 |
| 10 | LCD_TE | 未引出/未使用 | - |
| 11 | LCD_IO3 | LCD_SDA3 | GPIO6 |
| 12 | TP_RST | Extend_IO1 | TCA9555 P0.1 |
| 13 | TP_SCL | TP_SCL | GPIO10 |
| 14 | TP_SDA | TP_SDA | GPIO11 |
| 15 | TP_INT | Extend_IO2 | TCA9555 P0.2 |

结论：**接口兼容**。但必须由 TCA9555 主动释放 LCD_RST/TP_RST（输出高），否则可能一直处于复位。demo `1.85-S3-audio-board-test` 将 RST 设为 -1、靠模块 RC 复位，本项目将显式控制复位以提升可靠性。

### 2.2 资源占用/冲突

| 外设 | 引脚/资源 | 冲突结论 |
|---|---|---|
| QSPI LCD | GPIO3/4/6/7/8/9 + GPIO5 背光 | 与 I2S(12-16)、I2C(10/11)、SD(40-42)、RGB(38) 不冲突 |
| 触摸 | I2C0 0x15 (CST816S) | 与 ES8311(0x18)、ES7210(0x40)、TCA9555(0x20)、PCF85063(0x51) 地址不冲突 |
| 音频 | I2S0 GPIO12-16；ES8311/ES7210 共用 I2C0 | 与 LCD 不冲突；需共享 I2S 全双工与 codec mutex |
| SD | SDMMC 1-bit GPIO40/41/42 | 与摄像头 D3(39)/其他不冲突；不用 SPI 模式可避开 SPI2 DMA 争用 |
| 摄像头 | GPIO1/2/17/18/21/39/43-48 | 与电池 ADC(GPIO1) 互斥；首版不启用摄像头 |
| RGB 灯 | GPIO38 | 可用，但建议 UI 状态优先用屏幕表达 |
| 按键 | BOOT GPIO0；Key1-3 经 TCA9555 P1.1-P1.3 | 深睡仅 GPIO0 可唤醒 |
| 电池 ADC | GPIO1，默认未连接 | 需改焊 0R；启用后摄像头不可用 |
| USB 控制台 | GPIO19/20 USB-SERIAL-JTAG | 串口 CLI/日志走 USB-C |

### 2.3 已识别的硬件限制

1. **电池电压不可直接测**：BAT_ADC 在 GPIO1，原厂默认不启用，需焊接 0R；同时失去摄像头 HREF。首版做“可选电池 ADC + 未启用时显示未知/USB”，不阻塞语音功能。
2. **充电状态无 MCU 回读**：板上只有充电 LED，软件无法区分充电/充满。SYS-006 需硬件改版或外接检测。
3. **TCA9555 按键不可深睡唤醒**：深睡唤醒源限定 GPIO0(BOOT)。
4. **无 AEC 参考回采**：ES7210 采集与扬声器共用腔体，首版需在播放时静音上行（半双工），否则模型会听到自己的 TTS；全双工 barge-in 需后续 AEC 或严格能量门控。
5. **无 TE 同步**：1.85" 模块 TE 未接入 SoC，无法做硬件撕裂同步；靠 QSPI 带宽和 LVGL 局部刷新规避。
6. **屏幕/结构尺寸**：模块 55×55mm，显示区 45.68×45.68mm；最终挂坠堆叠必须 CAD 复核，开发板不等于可量产结构。

## 3. 技术栈选型

| 层 | 选型 | 说明/风险 |
|---|---|---|
| OS/框架 | ESP-IDF v5.5 (Xtensa LX7, 240MHz) | demo 同版本；组件管理器拉取依赖 |
| GUI | LVGL 9.5 + esp_lvgl_port 2.9 | 必须由 lvgl_port 锁保护；LVGL 9 API 与 demo AIWatch 一致 |
| 显示驱动 | 自带 `esp_lcd_st77916`（QSPI）+ esp_lcd_panel_io_spi | 从 1.85 demo 移植；`quad_mode=1`，cmd_bits=32；上电先 3MHz 读版本再切 80MHz |
| 触摸 | esp_lcd_touch + esp_lcd_touch_cst816s | 轮询 30-50Hz；软件手势（单击/双击/长按/滑动） |
| 音频 | esp_codec_dev + ES8311 + ES7210 + I2S std | 16kHz/16bit 单声道；TTS 24kHz→16kHz 多相重采样 |
| 语音协议 | 豆包 Realtime O2.0 二进制协议（官方文档） | WebSocket + 4 字节头 + event id + session id + payload；音频 20ms/640B 上行 |
| WebSocket | esp_websocket_client 1.x | 需处理分片、TLS、重连退避、TX/RX 队列 |
| JSON | cJSON（IDF 内置） | 大 JSON 用 PSRAM 分配钩子，避免内部堆碎片 |
| 配置 | NVS | WiFi/豆包凭证/音量/亮度/人格；不提交真实密钥 |
| 配网 | SoftAP + Captive Portal + HTTP 配置页 | 参考 demo wifi_manager/webserver |
| 时间 | PCF85063 + SNTP | 断网用 RTC，联网校时 |
| 存储 | SDMMC 1-bit + FATFS | 动画/音效/日志；缺卡降级 |
| 诊断 | esp_console USB-SERIAL-JTAG CLI + ESP_LOG | 每个模块独立 TAG；运行统计、错误码、堆监控 |

## 4. 需求逐项可行性

### 4.1 语音对话（VOI）

| ID | 需求 | 可行性 | 实现/风险 |
|---|---|---|---|
| VOI-001 | 实体按键启动语音 | 高 | BOOT/Key1 短按触发；150ms 内先切 UI LISTENING，再建链 |
| VOI-002 | 麦克风实时上行 | 高 | ES7210→I2S RX→20/40ms 分片→base64/binary；需 VAD 与静音不误发 |
| VOI-003 | 豆包流式回复 | 高 | O2.0 TTSResponse(352) 二进制音频；24k PCM16→16k 重采样→环形缓冲播放 |
| VOI-004 | 聆听/思考/说话反馈 | 高 | 事件总线驱动 UI；协议事件与播放状态双确认 |
| VOI-005 | 判停 | 高 | 服务端 VAD（end_smooth_window_ms）或按键松开 EndASR；建议默认服务端 VAD + 按键可强制结束 |
| VOI-006 | 播放中打断 | 高 | 按键打断：停播放、清 ring、发 FinishSession/新会话；首版不做无按钮 barge-in |
| VOI-007 | 网络错误恢复 | 中高 | 指数退避 2-60s、鉴权错误停止重试、UI Offline/Error；需区分 4xx/5xx/超时 |
| VOI-008 | 音量调节 | 高 | ES8311 音量 + 软件音量；UI/CLI/语音指令 |
| VOI-009 | 字幕 | 高（P1） | 16px CJK 字体 + 单行滚动；不遮挡面部 |
| VOI-010 | 多轮上下文 | 高（P1） | 同一 session/dialog_id；断线重连用 dialog_id 或会话内保留 |
| VOI-011 | 音色配置 | 高（P1） | StartSession.tts.speaker；控制台授权音色 |
| VOI-012 | 唱歌/故事 | 中（P2） | O2.0 enable_music 需企业/版权资质；先不做 |

### 4.2 熊猫情绪与动画（ANI）

| ID | 需求 | 可行性 | 实现/风险 |
|---|---|---|---|
| ANI-001~009 | Idle/Listening/Thinking/Speaking/Happy/Surprise/Sad/Sleep/Error | 高 | LVGL 矢量熊猫脸 + 状态机；Speaking 用播放 RMS 驱动嘴型 |
| ANI-010~014 | Curious/Shy/Angry/Wake/Interrupt | 高（P1） | 同一表情系统增加参数/动画 |
| ANI-015 | 自定义表情包 | 中（P2） | 需要资源格式/许可/存储方案 |
| 统一状态机 | 设备/语音/情绪三层 | 高 | 三层枚举分离，事件总线唯一入口 |
| SD 序列帧 | 预渲染 3D + PSRAM 小缓存 | 中 | 先矢量脸保证 P0；SD 播放器做 RGB565/RLE + 缺卡兜底 |

### 4.3 触摸交互（TCH）

| ID | 需求 | 可行性 | 实现/风险 |
|---|---|---|---|
| TCH-001 | 单击唤醒 | 高 | 软件手势识别；CST816S 轮询 |
| TCH-002 | 长按说话 | 高 | 与实体键统一 PTT 策略；长按阈值 500ms |
| TCH-003 | 双击 | 高 | 300ms 窗口；首版双击查看电量/重复互动 |
| TCH-004/005/006 | 滑动/上滑/长按角色 | 中高（P1） | 软件滑动识别；需阈值/防误触 |
| TCH-007 | 误触防护 | 中高 | 边缘/短脉冲过滤、佩戴锁、双击确认敏感动作；需真机调参 |

### 4.4 基础设备（SYS）

| ID | 需求 | 可行性 | 实现/风险 |
|---|---|---|---|
| SYS-001 | 电量显示 | 低（硬件） | 见 2.3；可选 ADC + 未知态；需硬件改焊 |
| SYS-002 | WiFi 状态 | 高 | 事件驱动 UI 图标 |
| SYS-003 | 时间显示 | 高 | PCF85063 + SNTP；无网络用 RTC |
| SYS-004 | 亮度 | 高 | LEDC PWM GPIO5；0-100%，低电降亮度 |
| SYS-005 | 音量 | 高 | ES8311 + 软件限幅 |
| SYS-006 | 充电状态 | 低（硬件） | 无回读；先显示 USB/未知 |
| SYS-007 | 低电量保护 | 中 | 有 ADC 才可精确；否则按运行时长/用户设置降级 |
| SYS-008 | 配网 | 高 | AP+Captive Portal+HTTP；串口 CLI 兜底 |
| SYS-009 | 闹钟/提醒 | 高（P1） | RTC + 定时器；深睡定时唤醒需权衡功耗 |
| SYS-010 | 天气/日期 | 中（P1） | 需后端/第三方 API；失败不显示伪数据 |
| SYS-011 | 手机配置 | 中（P1） | BLE 配网或网页；首版网页+串口 |
| SYS-012 | 书签/学习记录 | 中（P2） | SD/云端同步与隐私策略未定 |

### 4.5 主动陪伴

被动响应 + 轻量主动反馈：可行性高。默认不主动说话，仅眨眼/表情；夜间静默、提醒需显式设置。建议首版只做被动，降低打扰和功耗。

## 5. 豆包协议调研结论

### 5.1 两条链路

1. **官方 O2.0/SC2.0（推荐生产）**
   - URL：`wss://openspeech.bytedance.com/api/v3/realtime/dialogue`
   - Header：`X-Api-App-ID`、`X-Api-Access-Key`、`X-Api-Resource-Id: volc.speech.dialog`、`X-Api-App-Key: PlgvMymc7f3tQnJ6`、可选 `X-Api-Connect-Id`
   - 二进制帧：4B header + event id(4B) + session id len/body + payload len/body
   - 事件：StartConnection(1)/ConnectionStarted(50)、StartSession(100)/SessionStarted(150)、TaskRequest(200) 音频、TTSSentenceStart(350)/TTSResponse(352)/TTSEnded(359)、ASRInfo(450)/ASRResponse(451)/ASREnded(459)、ChatResponse(550)/ChatEnded(559)、FinishSession(102)、错误帧(0b1111)
   - 输入：PCM16/16k/单声道/小端，20ms=640B；输出：PCM16/24k（需 tts.audio_config.format=pcm_s16le）
   - 模型：O2.0 `model=1.2.1.1`；SC2.0 `model=2.2.0.0` + `character_manifest`
2. **demo 的 JSON 链路（可作回退）**
   - URL：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`
   - Header：`X-Api-Key`
   - 文本 JSON 事件 session.create / input_audio_buffer.append / response.output_audio.delta 等
   - 该链路在 demo 中已跑通，但并非 PDF 官方协议；生产需确认账号权限/合规性

**本项目策略**：协议封装在 `panda_voice`，默认实现官方 O2.0；保留 JSON 模式作为联调回退。所有凭证只从 NVS/网页/串口写入，不硬编码。

### 5.2 关键坑位

- 服务端会 gzip 压缩下行 JSON/文本；必须用 tinfl 正确解压（注意 gzip 头剥离后是 raw deflate，不能带 zlib 头标志）。
- WebSocket 客户端分片（buffer_size 1-4KB）会把一个事件拆成多片；必须按 payload_offset 重组。
- 上行必须持续供流：会话创建后停流可能触发 `AudioServerNoAudioInputTooLongError`；静音时用零 PCM 保持节奏或使用 `input_mod=keep_alive`。
- 播放时若继续开麦，模型会听到自身 TTS；首版播放时静音上行并停采集。
- 旧轮次迟到音频必须按 session/response id 丢弃，否则会插入新会话。
- 24k→16k 重采样不要用简单抽样；demo 的多相重采样可复用。
- 断线重连要指数退避并限制次数；鉴权失败不要无限重试。
- 大 JSON/音频缓冲放 PSRAM；内部 DMA 堆要留给 LCD/I2S/SD。

## 6. UI/动画技术路线

### 6.1 首版（P0）矢量熊猫脸

- 360×360 全屏白底，黑色眼斑、白眼球、黑瞳孔、高光、鼻子、嘴、腮红、眉毛。
- 所有元素为 LVGL 对象（圆角矩形/圆/线），不依赖外部资源；缺 SD 卡也能完整工作。
- 状态机驱动参数：眼睛开合、瞳孔位置、眉毛角度、嘴型、腮红透明度、呼吸缩放。
- Speaking 嘴型由播放 RMS 包络驱动（非文本字数）。
- 动画定时器 30ms，仅更新变化属性；待机降帧/停动画省电。

### 6.2 SD 序列帧（P1）

- 目录：`/sdcard/panda/manifest.json` + `animations/<state>/fNNN.r565`（RGB565 原始帧）或 RLE 压缩。
- PSRAM 双缓冲 2×253KB；SD 顺序读；帧率 8-12fps；缺帧/坏帧自动回退矢量脸。
- 主要风险：SD 随机读延迟、PSRAM 带宽、LVGL 全屏刷写；需实测再定帧率和压缩格式。
- 首版不把核心 UI 依赖 SD，避免“无卡变砖”。

## 7. 电源、热、声学与结构

- **续航**：需实测 6 种场景电流后再定电池容量；固件提供亮度/帧率/CPU 降频/自动休眠策略。
- **温升**：WiFi+功放+背光+CPU 并发是峰值；固件支持温度降级（后续接 NTC/内部温度传感器）。
- **声学**：双麦阵列与外壳开孔需整机复测；首版用单麦（MIC1）即可；播放与录音共用腔体，AEC 未验证前保持半双工。
- **结构**：55×55mm 屏 + 音频板 + 电池的 3D 堆叠必须 CAD 验证；开发板厚度/连接器不满足最终挂坠。

## 8. 里程碑实现计划（本工程）

- M0 硬件/工程骨架：引脚清单、串口 CLI、显示/触摸/音频/SD/RTC 自检。
- M1 语音闭环：按键 PTT、O2.0 鉴权/会话、PCM 上行、流式播放、打断、错误恢复。
- M2 熊猫角色：P0 表情、状态机、嘴型同步、缺卡兜底。
- M3 产品化基础：配网、电量（可选）、时间、亮度/音量、睡眠/低电策略、隐私反馈。
- M4 结构/可靠性：整机装配、声学、温升、续航、跌落/摩擦测试（需硬件团队）。

## 9. 待确认/待办

1. 用户连接真机（当前主机未检测到 `/dev/ttyACM*` 或 `/dev/ttyUSB*`），否则无法烧录验证。
2. 火山引擎控制台确认：开通的是 O2.0 还是 SC2.0；App ID/Access Key/Resource-Id/音色/并发/计费。
3. 是否允许改焊 GPIO1 电池 ADC 的 0R 电阻（会失去摄像头）。
4. 是否首版就要求 SD 序列帧动画；若资源未就绪，先用矢量脸。
5. 结构团队提供屏幕开孔/遮光/麦克风/扬声器/天线净空 CAD 反馈。
