"""校验 audio-raw 串口导出，生成原始24位WAV、试听副本和分析结果。

离线：python scripts/audio_raw_to_wav.py --log voice.txt --output logs/mic-voice
用户主动采集：python scripts/audio_raw_to_wav.py --port COM8 --output logs/mic-voice
默认不打开串口；使用 --port 时至多占用90秒，结束/异常必定释放。
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import struct
import time
import wave
import zlib


@dataclass(frozen=True)
class RawRecording:
    rate: int
    frames: int
    start: int
    payload: bytes


def parse_recording(text: str) -> RawRecording:
    """严格检查单次导出：按序偏移、总长度、帧数、BEGIN/END与CRC均一致。"""
    header = None
    payload = bytearray()
    finished = False
    for line in text.splitlines():
        marker = line.find("MICRAW ")
        if marker < 0:
            continue
        line = line[marker:].strip()
        if line.startswith("MICRAW BEGIN"):
            if header is not None:
                raise ValueError("日志包含多次导出，请每次录音保存独立日志")
            match = re.fullmatch(
                r"MICRAW BEGIN v=1 rate=(\d+) frames=(\d+) start=(\d+) bytes=(\d+) crc32=([0-9A-Fa-f]{8})",
                line,
            )
            if match is None:
                raise ValueError("BEGIN格式不完整")
            rate, frames, start, length = map(int, match.groups()[:4])
            if rate != 48077 or frames != 24000 or start != 144231 or length != frames * 3:
                raise ValueError("不支持的采集参数，预期v1原始24位单声道片段")
            header = (rate, frames, start, length, int(match.group(5), 16))
        elif line.startswith("MICRAW DATA"):
            if header is None or finished:
                raise ValueError("DATA不在BEGIN与END之间")
            match = re.fullmatch(r"MICRAW DATA (\d+) ([0-9A-Fa-f]+)", line)
            if match is None or len(match.group(2)) % 2 != 0:
                raise ValueError("DATA行损坏或被终端折行")
            offset = int(match.group(1))
            chunk = bytes.fromhex(match.group(2))
            remaining = header[3] - len(payload)
            if offset != len(payload) or remaining <= 0 or len(chunk) != min(32, remaining):
                raise ValueError(f"DATA漏行、重复或错序：预期偏移{len(payload)}，收到{offset}")
            payload.extend(chunk)
        elif line.startswith("MICRAW END"):
            if header is None or finished:
                raise ValueError("重复END或缺少BEGIN")
            match = re.fullmatch(r"MICRAW END bytes=(\d+) crc32=([0-9A-Fa-f]{8})", line)
            if match is None:
                raise ValueError("END格式不完整")
            if int(match.group(1)) != header[3] or int(match.group(2), 16) != header[4]:
                raise ValueError("BEGIN/END长度或CRC不一致")
            if len(payload) != header[3] or zlib.crc32(payload) != header[4]:
                raise ValueError("数据长度或CRC校验失败，不能生成可信录音")
            finished = True
        elif line.startswith("MICRAW ABORT"):
            raise ValueError("板端导出被取消")
    if header is None or not finished:
        raise ValueError("缺少完整的MICRAW BEGIN/DATA/END，请保存全量日志")
    return RawRecording(header[0], header[1], header[2], bytes(payload))


def decode_pcm24(payload: bytes) -> list[int]:
    """小端3字节二进制补码；保留24位精度，避免先转16位丢失弱信号。"""
    if len(payload) % 3 != 0:
        raise ValueError("PCM24长度必须是3字节的整数倍")
    result = []
    for offset in range(0, len(payload), 3):
        value = int.from_bytes(payload[offset:offset + 3], "little")
        if value & 0x800000:
            value -= 0x1000000
        result.append(value)
    return result


def analyze_samples(samples: list[int]) -> dict:
    """直流与交流分开统计；RMS反映幅度，不能自动证明存在人声。"""
    mean = sum(samples) / len(samples)
    centered = [value - mean for value in samples]
    rms = math.sqrt(sum(value * value for value in centered) / len(centered))
    peak = max(abs(value) for value in centered)
    dbfs = None
    if rms > 0:
        dbfs = 20 * math.log10(rms / 8388608)
    return {
        "minimum": min(samples), "maximum": max(samples), "mean_dc": mean,
        "ac_rms": rms, "ac_peak": peak, "ac_rms_dbfs": dbfs,
        "changed": sum(a != b for a, b in zip(samples, samples[1:])),
        "near_full": sum(abs(value) >= 8386560 for value in samples),
    }


def listening_copy(samples: list[int]) -> tuple[bytes, float]:
    """仅电脑端去均值、整段固定增益；不做滤波、降噪或自动动态增益。

    试听副本会同时放大底噪；原始WAV始终保存原字节，分析以原始数据为准。
    """
    mean = sum(samples) / len(samples)
    peak = max(abs(value - mean) for value in samples)
    gain = 0.0
    if peak > 0:
        gain = 12000 / peak
    values = [round((value - mean) * gain) for value in samples]
    return b"".join(struct.pack("<h", value) for value in values), gain


def write_wav(path: Path, payload: bytes, rate: int, width: int) -> None:
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(width)
        stream.setframerate(rate)
        stream.writeframes(payload)


def plot_recording(samples: list[int], rate: int, path: Path) -> None:
    """波形含原始直流；频谱去均值后加Hann窗，幅度参考24位满幅。

    显示频谱只是定位工具，不能仅凭一个峰值判定录到了人声。
    """
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    raw = np.asarray(samples, dtype=np.float64)
    centered = raw - raw.mean()
    window = np.hanning(len(raw))
    spectrum = np.abs(np.fft.rfft(centered * window)) * 2 / window.sum()
    spectrum[0] /= 2
    spectrum[-1] /= 2
    db = 20 * np.log10(np.maximum(spectrum / 8388608, 1e-12))
    frequencies = np.fft.rfftfreq(len(raw), 1 / rate)
    figure, axes = plt.subplots(2, 1, figsize=(10, 6), constrained_layout=True)
    # 全部24000个样本均参与绘图，不用抽点隐藏高频噪声。
    axes[0].plot(np.arange(len(raw)) / rate, raw, linewidth=0.5)
    axes[0].set(xlabel="Time (s)", ylabel="Original PCM24", title="Raw microphone waveform (DC retained)")
    axes[1].plot(frequencies, db, linewidth=0.7)
    axes[1].set(xlabel="Frequency (Hz)", ylabel="Amplitude (dBFS)", xlim=(0, rate / 2), ylim=(-140, 0),
                title="Spectrum (DC removed, Hann window)")
    for axis in axes:
        axis.grid(alpha=0.25)
    figure.savefig(path, dpi=150)
    plt.close(figure)


def save_outputs(recording: RawRecording, output: Path, plot: bool) -> dict:
    output.parent.mkdir(parents=True, exist_ok=True)
    samples = decode_pcm24(recording.payload)
    metrics = analyze_samples(samples)
    listen, gain = listening_copy(samples)
    metrics.update({"rate": recording.rate, "frames": recording.frames, "start_frame": recording.start,
                    "duration_seconds": recording.frames / recording.rate, "listen_gain": gain,
                    "crc32": f"{zlib.crc32(recording.payload):08X}"})
    write_wav(Path(str(output) + ".raw24.wav"), recording.payload, recording.rate, 3)
    write_wav(Path(str(output) + ".listen.wav"), listen, recording.rate, 2)
    Path(str(output) + ".json").write_text(json.dumps(metrics, indent=2, allow_nan=False), encoding="utf-8")
    if plot:
        plot_recording(samples, recording.rate, Path(str(output) + ".png"))
    return metrics


def capture_log(port: str, path: Path, timeout: float = 90) -> str:
    """仅用户指定--port时短暂打开串口；保存原始日志，退出由with保证释放。"""
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
            # 避免把数千行原始数据刷屏，控制阶段提示仍显示。
            return text

        wait_for_ready(serial, record)
        print("Ready: 3-second countdown, then keep quiet OR sustain AH for 5 seconds.", flush=True)
        serial.write(b"hmi_test audio-raw\r")
        started = time.monotonic()
        output = ""
        pending = ""
        try:
            while time.monotonic() - started < timeout:
                data = serial.read()
                text = record(data)
                output += text
                pending += text
                lines = pending.split("\n")
                pending = lines.pop()
                for line in lines:
                    if "MICRAW DATA " not in line:
                        print(line.rstrip(), flush=True)
                if "TEST IDLE" in output:
                    if "TEST RESULT audio-raw WAIT" not in output:
                        raise RuntimeError("audio-raw未完成导出，请查看保存的串口日志")
                    return "".join(chunks)
                if any(marker in output for marker in ("TEST BUSY", "TEST UNKNOWN", "TEST ERROR", "HardFault", "assertion failed")):
                    raise RuntimeError("板端拒绝测试或发生异常，请查看日志")
            raise TimeoutError("audio-raw超时，已请求停止")
        except BaseException:
            serial.write(b"hmi_test stop\r")
            raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--log", type=Path, help="包含一次完整导出的终端文本日志")
    inputs.add_argument("--port", help="由用户主动指定的Windows串口，例如COM8")
    parser.add_argument("--output", required=True, type=Path, help="输出文件前缀，例如logs/mic-voice")
    parser.add_argument("--no-plot", action="store_true", help="仅输出WAV/JSON，不需要matplotlib")
    args = parser.parse_args()
    if not args.no_plot:
        # 项目内可选依赖，不修改全局Python环境。
        import sys
        dependencies = Path(__file__).resolve().parents[1] / "logs/audio-raw-python"
        if dependencies.is_dir():
            sys.path.insert(0, str(dependencies))
        try:
            import matplotlib  # noqa: F401
            import numpy  # noqa: F401
        except ImportError:
            parser.error("绘图需要matplotlib：python -m pip install --target logs/audio-raw-python matplotlib；或加--no-plot")
    if args.port:
        text = capture_log(args.port, Path(str(args.output) + ".serial.txt"))
    else:
        text = args.log.read_text(encoding="utf-8-sig", errors="replace")
    recording = parse_recording(text)
    metrics = save_outputs(recording, args.output, not args.no_plot)
    print(json.dumps(metrics, indent=2, allow_nan=False))
    print(f"Saved: {args.output}.raw24.wav / .listen.wav / .json" + (" / .png" if not args.no_plot else ""))
    print("Listening copy amplifies noise too; compare QUIET and VOICE recordings before drawing conclusions.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, RuntimeError, TimeoutError) as error:
        raise SystemExit(f"ERROR: {error}")
