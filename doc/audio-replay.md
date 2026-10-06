# 录音回放逐步排查：audio-replay

最新实测：v7完整采集/回放各5秒、overrun=0，用户持续说话（正面、10cm以内）仍只有沙沙声。处理积压已解决，语音未通过验收；本轮按用户要求不改源码/不烧录，详细定位与完整日志见[audio-replay-v7-analysis.md](audio-replay-v7-analysis.md)。

2026-10-07按用户授权实现下一步诊断：新增独立`audio-raw`，连续录5秒并导出第4秒内约半秒原始24位左声道数据，电脑生成WAV/波形/频谱；原回放例程保留作对照。操作见[audio-raw.md](audio-raw.md)，等待安静/持续发声两轮原始输入验收。

日期：2026-10-06。当前为v7：独立麦克风采集5秒，关闭采集硬件，再回放5秒。v5持续说话只听到沙沙声，安静/说话的稳定幅度几乎相同；v6恢复原始BSP约3.077MHz麦克风时钟、约48kHz采样，低通三抽一后仍保存/播放约16kHz语音。v6板上两轮发生队列溢出，未进入回放；v7修复处理余量与唤醒调度，语音回放仍待人工验收。

## 当前 v7：修复采集溢出，录音5秒、回放5秒

J8沿用已连接的JBL 4Ω C11R。执行：

```text
hmi_test audio-replay
```

1. 倒计时3秒，准备说一段话，例如“一二三四五，你好，麦克风测试”。
2. `MIC RECORD NOW`：开始独立连续采集5秒；持续发声，录音期间喇叭保持静音。
3. `MIC RECORD DONE`：停止发声；SSI/DTC已经关闭，随后关闭GPT1，线程打印五个逐秒统计并准备幅度。
4. `AUDIO PLAY NOW`：回放同一段完整录音5秒；观察是否听到刚才的话。
5. `AUDIO PLAY DONE`后关闭GPT2/GPT6、静音、释放录音RAM，返回WAIT/IDLE。包含倒计时约13秒，统计输出可能增加阶段之间的时间。

请提交完整日志，并描述回放是否有声、能否辨认刚才的话、声音是否连续。样本数和计时完成不自动代表人声录制正确。可用 `hmi_test stop` 协作退出，退出仍清理硬件。

### 采集、存储与时间

SSI使用24位PCM、32位时隙、4字节FIFO/DTC，每帧两声道。GPT1恢复原始BSP的period_counts=39、duty_cycle_counts=19，位时钟120MHz/39约3.077MHz，双声道每帧64个时钟，原始帧率约48076.92Hz。采集240384帧，低通后每3帧保存1样本，得到80128个约16025.64Hz的单声道样本；GPT2周期仍为7488。采集与播放理论时长均4.999987秒。

原始48kHz五秒双声道需要1923072字节；完整16kHz PCM16单声道仍需要160256字节，超出现有空闲堆。本版本逐块提取左声道，在24位精度执行约40Hz高通，再使用31点对称FIR低通、三抽一；除以32后用G.711 mu-law保存，每样本1字节，共80128字节，堆申请80136字节含哨兵。低通采用Blackman窗、截止约5kHz，系数和32768，1kHz增益约1、8kHz约0.017，降低下采样混叠；64位乘加避免溢出。每个DTC缓冲384原始帧/3072字节，四块环形缓冲含哨兵共12320字节静态RAM，不依赖SD卡。相对v4除以256，存储多保留3位弱信号；mu-law编码限幅到±32635，按秒统计storage_clipped。

高通递推调整为 `y[n] = x[n] - x[n-1] + (191/192)*y[n-1]`，适配约48kHz输入，截止仍约40Hz。高通与FIR状态跨块保持、每次录音置零。滤波不能恢复原本没有录到的人声，不能据此宣称麦克风故障已解决。

每块384原始帧/3072字节产生128保存样本，625次回调提交下一个Read，第626块完成后才Stop。FSP允许在RX_FULL回调提交下一次接收，块间不Stop/Open；完成中断唤醒线程，线程依序处理已完成块，中断把下一次接收交给队列空闲块。队列最多容纳一块正在写入和三块正在处理或待处理的数据；完成未消费块达到四块、没有空闲块时报告overrun，不静默覆盖。采样中断不打印、分配内存或执行滤波/编码。

### 日志与播放处理

`MIC RECORD DONE`应为frames=240384/240384、blocks=626/626、elapsed约5000ms。`MIC errors`应为read=0、overrun=0、early_idle=0、stop=0、正常IDLE。`MIC second=1..5`统计原始24位左声道、近满幅数、块内变化数及右声道峰值；前四段各48077帧，最后48076帧，合计240384。`MIC HP24`统计高通后的原始48kHz信号，storage_clipped统计该秒降采样后存储限幅次数。

新增 `MIC HP24 second=1..5`：滤波后的24位最小值、最大值、平均绝对幅度及存储限幅次数。先看后几秒的mean_abs是否随安静/发声产生可重复变化；原始峰值和changed_in_blocks不能独立证明有人声。播放时解码mu-law、去残余均值、有限增益和首尾约10ms淡入淡出。全部五秒样本都回放，启动段没有丢弃；仅用后四秒的高通数据估计均值和交流峰值。最大增益64倍，最终数字仍限于±3000（已试听过的参考音峰值），底噪同样可能放大。稳定数据完全不变化时gain=0，仍播放五秒并打印统计。

`AUDIO PLAY DONE`应为samples=80128/80128、elapsed约5000ms、pwm_error=0；limited记录被限幅的样本数，启动段较大可能触发限幅。输出PWM仍为GPT6/P702/P703、80kHz，GPT2逐样本更新，结束两脚置低。成功硬件执行返回WAIT，需用户听音验收。

### 单文件与验证

全部逻辑位于 `src/test/test-audio-replay.c`，保留唯一线程入口，不包含MSH注册、创建线程或跨例程调用；`src/test-main.c`保持串行调度，`src/hal_entry.c`和原独立audio-mic未修改。

`validate_audio_replay.py`提取真实C函数，在ARM模拟器执行15组检查。新增24ms调度延迟后全部样本内容/顺序保持、34ms延迟与四块突发溢出保护、信号量与临时优先级清理检查。覆盖240384原始帧到80128保存样本的完整顺序与内容（独立移位历史对照实际环形FIR）、两阶段5秒时长、FIR直流增益/500Hz保留/16kHz混叠抑制、高通直流衰减、G.711、符号扩展、互斥阶段、取消/超时/溢出/Read与启动失败、哨兵、幅度边界和清理。15组ARM及30项主机检查通过，记录 `logs/audio-replay-overrun-arm.log`、`logs/audio-replay-overrun-host-tests.log`；桩不替代真实I²S时序与语音试听。

v6构建0 errors/0 warnings，Flash1156036字节、静态RAM536072字节；DAP-LINK/PyOCD成功烧录1156112字节，退出码0，记录 `logs/audio-replay-clock-build.log`、`logs/audio-replay-clock-flash.log`。此为v6历史交付结果；随后两轮实物溢出失败，见下方原始日志。

v5历史构建：Studio编译0 errors/0 warnings，Flash1155332字节、静态RAM531848字节；DAP-LINK/PyOCD烧录1155344字节成功，退出码0，记录 `logs/audio-replay-highpass-build.log`、`logs/audio-replay-highpass-flash.log`。随后两轮用户日志显示未录到可辨认的人声，详见下方。助手不占用COM8。

v4历史构建：Studio编译0 errors/0 warnings，Flash1154804字节、静态RAM531720字节。DAP-LINK/PyOCD成功烧录1154832字节，退出码0；记录 `logs/audio-replay-5s-build.log`、`logs/audio-replay-5s-flash.log`。用户随后实测各5秒计时与全部帧完成，但持续发声仍无声，见下方原始日志。

### v7 处理余量和调度修复（2026-10-06）

v6每块输入间隔7987.2µs，工程以`-O0`构建。通过SWD在实际Cortex-M4 SRAM运行真实统计/FIR/mu-law处理函数（32块伪随机24位数据，关闭中断），测得`-O0`最大738198周期/6151.6µs，平均6146.1µs；`-O2`最大224373周期/1869.8µs，平均1866.6µs。此测量只验证算法吞吐量，不包含Flash取指、DTC或RTOS抢占，也不能证明已录到人声。脚本`scripts/benchmark_audio_replay.py`不打开COM8，结束或执行失败后复位回Flash固件；运行时会暂停应用，请勿与其他测试同时执行。日志`logs/audio-replay-overrun-benchmark-v7.log`。

v7对实时运算区局部使用GCC `-O2`，区外及整个工程仍保留原编译设置。双缓冲改为四块环形队列，额外静态RAM6160字节。录音阶段暂时把当前测试线程优先级从21提高到14（数字越小优先级越高），硬件清理后恢复进入录音前的优先级；不创建第二个线程。RX_FULL先提交下块接收，再释放信号量唤醒线程，替代1ms轮询；等待上限10ms以便检查停止/超时。FSP SSI ISR没有RT-Thread中断进出调用，回调内在使用内核IPC前补齐`rt_interrupt_enter/leave`。采集结束关闭SSI后才销毁信号量。

新增`MIC timing`打印每块最大/平均处理微秒数和从完成到处理的最大等待时间，DWT计时包含真实运行时的抢占；`MIC queue peak`包含正在处理的块。块间隔约7987µs，正常期望处理时间小于该间隔、队列峰值小于4、overrun=0。信号量计数只负责唤醒，完成/消费索引才决定数据所有权；完整内容检查验证不会因合并唤醒而漏块。

v7 Studio构建0 errors/0 warnings，Flash1156412字节、静态RAM542280字节；日志`logs/audio-replay-overrun-build.log`。DAP-LINK/PyOCD已成功烧录1156496字节、退出码0，日志`logs/audio-replay-overrun-flash.log`。15组ARM与30项主机检查通过；ARM模拟器的处理瞬时完成，因此模拟检查验证内容/生命周期，耗时结论来自上述硬件基准。真实RTOS下的持续采集与语音试听待用户执行`hmi_test audio-replay`，确认REPLAY v7，再提供完整日志和听感。不得把修复溢出等同于语音验收通过。

