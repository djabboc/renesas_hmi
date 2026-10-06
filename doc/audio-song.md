# 单文件歌曲播放：audio-song

日期：2026-10-06。状态：软件自测、编译、烧录及用户实物试听验收通过。录音回放 `audio-replay` 按用户要求暂停。

## 如何测试

JBL 4Ω C11R 已接 J8，沿用提示音验收时的接线。打开调试串口 COM8，输入：

```text
hmi_test audio-song
```

应该听到一遍约 12.23 秒的《欢乐颂》器乐旋律，随后自动静音。不是录音回放，没有倒计时或说话要求。再次输入同一条命令会从头播放；运行中可输入 `hmi_test stop` 提前结束。

用户在固件 `6d92ec9` 上手动运行，实际日志如下：

```text
msh >hmi_test audio-song
TEST BEGIN audio-song
msh >SONG PCM mono rate=16000 samples=195729 bytes=391458 peak=3000 duration=12233 ms
SONG playing once on J8; PWM=80kHz, source=offline decoded MP3
SONG output samples=195729/195729 pwm_error=0; listening confirmation required
TEST RESULT audio-song WAIT code=1 elapsed=12258 ms
TEST IDLE
```

用户确认“声音连续、正常”，本例程验收通过。195729/195729 样本完成、pwm_error=0，12258ms 后返回 WAIT/IDLE。WAIT 是固件对需要人工试听的例程保留的结果，不会因文档验收改为 PASS。提前停止显示 FAIL/code=-9，是主动取消结果，不等于硬件故障。

## MP3 在哪里解码

流程：`MP3 → 电脑 FFmpeg 解码/混成单声道/重采样 → Python 去直流、限幅和淡入淡出 → C 的 PCM 数组 → GPT2 逐样本更新 GPT6 PWM → J8`。

**本例程不在开发板上解码 MP3。** MP3 是输入素材；C 数组保存有符号 16 位 PCM，不是压缩 MP3 字节。RA6M3 的 JPEG 解码外设不解码 MP3；这个方案没有增加 MP3 解码库，也不依赖 SD 卡、USB 或网络。它验证现有喇叭输出链路能播放一段真实音频波形。

内置素材为 `scripts/assets/ode-to-joy.mp3`：由脚本合成的贝多芬《欢乐颂》公版旋律片段，无人声，不使用网络上的商业录音。编曲/合成音色与录音由本项目生成，以 CC0-1.0 提供，详见 `scripts/assets/audio-README.md`。MP3 编码/解码的填充使最终样本时长略长于 12.2 秒。

## 怎样阅读代码

`src/test/test-audio-song.c` 是本工程中的独立单文件例程，使用已有 RT-Thread/FSP BSP 编译，不是脱离 BSP 的完整工程。唯一公开入口是 `test_audio_song_thread(void *argument)`。测试内没有 MSH、自动启动导出或线程创建；`src/test-main.c` 负责注册 `audio-song` 命令并创建线程，禁止与其他测试重叠。

先读线程入口，再读 `run_test()` 和 `audio_tick()`。文件末尾是长数组，初学时可以跳过。代码旁的注释解释各步骤：

1. 检查 GPT 时钟、PCM 元信息；只恢复 P702/P703 的 PWM 引脚复用。
2. 从 `g_timer6_cfg` 和 `g_timer2_cfg` 复制局部配置，不修改 FSP 生成的全局配置。
3. GPT6 以 120MHz/1500=80kHz 输出两路 PWM，GPT2 以 120MHz/7500=16kHz 触发采样中断。
4. 每个中断从 Flash 取一个样本，两路占空比围绕 50% 反向变化；样本再次限幅至 ±3000。到数组结尾不再读取，改为两路中点。
5. 线程等待完成，并检查停止事件、PWM 返回错误和有限超时；依次关闭 GPT2、GPT6，最后将两控制脚设低。

数组使用 `static const int16_t`，占 Flash 391458 字节，不申请整首歌的 RAM 缓冲。静态播放状态只有位置和错误码；线程使用总入口已有的 12KB 栈。播放时长约 12.233 秒，等待上限为时长加 2 秒，超时会清理退出。默认歌曲已通过实物连续播放与试听验收。

## 替换为自己的 MP3

Python 只使用标准库，额外需要 FFmpeg。脚本优先使用 `--ffmpeg`、PATH，随后寻找用户 Downloads 中 `ffmpeg*/bin/ffmpeg.exe`。在工程根目录执行：

```powershell
python scripts/mp3_to_array.py "C:/Music/example.mp3" --start 0 --seconds 12 --update-c src/test/test-audio-song.c --preview-wav logs/my-song.wav
python scripts/build_flash.py
```

工具路径不在上述位置时，附加 `--ffmpeg "C:/Tools/ffmpeg/bin/ffmpeg.exe"`。

转换固定输出 16kHz、单声道、有符号 16 位 PCM，去均值后将峰值归一化至 3000，首尾各最多 20ms 淡入淡出。输入空音频或全静音会拒绝。默认取开头 15 秒，最多允许 30 秒；每秒占约 32KB Flash。生成标记内的数组和元信息一起更新，其余播放代码保留。预览 WAV 与板端数组完全相同，可先在电脑试听，电脑试听音量不等于 J8 音量。

脚本只转换，不自动编译、烧录、打开串口。更换歌曲后必须重新构建烧录，Flash 超限应缩短片段。不要手动提高代码限幅来调大音量；本例保持低幅测试。自己的素材许可由输入素材决定，不因转换而改变。

重新生成默认 MP3 和数组：

```powershell
python scripts/mp3_to_array.py --make-demo scripts/assets/ode-to-joy.mp3 --update-c src/test/test-audio-song.c --preview-wav logs/audio-song-preview.wav
```

## 已完成的自测与验收边界

- 30 项主机回归通过，其中 7 项歌曲转换检查覆盖去直流/幅度/静音拒绝/片段范围/文件更新边界、真实 MP3 解码与嵌入数组一致性、WAV 格式；独立过零计数确认首音 E4 的频率范围。
- `scripts/validate_audio_song.py` 提取实际 C 函数，以 Studio ARM GCC 编译后在 ARM 模拟器执行。5 组检查覆盖正负 PCM/PWM、EOF、正常完成、取消/超时、PWM 错误、四个启动失败位置及清理、时钟错误拒绝播放。硬件调用使用桩，不能替代实物播放。
- RT-Thread Studio 构建：0 errors、0 warnings；Flash 1151748 字节，静态 RAM 529416 字节。日志：`logs/audio-song-build.log`。
- DAP-LINK/PyOCD 烧录成功并复位；写入 1151760 字节，记录 `logs/audio-song-flash.log`。ELF 符号表确认 `song_pcm` 的 391458 字节位于 Flash 的 `.text` 区，不占歌曲 RAM 缓冲。
- `src/hal_entry.c` 原始哈希检查通过；现在共有 46 个独立例程。
- 用户手动运行默认歌曲并确认“声音连续、正常”；日志显示完整样本数、零 PWM 错误、正常返回 IDLE。助手未打开 COM8。本次覆盖默认片段的一次完整播放，不扩展为长期播放或任意素材音质验证。

本地 ARM 验证可复用已有环境：

```powershell
python -m unittest discover -s scripts/tests -v
python scripts/validate_audio_song.py --dependencies logs/oled-validation/python
```

ARM 验证需要 `unicorn`、`pyelftools` 和 Studio 的 ARM GCC；它们不是转换脚本或固件的运行依赖。
