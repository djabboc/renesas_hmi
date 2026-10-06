# audio-raw 原始输入对照与参考音测试记录

日期：2026-10-07。用户执行两轮采集后要求助手直接读取项目文件。读取的是`logs/mic-quiet.*`与`logs/mic-voice.*`，不是先前工具验证用的合成录音。本轮未修改测试源代码、未烧录、未打开COM8。

## 完整性和运行状态

两轮均连续采集240384/240384帧、626/626块、5000ms，read/overrun/early_idle/stop均0，idle=1。块处理最大978µs，平均746/747µs，最大等待33µs，队列峰值1/2。各自保存24000帧、72000字节原始左声道，片段约0.4992秒、位于连续录音第4秒。导出2250行均按序；CRC为安静D9C78950、发声CDAD7B46，解析器重新验证通过。两份原始WAV的数据区也与串口解码出的72000字节逐字节一致。总运行均26569ms后WAIT/IDLE。

R24_peak每秒均0，L_upper_nonzero每秒均0；原始L32预览与低24位符号扩展、WAV负样本一致。传输和文件格式层面没有发现丢块、截断或符号错误；不能因此保证麦克风真实时序与声学响应正确。

## 幅度和频带对照

| 指标 | 安静 | 发声 |
| --- | ---: | ---: |
| 原始最小值 | -9129 | -13722 |
| 原始最大值 | 9122 | 12118 |
| 直流均值 | -661.34 | -894.08 |
| 去均值后全片段RMS | 3313.80 | 5776.56 |
| 去均值后全片段RMS dBFS | -68.07 | -63.24 |
| 100～8000Hz频带RMS（Hann窗估算） | 714.80 | 732.70 |
| 40Hz以下功率占比（Hann窗估算） | 90.10% | 94.13% |
| 近满幅样本 | 0 | 0 |

发声轮原始总RMS增加1.743倍、4.827dB，但100～8000Hz频带只增加1.025倍、0.215dB。图中增加的能量主要是极低频慢变化，语音频带没有明显的谐波/共振峰增强；两轮该频带频谱大体重合。不能仅凭总RMS或峰值变大就判定已录到人声。

分析方法：原始PCM24显式符号扩展，扣整段均值后乘Hann窗做FFT；平方谱按窗均方校正，单边除直流/奈奎斯特外功率乘2，再对指定频带求和开平方估计RMS。全片段RMS是未加窗的原始时间统计，与加窗分频带结果不能直接相减。频率分辨率约2.003Hz，每轮仅一个半秒片段，比较不是重复试验或严格的信噪比测量。

生成对比图`logs/mic-quiet-voice-comparison.png`：共同幅度标尺的原始去均值波形、0～100Hz放大频谱和100～8000Hz频谱。数值留在`logs/mic-quiet-voice-comparison.json`；原始完整串口数据/WAV/单轮JSON/PNG均保留在logs。本轮判断依据数值与图像，未宣称助手实际听到了人声。

## 定位结论与边界

原始样本保存于高通/FIR/mu-law/播放增益之前，语音频带已经缺少明显发声响应。因此当前优先排查声学输入、U9麦克风和SSI的真实输入时序；已有扬声器参考音/歌曲正常，加上原始数据本身的频带结果，继续调播放音量或增益没有针对性。40Hz高通会去掉大量慢变化，剩余弱噪声再被自动增益提升，能够解释此前板端听到沙沙声的现象。

这些结果仍不能认定U9损坏，也不能完全排除SSI边沿/WS/位对齐导致只接收到错误数据。下一步应先核对U9真实拾音孔位置和是否遮挡，再以明确的声学信号（例如外部1kHz音）检查原始输入频谱是否出现对应峰；结合供电和SCK/WS/SD实测区分麦克风输出与MCU接收。仅把数据变化、满帧数或峰值变大判作语音通过仍不成立。

已生成外部参考音`logs/mic-reference-1000hz.wav`（12秒、48kHz单声道、1kHz正弦、PCM16峰值8000、首尾20ms淡入淡出，未自动播放）。可用电脑/手机扬声器在板载麦克风附近播放，覆盖MIC RECORD NOW到DONE，再主动运行`python scripts/audio_raw_to_wav.py --port COM8 --output logs/mic-tone`。数据中若出现对应1kHz明显峰，证明这次受控声学信号能进入原始采集；无峰时继续核对孔位、供电和实际I²S时序。数字文件幅度不等于U9位置的声压，是否在麦克风处可听见该音仍需实际确认。

