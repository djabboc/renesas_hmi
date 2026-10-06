# 原始麦克风诊断：audio-raw

2026-10-07实测更新：安静/发声两轮采集和导出完整、CRC通过。发声轮总RMS增加4.83dB，但100～8000Hz频带仅增加0.215dB，仍无清晰语音响应；优先排查拾音/麦克风/I²S实际输入，详见[audio-raw-results.md](audio-raw-results.md)。原始导出流程已验证，语音未验收通过。

日期：2026-10-07。目的：确认稳定阶段的SSI输入是否包含人声，再决定排查麦克风/I²S还是后处理/回放。此前audio-replay v7已经完整录5秒/播5秒、overrun=0，但持续说话仅有沙沙声。新增例程是独立诊断，不把数字变化或导出成功判定为语音验收通过。

## 结构与采集内容

源码`src/test/test-audio-raw.c`，唯一线程入口`test_audio_raw_thread`；唯一命令由`src/test-main.c`注册为`hmi_test audio-raw`。测试文件没有MSH内容、没有创建线程、没有调用其他例程。与现有测试互斥运行，上电不自动启动。

保持与audio-replay v7相同的麦克风工作点：GPT1周期39、120MHz时钟，SSI主机24位PCM/32位时隙、右对齐、双声道、4字节DTC；有效原始帧率约48076.923Hz。连续采集240384帧约5秒，保存从帧144231开始的24000个左声道样本，即约3.000～3.499秒、约半秒的稳定片段。片段足以观察持续元音和频谱，不适合判断完整句子。

保存位置在高通、FIR、mu-law、自动增益之前。仅将SSI每个32位字的低24个有效位按小端拆成三个字节，所有24位均保留；不滤波、不降采样、不压缩、不限幅。前8个选中帧同时打印原始L32/R32及符号扩展后的L24；逐秒统计原始幅度、右声道峰值和左字高8位非零次数。高8位统计用于观察填充/符号扩展现象，非零不自动判定为错误。

原始片段申请72008字节含哨兵，两个DTC接收缓冲申请6160字节含哨兵，合计78168字节；结束或错误退出全部释放，没有永久静态大录音数组。原始复制很短，双缓冲满时仍报错，不覆盖未消费块；采集阶段线程优先级临时14，中断完成后信号量唤醒线程，退出恢复原优先级并释放信号量。保留块处理/等待时间和队列峰值日志，用实物确认余量。

采集时扬声器保持静音，不播放参考音；采集硬件关闭后才打印片段。导出采用32字节/行的十六进制数据、字节偏移和标准CRC-32/IEEE，典型2250行；BEGIN/END附带总长度和CRC。所有行小于控制台128字节缓冲。115200波特率导出通常约17～20秒，整个例程约30秒。请不要在导出期间输入其他命令；`hmi_test stop`仍可协作取消，取消输出ABORT，不生成完整录音。

## 推荐操作：由你运行采集脚本

助手没有打开COM8。请关闭串口助手对COM8的占用，在项目根目录终端依次执行以下两轮；每轮脚本完成后自动释放串口，异常也释放，单轮上限90秒。

第一轮安静：

```powershell
python scripts/audio_raw_to_wav.py --port COM8 --output logs/mic-quiet
```

脚本先确认板卡ready=1/busy=0，再发送一次`hmi_test audio-raw`。看到3秒倒计时后保持安静；`MIC RECORD NOW`到`MIC RECORD DONE`为5秒录音。随后只需等待导出结束和文件生成。

第二轮持续发“啊——”：

```powershell
python scripts/audio_raw_to_wav.py --port COM8 --output logs/mic-voice
```

看到`MIC RECORD NOW`后，对着板载麦克风拾音孔、距离5～10cm持续发声全部5秒；`MIC RECORD DONE`后停止。因为只保存第4秒的半秒，短促发声可能错过片段。

脚本默认不打开串口，只有你明确传入`--port`时才打开。不会自动播放WAV。若COM8被占用，Windows拒绝打开，测试命令不会发送。

## 外部1kHz参考音测试

`--output`只指定文件名，不会播放声音。参考音必须先由电脑或手机的外部扬声器实际播放，不能仅执行采集命令。

1. 打开`logs/mic-reference-1000hz.wav`，在播放器中启用循环播放。文件长12秒，是连续的1kHz提示音；保持适中的音量。
2. 将外部扬声器放在U9麦克风拾音孔附近约5～10cm，确认在板旁能清楚听到该音，期间不说话。
3. 关闭占用COM8的串口助手，在项目根目录执行下面的命令。保持参考音播放，覆盖倒计时和`MIC RECORD NOW`到`MIC RECORD DONE`的全部5秒。

```powershell
python scripts/audio_raw_to_wav.py --port COM8 --output logs/mic-reference-test-01
```

