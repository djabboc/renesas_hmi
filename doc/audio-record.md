# 独立麦克风录音与 MP3：audio-record

最新实物结果：mic-record-03 完整导出 CRC=8F04B421、WAV字节匹配，用户反馈仍只有沙沙声。已逐项核对官方 BSP 与瑞萨 SSI 示例，详情见 [audio-official-review.md](audio-official-review.md)；人声仍未验收，没有确认具体根因。

日期：2026-10-07。目的：把麦克风采集到的连续声音交给电脑试听，先判断是否录到了清晰人声，再排查板端回放。样本变化、峰值变大、接收计数完整都不能代替人声验收。

2026-10-07 曾按用户要求暂停，随后恢复调试；证据汇总见 [audio-debug-handoff.md](audio-debug-handoff.md)。现已取消2～3cm定向录音前提，下一项为 [CPU直读FIFO对照](audio-fifo.md)。

后续数字信号诊断见 [audio-wire-check.md](audio-wire-check.md)：用户没有逻辑分析仪、示波器或万用表，已用开发板自身采样 SCK/WS/SD，在生产 WS 配置下独立解码出 10 个左声道样本，与 SSI FIFO 10/10 逐字匹配。该短快照不证明录到了人声，也未覆盖5秒DTC录音；后续以CPU直读FIFO对照继续排查。

## 文件与独立性

- `src/test/test-audio-record.c`：单个C文件、一个线程入口，独立初始化和关闭GPT1、SSI/DTC，不创建线程、不注册命令、不调用其他测试、不播放喇叭。
- `src/test-main.c`：注册`hmi_test audio-record`，沿用一次一个线程、禁止重叠的调度方式。
- `scripts/audio_record_to_mp3.py`：保存完整串口数据，校验后在电脑端生成WAV/MP3；只在用户指定`--port`时打开串口。

`src/hal_entry.c`以及已有audio-mic/audio-raw/audio-replay例程保留。本例程不需要SD卡、系统USB或外部参考音。

## 一次录音的流程

1. 申请录音和双DMA缓冲，关闭喇叭输出，倒计时3秒。
2. 配置原BSP的GPT1时钟和SSI格式，连续采集约6秒。前125块约0.9984秒用于预热，随后626块约5秒完整保存。
3. SSI完成中断只提交下一块缓冲和唤醒当前线程；线程消费已完成缓冲。采集期间不打印样本。队列满、读错误、提前空闲或超时均返回错误，不能静默覆盖数据。
4. 关闭SSI/GPT、恢复线程优先级，打印处理前的左右声道预览、逐秒统计以及PCM16限幅次数。
5. 从已保存的RAM打印带偏移和CRC的完整PCM16数据，退出后释放缓冲。导出期间约21秒，录音已经结束。
6. 电脑脚本检查完整数据，再生成WAV和MP3。编码期间COM8已经释放，不会自动播放音频。

## 样本格式与RAM取舍

U9为MSM261S3526Z0CM，L/R接地选左声道。SSI接收右对齐PCM24、32位时隙、左右各一个32位字。GPT1周期39，PCLKD=120MHz，计算BCLK约3.076923MHz、每声道采样率约48076.923Hz；这些计算值不能代替实测时钟。

当前完整测试固件静态RAM为543528字节，芯片总SRAM为655360字节，尚需内核对象和测试线程。5秒全速左声道PCM24需要721152字节，无法放进当前RAM。因此本例程在48kHz输入上执行固定127点低通，然后六抽一：

```text
SSI LEFT PCM24 -> 3kHz抗混叠低通 -> 六抽一 -> 固定除以32 -> PCM16限幅 -> 打印
```

系数和为32768，直流增益为1。固定除以32比常规24转16的除以256多保留3位弱输入，超过16位范围时限幅并计数；这不是按当前声音自动计算的增益。本例程没有高通、降噪、mu-law或自动增益。滤波前的L32/R32/L24预览和逐秒统计帮助核对输入。

保存40064个单声道PCM16样本、80128字节；WAV标记8013Hz，时长约4.9999秒。实际采样率8012.8205Hz，整数标记误差约22ppm。录音内容适合判断语音，但不能据此验收全频带或高保真音质。这里的“原始WAV”指MP3编码和电脑试听处理之前的板端PCM16，并非全速、未经处理的SSI PCM24；需要后者的半秒精确片段时，已有audio-raw保留该能力。

