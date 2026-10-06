# 独立 MVP 重构回归（2026-10-01）

2026-10-07新增audio-raw原始采集诊断：8组真实C函数ARM检查、38项主机检查和原回放15组ARM回归通过，Studio构建0 errors/0 warnings（Flash1162152、静态RAM542616字节）。原始WAV/试听副本/波形频谱工具已实现，合成输入只能验证工具，实物声学与导出仍待用户验收，详见`audio-raw.md`。

## v6采集溢出，v7修复待复验（2026-10-06）

后续v7板测：240384/240384帧、626/626块、5000ms，overrun=0；处理最大1291µs、平均1242µs，等待最大41µs，队列峰值1/4；回放80128/80128样本、5001ms、pwm_error=0。用户持续说话，正面10cm以内，回放只有沙沙声。队列问题已解决，语音未验收。按用户要求仅分析和归档，不改源代码/不烧录，详见`audio-replay-v7-analysis.md`。

用户两轮`audio-replay`分别完成16896/240384帧（46/626块、361ms）、106752/240384帧（280/626块、2230ms），均overrun=1、result=-3，FAIL/IDLE；没有进入回放。原始日志已存`audio-replay.md`。不能将此记为语音测试通过。

真实Cortex-M4 SRAM处理基准：每块输入间隔7987.2µs，O0最大6151.6µs、O2最大1869.8µs；基准不包含完整应用时序。v7局部O2、四块环形缓冲、信号量唤醒、录音临时优先级14，退出恢复原优先级/释放IPC；增加DWT处理/等待耗时、队列峰值。15组ARM检查（含24ms延迟完整内容和34ms溢出保护）、30项主机检查通过，构建0 errors/0 warnings，Flash1156412、静态RAM542280字节。DAP-LINK/PyOCD已成功烧录1156496字节、退出码0（`logs/audio-replay-overrun-flash.log`）。实际5秒连续采集及可辨认语音仍待用户验收，助手未打开COM8。


当前交付结构为 `src/test-main.c` + `src/test/` 下 46 个独立 C 文件；`src/hal_entry.c` 与重构前逐字节一致。以下是本轮新固件自测，后面的原始记录保留为历史证据，不自动等同于重构后的人工验收。

## v5无明显发声响应，v6恢复BSP麦克风时钟（2026-10-06）

用户确认v5第一轮持续说话，只听到沙沙声；随后第二轮保持安静（除环境噪音）。说话HP24 mean_abs第2～5秒1295/946/981/910，安静1359/976/957/882；第3～5秒平均945.7/938.3，仅差约0.8%。两轮均采集80128帧5000ms/输出80128样本5001ms、传输错误0，语音验收仍失败。两轮完整日志在 `audio-replay.md`；不再将原独立audio-mic启动极值视为人声质量通过的证据。

原始BSP的GPT1为120MHz/39约3.077MHz，助手早期改为117约1.026MHz。型号公开二手摘要提示正常模式1.5～4MHz、低功耗150～600kHz；尚未取得核验原厂PDF，不能认定时钟已是根因。v6优先恢复BSP配置，约48kHz连续采集240384帧，经31点FIR低通、三抽一保存80128个约16kHz mu-law样本；关闭采集硬件后回放，两个阶段理论均4.999987秒。原始HP24按48077帧/秒统计；堆仍80136字节，DTC双缓冲静态6160字节含哨兵。

14组真实C函数ARM检查、30项主机检查通过，验证全部240384原始帧到80128保存样本的顺序、独立移位历史对照环形FIR、5秒时长、500Hz保留/16kHz混叠抑制及原有错误清理。Studio构建0 errors/0 warnings，Flash1156036字节、静态RAM536072字节；DAP-LINK/PyOCD成功烧录1156112字节、退出码0，日志 `logs/audio-replay-clock-*.log`；助手不打开COM8，v6物理时序与语音仍待用户验收。

## 五秒回放v4实测失败与v5高通处理历史（2026-10-06）

用户确认全程持续说话但回放仍无声。v4采集80128/80128帧、626/626块，5000ms；输出80128/80128样本，5001ms，read/overrun/early_idle/stop/pwm_error均0，13241ms后WAIT/IDLE。第二秒原始均值-93577，后续逐渐趋近0；全局减均值不能移除慢漂移，gain_q8=875约3.42倍，limited=15219约19.0%。完整日志见 `audio-replay.md`，不记为人声采集成功。

v5保持独立录5秒/后回放5秒，先以PCM24约40Hz高通消除慢漂移，再除以32压缩为mu-law，多保留3位弱信号。打印每秒HP24 min/max/mean_abs/storage_clipped；待安静/说话对照和听感确认，尚不能证明物理麦克风问题解决。13组ARM检查与30项主机检查通过，包含直流衰减、约500Hz语音信号叠加漂移的幅度保留，以及原有完整采集/回放/错误清理。Studio编译0 errors/0 warnings，Flash1155332字节、静态RAM531848字节；DAP-LINK/PyOCD成功烧录1155344字节、退出码0，记录 `logs/audio-replay-highpass-*.log`；不打开COM8。

## 五秒录音/五秒回放重新生成：v4软件交付历史（2026-10-06）

用户要求替换之前诊断流程，重写test-audio-replay：独立连续采集5秒，停止SSI/DTC/GPT1，再回放同一段完整录音5秒。24位PCM/32位时隙/原帧率约16025.64Hz，80128帧理论4.999987秒；双缓冲128帧/块，共626块，mu-law编码保存80128字节，堆申请80136字节含哨兵，两个DTC缓冲静态2064字节。逐秒原始统计用于区分启动段；回放用后4秒估计幅度、全5秒均回放，最大64倍增益、限幅±3000。保留单C/唯一线程入口，不改其他例程或hal_entry，不占用COM8。

12组实际C函数ARM模拟检查通过，验证全部帧内容与顺序、连续5秒采集/5秒播放及互斥阶段、G.711/符号扩展、取消/超时/溢出/提前IDLE/读与启动错误/守护字/内存失败/幅度边界和清理；30项主机结构/工具回归通过。记录 `logs/audio-replay-5s-arm.log`、`logs/audio-replay-5s-host-tests.log`、`logs/audio-replay-5s-build.log`、`logs/audio-replay-5s-flash.log`；Studio编译0 errors/0 warnings，Flash1154804字节、静态RAM531720字节；DAP-LINK/PyOCD成功烧录1154832字节、退出码0。后续板测计时与帧数完成，但持续说话回放无声，见上方v5排查记录。

## 独立麦克风复测与v3回放日志归档（2026-10-06）

用户要求重新测试原有 `audio-mic`，按引导顺序为安静/提前持续发声。两轮8192帧完成，min/max分别-580934/4779456与-776612/8387346，changed7821/8183，540/539ms后PASS/IDLE。最大值增加约1.755倍、第二轮接近24位正满量程；采集流程完成、有发声响应线索，但整段极值不能排除启动瞬态，不能据此确认音质或回放通过。原例程直接录启动半秒，v3回放丢弃启动段再倒计时录音；24位格式与帧率相同，两者采样时机不同。本轮未改代码、未烧录、不占用COM8。

此前v3安静/发声两轮用户确认前后参考音正常，中间仍无声。原始24位mean_abs_ac2888/4594，转PCM16为11/17，处理后88/140，播放各8192/8192、pwm_error=0，7867/7866ms后正常WAIT/IDLE。符号扩展与转换打印相符，不能由此证明物理I²S时序或录到正确人声。两轮完整原始日志与本轮独立麦克风日志已补入 `audio-replay.md`。当前录音回放仍未验收，下一定位点是启动段与稳定段差异，而非仅凭极值提高音量。

## 录音回放：v2预热数据改善，发声仍无响应；v3格式对照（2026-10-06）

用户按顺序完成v2安静/持续发声两轮，确认中间无声、前后参考音正常。预热段大幅数据被丢弃，正式段安静/发声交流峰值49/62、mean_abs_ac14/12，增益均8倍上限，处理后平均幅度118/102。两轮接收、IDLE、DTC和哨兵正常，正常WAIT/IDLE，耗时9375/9374ms；完整原始日志已归档 `audio-replay.md`。启动峰值在正式段消失，但未见明确人声响应，仍未验收回放。

当前v3改为原始audio-mic的PCM24/32位时隙、4字节FIFO/DTC，打印原始字/显式符号扩展/转PCM16结果。保留预热、连续时钟和原播放处理参数；同一65536字节缓冲录8192帧约半秒。19组ARM检查和30项主机回归通过，Studio构建0 errors/0 warnings，Flash1158364字节、静态RAM529480字节，记录 `logs/audio-replay-pcm24-*.log`。DAP-LINK/PyOCD已成功烧录1158416字节、退出码0。未占用COM8，不宣称格式问题已确认或修复，待实物对照。

## 录音回放：v1 发声对照失败，交付 v2 预热诊断（2026-10-06）

用户明确确认持续发声，中间回放仍无声，前后参考音正常。v1安静/发声后三窗口mean_abs_ac分别204/349/220与239/359/199，处理后377与371，两轮gain均23/256；没有明确人声响应。完整发声原始日志已归档 `audio-replay.md`，不能把changed或样本完成判为“已录到声音”。

v2先接收并丢弃启动段，WS_CONTINUE保持SSI时钟，倒计时后录正式段；完成回调保存状态并Stop，等待IDLE后重启/关闭，打印WARMUP/RECORD、LRCONT与引脚复用。保留16位采集和原幅度处理，预热复用同一缓冲，不改 `src/hal_entry.c` 或其他例程。17组ARM模拟检查、30项主机回归通过；Studio编译0 errors/0 warnings，Flash1157420字节、静态RAM529480字节，记录 `logs/audio-replay-warmup-*.log`。DAP-LINK/PyOCD已成功烧录1157520字节、退出码0。本轮不打开COM8，录音回放仍未验收，等待用户实物复测。

## 录音回放恢复排查：诊断版 v1（2026-10-06）

安静基线已由用户完成：前后参考音正常，中间无声，8241ms后WAIT/IDLE。16384帧接收完成、无提前IDLE、缓冲哨兵完整、初始化填充值无残留；左声道变化，右声道全零。两个参考音均8192/8192、pwm_error=0、duty_A=682..818，支持本例程播放路径可以出声。第一256ms窗口有接近满量程的峰值32763，后三窗口均值-3145→-2432→-1258，疑似启动/稳定过程；整段增益23/256约0.09倍，处理后mean_abs_ac=377。安静无声不判录音失败，下一轮用同一固件持续发声对照，暂不修改采集格式或音量。完整日志和分析见 `audio-replay.md` 的“第一轮实物记录”。

用户在歌曲播放验收通过后恢复 `audio-replay`，要求逐步区分录音与播放问题并增加日志。当前先保持16位SSI/32位时隙及原有低幅处理，增加本文件同一RAM/PWM路径的前后参考音；录音停止后打印SSI/DTC状态、完成/提前IDLE、哨兵与填充值、原始左右声道、四段窗口和少量十六进制样本，处理后打印幅度与增益，三次播放各自打印进度、PWM错误、占空比范围。长日志拆为短行，适配现有128字节格式化缓冲；没有在采样中断内打印。

验证：30项主机回归通过；`validate_audio_replay.py` 扩展为12组实际C函数ARM模拟检查，覆盖PCM、统计、SSI回调、参考音、正常完成/EOF、取消/超时、PWM与启动失败清理及日志长度。Studio编译0 errors、0 warnings，Flash1156532字节、静态RAM529480字节。日志 `logs/audio-replay-diagnostic-host-tests.log`、`logs/audio-replay-diagnostic-arm.log`、`logs/audio-replay-diagnostic-build.log`。用户操作与诊断字段见 `audio-replay.md`。

已通过DAP-LINK/PyOCD成功烧录并复位，编程1156624字节，记录 `logs/audio-replay-diagnostic-flash.log`。当时安静轮已完成、发声轮待测；后续发声对照结果见上方v2记录；不宣称已查明或解决原因。助手不打开COM8，保持手动测试方式。旧回放失败状态与歌曲已通过状态保留。

## 歌曲播放：用户实物试听验收通过（2026-10-06）

用户在固件 `6d92ec9` 上运行默认歌曲，并确认“声音连续、正常”。实际日志：

```text
msh >hmi_test audio-song
TEST BEGIN audio-song
msh >SONG PCM mono rate=16000 samples=195729 bytes=391458 peak=3000 duration=12233 ms
SONG playing once on J8; PWM=80kHz, source=offline decoded MP3
SONG output samples=195729/195729 pwm_error=0; listening confirmation required
TEST RESULT audio-song WAIT code=1 elapsed=12258 ms
TEST IDLE
```

结论：全部样本完成、PWM 无错误、正常退出，结合人工试听，默认歌曲播放验收通过。固件保留观察性 WAIT，不修改为自动 PASS。本次覆盖默认片段的一次播放；未据此宣称长期压力、主动停止或任意歌曲均已实物验证。录音回放仍按用户要求暂停。

用户暂停录音回放，新增 `test-audio-song.c`。MP3 由电脑端转换为 16kHz、单声道、16 位 PCM；板端直接从 Flash 播放，不宣称在板上实现 MP3 解码。默认素材为本项目合成的《欢乐颂》器乐片段，195729 样本、391458 字节 PCM、峰值 3000、约 12.233 秒。独立配置 GPT6/P702/P703 的 80kHz PWM 和 GPT2 的 16kHz 采样中断，包含限幅、EOF、协作停止、超时、错误诊断及清理。

交付前自测：30 项主机回归通过（新增 7 项转换验证，结构检查更新为 46 个文件）；5 组实际 C 函数 ARM 模拟执行检查通过。Studio 编译 0 errors、0 warnings，Flash 1151748 字节、静态 RAM 529416 字节。构建日志 `logs/audio-song-build.log`，DAP-LINK/PyOCD 烧录成功并复位，写入 1151760 字节，烧录日志 `logs/audio-song-flash.log`。转换脚本 `scripts/mp3_to_array.py`，使用说明 `audio-song.md`。助手未打开 COM8，实际输出由用户手动运行后确认；录音回放保持暂停，未记为通过。

## 录音回放改进：回应“没有听到自己的声音”（2026-10-06）

用户自行测试后反馈“没有听到自己的声音”，因此旧版回放没有通过人工验收。已通过的低幅提示音仍保留；旧版样本数完成仅说明播放流程结束。旧版只有约半秒录音，立即回放且把音频衰减到 1/8，可能错过发声或声音过小；目前没有用户该轮采样日志，不能据此唯一确定原因。

修改仅涉及独立 `test-audio-replay.c`：增加可取消的 3 秒倒计时和 1 秒停声间隔；SSI 使用 16 位 PCM、32 位时隙，16384 双声道帧录制约 1.02 秒，仍占 65536 字节。原地提取左声道，减去均值去直流，根据峰值调整幅度（目标 3000、最多放大 8 倍、限幅 ±4000），首尾约 10ms 淡入淡出。新增采样均值/交流峰值/变化数/输入饱和计数、增益/处理后峰值/输出限幅次数和 PWM 返回错误诊断；结束直接配置 P702/P703 为低，不重新配置整张引脚表。保留一个线程入口，无 MSH 或跨例程依赖。

验证：23 项结构/主机工具检查通过，`scripts/validate_audio_replay.py` 的 6 组实际 ARM 算术执行检查通过；RT-Thread Studio 构建 0 errors、0 warnings，静态 RAM 529400 字节，与改动前相同。已通过 DAP-LINK 成功烧录，记录 `logs/audio-replay-fix-build-flash.log`。本轮按用户自行测试的选择，不打开 COM8 或自动运行音频命令；16 位 SSI/DTC 数据与实际听音待用户复验，不宣称声音问题已经在板上解决。