4. 出现`MIC RECORD DONE`后停止外部参考音，继续等待约20秒导出结束。生成WAV/JSON/PNG后脚本释放COM8。
5. 告诉助手采集完成，并确认参考音在上述5秒内持续可听。助手直接读取本机输出，检查CRC、原始频谱与安静轮的1kHz附近差别。

复测时将末尾编号改为02、03，避免覆盖前一次记录。仅看到“采集完成”或文件名含tone不足以判定录到了参考音；缺少明显峰也不能单独认定麦克风损坏。

## 另一种操作：自己保存终端日志

在115200、8N1的串口助手中，分别保存安静和发声两次完整文本日志。每轮执行：

```text
hmi_test audio-raw
```

从倒计时到TEST IDLE的全部输出都应保存，包括MICRAW BEGIN、2250行MICRAW DATA、MICRAW END。不要只复制终端可见窗口，也不要让终端把长行实际拆成多行。然后离线转换：

```powershell
python scripts/audio_raw_to_wav.py --log quiet.txt --output logs/mic-quiet
python scripts/audio_raw_to_wav.py --log voice.txt --output logs/mic-voice
```

离线模式完全不打开串口。脚本检查单次导出、行顺序/偏移、完整长度、帧数、BEGIN/END一致性及CRC；漏行、重复、损坏、取消和混合多次录音均报错，不能生成可信录音。

## 输出文件与听音

- `.serial.txt`：主动串口采集模式保存的完整原始日志。
- `.raw24.wav`：48077Hz、单声道、24位、原始72000字节完全保留，没有去直流或增加音量；弱输入可能非常轻。
- `.listen.wav`：电脑端只去整段均值，并使用一次固定增益使峰值为12000，转换为16位。没有滤波、降噪或动态增益；会同时放大底噪。该副本用于试听，不代替原始数据。
- `.json`：原始最小/最大值、直流均值、交流RMS/峰值、RMS dBFS、变化数、近满幅数、试听副本增益、CRC等。
- `.png`：完整原始波形（含直流）和频谱（去均值后Hann窗），横轴到奈奎斯特频率约24kHz；不是人声自动识别结果。

WAV整数采样率48077与实际48076.923相差约1.6ppm，对半秒诊断可忽略。原始文件保留原始字节，只有试听副本做了上述处理。对比两轮的原始RMS/频谱和实际听感，不能只比较分别归一化后的试听音量。

在本机，绘图依赖安装于项目忽略目录`logs/audio-raw-python`，不修改全局Python环境。其他电脑可执行：

```powershell
python -m pip install --target logs/audio-raw-python matplotlib
```

没有绘图依赖时，加`--no-plot`仍可使用纯标准库生成WAV/JSON。脚本在开启串口前检查依赖，避免开始录音后才发现缺少绘图库。

## 如何继续定位

先听`.listen.wav`，保留`.raw24.wav`作为原始证据：如果电脑端能分辨发“啊”，板端回放仍只有噪声，转查后处理/板端播放；如果电脑端同样只有噪声，且安静/发声的原始波形与频谱无可重复差别，重点核对U9拾音孔、供电、SCK/WS/SD和SSI位对齐。这一步仍不能仅凭底噪判定麦克风损坏。

请提供两轮完整`.serial.txt`（或手工终端日志）、`.json`、`.png`和试听反馈；原始WAV也应保留，便于进一步分析。不要把2250行数据粘贴到聊天里，发送文件即可。

## 验证记录

8组真实C函数ARM模拟检查通过：连续240384帧时长、选中24000帧的全部24位字节与顺序、CRC已知向量、逐行偏移/导出内容、关闭硬件后导出、动态分配/哨兵、取消/超时/队列溢出/Read和启动失败、IPC与优先级恢复。桩不模拟真实麦克风声学响应。

38项主机检查通过，新增协议损坏/丢行/错序/截断/多次录音拒绝、24位正负端点、原始WAV字节和格式、试听副本与直流/RMS、有限串口采集和失败/超时释放。原audio-replay另有15组ARM回归通过。已用带9000直流偏置及1kHz/3kHz分量的合成PCM验证完整日志→CRC→WAV/JSON/PNG流程，原始WAV保留原字节，频谱两处峰值和原始波形偏置均正确；日志`logs/audio-raw-conversion-plot.log`。合成输入不是真实板上录音。

Studio构建0 errors/0 warnings，Flash1162152字节、静态RAM542616字节。DAP-LINK/PyOCD已成功烧录1162256字节、退出码0，记录`logs/audio-raw-flash.log`。现有audio-replay和hal_entry源文件未修改。记录`logs/audio-raw-arm.log`、`logs/audio-raw-host-tests.log`、`logs/audio-raw-replay-regression.log`和`logs/audio-raw-build.log`。实物原始采集、导出时序和语音内容待用户测试。
