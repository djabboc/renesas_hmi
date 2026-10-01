# HMI-Board 全外设测试使用手册

适用：HMI-Board V3.1、RA6M3、RT-Thread Studio、480×272 ST7282/GT911。任务范围见 `peripheral-test-taskbook.md`，实际自测结果见 `peripheral-test-results.md`。

## 构建、运行及结果含义

```powershell
python scripts/select_example.py peripheral
python scripts/build_flash.py
```

默认只启用 `src/peripheral-test.c` 中的 `INIT_APP_EXPORT(peripheral_test_start)`。上电不自动测试，MSH 命令将任务投递给唯一 `ptest` 线程，各项串行运行，不在串口命令线程中直接操作外设。该配置下主线程暂停原来的 LED 闪烁，避免干扰 LED 测试。

```text
hmi_test help
hmi_test all
hmi_test status
hmi_test stop
```

- `all` 是无需接跳线的基础批次：网口 MAC 回环、SD 只读、CAN 内部回环、麦克风采集、USB 枚举尝试、LED、RTC 走时、ADC 采样、LCD 五色、触摸身份、G2D、RW007 信息。**不是所有阶段的完整验收**；进一步阶段按下面命令逐项执行。
- `PASS`：该阶段的具体断言通过；例如 MAC 回环通过不代表 RJ45 到路由器的路径通过。
- `WAIT`：程序已执行，需视觉/听觉/按键/已知电压/主机比对等验收。
- `SKIP`：未找到必需条件，例如无 TF 卡、未建立网线链路、USB 未枚举。USB 未枚举本身不能区分未接线与兼容性问题。
- `FAIL`：驱动错误、内容不一致或超时。按输出的阶段排查，不忽略失败。
- `status` 的每项记录是本次启动以来该设备**最后运行的阶段**，不是该设备所有功能的总体结论；完整历史保存在串口日志。
- `stop` 请求取消，在当前轮询/阶段检查点生效。FSP 内部个别硬件等待不保证可抢占；硬件故障导致命令无响应时需复位。G2D 超时会保留仍可能被 DMA 引用的内存并明确要求复位。

自动采集工具有时间上限，结束/异常均关闭 COM8，不启动常驻监视器：

```powershell
python scripts/test_peripherals.py
python scripts/test_peripherals.py --command "hmi_test eth phy" --command "hmi_test eth mac" --command "hmi_test eth phyloop"
python scripts/test_peripherals.py --command "hmi_test eth lwip" --timeout 90
```

日志位于 `logs/peripherals_日期_时间.log`（不提交原始日志）。脚本退出码 0 表示没有检测到 FAIL/脚本错误，**WAIT/SKIP 仍需复测**。正在自行使用 COM8 终端时，先关闭终端再运行脚本。

## P01 有线网口：PHY → 回环 → lwIP

源码：`src/test-ethernet.c`，应用层复用 `src/rw007-internet.c` 的单线程 raw lwIP 流程。物理数据通过 RA6M3 Ethernet MAC/EDMAC、RMII、RTL8201F，独立于 RW007。

依次执行：

```text
hmi_test eth phy
hmi_test eth mac
hmi_test eth phyloop
hmi_test eth link
hmi_test eth lwip
```

1. `phy`：硬复位 PHY，通过 MDIO 读取 ID/BMCR/BMSR，拒绝全零/全 FF ID。实测 ID 为 `001C:C816`。
2. `mac`：MAC 内部回环，实际 DMA 发送/接收 12 帧，分别覆盖 60/512/1514 字节，逐字节校验。无需网线。
3. `phyloop`：PHY BMCR 数字回环，固定 100M 全双工，实际经 RMII 收发并校验同样的 12 帧。无需短接 RJ45；不验证网线、变压器到对端的模拟链路。
4. `link`：接路由器的 LAN 口，等候物理链路与协商。输出速度/双工枚举及 ECMR；确认 LINK 灯，ACT 灯在通信时闪烁。
5. `lwip`：取得 DHCP 地址 → DNS → TCP → HTTP，访问 `http://www.msftconnecttest.com/connecttest.txt`。只有 HTTP 200 且响应正文匹配 `Microsoft Connect Test` 才通过。不是 HTTPS 或长期稳定性测试。