## 两轮控制日志（数据行保留在原始文件）

### 安静

完整原始文件：`logs/mic-quiet.serial.txt`。以下仅省略2250行十六进制数据，不省略状态和导出校验信息。

```text
msh >hmi_test status
TEST STATUS ready=1 busy=0
msh >hmi_test audio-raw
TEST BEGIN audio-raw
msh >MICRAW v1: record 5s; export original LEFT at 3.0..3.5s, no playback
MICRAW save the full serial log; speak/keep quiet through all 5 seconds
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC BCLK=3076923 Hz rate~48077; GPT1 period=39 (original BSP clock)
MIC RAW PCM24 slot32: frames=240384; save start=144231 count=24000 (no DSP)
MIC queue: buffers=2 block_period_us=7987 capture_priority=14
MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE
MIC RECORD DONE: frames=240384/240384 blocks=626/626 elapsed=5000 ms
MIC errors: read=0 overrun=0 early_idle=0 stop=0 idle=1 result=0
MIC timing: process_max_us=978 avg_us=746 ready_wait_max_us=33
MIC queue peak=1/2 (includes processing block)
MIC RAW second=1 n=48077 min=-625927 max=4270248 mean=16293
MIC RAW second=1 R24_peak=0 L_upper_nonzero=0
MIC RAW second=2 n=48077 min=-23044 max=10204 mean=-7317
MIC RAW second=2 R24_peak=0 L_upper_nonzero=0
MIC RAW second=3 n=48077 min=-13229 max=7028 mean=-1789
MIC RAW second=3 R24_peak=0 L_upper_nonzero=0
MIC RAW second=4 n=48077 min=-11296 max=9122 mean=-397
MIC RAW second=4 R24_peak=0 L_upper_nonzero=0
MIC RAW second=5 n=48076 min=-9816 max=7951 mean=-786
MIC RAW second=5 R24_peak=0 L_upper_nonzero=0
MIC RAW preview=0 L32=00FFE9CA R32=00000000 L24=-5686
MIC RAW preview=1 L32=00FFEA9A R32=00000000 L24=-5478
MIC RAW preview=2 L32=00FFEA0D R32=00000000 L24=-5619
MIC RAW preview=3 L32=00FFEC95 R32=00000000 L24=-4971
MIC RAW preview=4 L32=00FFED1F R32=00000000 L24=-4833
MIC RAW preview=5 L32=00FFEC8C R32=00000000 L24=-4980
MIC RAW preview=6 L32=00FFEB04 R32=00000000 L24=-5372
MIC RAW preview=7 L32=00FFEB8E R32=00000000 L24=-5234
MICRAW exporting after SSI/GPT closed; expect about 20 seconds at 115200 baud
MICRAW BEGIN v=1 rate=48077 frames=24000 start=144231 bytes=72000 crc32=D9C78950
MICRAW END bytes=72000 crc32=D9C78950
TEST RESULT audio-raw WAIT code=1 elapsed=26569 ms
TEST IDLE
```

### 发声

完整原始文件：`logs/mic-voice.serial.txt`。以下仅省略2250行十六进制数据，不省略状态和导出校验信息。