新操作：执行 `hmi_test audio-replay`，倒计时后 `MIC RECORD NOW` 时说“你好”，看到 recording finished 停声，1 秒后试听。预期输出 16384/16384、pwm_error=0、约 6 秒后 WAIT/IDLE；请提供完整诊断日志和回放声音结论。

## 音频输出恢复验收：提示音板端完成，试听待确认（2026-10-06）

用户确认 J8 已连接 JBL 4Ω C11R 喇叭，要求开始测试。未修改或重新烧录固件，执行现有半秒低幅提示音例程；主机命令设 8 秒上限，确认调度器空闲后发送，结束释放 COM8。

```text
msh >hmi_test status
TEST STATUS ready=1 busy=0
msh >hmi_test audio-tone
TEST BEGIN audio-tone
msh >AUDIO output samples=8000/8000; listening confirmation required
TEST RESULT audio-tone WAIT code=1 elapsed=509 ms
TEST IDLE
```

原始日志：`logs/peripherals_20261006_202150.log`。测试后线程列表只有原有系统线程，未留下测试线程；堆 used=11128、available=114720，COM8 released。板端证明定时播放完成与线程退出，不代替喇叭有声及音质确认。用户回复“重试”，尚未确认听到声音。重试时 COM8 被其他程序占用，主机返回 WinError 5（拒绝访问），未发送测试命令，也未播放声音；记录 `logs/peripherals_20261006_202314.log`。已请用户关闭占用 COM8 的终端后重试；录音回放等待提示音试听反馈后继续。

## 音频提示音：关闭占用终端后重试完成（2026-10-06）

用户关闭占用 COM8 的串口终端并回复“已关闭，可以重试”。预告后等待 3 秒，再执行一轮半秒低幅提示音；命令上限 8 秒。

```text
msh >hmi_test status
TEST STATUS ready=1 busy=0
audio-tone runs=1 last=WAIT
msh >hmi_test audio-tone
TEST BEGIN audio-tone
msh >AUDIO output samples=8000/8000; listening confirmation required
TEST RESULT audio-tone WAIT code=1 elapsed=509 ms
TEST IDLE
```

日志：`logs/peripherals_20261006_202410.log`。系统线程列表正常，used=11128、available=114720，与第一次测试相同；COM8 已释放。用户随后确认“听到了，声音正常”，结合 8000/8000 样本与正常退出，本项低幅提示音人工验收通过；录音回放继续测试。该结论不代表喇叭满功率匹配或长期驱动能力已验证。

## 音频录音回放：采集与播放完成，试听待确认（2026-10-06）

用户确认准备好录音回放，预告后等待 3 秒，运行现有独立 `audio-replay` 例程。采集 8192 个双声道帧后回放左声道，主机命令上限 8 秒。

```text
msh >hmi_test status
TEST STATUS ready=1 busy=0
audio-tone runs=2 last=WAIT
msh >hmi_test audio-replay
TEST BEGIN audio-replay
msh >MIC capture 8192 stereo frames at ~16026Hz; microphone on LEFT
MIC DMA complete left min=-587260 max=4836601 changed=7791/8191; acoustic response needs speaking test
AUDIO output samples=8192/8192; listening confirmation required
TEST RESULT audio-replay WAIT code=1 elapsed=1057 ms
TEST IDLE
```

原始日志：`logs/peripherals_20261006_202537.log`。采集 DMA 与回放样本均完成，1057 ms 后正常退出。测试后系统线程列表正常，used=11128、available=114720，与提示音测试后相同；maximum=89192，反映本次录音缓冲的峰值分配。COM8 已释放。用户选择自行运行测试，实际回放能否辨认其发声及音质仍待反馈，当前不自动记为已通过。已提供手动执行步骤，后续不自动重试或占用 COM8。

## 当前验收范围（2026-10-02 更新）

按用户最新要求，删除 RTC 长时间走时精度测试，保留音频输出/录音回放与 CAN 外部收发两类待办。RTC 短时走时、闹钟的已通过记录保留。

| 项目 | 当前状态与证据 |
| --- | --- |
| 音频输出/录音回放 | 提示音板端与人工试听通过；旧回放未听到本人声音，改进版待复验 |
| CAN外部收发 | 等待 USB-CAN 或另一 CAN 节点；内部回环已通过 |
| JPEG到LCD联合显示（已补齐） | 灰图及480×272彩色风光图均经用户视觉确认；彩色图12/12区域匹配、解码与校验16 ms、30205 ms正常退出；G2D仍仅验证内存 |

USB收尾已由后续日志补齐：128字节跨包回显一致，运行中一次物理拔插后重新枚举并回显成功。当前BLE也已补齐：首轮连接并收到断开事件，第二轮重新运行后再次连接成功。音频提示音/回放和CAN外部收发仍等待既定物料。明确执行条件和验收边界已整理到 `TODO.md`。下文历史待验收描述保留其当时语境，以最新逐项结论和本节核对为准。本次仅审阅、更新文档，未打开串口、执行板测或修改固件。

## 范围调整：删除 RTC 长时间精度测试（2026-10-02）

用户明确要求“RTC 长时间走时精度不用测试了，删除；另外两项保留”。已从当前待办、任务书和 roadmap 中移除 RTC 长期测量要求，音频输出/录音回放及 CAN 外部收发继续保留。没有新增或删除 RTC 基础测试代码；`rtc-tick` 和 `rtc-alarm` 已通过的验收记录保留。取消长期精度测量不记为已测通过。

本轮只更新文档，未修改或烧录固件、未打开串口。下文历史记录中当时计划的长期精度对照已由本次范围调整取消。

## 彩色风光 JPEG：板端与人工验收通过（2026-10-01）

用户对固件 `f65bd8b` 明确反馈“显示效果很好”，提供完整日志：

```text
msh >hmi_test graphics-jpeg
TEST BEGIN graphics-jpeg
msh >JPEG color patches=12/12 (RGB565 channel tolerance=2/4/2)
JPEG 480x272 lines=272 status=A1 pixel=3BF7
JPEG compressed=60901 bytes decode/check=16 ms
JPEG LCD: landscape 480x272, native RGB565; observe for 30 seconds
JPEG LCD: frames=1936; visual confirmation required
TEST RESULT graphics-jpeg WAIT code=1 elapsed=30205 ms
TEST IDLE
```

尺寸和完成行数正确，12 个参考区域全部匹配，结合用户对实际画面的确认，关闭彩色风光图待办。程序 WAIT 保留其人工观察语义，文档记为验收通过。

16 ms 是当前例程的解码与区域校验合计时间，包含驱动调用、等待调度和检查，不能当作独立硬件解码耗时，也不能据此宣称连续视频帧率；1936 是持续扫描期间的 GLCDC 行检测回调计数。此次验证一张 baseline 4:4:4 JPEG，不表示其他 JPEG 编码形式、大图或全部像素均完成精确比对。

本轮仅归档用户实测日志、更新验收状态及学习说明，没有修改固件、重新烧录或打开 COM8。

## 彩色风光 JPEG 与转换脚本：交付待验收（2026-10-01）

用户要求显示更复杂的彩色风光图，并在 scripts 下提供 JPG 转数组 Python 脚本。新增 `scripts/jpg_to_array.py`：EXIF 方向校正、RGB 转换、居中裁剪/缩放、baseline 4:4:4 编码、C 数组及区域颜色参考生成；支持只输出文本或更新现有 C 文件的唯一标记块，拒绝覆盖输入图片及错误标记。

当前素材为 Hannes Röst 的 Fronalpstock 山景实拍全景图，CC BY-SA 3.0，来源与改动见 `scripts/assets/README.md`。1280 像素宽来源缩略图保留供离线转换；实际嵌入的是 480×272、60901 字节 JPEG，预览为 `scripts/assets/landscape-480x272.jpg`。

`graphics-jpeg` 独立例程改为直接解码到现有 LCD RGB565 帧缓冲，使用真实行跨度并核对 272 行完成。Python 从最终 JPEG 软件解码得到 12 个 8×8 区域的 R5/G6/B5 均值；板端容差 2/4/2，全部区域匹配后原尺寸显示 30 秒。超时、停止、熄屏和外设关闭路径保留。原灰图已通过的日志保持历史记录，不等同于本次彩色图通过。

自测：23 项主机测试全部通过，包括数组字节回读/软件解码、渐进式转 baseline、EXIF 旋转、错误参数、原图防覆盖、标记更新范围、C 数组与预览一致性，以及原有 17 项结构/工具检查。RT-Thread Studio 构建为 0 errors、0 warnings，Flash 757288 字节，静态 RAM 529400 字节；构建记录 `logs/jpeg-landscape-build.log`。无需新增完整输出图像缓冲；对齐输入 SRAM 缓冲为 65000 字节（含预取余量）。 DAP-LINK 烧录成功，记录 `logs/jpeg-landscape-flash.log`。

待用户执行 `hmi_test graphics-jpeg` 并提供日志，预期 12/12 颜色区域、480×272、272 行；观察蓝天白云、绿色山坡、岩壁及房屋细节与预览一致，约 30 秒后 WAIT/IDLE。本轮不打开 COM8；硬件解码与视觉结果在收到实际日志前保持待验收。

## JPEG 到 LCD：用户验收通过（2026-10-01）

用户明确反馈“显示成功”，提供固件 `7d15f5b` 对应命令的完整日志：

```text
msh >hmi_test graphics-jpeg
TEST BEGIN graphics-jpeg
msh >JPEG 16x16 lines=16 status=A1 pixel=8410
JPEG LCD: black background, white borders; left=16x16 right=160x160 gray
JPEG LCD: decoded pixels verified; observe for 15 seconds
JPEG LCD: frames=974; visual confirmation required
TEST RESULT graphics-jpeg WAIT code=1 elapsed=15211 ms
TEST IDLE
```

尺寸、解码行数和全像素检查通过，GLCDC 行检测回调计数 974，显示观察后正常退出。结合用户视觉确认，关闭 JPEG 到 LCD 联合显示待办。程序 WAIT 保留其“等待人工观察”的语义，文档记录人工验收通过，不修改程序结果为自动 PASS。15211 ms 包含主动观察时间，不是 JPEG 解码耗时；灰色固定图不证明彩色通道顺序、其他 JPEG 格式或大图处理均正确。

本轮仅归档日志和更新验收状态，没有修改固件、重新烧录或打开 COM8。

## JPEG 到 LCD：补充显示阶段，待人工验收（2026-10-01）

`graphics-jpeg` 保留原来的 16×16 灰色 baseline JPEG 和全部 256 像素校验。解码成功并关闭 JPEG 外设后，将解码输出复制到 LCD 帧缓冲：黑底，左侧 16×16 原图，右侧 160×160 最近邻放大图，两幅图都有白边。画面在启动扫描前绘制完毕，使用生成配置的行跨度；独立配置 ST7282 时序及 P105/P100，观察 15 秒后熄屏、停止并关闭 GLCDC。支持协作停止，关闭等待有超时限制。所有功能仍在同一个 C 文件中，没有增加 MSH 内容或调用其他测试例程。

自测：17 项结构/工具测试通过，包含 45 个独立例程、唯一线程入口、无跨例程调用及 hal_entry.c 原始哈希检查；RT-Thread Studio 编译为 0 errors、0 warnings。已通过 DAP-LINK 成功烧录并复位目标，构建与烧录日志见 `logs/jpeg-lcd-build-flash.log`。本轮未打开 COM8，也未自动运行板上命令，不能以编译成功代替屏幕验收。

待用户执行 `hmi_test graphics-jpeg`，提供完整串口日志，并确认黑底上的两幅灰图、完整白边及约 15 秒后自动熄屏。程序正常结果预期为 WAIT；之前内存校验的 PASS 记录不自动转为新增显示阶段通过。固定灰图不覆盖彩色通道顺序、复杂 JPEG 或大图解码；`graphics-g2d` 仍是内存测试。

## 范围调整：取消外接 SWD 验证（2026-10-01）

用户询问是否编写过该项测试代码，并明确认为没有必要测试。核对交付内容，外接SWD没有独立C测试文件或命令，原任务书只有通过独立调试器接线、连接和操作的验证步骤。按用户决定取消该项，移出待办与本轮验收范围，不再要求准备物料。

保留板载ART-Link/DAP-LINK烧录、COM8命令收发、实体复位的已通过记录。外接SWD取消不记为PASS。本轮只改文档，没有修改代码、执行板测或占用串口。

## 实体复位键：重新启动与状态清零验收通过（2026-10-01）

用户按复位测试指引提供日志：复位前 `hmi_test status` 显示 ready=1、busy=0，已有 gpio-inputs 5次、gpio-led 4次、pmod-i2c 1次运行记录；随后重新出现 RT-Thread 5.0.1 启动横幅、Hello RT-Thread 和 i2c1 注册信息，说明系统已重新启动并执行总线初始化。

第一份日志中的 status 位于启动横幅之前，仅作为复位前状态。用户随后在启动完成后补充以下输出，只有 ready=1、busy=0，没有任何 runs 行，确认调度器已就绪、无测试占用且运行计数清零。结合先前重新启动日志，实体复位键完整验收通过，关闭复位后状态待补充项。

复位后原始输出：

```text
msh >hmi_test status
TEST STATUS ready=1 busy=0
```

此前复位前状态及重新启动的原始输出（仅去除重复空提示符）：

```text
msh >hmi_test status
TEST STATUS ready=1 busy=0
gpio-inputs runs=5 last=WAIT
gpio-led runs=4 last=WAIT
pmod-i2c runs=1 last=WAIT
 \ | /
- RT -     Thread Operating System
 / | \     5.0.1 build Oct  1 2026 22:08:59
 2006 - 2022 Copyright by RT-Thread team

Hello RT-Thread!
msh >[0] I/I2C: I2C bus [i2c1] registered
[0] D/NO_TAG: software simulation i2c1 init done, pin scl: 514, pin sda 515
```

本轮仅归档用户证据，未打开 COM8、修改代码或烧录。

## 用户逐项验收：三颗 LED 视觉确认通过（2026-10-01）

用户按 `hmi_test gpio-led` 的指引观察后明确反馈：“三个LED 依次闪烁”。据此，P209/P210/P204 三颗 LED 的依次闪烁视觉验收通过，关闭此前缺少目视确认的待办。

本次只有视觉反馈，没有新的串口日志或耗时；此前归档的 `WAIT code=1 elapsed=2174 ms` 保留为当时的运行证据，不将该耗时当作本次测量值。WAIT 为例程的人工观察结果码，不代表故障。本次仅更新文档，未修改代码、烧录或打开 COM8。

## 用户逐项验收：GPIO 三路输入电平通过（2026-10-01）

证据来源：用户本次回传的五次 `hmi_test gpio-inputs` 日志，当前固件 `8b3fb6b`。按此前指引，先全部松开，再分别按住三键，最后全部松开。程序只采样一次并返回 WAIT；结合实际操作与下表电平判定，本项人工验收通过。

| 操作 / 日志顺序 | P005 | P006 | P007 | 耗时 |
| --- | ---: | ---: | ---: | ---: |
| 初始全部松开 | 1 | 1 | 1 | 7 ms |
| 单键按住（P006） | 1 | 0 | 1 | 8 ms |
| 单键按住（P005） | 0 | 1 | 1 | 8 ms |
| 单键按住（P007） | 1 | 1 | 0 | 8 ms |
| 最后全部松开 | 1 | 1 | 1 | 7 ms |

三路输入均表现为松开高、按下低；每次单键按住时其他两路保持高电平，最后恢复全高。五次均正常返回 TEST IDLE。只记录引脚与电平关系，不据此推断实物按键左右排列；短按/长按与消抖的证据仍见此前 `gpio-keys` 记录。本轮仅归档文档，未修改代码、烧录或打开 COM8。

