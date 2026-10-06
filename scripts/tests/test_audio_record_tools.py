"""用合成PCM、内存串口和真实FFmpeg验证录音工具，不占用COM8。"""

from contextlib import redirect_stdout
import io
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import audio_record_to_mp3 as tool
from test_audio_raw_tools import FakePort


def make_export(samples=None):
    if samples is None:
        samples = [-32768, -1, 0, 1] * 10016
    payload = struct.pack(f"<{len(samples)}h", *samples)
    checksum = zlib.crc32(payload)
    lines = [f"MICREC BEGIN v=1 rate=8013 frames=40064 bits=16 channels=1 bytes=80128 crc32={checksum:08X}"]
    for offset in range(0, len(payload), 32):
        lines.append(f"MICREC DATA {offset} {payload[offset:offset + 32].hex().upper()}")
    lines.append(f"MICREC END bytes=80128 crc32={checksum:08X}")
    return "\n".join(lines), payload


class RecordToolsTests(unittest.TestCase):
    def test_complete_recording_keeps_all_bytes_and_signed_samples(self):
        text, payload = make_export()
        recording = tool.parse_recording("msh >" + text)
        self.assertEqual(recording.payload, payload)
        self.assertEqual((recording.rate, recording.frames), (8013, 40064))
        self.assertEqual(struct.unpack("<4h", recording.payload[:8]), (-32768, -1, 0, 1))

    def test_bad_crc_and_bad_parameters_rejected(self):
        text = make_export()[0]
        for damaged in (
            text.replace("0080", "0180", 1),
            text.replace("rate=8013", "rate=8000", 1),
            text.replace("channels=1", "channels=2", 1),
            text.replace("frames=40064", "frames=40063", 1),
            text.replace("bytes=80128", "bytes=80126", 1),
        ):
            with self.subTest(damaged=damaged[:100]), self.assertRaises(ValueError):
                tool.parse_recording(damaged)

    def test_missing_reordered_duplicate_and_truncated_rows_rejected(self):
        lines = make_export()[0].splitlines()
        for variant in (
            lines[:4] + lines[5:],
            lines[:2] + [lines[3], lines[2]] + lines[4:],
            lines[:4] + [lines[3]] + lines[4:],
            lines[:-1],
            lines[:2] + ["MICREC DATA 32 Z0"] + lines[3:],
        ):
            with self.subTest(lines=variant[:4]), self.assertRaises(ValueError):
                tool.parse_recording("\n".join(variant))

    def test_abort_mixed_recordings_and_trailing_data_rejected(self):
        text = make_export()[0]
        for variant in (text + "\nMICREC ABORT offset=80128", text + text,
                        text + "\nMICREC DATA 80128 00", text + "\n" + text):
            with self.subTest(end=variant[-120:]), self.assertRaises(ValueError):
                tool.parse_recording(variant)

    def test_listening_copy_removes_only_dc_and_applies_fixed_gain(self):
        payload = struct.pack("<4h", 900, 1100, 900, 1100)
        listening, metrics = tool.prepare_listening(payload)
        self.assertEqual(struct.unpack("<4h", listening), (-12000, 12000, -12000, 12000))
        self.assertEqual(metrics["pcm16_mean_dc"], 1000)
        self.assertEqual(metrics["pcm16_ac_rms"], 100)
        self.assertEqual(metrics["listening_fixed_gain"], 120)
        quiet, metrics = tool.prepare_listening(struct.pack("<4h", 50, 50, 50, 50))
        self.assertEqual(quiet, bytes(8))
        self.assertEqual(metrics["listening_fixed_gain"], 0)

    def test_wav_retains_every_pcm_byte_and_exact_rate(self):
        text, payload = make_export()
        recording = tool.parse_recording(text)
        with tempfile.TemporaryDirectory() as temporary:
            wav = Path(temporary) / "recording.wav"
            tool.write_wav(wav, payload, recording.rate)
            with wave.open(str(wav), "rb") as source:
                self.assertEqual((source.getnchannels(), source.getsampwidth(), source.getframerate()), (1, 2, 8013))
                self.assertEqual(source.getnframes(), 40064)
                self.assertEqual(source.readframes(source.getnframes()), payload)

    def test_live_capture_uses_new_command_saves_all_rows_and_releases_port(self):
        text = make_export()[0] + "\nTEST RESULT audio-record WAIT code=1\nTEST IDLE\n"
        port = FakePort([b"TEST STATUS ready=1 busy=0\n", text[:100].encode(), text[100:].encode()])
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "recording.txt"
            with patch("serial_test_port.SerialPort", return_value=port), redirect_stdout(io.StringIO()):
                captured = tool.capture_log("COM8", log)
            self.assertEqual(log.read_text(), captured)
        self.assertTrue(port.closed)
        self.assertEqual(port.writes, [b"\rhmi_test status\r", b"hmi_test audio-record\r"])

    def test_board_failure_and_timeout_request_stop_and_close_port(self):
        failure = FakePort([b"TEST STATUS ready=1 busy=0\n", b"TEST RESULT audio-record FAIL code=-3\nTEST IDLE\n"])
        timeout = FakePort([b"TEST STATUS ready=1 busy=0\n"])
        with tempfile.TemporaryDirectory() as temporary:
            for index, port in enumerate((failure, timeout)):
                with patch("serial_test_port.SerialPort", return_value=port), redirect_stdout(io.StringIO()):
                    with self.assertRaises((RuntimeError, TimeoutError)):
                        tool.capture_log("COM8", Path(temporary) / f"{index}.txt", timeout=0 if index else 1)
                self.assertTrue(port.closed)
                self.assertEqual(port.writes[-1], b"hmi_test stop\r")

    def test_denied_serial_open_leaves_no_log_and_same_prefix_can_retry(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "recording.txt"
            with patch("serial_test_port.SerialPort") as factory:
                factory.return_value.__enter__.side_effect = PermissionError("COM8 occupied")
                with self.assertRaises(PermissionError):
                    tool.capture_log("COM8", log)
                self.assertFalse(log.exists())
            text = make_export()[0] + "\nTEST RESULT audio-record WAIT code=1\nTEST IDLE\n"
            port = FakePort([b"TEST STATUS ready=1 busy=0\n", text.encode()])
            with patch("serial_test_port.SerialPort", return_value=port), redirect_stdout(io.StringIO()):
                captured = tool.capture_log("COM8", log)
            self.assertTrue(port.closed)
            self.assertEqual(tool.parse_recording(captured).frames, 40064)

    def test_log_creation_race_preserves_old_log_and_closes_serial(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "recording.txt"
            log.write_bytes(b"previous recording")
            port = FakePort([])
            with patch("serial_test_port.SerialPort", return_value=port):
                with self.assertRaises(FileExistsError):
                    tool.capture_log("COM8", log)
            self.assertTrue(port.closed)
            self.assertEqual(log.read_bytes(), b"previous recording")
            self.assertEqual(port.writes, [])

    def test_existing_prefix_refused_before_serial_open(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary) / "voice"
            Path(str(prefix) + ".wav").write_bytes(b"previous recording")
            with patch.object(sys, "argv", ["tool", "--port", "COM8", "--output", str(prefix)]), \
                    patch.object(tool, "find_ffmpeg", return_value=Path("ffmpeg")), \
                    patch.object(tool, "run_ffmpeg"), patch.object(tool, "capture_log") as capture:
                with self.assertRaises(ValueError):
                    tool.main()
                capture.assert_not_called()
            self.assertEqual(Path(str(prefix) + ".wav").read_bytes(), b"previous recording")

    def test_real_mp3_encoding_preserves_duration_and_known_tone(self):
        ffmpeg = tool.find_ffmpeg()
        samples = [round(1500 * math.sin(2 * math.pi * 1000 * index / 8013)) + 500 for index in range(40064)]
        text, payload = make_export(samples)
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary) / "synthetic"
            metrics = tool.save_outputs(tool.parse_recording(text), prefix, ffmpeg)
            self.assertAlmostEqual(metrics["duration_seconds"], 5, places=3)
            for suffix in (".mp3", ".raw.mp3"):
                encoded = Path(str(prefix) + suffix)
                decoded = subprocess.run(
                    [str(ffmpeg), "-v", "error", "-i", str(encoded), "-f", "s16le", "-ac", "1", "-ar", "16000", "-"],
                    check=True, capture_output=True, timeout=30,
                ).stdout
                values = struct.unpack(f"<{len(decoded) // 2}h", decoded)
                # MPEG-2 MP3每帧576样本；本机编码器留下约33ms帧填充。
                # WAV必须精确，MP3允许不足一帧的填充，不能误判为丢失录音。
                self.assertAlmostEqual(len(values) / 16000, 5, delta=0.04)
                # 在稳定1秒上计算1kHz投影，确认编码没有把已知音调变成静音。
                stable = values[16000:32000]
                sine = sum(value * math.sin(2 * math.pi * 1000 * index / 16000) for index, value in enumerate(stable))
                cosine = sum(value * math.cos(2 * math.pi * 1000 * index / 16000) for index, value in enumerate(stable))
                amplitude = 2 * math.hypot(sine, cosine) / len(stable)
                self.assertGreater(amplitude, 1000 if suffix == ".raw.mp3" else 9000)
            with wave.open(str(prefix) + ".wav", "rb") as source:
                self.assertEqual(source.readframes(40064), payload)


if __name__ == "__main__":
    unittest.main()
