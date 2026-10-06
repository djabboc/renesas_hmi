"""把独立 audio-record 例程打印的 PCM 校验后转换为 WAV 和 MP3。

用户主动采集：python scripts/audio_record_to_mp3.py --port COM8 --output logs/mic-record-01
离线转换：python scripts/audio_record_to_mp3.py --log recording.txt --output logs/mic-record-01
默认不打开串口、不播放声音。MP3 编码由电脑端 FFmpeg 完成。
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import time
import wave
import zlib

from mp3_to_array import find_ffmpeg, run_ffmpeg


@dataclass(frozen=True)
class Recording:
    rate: int
    frames: int
    payload: bytes


def parse_recording(text: str) -> Recording:
    """严格核对偏移、完整长度和 CRC；损坏或混合的导出不能变成录音。"""
    header = None
    payload = bytearray()
    finished = False
    for line in text.splitlines():
        marker = line.find("MICREC ")
        if marker < 0:
            continue
        line = line[marker:].strip()
        if line.startswith("MICREC BEGIN"):
            if header is not None:
                raise ValueError("日志包含多次录音，请分别保存")
            match = re.fullmatch(
                r"MICREC BEGIN v=1 rate=(\d+) frames=(\d+) bits=16 channels=1 bytes=(\d+) crc32=([0-9A-Fa-f]{8})",
                line,
            )
            if match is None:
                raise ValueError("BEGIN 格式损坏")
            rate, frames, length = map(int, match.groups()[:3])
            if (rate, frames, length) != (8013, 40064, 80128):
                raise ValueError("不支持的录音参数")
            header = (rate, frames, length, int(match.group(4), 16))
        elif line.startswith("MICREC DATA"):
            if header is None or finished:
                raise ValueError("DATA 不在 BEGIN/END 之间")
            match = re.fullmatch(r"MICREC DATA (\d+) ([0-9A-Fa-f]+)", line)
            if match is None or len(match.group(2)) % 2 != 0:
                raise ValueError("DATA 格式损坏或被终端折行")
            offset = int(match.group(1))
            chunk = bytes.fromhex(match.group(2))
            remaining = header[2] - len(payload)
            if offset != len(payload) or remaining <= 0 or len(chunk) != min(32, remaining):
                raise ValueError(f"漏行、错序或重复：预期偏移 {len(payload)}，收到 {offset}")
            payload.extend(chunk)
        elif line.startswith("MICREC END"):
            if header is None or finished:
                raise ValueError("重复 END 或缺少 BEGIN")
            match = re.fullmatch(r"MICREC END bytes=(\d+) crc32=([0-9A-Fa-f]{8})", line)
            if match is None:
                raise ValueError("END 格式损坏")
            if (int(match.group(1)), int(match.group(2), 16)) != header[2:]:
                raise ValueError("BEGIN/END 不一致")
            if len(payload) != header[2] or zlib.crc32(payload) != header[3]:
                raise ValueError("录音长度或 CRC 校验失败")
            finished = True
        elif line.startswith("MICREC ABORT"):
            raise ValueError("板端导出已停止")
    if header is None or not finished:
        raise ValueError("缺少完整 BEGIN/DATA/END，不能生成可信录音")
    return Recording(header[0], header[1], bytes(payload))


def write_wav(path: Path, payload: bytes, rate: int) -> None:
    """WAV 头描述 PCM；原始版本的数据字节与板端打印完全一致。"""
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(payload)


def prepare_listening(payload: bytes) -> tuple[bytes, dict]:
    """试听副本只去整段均值和施加一次固定增益，没有降噪或动态 AGC。

    原始 WAV 始终保留。分别归一化的安静/说话录音不能用音量比较响应。
    """
    samples = list(struct.unpack(f"<{len(payload) // 2}h", payload))
    mean = sum(samples) / len(samples)
    centered = [sample - mean for sample in samples]
    peak = max(abs(sample) for sample in centered)
    gain = 0.0
    if peak > 0:
        gain = 12000 / peak
    listening = b"".join(struct.pack("<h", round(sample * gain)) for sample in centered)
    metrics = {
        "pcm16_minimum": min(samples),
        "pcm16_maximum": max(samples),
        "pcm16_mean_dc": mean,
        "pcm16_ac_rms": math.sqrt(sum(sample * sample for sample in centered) / len(samples)),
        "pcm16_near_full": sum(abs(sample) >= 32760 for sample in samples),
        "listening_fixed_gain": gain,
    }
    return listening, metrics


def save_outputs(recording: Recording, output: Path, ffmpeg: Path) -> dict:
    """电脑端编码 MP3；同时保留编码前的 PCM，避免有损音频成为唯一证据。"""
    output.parent.mkdir(parents=True, exist_ok=True)
    raw_wav = Path(str(output) + ".wav")
    listening_wav = Path(str(output) + ".listen.wav")
    listening, metrics = prepare_listening(recording.payload)
    write_wav(raw_wav, recording.payload, recording.rate)
    write_wav(listening_wav, listening, recording.rate)
    # 8013Hz 是实际采集速率的整数近似；MP3 编码使用标准 16kHz。
    # 重采样保留时长，不会补回原始约4kHz以上的频率内容。
    for source, destination in (
        (raw_wav, Path(str(output) + ".raw.mp3")),
        (listening_wav, Path(str(output) + ".mp3")),
    ):
        run_ffmpeg(ffmpeg, ["-i", str(source), "-vn", "-ac", "1", "-ar", "16000",
                           "-c:a", "libmp3lame", "-b:a", "48k", str(destination)])
    metrics.update({
        "rate": recording.rate,
        "frames": recording.frames,
        "duration_seconds": recording.frames / recording.rate,
        "pcm_crc32": f"{zlib.crc32(recording.payload):08X}",
        "board_processing": "PCM24 LEFT -> 127-tap 3kHz LPF -> decimate 6 -> divide 32 -> saturate PCM16",
        "listening_processing": "remove one whole-recording mean; apply one fixed gain; no noise reduction",
        "acceptance": "Pending: listener must recognize the spoken words; changing samples or RMS is insufficient",
    })
    Path(str(output) + ".json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    return metrics


def capture_log(port: str, path: Path, timeout: float = 90) -> str:
    """仅用户显式指定 --port 才采集；至多90秒，结束和异常均关闭串口。"""
    from serial_test_port import SerialPort
    from test_peripherals import wait_for_ready

    path.parent.mkdir(parents=True, exist_ok=True)
    chunks = []
    with path.open("wb") as log, SerialPort(port) as serial:
        def record(data: bytes) -> str:
            log.write(data)
            log.flush()
            text = data.decode("utf-8", errors="replace")
            chunks.append(text)
            return text

        wait_for_ready(serial, record)
        print("Ready: countdown 3s, then speak for all 6s (warmup 1s + saved recording 5s).", flush=True)
        serial.write(b"hmi_test audio-record\r")
        started = time.monotonic()
        output = ""
        pending = ""
        try:
            while time.monotonic() - started < timeout:
                text = record(serial.read())
                output += text
                pending += text
                lines = pending.split("\n")
                pending = lines.pop()
                for line in lines:
                    if "MICREC DATA " not in line:
                        print(line.rstrip(), flush=True)
                if "TEST IDLE" in output:
                    if "TEST RESULT audio-record WAIT" not in output:
                        raise RuntimeError("板端录音没有完成，请查看保存的日志")
                    return "".join(chunks)
                if any(marker in output for marker in ("TEST BUSY", "TEST UNKNOWN", "TEST ERROR", "HardFault", "assertion failed")):
                    raise RuntimeError("板端拒绝命令或发生异常，请查看日志")
            raise TimeoutError("录音导出超时，已请求停止")
        except BaseException:
            serial.write(b"hmi_test stop\r")
            raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--port", help="由用户主动指定串口，例如 COM8")
    inputs.add_argument("--log", type=Path, help="包含一次完整 MICREC 导出的串口文本")
    parser.add_argument("--output", required=True, type=Path, help="新的输出文件前缀")
    parser.add_argument("--ffmpeg", type=Path, help="可选 ffmpeg.exe 路径")
    args = parser.parse_args()
    # 在开启串口前核对编码器及输出名，避免录音后才发现环境不可用。
    ffmpeg = find_ffmpeg(args.ffmpeg)
    run_ffmpeg(ffmpeg, ["-f", "lavfi", "-i", "anullsrc=r=16000:cl=mono", "-t", "0.01",
                       "-c:a", "libmp3lame", "-f", "null", "-"])
    for suffix in (".serial.txt", ".wav", ".listen.wav", ".raw.mp3", ".mp3", ".json"):
        if Path(str(args.output) + suffix).exists():
            raise ValueError("输出前缀已存在，请换一个编号，避免覆盖之前的录音")
    if args.port:
        text = capture_log(args.port, Path(str(args.output) + ".serial.txt"))
    else:
        text = args.log.read_text(encoding="utf-8-sig", errors="replace")
    recording = parse_recording(text)
    metrics = save_outputs(recording, args.output, ffmpeg)
    print(json.dumps(metrics, indent=2))
    print(f"Saved: {args.output}.mp3 (listening), .raw.mp3, .wav, .listen.wav, .json")
    print("Listen for your spoken words. Noise alone does not mean microphone acceptance passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        raise SystemExit(f"ERROR: {error}")