网口使用 MCU 唯一 ID 派生的本地管理 MAC，4 个 RX/2 个 TX 描述符。回环阶段只为 FSP 提供强制速率/链路配置，PASS 必须收到正确数据，不能靠强制的链路状态判定通过。每项结束关闭 MAC，回环设置被清除。

最终拔插验收：拔掉网线运行 `link`，应在约 6 秒后 SKIP；重新插入后重跑 `link`、`lwip`，应恢复成功。该例程每次重建连接，不提供后台自动重连服务。

## P02 TF 卡：介质 → 只读 → FAT 文件

源码：`src/test-sd.c`、`src/test-fatfs.c`。使用已有 FatFs 引擎和 FSP SDHI1/DMAC，不启动另一个 DFS/SD 后台线程。

```text
hmi_test sd info
hmi_test sd read
hmi_test sd file
```

- `info`：卡检测、初始化、扇区数、512 字节扇区大小、时钟和写保护。
- `read`：对扇区 0 连续读取两次并比较；不会写入卡。
- `file`：需已格式化为 FAT16/FAT32 的卡。按顺序用 `FA_CREATE_NEW` 尝试 `HMI00.TST`～`HMI99.TST`，不覆盖已存在的文件。写入 8192 字节已知模式，sync/close，卸载并重新挂载，读回全部内容和长度校验。文件保留供电脑检查，测试后可自行删除。
- 无卡为 SKIP；无可用文件名或文件系统不支持会报错，**不会自动格式化、删除或裸写扇区**。
- 每项关闭 SDHI；最终可拔卡重跑 `info`，插回后重跑 `file`。不要在写文件时拔卡。

## P03 CAN：内部回环 → 对端收发

源码：`src/test-can.c`。统一使用 **500 kbit/s，标准数据帧，8 字节**。

```text
hmi_test can loop
hmi_test can bus
```

`loop` 使用控制器内部回环，8 组不同载荷，逐字节验证 ID、帧类型、DLC、数据及 TX/RX 事件，无需外接节点。

`bus` 需要 USB-CAN 或另一节点：CANH 对 CANH、CANL 对 CANL、共地，确认两端终端匹配。板载已有约 120Ω 分裂终端，勿无条件额外叠加终端。

板端发送 ID `0x321`，数据 `00 01 02 03 04 05 06 07`；对端收到后，在 10 秒内回复 **ID `0x322`、相同 8 字节**。须同时观察到 TX 完成、正确回复及零错误事件。对端应处于正常模式以提供 ACK。无 ACK/超时是 FAIL，不能当作内部回环通过后的“自动通过”。

## P04/P05 音频：采集 → 输出 → 回放

源码：`src/test-audio.c`。数字麦克风在 I²S 左声道；FSP 的 24 位数据右对齐，程序显式符号扩展。

```text
hmi_test audio mic
hmi_test audio tone
hmi_test audio replay
```

- `mic`：GPT1 提供音频时钟，经 SSI/DTC 接收 8192 个双声道帧，实际采样率约 16026 Hz。检查接收完成和左声道有变化，输出最小/最大值；约 0.51 秒。采样数据有变化不能单独证明拾音品质，最终需静音/说话对比。
- `tone`：GPT6 PWM 载波、GPT2 采样更新，输出约 0.5 秒低幅方波提示音；检查输出样本数后返回 WAIT，需接扬声器试听。
- `replay`：先采集约 0.51 秒，再降低幅度回放。命令开始时发声，观察能否听到自己的声音；此演示不保存 WAV，不做连续录放。
- PWM 两桥臂围绕 50% 作有限差分变化；退出后停止两个定时器并将两控制脚置低。

扬声器接口是本地 PWM 音频，不代表 RW007 支持 A2DP；已取消的 ble-sound 不在本轮重新实施。

## P06 USB Device：枚举 → 二进制 ECHO

源码：`src/test-usb.c`、`board/tusb_config.h`；TinyUSB 0.17.0 的 RUSB2 USBFS + CDC。

使用接 RA6M3 的**系统 USB Type-C**（原理图 Con4）；ART-Link 调试 USB（Con5）和 COM8 是另一条通路。系统接口接电脑后应出现 `HMI USB CDC Echo` / 新的串口。

```text
hmi_test usb probe
hmi_test usb echo
```

