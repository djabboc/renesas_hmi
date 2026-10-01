# 独立 MVP 重构回归（2026-10-01）

当前交付结构为 `src/test-main.c` + `src/test/` 下 45 个独立 C 文件；`src/hal_entry.c` 与重构前逐字节一致。以下是本轮新固件自测，后面的原始记录保留为历史证据，不自动等同于重构后的人工验收。

## 用户逐项验收：LED 与按键（2026-10-01）

证据来源：用户在本对话提供的串口日志；对应重构提交 `5b141e3`。本次仅整理记录，未重新打开 COM8 或运行板端测试。

| 项目 | 实际结果 | 验收范围 |
| --- | --- | --- |
| `gpio-led` | `WAIT code=1`，2174 ms，正常返回 `TEST IDLE` | P209/P210/P204 输出序列执行完成；用户尚未确认三颗 LED 的实际变化，视觉验收待补充 |
| `gpio-keys` | `PASS code=0`，15017 ms，正常返回 `TEST IDLE` | 三键按下/抬起检测通过，全部识别为 SHORT；长按识别尚未验证，事件次数与实际操作是否一致仍待用户确认 |

整理后的原始输出（仅去除空行和重复提示符）：

```text
msh >hmi_test gpio-led
TEST BEGIN gpio-led
LED 0 pin=P209 active=0
LED 1 pin=P210 active=0
LED 2 pin=P204 active=1
LED output sequence finished; visual confirmation required
TEST RESULT gpio-led WAIT code=1 elapsed=2174 ms
TEST IDLE

msh >hmi_test gpio-keys
TEST BEGIN gpio-keys
KEY levels P005=1 P006=1 P007=1 (pressed=0)
KEY 0 DOWN
KEY 0 UP SHORT
KEY 1 DOWN
KEY 1 UP SHORT
KEY 2 DOWN
KEY 2 UP SHORT
KEY 2 DOWN
KEY 2 UP SHORT
KEY 2 DOWN
KEY 2 UP SHORT
KEY 2 DOWN
KEY 2 UP SHORT
KEY 0 press=1 release=1
KEY 1 press=1 release=1
KEY 2 press=4 release=4
TEST RESULT gpio-keys PASS code=0 elapsed=15017 ms
TEST IDLE
```

首轮日志中 KEY 2 有四次完整短按，不能仅凭重复次数判断为抖动，也不能在未核对实际操作前宣称消抖通过。LED 目视结果和首轮操作次数仍待用户确认。

### 按键长按补测

用户随后提供以下日志，三个键均识别到完整长按，KEY 0 另有一次完整短按：

```text
msh >hmi_test gpio-keys
TEST BEGIN gpio-keys
KEY levels P005=1 P006=1 P007=1 (pressed=0)
KEY 2 DOWN
KEY 2 UP LONG
KEY 1 DOWN
KEY 1 UP LONG
KEY 0 DOWN
KEY 0 UP LONG
KEY 0 DOWN
KEY 0 UP SHORT
KEY 0 DOWN
KEY 0 press=3 release=2
KEY 1 press=1 release=1
KEY 2 press=1 release=1
TEST RESULT gpio-keys PASS code=0 elapsed=15019 ms
TEST IDLE
```

结论：结合首轮日志，三键短按、长按及按下/抬起识别均已有证据。本轮 KEY 0 最后一次 DOWN 在测试窗口结束前没有对应 UP，记录为未完成的一次操作，不据此判断硬件故障。源码固定采样 15 秒；PASS 只要求每个键至少检测到一次按下和一次释放，不要求最终累计次数相等，也不会等待最后一个键松开。是否在窗口结束后才松手尚未经用户确认。本次只更新验收文档，不修改固件。

## 用户逐项验收：LCD 五色与背光问题（2026-10-01）

用户确认“五色正常”，但反馈“背光变化不明显”。本次用户日志来自修复前的固件，整理如下：

```text
msh >hmi_test lcd-colors
TEST BEGIN lcd-colors
LCD color=F800
LCD color=07E0
LCD color=001F
LCD color=FFFF
LCD color=0000
LCD interrupts=138; color/brightness require visual confirmation
TEST RESULT lcd-colors WAIT code=1 elapsed=2156 ms
TEST IDLE

msh >hmi_test lcd-backlight
TEST BEGIN lcd-backlight
LCD brightness=0%
LCD brightness=20%
LCD brightness=40%
LCD brightness=60%
LCD brightness=80%
LCD brightness=100%
LCD brightness=80%
LCD brightness=60%
LCD brightness=40%
LCD brightness=20%
LCD brightness=0%
LCD interrupts=188; color/brightness require visual confirmation
TEST RESULT lcd-backlight WAIT code=1 elapsed=2936 ms
TEST IDLE
```