原始日志（仅去除重复提示符和空行）：

```text
msh >hmi_test gpio-inputs
TEST BEGIN gpio-inputs
KEY levels P005=1 P006=1 P007=1 (pressed=0)
TEST RESULT gpio-inputs WAIT code=1 elapsed=7 ms
TEST IDLE

msh >hmi_test gpio-inputs
TEST BEGIN gpio-inputs
KEY levels P005=1 P006=0 P007=1 (pressed=0)
TEST RESULT gpio-inputs WAIT code=1 elapsed=8 ms
TEST IDLE

msh >hmi_test gpio-inputs
TEST BEGIN gpio-inputs
KEY levels P005=0 P006=1 P007=1 (pressed=0)
TEST RESULT gpio-inputs WAIT code=1 elapsed=8 ms
TEST IDLE

msh >hmi_test gpio-inputs
TEST BEGIN gpio-inputs
KEY levels P005=1 P006=1 P007=0 (pressed=0)
TEST RESULT gpio-inputs WAIT code=1 elapsed=8 ms
TEST IDLE

msh >hmi_test gpio-inputs
TEST BEGIN gpio-inputs
KEY levels P005=1 P006=1 P007=1 (pressed=0)
TEST RESULT gpio-inputs WAIT code=1 elapsed=7 ms
TEST IDLE
```

## SSD1306 外接 I²C 基础显示验收与动画优化（2026-10-01）

### 动画优化版人工验收通过

对应固件 `8b3fb6b`，用户明确反馈“完美验收通过，全部正常”。基础图案、移动方块、透视旋转立方体和广告字幕均通过视觉验收，关闭此前动画残影与新场景待复验项。证据为用户本次提供的串口日志及视觉结论；本轮仅整理文档，未打开 COM8、未修改或重新烧录固件。

| 场景 | 帧数 | 实测帧率 | 平均数据字节/帧 | 平均传输耗时 |
| --- | ---: | ---: | ---: | ---: |
| 移动方块 | 117 | 29.2 fps | 28 | 7 ms |
| 透视立方体 | 481 | 30.0 fps | 111 | 24 ms |
| 循环广告字幕 | 542 | 15.0 fps | 376 | 64 ms |

滚动字幕的传输量较大，平均传输 64 ms，实际帧率为 15.0 fps，未达到约 30 fps 的目标；用户已确认该效果正常，按实测值记录验收，不将目标帧率当作结果。总计 1142 帧（含两帧静态图案），数据 263129 字节，总耗时 60577 ms，返回 WAIT 后正常到达 TEST IDLE。WAIT 是此例程预设的人工观察结果码，结合用户视觉确认后记录为验收通过。

原始日志如下，仅移除行内 `msh >` 提示符。三条统计行末尾在用户回传内容中被截断，所有 `tx_ms_max` 数值均未知，不补写或推测；平均耗时字段完整可读。

```text
msh >hmi_test pmod-i2c
TEST BEGIN pmod-i2c
OLED ACK addr=0x3C; using SSD1306 128x64 profile
OLED border: all four edges should be visible (2 seconds)
OLED checkerboard: 8x8 squares across whole screen (2 seconds)
OLED scene=block duration=4000 ms target_period=33 ms
OLED scene=block frames=117 fps=29.2 data_bytes/frame=28 tx_ms_avg=7 tx_m
OLED scene=cube duration=16000 ms target_period=33 ms
OLED scene=cube frames=481 fps=30.0 data_bytes/frame=111 tx_ms_avg=24 tx_
OLED scene=marquee duration=36000 ms target_period=33 ms
OLED scene=marquee frames=542 fps=15.0 data_bytes/frame=376 tx_ms_avg=64
OLED frames=1142 data_bytes=263129; visual confirmation required
TEST RESULT pmod-i2c WAIT code=1 elapsed=60577 ms
TEST IDLE
```

以下为优化前反馈及开发过程记录，待复验描述仅代表当时状态，以本节最终验收结论为准。

用户反馈：固件 `e3b2152` 的显示“全部正常”，但移动动画刷新较慢，肉眼有明显残影；本次没有补充串口日志。记录为基础图案功能通过、动画流畅度需要改进，不将残影归因于已测量的面板响应时间。

本轮优化只修改 `test-pmod-i2c.c`：

- 从固定整屏传输改为按页比较新旧缓冲区，只写变化区间；未变化页零传输，旧像素也会被擦除。只在写入成功后更新副本，失败后强制下次全量刷新。
- 移除每帧传输完成后固定 80 ms 等待，改为含绘图/传输在内的 33 ms 目标帧间隔。旧实现理论上限小于 12.5 fps，新实际帧率需读取日志，未声称已经达到 30 fps。
- 保留边框、棋盘格与移动方块对照，增加两轴旋转、近大远小的立方体，远侧边为虚线；它是平面投影的纵深效果。增加 2 倍字号循环广告与反向追逐的跑马灯边框，消息跨尾部直接衔接开头。
- 各动画输出实际帧率、平均数据字节数与平均/最长传输耗时；约 60 秒自然结束，字幕在 36 秒演示窗口持续循环，停止时也关闭显示。
- 未修改共用 I²C 驱动时序、GT911、其他测试或 `hal_entry.c`。SSD1306 无整帧交换同步，缩短传输可减少撕裂窗口，但残影是否改善仍需用户验收。

本轮自测：

- Studio 构建 0 errors、0 warnings，见本机 `logs/ssd1306-animation-build.log`；17 项既有工具/结构回归通过，`git diff --check` 通过。验证完成后已通过 DAP-LINK/PyOCD 烧录并复位，见 `logs/ssd1306-animation-flash.log`。
- `scripts/validate_oled_animation.py` 将实际例程和模拟 RT/I²C 一起编译为 ARM 程序，在 Unicorn 中执行；7 组逻辑验证通过：初始全刷/静止零传输、移动擦除与小范围更新、写失败后全刷恢复、取消不发送、旋转画面变化与传输一致、字幕像素位移与首尾循环、包边界和三种动画调度。结果见 `logs/ssd1306-animation-validation.log`。
- 从执行结果提取两幅立方体和一幅广告帧，已检查标题、线框及文字像素布局。图像是代码生成的缓冲区预览，不是板上拍摄；见本机 `logs/oled-validation/preview.png`。
- 模拟器只验证绘图/传输逻辑，不验证真实 I²C 时钟、面板刷新率、实测帧率或残影。本次未打开 COM8；接线不变，待用户执行同一命令提供三段统计和视觉结论。

重跑逻辑验证（只使用本机日志目录安装测试依赖，不访问串口）：

```text
python -m pip install --target logs/oled-validation/python unicorn==2.1.4 pyelftools==0.33
python scripts/validate_oled_animation.py --dependencies logs/oled-validation/python
```

脚本默认查找 Studio 10.2.1 ARM GCC，可用 `--compiler` 指定另一已安装路径；仿真使用软件浮点，板上构建仍使用工程自身的编译设置。

以下保留基础版本交付记录；其中 50 帧、80 ms 和 `frames=52` 为旧版行为，新版以当前使用文档为准。

用户没有原 0x50 夹具，提供 I²C SSD1306 128×64 OLED。`src/test/test-pmod-i2c.c` 已替换为独立显示例程，命令继续使用 `hmi_test pmod-i2c`；仍仅有一个线程入口，不包含命令行处理，不调用其他例程，无 LVGL 依赖。

- 接线：四针模块 VCC→3.3V、GND→GND、SCL→P202、SDA→P203。使用模块丝印辨认，不依赖针脚排列。
- 只向 0x3C、0x3D 发送 NOP，选择首个应答地址，然后使用 SSD1306 128×64、电荷泵、页寻址配置。应答不代表自动识别芯片型号。
- 显示四边框 2 秒、8×8 棋盘格 2 秒、50 帧左右移动方块。每包检查 I²C 写入结果，每页检查停止请求。成功路径输出 `frames=52`，关闭显示后返回 WAIT，等人工确认图像。
- 无地址应答返回 SKIP；初始化或刷新写入失败返回 FAIL。已找到模块后，失败和停止均尝试发送关闭显示命令；保留共享 i2c1 总线，不改变 GT911 复位/IRQ 配置。
- 此版本替代旧 0x50 单字节读测试，历史无应答日志仍保留，不能用来判定新 OLED 例程是否通过。

自测：17 项主机结构与工具回归通过（包含 `hal_entry.c` 原始哈希、45 个独立文件和唯一入口约束）；`git diff --check` 通过；Studio 编译 0 errors、0 warnings；DAP-LINK/PyOCD 烧录成功并复位开发板。构建/烧录过程保存在本机 `logs/ssd1306-i2c-build-flash.log`。本次助手未打开 COM8，实际 OLED 接线、地址应答、显示效果与异常分支尚未板测，待用户回传日志和图像观察结论。

## GPIO IRQ12 改线交付与验收通过（2026-10-01）

按用户要求，移除 `test-pmod-irq0.c` / `test-pmod-irq1.c` 及对应命令，改为两个独立例程：

| 新命令 / 文件名（前缀 test-） | 接线和触发 | 预期断言（非实测日志） |
| --- | --- | --- |
| `gpio-irq-rising` | P009 → P008/IRQ12，上升沿 | `edges=8 expected=8 levels=16/16 edge_checks=16/16` |
| `gpio-irq-both` | 同一接线，双边沿 | `edges=16 expected=16 levels=16/16 edge_checks=16/16` |

选择理由：P708/IRQ11 和 P709/IRQ10 分别与按键 P006/P005 共享硬件中断通道，不能单靠改配置数字重新分配。P008 固定对应 IRQ12，P009 只驱动 GPIO 电平。因此新例程只保存、配置、恢复 P008/P009，不再临时取消按键 ISEL；若 P502 已选择 IRQ12 或 IRQ12 的 NVIC 已使能，则报忙退出。中断启用前检查跳线，逐次比对边沿累计数；上升沿测试也核对下降沿未产生额外中断。

工程改动包括两份例程、调度器注册、IRQ12 向量与事件映射（NVIC 43）、文档。`src/hal_entry.c` 保持原始字节，其他测试源码与按键配置未修改。仍为 45 个独立文件；两个新例程各有一个线程入口，均不包含命令行处理。

自测：17 项主机工具/结构回归通过；Studio 构建 0 errors、0 warnings；DAP-LINK/PyOCD 烧录成功并复位。链接映射包含两个新入口，未包含旧 Pmod IRQ 入口。构建/烧录过程见本机 `logs/gpio-irq12-build-flash.log`。交付时新接线尚未进行板端功能验收；随后用户复验通过，实测日志见下方。本次助手没有打开 COM8。下文旧 Pmod IRQ0/IRQ1 的 PASS 为历史验收，保留其原始日志，不套用到新 GPIO IRQ12 例程。

用户在固件 `4344998` 上回传两项日志，验收通过。证据来自本对话，仅去除重复提示符与空行：

```text
msh >hmi_test gpio-irq-rising
TEST BEGIN gpio-irq-rising
GPIO IRQ rising output pin=0009 -> IRQ pin=0008, channel=12
GPIO IRQ rising wire set=0 output=0 input=0
GPIO IRQ rising wire set=1 output=1 input=1
GPIO IRQ rising wire set=0 output=0 input=0
GPIO IRQ rising wire set=1 output=1 input=1
GPIO IRQ rising wire set=0 output=0 input=0
GPIO IRQ rising wire PASS
GPIO IRQ rising IRQCR=31 NVIC=1 ISEL=1; RISING
GPIO IRQ rising GPIO->IRQ edges=8 expected=8 levels=16/16 edge_checks=16/16
TEST RESULT gpio-irq-rising PASS code=0 elapsed=213 ms
TEST IDLE

msh >hmi_test gpio-irq-both
TEST BEGIN gpio-irq-both
GPIO IRQ both output pin=0009 -> IRQ pin=0008, channel=12
GPIO IRQ both wire set=0 output=0 input=0
GPIO IRQ both wire set=1 output=1 input=1
GPIO IRQ both wire set=0 output=0 input=0
GPIO IRQ both wire set=1 output=1 input=1
GPIO IRQ both wire set=0 output=0 input=0
GPIO IRQ both wire PASS
GPIO IRQ both IRQCR=32 NVIC=1 ISEL=1; BOTH_EDGE
GPIO IRQ both GPIO->IRQ edges=16 expected=16 levels=16/16 edge_checks=16/16
TEST RESULT gpio-irq-both PASS code=0 elapsed=212 ms
TEST IDLE
```

结论：P009 输出到 P008/IRQ12 的接线检查和 16 次电平读回全部匹配；上升沿模式累计 8 次中断，逐步计数同时验证下降沿未触发；双边沿模式累计 16 次中断。两项 `edge_checks=16/16`，分别耗时 213 ms、212 ms，均正常返回 `TEST IDLE`。关闭新接线待复验项。本轮只整理文档，不修改或重新烧录固件，也不打开串口；不将这次成功路径延伸为取消、断线或占用分支已实测。

## 用户逐项验收：LED 与按键（2026-10-01）

证据来源：用户在本对话提供的串口日志；对应重构提交 `5b141e3`。本次仅整理记录，未重新打开 COM8 或运行板端测试。

| 项目 | 实际结果 | 验收范围 |
| --- | --- | --- |
| `gpio-led` | `WAIT code=1`，2174 ms，正常返回 `TEST IDLE` | P209/P210/P204 输出序列执行完成；当时尚缺视觉确认，后续用户明确确认三颗 LED 依次闪烁，现已验收通过 |
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

修复版 RT-Thread Studio 构建 0 错误、0 警告，DAP-LINK 烧录完成；17 项主机回归通过，构建与烧录日志为 `logs/touch-irq-fix-build.log`。修复后的真实触摸中断已由下方用户复验确认；助手未打开 COM8。

### 触摸中断修复复验通过

用户在固件 `5193620` 上重新运行 `hmi_test touch-irq`，记录到 78 次下降沿中断，返回 PASS，15139ms 后正常进入 TEST IDLE。关键日志节选如下（省略重复帧与中间轨迹，各段不表示连续采样）：

```text
msh >hmi_test touch-irq
TEST BEGIN touch-irq
TOUCH addr=14 id=911 range=480x272
TOUCH P004/IRQ9 falling-edge enabled; touch within 15 seconds
TOUCH N=1 id=0 (134,145)
TOUCH N=1 id=0 (222,118)
TOUCH N=1 id=0 (211,135)
TOUCH N=0
TOUCH N=2 id=0 (316,104) id=1 (195,118)
TOUCH N=0
TOUCH N=1 id=0 (122,155)
TOUCH N=1 id=0 (288,140)
TOUCH N=0
TOUCH observed_points=50; position/multitouch accuracy requires interaction
TOUCH falling-edge IRQ count=78; touch the panel to generate edges
TEST RESULT touch-irq PASS code=0 elapsed=15139 ms
TEST IDLE
```

结论：本例 P004/IRQ9 中断链路复验通过，先前 IRQ=0 的问题已闭环；同时观察到单指滑动、双指和松手上报。`observed_points=50` 是读到的各帧触点数量累计，IRQ=78 是下降沿次数，两者统计对象不同，不要求相等。本项仍使用线程轮询读取作为对照，不据此宣称逐帧中断唤醒或触摸位置精度已验收；下一项通过无 LVGL 画板检查实际位置和绘图体验。

## 用户逐项验收：无 LVGL 画板（2026-10-01）

当前固件为 `5193620`。用户执行 `hmi_test lcd-touch`，确认四项操作“正常”：选色绘图及位置跟手、抬手换位置不误连线、双指绘图后松手回到 N=0、CLEAR 清屏。本项人工验收通过。

