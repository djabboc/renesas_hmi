# audio-replay v7：完整采集后只有沙沙声

**终止归档（2026-10-07）：用户明确放弃麦克风录音及回放排查。语音未通过、根因未确定；不再要求录音、复测或参数调整。以下方法、命令和待验证分支仅为历史记录；最终状态见 [audio-debug-closed.md](audio-debug-closed.md)。**

日期：2026-10-06。用户确认录音5秒内持续说话，从开发板正面、距离10cm以内发声；回放只有沙沙声。按用户要求，本轮只核对代码、硬件资料和日志，不修改测试源文件、不构建/烧录、不打开COM8。

## 可以确认的事实

| 环节 | 本轮证据 | 结论范围 |
| --- | --- | --- |
| 连续接收/线程处理 | 240384/240384帧，626/626块，5000ms，overrun=0 | v6的队列溢出已解决，软件交接完成；不能据此证明声学内容正确 |
| 处理余量 | 最大1291µs、平均1242µs；块间隔7987µs；等待最大41µs；队列峰值1/4 | 本轮无处理积压，继续扩缓冲或优化速度没有针对性 |
| 输入稳定段 | HP24第2～5秒mean_abs=1028/951/926/964 | 高通后、FIR及mu-law前已经是弱信号；幅度数字不能单独判断语音内容 |
| 回放执行 | 80128/80128样本，5001ms，pwm_error=0；用户能听见噪声 | 播放有输出；结合此前参考音/歌曲正常，播放硬件为低优先级疑点 |

第1秒原始数据接近24位满幅，但第2～5秒迅速回落。原始均值从26599变为-14515/-3298/-2333/150，呈现启动后的偏置变化。大启动极值不能当作成功录到人声的证据；早期独立audio-mic只录启动后的半秒，其PASS证明接收完成和数字变化，尚不足以证明语音采集正确。

本轮第3～5秒HP24平均绝对值947.0；历史v5安静938.3、持续说话945.7。相近幅度支持“没有明显发声响应”的疑点，但两个版本采样率不同，且平均绝对值不是频谱或波形，不能严格认定这三段是相同噪声，也不能直接认定麦克风损坏。

## 沙沙声如何形成

当前播放按后4秒峰值自动放大：stable_peak=133，gain_q8=5774，即22.5547倍。没有清晰人声时，底噪也被提升到接近目标峰值3000，因此“现在有沙沙声”不意味着录音恢复正常，也不支持继续增加增益。

limited=7940，占全部样本约9.91%，这是播放处理限幅次数，不是麦克风过载次数。依据当前代码，后4秒所有解码样本减均值后的幅度不超过stable_peak=133，乘增益5774/256后不超过3000；因此本轮这些限幅落在用来估算增益之外的第1秒。启动瞬态能解释开头的失真，不能单独解释后4秒持续缺少人声。

## 只读代码及硬件核对

- 原理图Audio页：U9为MSM261S3526Z0CM，L/R接地选择左声道，SD接I2S_RXD并有100k下拉。R24_peak=0符合未选中右声道的现象；它不能证明左声道拾音正常。
- FSP的24位PCM/32位时隙配置自动置PDTA，数据右对齐；例程掩码低24位并显式扩展符号。DTC为4字节传输，按左右各一个32位字读取。没有在这些源码环节发现明确的位宽/声道/符号错误，实际串行对齐仍需测量。
- 高通约40Hz，FIR低通约5kHz；按当前系数计算浮点频响，高通与低通合计在100/300/1000/3000Hz的幅度为0.9310/0.9939/1.0012/0.9145。正常语音频段没有被整体滤掉。这是公式核对，不代替实物原始波形检查。
- G.711编解码、滤波及分块样本顺序已经用合成输入执行真实C函数检查；模拟输入绕过物理麦克风和I2S时序，因此不能据此宣称录到的人声正确。
- BCLK=3076923是根据配置计算的打印值；接收帧数/耗时支持约48kHz的有效接收速率，但不能代替在U9引脚测量时钟电平、相位、WS和SD。