`probe` 等待枚举最多 5 秒。`echo` 在枚举后提供 30 秒回显窗口，原样回显收到的字节，不进行换行转换。设备输出 RX/排队 TX/发送完成事件统计；最终接收正确性由电脑比对，设备侧返回 WAIT。

知道新串口号后可使用自动主机比对，例如新端口为 COM12：

```powershell
python scripts/test_peripherals.py --command "hmi_test usb echo" --usb-port COM12 --timeout 45
```

工具发送全部 0～255 字节及跨包数据，包含 NUL、FF 和 CR/LF，并逐字节比较回显。`--port COM8` 是控制口，`--usb-port` 必须是系统 USB 新端口。重复拔插后重新执行。测试结束设备断开 CDC 并关闭 USBFS。

描述符使用示例 VID/PID `CAFE:4001`，仅作本板开发测试。USBFS IRQ 40、P407 VBUS 和 48 MHz USB 时钟属于接入要求。

## P07 按键、LED、复位和调试串口

源码：`src/test-gpio.c`。

```text
hmi_test gpio inputs
hmi_test gpio led
hmi_test gpio keys
```

- `inputs` 显示 P005/P006/P007 电平，按下为 0。
- `led` 依次驱动 P209/P210 两个低有效红灯和 P204（Arduino D13）的高有效蓝灯；人工确认逐个变化，返回 WAIT。
- `keys` 开放 15 秒窗口，每 5 ms 采样、4 次稳定后确认变化；输出按下/抬起、短按/长按（800 ms 分界）和计数。三个键都观察到完整按下/抬起才 PASS，长按/消抖效果仍需人工检查。
- 复位键只作人工测试：按下后应重新输出 RT-Thread 启动信息及测试调度器就绪，状态清零。程序不替代物理复位键测试。
- ART-Link SWD 下载、UART9/COM8 命令及回传由构建烧录和有时限的串口脚本验证；外接调试器排针仍需实物连接。

## P08/P09 LCD、背光和触摸

源码：`src/test-display.c`（LCD）、`src/test-touch.c`（GT911）。

```text
hmi_test lcd colors
hmi_test lcd backlight
hmi_test touch info
hmi_test touch points
```

`colors` 显示红绿蓝白黑，检查 GLCDC 中断持续产生；`backlight` 在五色后显示白色并执行 0→100→0% 的 PWM 阶梯。两项均需视觉验收，结束后等待 GLCDC 在帧边界停稳并关闭背光。

`info` 复位并读取 GT911 身份和 480×272 范围；`points` 持续 15 秒读取最多 5 个触点、ID、坐标并清除数据就绪标志，检查坐标界限。视觉位置、抬手、多点轨迹仍按已有画板/LVGL 例程复验。

原有长期交互例程可切换：

```powershell
python scripts/select_example.py lcd-touch
python scripts/build_flash.py
```

可选 profile：`peripheral`、`lcd`、`touch`、`lcd-touch`、`lcd-lvgl`、`lcd-touch-lvgl`、`ble`、`rw007`。所有其他 INIT_APP_EXPORT 会被注释。切换是源码修改，必须重新构建烧录；不会在当前运行中启动第二个持续例程。原用法分别见已有 LCD/触摸/LVGL 文档。

## P10 RW007

统一调度器提供独立有限测试：

```text
hmi_test rw007 info
hmi_test rw007 wifi
hmi_test rw007 ble
hmi_test rw007 adv
```

复用已验证的原驱动，每项复位、执行、关闭；`adv` 保持约 15 秒供手机观察，返回 WAIT。固件初始全 FF SPI 应答会重试，日志保留该现象。

带热点凭据的联网测试仍切换 `rw007` profile，使用已有 `rw007_demo internet <ssid> <password>` 或 `scripts/test_rw007_internet.py`，不在源码保存密码。详见 `rwoo7-demo.md`。BLE 通用串口/遥控仍受模块固件限制，参见 `ble-capabilities.md`。

## P11 Arduino/Pmod/ADC 扩展接口

源码：`src/test-pmod.c`、`src/test-gpio.c`、`src/test-adc.c`。以下回环**先按表接跳线**，测试时取下会驱动同一线路的外接模块。建议 GPIO 输出到输入串联 1 kΩ，使用板上 3.3 V 电平。