录音加哨兵80136字节，双DMA缓冲6160字节，动态申请合计86296字节。SWD只读核对复位后空闲堆：total=111704、used=11128、free=100576字节，随后还要申请12288字节测试栈和线程/事件对象；当前空间能容纳本例程，但余量较小，故保留申请失败退出路径，记录`logs/audio-record-heap.log`。申请失败返回`FAIL`；停止、取消和错误退出均清理资源。板端只打印PCM，MP3由电脑的FFmpeg编码，不在MCU上运行MP3编码器。

## 用户测试：只需说话

烧录本次固件后，关闭占用COM8的串口助手，在项目根目录运行：

```powershell
python scripts/audio_record_to_mp3.py --port COM8 --output logs/mic-record-01
```

脚本先确认板卡ready=1/busy=0，再发`hmi_test audio-record`。3秒倒计时后，看到`MIC RECORD NOW`，对着麦克风正常说话，覆盖全部6秒。可反复说“麦克风测试，一二三四五”；前1秒预热，后5秒保存。看到`MIC RECORD DONE`后停止说话，等待完整导出及文件生成，总过程约30秒。

打开`logs/mic-record-01.mp3`试听，并反馈：能否辨认上述文字，还是只有噪声、静音或失真。**能听清自己的话才算人声采集验收通过**。文件已经位于本机，助手可直接读取，不必把数千行数据粘贴到聊天。

再次测试使用02、03等新编号。脚本拒绝已存在的输出前缀，防止覆盖旧录音。默认串口占用上限90秒，正常结束、错误、超时和中断退出都会关闭端口；助手不会自动开启COM8。

## 输出与MP3的含义

| 后缀 | 内容 |
| --- | --- |
| `.serial.txt` | 主动采集时保存的完整串口文本，包括全部2504行数据 |
| `.wav` | 板端打印的PCM16字节原样保存，单声道、8013Hz，不做电脑端增益 |
| `.raw.mp3` | 从上述WAV直接编码，不做去均值或增益 |
| `.listen.wav` | 只去整段均值并乘一次固定增益，使峰值为12000；没有降噪或动态AGC |
| `.mp3` | 从试听WAV编码，方便听弱输入；底噪也会一起放大 |
| `.json` | 板端PCM16幅度、直流、交流RMS、限幅样本、试听增益、CRC及处理说明 |

MP3是有损编码，使用16kHz、48kbit/s、单声道；从8013Hz重采样不会恢复原本没有保存的高频内容。本机FFmpeg的MP3解码比WAV多约33ms帧填充，不是新增录音；WAV具有准确的40064帧，诊断数值和字节一致性以WAV为准。

试听副本会放大噪声，不能用“有声”判断录到了人声，也不能拿分别归一化的安静/发声副本音量作响应比较。原始PCM16和未增益MP3同时保留。

FFmpeg优先从`--ffmpeg`指定路径、PATH或本机Downloads中的`ffmpeg*/bin/ffmpeg.exe`寻找；该工具已经在本机可用。需要其他路径时：

```powershell
python scripts/audio_record_to_mp3.py --port COM8 --output logs/mic-record-02 --ffmpeg C:/Tools/ffmpeg/bin/ffmpeg.exe
```

## 手工保存日志后离线转换

也可在115200、8N1的串口终端执行：

```text
hmi_test audio-record
```

保存从BEGIN到END的全部数据，不能仅复制可见窗口，也不要实际折行。然后离线运行：

```powershell
python scripts/audio_record_to_mp3.py --log recording.txt --output logs/mic-offline-01
```

离线方式不打开串口。协议为`MICREC BEGIN v=1 rate=8013 frames=40064 bits=16 channels=1 bytes=80128 crc32=...`、连续字节偏移的DATA行及END。脚本拒绝漏行、重复、错序、坏CRC、截断、取消和混合多次录音；有损MP3不用于CRC校验。

## 自动检查与实物验收边界

9组真实C函数ARM模拟检查通过：24位符号、CRC已知向量、完整预热/录音帧数、全部40064个PCM16对独立卷积参考、WAV导出字节/偏移、关闭硬件后导出、动态申请与哨兵、取消/超时/队列溢出/启动和读错误、IPC/优先级恢复、低通直流增益与高频抑制。FSP桩不模拟麦克风的声学响应。