- 五色：程序流程结束，结合用户目视确认，本项验收通过。
- 背光：未通过视觉验收。排查发现 `configuration.xml` 将 P100 配置为 `gpt5.gtiocb`，但例程调用 `R_GPT_DutyCycleSet` 时误选 `GPT_IO_PIN_GTIOCA`。FSP 分别更新 A/B 的比较和强制占空比寄存器，因此调用返回成功也无法改变 P100；B 输出保留生成配置的初始 50% 占空比。GLCDC 中断仅证明显示扫描在运行，不能证明 PWM 变化。
- 修复：选择 `GPT_IO_PIN_GTIOCB`，启动定时器前设置 0%，每档观察时间由 250ms 延长到 1 秒，并每 20ms 检查停止请求。日志明确显示 P100/GTIOC5B 的请求占空比；人眼亮度不要求与占空比线性对应。
- 修复验证：RT-Thread Studio 构建 0 错误、0 警告，DAP-LINK 烧录完成；17 项主机回归通过。构建/烧录记录为 `logs/lcd-backlight-channel-fix-build.log`。本次未打开 COM8，未自动运行板端例程，留给用户观察实际明暗变化。
- 修复后的实物明暗变化已由用户复验确认，详见下方记录。

### 背光修复复验通过

固件对应提交 `cfcaf1b`。用户运行修复版后反馈“效果很好”，确认背光变化正常；结合以下完整阶梯输出，本项视觉验收通过。设备返回 WAIT 表示需要人工观察，本次人工确认已补齐，不修改固件中的结果语义。

```text
msh >hmi_test lcd-backlight
TEST BEGIN lcd-backlight
LCD P100/GTIOC5B duty=0%; observe for 1 second
LCD P100/GTIOC5B duty=20%; observe for 1 second
LCD P100/GTIOC5B duty=40%; observe for 1 second
LCD P100/GTIOC5B duty=60%; observe for 1 second
LCD P100/GTIOC5B duty=80%; observe for 1 second
LCD P100/GTIOC5B duty=100%; observe for 1 second
LCD P100/GTIOC5B duty=80%; observe for 1 second
LCD P100/GTIOC5B duty=60%; observe for 1 second
LCD P100/GTIOC5B duty=40%; observe for 1 second
LCD P100/GTIOC5B duty=20%; observe for 1 second
LCD P100/GTIOC5B duty=0%; observe for 1 second
LCD interrupts=719; color/brightness require visual confirmation
TEST RESULT lcd-backlight WAIT code=1 elapsed=11229 ms
TEST IDLE
```

背光通道选择错误已闭环；本次未测量 PWM 波形、背光电流或亮度线性度。记录来源为用户串口日志与目视反馈，助手未打开 COM8。

## 用户逐项验收：触摸信息（2026-10-01）

用户提供的串口输出如下；当前固件对应提交 `cfcaf1b`，后续提交仅更新文档。本次助手未打开 COM8。

```text
msh >hmi_test touch-info
TEST BEGIN touch-info
TOUCH addr=5D id=911 range=480x272
TEST RESULT touch-info PASS code=0 elapsed=119 ms
TEST IDLE
```

结论：GT911 信息读取验收通过，I²C 地址为 0x5D，芯片标识为 911，报告范围为 480×272，正常清理并退出。此项不验证触点坐标、松手、多指、中断或界面操作；这些项目继续单独验收。

### 触点轮询与五指数据复测

用户随后执行 `hmi_test touch-points`，返回 `WAIT code=1 elapsed=15131 ms` 并正常进入 `TEST IDLE`。日志中的关键过程如下（节选，省略重复帧；各段之间不表示连续采样）：

