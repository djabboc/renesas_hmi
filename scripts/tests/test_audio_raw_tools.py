"""原始录音工具：用合成PCM和内存串口验证，不访问真实COM8。"""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import audio_raw_to_wav as tool


def make_export(samples=None):
    if samples is None:
        samples = [-8388608, -1, 0, 1, 8388607] * 4800
    payload = b"".join((value & 0xFFFFFF).to_bytes(3, "little") for value in samples)
    checksum = zlib.crc32(payload)
    lines = [f"MICRAW BEGIN v=1 rate=48077 frames=24000 start=144231 bytes=72000 crc32={checksum:08X}"]
    for offset in range(0, len(payload), 32):
        lines.append(f"MICRAW DATA {offset} {payload[offset:offset + 32].hex().upper()}")
    lines.append(f"MICRAW END bytes=72000 crc32={checksum:08X}")
    return "\n".join(lines), payload, samples


class FakePort:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.closed = True

    def read(self):
        return self.chunks.pop(0) if self.chunks else b""

    def write(self, data):
        self.writes.append(data)


class RawLogTests(unittest.TestCase):
    def test_signed_endpoints_and_console_prefix_preserved(self):
        text, payload, samples = make_export()
        text = "msh >" + text + "\nTEST RESULT audio-raw WAIT code=1\nTEST IDLE\n"
        recording = tool.parse_recording(text)
        self.assertEqual(recording.payload, payload)
        self.assertEqual(tool.decode_pcm24(recording.payload), samples)
        self.assertEqual(recording.start, 144231)

    def test_missing_duplicate_reordered_and_truncated_rows_rejected(self):
        lines = make_export()[0].splitlines()
        variants = [lines[:4] + lines[5:], lines[:4] + [lines[3]] + lines[4:],
                    lines[:2] + [lines[3], lines[2]] + lines[4:], lines[:-1]]
        for variant in variants:
            with self.subTest(variant=variant[:5]), self.assertRaises(ValueError):
                tool.parse_recording("\n".join(variant))

    def test_payload_crc_length_header_and_abort_rejected(self):
        text = make_export()[0]
        variants = [text.replace("000080", "010080", 1),
                    text.replace("rate=48077", "rate=16026", 1),
                    text.replace("MICRAW END bytes=72000", "MICRAW END bytes=71999", 1),
                    text.replace("MICRAW DATA 32 ", "MICRAW DATA 32 X", 1),
                    text + "\nMICRAW ABORT offset=72000"]
        for variant in variants:
            with self.subTest(begin=variant[:100]), self.assertRaises(ValueError):
                tool.parse_recording(variant)
        with self.assertRaises(ValueError):
            tool.parse_recording(text + "\n" + text)

    def test_raw_wav_bytes_rate_and_listen_copy_are_distinct(self):
        text, payload, samples = make_export([3000, 4000] * 12000)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "voice"
            metrics = tool.save_outputs(tool.parse_recording(text), output, plot=False)
            with wave.open(str(output) + ".raw24.wav") as stream:
                self.assertEqual((stream.getframerate(), stream.getsampwidth(), stream.getnframes()), (48077, 3, 24000))
                self.assertEqual(stream.readframes(24000), payload)
            with wave.open(str(output) + ".listen.wav") as stream:
                values = struct.unpack("<24000h", stream.readframes(24000))
                self.assertEqual(values[:4], (-12000, 12000, -12000, 12000))
            self.assertEqual(metrics["mean_dc"], 3500)
            self.assertEqual(metrics["ac_rms"], 500)
            self.assertEqual(json.loads(Path(str(output) + ".json").read_text())["frames"], 24000)

    def test_silence_and_constant_offset_remain_silent_in_listen_copy(self):
        for samples in ([0] * 20, [500] * 20):
            metrics = tool.analyze_samples(samples)
            self.assertEqual(metrics["ac_rms"], 0)
            self.assertIsNone(metrics["ac_rms_dbfs"])
            pcm, gain = tool.listening_copy(samples)
            self.assertEqual(gain, 0)
            self.assertEqual(pcm, bytes(40))

    def test_explicit_capture_fragmented_export_and_handle_release(self):
        text = make_export()[0] + "\nTEST RESULT audio-raw WAIT code=1\nTEST IDLE\n"
        encoded = text.encode()
        port = FakePort([b"TEST STATUS ready=1 busy=0\n"] + [encoded[i:i + 127] for i in range(0, len(encoded), 127)])
        with tempfile.TemporaryDirectory() as folder, patch("serial_test_port.SerialPort", return_value=port), redirect_stdout(io.StringIO()):
            captured = tool.capture_log("COM8", Path(folder) / "serial.txt")
            self.assertEqual(len(tool.parse_recording(captured).payload), 72000)
        self.assertTrue(port.closed)
        self.assertEqual(port.writes, [b"\rhmi_test status\r", b"hmi_test audio-raw\r"])

    def test_failed_board_test_requests_stop_and_releases_handle(self):
        port = FakePort([b"TEST STATUS ready=1 busy=0\n", b"TEST RESULT audio-raw FAIL code=-3\nTEST IDLE\n"])
        with tempfile.TemporaryDirectory() as folder, patch("serial_test_port.SerialPort", return_value=port), redirect_stdout(io.StringIO()):
            with self.assertRaises(RuntimeError):
                tool.capture_log("COM8", Path(folder) / "serial.txt")
        self.assertTrue(port.closed)
        self.assertEqual(port.writes[-1], b"hmi_test stop\r")

    def test_capture_timeout_requests_stop_and_releases_handle(self):
        port = FakePort([b"TEST STATUS ready=1 busy=0\n"])
        with tempfile.TemporaryDirectory() as folder, patch("serial_test_port.SerialPort", return_value=port), redirect_stdout(io.StringIO()):
            with self.assertRaises(TimeoutError):
                tool.capture_log("COM8", Path(folder) / "serial.txt", timeout=0)
        self.assertTrue(port.closed)
        self.assertEqual(port.writes[-1], b"hmi_test stop\r")


if __name__ == "__main__":
    unittest.main()