| 命令 | 接线 | 校验 |
| --- | --- | --- |
| `hmi_test gpio loop` | Arduino D2/P008 → D9/P009 | 16 次高低电平逐次比对 |
| `hmi_test pmod spi0` | Pmod0 J1 的 MOSI/P305（2脚）→ MISO/P304（3脚） | SCI6，1 MHz，8×64 字节回环 |
| `hmi_test pmod spi1` | Pmod1 J2 的 MOSI/P613（2脚）→ MISO/P614（3脚） | SCI7，同上 |
| `hmi_test pmod arduino` | Arduino D11/P512 → D12/P511 | SCI4，同上；D13/P204 为 SCK，D10/P712 为 CS |
| `hmi_test pmod irq0` | Pmod0 GPIO0/P211（8脚）→ IRQ/P708（7脚） | IRQ11，16 个双边沿中断 |
| `hmi_test pmod irq1` | Pmod1 GPIO0/P710（8脚）→ IRQ/P709（7脚） | IRQ10，同上 |
| `hmi_test pmod i2c` | Arduino SCL/P202、SDA/P203 接 3.3 V I²C 设备，7 位地址 0x50，并共地/合适上拉 | 只读 1 字节应答；不校验该设备内容 |
| `hmi_test adc sample` | A0/P000，可先悬空观察 | 32 次转换、极值、均值，仅 WAIT |
| `hmi_test adc low` | A0/P000 接 GND | 所有读数 <100 |
| `hmi_test adc high` | A0/P000 接板上 3.3 V | 所有读数 >3995 |

ADC 为 12 位，估算电压假设参考为 3.3 V；输入限于 0～3.3 V，不能接 5 V。这里只验证 A0 通道，其他模拟脚不是逐脚产测。P202/P203 与 GT911 共用 I²C，因此不同时运行持续触摸例程。Pmod IRQ 与部分用户按键复用中断通道，调度器串行运行并关闭 IRQ。

## P12 RTC

```text
hmi_test rtc tick
hmi_test rtc alarm
```

使用板上 32.768 kHz 晶振。每次设为测试日期 `2026-10-01 12:00:00`，约 3.2 秒后读取并验证走时；`alarm` 同时验证第 2 秒闹钟中断恰好一次。结束关闭 RTC 并停止计数。**这是会改写 RTC 时间的测试，不是实时时钟应用。**

VBATT 在原理图中接 3.3 V，没有独立后备电池，不把断电保持作为验收条件。更长时间的精度比较需外部时钟。

## P13/P14 JPEG、2D 加速

```text
hmi_test graphics jpeg
hmi_test graphics g2d
```

- JPEG：内置 16×16、RGB(128,128,128) 的已知 baseline JPEG。复制到对齐的 SRAM 输入缓冲并采用完整输入模式，验证尺寸、16 行完成事件、全部 RGB565 像素（允许一个量化步差异）。实测像素 `0x8410`。
- G2D：硬件对 16×16 RGB565 缓冲填红色，再绘制中央 8×8 绿色矩形，逐像素检查。渲染完成轮询有上限；正常退出释放 D/AVE 缓冲及设备，使用 RT-Thread 堆。
- 这两项可独立于 LCD 视觉效果判定硬件计算正确；没有据此声称视频播放或所有图形操作已覆盖。

## 维护和构建注意事项

源码职责、注释约定、格式检查与主机回归方法见 `peripheral-code-quality.md`。

`scripts/configure_peripheral_tests.py` 补全 Studio 头文件搜索路径；根 `SConscript` 声明 USB 源码。RTC/ADC 驱动与原工程 FSP v3.5.0 一致；TinyUSB 来源/许可见 `third_party/tinyusb/README.hmi.md`。

`ra_gen/vector_data.c/.h` 新增 USBFS 40、RTC alarm 41、RTC carry 42。FSP 重新生成后须保留/重新配置这三个中断，不能只看 C 文件编译通过。没有全局启用 BSP_USING_ETH/SDHI 等自动初始化，防止与测试直接使用 FSP 控制器冲突。

恢复默认测试集合并提交前，应选择 `peripheral`、构建、确认只有一个有效 INIT_APP_EXPORT。最终逐项验收前按任务书准备网线/路由器、FAT 卡、CAN 对端、系统 USB 数据线、扬声器与跳线。