```text
TOUCH addr=5D id=911 range=480x272
TOUCH N=1 id=0 (237,145)
TOUCH N=0

TOUCH N=2 id=0 (279,109) id=1 (202,130)
TOUCH N=1 id=0 (279,109)
TOUCH N=0

TOUCH N=4 id=0 (303,82) id=1 (175,75) id=2 (246,56) id=3 (97,178)
TOUCH N=4 id=0 (303,82) id=1 (175,75) id=2 (246,56) id=3 (98,149)
TOUCH N=3 id=1 (175,75) id=2 (246,56) id=3 (98,149)
TOUCH N=2 id=1 (175,75) id=3 (98,149)
TOUCH N=1 id=3 (97,113)
TOUCH N=0

TOUCH N=5 id=0 (133,105) id=1 (243,55) id=2 (324,37) id=3 (402,87) id=4 (143,216)
TOUCH N=3 id=0 (133,105) id=1 (243,55) id=2 (324,37)
TOUCH N=0
TOUCH observed_points=237; position/multitouch accuracy requires interaction
TEST RESULT touch-points WAIT code=1 elapsed=15131 ms
TEST IDLE
```

结论：日志证明轮询能够读取 1～5 个触点，五指同帧 ID 为 0～4；移动期间坐标变化，部分手指释放后剩余 ID 得以保留，全部释放后报告 N=0。所报告坐标均在 480×272 范围内。`observed_points=237` 是各帧触点数量的累计值，不是点击次数或触摸帧数。重复坐标本身不构成故障证据，静止触点可以重复上报。

本项数据读取、多触点和释放上报检查通过；屏幕位置对应关系、边缘精度和跟手效果尚待可视化画板确认，触摸中断另由 `touch-irq` 验证。设备返回 WAIT 符合例程设计，不将本次结果扩大为全部触摸功能已通过。

### 触摸中断复测发现问题及修复

用户在固件 `cfcaf1b` 上执行 `hmi_test touch-irq`，多次点击、往返滑动和松手均有坐标记录，但 IRQ 计数为 0。本项验收不通过；原程序的 WAIT 不能作为中断正常的证据。以下为关键日志节选，省略重复帧和中间轨迹：

```text
msh >hmi_test touch-irq
TEST BEGIN touch-irq
TOUCH addr=14 id=911 range=480x272
TOUCH N=1 id=0 (244,186)
TOUCH N=0
TOUCH N=1 id=0 (174,197)
TOUCH N=1 id=0 (384,157)
TOUCH N=1 id=0 (152,197)
TOUCH N=1 id=0 (469,151)
TOUCH N=1 id=0 (238,174)
TOUCH N=0
TOUCH observed_points=108; position/multitouch accuracy requires interaction
TOUCH falling-edge IRQ count=0; touch the panel to generate edges
TEST RESULT touch-irq WAIT code=1 elapsed=15134 ms
TEST IDLE
```

源码排查确认：

1. 复位结束后调用 `rt_pin_mode(P004, PIN_MODE_INPUT)`，配置中缺少 `IOPORT_CFG_IRQ_ENABLE`，清除了 IRQ 引脚输入使能；此时 I²C 轮询仍可正常读取数据。
2. 当前 `drv_gpio.c` 的 `rt_pin_attach_irq` 只保存 mode，`rt_pin_irq_enable` 使用原生成配置打开 ICU，不应用该 mode。IRQ9 的生成配置实际是上升沿，原例程却打印为下降沿。
3. `rt_pin_mode` 会重新打开整个 IOPORT；复位时连续调用可能还原刚设置的另一个引脚。本例改用检查返回值的单引脚 FSP 配置，保留已验证画板例程的复位等待时间。0x14 和 0x5D 均为 GT911 合法地址，本次地址变化本身不是故障证据。

修复仅位于独立例程 `test-touch-irq.c`：复位后明确设置 P004 为输入并使能 IRQ；复制 IRQ9 配置，显式选择下降沿及本例回调，通过 FSP 打开、使能、关闭 ICU；回调仍只计数。线程继续轮询坐标用于对照，有触点且有 IRQ 才返回 PASS，有触点却无 IRQ 返回 FAIL，无触点保留 WAIT。此判定不证明逐帧中断驱动或位置精度。

修复版 RT-Thread Studio 构建 0 错误、0 警告，DAP-LINK 烧录完成；17 项主机回归通过，构建与烧录日志为 `logs/touch-irq-fix-build.log`。修复后的真实触摸中断计数仍待用户复验；本次未打开 COM8，不将代码检查或构建结果替代硬件验收。

