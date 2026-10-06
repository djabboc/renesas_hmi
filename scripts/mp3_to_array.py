#!/usr/bin/env python3
"""把 MP3 离线解码为 16kHz/单声道/16 位 PCM，嵌入独立歌曲例程。

Python 只使用标准库；MP3 编解码调用 FFmpeg，不访问开发板或串口。
  python scripts/mp3_to_array.py --make-demo scripts/assets/ode-to-joy.mp3
  python scripts/mp3_to_array.py music.mp3 --update-c src/test/test-audio-song.c
可用 --ffmpeg 指定 ffmpeg.exe；--preview-wav 保存板端数组的试听版本。
"""

import argparse
from array import array
import hashlib
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import wave


SAMPLE_RATE = 16000
MAX_SECONDS = 30
TARGET_PEAK = 3000
FADE_SAMPLES = SAMPLE_RATE // 50
METADATA_BEGIN = "/* BEGIN GENERATED SONG METADATA */"
METADATA_END = "/* END GENERATED SONG METADATA */"
DATA_BEGIN = "/* BEGIN GENERATED SONG PCM */"
DATA_END = "/* END GENERATED SONG PCM */"


def find_ffmpeg(explicit: Path | None = None) -> Path:
    """优先用显式路径或 PATH，其次寻找本机 Downloads 的 FFmpeg 解压目录。"""
    if explicit is not None:
        if not explicit.is_file():
            raise ValueError(f"FFmpeg 不存在：{explicit}")
        return explicit.resolve()
    installed = shutil.which("ffmpeg")
    if installed:
        return Path(installed)
    candidates = sorted((Path.home() / "Downloads").glob("ffmpeg*/bin/ffmpeg.exe"))
    if candidates:
        return candidates[-1]
    raise ValueError("请安装 FFmpeg 并加入 PATH，或用 --ffmpeg 指定 ffmpeg.exe")


def run_ffmpeg(executable: Path, arguments: list[str]) -> None:
    """按参数列表调用工具，不拼接 shell 命令；失败时保留解码诊断。"""
    subprocess.run([str(executable), "-hide_banner", "-loglevel", "error", "-y", *arguments],
                   check=True, timeout=120)


def write_wav(path: Path, samples: array) -> None:
    """WAV 与板端使用相同的有符号样本，文件字节序固定为小端。"""
    little_endian = array("h", samples)
    if sys.byteorder != "little":
        little_endian.byteswap()
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(little_endian.tobytes())


