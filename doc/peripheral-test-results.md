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