## 构建与结构检查

- RT-Thread Studio：0 错误、0 警告，DAP-LINK 烧录成功。最终构建记录 `logs/mvp-build-delivery.log`。
- Flash 677944 字节，静态 RAM 468848 字节。全部 45 个线程入口均存在于 ELF；应用测试只有 `hmi_test` 一个 MSH 导出，无测试自动启动入口。
- 主机回归 17 项通过：串口分块、失败/超时处理、错误命令、USB 字节比较、文件/线程/命令一一对应、无跨例程入口调用、测试中无 MSH/线程创建/自定义测试头，以及 hal_entry 的 SHA-256。
- 新增脚本 `scripts/test_lifecycle.py` 验证动态线程生命周期，串口仅在有时限的测试批次中打开，退出即关闭。

## 可自动断言的实机结果

| 范围 | 本轮观察 |
| --- | --- |
| 网口 `eth-phy` | PASS，PHY ID `001C:C816` |
| 网口 `eth-mac` / `eth-phyloop` | PASS，各核对 12/12 帧，覆盖 60、512、1514 字节 |
| 网口 `eth-link` / `eth-lwip` | PASS；DHCP、DNS、TCP、HTTP 200 和正文 MATCH；lwIP → MAC → lwIP 连续切换通过 |
| `can-loop` | PASS，500 kbit/s 内部收发校验 |
| `audio-mic` | PASS，8192 帧 DMA 完成、左声道数据有变化；实际声学响应仍待说话/静音对照 |
| `rtc-tick` / `rtc-alarm` | PASS，日期/秒计数与单次闹钟事件符合预期 |
| `touch-info` | PASS，GT911、480×272 |
| `graphics-g2d` / `graphics-jpeg` | PASS，像素、尺寸及完成事件校验 |
| `rw007-info` / `rw007-wifi` / `rw007-ble` | PASS，固件查询、8 条 Wi-Fi 报告及 BLE 报告/扫描完成事件 |

RW007 复位后仍观察到首帧全 FF 并自动重试恢复，未隐藏该现象，不能宣称 SPI 零错误。

## 线程退出、停止与资源回收

- 原 256 字节 idle 栈在新 cleanup 日志路径中不足；调试时捕获并修复了调度锁内分配/打印断言及栈耗尽。最终改为短临界区撤销事件指针，解锁后释放/打印，并把 idle 栈设为 2048 字节，增加编译期最小栈检查。
- 三个界面 `lcd-lvgl`、`lcd-touch-lvgl`、`lcd-touch` 各运行并停止 3 次，均初始化成功并完成清理。
- 每次运行中连续发送另外两项测试请求，均被 BUSY 拒绝；停止后下一例程正常运行。主动停止记录 `FAIL code=-9`，属于已预期的取消结果，不当作功能通过。
- 每次停止后的线程列表不再包含 `hmitest`。三个完整循环结束后可用堆均为 **171584 字节**，未观察到随启动次数增长的泄漏。首次 LVGL 初始化前可用堆为 175272 字节；差额为库初始化/缓存常驻占用。
- idle 栈峰值为 15%；本轮基础/扩展批次观测到堆峰值 92880 字节。不存在常驻 `ptest` 工作线程。
- 生命周期记录：`logs/mvp-lifecycle.log`；最终交付固件再次复跑同样 9 次 GUI 启停，三轮堆仍为 171584 字节，见 `logs/mvp-lifecycle-delivery.log`。基础记录：`logs/mvp-hardware-baseline-03.log`；扩展记录：`logs/mvp-hardware-extended.log`。失败修正前日志仅用于定位，不计入通过证据。

## 待用户提供实物条件后验收

