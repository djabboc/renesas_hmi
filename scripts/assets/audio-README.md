# 歌曲测试素材

`ode-to-joy.mp3` 用于 `src/test/test-audio-song.c`。它不是网络下载的商业歌曲录音。

- 旋律：Ludwig van Beethoven《欢乐颂》（第九交响曲主题）的短片段，原曲已进入公版。
- 本项目用 `scripts/mp3_to_array.py` 的 `make_demo()` 合成双乐句器乐旋律，160 拍/分钟，基频加两个谐波，无人声。
- 本项目新增的简单编配、合成声音及录音按 **CC0-1.0** 提供：https://creativecommons.org/publicdomain/zero/1.0/ 。代码保持其文件所标识的许可。
- 文件：16kHz、单声道、48kbit/s MP3；使用 FFmpeg/libmp3lame 编码。
- C 数组为 FFmpeg 解码后的 16 位 PCM，经去直流、低峰值归一化与首尾淡入淡出，沿用本录音的 CC0-1.0。

生成与转换命令：

```powershell
python scripts/mp3_to_array.py --make-demo scripts/assets/ode-to-joy.mp3 --update-c src/test/test-audio-song.c --preview-wav logs/audio-song-preview.wav
```

替换自己的录音后应更新素材说明；转换不会改变原录音许可。
