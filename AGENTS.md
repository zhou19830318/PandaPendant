# Panda AI 多模型协作开发规则（必读）

本仓库由多个 AI 工具/模型接力开发。**每一次会话都必须按本文件执行**：
开工前读进度，收工前做验证并更新文档。文档不是附属品，是交付物的一部分。

## 0. 项目一句话

基于 Waveshare ESP32-S3-AUDIO-Board + 1.85" ST77916 触摸屏的熊猫智能挂坠固件，
ESP-IDF v5.5 + LVGL 9 + 豆包实时语音。产品需求见 PRD，可行性见 `docs/feasibility.md`。

## 1. 开工前（每次会话，按顺序）

1. 读 `docs/PROGRESS.md`（当前快照 + 最近交接日志）。
2. 读 `docs/TODO.md`（任务清单与状态）。
3. 读 `docs/tech_notes.md`（已验证的坑位，避免重复踩坑）。
4. 需要时再读 `docs/feasibility.md` 和 PRD，不要一上来通读 `demo/`。
5. 在 `docs/PROGRESS.md` 的“当前会话”区登记：日期、工具/模型、一句话目标、验收标准。
6. 先跑一次基线验证（见第 5 节）。**不要假设上次的构建/硬件状态仍然成立。**
7. 一次只认领一个功能（`docs/TODO.md` 中的一项），不顺手做大重构。

如果发现上一位留下的“当前会话”未收尾：不要删除，先按现状补记验证结果或标记 `[!]`。

## 2. “完成”的定义（DoD）

一个功能只有同时满足以下条件才算完成：

- [ ] 代码实现，且 `idf.py build` 通过（记录 bin 大小与分区余量）。
- [ ] 做了与改动相称的运行验证：串口 CLI、HTTP 页面、真机日志或自动化测试。
- [ ] `docs/TODO.md` 状态已更新，`[x]` 必须能指向具体验证证据。
- [ ] `docs/PROGRESS.md` 已更新快照，并追加一条交接日志（命令 + 实际结果）。
- [ ] `docs/tech_notes.md` 记录了新踩的坑/新实测结论（没有就写“无新增”）。
- [ ] 用户可见的命令/行为/限制变化，已同步到 `README.md`。

无法真机验证时（例如屏幕未接、无凭证）：状态标 `[~]`，并在日志中写清
“缺什么、怎么补验、预期看到什么”。**不允许把未验证项标成 `[x]`。**

## 3. 状态图例（三个文档统一）

| 标记 | 含义 |
|---|---|
| `[ ]` | 未开始 |
| `[~]` | 代码完成/编译通过，尚未完整验证（必须写明缺口） |
| `[x]` | 已完成并有验证证据（记录日期/命令/结果） |
| `[!]` | 阻塞（写明阻塞原因与解除条件） |

## 4. 收工前必须更新的文件

| 文件 | 更新内容 |
|---|---|
| `docs/TODO.md` | 任务状态、验收标准、阻塞项、真机验证记录、构建状态 |
| `docs/PROGRESS.md` | 顶部“当前状态/最近验证”、当前会话、下一步、交接日志 |
| `docs/tech_notes.md` | 新踩坑、引脚/时序/内存等实测结论，按主题追加 |
| `README.md` | 用户可见的构建/烧录/CLI/行为/已知限制变化 |

`docs/PROGRESS.md` 的日志**新的在最上面**，单条保持简短（10 行以内），
历史记录只追加不删改。超过约 30 条时，把旧记录移到
`docs/progress_archive_YYYY.md` 并在原文件留一行索引。

## 5. 验证手册

环境：`source /home/conor/esp32/esp-idf/export.sh`（路径不同则换成本机 IDF），
工程目录 `panda_ai/`。当前开发板串口：`/dev/ttyACM0`（USB-SERIAL-JTAG，115200）。

```bash
# 1) 构建（必须）
source /home/conor/esp32/esp-idf/export.sh
cd panda_ai && idf.py build
# 关注最后两行：panda_ai.bin binary size ... free

# 2) 烧录 + 人工观察（需要 TTY）
idf.py -p /dev/ttyACM0 flash monitor

# 3) 自动化串口冒烟（无需 TTY；关闭 DTR/RTS，并丢弃可能的启动日志）
python3 - <<'PY'
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3)
s.dtr = False; s.rts = False
time.sleep(0.3); s.reset_input_buffer()
for cmd in ('status', 'wifi status', 'heap'):
    s.write((cmd + '\r\n').encode()); time.sleep(1.0)
    print('===', cmd); print(s.read(4096).decode('utf-8', 'replace').strip())
s.close()
PY

# 4) 配网页面（无 WiFi 凭证时设备开 AP）
#    SSID: CONFIG_PANDA_WIFI_AP_PREFIX-XXXX，密码 CONFIG_PANDA_WIFI_AP_PASSWORD
#    浏览器打开 http://192.168.4.1/ ，/api/status 可查状态
```

验证结论必须记录**实际输出**（关键日志行、尺寸、返回值），不能只写“已测试”。

## 6. 交接日志模板（复制到 `docs/PROGRESS.md` 顶部）

```markdown
### YYYY-MM-DD HH:MM · <工具/模型> · <任务名>
- 目标：一句话
- 结果：完成 / 部分完成 / 阻塞
- 改动：文件列表（一句话说明）
- 验证：
  - `idf.py build` → 通过，bin 0x…（剩余 …%）
  - 串口 `status` → …（关键输出）
- 未验证 / 风险：缺什么、怎么补验
- 下一步：给下一位的具体动作
- 阻塞：无 / 具体原因
```

## 7. 禁止事项

- **禁止提交、打印或写入文档任何真实密钥**。`doubao_api_key.txt`、NVS 中的
  `api_key` 一律不出现；`settings show` 已做脱敏，不要绕过。
- 不修改 `demo/`（厂商/参考代码，只读）；产品代码只在 `panda_ai/`。
- 不手改 `panda_ai/sdkconfig`（被 gitignore，属本机生成物）；
  需要固化配置时改 `panda_ai/sdkconfig.defaults` 或 `Kconfig.projbuild`。
- 不用 `idf.py monitor` 做自动化验证（需要 TTY），用上面的 pyserial 方式。
- 不伪造验证结果，不把阻塞项写成完成，不删除他人的未完成会话记录。

## 8. 常用事实（会随进度变化，以 PROGRESS 为准）

- 屏幕、触摸、喇叭、电池与 SD 卡均已连接；电池 ADC 分压电阻 R53/R78 未焊，
  GPIO1 悬空，电量显示“未知”。
- 功放（PA）只在写 PCM 时开启，空闲 800ms 后静音 DAC 并关 PA（勿扰立即关断）；
  PA 通电本身有底噪，播放中的残余滋滋声属硬件，不是软件丢帧。
- 语音生产协议为豆包 O2.0 二进制（`api_mode=o2`，已真机联通）；JSON 链路
  保留为默认回退。
- 任一网络（STA 或 AP）在线时配网页都会自动启动；无 WiFi 凭证时开机进入
  APSTA 并开放 `192.168.4.1` 配网，STA 恢复后 AP 自动关闭。
- 内部 RAM 启动后约 80–110KB（以 `status`/`heap` 为准），音频/JSON/环形缓冲
  必须放 PSRAM。
