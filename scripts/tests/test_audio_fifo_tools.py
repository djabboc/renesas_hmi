"""验证失败采集被拒绝，以及WAV保存原始样本、正确标记全速录音参数。"""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import audio_fifo_to_mp3 as fifo


def complete_report() -> bytes:
    words = [0] * 24
    words[0] = 0x4649464F
    words[1] = 3
    words[9] = 0x442B4201
    words[21] = 0x402B4200
    # 起始值接近DWT回绕；主机应按无符号32位差值计算时长。
    words[12] = 0xF0000000
    words[13] = (words[12] + 600000000) & 0xFFFFFFFF
    words[14] = 144231 + fifo.FRAMES
    words[15] = fifo.FRAMES
    words[16] = (-123456) & 0xFFFFFFFF
    words[17] = 234567
    words[23] = 0x444F4E45
    return struct.pack("<24I", *words)


class FifoToolsTests(unittest.TestCase):
    def test_complete_report_handles_cycle_wrap_and_signed_extrema(self):
        report = fifo.validate_report(complete_report())
        self.assertEqual(report["capture_duration_seconds"], 5.0)
        self.assertEqual(report["left24_minimum"], -123456)
        self.assertEqual(report["left24_maximum"], 234567)

    def test_rejects_incomplete_or_faulted_program(self):
        with self.assertRaises(ValueError):
            fifo.validate_report(complete_report()[:-4])
        for index in (0, 1, 23):
            with self.subTest(index=index):
                words = list(struct.unpack("<24I", complete_report()))
                words[index] = 0
                with self.assertRaises(ValueError):
                    fifo.validate_report(struct.pack("<24I", *words))

    def test_rejects_initialization_overrun_short_recording_and_bad_duration(self):
        for index in (*range(2, 9), 18, 20, 14, 15, 13):
            with self.subTest(index=index):
                words = list(struct.unpack("<24I", complete_report()))
                words[index] += 1 if index != 13 else 12000000
                with self.assertRaises(ValueError):
                    fifo.validate_report(struct.pack("<24I", *words))

    def test_wav_preserves_signed_samples_and_full_rate(self):
        pcm = struct.pack("<4h", -32768, -256, 0, 32767) * (fifo.FRAMES // 4)
        with tempfile.TemporaryDirectory() as folder:
            prefix = Path(folder) / "recording"
            with patch.object(fifo, "run_ffmpeg"):
                with patch("builtins.print"):
                    fifo.save_audio(prefix, pcm, fifo.validate_report(complete_report()), Path("unused"))
            with wave.open(str(prefix) + ".wav", "rb") as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 48077))
                self.assertEqual(wav.getnframes(), fifo.FRAMES)
                self.assertEqual(wav.readframes(fifo.FRAMES), pcm)
            with self.assertRaises(ValueError):
                fifo.save_audio(prefix, pcm[:-2], {}, Path("unused"))

    def test_polarity_variant_matches_actual_setup_and_rejects_wrong_label(self):
        words = list(struct.unpack("<24I", complete_report()))
        words[9] ^= 0x2000
        words[22] = 0x2000
        payload = struct.pack("<24I", *words)
        report = fifo.validate_report(payload, "clock-invert")
        self.assertEqual(report["variant"], "clock-invert")
        self.assertEqual(report["changed_setup_bits"], "00002000")
        with self.assertRaises(ValueError):
            fifo.validate_report(payload, "baseline")
        with self.assertRaises(ValueError):
            fifo.validate_report(complete_report(), "clock-invert")
        # 不能把极性与延迟/分频同时改动的录音标为单变量对照。
        for extra_bit in (0x100, 0x10, 0x1000):
            with self.subTest(extra_bit=extra_bit):
                changed = words.copy()
                changed[9] ^= extra_bit
                with self.assertRaises(ValueError):
                    fifo.validate_report(struct.pack("<24I", *changed), "clock-invert")

    def test_rejects_setup_while_receiver_enabled_or_missing_receive_enable(self):
        for index in (9, 21):
            words = list(struct.unpack("<24I", complete_report()))
            words[index] ^= 1
            with self.assertRaises(ValueError):
                fifo.validate_report(struct.pack("<24I", *words))


if __name__ == "__main__":
    unittest.main()
