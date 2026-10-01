> 结构迁移说明（2026-10-01）：本文保留原例程的实现与验收历史。当前单文件位置和 MSH 命令已统一迁移，使用方法见 `peripheral-tests.md`，完整映射见 `peripheral-code-quality.md`；不要再照旧文启用 INIT_APP_EXPORT 或常驻线程。

# RW007 固件功能与互联网测试例程

验收日期：2026-10-01。用户已确认验收通过。

源文件按用户指定命名为 `src/rwoo7-demo.c`（字母 o），实际模块型号及串口命令为 RW007 / `rw007_demo`（数字 0）。

## 功能和启动

当前只启用 `INIT_APP_EXPORT(rwoo7_demo_start)`，其他例程的 `INIT_APP_EXPORT` 均已注释。上电自动执行信息查询、Wi-Fi 扫描、BLE 被动扫描、60 秒 BLE 广播；互联网测试需要显式传入热点凭据后执行。

每项测试会重新初始化模块，断开此前连接，避免扫描和广播状态互相干扰。所有 SPI 操作和 lwIP 调用都在一个 RW007 工作线程中串行执行，MSH 命令通过邮箱提交任务。

| 命令 | 功能 | 成功依据 |
| --- | --- | --- |
| `rw007_demo status` | 查询运行与最近测试结果 | 查看状态，不执行新测试 |
| `rw007_demo all` | 顺序重跑上电四项测试 | 四项分别返回 PASS；不包括互联网测试 |
| `rw007_demo info` | 查询固件版本、序列号、Wi-Fi MAC | 命令成功且返回长度有效 |
| `rw007_demo wifi` | 扫描 Wi-Fi 热点 | 返回成功的扫描完成事件；另外统计实际扫描报告 |
| `rw007_demo ble` | 查询 BLE 地址，进行 3 秒被动扫描 | 返回扫描完成事件且 reason=0；另外统计实际广播报告 |
| `rw007_demo adv` | 开启 60 秒内置 BLE 配网广播 | 命令获确认，手机实际发现和连接另行验证 |
| `rw007_demo internet <ssid> <password>` | 连接指定热点并执行 DHCP、DNS、HTTP 测试 | HTTP 200 且响应体开头匹配固定测试文本 |

扫描最多打印前 24 条报告，统计数可超过打印数。BLE 报告数不等于设备数，同一设备可能多次出现；被动扫描未取得设备名称时显示 `<unnamed>`。没有报告但扫描正常结束时，会另外提示未验证接收能力。

忙时命令提示稍后重试；非法命令和多余参数只打印帮助。

## 硬件与固件

| 信号 | 引脚 |
| --- | --- |
| CS / MISO / MOSI / CLK | P308 / P309 / P310 / P311 |
| RESET | P312 |
| INT | P015 / IRQ13 |

使用 FSP SCI3，模式 0、1 MHz。模块复位时 CS 保持输入，等待 INT 就绪后配置 SPI，参考本工程已验证的 `ble.c`。两阶段 SPI 传输检查包头、序号和长度，使用有界等待。

本板实测版本为 `RW007_2.1.0-a7a0d089-57`，Wi-Fi MAC 为 `FC:58:4A:A6:ED:30`，BLE 广播名称为 `RW007-ED30`。程序没有升级模块内部固件。