串口原始输出如下（仅去除重复提示符）：

```text
msh >hmi_test lcd-touch
TEST BEGIN lcd-touch
LCD-TOUCH: GT911 at 0x14, range=480x272
LCD-TOUCH: ready, direct RGB565; select color, draw, CLEAR
LCD-TOUCH: DOWN id=0 x=92 y=144
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=205 y=55
LCD-TOUCH: color=2
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=220 y=129
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=291 y=41
LCD-TOUCH: color=3
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=317 y=115
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=446 y=41
LCD-TOUCH: clear=1
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=359 y=52
LCD-TOUCH: color=4
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=297 y=170
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=118 y=53
LCD-TOUCH: color=1
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=164 y=118
LCD-TOUCH: UP id=0
LCD-TOUCH: DOWN id=0 x=444 y=56
LCD-TOUCH: clear=2
LCD-TOUCH: UP id=0
TEST RESULT lcd-touch WAIT code=1 elapsed=60019 ms
TEST IDLE
```

日志确认界面初始化、选色、两次清屏和 60 秒后退出正常；本段只出现 id=0，双指体验及无误连线的验收依据为用户明确反馈，不将其表述为串口直接证明。WAIT 符合交互例程的结果语义，人工验收结论单独记录。本次仅归档用户日志，未打开 COM8。

## 用户逐项验收：LVGL 显示（2026-10-01）

当前固件为 `5193620`。用户执行 `hmi_test lcd-lvgl` 后确认“正常”，对应五色色块及文字正确、进度条持续往返、运行计数递增且无明显花屏。本项人工验收通过。

```text
msh >hmi_test lcd-lvgl
TEST BEGIN lcd-lvgl
LVGL: ready 480x272 RGB565, draw buffer=19200 bytes
LVGL: updates=50 flushes=165
LVGL: updates=100 flushes=320
LVGL: updates=150 flushes=475
LVGL: updates=200 flushes=630
LVGL: updates=250 flushes=785
LVGL: updates=300 flushes=940
LVGL: updates=350 flushes=1095
LVGL: updates=400 flushes=1250
LVGL: updates=450 flushes=1405
LVGL: updates=500 flushes=1560
LVGL: updates=550 flushes=1715
TEST RESULT lcd-lvgl WAIT code=1 elapsed=60022 ms
TEST IDLE
```

日志确认 RGB565 显示初始化、19200 字节绘图缓冲、更新与刷新计数持续增长，并在约 60 秒后正常退出。最后一条周期日志为 updates=550/flushes=1715，不将其当作退出时最终计数；flushes 是刷新回调次数，不等同于完整帧数或 FPS。WAIT 的人工观察要求已由用户确认补齐。本例不接入触摸，触摸控件由下一项 `lcd-touch-lvgl` 单独验收。本次助手未打开 COM8。

## 用户逐项验收：LVGL 触摸控件（2026-10-01）

当前固件为 `5193620`。用户先提供 `hmi_test lcd-touch-lvgl` 日志，随后补充确认操作正常。关键日志节选如下，省略重复 DOWN/UP 和 RESET 输出：

```text
msh >hmi_test lcd-touch-lvgl
TEST BEGIN lcd-touch-lvgl
TOUCH-LVGL: GT911 at 0x14, range=480x272
TOUCH-LVGL: ready, color buttons / switch / slider / RESET
TOUCH-LVGL: color=GREEN clicks=1
TOUCH-LVGL: color=RED clicks=2
TOUCH-LVGL: color=GREEN clicks=3
TOUCH-LVGL: level=42
TOUCH-LVGL: color=BLUE clicks=4
TOUCH-LVGL: RESET blue enabled level=50 clicks=0
TOUCH-LVGL: level=29
TOUCH-LVGL: RESET blue enabled level=50 clicks=0
TEST RESULT lcd-touch-lvgl WAIT code=1 elapsed=60015 ms
TEST IDLE
```

已确认初始化成功、三种颜色按钮事件、滑块松手值 42/29、RESET 默认状态日志，以及约 60 秒后正常退出。原始日志多次出现 RESET，每次前面均有 DOWN/UP；用户未说明是否逐次主动点击，因此不据此判断误触或重复触发。

补充验收：助手明确询问颜色与 RESET 画面、多次 RESET 是否主动点击、开关 OFF/恢复、滑块 0/100 端点及跟手和松手稳定性、双指主触点先抬起时 WAIT 和全部松开后 UP/N=0，用户随后回复“正常！”。据此，本项五项交互人工验收通过，多次 RESET 未被用户报告为误触。开关、滑块端点和双指保护的结论依据为用户反馈，不表述为本段串口日志直接证明。设备 WAIT 的人工观察要求已补齐；助手本次仅归档日志，未打开 COM8、未修改固件。

## 用户逐项验收：麦克风采样（2026-10-01）

当前固件为 `5193620`。用户连续执行三次 `hmi_test audio-mic`，每次均完成 8192 个双声道帧采集，返回 PASS，耗时 539ms，并正常进入 TEST IDLE。用户随后确认“第一次是安静，第二、三次是发声，第三次最大”。采样链路与本轮安静/发声幅度对照通过。

| 次序/用户确认条件 | 左声道最小值 | 左声道最大值 | 峰峰值（max−min） | 相邻样本变化数 |
| --- | --- | --- | --- | --- |
| 1：安静 | -563990 | 4732264 | 5296254 | 7819/8191 |
| 2：发声 | -856752 | 8265912 | 9122664 | 8179/8191 |
| 3：发声，声音最大 | -845440 | 8370830 | 9216270 | 8181/8191 |

三次关键输出如下，省略相同的命令、启动提示和退出提示：

```text
MIC DMA complete left min=-563990 max=4732264 changed=7819/8191; acoustic response needs speaking test
TEST RESULT audio-mic PASS code=0 elapsed=539 ms
MIC DMA complete left min=-856752 max=8265912 changed=8179/8191; acoustic response needs speaking test
TEST RESULT audio-mic PASS code=0 elapsed=539 ms
MIC DMA complete left min=-845440 max=8370830 changed=8181/8191; acoustic response needs speaking test
TEST RESULT audio-mic PASS code=0 elapsed=539 ms
```

结合用户确认，后两次发声时峰峰值较安静时增大，第三次峰峰值最大，说明本轮观察到采样幅度随发声条件变化。24 位有符号样本正上限为 8388607，后两次正峰值接近该上限；仅凭极值不能认定削顶，也不能排除启动瞬态或数据格式问题。当前程序 PASS 只检查采集完成和数据存在变化，人工对照补充了基本声学响应证据，不证明音质、噪声、线性度或幅度准确性；后续通过录音试听继续验收。本次仅整理用户日志与补充确认，未打开 COM8。

## 用户逐项验收：RTC 走时与闹钟（2026-10-01）

当前固件为 `5193620`。用户提供两项完整日志：

```text
msh >hmi_test rtc-tick
TEST BEGIN rtc-tick
RTC source=32.768k crystal date=2026-10-01 12:00:03 alarm_count=0
TEST RESULT rtc-tick PASS code=0 elapsed=5513 ms
TEST IDLE

msh >hmi_test rtc-alarm
TEST BEGIN rtc-alarm
RTC source=32.768k crystal date=2026-10-01 12:00:03 alarm_count=1
TEST RESULT rtc-alarm PASS code=0 elapsed=5513 ms
TEST IDLE
```

两项基础功能验收通过：RTC 从预设的 12:00:00 走到 12:00:03，走时例程没有闹钟事件，闹钟例程恰好一次，两项均正常退出。本轮不覆盖长期计时精度或断电保持。

用户反馈“rtc-tick 超过 3 秒，等了 6 秒”。经源码核对，两个例程均先执行 `RTC_OSCILLATOR_SETTLE_MS=2200` 的固定晶振稳定等待，设置日期后再执行 `RTC_OBSERVATION_MS=3200` 的走时观察，另有驱动初始化及清理开销。总耗时 5513ms 符合当前实现，日期只前进约 3 秒也正确，不是 RTC 走慢。之前指导中的“约 3 秒”漏计初始化时间，已将使用手册修正为整条命令约 5.5～6 秒。本次仅更新说明和验收记录，未修改或烧录固件、未打开 COM8。

## 用户逐项验收：G2D 内存绘图（2026-10-01）

当前固件为 `5193620`，用户提供日志：

```text
msh >hmi_test graphics-g2d
TEST BEGIN graphics-g2d
G2D hardware red clear + green rectangle: corners=F800 center=07E0
TEST RESULT graphics-g2d PASS code=0 elapsed=11 ms
TEST IDLE
```

本项验收通过：D/AVE 2D 在独立的 16×16 RGB565 内存画布上填充红色背景和中央 8×8 绿色矩形，程序逐像素比较，日志摘要的 F800/07E0 分别对应红色/绿色，11ms 后正常退出。

用户反馈“没有显示图像”。这符合例程设计：测试没有启动 GLCDC，也没有把小画布送入 LCD 帧缓冲；屏幕无图像不是本项故障。本项证明该绘图操作及像素结果，不代表 G2D 到 LCD 的联合显示链路已验收。下一项独立测试 JPEG 解码，同样在内存中校验。本次仅归档用户日志，未修改固件、未打开 COM8。

## 用户逐项验收：JPEG 硬件解码（2026-10-01）

当前固件为 `5193620`，用户提供日志：

```text
msh >hmi_test graphics-jpeg
TEST BEGIN graphics-jpeg
JPEG 16x16 lines=16 status=A1 pixel=8410
TEST RESULT graphics-jpeg PASS code=0 elapsed=9 ms
TEST IDLE
```

本项验收通过：内置 16×16 灰色 baseline JPEG 完成硬件解码，解码行数为 16，首像素 RGB565 为 0x8410，全部 256 个像素均通过允许量化误差的通道校验；9ms 后正常退出。status=A1 为例程累计的回调状态位，程序已检查完成位及错误位。验证在内存中完成，不依赖 LCD，也不表示其他 JPEG 格式或大图解码均已覆盖。本次仅归档用户日志，未修改固件、未打开 COM8。

## 用户逐项验收：CAN 内部回环（2026-10-01）

当前固件为 `5193620`，用户提供日志：

```text
msh >hmi_test can-loop
TEST BEGIN can-loop
CAN 500k loop: TX ID=321, expected RX ID=321 same 8 bytes
CAN tx=1 rx=1 errors=0
TEST RESULT can-loop PASS code=0 elapsed=18 ms
TEST IDLE
```

本项验收通过：500 kbit/s、标准数据帧 ID 0x321、每帧 8 字节，源码循环验证 8 组不同载荷，核对发送/接收事件、ID、帧类型、长度和内容，未记录错误事件，18ms 后正常退出。`tx=1 rx=1` 是最后一组的事件计数；程序每组开始前清零，因此不代表只测试了一组，也不是 8 组的累计收发数。

内部回环不经过外部 XL2551 与接线，外部总线仍需 USB-CAN 或另一个正常工作的 CAN 节点配合 `can-bus` 验证。用户随后确认没有对端设备，外部收发暂缓并加入 `TODO.md`；已说明短接 CANH/CANL 不能代替回环。本次仅归档用户日志与测试条件，未修改固件、未打开 COM8。

## 用户逐项验收：TF 卡检测问题（2026-10-01）

用户确认有 TF 卡后，运行当前固件 `5193620` 的信息和只读测试，两项均在检测阶段跳过：

```text
msh >hmi_test sd-info
TEST BEGIN sd-info
SD card_inserted=0
TEST RESULT sd-info SKIP code=2 elapsed=5 ms
TEST IDLE

msh >hmi_test sd-read
TEST BEGIN sd-read
SD card_inserted=0
TEST RESULT sd-read SKIP code=2 elapsed=4 ms
TEST IDLE
```

两项均未进入介质初始化和数据读取，不能根据这些结果判断卡容量或文件系统格式。核对 V3.1 原理图 SD Card/PinMap 页及 RA6M3 引脚表后，发现卡座 CD 连接 P405 普通 GPIO，而生成配置 `g_sdmmc1_cfg.card_detect` 选择了 `SDMMC_CARD_DETECT_CD`；旧例程直接读取 SDHI 专用检测输入，与实物连接不匹配。旧日志的 SKIP 不能作为实物确实无卡的证据。

修复三个独立文件 `test-sd-info.c`、`test-sd-read.c`、`test-sd-file.c`：各自配置 P500～P505 的 SDHI1 总线功能、P405 上拉输入，读取低有效卡检测信号；禁用未接线的 SDHI 专用 CD 检测，仍以实际 GPIO 判定是否插卡。读/写传输等待、FatFs 状态和同步时继续检查 GPIO；退出恢复引脚。增加打开控制器和介质初始化的错误码，便于区分后续失败阶段。例程之间仍无调用依赖，不添加公共测试头，不格式化卡。

修复版构建 0 错误、0 警告，17 项主机回归通过，DAP-LINK 烧录完成，记录见 `logs/sd-card-detect-fix-build.log`。修复后的插卡检测、介质初始化和扇区读取待用户复验；文件写入阶段尚未验收。本次助手未打开 COM8、未执行卡写入。

### TF 卡检测与信息修复复验通过

用户在修复固件 `e1bb9a3` 上执行 `hmi_test sd-info`，日志如下：

```text
msh >hmi_test sd-info
TEST BEGIN sd-info
SD P405 detect=1 (1=inserted, 0=absent)
SD sectors=15613952 bytes/sector=512 clock=30000000 protected=0
TEST RESULT sd-info PASS code=0 elapsed=41 ms
TEST IDLE
```

本项验收通过：P405 实际检测到插卡，SDHI 介质初始化成功，容量为 15613952×512=7994343424 字节（约 7.99 GB / 7.45 GiB），扇区大小为 512 字节，报告时钟 30MHz、写保护标志为 0，41ms 后正常退出。旧例程误读 SDHI 专用 CD 导致的无卡判断问题，在本次插卡路径上已复验解决；修复版无卡路径尚待拔卡复测。此项不验证扇区内容或文件系统，下一步继续 `sd-read`。本次仅归档用户日志，未打开 COM8、未执行卡写入。

### TF 卡只读扇区复验通过

用户在固件 `e1bb9a3` 上继续执行 `hmi_test sd-read`：

```text
msh >hmi_test sd-read
TEST BEGIN sd-read
SD P405 detect=1 (1=inserted, 0=absent)
SD sectors=15613952 bytes/sector=512 clock=30000000 protected=0
SD sector0 repeat-read matched, signature=55AA
TEST RESULT sd-read PASS code=0 elapsed=49 ms
TEST IDLE
```

本项验收通过：独立重新初始化同一容量的卡，两次读取扇区 0，全部 512 字节比较一致；末尾签名字节为 55 AA，49ms 后正常退出。本阶段没有写入卡；55AA 不能单独证明 FAT32 格式或文件系统完整性，文件挂载与写入读回由下一项 `sd-file` 验证。本次助手仅归档用户日志，未打开 COM8。

### TF 卡文件读写与随后只读复测通过

用户在固件 `e1bb9a3` 上完成文件测试后，再次执行只读测试：

```text
msh >hmi_test sd-file
TEST BEGIN sd-file
SD P405 detect=1 (1=inserted, 0=absent)
SD sectors=15613952 bytes/sector=512 clock=30000000 protected=0
SD created 0:/HMI00.TST (retained for inspection)
SD file remount/readback 8192 bytes MATCH
TEST RESULT sd-file PASS code=0 elapsed=247 ms
TEST IDLE

msh >hmi_test sd-read
TEST BEGIN sd-read
SD P405 detect=1 (1=inserted, 0=absent)
SD sectors=15613952 bytes/sector=512 clock=30000000 protected=0
SD sector0 repeat-read matched, signature=55AA
TEST RESULT sd-read PASS code=0 elapsed=49 ms
TEST IDLE
```