| 状态 | 项目与原因 |
| --- | --- |
| WAIT | `gpio-led`、`gpio-inputs`、`gpio-keys`：需要观察 LED、实际按键操作 |
| WAIT | `audio-tone`、`audio-replay`：播放采样数达到预期，声音内容/音质需试听 |
| WAIT | `lcd-colors`、`lcd-backlight`、三个界面例程：程序和生命周期已检查，图像/触摸手感需目视操作 |
| WAIT | `touch-points`、`touch-irq`：窗口内无人触摸，坐标和实际中断边沿需触摸复测 |
| WAIT | `rw007-adv`：命令被接受并观察 15 秒，手机可发现性需手机扫描 |
| WAIT | `adc-sample`：只报告悬空读数，不能证明精度 |
| SKIP | `sd-info`、`sd-read`、`sd-file`：未插 TF 卡，未实际读写文件 |
| SKIP | `usb-probe`、`usb-echo`：系统 USB 未枚举，未验证主机回显 |
| SKIP | `pmod-i2c`：外部 0x50 设备无响应 |
| 未执行外接条件验证 | `can-bus`、`gpio-loop`、`pmod-spi0/spi1/arduino/irq0/irq1`、`adc-low/high`：缺少已确认的对端、跳线或已知电压 |
| 需重测 | `rw007-internet`：本轮接收运行时凭据并完成扫描，但未发现用户热点 `Hotspot`，返回 `FAIL code=-1` 并清理退出；不声明重构版 Wi-Fi 联网通过。此前联网验收保留在历史记录中 |

热点失败后立即运行有线 lwIP、MAC/PHY 回环、JPEG、RW007 信息及触摸识别均正常；记录 `logs/mvp-internet-runtime.log` 与 `logs/mvp-final-regression.log`。网络凭据未写入源码，采集脚本对命令回显和异常消息脱敏。

最终烧录后再次验证提示音、录音回放、无卡信息路径和有线互联网访问，均符合 PASS/WAIT/SKIP 预期；见 `logs/mvp-delivery-smoke.log`。

实物接线、运行方法见 `peripheral-tests.md`；全部文件阅读目录见 `peripheral-code-quality.md`。所有未满足条件项保留待验收，未用编译成功替代硬件结论。

---

## 重构前历史记录

# 全外设测试实现与自测记录

日期：2026-10-01。板卡：HMI-Board V3.1 / RA6M3。测试方式：RT-Thread Studio 构建、DAP-LINK/PyOCD 烧录、COM8 有时限命令采集。按本轮要求不等待中途人工验收，直接交付代码与文档。

## 交付状态

P01～P15 的测试入口、分级命令、接线与判定标准已实现/整理，详见 `peripheral-test-taskbook.md` 和 `peripheral-tests.md`。默认唯一有效入口是 `INIT_APP_EXPORT(peripheral_test_start)`，启动后等待命令，不自动执行测试。`hmi_test all` 是基础批次，不代表所有阶段均执行。

**代码完成与硬件验收分开记录。** 本轮已执行测试没有遗留 FAIL；没有线缆、介质、对端或人工操作的阶段仍待最终复测。USB 未枚举的具体原因尚未确定，不能仅凭调试串口可用认定系统 USB 正常。

## 板测证据