协议依据为 [官方 RW007 驱动固定版本](https://github.com/RT-Thread-packages/rw007/tree/94df57f856bc2e8661022a379aa1c3d6a3bc5149)。Wi-Fi 报告及连接参数采用其 RT-Thread 5 数据布局；不能直接假定其他固件版本的数据布局相同。

## 构建与网络接入

```text
python scripts/build_flash.py
```

只编译时使用 `--build-only`。脚本必须看到实际的 `Build Finished. 0 errors` 且构建进程成功，才会允许烧录，避免工程加载失败时误用旧固件。

网络部分复用工程已有的 lwIP 2.1.2：

- `src/rw007-internet.c` / `.h`：STA 以太网帧、DHCP、DNS、TCP 和 HTTP 连通性测试。
- `board/rw007_net/lwipopts.h`：IPv4、raw API、`NO_SYS=1` 专用配置；由现有工作线程驱动输入与定时器，没有另建 TCP/IP 线程。
- `board/rw007_net/arch/cc.h`：RT-Thread 平台适配。
- `scripts/configure_rw007_network.py`：将所需 lwIP 源码和头文件路径加入未跟踪的本地 `.cproject`；构建脚本自动调用，重复执行不会重复添加配置。
- 根目录 `SConscript`：同步声明所需源码和头文件路径；实际验收使用的是 RT-Thread Studio 构建流程。

通过 Studio 界面直接构建前，可先执行 `python scripts/configure_rw007_network.py`，再让 IDE 重新加载外部修改后的工程配置。本例程没有启用 RT-Thread 的系统 socket/WLAN 自动初始化，专用配置也不应与另一套 lwIP 实例同时启用。

以太网发送使用有界队列，避免在接收回调中递归执行 SPI、覆盖正在解析的缓冲区。主控发送 STA 数据包类型 1，接收类型 1 的以太网帧交给 lwIP。

## 测试互联网

保持手机 2.4 GHz 热点及其互联网数据连接开启。在本机终端执行：

```text
python scripts/test_rw007_internet.py --port COM8 --ssid Hotspot
```

脚本会交互请求密码，不需要把密码写入代码或命令行参数。它等待例程空闲后提交一次联网任务，隐藏发送命令的回显、脱敏记录结果；读取完结果和资源状态后立即关闭串口，整个会话最多等待 110 秒。其他终端占用 COM8 时会报错，不会关闭其他程序。

也可以在板端 MSH 手动输入：

```text
rw007_demo internet "Hotspot" "<实际热点密码>"
```

占位文字需要替换为实际密码。普通 MSH 输入可能出现在终端回显和内存中的命令历史；自动测试脚本对自己的输出和日志进行脱敏。任务工作缓冲区中的密码用后清除，不保存到工程文件。当前固件连接参数使用 32 字节密码字段，例程接受 8～31 字节 WPA 密码、1～32 字节 SSID；完整命令还受当前 80 字节 MSH 命令缓冲区限制。自动脚本不接受凭据中的双引号或换行。已验证的热点使用 WPA2 AES。

联网步骤与判定：

1. 扫描匹配指定 SSID，选择收到的匹配报告中信号最强的一条。
2. 提交连接参数，等待模块报告关联成功，而不是仅依据连接命令 ACK。
3. DHCP 获取 IPv4 地址、网关和 DNS，等待上限 30 秒。
4. DNS 解析 `www.msftconnecttest.com`，等待上限 15 秒。
5. 通过 TCP 80 端口请求 `/connecttest.txt`，连接及响应等待上限 20 秒。只有 HTTP 状态为 200，且响应体开头匹配 `Microsoft Connect Test`，才输出 `INTERNET PASS`。

测试结束后停止 DHCP、清理 TCP 连接并复位模块，主动结束热点连接。因此随后 `ready=0` 是本次测试的正常结束状态，可再次提交测试命令。

这是单次明文 HTTP 连通性检查，不验证 HTTPS/TLS、服务器身份、长期稳定性或所有互联网服务。响应缓冲区为 2048 字节，只服务于该固定小响应端点，不作为通用 HTTP 客户端使用。公网端点不可达或返回内容变化时，应依据 DHCP、DNS、TCP、HTTP 各阶段输出定位，不能仅凭一次失败断言整个互联网不可用。

## 状态与已知行为

`pass_mask` 和 `fail_mask` 保存各项最近一次测试结果：信息 `0x02`、Wi-Fi 扫描 `0x04`、BLE 扫描 `0x08`、广播命令 `0x10`、互联网 `0x40`。`0x1E` 表示前四项通过，`0x5E` 表示加上互联网测试均通过；它们不是当前连接状态。

`connected` 表示 BLE 连接状态。`adv_command_active` 表示主控记录的广播命令活动窗口，实际手机连接后会清零；不会自动延长广播，过期后使用 `rw007_demo adv` 再开启。

每次模块复位后的首个 SPI 包实测可能返回全 `FF`，命令自动重试后恢复。`errors` 累计记录这些传输失败，因此四项测试后常见 `errors=4`，并非零错误；必须结合最终结果、是否持续增长及 `malformed` 判断。BLE 初始化和扫描启动在本固件上可能没有单独 ACK，例程依据后续响应、报告和完成事件判断，仍保留拒绝和超时处理。

## 实测与验收记录

2026-10-01，RT-Thread Studio 编译 0 错误、7 个停用例程入口的未使用函数警告；DAP-LINK/PyOCD 烧录成功。最终联网版 Flash 181356 字节、静态 RAM 69680 字节，ELF 仅保留当前例程初始化入口。

用户回传结果：

- `rw007_demo all` 四项通过，`run=5 pass_mask=0x1E fail_mask=0x00`。
- Wi-Fi 和 BLE 扫描各收到 7 条报告，并正常完成。
- 手机连接成功、收到订阅事件、之后断开。
- 开启自己的 2.4 GHz 手机热点 `Hotspot` 后，扫描识别到信道 1、RSSI −44 dBm、WPA2 AES；共 8 条报告，扫描通过。

板端互联网实测：

```text
NET: IP=192.168.9.97
NET: gateway=192.168.9.251
NET: DNS=192.168.9.251
NET: www.msftconnecttest.com=23.205.151.10
NET: HTTP status=200 received=187 expected_body=MATCH
NET: tx=14 rx=13 drops=0; INTERNET PASS result=0
RESULT: internet PASS result=0
RW007: run=2 finished; pass_mask=0x5E fail_mask=0x00
```

IP 和解析结果是本次现场数据，程序没有硬编码这些地址。运行线程栈峰值为 12%（8192 字节栈），可用堆 566096 字节，`malformed=0`。包含上电四项测试及一次联网的累计 SPI 错误数为 5，均为上述启动重试。

本地日志为 `logs/msh_20261001_012510.log` 和 `logs/internet_20261001_014725.log`，日志不提交。本次自动联网会话占用 COM8 约 10 秒，随后已释放。用户明确回复“验收通过”，代码和本文随任务提交归档。

SoftAP、BLE 配网完整流程、远端 GATT 读写及 BLE 串口透传不在本次已验证范围。相关限制见 [RW007 固件能力说明](ble-capabilities.md)。`ble-sound` 已按用户要求放弃，保留 [历史评估](ble-sound.md)。