12项主机检查通过，包括协议损坏拒绝、PCM/WAV完整字节、试听转换、有限采集/异常释放、拒绝覆盖旧录音，以及真实FFmpeg编码/解码：合成1kHz音调的幅度和时长保留。合成输入只验证工具，不能代替真实麦克风录音。另有17项总入口/独立性工具检查通过，当前共48个独立测试文件。

Studio构建0 errors/0 warnings，Flash1168016字节、静态RAM543528字节。DAP-LINK/PyOCD烧录成功、退出码0。原始证据保存在本机`logs/audio-record-arm.log`、`logs/audio-record-host-tests.log`、`logs/audio-record-independent-tests.log`、`logs/audio-record-build.log`和`logs/audio-record-flash.log`；合成转换产物为`logs/audio-record-synthetic.*`，不能当成实物录音。

另用SWD在真实Cortex-M4执行32块实际处理函数，最大270984周期、2258.2µs，平均2257.1µs（120MHz），输入块间隔约7987µs。该基准暂时运行SRAM程序，完成后复位回已烧录的Flash固件；不打开COM8，也不代表实际麦克风采集通过。结果见`logs/audio-record-benchmark.log`。

实际声音是否正常仍待用户按上述顺序录音试听；板端最终`WAIT`表示导出完成、等待声学确认，不自动宣布麦克风通过。

## 首轮实物录音与串口重试修复（2026-10-07）

用户先运行mic-record-01，COM8被其他终端占用，Windows返回WinError 5；未发出录音命令。旧脚本先创建日志再开串口，留下了0字节日志，同编号重试被防覆盖检查拒绝。已改成先打开串口、再独占创建日志：拒绝访问不会留下空文件，正常释放后可用原编号重试。已核对并清理该次失败的0字节mic-record-01.serial.txt；其他录音保留。新增两项检查覆盖拒绝访问后同名前缀重试，以及日志创建冲突时保留旧内容并释放串口，共12项主机检查通过。此修复仅修改电脑脚本，不改固件、不重新烧录、不打开COM8。

随后用户用mic-record-02完成实际采集。重新解析完整2504行数据并通过CRC=8B437AD7，原始WAV的80128字节与导出逐字节一致，试听WAV也与脚本的去均值/固定增益结果逐字节一致。MIC采集288384/288384帧、751/751块，5998ms；保存40064/40064 PCM16样本，约4.999875秒。read/overrun/early_idle/stop全0，storage_clipped=0；处理最大1512µs、平均1442µs、就绪等待最大33µs，队列峰值1/2，29659ms后WAIT/IDLE。COM8由用户运行的脚本正常释放。

PCM16最小值-933、最大值339、均值-80.452、交流RMS169.229；试听固定增益14.0755倍。文件mic-record-02.wav/.listen.wav/.raw.mp3/.mp3/.json和完整串口日志均保存在本机logs。这里的计数和幅度只证明接收、保存、导出和转换完成；用户随后明确反馈“持续说话，只有噪音沙沙”。实际语音采集验收未通过；完整传输与转换不能替代声音内容正确。

以下原始控制日志只省略2504行十六进制DATA；完整文件为logs/mic-record-02.serial.txt。