文件读写验收通过：新建 HMI00.TST，写入 8192 字节，完成同步、关闭、卸载/重新挂载后，文件长度及全部数据比较一致，247ms 后正常退出。测试文件保留在卡上；没有格式化或覆盖旧文件。随后的独立只读例程再次初始化并验证扇区 0 两次读取一致，49ms 后正常退出，说明这次文件测试退出后仍能正常访问卡。

TF 卡信息、只读和文件读写三个阶段均已通过；仍需在 TEST IDLE 时拔卡验证 detect=0/SKIP，再插回重新执行以验证恢复。尚未据此确认掉电持久性、写入中拔卡恢复或长期稳定性。本次仅归档用户日志，助手未打开 COM8。

### TF 卡空闲拔插与恢复验收通过

用户按指导在 TEST IDLE 时拔卡，执行信息查询，再插回卡执行文件读写，日志如下（固件 `e1bb9a3`）：

```text
msh >hmi_test sd-info
TEST BEGIN sd-info
SD P405 detect=0 (1=inserted, 0=absent)
TEST RESULT sd-info SKIP code=2 elapsed=26 ms
TEST IDLE

msh >hmi_test sd-file
TEST BEGIN sd-file
SD P405 detect=1 (1=inserted, 0=absent)
SD sectors=15613952 bytes/sector=512 clock=30000000 protected=0
SD created 0:/HMI01.TST (retained for inspection)
SD file remount/readback 8192 bytes MATCH
TEST RESULT sd-file PASS code=0 elapsed=269 ms
TEST IDLE
```

空闲拔插恢复验收通过：无卡时 GPIO 检测为 0，26ms 后正常 SKIP；插回后检测为 1，重新初始化成功，使用下一个空闲文件名 HMI01.TST，8192 字节重新挂载读回一致，269ms 后正常退出。程序以 CREATE_NEW 保留原 HMI00.TST，此次未重新校验旧文件内容。

TF 卡本轮基础验收完成，覆盖卡信息、扇区重复读取、文件写入/重新挂载读回、无卡判断与空闲插回恢复。卡上保留 HMI00.TST、HMI01.TST 两个测试文件。未测试写入中拔卡、掉电恢复、全卡扫描或长期寿命。本次仅归档用户日志，助手未打开 COM8。

## 用户逐项验收：系统 USB 枚举与 CDC 回显（2026-10-01）

当前固件为 `e1bb9a3`。用户通过 Type-C 连接安卓手机后执行 `usb-probe`：

```text
msh >hmi_test usb-probe
TEST BEGIN usb-probe
USB system connector VBUS=1; debug USB/COM8 is separate
USB mounted_events=0 rx=0 tx_queued=0 tx_complete_events=0
TEST RESULT usb-probe SKIP code=2 elapsed=5013 ms
TEST IDLE
```

此项未通过：VBUS 输入读为高电平，但 5 秒内没有完成主机 SET_CONFIGURATION，不能由供电推断 USB 数据链路正常。用户仅补充连接端为安卓手机，具体型号、OTG 开关状态、USB 主机角色及手机提示仍未确认。CDC 数据回显尚未进入验收。

只读检查确认：V3.1 系统 Type-C 的 CC1/CC2 各有 5.1kΩ 下拉，D+/D− 接至 MCU；工程配置 USB 时钟为 PLL 240MHz/5=48MHz，USBFS 中断使用向量 40，应用配置 P407 为 VBUS 功能，TinyUSB 配置 CDC 设备。尚未发现可直接归因的配置错误；这些静态检查不能代替实际枚举，也未证明手机、线材或固件任一方一定正常。下一步确认手机 OTG/主机模式及数据线，必要时改用电脑作对照。本次仅归档与检查，未修改固件、未打开 COM8。


### 电脑 A-to-C 对照与诊断版本（2026-10-01）

用户补充：安卓手机始终解锁，显示充电标志；设置搜索显示“OTG未安装”，无其他 USB 提示。随后改用 A-to-C 数据线连接电脑，仍得到：

```text
msh >hmi_test usb-probe
TEST BEGIN usb-probe
msh >USB system connector VBUS=1; debug USB/COM8 is separate
USB mounted_events=0 rx=0 tx_queued=0 tx_complete_events=0
TEST RESULT usb-probe SKIP code=2 elapsed=5013 ms
TEST IDLE
```

结论：手机和电脑均未完成枚举，不能只归因于手机 OTG。充电标志不能证明数据通路正常，SKIP 不作为验收通过。原理图 PDF 第 3 页进一步确认 P407 经 R68/R69 分压检测 VCC_USB_SYS，USB_D_P/N 接 MCU 专用 USB_DP/DM 引脚；第 8 页确认系统连接器 Con4 的 CC 下拉和数据线。时钟配置的静态检查尚不能替代运行时检查。

本次修改：

1. `board/usb-runtime.c` 按 FSP v3.5.0 官方 `usbfs_interrupt_handler` 顺序，先清 ICU 中断请求再进入 TinyUSB 处理，避免处理末尾清掉期间新产生的请求。此处为中断顺序修正，尚未证明是本次失败的根因。
2. `usb-probe`、`usb-echo` 分别在各自 C 文件内维护 IRQ、总线复位、SETUP 和描述符请求计数；适配层仅转发回调，中断内不打印日志。
3. 枚举窗口从 5 秒调整为 15 秒；开始及枚举结束时打印 VBUS GPIO、VBSTS、DPRPU、时钟分频和 USB 状态寄存器。未完成枚举继续返回 SKIP，不伪报 PASS。

验证：最终 RT-Thread Studio 构建 0 错误、0 警告；DAP-LINK 烧录成功并复位，记录见 `logs/usb-enumeration-diagnostics-build.log`。17 项主机工具/架构检查通过，`git diff --check` 通过；这些检查不等于 USB 实机枚举通过。

官方中断顺序参考：https://github.com/renesas/fsp/blob/v3.5.0/ra/fsp/src/r_usb_basic/src/hw/r_usb_mcu.c 。下一步在电脑连接条件下重跑 `hmi_test usb-probe`，保留新增诊断行。USB 枚举与 ECHO 的板上验收仍未完成；本轮未打开 COM8。

### 电脑 USB 枚举复验通过（2026-10-01）

用户保持电脑 A-to-C 连接，在诊断固件 `1b36971` 上复测：

```text
msh >hmi_test usb-probe
TEST BEGIN usb-probe
msh >USB system connector VBUS=1; debug USB/COM8 is separate
USB start: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB start: SYSSTS0=0001 INTSTS0=00C0 INTENB0=FD00 USBADDR=0000 NVIC=1
USB start: irq=1 reset=0 setup=0 desc_device=0 desc_config=0
USB waiting up to 15 seconds for host enumeration
USB enumeration: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB enumeration: SYSSTS0=0001 INTSTS0=20B0 INTENB0=DD00 USBADDR=0002 NVIC=1
USB enumeration: irq=47 reset=2 setup=13 desc_device=3 desc_config=4
USB mounted_events=1 rx=0 tx_queued=0 tx_complete_events=0
TEST RESULT usb-probe PASS code=0 elapsed=383 ms
TEST IDLE
```

结论：电脑端基本枚举验收通过。USB 中断进入 47 次、总线复位 2 次、SETUP 请求 13 次，设备/配置描述符回调分别 3/4 次；主机分配地址 2，完成 SET_CONFIGURATION 并触发一次挂载回调。耗时 383 ms，说明本次成功并不需要超过原有 5 秒窗口。中断顺序修正后复测成功，但未做单因素对照，不能将全部原因唯一归结于该修改。

本项确认数据链路能够完成枚举；rx/tx 为 0 属于 probe 的预期行为，尚未验证 CDC ECHO、连续拔插或安卓手机连接。下一项执行 `hmi_test usb-echo`，在 30 秒窗口内打开系统 USB 新增的 COM 端口，关闭串口助手本地回显，发送数据并核对收到的字节。测试结束设备主动断开，新端口消失属于例程清理行为；超时可等 TEST IDLE 后重新执行。

本次仅归档用户日志并引导下一项，未修改/烧录固件，未打开 COM8；文档检查使用 `git diff --check`。

### USB CDC 文本回显验收通过（2026-10-01）

用户在固件 `1b36971` 上运行 `usb-echo`，同时提供电脑串口助手的十六进制收发记录。以下板端日志仅整理了粘贴产生的折行、空白和行尾转义痕迹，数值保持不变：

```text
msh >hmi_test usb-echo
TEST BEGIN usb-echo
msh >USB system connector VBUS=1; debug USB/COM8 is separate
USB start: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB start: SYSSTS0=0001 INTSTS0=00C0 INTENB0=FD00 USBADDR=0000 NVIC=1
USB start: irq=2 reset=0 setup=0 desc_device=0 desc_config=0
USB waiting up to 15 seconds for host enumeration
USB enumeration: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB enumeration: SYSSTS0=0001 INTSTS0=20B0 INTENB0=DD00 USBADDR=0002 NVIC=1
USB enumeration: irq=48 reset=2 setup=13 desc_device=3 desc_config=4
USB CDC configured, echo window 30s. Use the NEW COM port.
USB mounted_events=1 rx=13 tx_queued=13 tx_complete_events=1
TEST RESULT usb-echo WAIT code=1 elapsed=30341 ms
TEST IDLE
```

电脑串口助手记录：

```text
[18:33:54.229] TX>
48 65 6C 6C 6F 20 55 53 42 20 31 32 33
[18:33:54.248] RX>
48 65 6C 6C 6F 20 55 53 42 20 31 32 33
```

结论：13 字节 `Hello USB 123` 逐字节一致，基础文本 ECHO 验收通过。板端接收和发送排队均为 13 字节，并发生一次发送完成回调；主机记录进一步证明收到正确内容。两条主机记录相隔约 19 ms，仅为本次串口助手显示时间差，不作为 USB 性能基准。设备返回 WAIT 是设计行为：最终内容正确性需要主机比对，本次已有用户日志完成核对。

用户观察到运行后 Windows 提示音，以及约 30 秒后再次提示音。这与枚举接入、回显窗口结束后例程主动断开 USB 的生命周期相符，不是连续掉线证据。此次也验证了此前 probe 退出后，echo 能重新枚举；尚未验证运行中的物理拔插、特殊二进制字节或跨包数据。下一步可在新的 echo 窗口用 HEX 模式发送 `00 01 7F 80 FE FF 0D 0A`，关闭自动追加换行及本地回显，核对原样返回。

本次仅归档用户日志，未修改/烧录固件，未打开 COM8；文档经 `git diff --check` 检查。

### USB CDC 特殊二进制字节回显验收通过（2026-10-01）

用户再次运行 `usb-echo`，以 HEX 模式连续发送 5 组相同的 8 字节数据。电脑端原始收发记录：

```text
[18:36:02.981] TX> 00 01 7F 80 FE FF 0D 0A
[18:36:02.993] RX> 00 01 7F 80 FE FF 0D 0A
[18:36:03.596] TX> 00 01 7F 80 FE FF 0D 0A
[18:36:03.605] RX> 00 01 7F 80 FE FF 0D 0A
[18:36:04.175] TX> 00 01 7F 80 FE FF 0D 0A
[18:36:04.183] RX> 00 01 7F 80 FE FF 0D 0A
[18:36:04.669] TX> 00 01 7F 80 FE FF 0D 0A
[18:36:04.676] RX> 00 01 7F 80 FE FF 0D 0A
[18:36:05.091] TX> 00 01 7F 80 FE FF 0D 0A
[18:36:05.102] RX> 00 01 7F 80 FE FF 0D 0A
```

板端日志（仅整理重复提示符、空行和粘贴产生的行尾转义痕迹）：

```text
msh >hmi_test usb-echo
TEST BEGIN usb-echo
msh >USB system connector VBUS=1; debug USB/COM8 is separate
USB start: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB start: SYSSTS0=0001 INTSTS0=00C0 INTENB0=FD00 USBADDR=0000 NVIC=1
USB start: irq=2 reset=0 setup=0 desc_device=0 desc_config=0
USB waiting up to 15 seconds for host enumeration
USB enumeration: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB enumeration: SYSSTS0=0001 INTSTS0=20B0 INTENB0=DD00 USBADDR=0002 NVIC=1
USB enumeration: irq=48 reset=2 setup=13 desc_device=3 desc_config=4
USB CDC configured, echo window 30s. Use the NEW COM port.
USB mounted_events=1 rx=40 tx_queued=40 tx_complete_events=5
TEST RESULT usb-echo WAIT code=1 elapsed=30342 ms
TEST IDLE
```

结论：5 组收发逐字节一致，共 40 字节，与板端 rx=40、tx_queued=40、tx_complete_events=5 一致。NUL（00）、高位字节（80/FE/FF）及 CR/LF（0D/0A）均原样回显，未被字符串终止或换行处理截断/转换。本次特殊字节验收通过；板端 WAIT 仍按主机实际比对结果完成人工验收。

目前 USB 基础枚举、文本及上述特殊二进制字节回显已通过，可以继续 RW007 独立例程验收。单次超过 64 字节的跨包传输、完整 0～255 字节集合、长时间压力、运行中拔插及安卓手机连接尚未验证，不据此宣称通过。无需为本次记录修改固件；未打开 COM8，文档使用 `git diff --check` 检查并提交。

### USB 128字节跨包回显验收通过（2026-10-01）

用户提供主机一组128字节TX/RX记录，内容均为顺序00～7F，逐字节一致，跨越本例程64字节端点最大包长，跨包回显验收通过。板端累计 rx=2304、tx_queued=2304，相当于18组128字节的总长度；用户只贴出其中一组主机记录，不据总计数推断其余数据内容均已逐字节比对。tx_complete_events=72 是发送完成回调次数，不能当作应用消息条数。

本轮耗时30340 ms、mounted_events=1，正常返回 WAIT 与 TEST IDLE。两条主机记录相隔约14 ms，仅为串口助手时间戳差，不作USB吞吐或延迟基准。当前关闭跨包数据待补项，运行中物理拔插恢复仍待验证。本次只归档用户日志，不修改固件或打开COM8。

板端日志（仅去除行内提示符）：

```text
msh >hmi_test usb-echo
TEST BEGIN usb-echo
USB system connector VBUS=1; debug USB/COM8 is separate
USB start: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB start: SYSSTS0=0001 INTSTS0=00C0 INTENB0=FD00 USBADDR=0000 NVIC=1
USB start: irq=1 reset=0 setup=0 desc_device=0 desc_config=0
USB waiting up to 15 seconds for host enumeration
USB enumeration: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB enumeration: SYSSTS0=0001 INTSTS0=20B1 INTENB0=DD00 USBADDR=0002 NVIC=1
USB enumeration: irq=47 reset=2 setup=13 desc_device=3 desc_config=4
USB CDC configured, echo window 30s. Use the NEW COM port.
USB mounted_events=1 rx=2304 tx_queued=2304 tx_complete_events=72
TEST RESULT usb-echo WAIT code=1 elapsed=30340 ms
TEST IDLE
```

主机原始收发记录：

