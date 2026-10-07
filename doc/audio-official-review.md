# 麦克风官方源码对照与 mic-record-03（2026-10-07）

**终止归档（2026-10-07）：用户明确放弃麦克风录音及回放排查。语音未通过、根因未确定；不再要求录音、复测或参数调整。以下方法、命令和待验证分支仅为历史记录；最终状态见 [audio-debug-closed.md](audio-debug-closed.md)。**

用户再次运行 audio-record，试听仍为沙沙声，并要求参考官方实现排查。本轮完成官方 BSP、上游源码及瑞萨 SSI 示例的对照，复核新录音的导出数据。后续增加临时 SRAM 的 CPU 直读 FIFO 对照，见 [audio-fifo.md](audio-fifo.md)；没有改写 Flash 或打开 COM8。

## 实际使用了哪些官方代码

本工程的 SSI、GPT、DTC 底层驱动来自官方 BSP 中的瑞萨 FSP。录音应用的缓冲队列、样本统计、降采样、导出协议由本工程实现，不能把这套应用称为已经验证过的官方麦克风例程。

本机参考包为 `C:/RT-ThreadStudio/repo/Extract/Board_Support_Packages/RealThread/HMI-Board/1.2.0/projects/hmi-board-factory`。同时读取 [RT-Thread-Studio 官方 HMI BSP 仓库](https://github.com/RT-Thread-Studio/sdk-bsp-ra6m3-hmi-board)，核对的上游版本为 `9463e9d259c3c9c7bb520e1070064a692bf98903`（提交时间 2025-04-29）。本次选取的 2897 个工程 C/H 文件与本机官方包对应文件的 Git blob SHA 全部一致，未遇到树截断。

以下 8 个完整文件与官方 factory 工程逐字节相同，SHA256 已保存到本机 `logs/mic-official-review/current-official-comparison.json`：

| 范围 | 当前文件 | 对照结果 |
| --- | --- | --- |
| SSI 接收实现 | `ra/fsp/src/r_ssi/r_ssi.c` | 完整文件一致 |
| GPT 时钟驱动 | `ra/fsp/src/r_gpt/r_gpt.c` | 完整文件一致 |
| DTC 传输驱动 | `ra/fsp/src/r_dtc/r_dtc.c` | 完整文件一致 |
| SSI 编译配置 | `ra_cfg/fsp_cfg/r_ssi_cfg.h` | 完整文件一致 |
| DTC 编译配置 | `ra_cfg/fsp_cfg/r_dtc_cfg.h` | 完整文件一致 |
| GPT 编译配置 | `ra_cfg/fsp_cfg/r_gpt_cfg.h` | 完整文件一致 |
| 系统时钟 | `ra_gen/bsp_clock_cfg.h` | 完整文件一致 |
| 引脚复用 | `ra_gen/pin_data.c` | 完整文件一致 |

另外，`ra_gen/hal_data.c` 中 GPT1、SSI0、DTC10 的生成配置段逐字相同：GPT1 周期39、占空计数19；PCM24、32位时隙、主机模式、内部音频时钟、分频1、WS_CONTINUE_OFF；DTC 4字节搬运。当前 audio-record 使用这些配置，仅设置自己的回调及接收缓冲；原始 audio-mic 曾覆盖为周期117，不能用它的约16kHz状态代表当前录音配置。

## 官方包是否包含可直接运行的麦克风录音应用

在本机官方包、上游对应工程的应用/board源码以及公共 libraries 中，未找到板载 U9 的接收调用或录音完成处理。上游本次选取的非 FSP、非生成配置应用/board文件共2064个，未出现 `R_SSI_Read`、`I2S_EVENT_RX_FULL`、SSI FIFO 接收或 `g_i2s` 接收调用的匹配。

factory 工程的 `board/ra6m3_it.c` 中 `i2s_callback()` 是空函数。`Page_MainMenu.c` 声明了 `IMG_Recorder` 图标，但实际菜单项只有 LEDAndBtn、SDCard、WiFi、ETH、Game2048、Info，也没有 Recorder 页面源码。因此，在本次核对的官方 HMI 包与仓库版本中，能够直接参考的是配置和 FSP 驱动，未找到“已经能录出人声”的完整 U9 应用。此结论不代表其他发布包、分支或厂家私下提供的工程不存在录音例程。

官方源码入口：

- [factory 麦克风生成配置](https://github.com/RT-Thread-Studio/sdk-bsp-ra6m3-hmi-board/blob/9463e9d259c3c9c7bb520e1070064a692bf98903/projects/hmi-board-factory/ra_gen/hal_data.c)
- [factory SSI 驱动](https://github.com/RT-Thread-Studio/sdk-bsp-ra6m3-hmi-board/blob/9463e9d259c3c9c7bb520e1070064a692bf98903/projects/hmi-board-factory/ra/fsp/src/r_ssi/r_ssi.c)
- [factory 空回调](https://github.com/RT-Thread-Studio/sdk-bsp-ra6m3-hmi-board/blob/9463e9d259c3c9c7bb520e1070064a692bf98903/projects/hmi-board-factory/board/ra6m3_it.c)
- [factory 菜单](https://github.com/RT-Thread-Studio/sdk-bsp-ra6m3-hmi-board/blob/9463e9d259c3c9c7bb520e1070064a692bf98903/projects/hmi-board-factory/board/lvgl/demo/factory_demo/Page/Page_MainMenu.c)

## 瑞萨 SSI 示例的适用范围

另读取 [瑞萨 EK-RA6M3 SSI 示例](https://github.com/renesas/ra-fsp-examples/tree/master/example_projects/ek_ra6m3/ssi/ssi_ek_ra6m3_ep)。其 readme 描述生成正弦波，通过 SSITX→SSIRX 回环，以双缓冲和 `R_SSI_WriteRead()` 校验收发数据。配置为 PCM16、16位时隙，示例面向 EK-RA6M3 的回环接线，未使用本板 U9。

它可用于对照 FSP 的打开、非阻塞传输、完成事件和双缓冲流程，不能直接把16位时隙或该板接线套用到当前麦克风。当前 `R_SSI_Read()` 的官方注释允许在 `I2S_EVENT_RX_FULL` 后再次提交接收；现有录音回调按此方式续接下一块。`r_ssi_start()` 在 REN 已启用时不会为每一块重新清 FIFO；源码未支持“每8ms重新启动麦克风导致静音”的判断。

瑞萨示例的读取副本保存在本机 `logs/mic-official-review/renesas-ssi/`，官方包/上游查询、文件清单和差异检查保存在同一父目录。

## mic-record-03 实物结果

助手离线解析完整 `.serial.txt`，复核2504行数据、80128字节、40064样本与 CRC32=`8F04B421`；WAV 头为单声道/PCM16/8013Hz，其数据区与串口导出逐字节一致。用户反馈试听仍只有沙沙声，人声验收未通过。用户此前已确认在10cm以内持续说话，本次明确反对以2～3cm作为验收条件；已取消这一前提，按正常说话距离录不到人声继续排查。

| 指标 | mic-record-03 |
| --- | --- |
| 源采集 | 288384/288384帧、751/751块、5998ms |
| 传输错误 | read/overrun/early_idle/stop 全0 |
| 处理耗时 | 最大1514µs、平均1441µs，块间隔约7987µs |
| 就绪等待/队列峰值 | 33µs、1/2 |
| 保存样本/字节 | 40064 / 80128 |
| 限幅次数 | 0 |
| PCM16 min/max/均值 | -549 / 332 / -51.743 |
| 交流RMS | 142.757 |
| 电脑试听固定增益 | 24.1324倍 |
| 总耗时 | 29658ms，WAIT/IDLE，用户脚本已释放COM8 |

完整录音保存在本机 `logs/mic-record-03.*`；仅去掉 DATA 的控制日志为 `logs/mic-record-03.control.txt`。PCM16 和原始 PCM24 幅度不能直接比较；两次分别归一化的 MP3 听起来一样响，也不能用来判断发声响应。

## 当前能够定位到哪里

已经确认：板端接收和导出计数完整、无报告的溢出/限幅；WAV没有在串口转换中改写样本；底层及生成配置与官方一致；此前短快照中 SD 独立解码与 SSI FIFO 左声道10/10匹配。电脑试听绕过了 J8/PWM 输出；此前未经滤波的 audio-raw 也缺少明确的语音响应。

还不能确认：U9的声学部分与实际供电是否正常、器件原厂全部时序要求是否满足，以及完整5秒的DTC/应用缓冲内容是否在所有时刻与SD输出一致。短快照的10字匹配不能扩大为整条长期录音链路完全通过。

因此没有已证实的具体根因。本次未发现可依据官方差异直接修复的时钟、格式或驱动错误；不能以此排除所有软件问题，更不能宣布U9损坏。下一步在相同官方配置下，以CPU直接读取FIFO、全速保存5秒PCM16，绕过DTC、线程队列、滤波和降采样，隔离自写应用流程。若能录到人声，回查生产采集链路；若仍只有噪声，继续检查SSI之前的数字与声学链路。如有另一份已经能录出人声的官方工程，仍应按其源码和实测结果对照。用户当前无逻辑分析仪、示波器或万用表。