| 任务 | 本轮实际结果 | 尚需最终复测 |
| --- | --- | --- |
| P01 网口 | 2026-10-01 功能验收完成：phy/mac/phyloop/link/lwip 五阶段 PASS，MAC/PHY 回环各 12/12 帧；无链路时有限等待返回 SKIP；插回后 link/lwip 恢复 PASS，100Mbps 全双工及真实互联网访问正常。 | 本轮规定的功能与拔插恢复项目已通过；未做长期稳定性/吞吐量测试。 |
| P02 TF | SDHI 打开及卡检测可执行，`card_inserted=0`；`read/file` SKIP。 | 插 FAT16/32 卡后依次 info/read/file，确认新建文件 8192 字节读回一致。 |
| P03 CAN | 内部回环 8 组载荷通过，最终 TX=1/RX=1/errors=0。 | CAN 收发器到外部节点尚未验证；500 kbit/s 对端回复 0x322。 |
| P04 麦克风 | SSI/DTC 接收 8192 个双声道帧完成，左声道数据有变化，PASS；约 16026 Hz。 | 说话/静音对比、增益与音质。DMA 完成不能代替音质验收。 |
| P05 扬声器 | 提示音输出计数 8000/8000，录音回放 8192/8192，WAIT。 | 接扬声器试听提示音和录音，确认无明显失真。 |
| P06 USB | USBFS 启动、5 秒枚举窗口及退出已执行；VBUS 读取 1，但 mounted_events=0，SKIP。 | 系统 Con4 数据线连接电脑，确认新 CDC 端口后执行二进制 ECHO/拔插；不能使用 ART-Link COM8 代替。 |
| P07 LED/按键 | 三 LED 输出序列执行；三按键空闲均为 1，15 秒采样窗口正常结束、无按压事件，WAIT。 | 逐灯观察、三个键短按/长按/抬起与消抖、实体复位键。 |
| P08 LCD/背光 | 五色输出 GLCDC 中断 138 次；亮度 0→100→0% 输出，中断 316 次；干净关闭，WAIT。 | 五色、亮度与原有 LVGL UI 视觉验收。 |
| P09 触摸 | GT911 ID 911、范围 480×272 通过；本轮回归地址 0x5D，坐标窗口没有人为触摸，WAIT。 | 单指/多指、边缘位置、松手、原画板与 LVGL 滑块交互。 |
| P10 RW007 | 固件信息 PASS：`RW007_2.1.0-a7a0d089-57`；Wi-Fi 扫描 8 条、BLE 扫描 5 条通过。广播命令接受并开放观察窗口，WAIT。 | 手机发现/连接复验；联网复用此前已验收例程，本轮未重新接入热点。通用 BLE 串口限制不变，ble-sound 不实施。 |
| P11 扩展口/ADC | A0 完成 32 次 ADC 转换，悬空读数 WAIT；外接 I²C 0x50 无应答，SKIP。 | GPIO、三路 SPI、两路 IRQ 跳线尚未接线板测；ADC GND/3.3V 和 I²C 对端读应答待测。 |
| P12 RTC | 32.768 kHz 晶振走时通过，读到测试日期 12:00:03；第 2 秒闹钟恰好 1 次，PASS。 | 长时间精度对照；无后备电池，不要求断电保持。 |
| P13 JPEG | 16×16 已知 JPEG 完成 16 行，status=A1，像素 0x8410；逐像素比较 PASS。 | 可选结合 LCD 观察；未声称覆盖全部 JPEG 格式。 |
| P14 D/AVE 2D | 硬件红色背景/绿色矩形，角点 F800、中心 07E0，逐像素校验及重复运行 PASS。 | 后续可重复运行观察稳定性。 |
| P15 调试接口 | SWD 烧录成功；UART9/COM8 收发命令和结果；每个采集进程退出释放端口。 | 外接调试排针与实体复位键需实物操作。 |

## 用户实物复测：P01 网口（2026-10-01）

证据来源：用户在本对话提供的完整串口输出；本次没有由助手重新打开 COM8 或运行测试。记录固件提交为 `97b1a5f` 对应测试套件，未因本轮日志修改固件。

| 阶段 | 关键输出 | 结论 |
| --- | --- | --- |
| `hmi_test eth phy` | MAC `02:48:28:27:8D:3C`；PHY `001C:C816`，BMCR=1000，BMSR=7849；144 ms | PASS |
| `hmi_test eth mac` | 60/512/1514 字节共 12/12 帧匹配，irq=21；214 ms | PASS |
| `hmi_test eth phyloop` | 同样 12/12 帧匹配，irq=21；215 ms | PASS |
| `hmi_test eth link` | link UP，speed/duplex enum=4（100Mbps 全双工），ECMR=00000066；1667 ms | PASS |
| `hmi_test eth lwip` | DHCP IP `192.168.0.102`，网关及 DNS `192.168.0.1`；3265 ms | PASS |

联网阶段解析 `www.msftconnecttest.com` 到 `23.205.151.12`，TCP 连接成功，HTTP status=200、received=187、expected_body=MATCH；统计 tx=13、rx=8、drops=0，输出 `INTERNET PASS result=0`。drops=0 是此测试接收路径的计数，不代表整个网络不存在丢包。五个阶段均正常返回 `TEST IDLE`。

后续用户日志：`hmi_test eth link` 和 `hmi_test eth lwip` 均输出 `ETH no physical link`，在 6156 ms 后返回 `SKIP code=2` 和 `TEST IDLE`。无链路时有限等待并正常退出的行为符合预期；日志本身不能确认第二次命令前是否已插回网线。

恢复阶段用户日志：`hmi_test eth link` 在 1667 ms 后 PASS，链路 UP，speed/duplex enum=4（100Mbps 全双工），ECMR=00000066。随后 `hmi_test eth lwip` 在 3279 ms 后 PASS，重新取得 IP `192.168.0.102`，网关/DNS `192.168.0.1`，域名解析为 `23.205.151.12`；TCP 连接成功，HTTP status=200、received=187、expected_body=MATCH，tx=12、rx=8、drops=0，输出 `INTERNET PASS result=0`。两条命令均返回 `TEST IDLE`。