### v6 两轮采集失败原始日志

用户两轮均在采集阶段队列溢出，处理完成块44/278，接收完成块46/280。尚未进入播放，不能用这两轮判断48kHz时钟下的语音质量。

```text
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY v6: independent MIC 5s at ~48k -> playback 5s at ~16k; original BSP clock
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC BCLK=3076923 Hz rate~48077; GPT1 period=39 (original BSP clock)
MIC PCM24 slot32; 240384 frames, 626 blocks of 384 frames; output mono mu-law
MIC filter: high-pass ~40Hz at PCM24; storage divisor=32
MIC resample: FIR 31 taps ~5kHz; 3 input frames -> 1 stored sample
MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE
MIC RECORD DONE: frames=16896/240384 blocks=46/626 elapsed=361 ms
MIC errors: read=0 overrun=1 early_idle=0 stop=0 idle=1 result=-3
TEST RESULT audio-replay FAIL code=-3 elapsed=3423 ms
TEST IDLE

msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY v6: independent MIC 5s at ~48k -> playback 5s at ~16k; original BSP clock
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC BCLK=3076923 Hz rate~48077; GPT1 period=39 (original BSP clock)
MIC PCM24 slot32; 240384 frames, 626 blocks of 384 frames; output mono mu-law
MIC filter: high-pass ~40Hz at PCM24; storage divisor=32
MIC resample: FIR 31 taps ~5kHz; 3 input frames -> 1 stored sample
MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE
MIC RECORD DONE: frames=106752/240384 blocks=280/626 elapsed=2230 ms
MIC errors: read=0 overrun=1 early_idle=0 stop=0 idle=1 result=-3
TEST RESULT audio-replay FAIL code=-3 elapsed=5292 ms
TEST IDLE
```

### 时钟排查依据及证据范围

本地原始BSP及当前 `ra_gen/hal_data.c` 的GPT1周期均为0x27（39），对应约3.077MHz；SSI配置为internal clock、bit_clock_div=1、32位时隙。助手早期改为117，以直接获得约16kHz采样，这是后续v1～v5持续沿用的设置。两轮安静/说话幅度基本相同后，需优先验证恢复BSP工作点，不能继续把启动极值当成人声。