```text
[0001] [22:51:51.292] TX> 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 20 21 22 23 24 25 26 27 28 29 2A 2B 2C 2D 2E 2F 30 31 32 33 34 35 36 37 38 39 3A 3B 3C 3D 3E 3F 40 41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F 50 51 52 53 54 55 56 57 58 59 5A 5B 5C 5D 5E 5F 60 61 62 63 64 65 66 67 68 69 6A 6B 6C 6D 6E 6F 70 71 72 73 74 75 76 77 78 79 7A 7B 7C 7D 7E 7F
[0002] [22:51:51.306] RX> 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F 20 21 22 23 24 25 26 27 28 29 2A 2B 2C 2D 2E 2F 30 31 32 33 34 35 36 37 38 39 3A 3B 3C 3D 3E 3F 40 41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F 50 51 52 53 54 55 56 57 58 59 5A 5B 5C 5D 5E 5F 60 61 62 63 64 65 66 67 68 69 6A 6B 6C 6D 6E 6F 70 71 72 73 74 75 76 77 78 79 7A 7B 7C 7D 7E 7F
```

### USB 运行中物理拔插与回显恢复验收通过（2026-10-01）

用户按指引保持ART-Link/COM8连接，只拔插系统USB线，并提供同一次 `usb-echo` 窗口内的两组收发记录。拔插前 `11 22 33 44`、插回后 `AA 55 00 FF` 均逐字节一致。板端 mounted_events=2，rx=8、tx_queued=8、tx_complete_events=2，30341 ms后正常返回WAIT与TEST IDLE；日志未出现重新启动横幅。

结论：本次运行中的物理拔插、重新枚举与数据回显恢复通过，关闭USB物理拔插待办。结合此前128字节跨包验收，当前任务书USB基础收尾已完成。本次只覆盖一次拔插循环，不扩展为长时间压力或安卓OTG兼容性通过。本轮仅归档用户证据，未修改/烧录固件或打开COM8。

主机原始记录：

```text
[23:03:00.205] TX> 11 22 33 44
[23:03:00.212] RX> 11 22 33 44
[23:03:12.413] TX> AA 55 00 FF
[23:03:12.421] RX> AA 55 00 FF
```

板端原始记录（仅去除行内提示符）：

```text
msh >hmi_test usb-echo
TEST BEGIN usb-echo
USB system connector VBUS=1; debug USB/COM8 is separate
USB start: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB start: SYSSTS0=0001 INTSTS0=00C0 INTENB0=FD00 USBADDR=0000 NVIC=1
USB start: irq=2 reset=0 setup=0 desc_device=0 desc_config=0
USB waiting up to 15 seconds for host enumeration
USB enumeration: VBUS_pin=1 VBSTS=1 DPRPU=1 UCK=40 SYSCFG=0411
USB enumeration: SYSSTS0=0001 INTSTS0=00B1 INTENB0=DD00 USBADDR=0002 NVIC=1
USB enumeration: irq=48 reset=2 setup=13 desc_device=3 desc_config=4
USB CDC configured, echo window 30s. Use the NEW COM port.
USB mounted_events=2 rx=8 tx_queued=8 tx_complete_events=2
TEST RESULT usb-echo WAIT code=1 elapsed=30341 ms
TEST IDLE
```

## 用户逐项验收：RW007 模块信息（2026-10-01）

用户在当前固件上执行独立例程 `rw007-info`，提供如下日志：

```text
msh >hmi_test rw007-info
TEST BEGIN rw007-info
msh >RW007: RW007 SCI3 mode0 1MHz, IRQ13 ready
RW007: bad SPI header phase1 FFFFFFFF FFFFFFFF flags=FF
RW007: command=0 result=0 bytes=0
RW007: command=5 result=0 bytes=24
INFO: firmware=RW007_2.1.0-a7a0d089-57
RW007: command=4 result=0 bytes=32
INFO: serial=rw007c745bb22fc584aa6ed30
RW007: command=2 result=0 bytes=6
INFO: Wi-Fi MAC=FC:58:4A:A6:ED:30 expected BLE name=RW007-ED30
TEST RESULT rw007-info PASS code=0 elapsed=1874 ms
TEST IDLE
```

结论：模块信息读取验收通过，耗时 1874 ms，正常回到 TEST IDLE。固件版本、序列号和 Wi-Fi MAC 与此前记录一致；`RW007-ED30` 是依据 MAC 推导的预期 BLE 名称，本项未验证实际广播。

保留异常：模块启动阶段出现一次全 FF SPI 头，随后初始化及信息查询均成功。本次说明后续通信已恢复，不能据此宣称 SPI 无错误或已定位全 FF 的根因。Wi-Fi 扫描、联网和 BLE 扫描/广播仍需分别验收。

下一步开启手机 2.4 GHz 热点，执行 `hmi_test rw007-wifi`，核对扫描结果是否包含用户热点。此阶段仅扫描，无需密码。本次只归档用户日志，未修改或烧录固件，未打开 COM8；文档通过 `git diff --check` 后提交。

## 用户逐项验收：RW007 Wi-Fi 扫描（2026-10-01）

用户开启 2.4 GHz 热点后运行独立例程，提供以下日志：

```text
msh >hmi_test rw007-wifi
TEST BEGIN rw007-wifi
msh >RW007: RW007 SCI3 mode0 1MHz, IRQ13 ready
RW007: bad SPI header phase1 00000000 00000000 flags=00
RW007: command=0 result=0 bytes=0
RW007: command=1 result=0 bytes=0
RW007: command=6 result=0 bytes=0
WIFI[1]: SSID=Hotspot channel=11 RSSI=-48 security=0x00400004
WIFI[2]: SSID=iTV-YaMp channel=8 RSSI=-79 security=0x00400006
WIFI[3]: SSID=ChinaNet-YaMp channel=8 RSSI=-79 security=0x00400006
WIFI[4]: SSID=<hidden> channel=6 RSSI=-81 security=0x00400004
WIFI[5]: SSID=ChinaNet-0916 channel=6 RSSI=-83 security=0x00400004
WIFI[6]: SSID=TP-LINK_3842 channel=11 RSSI=-88 security=0x00400004
WIFI: scan complete result=0 reports=6
TEST RESULT rw007-wifi PASS code=0 elapsed=3275 ms
TEST IDLE
```

结论：Wi-Fi 扫描验收通过，耗时 3275 ms，共收到 6 条报告，完成事件 result=0。用户热点 Hotspot 可见，信道 11、RSSI=-48 dBm；本项只确认扫描，不代表已关联热点或可访问互联网。

保留异常：本次启动阶段 SPI 头为全 0（flags=00），与上一项信息读取的全 FF 不同，后续初始化及扫描成功。尚未定位启动异常应答的根因，不能声明 SPI 零错误。

下一步保持热点和手机移动数据开启，使用 `hmi_test rw007-internet <ssid> <password>` 验证热点关联、DHCP、DNS、TCP 和 HTTP 正文匹配。当前扫描已能发现目标热点，但此前重构版联网因未发现热点而失败的记录，仍需新的联网结果才能关闭。本次只归档用户日志，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

## 用户逐项验收：RW007 热点联网（2026-10-01）

用户保持 2.4 GHz 热点 Hotspot 与手机移动数据开启，执行独立联网例程。日志如下，命令中的密码替换为占位符，不将凭据保存到仓库：

```text
msh >hmi_test rw007-internet Hotspot <password>
TEST BEGIN rw007-internet
msh >RW007: RW007 SCI3 mode0 1MHz, IRQ13 ready
RW007: bad SPI header phase1 00000000 00000000 flags=00
RW007: command=0 result=0 bytes=0
RW007: command=1 result=0 bytes=0
RW007: command=6 result=0 bytes=0
WIFI[1]: SSID=Hotspot channel=11 RSSI=-50 security=0x00400004
WIFI[2]: SSID=iTV-YaMp channel=8 RSSI=-79 security=0x00400006
WIFI[3]: SSID=<hidden> channel=6 RSSI=-79 security=0x00400004
WIFI[4]: SSID=ChinaNet-YaMp channel=8 RSSI=-79 security=0x00400006
WIFI[5]: SSID=<hidden> channel=9 RSSI=-79 security=0x00400006
WIFI[6]: SSID=华为 channel=6 RSSI=-80 security=0x00400004
WIFI[7]: SSID=ChinaNet-0916 channel=6 RSSI=-84 security=0x00400004
WIFI[8]: SSID=SCQD channel=5 RSSI=-85 security=0x00400004
WIFI[9]: SSID=CU_K5nC channel=1 RSSI=-85 security=0x00400004
WIFI[10]: SSID=<hidden> channel=5 RSSI=-86 security=0x00400004
WIFI[11]: SSID=TP-LINK_D988 channel=1 RSSI=-88 security=0x00400004
WIFI[12]: SSID=MERCURY_3A98 channel=1 RSSI=-89 security=0x00400006
WIFI: scan complete result=0 reports=12
RW007: command=2 result=0 bytes=6
NET: joining SSID=Hotspot
RW007: command=7 result=0 bytes=0
WIFI: event=1 result=0 bytes=0
NET: hotspot associated
NET: waiting for DHCP
NET: IP=192.168.249.97
NET: gateway=192.168.249.115
NET: DNS=192.168.249.115
NET: www.msftconnecttest.com=23.206.188.213
NET: TCP connected, requesting public connectivity endpoint
NET: HTTP status=200 received=187 expected_body=MATCH
NET: tx=15 rx=16 drops=0; INTERNET PASS result=0
TEST RESULT rw007-internet PASS code=0 elapsed=7756 ms
TEST IDLE
```

结论：重构后的独立 Wi-Fi 联网例程验收通过，耗时 7756 ms。成功关联热点，取得 DHCP 地址、网关及 DNS，解析公网域名、建立 TCP 连接，收到 HTTP 200 且正文匹配。网络统计 tx=15、rx=16、drops=0，正常退出回到 TEST IDLE。此前因未发现 Hotspot 导致的联网待复测项，本次关闭；保留原失败记录作为环境条件不足时的历史证据。

启动时仍有一次全 0 SPI 头异常，后续通信恢复。网络层 drops=0 不代表底层 SPI 从未出错，也不证明长期连接稳定性；本项未覆盖 HTTPS、吞吐量或断网重连。

下一项为 `hmi_test rw007-ble`：由开发板扫描周围 BLE 广播，检查报告及扫描结束事件。本次只更新验收文档，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

## 用户逐项验收：RW007 BLE 扫描（2026-10-01）

用户执行独立 BLE 扫描例程，日志如下：

```text
msh >hmi_test rw007-ble
TEST BEGIN rw007-ble
msh >RW007: RW007 SCI3 mode0 1MHz, IRQ13 ready
RW007: bad SPI header phase1 00000000 00000000 flags=00
RW007: command=0 result=0 bytes=0
BLE: init queued without ACK; following operation must confirm support
BLE: response=0x51 result=0 bytes=12
BLE: public=FC:58:4A:A6:ED:30
BLE[1]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE[2]: 7B:09:11:6B:5D:86 type=1 RSSI=-63 name=<unnamed>
BLE[3]: 7B:09:11:6B:5D:86 type=1 RSSI=-63 name=<unnamed>
BLE[4]: 7B:09:11:6B:5D:86 type=1 RSSI=-61 name=<unnamed>
BLE[5]: 7B:09:11:6B:5D:86 type=1 RSSI=-64 name=<unnamed>
BLE[6]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE[7]: B4:E7:B3:79:31:8B type=0 RSSI=-53 name=<unnamed>
BLE[8]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE[9]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE[10]: B4:E7:B3:79:31:8B type=0 RSSI=-53 name=<unnamed>
BLE[11]: 7B:09:11:6B:5D:86 type=1 RSSI=-63 name=<unnamed>
BLE[12]: 7B:09:11:6B:5D:86 type=1 RSSI=-62 name=<unnamed>
BLE[13]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE[14]: B4:E7:B3:79:31:8B type=0 RSSI=-57 name=<unnamed>
BLE[15]: 7B:09:11:6B:5D:86 type=1 RSSI=-63 name=<unnamed>
BLE[16]: 7B:09:11:6B:5D:86 type=1 RSSI=-62 name=<unnamed>
BLE[17]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE[18]: 7B:09:11:6B:5D:86 type=1 RSSI=-65 name=<unnamed>
BLE[19]: B4:E7:B3:79:31:8B type=0 RSSI=-55 name=<unnamed>
BLE[20]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE[21]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE[22]: 7B:09:11:6B:5D:86 type=1 RSSI=-59 name=<unnamed>
BLE[23]: B4:E7:B3:79:31:8B type=0 RSSI=-55 name=<unnamed>
BLE[24]: 7B:09:11:6B:5D:86 type=1 RSSI=-60 name=<unnamed>
BLE: scan complete reason=0 reports=25
TEST RESULT rw007-ble PASS code=0 elapsed=5169 ms
TEST IDLE
```

结论：BLE 扫描验收通过，耗时 5169 ms。模块地址查询成功，扫描收到 25 条报告，并以 reason=0 正常结束。程序只打印前 24 条，因此日志最后编号为 24 与 reports=25 不矛盾。已打印记录含两个不同地址，重复报告不能计作新的设备，第 25 条未打印，不能断言总共恰好两个设备。

本例程使用被动扫描，`name=<unnamed>` 表示当前报告中没有解析到名称，不代表扫描失败。初始化命令本身没有 ACK，但后续地址响应和实际扫描报告提供了 BLE 功能证据。启动时仍出现一次全 0 SPI 头，保留为尚未定位原因的异常，未据此否定已完成的扫描，也不宣称 SPI 零错误。

下一项运行 `hmi_test rw007-adv`，手机 BLE 扫描页确认能发现 RW007-ED30。源码下发 60 秒广播参数，但测试线程只观察 15 秒即执行关闭；手机须提前开始扫描，在该窗口内确认。扫描接收通过不替代广播可发现性、连接或 GATT 数据功能验收。

本次仅归档用户日志，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

## 用户逐项验收：RW007 BLE 广播可发现性（2026-10-01）

用户确认手机能够发现 `RW007-ED30`，手工报告地址为 `FC:56:4A:A6:ED:30`。板端日志如下，仅整理行尾空白：

```text
msh >hmi_test rw007-adv
TEST BEGIN rw007-adv
msh >RW007: RW007 SCI3 mode0 1MHz, IRQ13 ready
RW007: bad SPI header phase1 00000000 00000000 flags=00
RW007: command=0 result=0 bytes=0
BLE: init queued without ACK; following operation must confirm support
RW007: command=25 result=0 bytes=0
ADV: command accepted for 60 seconds; verify discovery on phone
TEST RESULT rw007-adv WAIT code=1 elapsed=17156 ms
TEST IDLE
```

结论：按用户手机发现设备的反馈，广播可发现性验收通过。板端广播命令返回成功，并完成观察窗口后正常退出，总耗时 17156 ms（包含初始化）。WAIT 表示此项依赖手机确认，本次用户反馈已补齐可发现性的人工证据。日志中的 60 秒为下发的广播时长参数，例程实际观察约 15 秒即关闭，二者并不等同。

地址差异已核实关闭（2026-10-01）：此前用户手工记录为 `FC:56:4A:A6:ED:30`，该历史反馈保留。用户按指引复核后明确回复 `FC:58:4A:A6:ED:30`，与 `rw007-info` 的 Wi-Fi MAC、`rw007-ble` 的 BLE public 地址一致，确认第二字节为 `58`。广播可发现性及手机地址一致性均已验收通过。本次只收到地址确认，没有新增板端日志；不扩展为连接、服务枚举或 GATT 收发验收。助手本轮只更新文档，未打开 COM8、修改代码或烧录。

启动时一次全 0 SPI 头异常仍保留，后续命令成功不代表底层零错误。下一项可先做 A0/P000 的 ADC 采样，再用已知 GND/3.3 V 输入进行端点验证。本次仅归档用户日志并更新状态，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

### 当前 BLE 例程连接、断开后重跑再连接通过（2026-10-01）

用户按连接复验指引提供两轮 `hmi_test rw007-adv` 日志。第一轮出现 `connection status=0`，随后在TEST RESULT之前出现 `disconnected`；等TEST IDLE后启动第二轮，再次出现 `connection status=0`。当前例程的连接、断开事件上报、重新运行后再次连接路径验收通过，关闭该收尾项。