结论：P01 本轮功能验收完成，覆盖 PHY 识别、MAC/PHY 回环、物理链路、DHCP/DNS/TCP/HTTP、无链路超时退出以及插回后重新执行命令恢复联网。该例程每次重新建立连接，本轮没有验证后台自动重连、长期稳定性或吞吐量。

## 已定位并修复的问题

1. GLCDC Stop 请求在下一帧边界才完成。立即 Close 曾失败；改为有限重试等待停止，并保留静态配置对象，复测颜色/背光均能退出。
2. 小 JPEG 使用输入计数模式时头部预取导致输入暂停。改为对齐 SRAM 缓冲及完整输入模式，实测全部输出像素通过。
3. SSI 的 24 位采样右对齐；补上显式符号扩展和回放位宽换算。采样统计不再把负样本当作正值。
4. D/AVE 使用 RT-Thread 堆；在供应商 flush 前加入硬件忙状态超时检查。若超时保留 DMA 可能引用的内存并要求复位，避免释放后仍被硬件访问。
5. USB 补全 P407 外设复用和失败退出清理；枚举尚未通过，不将此修改称为已验证的 USB 功能修复。
6. GPIO 测试恢复按键配置；LED 日志改为与原理图一致的 P209/P210/P204 表示。
7. 串口调度器在输出 TEST IDLE 前解除 busy，避免连续命令误判忙；串口脚本使用增量 UTF-8 解码，并以上下文管理器释放端口。

## 构建、切换与资源检查

- `lcd-touch-lvgl` profile 切换构建通过，0 errors / 8 warnings；Flash 633140 B，静态 RAM 376264 B。本轮只验证该 profile 的构建，没有重新人工验收 UI。
- 已切回默认 `peripheral` profile；最终构建 0 errors / 8 warnings，Flash **298076 B**、静态 RAM **355424 B**，PyOCD 烧录成功。ELF 只包含当前套件的应用初始化入口，另有必要的系统初始化入口。
- 8 个警告均来自被注释 INIT_APP_EXPORT 的例程入口函数未使用，与“一次一个例程”的要求相符。
- 集成批次与补充阶段前后可用堆均为 276256 B，最大使用量 89144 B；ptest 的 12288 B 栈峰值 20%。这些有限运行中没有观察到堆持续下降，不代表长期压力测试。
- Python 脚本语法检查、Git 空白检查以及唯一应用初始化入口检查完成。

本机原始证据（`logs/` 不提交，关键结果在本文件归档）：

- `logs/peripherals_20261001_030215.log`：初始 Ethernet/CAN/G2D；包含修复前 JPEG 失败。
- `logs/peripherals_20261001_030236.log`：初始 RTC、音频等；包含修复前 LCD 退出失败。
- `logs/peripherals_20261001_030542.log`：JPEG/LCD 修复复测、音频计数、RW007 扫描。
- `logs/peripherals_20261001_031829.log`：最终集成批次、PHY 回环、RTC 闹钟、lwIP/SD/I²C 前提检查。
- `logs/peripherals_20261001_031918.log`：JPEG/G2D 重跑、按键/触摸窗口、背光、广播及资源检查。
- `logs/peripheral-profile-lvgl.log`：原 LVGL profile 构建。
- `logs/peripheral-build-final.log`：最终默认 profile 构建/烧录。
- `logs/peripherals_20261001_032230.log`：最终固件 LED/按键恢复配置与 USB 连续两次启动/退出复测；仍为 WAIT/SKIP，无 FAIL，COM8 已释放。

## 用户集中复测顺序

2026-10-01 在继续 TF 验收前，按用户要求先完成代码可读性整改；重构内容、构建与独立回归证据见 `peripheral-code-quality.md`。重构版已重新验证网口回环及真实互联网访问，原验收记录保留。

按使用手册从每项底层阶段开始，不跳过前置检查：网口 PHY→MAC→PHY loop→link→lwIP；TF info→read→file；CAN loop→bus；USB probe→echo；随后音频、按键/LED、屏幕/触摸、RTC、扩展口、RW007。准备网线/路由器、FAT 卡、CAN 对端、系统 USB 数据线、扬声器与跳线。无需更改测试源码，持续交互 UI 则使用 profile 选择脚本构建切换。