```text
msh >hmi_test status
TEST STATUS ready=1 busy=0
audio-raw runs=1 last=WAIT
msh >hmi_test audio-raw
TEST BEGIN audio-raw
msh >MICRAW v1: record 5s; export original LEFT at 3.0..3.5s, no playback
MICRAW save the full serial log; speak/keep quiet through all 5 seconds
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC BCLK=3076923 Hz rate~48077; GPT1 period=39 (original BSP clock)
MIC RAW PCM24 slot32: frames=240384; save start=144231 count=24000 (no DSP)
MIC queue: buffers=2 block_period_us=7987 capture_priority=14
MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE
MIC RECORD DONE: frames=240384/240384 blocks=626/626 elapsed=5000 ms
MIC errors: read=0 overrun=0 early_idle=0 stop=0 idle=1 result=0
MIC timing: process_max_us=978 avg_us=747 ready_wait_max_us=33
MIC queue peak=1/2 (includes processing block)
MIC RAW second=1 n=48077 min=-7313348 max=6661355 mean=28774
MIC RAW second=1 R24_peak=0 L_upper_nonzero=0
MIC RAW second=2 n=48077 min=-46910 max=737 mean=-16409
MIC RAW second=2 R24_peak=0 L_upper_nonzero=0
MIC RAW second=3 n=48077 min=-18694 max=5233 mean=-4124
MIC RAW second=3 R24_peak=0 L_upper_nonzero=0
MIC RAW second=4 n=48077 min=-13722 max=12118 mean=-709
MIC RAW second=4 R24_peak=0 L_upper_nonzero=0
MIC RAW second=5 n=48076 min=-11472 max=8803 mean=-883
MIC RAW second=5 R24_peak=0 L_upper_nonzero=0
MIC RAW preview=0 L32=00FFF5BB R32=00000000 L24=-2629
MIC RAW preview=1 L32=00FFF590 R32=00000000 L24=-2672
MIC RAW preview=2 L32=00FFF2EE R32=00000000 L24=-3346
MIC RAW preview=3 L32=00FFF5DA R32=00000000 L24=-2598
MIC RAW preview=4 L32=00FFF441 R32=00000000 L24=-3007
MIC RAW preview=5 L32=00FFEFAE R32=00000000 L24=-4178
MIC RAW preview=6 L32=00FFF2B4 R32=00000000 L24=-3404
MIC RAW preview=7 L32=00FFF1B2 R32=00000000 L24=-3662
MICRAW exporting after SSI/GPT closed; expect about 20 seconds at 115200 baud
MICRAW BEGIN v=1 rate=48077 frames=24000 start=144231 bytes=72000 crc32=CDAD7B46
MICRAW END bytes=72000 crc32=CDAD7B46
TEST RESULT audio-raw WAIT code=1 elapsed=26569 ms
TEST IDLE
```


## 第三轮：未播放参考音，不能作为1kHz测试（2026-10-07）

用户完成名为`mic-tone`的采集后明确回复“没有播放参考音”。助手最初依据文件名将其当作外部参考音试验，随后撤回该解释。文件名不能代表实物测试条件；本轮仅记录为“未播放参考音的一次额外采集”，不能凭没有1kHz峰判断麦克风失效，也不能视为受控声学测试完成。

分析对象是00:46:30生成的原始WAV/JSON，72000字节、24000帧，CRC32=DD78FE39。初次分析时完整串口导出校验通过，WAV数据区与导出逐字节一致。分析期间，同名前缀的serial文件在00:49:42被下一次尝试覆盖，仅剩639字节的录音开始日志；该不完整日志不与旧WAV混用，不记为一次新的完整采集。旧WAV/试听副本/JSON/图已另存为`logs/mic-no-reference-20261007-004630.*`，保留可核验的原始WAV字节。

| 指标（原始PCM24） | 安静轮 | 发声轮 | 第三轮，未播放参考音 |
| --- | ---: | ---: | ---: |
| 全片段交流RMS | 3313.80 | 5776.56 | 2600.12 |
| 100～8000Hz频带RMS | 714.80 | 732.70 | 691.79 |
| 950～1050Hz频带RMS | 74.86 | 80.46 | 80.51 |
| 40Hz以下功率占比 | 90.10% | 94.13% | 85.06% |

方法与前述Hann窗频带分析一致。第三轮在1kHz附近没有明显音调峰，频带幅度与安静轮只差约0.63dB；由于没有施加参考音，这个现象不能作为麦克风对参考音无响应的证据。更正后的三轮对比图和数值为`logs/mic-no-reference-comparison.png/.json`；复现脚本`logs/analyze-mic-no-reference.py`只读文件，不打开串口。此前名为mic-tone-comparison的初步产物不作为参考音试验结论。

参考文件本身已离线核验：48kHz、PCM16峰值8000，取稳定1秒做FFT，主峰为1000Hz。这只验证文件内容，不能代替扬声器实际发声。

下一轮按`audio-raw.md`的“外部1kHz参考音测试”操作，使用新的`logs/mic-reference-test-01`前缀，避免覆盖既有证据。只有确认外部扬声器全程发声、在U9附近可清楚听见，才将新录音作为受控参考音试验。本轮未修改固件、未烧录、未打开COM8。
