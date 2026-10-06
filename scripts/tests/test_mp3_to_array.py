"""歌曲转换验证：幅度、边界、生成块原子更新、真实 MP3 解码与已嵌入数据一致性。"""

from array import array
import hashlib
from pathlib import Path
import re
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mp3_to_array as converter

ROOT = Path(__file__).resolve().parents[2]


class SongConversionTests(unittest.TestCase):
    def test_dc_removed_peak_bounded_and_endpoints_muted(self):
        samples = array("h", [5000 - 2000, 5000 + 2000] * 1000)
        output = converter.prepare_samples(samples)
        self.assertEqual((output[0], output[-1]), (0, 0))
        self.assertEqual((output[1000], output[1001]), (-3000, 3000))
        self.assertLessEqual(max(abs(value) for value in output), 3000)
        self.assertLess(abs(output[1]), 3000)
        self.assertEqual(len(output), len(samples))

    def test_signed_full_scale_and_tiny_inputs(self):
        for samples in (array("h", [-32768, 32767] * 1000), array("h", [-10, 10])):
            output = converter.prepare_samples(samples)
            self.assertTrue(all(-3000 <= value <= 3000 for value in output))
            self.assertEqual(output[0], 0)
            self.assertEqual(output[-1], 0)

    def test_empty_silent_and_oversized_inputs_rejected(self):
        for samples in (array("h"), array("h", [999] * 100),
                        array("h", [0] * (converter.SAMPLE_RATE * converter.MAX_SECONDS + 1))):
            with self.assertRaises(ValueError):
                converter.prepare_samples(samples)

    def test_failed_marker_update_leaves_original_untouched(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "test.c"
            original = converter.METADATA_BEGIN + "\nold\n" + converter.METADATA_END
            path.write_text(original, encoding="utf-8")
            with self.assertRaises(ValueError):
                converter.update_c(path, "replacement", "data")
            self.assertEqual(path.read_text(encoding="utf-8"), original)

    def test_conversion_rejects_invalid_clip_limits(self):
        source = ROOT / "scripts/assets/ode-to-joy.mp3"
        for start, seconds in ((-1, 1), (0, 0), (0, 31), (float("nan"), 1), (0, float("inf"))):
            with self.assertRaises(ValueError):
                converter.decode_mp3(source, Path("unused"), start, seconds)

    def test_embedded_pcm_matches_actual_mp3_conversion(self):
        try:
            ffmpeg = converter.find_ffmpeg()
        except ValueError as error:
            self.skipTest(str(error))
        asset = ROOT / "scripts/assets/ode-to-joy.mp3"
        samples = converter.decode_mp3(asset, ffmpeg, 0, 15)
        source = (ROOT / "src/test/test-audio-song.c").read_text(encoding="utf-8")
        count = int(re.search(r"#define SONG_SAMPLE_COUNT (\d+)u", source).group(1))
        peak = int(re.search(r"#define SONG_PCM_PEAK (\d+)u", source).group(1))
        data = source.split(converter.DATA_BEGIN)[1].split(converter.DATA_END)[0]
        embedded = array("h", map(int, re.findall(r"-?\d+", data.split("{", 1)[1].split("}", 1)[0])))
        self.assertEqual(embedded, samples)
        self.assertEqual(count, len(samples))
        self.assertEqual(peak, max(abs(sample) for sample in samples))
        self.assertIn(hashlib.sha256(asset.read_bytes()).hexdigest(), source)
        self.assertTrue(12 < len(samples) / converter.SAMPLE_RATE < 13)
        self.assertGreater(sum(sample != 0 for sample in samples), len(samples) * 0.8)
        # 第一个音符 E4；对波形做独立的过零计数，检查解码/采样率没有颠倒。
        segment = samples[4000:5600]
        crossings = sum(left <= 0 < right for left, right in zip(segment, segment[1:]))
        self.assertTrue(31 <= crossings <= 35, f"E4 crossings/0.1s: {crossings}")

    def test_wav_preview_has_correct_pcm_format(self):
        import wave
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "preview.wav"
            converter.write_wav(path, array("h", [-3000, 0, 3000]))
            with wave.open(str(path), "rb") as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 16000))
                self.assertEqual(wav.readframes(3), b"\x48\xf4\x00\x00\xb8\x0b")


if __name__ == "__main__":
    unittest.main()