```text
msh >hmi_test status
TEST STATUS ready=1 busy=0
msh >hmi_test audio-record
TEST BEGIN audio-record
msh >MICREC v1: warmup 1s + record 5s -> print PCM16, no speaker playback
MICREC LPF 3kHz -> decimate 6 -> divide 32 -> PCM16; no automatic gain
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC BCLK=3076923 Hz rate~48077; GPT1 period=39 (original BSP clock)
MIC PCM24 slot32: source_frames=288384; warmup_frames=48000; PCM16_frames=40064
MIC queue: buffers=2 block_period_us=7987 capture_priority=14
MIC RECORD NOW: 6 seconds; first 1s warmup, then save 5s; keep speaking
MIC RECORD DONE: frames=288384/288384 blocks=751/751 elapsed=5998 ms
MIC errors: read=0 overrun=0 early_idle=0 stop=0 idle=1 result=0
MIC timing: process_max_us=1512 avg_us=1442 ready_wait_max_us=33
MIC queue peak=1/2 (includes processing block)
MIC INPUT second=1 n=48077 min=-30420 max=4658 mean=-8648
MIC INPUT second=1 R24_peak=0 L_upper_nonzero=0
MIC INPUT second=2 n=48077 min=-11790 max=11235 mean=-2501
MIC INPUT second=2 R24_peak=0 L_upper_nonzero=0
MIC INPUT second=3 n=48077 min=-12118 max=8630 mean=-436
MIC INPUT second=3 R24_peak=0 L_upper_nonzero=0
MIC INPUT second=4 n=48077 min=-9531 max=7371 mean=-1184
MIC INPUT second=4 R24_peak=0 L_upper_nonzero=0
MIC INPUT second=5 n=48076 min=-8632 max=11856 mean=-98
MIC INPUT second=5 R24_peak=0 L_upper_nonzero=0
MIC INPUT preview=0 L32=00FF90A5 R32=00000000 L24=-28507
MIC INPUT preview=1 L32=00FF91F2 R32=00000000 L24=-28174
MIC INPUT preview=2 L32=00FF91A6 R32=00000000 L24=-28250
MIC INPUT preview=3 L32=00FF9111 R32=00000000 L24=-28399
MIC INPUT preview=4 L32=00FF94F4 R32=00000000 L24=-27404
MIC INPUT preview=5 L32=00FF92B7 R32=00000000 L24=-27977
MIC INPUT preview=6 L32=00FF90C2 R32=00000000 L24=-28478
MIC INPUT preview=7 L32=00FF92C0 R32=00000000 L24=-27968
MICREC exporting after SSI/GPT closed; wait about 21s at 115200 baud
MICREC PCM16 samples=40064/40064 bytes=80128 storage_clipped=0
MICREC BEGIN v=1 rate=8013 frames=40064 bits=16 channels=1 bytes=80128 crc32=8B437AD7
MICREC END bytes=80128 crc32=8B437AD7
TEST RESULT audio-record WAIT code=1 elapsed=29659 ms
TEST IDLE
```

## 用户试听结论与下一步定位（2026-10-07）

用户确认mic-record-02采集期间持续说话，电脑端MP3只有沙沙声，没有可辨认的人声。验收结论：导出/转换流程通过，麦克风语音内容未通过。不是仅依据min/max推断，而是实际试听反馈；该反馈不能单独证明U9损坏。

本次声音由电脑播放，绕开J8、PWM和板端回放。此前未处理的audio-raw安静/发声频谱也缺少明显语音响应，两项证据共同把重点放到声学输入、U9输出和SSI收到的原始数据。新例程没有高通或mu-law，低通在100～2000Hz计算增益约1，实际C函数和MP3工具已用已知合成音验证；仍不以这些检查证明真实声学路径正确。

再次只读核对：g_i2s0_cfg为24位PCM/32位时隙/SSI主机/内部音频时钟/分频1，DTC4字节；FSP的24位设置自动置PDTA右对齐，例程掩码低24位并显式扩展符号。P403/P404/P406恢复生成表中的SSI复用，GPT1周期39与原BSP一致。没有在这些源码配置处找到可直接确认的错误；寄存器设置及根据计数计算的速率不能代替U9引脚上的真实电平、边沿与WS对齐。

下一步应直接观察U9的输出以区分器件与MCU：原理图U9引脚1=VDD、2=SCK、3=GND、4=L/R、5=WS、6=SD；VDD应为3.3V、L/R接地，计算SCK约3.076923MHz、WS约48.0769kHz，左右各32个SCK。示波器用于供电、电平和时序，逻辑分析仪按I2S左声道解码SD并与发声动作对照；实际采样边沿与数据延迟必须依据器件原厂资料核验，不能盲切寄存器。

若SD直接解码有清晰人声、MCU记录没有，重点核对SSI采样边沿、WS/位对齐及FIFO/DTC顺序；若供电和时钟符合规格、SD本身没有声学响应，重点核对拾音孔/遮挡、焊接和U9。当前尚缺这些实测证据，不能把哪一项称为已确定根因。本轮仅记录试听结论与源码核对，没有改固件、没有烧录、没有打开COM8；已询问用户是否有逻辑分析仪/示波器，以确定可执行的下一步。