def make_demo(path: Path, ffmpeg: Path) -> None:
    """合成《欢乐颂》公版旋律片段；这是本项目合成录音，不是网络歌曲录音。"""
    phrase_one = [(64, 1), (64, 1), (65, 1), (67, 1), (67, 1), (65, 1),
                  (64, 1), (62, 1), (60, 1), (60, 1), (62, 1), (64, 1),
                  (64, 1.5), (62, 0.5), (62, 2)]
    phrase_two = phrase_one[:12] + [(62, 1.5), (60, 0.5), (60, 2)]
    samples = array("h", [0] * (SAMPLE_RATE // 10))
    for midi_note, beats in phrase_one + phrase_two:
        frequency = 440.0 * 2.0 ** ((midi_note - 69) / 12.0)
        count = round(SAMPLE_RATE * 0.375 * beats)  # 160 拍/分钟，32 拍约 12 秒。
        sounding = count - SAMPLE_RATE // 40
        for index in range(count):
            value = 0
            if index < sounding:
                seconds = index / SAMPLE_RATE
                attack = min(1.0, index / (SAMPLE_RATE * 0.008))
                release = min(1.0, (sounding - index) / (SAMPLE_RATE * 0.06))
                angle = 2.0 * math.pi * frequency * seconds
                # 三个谐波组成柔和器乐音色，包络让音符间平滑启停。
                tone = math.sin(angle) + 0.25 * math.sin(2 * angle) + 0.10 * math.sin(3 * angle)
                value = round(5000 * tone * attack * release * math.exp(-1.8 * seconds))
            samples.append(value)
    samples.extend([0] * (SAMPLE_RATE // 10))
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hmi-song-") as temporary:
        wav = Path(temporary) / "synthesized.wav"
        write_wav(wav, samples)
        run_ffmpeg(ffmpeg, ["-i", str(wav), "-c:a", "libmp3lame", "-b:a", "48k", str(path)])


def prepare_samples(samples: array) -> array:
    """去直流、归一化到低峰值、首尾淡入淡出；拒绝空输入和全静音。"""
    if not samples:
        raise ValueError("没有音频样本")
    if len(samples) > SAMPLE_RATE * MAX_SECONDS:
        raise ValueError("PCM 超过 30 秒；请用 --seconds 选择短片段")
    mean = round(sum(samples) / len(samples))
    peak = max(abs(sample - mean) for sample in samples)
    if peak == 0:
        raise ValueError("音频只有静音或直流，没有可播放内容")
    fade = min(FADE_SAMPLES, len(samples) // 2)
    output = array("h")
    for index, sample in enumerate(samples):
        value = round((sample - mean) * TARGET_PEAK / peak)
        if fade > 0:
            value = round(value * min(fade, index, len(samples) - 1 - index) / fade)
        output.append(value)
    return output


def decode_mp3(source: Path, ffmpeg: Path, start: float, seconds: float) -> array:
    """FFmpeg 解码、混合左右声道并重采样；读取 PCM WAV 后处理幅度。"""
    if not source.is_file() or source.suffix.lower() != ".mp3":
        raise ValueError("输入必须是存在的 .mp3 文件")
    if not math.isfinite(start) or start < 0:
        raise ValueError("start 必须是非负的有限秒数")
    if not math.isfinite(seconds) or not 0 < seconds <= MAX_SECONDS:
        raise ValueError("seconds 必须大于 0 且不超过 30")
    with tempfile.TemporaryDirectory(prefix="hmi-mp3-") as temporary:
        wav = Path(temporary) / "decoded.wav"
        run_ffmpeg(ffmpeg, ["-ss", str(start), "-i", str(source), "-t", str(seconds),
                           "-vn", "-ac", "1", "-ar", str(SAMPLE_RATE), "-c:a", "pcm_s16le", str(wav)])
        with wave.open(str(wav), "rb") as decoded:
            if (decoded.getnchannels(), decoded.getsampwidth(), decoded.getframerate()) != (1, 2, SAMPLE_RATE):
                raise ValueError("FFmpeg 输出的 PCM 格式不符合例程")
            samples = array("h", decoded.readframes(decoded.getnframes()))
        if sys.byteorder != "little":
            samples.byteswap()
    return prepare_samples(samples)


def render_blocks(samples: array, source: Path) -> tuple[str, str]:
    """输出数据与元信息两块；播放逻辑留在 C 文件前部，长数组留在末尾。"""
    metadata = "\n".join([
        METADATA_BEGIN,
        "/* 由 scripts/mp3_to_array.py 生成；数组保存解码后的 PCM，不是 MP3 压缩字节。",
        f" * 输入 MP3 SHA256: {hashlib.sha256(source.read_bytes()).hexdigest()} */",
        f"#define SONG_SAMPLE_RATE {SAMPLE_RATE}u",
        f"#define SONG_SAMPLE_COUNT {len(samples)}u",
        f"#define SONG_PCM_PEAK {max(abs(sample) for sample in samples)}u",
        METADATA_END,
    ])
    lines = [DATA_BEGIN, "/* 自动生成的低幅单声道样本；static const 使数据留在 Flash。 */",
             "static const int16_t song_pcm[SONG_SAMPLE_COUNT] =", "{"]
    for offset in range(0, len(samples), 16):
        lines.append("    " + ", ".join(str(value) for value in samples[offset:offset + 16]) + ",")
    lines.extend(["};", DATA_END])
    return metadata, "\n".join(lines)


def update_c(path: Path, metadata: str, data: str) -> None:
    """同时校验两个唯一标记，再一次性更新；失败时不损坏原 C 文件。"""
    original = path.read_text(encoding="utf-8")
    updated = original
    for begin, end, replacement in [(METADATA_BEGIN, METADATA_END, metadata),
                                    (DATA_BEGIN, DATA_END, data)]:
        if updated.count(begin) != 1 or updated.count(end) != 1:
            raise ValueError(f"C 文件应恰有一对生成标记：{begin}")
        pattern = re.escape(begin) + r".*?" + re.escape(end)
        updated, count = re.subn(pattern, lambda match: replacement, updated, flags=re.S)
        if count != 1:
            raise ValueError("生成标记顺序错误，未更新 C 文件")
    path.write_text(updated, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--make-demo", type=Path, help="生成本项目合成的欢乐颂 MP3")
    parser.add_argument("--update-c", type=Path)
    parser.add_argument("--preview-wav", type=Path)
    parser.add_argument("--start", type=float, default=0)
    parser.add_argument("--seconds", type=float, default=15)
    args = parser.parse_args()
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if args.make_demo is not None:
        make_demo(args.make_demo, ffmpeg)
        print(f"Demo MP3: {args.make_demo}")
    source = args.source
    if source is None:
        source = args.make_demo
    if source is None or args.update_c is None:
        if args.make_demo is not None and args.update_c is None and args.source is None:
            return 0
        parser.error("转换时需要 source 和 --update-c")
    samples = decode_mp3(source, ffmpeg, args.start, args.seconds)
    metadata, data = render_blocks(samples, source)
    update_c(args.update_c, metadata, data)
    if args.preview_wav is not None:
        write_wav(args.preview_wav, samples)
    print(f"PCM: {len(samples)} samples, {len(samples) / SAMPLE_RATE:.3f}s, "
          f"{len(samples) * 2} Flash bytes, peak={max(abs(sample) for sample in samples)}")
    print(f"Updated: {args.update_c}; re-build and flash before testing")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