第一轮耗时17148 ms，第二轮17157 ms，均返回WAIT和TEST IDLE。第二轮没有在测试结束前记录断开事件，不记为第二次主动断开成功。这里只验证重启例程后的再次连接，未验证同一轮内断开后自动恢复广播/重连；日志未包含断开原因或手机界面，不单凭事件断定由哪端发起断开。0xA0/0xA8事件保留原样，不将其解释成通用BLE串口收发通过。

两轮初始化均出现首个SPI头全FF，后续命令和连接事件恢复正常；保留既知现象，不记录为零通信异常。本次仅归档用户日志，不修改/烧录固件或打开COM8。

原始输出（仅去除重复提示符与空行）：

```text
msh >hmi_test rw007-adv
TEST BEGIN rw007-adv
RW007: RW007 SCI3 mode0 1MHz, IRQ13 ready
RW007: bad SPI header phase1 FFFFFFFF FFFFFFFF flags=FF
RW007: command=0 result=0 bytes=0
BLE: init queued without ACK; following operation must confirm support
RW007: command=25 result=0 bytes=0
ADV: command accepted for 60 seconds; verify discovery on phone
BLE: connection status=0
BLE: event=0xA0 result=0 bytes=46
BLE: event=0xA0 result=0 bytes=46
BLE: event=0xA8 result=0 bytes=8
BLE: event=0xA8 result=0 bytes=8
BLE: disconnected
TEST RESULT rw007-adv WAIT code=1 elapsed=17148 ms
TEST IDLE

msh >hmi_test rw007-adv
TEST BEGIN rw007-adv
RW007: RW007 SCI3 mode0 1MHz, IRQ13 ready
RW007: bad SPI header phase1 FFFFFFFF FFFFFFFF flags=FF
RW007: command=0 result=0 bytes=0
BLE: init queued without ACK; following operation must confirm support
RW007: command=25 result=0 bytes=0
ADV: command accepted for 60 seconds; verify discovery on phone
BLE: connection status=0
BLE: event=0xA0 result=0 bytes=46
BLE: event=0xA0 result=0 bytes=46
BLE: event=0xA8 result=0 bytes=8
TEST RESULT rw007-adv WAIT code=1 elapsed=17157 ms
TEST IDLE
```

## 用户逐项验收：ADC A0 基础采样（2026-10-01）

按本轮悬空采样步骤，用户提供以下日志；本次未提供已知输入电压：

```text
msh >hmi_test adc-sample
TEST BEGIN adc-sample
msh >ADC A0/P000 n=32 min=1495 max=1865 avg=1656 approx_mV=1334 (Vref assumed 3300mV)
TEST RESULT adc-sample WAIT code=1 elapsed=11 ms
TEST IDLE
```

结论：A0/P000 的 32 次 ADC 转换、统计和退出流程完成，耗时 11 ms。原始读数 1495～1865，均值 1656；1334 mV 是按参考电压 3300 mV 假设换算的均值，不是外部电压表测量结果。悬空输入没有固定目标值，本次 WAIT 合理，不能据此确认端点准确性、精度或噪声性能。

下一步移除 A0 上可能存在的其他连接，用跳线将 Arduino A0/P000 接板上 GND，执行 `hmi_test adc-low`。例程要求 32 次采样的最大值小于 100 才 PASS；随后另行进行 3.3 V 高端测试。接线时先断电，确认针脚后再上电，保持原有调试串口接线。本次仅归档用户日志，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

### ADC 接线指引补充（2026-10-01）

用户反馈底部找不到 Arduino A0。核对工程原有背面渲染图和 V3.1 原理图第 3 页后确认：本板实际丝印为 P000，对应 Arduino A0 / J6 第 1 脚。此前仅使用 Arduino 名称的指引不够直观，已在 `peripheral-tests.md` 补充接口方向、6 孔排母位置，以及 P000/GND 标注图 `docs/picture/adc-a0-connector.png`。图由原有 back.png 裁剪标注，已目视核对。此项只澄清接线，adc-low 仍待用户实测，未更改固件、未打开 COM8。

### ADC 接地低端验收通过（2026-10-01）

在 P000/A0 接板上 GND 的测试步骤后，用户分别执行基础采样和低端断言例程，提供以下日志（仅整理空行及行尾空白）：

```text
msh >hmi_test adc-sample
TEST BEGIN adc-sample
msh >ADC A0/P000 n=32 min=0 max=2 avg=0 approx_mV=0 (Vref assumed 3300mV)
TEST RESULT adc-sample WAIT code=1 elapsed=9 ms
TEST IDLE

msh >hmi_test adc-low
TEST BEGIN adc-low
msh >ADC A0/P000 n=32 min=0 max=2 avg=0 approx_mV=0 (Vref assumed 3300mV)
TEST RESULT adc-low PASS code=0 elapsed=9 ms
TEST IDLE
```

结论：A0/P000 接地低端验收通过。两个独立例程各完成 32 次转换，读数均为 0～2，耗时各 9 ms；adc-low 的最大值 2 小于阈值 100。adc-sample 固定返回 WAIT，仅报告采样统计；本次低端 PASS 由 adc-low 的断言给出。均值和估算电压按整数运算显示为 0，不能据此认为每次转换都为 0，也不等于校准后的精度结论。

下一步断电，先移除 P000 与 GND 的连接，再将 P000 接电源排母丝印 3.3V 的孔，上电执行 `hmi_test adc-high`。不可保留接地跳线同时接 3.3V，避免短接电源；不要误接 5V 或 VIN。高端要求所有读数大于 3995。本次只归档用户日志，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

### ADC 3.3 V 高端验收通过（2026-10-01）

在 P000/A0 改接板上 3.3 V 的测试步骤后，用户提供以下日志：

```text
msh >hmi_test adc-high
TEST BEGIN adc-high
msh >ADC A0/P000 n=32 min=4081 max=4092 avg=4086 approx_mV=3292 (Vref assumed 3300mV)
TEST RESULT adc-high PASS code=0 elapsed=11 ms
TEST IDLE
```

结论：A0/P000 高端验收通过，32 次转换的最小值 4081 大于阈值 3995，耗时 11 ms，正常返回 TEST IDLE。均值 4086，按 3.3 V 参考假设估算为 3292 mV；这不是电压表实测值，不能把与 3300 mV 的差值直接解释为 ADC 绝对误差。

结合此前接地 0～2 的结果，A0 的基础采样及 GND/3.3 V 两端功能验收完成。未测试中间电压线性、参考电压校准或其他模拟输入通道，不宣称全通道精度验证完成。

下一项为 GPIO 外接回环：断电后移除 P000 到 3.3 V 的跳线，按实际丝印连接 P008（Arduino D2，输出）与 P009（Arduino D9，输入），建议串联 1 kΩ；两个引脚不接其他外部信号。上电执行 `hmi_test gpio-loop`，逐次核对 16 次高低电平。本次仅归档用户日志和更新验收状态，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

## 用户逐项验收：GPIO 外接回环（2026-10-01）

用户没有 1 kΩ 电阻，按本轮指引采用杜邦线直接连接 P008（Arduino D2）与 P009（Arduino D9）。本例程将 P008 配置为输出，P009 配置为输入；串联电阻用于误配置时限流，并非此项回环的必要条件。用户随后提供：

```text
msh >hmi_test gpio-loop
TEST BEGIN gpio-loop
msh >TEST RESULT gpio-loop PASS code=0 elapsed=35 ms
TEST IDLE
```

结论：本项 GPIO 外接回环验收通过，耗时 35 ms。根据例程的 PASS 条件，P008 输出的 16 次高低电平均由 P009 正确读回。仅验证这两个引脚及当前跳线的低速数字输入/输出，不替代所有 GPIO、外部中断或高速接口测试。

下一步断电并拆除 P008/P009 跳线，进行 Pmod0 SPI 回环：Pmod0 J1 的 MOSI/P305 与 MISO/P304 直接短接（更正：原理图针号为 3、5，即同排第 2、3 列），移除该口其他外接模块，上电执行 `hmi_test pmod-spi0`，期望 8×64 字节 MATCH 和 PASS。本次只归档用户日志并更新状态，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

### Pmod0 接线方向与针号更正（2026-10-01）

用户询问 MOSI/MISO 位于靠板边还是靠板内。核对原有背面渲染图方形 1 脚焊盘和 V3.1 原理图第 3 页 J1 后，确认：从元件面看 PCB 两排焊脚，靠板内/MCU 排从左至右为 CS、MOSI、MISO、SCK、GND、3V3；靠板边排为 IRQ、IO0、IO1、IO2、GND、3V3。MOSI/MISO 是靠内排第 2、3 列，对应本板原理图 J1-3/J1-5。此前指引把排内位置当作针号，已更正，并同步核对修正 Pmod1 及 Pmod IRQ 的原理图编号。

已向 `peripheral-tests.md` 增补方向表及 `docs/picture/pmod0-spi-pads.png`。图由工程原有渲染图裁剪标注，已目视检查；圈出的是 PCB 焊点，不能混同弯脚插座从板外看的插孔上下方向。当前 Pmod0 回环尚未收到用户测试日志，不记为已通过。本次未修改固件、未打开 COM8，文档经 `git diff --check` 检查后提交。

## 用户逐项验收：Pmod0 SPI 回环失败待定位（2026-10-01）

用户按接线指引运行 `pmod-spi0`：

```text
msh >hmi_test pmod-spi0
TEST BEGIN pmod-spi0
msh >TEST RESULT pmod-spi0 FAIL code=-1 elapsed=5 ms
TEST IDLE
```

结论：本项未通过。旧例程把分频计算、打开驱动、启动传输、回调错误和数据不匹配都归并为 -1；5 ms 的总耗时不足以确定具体失败原因。不能直接归因于接反插座或驱动缺陷。

本次给 `test-pmod-spi0.c` 增加分阶段诊断：

- 片选保持高电平，P305 临时作为 GPIO 输出 0/1/0/1，P304 配置为上拉输入逐次读回，每步等待 2 ms；断线时上拉应使低电平检查失败。此项只驱动 P305，不主动探测其他排的 GPIO。
- 接线检查通过后，显式将 P304/P305/P306 复用为 SCI6，按原有 1 MHz、mode0、工程生成的 DTC 配置执行八组 64 字节收发。退出时恢复片选及三个信号脚，仍是单文件、单线程入口。
- 分别报告引脚配置、分频、驱动打开/启动/关闭的 FSP 返回码；传输错误打印回调事件，超时返回超时码，字节不匹配报告首个 pattern/byte/TX/RX。
- 更正文件头的 J1 针号为 MOSI/P305=J1-3、MISO/P304=J1-5，与已核对原理图一致。

验证：RT-Thread Studio 最终构建 0 错误、0 警告，DAP-LINK 烧录成功并复位，日志见 `logs/pmod-spi0-diagnostics-build.log`。17 项主机工具与独立例程结构检查通过，`git diff --check` 通过；这些检查不等于 Pmod0 的板上回环已通过。

低速接线读回成功只证明 P305/P304 之间能传递高低电平，不能替代后面的 SPI 字节校验；失败只说明当前连线/电平未通过，仍需结合日志排查，不能自动断言是用户接线错误。本次未打开 COM8，等待用户在新固件上保持当前接线重跑同一命令，提供完整分阶段输出。

### Pmod0 低速接线检查失败（2026-10-01）

用户在诊断固件 `d244fcc` 上复测：

```text
msh >hmi_test pmod-spi0
TEST BEGIN pmod-spi0
msh >PMOD spi0 wire P305=0 P304=1
PMOD spi0 wire FAIL: check MOSI/P305 to MISO/P304 (J1-3 to J1-5)
TEST RESULT pmod-spi0 FAIL code=-8 elapsed=14 ms
TEST IDLE
```

结论：失败发生于低速 GPIO 接线检查，尚未打开 SCI6 或执行 SPI 数据传输。程序向 P305 写入低电平，P304 的实际读回仍为 1；P304 配置了上拉，因此结果与未接通的输入相符，但仅凭日志不能唯一判定为接错排、跳线断路、接触不良或板端电气问题。日志中的 P305=0 是请求输出值，并非对 P305 的独立电压测量。code=-8 对应本例程返回的 -RT_EIO。

下一步先核对实物 PMOD0 插座及跳线两端位置。先前图片圈出 PCB 焊点，弯脚插座从板外看插孔的上下方向不能直接按该图推断；需要用户提供能同时看清 PMOD0 丝印、插座开口和两端跳线的照片，再给出实物方向指引。如使用万用表通断检查，应先断开所有供电（包括两条 USB），确认目标孔分别通到 MOSI/P305、MISO/P304，且跳线自身导通。不将本次接线失败误记成 SPI 驱动或 DTC 故障。

本次只归档诊断结果，未修改或烧录固件、未打开 COM8；文档经 `git diff --check` 检查后提交。

### Pmod 接线实物照片核对（2026-10-01）

用户提供弯脚插座开口侧的近照。照片可见两根杜邦针插入插座，但 PMOD0/PMOD1 丝印及完整方位未清晰呈现，不能仅凭该图确认目标插座和针位。照片背景靠近无线模块，需优先排除插在相邻 PMOD1 的可能，此处是待核实判断，不是已确认的接错结论。

工程原有背面渲染图显示靠近 RW007 无线模块的插座标为 PMOD1，但此方位不能直接等同于用户实物；后续用户报告实物 PMOD0 箭头指向靠无线模块的接口。因此撤回按渲染图方位建议用户换口的判断，具体接口映射待电气测试确认。当前仍处于 P305/P304 低速连通性失败的排查阶段，未据照片判定 SPI 驱动问题。本次未修改固件、未打开 COM8。

### 用户实物丝印与参考图方位不一致（2026-10-01）

用户补充原话：“丝印上面只有PMOD1，没有PMOD0，我看到一个箭头PMOD0指向的就是靠近无线模块的”。该反馈保留为实物观察，不擅自改写成用户插错接口，也不据此断言板卡版本或 PCB 网络有误。此前助手根据参考渲染图断言靠无线模块必为 PMOD1，依据不足，已撤回。

可核实的软件映射为：`pmod-spi0` 使用 SCI6 / P305 MOSI / P304 MISO；`pmod-spi1` 使用 SCI7 / P613 MOSI / P614 MISO。下一步保持当前仅有 MOSI/MISO 跳线的接法，待 TEST IDLE 后执行独立例程 `hmi_test pmod-spi1` 作对照。若完成 8×64 字节 MATCH，可确认 SCI7 这组信号回环有效；若失败，原 spi1 例程缺少详细诊断，不能单凭 FAIL 推断接口身份或排除 SCI7。此对照不替代 pmod-spi0 的验收，也不引入例程间调用或初始化依赖。

本次仅更正接线指引和归档用户反馈，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

## 用户逐项验收：Pmod SPI1 回环及当前接线映射（2026-10-01）

用户按指引保持此前跳线不动，改运行独立例程 `pmod-spi1`，提供以下日志：

```text
msh >hmi_test pmod-spi1
TEST BEGIN pmod-spi1
msh >PMOD spi1 MOSI/MISO loop 8x64 bytes MATCH
TEST RESULT pmod-spi1 PASS code=0 elapsed=16 ms
TEST IDLE
```

结论：SCI7 / P613 MOSI / P614 MISO 的 SPI1 回环验收通过。1 MHz 下八组 64 字节（共 512 字节）逐字节匹配，耗时 16 ms，正常退出。结合保持接线的用户对照操作，可确认当前跳线使 P613/P614 这组信号回环成功；此前 SPI0 的 P305 输出低而 P304 读高，与当前接在另一组信号上的结果相符，不能据此判断 SCI6 或 DTC 故障。