[该型号的CSDN公开资源摘要](https://download.csdn.net/download/tel16675197553/92423697)列出正常模式1.5～4.0MHz、低功耗150～600kHz及3.072MHz测试条件，旧1.026MHz落在摘要列出的两个区间之间。此为二手摘要，尚未取得并核验原厂完整PDF，不能称为已验证规格或已确认根因；v6选择3.077MHz的直接依据为原始BSP配置。正常模式实际声学响应和回放仍需板测。

以下保留历史版本、用户原始日志及听感。

## v5 说话/安静对照（2026-10-06）

用户确认第一轮持续说话，回放为“沙沙的声音”；第二轮保持安静（除环境噪音）。两轮均完整采集/输出80128样本、626块，采集5000ms/回放5001ms，读/溢出/提前IDLE/停止/PWM错误均0。第二轮未单独提供听感，不推定与第一轮相同。

| 高通24位平均绝对幅度 | 持续说话 | 安静 | 说话/安静 |
| --- | ---: | ---: | ---: |
| 第2秒 | 1295 | 1359 | 0.953 |
| 第3秒 | 946 | 976 | 0.969 |
| 第4秒 | 981 | 957 | 1.025 |
| 第5秒 | 910 | 882 | 1.032 |

第3～5秒平均945.7/938.3，差约0.8%，未呈现明显发声响应；仅凭此不能区分硬件/时序/环境/距离等原因。第一秒仍有启动峰值、存储限幅120个。说话增益4439/256约17.34倍、输出限幅6902个；安静增益4085/256约15.96倍、输出限幅6566个。高通降低慢漂移影响后有可听噪声，但未得到可辨认的人声，语音验收失败。原独立audio-mic的启动半秒极值和changed只能支持数据接收，不能支持人声质量通过。

### 第一轮：持续说话，沙沙声

```text
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY v5: independent MIC 5s -> recorded playback 5s; PCM24 high-pass / mu-law
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC PCM24 slot32; 80128 frames, 626 blocks of 128 frames; output mono mu-law
MIC filter: high-pass ~40Hz at PCM24; storage divisor=32
MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE
MIC RECORD DONE: frames=80128/80128 blocks=626/626 elapsed=5000 ms
MIC errors: read=0 overrun=0 early_idle=0 stop=0 idle=1 result=0
MIC second=1 frames=16026 L24 min=-871023 max=8017252 mean=132122
MIC second=1 changed_in_blocks=15889 near_full=0 R24_peak=0
MIC HP24 second=1 min=-592485 max=7888214 mean_abs=63849 storage_clipped=120
MIC second=2 frames=16026 L24 min=-244385 max=-25960 mean=-92905
MIC second=2 changed_in_blocks=15893 near_full=0 R24_peak=0
MIC HP24 second=2 min=-3701 max=5514 mean_abs=1295 storage_clipped=0
MIC second=3 frames=16026 L24 min=-33728 max=-2556 mean=-14020
MIC second=3 changed_in_blocks=15890 near_full=0 R24_peak=0
MIC HP24 second=3 min=-4043 max=3940 mean_abs=946 storage_clipped=0
MIC second=4 frames=16026 L24 min=-18490 max=5726 mean=-5346
MIC second=4 changed_in_blocks=15886 near_full=0 R24_peak=0
MIC HP24 second=4 min=-4289 max=4759 mean_abs=981 storage_clipped=0
MIC second=5 frames=16024 L24 min=-14223 max=10991 mean=-646
MIC second=5 changed_in_blocks=15885 near_full=0 R24_peak=0
MIC HP24 second=5 min=-4429 max=4066 mean_abs=910 storage_clipped=0
PCM mu-law mono bytes=80128; stable_mean=7 stable_peak=173 gain_q8=4439
PCM output limited to +/-3000; silence/noise does not prove a recorded voice
AUDIO PLAY NOW: replaying the recorded 5 seconds
AUDIO PLAY DONE: samples=80128/80128 elapsed=5001 ms pwm_error=0 limited=6902
AUDIO duty_A=682..818 result=1; listening confirmation required
TEST RESULT audio-replay WAIT code=1 elapsed=13280 ms
TEST IDLE
```

### 第二轮：安静（除环境噪音）

```text
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY v5: independent MIC 5s -> recorded playback 5s; PCM24 high-pass / mu-law
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC PCM24 slot32; 80128 frames, 626 blocks of 128 frames; output mono mu-law
MIC filter: high-pass ~40Hz at PCM24; storage divisor=32
MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE
MIC RECORD DONE: frames=80128/80128 blocks=626/626 elapsed=5000 ms
MIC errors: read=0 overrun=0 early_idle=0 stop=0 idle=1 result=0
MIC second=1 frames=16026 L24 min=-920169 max=8291719 mean=142031
MIC second=1 changed_in_blocks=15887 near_full=0 R24_peak=0
MIC HP24 second=1 min=-588857 max=8188336 mean_abs=64105 storage_clipped=120
MIC second=2 frames=16026 L24 min=-275420 max=-25597 mean=-101614
MIC second=2 changed_in_blocks=15887 near_full=0 R24_peak=0
MIC HP24 second=2 min=-3879 max=6030 mean_abs=1359 storage_clipped=0
MIC second=3 frames=16026 L24 min=-33856 max=2314 mean=-14273
MIC second=3 changed_in_blocks=15887 near_full=0 R24_peak=0
MIC HP24 second=3 min=-4485 max=4739 mean_abs=976 storage_clipped=0
MIC second=4 frames=16026 L24 min=-13940 max=8526 mean=-3626
MIC second=4 changed_in_blocks=15894 near_full=0 R24_peak=0
MIC HP24 second=4 min=-4669 max=4798 mean_abs=957 storage_clipped=0
MIC second=5 frames=16024 L24 min=-14072 max=7980 mean=-3320
MIC second=5 changed_in_blocks=15879 near_full=0 R24_peak=0
MIC HP24 second=5 min=-4374 max=4038 mean_abs=882 storage_clipped=0
PCM mu-law mono bytes=80128; stable_mean=8 stable_peak=188 gain_q8=4085
PCM output limited to +/-3000; silence/noise does not prove a recorded voice
AUDIO PLAY NOW: replaying the recorded 5 seconds
AUDIO PLAY DONE: samples=80128/80128 elapsed=5001 ms pwm_error=0 limited=6566
AUDIO duty_A=682..818 result=1; listening confirmation required
TEST RESULT audio-replay WAIT code=1 elapsed=13281 ms
TEST IDLE
```

## v4 首轮板上日志（2026-10-06）

用户执行v4五秒版本后提供以下完整日志，并确认“持续说话，回放仍无声”。采集80128帧、626块均完成，耗时5000ms；回放80128样本全部输出，耗时5001ms。读错误、缓冲溢出、提前IDLE、停止错误及PWM错误均为0，正常返回WAIT/IDLE。计时与数据搬运完成，但语音验收失败。

左声道第一秒含接近满量程的启动峰值；第二秒均值为-93577，第三至第五秒均值逐渐趋近0，整体存在明显启动漂移。稳定区间峰值估计仍包含第二秒漂移，不能直接解释为人声峰值。右声道峰值始终为0，与使用左声道的配置相符；changed_in_blocks仅说明数字发生变化。gain_q8=875约为3.42倍；limited=15219约占全部样本19.0%，说明输出存在限幅，不能单凭样本数完成判断声音正常。

```text
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY v4: independent MIC 5s -> recorded playback 5s; RAM mu-law
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC PCM24 slot32; 80128 frames, 626 blocks of 128 frames; output mono mu-law
MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE
MIC RECORD DONE: frames=80128/80128 blocks=626/626 elapsed=5000 ms
MIC errors: read=0 overrun=0 early_idle=0 stop=0 idle=1 result=0
MIC second=1 frames=16026 L24 min=-892037 max=8387410 mean=133212
MIC second=1 changed_in_blocks=15888 near_full=1 R24_peak=0
MIC second=2 frames=16026 L24 min=-258684 max=-21314 mean=-93577
MIC second=2 changed_in_blocks=15894 near_full=0 R24_peak=0
MIC second=3 frames=16026 L24 min=-28754 max=3125 mean=-12616
MIC second=3 changed_in_blocks=15883 near_full=0 R24_peak=0
MIC second=4 frames=16026 L24 min=-13460 max=5176 mean=-4749
MIC second=4 changed_in_blocks=15888 near_full=0 R24_peak=0
MIC second=5 frames=16024 L24 min=-16629 max=6954 mean=-3595
MIC second=5 changed_in_blocks=15879 near_full=0 R24_peak=0
PCM mu-law mono bytes=80128; stable_mean=-111 stable_peak=877 gain_q8=875
PCM output limited to +/-3000; silence/noise does not prove a recorded voice
AUDIO PLAY NOW: replaying the recorded 5 seconds
AUDIO PLAY DONE: samples=80128/80128 elapsed=5001 ms pwm_error=0 limited=15219
AUDIO duty_A=682..818 result=1; listening confirmation required
TEST RESULT audio-replay WAIT code=1 elapsed=13241 ms
TEST IDLE
```

## v3 PCM24 操作历史

运行 `hmi_test audio-replay` 两次：第一轮安静，第二轮看到 `MIC RECORD NOW` 后持续发“啊——”，直到 `MIC recording finished` 停止；保存完整日志，报告前后参考音和中间回放。

流程：前参考音0.51秒 → 24位启动段0.51秒并丢弃 → SSI连续时钟倒计时3秒 → 正式录音0.51秒 → 原始24位/转换16位诊断 → 停声等待1秒 → 回放0.51秒 → 等待0.5秒 → 后参考音0.51秒。全程约8秒加打印耗时。

本轮改用工程原始 `audio-mic` 的24位PCM/32位时隙，FSP设置PDTA右对齐，FIFO/DTC为4字节；显式符号扩展后除以256转PCM16左声道。每帧占8字节，同一65536字节缓冲只容纳8192帧，因此录音改为半秒；请使用持续发声，避免短词错过窗口。参考音、采样/PWM时钟、去直流与有限增益参数保持原设置，不提高音量。每段窗口为2048帧约128ms。

新字段：`PCM24 RAW-L/RAW-R/WIN-L/WIN-R` 使用24位数字单位，`MIC raw frame` 显示原始32位字与符号扩展值；`MIC PCM16 frame` 和 `PCM CONVERTED-L` 是除以256后的16位值。24位和16位峰值不能直接比较。`fill_A5A5A5A5` 检查32位填充值残留，三次播放预期各8192/8192。仍需安静/发声差异和实际试听，未确认采集格式就是故障原因。

v3软件验证：19组实际C函数ARM模拟检查，包括24位符号端点、上下字节掩码、左右声道、原地转换完整性与哨兵、预热/录音生命周期和错误清理；30项主机回归通过。Studio构建0 errors/0 warnings，Flash1158364字节、静态RAM529480字节，堆缓冲65544字节含哨兵。记录 `logs/audio-replay-pcm24-arm.log`、`logs/audio-replay-pcm24-host-tests.log`、`logs/audio-replay-pcm24-build.log`；DAP-LINK/PyOCD已成功烧录1158416字节、退出码0，记录 `logs/audio-replay-pcm24-flash.log`。助手不打开COM8，`src/hal_entry.c`不变，录音回放仍未验收。

下面v1/v2步骤、测试数字和构建记录保留为历史证据，不作为当前v3录音长度或数据单位。

## v2 操作历史：安静与发声对照

J8 喇叭沿用当前接线。通过 COM8 输入：

```text
hmi_test audio-replay
```

本轮从头到尾保持安静，先建立基线，不要求辨认自己的声音。关注开头和结尾各约半秒的参考提示音是否都正常。v2 总流程约 9～10 秒，串口打印可能增加耗时：

1. `REF-BEFORE`：约 501Hz 低幅正弦参考音，0.51 秒。
2. `MIC WARMUP`：接收约 1.02 秒启动数据，打印左右声道统计后丢弃。SSI 保持打开，`WS_CONTINUE ON/LRCONT=1` 配置持续时钟；倒计时 3 秒后，看到 `MIC RECORD NOW` 开始正式录音约 1.02 秒。
3. `MIC recording finished` 后停止采集，再打印 SSI、左右声道、四段窗口与 PCM 处理数据。
4. 等待 1 秒，`RECORDED-L` 播放本次左声道录音；安静时可能是很小的底噪。
5. 间隔半秒，`REF-AFTER` 重播同一参考音，清理并返回 IDLE。

请保存完整日志，并说明前、后两个参考音是否正常，中间听到什么。如果录音数据没有变化，PCM 处理会拒绝回放，但仍播放后参考音并返回失败，避免把无有效数据记为通过。硬件启动失败、提前停止或录音接收异常会结束当前流程，后参考音可能不会执行。

## 第二轮：持续发声

保存 v2 安静基线后再执行同一命令，完成 v2 发声对照。尽量保持相同距离和环境；看到倒计时 `1...` 后准备发声，`MIC RECORD NOW` 时对着板载 MIC 持续说“啊——”，直到 `MIC recording finished` 就停止。录音只有约 1 秒，先用持续声音，便于判断声学响应，随后再用短词检查语音可辨识性。

分别比较两轮的 `RAW-L/WIN-L`、`PREPARED-L` 和 `RECORDED-L`，报告是否听到自己刚才的发声、前后参考音是否正常。不要在没有对照日志时只凭一个最大值判断录音正确。

## 新日志的含义

每行控制在当前 128 字节串口格式化缓冲以内，不调整全局串口配置。详细统计在接收停止后打印；RX 完成回调保存状态并立即停止接收/DTC，PWM 中断只更新占空比和少量计数，不打印。

| 日志 | 检查内容 | 如何定位 |
| --- | --- | --- |
| `MIC config/clocks/SSI opened/DTC access` | PCM=16、时隙=32、FIFO/DTC 每次 2 字节、SSI 寄存器、120MHz PCLKD、RX 中断号 | 排查配置、时钟和传输宽度；正常配置不能单独证明波形正确 |
| `MIC WARMUP/RECORD result` | 接收结果、RX 完成回调、提前 IDLE、约 1022ms 时长、CPU 尾部剩余样本 | 超时、提前 IDLE 或剩余 CPU 样本不为 0，优先检查 SSI/DTC |
| `MIC WARMUP/RECORD end/DTC/SSI closed` | 停止前的寄存器快照、DTC 剩余、停止回调 | DTC `remaining_length` 可能是块内重装长度，不能要求它一定为 0；错误中断会清除部分 SSI 标志，结束快照不覆盖全部历史错误 |
| `MIC buffer guards MATCH` | 65536 字节采集缓冲前后的 32 位哨兵 | 损坏时立即停止处理，排查越界；哨兵匹配不等于整个接收数据有效 |
| `fill_A5A5` | 初始化填充值在整个 L/R 缓冲中的残留 | 大量残留提示未完全填充；少量同值可能是真实样本，不能单独判错 |
| `RAW-L/RAW-R` | 两个原始声道的整体统计 | 发声时左声道应有可重复的响应；若右声道响应更强，再检查声道顺序，不自动切换 |
| `WIN-L/WIN-R` | 4 个约 256ms 窗口 | 判断开头启动异常、发声是否落在录音时间内；极大峰值只集中在第一段时，检查初始化/启动瞬态 |
| `MIC raw frame` | 每段前三个 L/R 样本，十六进制和有符号十进制 | 观察数据位、符号和声道；没有导出完整波形，少量样本不能证明音质 |
| `mean/ac_peak/mean_abs_ac` | 直流均值、去直流后的峰值和平均绝对幅度 | 用平均幅度配合峰值比较两轮，避免被少量异常峰值误导 |
| `zero/changed/clipped` | 零样本数、与前一样本不同次数、接近 16 位满幅次数 | 全零/固定值/大量满幅都需检查；数字变化也可能是错误数据或底噪 |
| `AUDIO prepared/PREPARED-L` | 增益 Q8、目标峰值 3000、输出限幅次数和处理后统计 | 原始数据明显变化而处理后大幅减弱，检查去直流/增益及峰值异常 |
| `AUDIO <label> output` | 三次播放各自的样本进度、PWM 错误和结果 | `REF-*` 预期8192/8192；`RECORDED-L` 预期16384/16384；完成不等于声音内容正确 |
| `duty_A/active_duty_samples` | A 桥臂写入的占空比范围和偏离中点的样本数 | 长期接近750说明差分输出很弱；参考音应约682..818。这里统计驱动请求，未测量引脚实际电压 |

`mean_abs_ac` 是去直流后的平均绝对幅度，不是 RMS。`clipped` 统计接近满量程的数字，不能单凭它认定模拟麦克风已饱和。幅度处理仍使用原方案：减均值、目标峰值 3000、最多 8 倍增益、最终限幅 ±4000、首尾约 10ms 淡入淡出；没有为排查而提高音量。

## 根据对照结果继续

- 两个参考音都正常，中间回放没有发声响应：本文件的 RAM/PWM 路径有参考支持，优先比较录音两轮数据，再检查声道、SSI 格式或增益。
- 原始数据随发声明显变化，处理后却接近零：集中检查 PCM 提取、去直流和增益，而不是立即改喇叭驱动。
- 前参考音正常，后参考音异常：排查采集后硬件状态、资源清理和缓冲；不能只归因录音幅度。
- 本例程参考音异常，而 `audio-song` 正常：集中检查本例程的 RAM 读样本和 GPT 配置差异。

目前回放以 16 位 SSI 接收，而已验收的 `audio-mic` 使用 24 位右对齐接收。v1 已取得安静/发声对照，当前 v2 仍保持 16 位采集和原幅度处理，先检验预热过程；若预热后仍缺乏声学响应，再安排 24 位路径对照。缺少实物日志时不宣称已经查明原因或修复。

## 单文件与自测

全部诊断和参考音位于 `src/test/test-audio-replay.c`，没有增加命令或线程；仍通过总入口运行唯一例程。参考音在本次采集缓冲中生成，并调用同一个 `play_audio()`，不调用 `audio-tone` 或 `audio-song`。缓冲共 65544 字节（含8字节哨兵），三次播放串行打开/关闭 GPT2 和 GPT6，停止中断后才释放缓冲。

`scripts/validate_audio_replay.py` 提取实际 C 函数在 ARM 模拟器执行 17 组检查，覆盖 PCM/增益/符号、左右声道统计、SSI 回调标志、预热/正式录音完整生命周期、各阶段取消/接收超时/提前IDLE/读与启动失败/Stop失败/IDLE超时清理、参考音/EOF、取消/超时/PWM 错误、四个启动失败位置的清理及处理/播放日志长度。FSP 使用桩，不能代替真实 SSI 数据或试听。另有 30 项主机结构/转换/工具回归通过；v2 结果见 `logs/audio-replay-warmup-arm.log` 和 `logs/audio-replay-warmup-host-tests.log`。FSP 桩模拟接收和回调，未测量物理时钟波形。

本轮由用户手动测试，助手不打开 COM8。实物结论和下一步修改将依据两轮完整日志记录。

v2 Studio 构建：0 errors、0 warnings；Flash1157420字节、静态RAM529480字节，构建日志 `logs/audio-replay-warmup-build.log`。预热复用原缓冲，仍只申请65544字节；`src/hal_entry.c` 保持原样。

v2 已通过DAP-LINK/PyOCD成功烧录，programmed1157520字节，进程退出码0，记录 `logs/audio-replay-warmup-flash.log`；本轮没有自动运行录音命令或占用COM8。用户完成正式录音试听前，不把软件自测记作回放验收通过。

## 第一轮实物记录：安静基线（2026-10-06）

固件 `2ac8265`。用户确认：前、后两个参考音都正常，中间回放无声。

接收1023ms完成、CPU尾部剩余0、DTC剩余块0、无提前IDLE、哨兵完整、填充值残留0；左右声道中只有左声道变化。前后参考音使用同一RAM/PWM路径，均8192/8192、pwm_error=0、duty_A=682..818，且用户听音正常。这支持继续优先检查录音数据和处理，不代表任意录音已经正确。

第一窗口的左声道最大32763、均值8703，后三窗口均值分别-3145、-2432、-1258，存在明显的开头大幅变化和后续漂移，疑似启动/稳定过程。整段峰值32297把增益压到23/256，约0.09倍；处理后平均绝对幅度377，比参考音1866明显小。峰值受启动异常影响是当前线索，不能凭这轮安静数据唯一确定原因。安静时无声也不能直接判录音失败，下一轮保持代码和音量不变，持续发声比较。

用户原始日志：

补充：`MIC DTC access=2 bytes block=514` 中的514是驱动改写后的原始length字段，即0x0202；`r_dtc.c` 将块/重复模式的长度2同时写入CRAH和CRAL。它不是每块514个样本，也不表示传输错误。`DTC end remaining_length=2` 是API提取的CRAL，配合remaining_blocks=0、CPU剩余0和完成回调解释。

```text
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY diagnostic v1: REF-BEFORE -> countdown -> capture -> inspect -> replay -> REF-AFTER
REPLAY stage 1: reference beep BEFORE; remember whether you hear it
PCM REF-BEFORE n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-BEFORE mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-BEFORE start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-BEFORE output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-BEFORE duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY stage 2: keep quiet for baseline OR sustain AH during recording
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC config: PCM=16 slot=32 channels=2 bytes=65536 expected=1022ms
MIC clocks: PCLKD=120000000 GPT1 period=117 BCLK~1025641Hz LRCLK~16026Hz
MIC SSI opened: SSICR=400B4000 SSIFCR=80000000 FIFO_access=2 bytes RX_IRQ=38
MIC DTC access=2 bytes block=514 src=4004E01C
MIC RECORD NOW: keep quiet OR sustain AH for this entire 1 second
MIC receive: result=0 complete=1 rx_events=2 early_idle=0
MIC receive: elapsed=1023 ms remaining_CPU_words=0
MIC SSI end: SSICR=440B4001 SSISR=00000000 SSIFSR=00010901
MIC DTC end: remaining_blocks=0 remaining_length=2
MIC SSI closed: idle_events=1 result=0
MIC buffer guards MATCH
MIC recording finished; stop speaking. REPLAY stage 3: inspect L/R and prepare
MIC buffer fill_A5A5=0/32768 (large count suggests incomplete reception)
PCM RAW-L n=16384 min=-3381 max=32763 mean=466 ac_peak=32297
PCM RAW-L mean_abs_ac=4341 zero=10 changed=14500 clipped=1
PCM RAW-R n=16384 min=0 max=0 mean=0 ac_peak=0
PCM RAW-R mean_abs_ac=0 zero=16384 changed=0 clipped=0
MIC window=0 frames=0..4095 (~256ms)
PCM WIN-L n=4096 min=-2231 max=32763 mean=8703 ac_peak=24060
PCM WIN-L mean_abs_ac=8986 zero=10 changed=3974 clipped=1
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=0 L=0000/0 R=0000/0
MIC raw frame=1 L=0000/0 R=0000/0
MIC raw frame=2 L=0000/0 R=0000/0
MIC window=1 frames=4096..8191 (~256ms)
PCM WIN-L n=4096 min=-3381 max=-2224 mean=-3145 ac_peak=921
PCM WIN-L mean_abs_ac=204 zero=0 changed=3676 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=4096 L=F74E/-2226 R=0000/0
MIC raw frame=4097 L=F750/-2224 R=0000/0
MIC raw frame=4098 L=F749/-2231 R=0000/0
MIC window=2 frames=8192..12287 (~256ms)
PCM WIN-L n=4096 min=-3094 max=-1774 mean=-2432 ac_peak=662
PCM WIN-L mean_abs_ac=349 zero=0 changed=3460 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=8192 L=F3EA/-3094 R=0000/0
MIC raw frame=8193 L=F3F1/-3087 R=0000/0
MIC raw frame=8194 L=F3F0/-3088 R=0000/0
MIC window=3 frames=12288..16383 (~256ms)
PCM WIN-L n=4096 min=-1777 max=-843 mean=-1258 ac_peak=519
PCM WIN-L mean_abs_ac=220 zero=0 changed=3387 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=12288 L=F913/-1773 R=0000/0
MIC raw frame=12289 L=F912/-1774 R=0000/0
MIC raw frame=12290 L=F913/-1773 R=0000/0
MIC DMA complete: frames=16384 min=-3381 max=32763 mean=466 ac_peak=32297
MIC DMA complete: changed=14500 clipped=1
AUDIO prepared: gain_q8=23 (256=1x) output_peak=2588 limited=0
PCM PREPARED-L n=16384 min=-345 max=2588 mean=-6 ac_peak=2594
PCM PREPARED-L mean_abs_ac=377 zero=12 changed=5336 clipped=0
AUDIO gain reason: target_peak=3000 max_gain=8x; use WIN-L stats to check startup outliers
REPLAY stage 4: AUDIO REPLAY NOW (recorded LEFT channel)
PCM RECORDED-L n=16384 min=-345 max=2588 mean=-6 ac_peak=2594
PCM RECORDED-L mean_abs_ac=377 zero=12 changed=5336 clipped=0
AUDIO RECORDED-L start: frames=16384 GPT2_period=7488 PWM_period=1500
AUDIO RECORDED-L output samples=16384/16384 pwm_error=0 result=1
AUDIO RECORDED-L duty_A=743..809 active_duty_samples=16001 elapsed=1030 ms
REPLAY stage 5: reference beep AFTER; compare with the first beep
PCM REF-AFTER n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-AFTER mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-AFTER start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-AFTER output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-AFTER duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY diagnostic finished result=1; report both beeps AND recorded voice
TEST RESULT audio-replay WAIT code=1 elapsed=8241 ms
TEST IDLE
```


## 第二轮实物记录：持续发声，仍无回放（2026-10-06）

固件仍为 v1。用户明确确认从 `MIC RECORD NOW` 到 recording finished 持续发声，中间回放无声，前后参考音正常。两轮后段数据很接近，不能判定“已经录到人声”。`changed` 大于零只证明样本变化；正常完成和缓冲完整只证明接收与输出流程完成。

| 指标 | 安静 | 持续发声 |
| --- | ---: | ---: |
| 第一段最大值 | 32763 | 32762 |
| 后三段 mean_abs_ac | 204 / 349 / 220 | 239 / 359 / 199 |
| 整段 gain_q8 | 23 | 23 |
| 处理后 mean_abs_ac | 377 | 371 |
| 中间播放样本 / PWM 错误 | 16384 / 0 | 16384 / 0 |
| 人工试听 | 中间无声 | 中间无声 |

判断：参考音支持播放路径正常；录音存在可重复的启动峰值与缓慢漂移，未见明确发声响应。不能据此断言麦克风损坏，也不能只提高播放增益后宣称修复。当前优先检查麦克风启动与连续时钟。

用户持续发声原始日志：

```text
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY diagnostic v1: REF-BEFORE -> countdown -> capture -> inspect -> replay -> REF-AFTER
REPLAY stage 1: reference beep BEFORE; remember whether you hear it
PCM REF-BEFORE n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-BEFORE mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-BEFORE start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-BEFORE output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-BEFORE duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY stage 2: keep quiet for baseline OR sustain AH during recording
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC config: PCM=16 slot=32 channels=2 bytes=65536 expected=1022ms
MIC clocks: PCLKD=120000000 GPT1 period=117 BCLK~1025641Hz LRCLK~16026Hz
MIC SSI opened: SSICR=400B4000 SSIFCR=80000000 FIFO_access=2 bytes RX_IRQ=38
MIC DTC access=2 bytes block=514 src=4004E01C
MIC RECORD NOW: keep quiet OR sustain AH for this entire 1 second
MIC receive: result=0 complete=1 rx_events=2 early_idle=0
MIC receive: elapsed=1023 ms remaining_CPU_words=0
MIC SSI end: SSICR=440B4001 SSISR=00000000 SSIFSR=00010901
MIC DTC end: remaining_blocks=0 remaining_length=2
MIC SSI closed: idle_events=1 result=0
MIC buffer guards MATCH
MIC recording finished; stop speaking. REPLAY stage 3: inspect L/R and prepare
MIC buffer fill_A5A5=0/32768 (large count suggests incomplete reception)
PCM RAW-L n=16384 min=-3384 max=32762 mean=463 ac_peak=32299
PCM RAW-L mean_abs_ac=4278 zero=9 changed=14535 clipped=1
PCM RAW-R n=16384 min=0 max=0 mean=0 ac_peak=0
PCM RAW-R mean_abs_ac=0 zero=16384 changed=0 clipped=0
MIC window=0 frames=0..4095 (~256ms)
PCM WIN-L n=4096 min=-2108 max=32762 mean=8593 ac_peak=24169
PCM WIN-L mean_abs_ac=8881 zero=9 changed=3968 clipped=1
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=0 L=0000/0 R=0000/0
MIC raw frame=1 L=0000/0 R=0000/0
MIC raw frame=2 L=0000/0 R=0000/0
MIC window=1 frames=4096..8191 (~256ms)
PCM WIN-L n=4096 min=-3384 max=-2100 mean=-3089 ac_peak=989
PCM WIN-L mean_abs_ac=239 zero=0 changed=3628 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=4096 L=F7C8/-2104 R=0000/0
MIC raw frame=4097 L=F7C9/-2103 R=0000/0
MIC raw frame=4098 L=F7C6/-2106 R=0000/0
MIC window=2 frames=8192..12287 (~256ms)
PCM WIN-L n=4096 min=-3055 max=-1693 mean=-2392 ac_peak=699
PCM WIN-L mean_abs_ac=359 zero=0 changed=3510 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=8192 L=F411/-3055 R=0000/0
MIC raw frame=8193 L=F415/-3051 R=0000/0
MIC raw frame=8194 L=F416/-3050 R=0000/0
MIC window=3 frames=12288..16383 (~256ms)
PCM WIN-L n=4096 min=-1700 max=-880 mean=-1257 ac_peak=443
PCM WIN-L mean_abs_ac=199 zero=0 changed=3426 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=12288 L=F95E/-1698 R=0000/0
MIC raw frame=12289 L=F95E/-1698 R=0000/0
MIC raw frame=12290 L=F962/-1694 R=0000/0
MIC DMA complete: frames=16384 min=-3384 max=32762 mean=463 ac_peak=32299
MIC DMA complete: changed=14535 clipped=1
AUDIO prepared: gain_q8=23 (256=1x) output_peak=2588 limited=0
PCM PREPARED-L n=16384 min=-345 max=2588 mean=-6 ac_peak=2594
PCM PREPARED-L mean_abs_ac=371 zero=15 changed=5275 clipped=0
AUDIO gain reason: target_peak=3000 max_gain=8x; use WIN-L stats to check startup outliers
REPLAY stage 4: AUDIO REPLAY NOW (recorded LEFT channel)
PCM RECORDED-L n=16384 min=-345 max=2588 mean=-6 ac_peak=2594
PCM RECORDED-L mean_abs_ac=371 zero=15 changed=5275 clipped=0
AUDIO RECORDED-L start: frames=16384 GPT2_period=7488 PWM_period=1500
AUDIO RECORDED-L output samples=16384/16384 pwm_error=0 result=1
AUDIO RECORDED-L duty_A=743..809 active_duty_samples=15986 elapsed=1030 ms
REPLAY stage 5: reference beep AFTER; compare with the first beep
PCM REF-AFTER n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-AFTER mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-AFTER start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-AFTER output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-AFTER duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY diagnostic finished result=1; report both beeps AND recorded voice
TEST RESULT audio-replay WAIT code=1 elapsed=8241 ms
TEST IDLE
```

## v2 改动与下一步判据

1. 配置 `I2S_WS_CONTINUE_ON`，先实际接收并丢弃一秒启动段，保持 SSI/GPT1 打开跨过三秒倒计时，再接收正式段。打印 WARMUP-L/R 和 LRCONT 配置，正式段仍有完整四窗口统计。
2. RX 完成回调保存寄存器、CPU剩余与完成时间后立即 Stop，线程等待有上限的 IDLE 再重启/关闭；Stop、读、超时和取消错误均清理硬件。物理连续时钟尚待实物数据支持，寄存器配置不替代示波器测量。
3. 明确恢复本例使用的 P403/P404/P406 复用并打印 PFS；没有发现其他测试使用这些引脚，未把引脚冲突记为已确认原因。DTC length 日志改为 `CRA_raw=0202 block_length=2`，避免误读514。
4. 保留16位采集、32位时隙、同一RAM、原增益与参考播放路径。预热后若正式段不再出现接近32767的启动峰值，说明启动线索获得支持；同时仍需安静/发声差异和实际回放确认。若仍无声学响应，再单独检查24位接收、I²S时序和麦克风接线。

当前未验收录音回放，不把软件模拟或构建通过记为实物修复。


## 第三、四轮实物记录：v2安静/发声仍无回放（2026-10-06）

固件 `494f3cb`。用户明确确认：第一轮安静、第二轮持续发声；第二轮中间仍无声，前后参考音正常。

| 指标 | v2安静 | v2持续发声 |
| --- | ---: | ---: |
| WARMUP min/max | -2272 / 18861 | -3392 / 27767 |
| 正式RAW-L min/max | -61 / 26 | -68 / 48 |
| 正式RAW-L交流峰值 | 49 | 62 |
| 正式RAW-L平均绝对交流幅度 | 14 | 12 |
| 后四窗口平均绝对交流幅度 | 11 / 10 / 16 / 5 | 21 / 6 / 13 / 7 |
| gain_q8 | 2048（8倍上限） | 2048（8倍上限） |
| PREPARED平均绝对交流幅度 | 118 | 102 |
| 播放duty_A | 742..756 | 739..759 |
| 总耗时 | 9375ms | 9374ms |

两轮WARMUP和RECORD均完整、rx_events=1、early_idle=0、idle=1、CPU剩余0、DTC剩余块0，哨兵和填充检查正常，右声道全零。正式段不再有v1接近满量程的峰值和大幅漂移，这支持启动异常线索；寄存器LRCONT=1本身不代替示波器波形。安静/发声正式段均只有很小幅度，无明确声学响应；8倍后仍远小于参考音，不能据此判麦克风已损坏或直接提高音量。下一轮单独对照24位接收/符号与对齐，保持播放处理参数。

两轮用户原始日志：

```text
msh >
msh >
msh >
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY diagnostic v2: REF -> WARMUP -> countdown -> RECORD -> REPLAY -> REF
REPLAY stage 1: reference beep BEFORE; remember whether you hear it
PCM REF-BEFORE n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-BEFORE mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-BEFORE start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-BEFORE output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-BEFORE duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY stage 2: warmup then countdown; keep quiet until recording cue
MIC pins P403=12010000 P404=12010000 P406=12010000
MIC config: PCM=16 slot=32 channels=2 bytes=65536 expected=1022ms
MIC clocks: PCLKD=120000000 GPT1 period=117 BCLK~1025641Hz LRCLK~16026Hz
MIC SSI opened: SSICR=400B4000 SSIFCR=80000000 SSIOFR=00000100
MIC WS_CONTINUE ON; FIFO_access=2 bytes RX_IRQ=38
MIC DTC access=2 bytes CRA_raw=0202 block_length=2 src=4004E01C
MIC WARMUP: receiving and discarding startup data; keep quiet
MIC WARMUP result=0 complete=1 rx_events=1 early_idle=0 idle=1
MIC WARMUP elapsed=1022 ms remaining_CPU_words=0
MIC WARMUP end: SSICR=440B4001 SSISR=00000000 SSIFSR=00010000
MIC WARMUP DTC remaining_blocks=0 remaining_length=2
PCM WARMUP-L n=16384 min=-2272 max=18861 mean=284 ac_peak=18577
PCM WARMUP-L mean_abs_ac=2712 zero=25 changed=14108 clipped=0
PCM WARMUP-R n=16384 min=0 max=0 mean=0 ac_peak=0
PCM WARMUP-R mean_abs_ac=0 zero=16384 changed=0 clipped=0
MIC countdown: SSI stays open, SSIOFR=00000100 LRCONT=1
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC RECORD NOW: keep quiet OR sustain AH for this entire 1 second
MIC RECORD result=0 complete=1 rx_events=1 early_idle=0 idle=1
MIC RECORD elapsed=1023 ms remaining_CPU_words=0
MIC RECORD end: SSICR=440B4001 SSISR=00000000 SSIFSR=00010000
MIC RECORD DTC remaining_blocks=0 remaining_length=2
MIC SSI closed: result=0
MIC buffer guards MATCH
MIC recording finished; stop speaking. REPLAY stage 3: inspect L/R and prepare
MIC buffer fill_A5A5=0/32768 (large count suggests incomplete reception)
PCM RAW-L n=16384 min=-61 max=26 mean=-12 ac_peak=49
PCM RAW-L mean_abs_ac=14 zero=344 changed=13451 clipped=0
PCM RAW-R n=16384 min=0 max=0 mean=0 ac_peak=0
PCM RAW-R mean_abs_ac=0 zero=16384 changed=0 clipped=0
MIC window=0 frames=0..4095 (~256ms)
PCM WIN-L n=4096 min=-61 max=2 mean=-26 ac_peak=35
PCM WIN-L mean_abs_ac=11 zero=5 changed=3379 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=0 L=FFF1/-15 R=0000/0
MIC raw frame=1 L=FFF2/-14 R=0000/0
MIC raw frame=2 L=FFF2/-14 R=0000/0
MIC window=1 frames=4096..8191 (~256ms)
PCM WIN-L n=4096 min=-55 max=10 mean=-19 ac_peak=36
PCM WIN-L mean_abs_ac=10 zero=98 changed=3339 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=4096 L=FFDB/-37 R=0000/0
MIC raw frame=4097 L=FFD9/-39 R=0000/0
MIC raw frame=4098 L=FFD9/-39 R=0000/0
MIC window=2 frames=8192..12287 (~256ms)
PCM WIN-L n=4096 min=-44 max=26 mean=-9 ac_peak=35
PCM WIN-L mean_abs_ac=16 zero=43 changed=3364 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=8192 L=FFF0/-16 R=0000/0
MIC raw frame=8193 L=FFF2/-14 R=0000/0
MIC raw frame=8194 L=FFEC/-20 R=0000/0
MIC window=3 frames=12288..16383 (~256ms)
PCM WIN-L n=4096 min=-18 max=21 mean=3 ac_peak=21
PCM WIN-L mean_abs_ac=5 zero=198 changed=3367 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=12288 L=000F/15 R=0000/0
MIC raw frame=12289 L=0011/17 R=0000/0
MIC raw frame=12290 L=000E/14 R=0000/0
MIC DMA complete: frames=16384 min=-61 max=26 mean=-12 ac_peak=49
MIC DMA complete: changed=13451 clipped=0
AUDIO prepared: gain_q8=2048 (256=1x) output_peak=392 limited=0
PCM PREPARED-L n=16384 min=-392 max=304 mean=-7 ac_peak=385
PCM PREPARED-L mean_abs_ac=118 zero=256 changed=13467 clipped=0
AUDIO gain reason: target_peak=3000 max_gain=8x; use WIN-L stats to check startup outliers
REPLAY stage 4: AUDIO REPLAY NOW (recorded LEFT channel)
PCM RECORDED-L n=16384 min=-392 max=304 mean=-7 ac_peak=385
PCM RECORDED-L mean_abs_ac=118 zero=256 changed=13467 clipped=0
AUDIO RECORDED-L start: frames=16384 GPT2_period=7488 PWM_period=1500
AUDIO RECORDED-L output samples=16384/16384 pwm_error=0 result=1
AUDIO RECORDED-L duty_A=742..756 active_duty_samples=13425 elapsed=1029 ms
REPLAY stage 5: reference beep AFTER; compare with the first beep
PCM REF-AFTER n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-AFTER mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-AFTER start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-AFTER output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-AFTER duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY diagnostic finished result=1; report both beeps AND recorded voice
TEST RESULT audio-replay WAIT code=1 elapsed=9375 ms
TEST IDLE

msh >
msh >
msh >
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY diagnostic v2: REF -> WARMUP -> countdown -> RECORD -> REPLAY -> REF
REPLAY stage 1: reference beep BEFORE; remember whether you hear it
PCM REF-BEFORE n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-BEFORE mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-BEFORE start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-BEFORE output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-BEFORE duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY stage 2: warmup then countdown; keep quiet until recording cue
MIC pins P403=12010000 P404=12010000 P406=12010000
MIC config: PCM=16 slot=32 channels=2 bytes=65536 expected=1022ms
MIC clocks: PCLKD=120000000 GPT1 period=117 BCLK~1025641Hz LRCLK~16026Hz
MIC SSI opened: SSICR=400B4000 SSIFCR=80000000 SSIOFR=00000100
MIC WS_CONTINUE ON; FIFO_access=2 bytes RX_IRQ=38
MIC DTC access=2 bytes CRA_raw=0202 block_length=2 src=4004E01C
MIC WARMUP: receiving and discarding startup data; keep quiet
MIC WARMUP result=0 complete=1 rx_events=1 early_idle=0 idle=1
MIC WARMUP elapsed=1022 ms remaining_CPU_words=0
MIC WARMUP end: SSICR=440B4001 SSISR=00000000 SSIFSR=00010000
MIC WARMUP DTC remaining_blocks=0 remaining_length=2
PCM WARMUP-L n=16384 min=-3392 max=27767 mean=-146 ac_peak=27913
PCM WARMUP-L mean_abs_ac=3343 zero=0 changed=14471 clipped=0
PCM WARMUP-R n=16384 min=0 max=0 mean=0 ac_peak=0
PCM WARMUP-R mean_abs_ac=0 zero=16384 changed=0 clipped=0
MIC countdown: SSI stays open, SSIOFR=00000100 LRCONT=1
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC RECORD NOW: keep quiet OR sustain AH for this entire 1 second
MIC RECORD result=0 complete=1 rx_events=1 early_idle=0 idle=1
MIC RECORD elapsed=1023 ms remaining_CPU_words=0
MIC RECORD end: SSICR=440B4001 SSISR=00000000 SSIFSR=00010000
MIC RECORD DTC remaining_blocks=0 remaining_length=2
MIC SSI closed: result=0
MIC buffer guards MATCH
MIC recording finished; stop speaking. REPLAY stage 3: inspect L/R and prepare
MIC buffer fill_A5A5=0/32768 (large count suggests incomplete reception)
PCM RAW-L n=16384 min=-68 max=48 mean=-6 ac_peak=62
PCM RAW-L mean_abs_ac=12 zero=477 changed=13504 clipped=0
PCM RAW-R n=16384 min=0 max=0 mean=0 ac_peak=0
PCM RAW-R mean_abs_ac=0 zero=16384 changed=0 clipped=0
MIC window=0 frames=0..4095 (~256ms)
PCM WIN-L n=4096 min=-68 max=33 mean=-17 ac_peak=51
PCM WIN-L mean_abs_ac=21 zero=57 changed=3386 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=0 L=0012/18 R=0000/0
MIC raw frame=1 L=0012/18 R=0000/0
MIC raw frame=2 L=0010/16 R=0000/0
MIC window=1 frames=4096..8191 (~256ms)
PCM WIN-L n=4096 min=-29 max=17 mean=-8 ac_peak=25
PCM WIN-L mean_abs_ac=6 zero=94 changed=3345 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=4096 L=FFFF/-1 R=0000/0
MIC raw frame=4097 L=0000/0 R=0000/0
MIC raw frame=4098 L=0002/2 R=0000/0
MIC window=2 frames=8192..12287 (~256ms)
PCM WIN-L n=4096 min=-35 max=48 mean=1 ac_peak=47
PCM WIN-L mean_abs_ac=13 zero=108 changed=3368 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=8192 L=FFF8/-8 R=0000/0
MIC raw frame=8193 L=FFF7/-9 R=0000/0
MIC raw frame=8194 L=FFF6/-10 R=0000/0
MIC window=3 frames=12288..16383 (~256ms)
PCM WIN-L n=4096 min=-26 max=31 mean=-2 ac_peak=33
PCM WIN-L mean_abs_ac=7 zero=218 changed=3402 clipped=0
PCM WIN-R n=4096 min=0 max=0 mean=0 ac_peak=0
PCM WIN-R mean_abs_ac=0 zero=4096 changed=0 clipped=0
MIC raw frame=12288 L=001A/26 R=0000/0
MIC raw frame=12289 L=0018/24 R=0000/0
MIC raw frame=12290 L=0018/24 R=0000/0
MIC DMA complete: frames=16384 min=-68 max=48 mean=-6 ac_peak=62
MIC DMA complete: changed=13504 clipped=0
AUDIO prepared: gain_q8=2048 (256=1x) output_peak=496 limited=0
PCM PREPARED-L n=16384 min=-496 max=432 mean=-7 ac_peak=489
PCM PREPARED-L mean_abs_ac=102 zero=506 changed=13519 clipped=0
AUDIO gain reason: target_peak=3000 max_gain=8x; use WIN-L stats to check startup outliers
REPLAY stage 4: AUDIO REPLAY NOW (recorded LEFT channel)
PCM RECORDED-L n=16384 min=-496 max=432 mean=-7 ac_peak=489
PCM RECORDED-L mean_abs_ac=102 zero=506 changed=13519 clipped=0
AUDIO RECORDED-L start: frames=16384 GPT2_period=7488 PWM_period=1500
AUDIO RECORDED-L output samples=16384/16384 pwm_error=0 result=1
AUDIO RECORDED-L duty_A=739..759 active_duty_samples=10720 elapsed=1029 ms
REPLAY stage 5: reference beep AFTER; compare with the first beep
PCM REF-AFTER n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-AFTER mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-AFTER start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-AFTER output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-AFTER duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY diagnostic finished result=1; report both beeps AND recorded voice
TEST RESULT audio-replay WAIT code=1 elapsed=9374 ms
TEST IDLE
```


## 第五、六轮实物记录：v3 PCM24安静/发声仍无回放（2026-10-06）

固件 `1736004`。用户明确确认第一轮安静、第二轮持续发声，中间仍无声、前后参考音正常。

| 指标 | 安静 | 持续发声 |
| --- | ---: | ---: |
| 原始PCM24 min/max | -14039 / 4370 | -14931 / 7631 |
| 原始PCM24 ac_peak / mean_abs_ac | 9582 / 2888 | 12309 / 4594 |
| 转PCM16 min/max | -54 / 17 | -58 / 29 |
| 转PCM16 ac_peak / mean_abs_ac | 36 / 11 | 47 / 17 |
| 处理后峰值 / mean_abs_ac | 288 / 88 | 376 / 140 |
| 播放duty_A | 744..756 | 743..758 |
| 总耗时 | 7867ms | 7866ms |

两轮WARMUP和RECORD均511ms完成、rx_events=1、early_idle=0、idle=1；CPU剩余0、DTC剩余块0、哨兵完整、填充值无残留，右声道全零。日志中的00FFE3F3扩展为-7181，再除以256转为-28，与打印值一致。未见明显符号扩展/转换错位；不能据此证明I²S物理时序完全正确。发声整段平均幅度有所增加，但窗口差异不一致，处理后仍很弱；不把这些统计记为语音采集或回放验收通过。

原始日志：

```text


msh >
msh >
msh >
msh >
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY diagnostic v3 PCM24: REF -> WARMUP -> countdown -> RECORD -> REPLAY -> REF
REPLAY stage 1: reference beep BEFORE; remember whether you hear it
PCM REF-BEFORE n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-BEFORE mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-BEFORE start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-BEFORE output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-BEFORE duty_A=682..818 active_duty_samples=7673 elapsed=519 ms
REPLAY stage 2: warmup then countdown; keep quiet until recording cue
MIC pins P403=12010000 P404=12010000 P406=12010000
MIC config: PCM=24 slot=32 channels=2 bytes=65536 expected=511ms
MIC clocks: PCLKD=120000000 GPT1 period=117 BCLK~1025641Hz LRCLK~16026Hz
MIC SSI opened: SSICR=402B4200 SSIFCR=80000000 SSIOFR=00000100
MIC WS_CONTINUE ON; FIFO_access=4 bytes RX_IRQ=38
MIC DTC access=4 bytes CRA_raw=0202 block_length=2 src=4004E01C
MIC WARMUP: receiving and discarding startup data; keep quiet
MIC WARMUP result=0 complete=1 rx_events=1 early_idle=0 idle=1
MIC WARMUP elapsed=511 ms remaining_CPU_words=0
MIC WARMUP end: SSICR=442B4201 SSISR=00000000 SSIFSR=00010000
MIC WARMUP DTC remaining_blocks=0 remaining_length=2
PCM24 WARMUP-L n=8192 min=-583940 max=4846586 mean=440573 ac_peak=4406013
PCM24 WARMUP-L mean_abs_ac=1140032 zero=21 changed=8168 clipped=0
PCM24 WARMUP-R n=8192 min=0 max=0 mean=0 ac_peak=0
PCM24 WARMUP-R mean_abs_ac=0 zero=8192 changed=0 clipped=0
MIC countdown: SSI stays open, SSIOFR=00000100 LRCONT=1
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC RECORD NOW: keep quiet OR sustain AH for this entire half second
MIC RECORD result=0 complete=1 rx_events=1 early_idle=0 idle=1
MIC RECORD elapsed=511 ms remaining_CPU_words=0
MIC RECORD end: SSICR=442B4201 SSISR=00000000 SSIFSR=00010000
MIC RECORD DTC remaining_blocks=0 remaining_length=2
MIC SSI closed: result=0
MIC buffer guards MATCH
MIC recording finished; stop speaking. REPLAY stage 3: inspect L/R and prepare
MIC buffer fill_A5A5A5A5=0/16384 (large count suggests incomplete reception)
PCM24 RAW-L n=8192 min=-14039 max=4370 mean=-5212 ac_peak=9582
PCM24 RAW-L mean_abs_ac=2888 zero=0 changed=8182 clipped=0
PCM24 RAW-R n=8192 min=0 max=0 mean=0 ac_peak=0
PCM24 RAW-R mean_abs_ac=0 zero=8192 changed=0 clipped=0
MIC window=0 frames=0..2047 (~128ms)
PCM24 WIN-L n=2048 min=-12807 max=-2453 mean=-8053 ac_peak=5600
PCM24 WIN-L mean_abs_ac=1535 zero=0 changed=2045 clipped=0
PCM24 WIN-R n=2048 min=0 max=0 mean=0 ac_peak=0
PCM24 WIN-R mean_abs_ac=0 zero=2048 changed=0 clipped=0
MIC raw frame=0 L=00FFE3F3/-7181 R=00000000/0
MIC PCM16 frame=0 L=-28 R=0 (signed24 / 256)
MIC raw frame=1 L=00FFE5BD/-6723 R=00000000/0
MIC PCM16 frame=1 L=-26 R=0 (signed24 / 256)
MIC raw frame=2 L=00FFE6D6/-6442 R=00000000/0
MIC PCM16 frame=2 L=-25 R=0 (signed24 / 256)
MIC window=1 frames=2048..4095 (~128ms)
PCM24 WIN-L n=2048 min=-14039 max=-165 mean=-7070 ac_peak=6969
PCM24 WIN-L mean_abs_ac=2700 zero=0 changed=2046 clipped=0
PCM24 WIN-R n=2048 min=0 max=0 mean=0 ac_peak=0
PCM24 WIN-R mean_abs_ac=0 zero=2048 changed=0 clipped=0
MIC raw frame=2048 L=00FFE9AB/-5717 R=00000000/0
MIC PCM16 frame=2048 L=-22 R=0 (signed24 / 256)
MIC raw frame=2049 L=00FFE6F1/-6415 R=00000000/0
MIC PCM16 frame=2049 L=-25 R=0 (signed24 / 256)
MIC raw frame=2050 L=00FFE82B/-6101 R=00000000/0
MIC PCM16 frame=2050 L=-23 R=0 (signed24 / 256)
MIC window=2 frames=4096..6143 (~128ms)
PCM24 WIN-L n=2048 min=-11461 max=3970 mean=-3676 ac_peak=7785
PCM24 WIN-L mean_abs_ac=1850 zero=0 changed=2045 clipped=0
PCM24 WIN-R n=2048 min=0 max=0 mean=0 ac_peak=0
PCM24 WIN-R mean_abs_ac=0 zero=2048 changed=0 clipped=0
MIC raw frame=4096 L=00FFD4ED/-11027 R=00000000/0
MIC PCM16 frame=4096 L=-43 R=0 (signed24 / 256)
MIC raw frame=4097 L=00FFD73A/-10438 R=00000000/0
MIC PCM16 frame=4097 L=-40 R=0 (signed24 / 256)
MIC raw frame=4098 L=00FFD52A/-10966 R=00000000/0
MIC PCM16 frame=4098 L=-42 R=0 (signed24 / 256)
MIC window=3 frames=6144..8191 (~128ms)
PCM24 WIN-L n=2048 min=-7420 max=4370 mean=-2049 ac_peak=6419
PCM24 WIN-L mean_abs_ac=2379 zero=0 changed=2043 clipped=0
PCM24 WIN-R n=2048 min=0 max=0 mean=0 ac_peak=0
PCM24 WIN-R mean_abs_ac=0 zero=2048 changed=0 clipped=0
MIC raw frame=6144 L=00000D48/3400 R=00000000/0
MIC PCM16 frame=6144 L=13 R=0 (signed24 / 256)
MIC raw frame=6145 L=00000CB3/3251 R=00000000/0
MIC PCM16 frame=6145 L=12 R=0 (signed24 / 256)
MIC raw frame=6146 L=00000A5A/2650 R=00000000/0
MIC PCM16 frame=6146 L=10 R=0 (signed24 / 256)
PCM CONVERTED-L n=8192 min=-54 max=17 mean=-19 ac_peak=36
PCM CONVERTED-L mean_abs_ac=11 zero=116 changed=6706 clipped=0
MIC PCM16 input: frames=8192 min=-54 max=17 mean=-19 ac_peak=36
MIC PCM16 input: changed=6706 clipped=0
AUDIO prepared: gain_q8=2048 (256=1x) output_peak=288 limited=0
PCM PREPARED-L n=8192 min=-280 max=288 mean=-7 ac_peak=295
PCM PREPARED-L mean_abs_ac=88 zero=239 changed=6705 clipped=0
AUDIO gain reason: target_peak=3000 max_gain=8x; use WIN-L stats to check startup outliers
REPLAY stage 4: AUDIO REPLAY NOW (recorded LEFT channel)
PCM RECORDED-L n=8192 min=-280 max=288 mean=-7 ac_peak=295
PCM RECORDED-L mean_abs_ac=88 zero=239 changed=6705 clipped=0
AUDIO RECORDED-L start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO RECORDED-L output samples=8192/8192 pwm_error=0 result=1
AUDIO RECORDED-L duty_A=744..756 active_duty_samples=5508 elapsed=518 ms
REPLAY stage 5: reference beep AFTER; compare with the first beep
PCM REF-AFTER n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-AFTER mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-AFTER start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-AFTER output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-AFTER duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY diagnostic finished result=1; report both beeps AND recorded voice
TEST RESULT audio-replay WAIT code=1 elapsed=7867 ms
TEST IDLE

msh >
msh >
msh >
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >REPLAY diagnostic v3 PCM24: REF -> WARMUP -> countdown -> RECORD -> REPLAY -> REF
REPLAY stage 1: reference beep BEFORE; remember whether you hear it
PCM REF-BEFORE n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-BEFORE mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-BEFORE start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-BEFORE output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-BEFORE duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY stage 2: warmup then countdown; keep quiet until recording cue
MIC pins P403=12010000 P404=12010000 P406=12010000
MIC config: PCM=24 slot=32 channels=2 bytes=65536 expected=511ms
MIC clocks: PCLKD=120000000 GPT1 period=117 BCLK~1025641Hz LRCLK~16026Hz
MIC SSI opened: SSICR=402B4200 SSIFCR=80000000 SSIOFR=00000100
MIC WS_CONTINUE ON; FIFO_access=4 bytes RX_IRQ=38
MIC DTC access=4 bytes CRA_raw=0202 block_length=2 src=4004E01C
MIC WARMUP: receiving and discarding startup data; keep quiet
MIC WARMUP result=0 complete=1 rx_events=1 early_idle=0 idle=1
MIC WARMUP elapsed=511 ms remaining_CPU_words=0
MIC WARMUP end: SSICR=442B4201 SSISR=00000000 SSIFSR=00010000
MIC WARMUP DTC remaining_blocks=0 remaining_length=2
PCM24 WARMUP-L n=8192 min=-862704 max=7109002 mean=374743 ac_peak=6734259
PCM24 WARMUP-L mean_abs_ac=1463796 zero=0 changed=8188 clipped=0
PCM24 WARMUP-R n=8192 min=0 max=0 mean=0 ac_peak=0
PCM24 WARMUP-R mean_abs_ac=0 zero=8192 changed=0 clipped=0
MIC countdown: SSI stays open, SSIOFR=00000100 LRCONT=1
MIC recording starts in 3...
MIC recording starts in 2...
MIC recording starts in 1...
MIC RECORD NOW: keep quiet OR sustain AH for this entire half second
MIC RECORD result=0 complete=1 rx_events=1 early_idle=0 idle=1
MIC RECORD elapsed=511 ms remaining_CPU_words=0
MIC RECORD end: SSICR=442B4201 SSISR=00000000 SSIFSR=00010000
MIC RECORD DTC remaining_blocks=0 remaining_length=2
MIC SSI closed: result=0
MIC buffer guards MATCH
MIC recording finished; stop speaking. REPLAY stage 3: inspect L/R and prepare
MIC buffer fill_A5A5A5A5=0/16384 (large count suggests incomplete reception)
PCM24 RAW-L n=8192 min=-14931 max=7631 mean=-4678 ac_peak=12309
PCM24 RAW-L mean_abs_ac=4594 zero=0 changed=8186 clipped=0
PCM24 RAW-R n=8192 min=0 max=0 mean=0 ac_peak=0
PCM24 RAW-R mean_abs_ac=0 zero=8192 changed=0 clipped=0
MIC window=0 frames=0..2047 (~128ms)
PCM24 WIN-L n=2048 min=-14931 max=-2558 mean=-8186 ac_peak=6745
PCM24 WIN-L mean_abs_ac=2131 zero=0 changed=2045 clipped=0
PCM24 WIN-R n=2048 min=0 max=0 mean=0 ac_peak=0
PCM24 WIN-R mean_abs_ac=0 zero=2048 changed=0 clipped=0
MIC raw frame=0 L=00FFF4B3/-2893 R=00000000/0
MIC PCM16 frame=0 L=-11 R=0 (signed24 / 256)
MIC raw frame=1 L=00FFF471/-2959 R=00000000/0
MIC PCM16 frame=1 L=-11 R=0 (signed24 / 256)
MIC raw frame=2 L=00FFF1FC/-3588 R=00000000/0
MIC PCM16 frame=2 L=-14 R=0 (signed24 / 256)
MIC window=1 frames=2048..4095 (~128ms)
PCM24 WIN-L n=2048 min=-14752 max=-3604 mean=-9167 ac_peak=5585
PCM24 WIN-L mean_abs_ac=1543 zero=0 changed=2047 clipped=0
PCM24 WIN-R n=2048 min=0 max=0 mean=0 ac_peak=0
PCM24 WIN-R mean_abs_ac=0 zero=2048 changed=0 clipped=0
MIC raw frame=2048 L=00FFDE40/-8640 R=00000000/0
MIC PCM16 frame=2048 L=-33 R=0 (signed24 / 256)
MIC raw frame=2049 L=00FFDB9A/-9318 R=00000000/0
MIC PCM16 frame=2049 L=-36 R=0 (signed24 / 256)
MIC raw frame=2050 L=00FFDD03/-8957 R=00000000/0
MIC PCM16 frame=2050 L=-34 R=0 (signed24 / 256)
MIC window=2 frames=4096..6143 (~128ms)
PCM24 WIN-L n=2048 min=-13860 max=7631 mean=161 ac_peak=14021
PCM24 WIN-L mean_abs_ac=4039 zero=0 changed=2046 clipped=0
PCM24 WIN-R n=2048 min=0 max=0 mean=0 ac_peak=0
PCM24 WIN-R mean_abs_ac=0 zero=2048 changed=0 clipped=0
MIC raw frame=4096 L=00FFDAAE/-9554 R=00000000/0
MIC PCM16 frame=4096 L=-37 R=0 (signed24 / 256)
MIC raw frame=4097 L=00FFDD2F/-8913 R=00000000/0
MIC PCM16 frame=4097 L=-34 R=0 (signed24 / 256)
MIC raw frame=4098 L=00FFDB92/-9326 R=00000000/0
MIC PCM16 frame=4098 L=-36 R=0 (signed24 / 256)
MIC window=3 frames=6144..8191 (~128ms)
PCM24 WIN-L n=2048 min=-8036 max=4644 mean=-1520 ac_peak=6516
PCM24 WIN-L mean_abs_ac=2389 zero=0 changed=2045 clipped=0
PCM24 WIN-R n=2048 min=0 max=0 mean=0 ac_peak=0
PCM24 WIN-R mean_abs_ac=0 zero=2048 changed=0 clipped=0
MIC raw frame=6144 L=000006C6/1734 R=00000000/0
MIC PCM16 frame=6144 L=6 R=0 (signed24 / 256)
MIC raw frame=6145 L=00000552/1362 R=00000000/0
MIC PCM16 frame=6145 L=5 R=0 (signed24 / 256)
MIC raw frame=6146 L=000008E5/2277 R=00000000/0
MIC PCM16 frame=6146 L=8 R=0 (signed24 / 256)
PCM CONVERTED-L n=8192 min=-58 max=29 mean=-18 ac_peak=47
PCM CONVERTED-L mean_abs_ac=17 zero=167 changed=6791 clipped=0
MIC PCM16 input: frames=8192 min=-58 max=29 mean=-18 ac_peak=47
MIC PCM16 input: changed=6791 clipped=0
AUDIO prepared: gain_q8=2048 (256=1x) output_peak=376 limited=0
PCM PREPARED-L n=8192 min=-320 max=376 mean=0 ac_peak=376
PCM PREPARED-L mean_abs_ac=140 zero=145 changed=6786 clipped=0
AUDIO gain reason: target_peak=3000 max_gain=8x; use WIN-L stats to check startup outliers
REPLAY stage 4: AUDIO REPLAY NOW (recorded LEFT channel)
PCM RECORDED-L n=8192 min=-320 max=376 mean=0 ac_peak=376
PCM RECORDED-L mean_abs_ac=140 zero=145 changed=6786 clipped=0
AUDIO RECORDED-L start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO RECORDED-L output samples=8192/8192 pwm_error=0 result=1
AUDIO RECORDED-L duty_A=743..758 active_duty_samples=7010 elapsed=518 ms
REPLAY stage 5: reference beep AFTER; compare with the first beep
PCM REF-AFTER n=8192 min=-3000 max=3000 mean=0 ac_peak=3000
PCM REF-AFTER mean_abs_ac=1866 zero=513 changed=8190 clipped=0
AUDIO REF-AFTER start: frames=8192 GPT2_period=7488 PWM_period=1500
AUDIO REF-AFTER output samples=8192/8192 pwm_error=0 result=1
AUDIO REF-AFTER duty_A=682..818 active_duty_samples=7673 elapsed=518 ms
REPLAY diagnostic finished result=1; report both beeps AND recorded voice
TEST RESULT audio-replay WAIT code=1 elapsed=7866 ms
TEST IDLE
```

## 回到原始独立audio-mic复测（2026-10-06）

按前一轮引导顺序解释为第一轮安静、第二轮提前持续发声；本轮用户只提供两条日志，没有另外描述发声距离或波形。该独立例程未修改、未重新烧录、不包含扬声器播放。助手未打开COM8。重复发送的同一组日志只归档一次。

```text
msh >hmi_test audio-mic
TEST BEGIN audio-mic
msh >MIC capture 8192 stereo frames at ~16026Hz; microphone on LEFT
MIC DMA complete left min=-580934 max=4779456 changed=7821/8191; acoustic response needs speaking test
TEST RESULT audio-mic PASS code=0 elapsed=540 ms
TEST IDLE
msh >hmi_test audio-mic
TEST BEGIN audio-mic
msh >MIC capture 8192 stereo frames at ~16026Hz; microphone on LEFT
MIC DMA complete left min=-776612 max=8387346 changed=8183/8191; acoustic response needs speaking test
TEST RESULT audio-mic PASS code=0 elapsed=539 ms
TEST IDLE
```

两轮各8192帧采集完成，540/539ms后正常PASS/IDLE。最大值从4779456升到8387346，约1.755倍；后者接近24位正满量程8388607。变化数7821/8183。结果说明独立采集流程完成、数据有变化，存在发声响应线索；仅最大值和changed不能证明正确人声，也不能断言麦克风损坏。接近满量程不直接等于声学饱和，需要检查峰值落在哪个时间窗口。

源码差异：独立audio-mic直接录下启动后的第一段半秒，沿用WS_CONTINUE_OFF；回放v3丢弃启动段、WS_CONTINUE_ON并保持SSI/GPT1跨过3秒倒计时，再录下一段半秒。两者现在都为24位右对齐、32位时隙、4字节FIFO/DTC、相同帧率。因此独立例程大峰值与回放正式段小峰值不能当作同一采样状态的对照。当前优先需要对独立采集数据分段，确认大幅值是否只集中于启动段；本轮按用户单独复测的要求保留原采集代码，回放仍未验收。

已从Studio安装包V3.1原理图Audio页核对U9为MSM261S3526Z0CM，L/R接地选择左声道，SD有100k下拉。依据文件 `C:/RT-ThreadStudio/repo/Extract/Board_Support_Packages/RealThread/HMI-Board/1.2.0/documents/hmi-board-v3.1-schematic.pdf`（PDF第12页）。当前未取得该麦克风型号的有效数据手册，不把推测的最低时钟/启动时间写成已确认规格。