原理图来源：Studio BSP的`documents/hmi-board-v3.1-schematic.pdf`第12页，本地既有渲染`logs/hmi-audio-schematic.png`；没有修改硬件配置。本轮未取得并核验该麦克风原厂完整数据手册，不把恢复BSP时钟等同于确认其全部工作条件正确。

## 当前定位和下一步证据

当前优先疑点在“声学输入→U9麦克风→SSI收到的原始数据”。可能是拾音孔/声学通路、麦克风供电或器件状态，也可能是I2S采样边沿、WS对齐等接收条件不匹配。没有证据把具体一项定为根因；对板正面说话也不能替代核对U9实际拾音孔位置和是否被遮挡。

下一步先取稳定阶段的小段原始左声道数据，在电脑端做正确24位符号扩展并直接生成WAV、观察波形/频谱，与同条件的安静段对照。这个数据点应在高通/FIR/mu-law/自动增益之前，先暂时绕开后处理链定位：电脑端能听清而板端不清，转查后处理/回放；电脑端也只有底噪，重点核对声学通路和I2S输入。导出诊断需要另行设计，不在本轮修改代码。

若具备示波器或逻辑分析仪，在U9附近核对3.3V供电、SCK、WS、SD，结合该型号原厂时序图对照SSI接收格式；固定声音输入时直接解码SD最能区分器件输出和MCU接收问题。现有日志不足以替代这些证据。

## 用户原始日志

```text
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY v7: independent MIC 5s at ~48k -> playback 5s at ~16k; queued capture
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC BCLK=3076923 Hz rate~48077; GPT1 period=39 (original BSP clock)
MIC PCM24 slot32; 240384 frames, 626 blocks of 384 frames; output mono mu-law
MIC filter: high-pass ~40Hz at PCM24; storage divisor=32
MIC resample: FIR 31 taps ~5kHz; 3 input frames -> 1 stored sample
MIC queue: buffers=4 block_period_us=7987 capture_priority=14
MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE
MIC RECORD DONE: frames=240384/240384 blocks=626/626 elapsed=5000 ms
MIC errors: read=0 overrun=0 early_idle=0 stop=0 idle=1 result=0
MIC timing: process_max_us=1291 avg_us=1242 ready_wait_max_us=41
MIC queue peak=1/4 (includes processing block)
MIC second=1 frames=48077 L24 min=-8388256 max=5458458 mean=26599
MIC second=1 changed_in_blocks=47935 near_full=1 R24_peak=0
MIC HP24 second=1 min=-8380837 max=4659093 mean_abs=49680 storage_clipped=148
MIC second=2 frames=48077 L24 min=-44300 max=3895 mean=-14515
MIC second=2 changed_in_blocks=47914 near_full=0 R24_peak=0
MIC HP24 second=2 min=-5372 max=5148 mean_abs=1028 storage_clipped=0
MIC second=3 frames=48077 L24 min=-16035 max=4969 mean=-3298
MIC second=3 changed_in_blocks=47902 near_full=0 R24_peak=0
MIC HP24 second=3 min=-4623 max=4791 mean_abs=951 storage_clipped=0
MIC second=4 frames=48077 L24 min=-15229 max=7796 mean=-2333
MIC second=4 changed_in_blocks=47904 near_full=0 R24_peak=0
MIC HP24 second=4 min=-5222 max=4295 mean_abs=926 storage_clipped=0
MIC second=5 frames=48076 L24 min=-13486 max=10285 mean=150
MIC second=5 changed_in_blocks=47915 near_full=0 R24_peak=0
MIC HP24 second=5 min=-4046 max=4469 mean_abs=964 storage_clipped=0
PCM mu-law mono bytes=80128; stable_mean=1 stable_peak=133 gain_q8=5774
PCM output limited to +/-3000; silence/noise does not prove a recorded voice
AUDIO PLAY NOW: replaying the recorded 5 seconds
AUDIO PLAY DONE: samples=80128/80128 elapsed=5001 ms pwm_error=0 limited=7940
AUDIO duty_A=682..818 result=1; listening confirmation required
TEST RESULT audio-replay WAIT code=1 elapsed=13268 ms
TEST IDLE
```
