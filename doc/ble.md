# RW007 BLE 最小发现例程

## 功能与启用

`src/ble.c` 使用板载 RW007 现有固件开启 BLE 广播，手机蓝牙调试助手可在 BLE 扫描页发现并连接。当前仅启用 `INIT_APP_EXPORT(ble_start)`，其他例程入口均已注释。

该例程直接使用已生成的 FSP SCI3 和 IRQ13 配置，不需要开启完整 Wi-Fi/LwIP 软件包，也不需要新增 `rtconfig.h` 宏。模块固件未升级。

## 硬件与协议

依据 HMI-Board V3.1 原理图和 SDK 工厂例程：

| 信号 | RA6M3 引脚 |
| --- | --- |
| CS | P308 |
| MISO | P309 |
| MOSI | P310 |
| CLK | P311 |
| RESET | P312 |
| INT | P015 / IRQ13 |

SCI3 工作在模式 0、1 MHz。模块复位期间 CS 保持输入，等待 INT 就绪后再配置 SPI。SPI 两阶段传输校验包头、序号和长度，带超时保护；命令发送最多重试三次。实测启动首包出现过全 FF 应答，重试后恢复，因此状态中的 `errors=1` 不代表例程已停止。

协议参考 [RT-Thread 官方 RW007 软件包](https://github.com/RT-Thread-packages/rw007) 的 `inc/spi_wifi_rw007.h`、`inc/spi_ble_rw007.h`、`src/spi_wifi_rw007.c`、`src/spi_ble_rw007.c`。该软件包使用 Apache-2.0 许可证。

实测模块固件为 `RW007_2.1.0-a7a0d089-57`，Wi-Fi MAC 为 `FC:58:4A:A6:ED:30`。官方广播名称规则为 `RW007-xxxx`，本板预期 `RW007-ED30`；Wi-Fi MAC 不应直接视为手机显示的 BLE 地址。

## 广播生命周期

程序查询固件版本、初始化模块及 BLE 外围角色，再发出命令 25（BLE 配网广播），每次请求时长 60000 ms。BLE 初始化在此固件上未观察到独立确认包，程序进一步检查广播命令返回值。

实测提前发送续期命令虽然返回成功，原计时器仍可能结束广播。因此程序接收 0xA7 广播结束事件后重新开启；未收到结束事件时，65 秒后执行后备重开。该方式可能存在短暂广播间隙，不保证无缝连续广播。

`ble_status` 显示就绪状态、最近广播命令状态、版本、SPI 传输/错误/IRQ 计数。`adv_command_ok=1` 表示命令已获确认，不能代替手机对空中广播的检查。初始化或后续通信失败时关闭 SPI/IRQ 并拉低模块复位，串口打印停止原因。

## 构建与检查

```text
python scripts/build_flash.py
python scripts/monitor_msh.py --port COM8
```

串口为 115200、8N1。在 MSH 中执行 `ble_status`、`list thread`、`free` 查看运行情况。手机选择 BLE 扫描，不是经典蓝牙串口搜索。

2026-10-01 的测试记录：

- RT-Thread Studio 编译 0 错误、6 个停用入口的未使用函数警告；Flash 93532 字节、静态 RAM 9688 字节。
- DAP-LINK/PyOCD 烧录并复位成功，版本查询及广播命令成功。
- 捕获 0xA7 / reason=13 后，广播命令再次返回 0，输出 `advertising restarted`。
- `ready=1 adv_command_ok=1 transfers=1413 errors=1`，唯一错误为上述启动重试；线程栈峰值 12%，可用堆 631208 字节。
- 串口日志：`logs/msh_20261001_004935.log`（本地日志不提交）。
- 用户确认手机能够扫描发现、连接成功，看到一个服务和两个特性，一个支持 indicate，一个支持 write；数据功能未测试。最小发现例程验收通过。

## 数据功能的边界

当前广播来自模块内置 BLE 配网服务。官方 README 明确写明外围模式目前仅支持微信小程序配网。手机看到 write/indicate 特性只证明模块提供对应 GATT 属性，不能证明任意数据会转交 RA6M3，或主控能够发送任意 indication。

后续 `ble-remote.c` / `ble-uart.c` 必须先核实写入事件转发和服务端发送接口。本例程未实现遥控、UART 透传或 ECHO，也未向模块写入 Wi-Fi 凭据。