更新接口定位：当前成功接线对应软件命令 pmod-spi1，不再仅凭用户所述 PMOD0 箭头或“靠无线模块”的相对位置指定软件端口。丝印/箭头与参考图之间的差异尚未查明，不声称板卡标错或具体硬件版本不同。

下一步断电，把跳线移至另一个 Pmod 接口的 MOSI/MISO（按该口标识核对，保持两脚相邻的接法），拆除当前这组跳线；上电执行 `hmi_test pmod-spi0`。其低速检查应先读回 0/1/0/1，再进行 SPI 字节校验。SPI1 通过不替代 SPI0 验收。此轮未修改或烧录固件、未打开 COM8，只归档结果及更新指引；文档经 `git diff --check` 检查后提交。

## 用户逐项验收：Pmod SPI0 复验通过（2026-10-01）

用户在无丝印接口上按另一个同向插座的成功孔位进行接线后，提供两组测试日志：

```text
msh >hmi_test pmod-spi1
TEST BEGIN pmod-spi1
msh >PMOD spi1 MOSI/MISO loop 8x64 bytes MATCH
TEST RESULT pmod-spi1 PASS code=0 elapsed=16 ms
TEST IDLE

: command not found.
msh >hmi_test pmod-spi0
TEST BEGIN pmod-spi0
msh >PMOD spi0 wire P305=0 P304=0
PMOD spi0 wire P305=1 P304=1
PMOD spi0 wire P305=0 P304=0
PMOD spi0 wire P305=1 P304=1
PMOD spi0 wire PASS; switching to SCI6
PMOD spi0 SCI6 1MHz mode0 DTC tx=1 rx=1
PMOD spi0 MOSI/MISO loop 8x64 bytes MATCH
TEST RESULT pmod-spi0 PASS code=0 elapsed=40 ms
TEST IDLE
```

结论：SPI1 再次通过，16 ms；SPI0 接线检查 0/1/0/1 全部匹配，随后 SCI6 在 1 MHz、mode0、收发 DTC 均启用的条件下完成 8×64 字节校验，总耗时 40 ms。两组独立例程均正常退出，本轮 Pmod SPI0/SPI1 回环功能验收完成，关闭 SPI0 待移线复测项。

此前 SPI0 的低速接线失败与当时未连通 P305/P304 的结果相符；修正接线后，在保留 DTC 的配置下测试成功，没有证据要求修复 SCI6/DTC 驱动。仍不由此次结果断言所有板卡的丝印位置、箭头含义或版本差异。日志中的 `: command not found.` 是两项测试之间的 shell 提示，来源未确认；后续合法命令已执行并通过，不作为 SPI 失败。

本次只验证两组 MOSI/MISO 外接回环，不覆盖真实 SPI 从设备的协议响应、外部片选/时钟引脚连通性或最高速率。下一步断电拆除 Pmod 回环跳线，将 Arduino 扩展排母上丝印 P512（MOSI/D11）和 P511（MISO/D12）短接，执行 `hmi_test pmod-arduino`。此次仅更新文档，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

## 用户逐项验收：Arduino 扩展口 SPI 回环（2026-10-01）

用户按 P512（MOSI/D11）与 P511（MISO/D12）直连的接线步骤，提供以下日志：

```text
msh >hmi_test pmod-arduino
TEST BEGIN pmod-arduino
msh >PMOD arduino MOSI/MISO loop 8x64 bytes MATCH
TEST RESULT pmod-arduino PASS code=0 elapsed=16 ms
TEST IDLE
```

结论：SCI4 的 Arduino 扩展口 SPI 回环验收通过，1 MHz 下八组 64 字节（共 512 字节）全部匹配，耗时 16 ms，正常退出。至此 Pmod SPI0、Pmod SPI1、Arduino SPI 三组 MOSI/MISO 回环均通过；不将回环通过等同于真实从设备协议、片选/时钟外部布线或最大速率已验证。

下一项为 Pmod0 外部中断回环：断电拆除 P512/P511 跳线，使用已通过 pmod-spi0 的同一个插座，连接 GPIO0/P211（J1-4）与 IRQ/P708（J1-2）。按原理图的对应列，另一排正对 MOSI/J1-3 的孔为 IO0/J1-4；正对 CS/J1-1 的孔为 IRQ/J1-2。这两个孔相邻，并非把原 MOSI/MISO 两孔直接换排（那样会接到 IO0/IO1）。此测试 GPIO0 为输出，IRQ 为输入，在无其他外接模块时可使用直接跳线。上电执行 `hmi_test pmod-irq0`，预期 edges=16 expected=16 和 PASS。本次仅归档用户日志，未修改或烧录固件、未打开 COM8；文档通过 `git diff --check` 后提交。

## 用户逐项验收：Pmod IRQ 无边沿与同通道输入冲突修复（2026-10-01）

用户确认连接 IRQ 与 IO0 后，分别执行两条命令，提供以下日志。尚未确认是否在两次命令之间移动了跳线，不假定两个插座同时接线：

```text
msh >hmi_test pmod-irq0
TEST BEGIN pmod-irq0
msh >PMOD irq0 GPIO->IRQ edges=0 expected=16
TEST RESULT pmod-irq0 FAIL code=-1 elapsed=176 ms
TEST IDLE

msh >hmi_test pmod-irq1
TEST BEGIN pmod-irq1
msh >PMOD irq1 GPIO->IRQ edges=0 expected=16
TEST RESULT pmod-irq1 FAIL code=-1 elapsed=177 ms
TEST IDLE
```

结论：两项均未通过，旧日志只报告中断计数，不能区分跳线未连通与中断路由问题。静态检查发现独立于接线的明确配置缺陷：

- P708 与 P006、P501 是 IRQ11 的候选输入；P709 与 P005 是 IRQ10 的候选输入。映射见 `board/ports/gpio_cfg.h`。
- `ra_gen/pin_data.c` 默认使能按键 P006、P005 的 ISEL。旧例程再使能 P708 或 P709 的 ISEL，未撤销同编号其他输入，造成重复选择。
- RA6M3 用户手册 R01UH0886EJ0110 Rev.1.10 第 20.2.5 节、PDF 第 484 页明确要求：同编号的 IRQn 只能在一个引脚上使能。已目视核对原文。重复选择不符合该要求，但未作单因素板测，不能声明用户当前接线也已正确或全部失败只由这一原因导致。

两份独立 C 例程的修正：

1. 保存目标输入、输出及同通道候选引脚的实际 PFS 配置。irq0 临时清除 P006/P501 的 ISEL，irq1 临时清除 P005 的 ISEL；保留它们的其他配置，测试退出后恢复实际原值。
2. 直接按引脚调用 R_IOPORT_PinCfg，避免本工程 rt_pin_mode 重新打开整张引脚表、把按键 ISEL 再次使能。检查并打印 FSP 初始化/清理错误。
3. 启用 ICU 前做 0/1/0/1/0 的低速接线检查，分别打印请求值、输出脚实际读回和 IRQ 输入脚实际读回。接线检查失败时不进入边沿计数。
4. 起始电平稳定为低后清除旧请求并启用双边沿中断，翻转 16 次并逐次读回。必须同时满足 levels=16/16 和 edges=16 才 PASS；IRQCR、NVIC 和目标 ISEL 状态用于进一步诊断。
5. 所有失败/取消路径均关闭已打开的 ICU 并恢复引脚；修正文件头原理图针号为 GPIO0=J1/J2-4、IRQ=J1/J2-2。保留单文件、单线程入口、无例程间调用的结构。

验证：RT-Thread Studio 构建 0 错误、0 警告；DAP-LINK 烧录成功并复位，记录见 `logs/pmod-irq-isolation-build.log`。17 项主机工具/独立例程结构检查通过，`git diff --check` 通过。构建与主机检查不替代板上边沿计数及资源恢复验收。

本轮未打开 COM8，修复后的板上验收仍待用户复测。先保持当前跳线运行 pmod-irq0；若 wire FAIL，再根据 pin 输出及已确认的插座映射核对另一组。只有接线与中断计数均通过才记录成功。

### Pmod IRQ0 修复复验通过，IRQ1 接线待复测（2026-10-01）

用户在固件 `6d80e4b` 上先运行 irq1、后运行 irq0，提供以下日志：

```text
msh >hmi_test pmod-irq1
TEST BEGIN pmod-irq1
msh >PMOD irq1 IRQ10 peer pin=0005 ISEL=1->0
PMOD irq1 GPIO0 pin=070A -> IRQ pin=0709, channel=10
PMOD irq1 wire set=0 output=0 input=1
PMOD irq1 wire FAIL: GPIO0 to IRQ; interrupt test not started
TEST RESULT pmod-irq1 FAIL code=-8 elapsed=23 ms
TEST IDLE

msh >hmi_test pmod-irq0
TEST BEGIN pmod-irq0
msh >PMOD irq0 IRQ11 peer pin=0006 ISEL=1->0
PMOD irq0 IRQ11 peer pin=0501 ISEL=0->0
PMOD irq0 GPIO0 pin=020B -> IRQ pin=0708, channel=11
PMOD irq0 wire set=0 output=0 input=0
PMOD irq0 wire set=1 output=1 input=1
PMOD irq0 wire set=0 output=0 input=0
PMOD irq0 wire set=1 output=1 input=1
PMOD irq0 wire set=0 output=0 input=0
PMOD irq0 wire PASS
PMOD irq0 IRQCR=32 NVIC=1 ISEL=1; BOTH_EDGE
PMOD irq0 GPIO->IRQ edges=16 expected=16 levels=16/16
TEST RESULT pmod-irq0 PASS code=0 elapsed=212 ms
TEST IDLE
```

结论：Pmod IRQ0（P211 输出 → P708 输入，IRQ11）修复复验通过。初始接线检查的五个电平全部匹配，随后 16 次电平读回及 16 个双边沿计数均正确，耗时 212 ms，正常退出。同通道按键 ISEL 隔离后的硬件结果支持此次配置修正有效，但不能由此断言所有初始化状态和恢复路径都已实测。

IRQ1（P710 输出 → P709 输入，IRQ10）本次未通过接线检查：输出脚实读为低，输入脚实读为高，耗时 23 ms，未开启边沿计数。不能记为 IRQ10 中断失效，也不能记为 IRQ1 已验收。如果两条命令之间未移动跳线，这与跳线实际连在 IRQ0 那组接口的情况一致；用户本条未明确说明是否移动，记录不作假定。

下一步断电，把 IRQ/IO0 跳线从已通过 IRQ0 的插座移到另一个插座相同方向、同排同列的对应两孔（即先前通过 pmod-spi1 的接口），确认仅连接 IRQ 与 IO0 后上电执行 `hmi_test pmod-irq1`。预期 wire PASS、edges=16、levels=16/16。此次仅归档用户日志和更新验收状态，未修改或烧录固件、未打开 COM8；文档经 `git diff --check` 检查后提交。

### Pmod IRQ1 修复复验通过与修改范围确认（2026-10-01）

用户提供新的 IRQ1 测试日志：

```text
msh >hmi_test pmod-irq1
TEST BEGIN pmod-irq1
msh >PMOD irq1 IRQ10 peer pin=0005 ISEL=1->0
PMOD irq1 GPIO0 pin=070A -> IRQ pin=0709, channel=10
PMOD irq1 wire set=0 output=0 input=0
PMOD irq1 wire set=1 output=1 input=1
PMOD irq1 wire set=0 output=0 input=0
PMOD irq1 wire set=1 output=1 input=1
PMOD irq1 wire set=0 output=0 input=0
PMOD irq1 wire PASS
PMOD irq1 IRQCR=32 NVIC=1 ISEL=1; BOTH_EDGE
PMOD irq1 GPIO->IRQ edges=16 expected=16 levels=16/16
TEST RESULT pmod-irq1 PASS code=0 elapsed=209 ms
TEST IDLE
```

结论：Pmod IRQ1（P710 输出 → P709 输入，IRQ10）修复复验通过。五步接线检查正确，16 次电平读回与 16 个双边沿计数均匹配，耗时 209 ms。结合此前 IRQ0 的 212 ms PASS，两路 Pmod 外部中断回环均完成验收，关闭 IRQ1 接线待复测项。

用户询问是否修改了其他用例代码。核对 Git 提交 `6d80e4b`，该次修复只改了两个 C 文件：`src/test/test-pmod-irq0.c`、`src/test/test-pmod-irq1.c`，以及 `doc/peripheral-tests.md`、本结果文档。两例程均存在同编号 IRQ 输入重复选择的问题，所以同步修复。未修改按键、SD、SPI 等其他例程代码，未修改公共 GPIO/ICU 驱动、FSP 生成配置、test-main 或 hal_entry。

运行期影响与代码修改范围需区分：irq0 临时清除 P006/P501 的 IRQ 选择位，irq1 临时清除 P005 的 IRQ 选择位；例程保存进入前的实际引脚配置并在正常、失败、取消退出路径恢复。此次日志验证了各路连通性和边沿计数，并非对所有恢复路径、其他外设功能的再次完整验收。本轮仅归档日志和说明修改范围，没有再次修改或烧录固件、没有打开 COM8；文档通过 `git diff --check` 后提交。

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
| `audio-mic` | PASS，8192帧DMA完成；后续用户已确认第一次安静、第二/三次发声且第三次最大，基本声学响应通过，未验证音质 |
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

## 当前验收状态汇总（已合并后续用户反馈）

| 状态 | 项目与原因 |
| --- | --- |
| GPIO 输入与 LED 已验收 | `gpio-inputs` 五次采样与逐键操作一致；`gpio-keys` 短按/长按记录见上文；用户已确认 `gpio-led` 三颗 LED 依次闪烁，视觉验收通过 |
| WAIT | `audio-tone`、`audio-replay`：播放采样数达到预期，声音内容/音质需试听 |
| 人工验收通过 | `lcd-colors`、`lcd-backlight`、画板与两个LVGL界面均有后续用户正常反馈，见各节日志 |
| 已复测通过 | `touch-points` 有1～5指及全部抬起记录；`touch-irq` 修复后计数78、PASS；结合画板/LVGL人工验收 |
| 人工可发现性与地址核对通过 | `rw007-adv`：手机发现 RW007-ED30；用户复核地址为 FC:58:4A:A6:ED:30，与模块日志一致，关闭此前第二字节差异 |
| WAIT | `adc-sample`：只报告悬空读数，不能证明精度 |
| 已复测通过 | `sd-info/read/file`、空闲拔插恢复均已通过，专用文件8192字节重挂载读回MATCH；不代表写入中拔卡安全 |
| USB收尾验收通过 | Windows枚举、文本、特殊字节、128字节跨包以及运行中物理拔插后回显恢复均有证据；拔插窗口mounted_events=2，rx/tx=8 |
| 人工验收通过 | `pmod-i2c`：基础图案与优化后全部动画正常；方块29.2 fps、立方体30.0 fps、字幕15.0 fps，总耗时60577 ms，用户明确验收通过 |
| 外接回环验收状态 | `can-bus` 仍待对端；旧 `pmod-irq0/irq1` 历史验收通过，现已替换为 `gpio-irq-rising/both`，新 IRQ12 接线亦已通过，分别 8/16 次中断、213/212 ms。ADC low/high、GPIO loop、Pmod SPI0/SPI1、Arduino SPI 已通过，见上方日志 |
| 已复测通过 | `rw007-internet`：此前未发现 Hotspot，返回 FAIL 并退出；2026-10-01 用户复测完成热点关联、DHCP、DNS、TCP、HTTP 200 与正文 MATCH，PASS，7756 ms。详见上方热点联网验收日志 |

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
