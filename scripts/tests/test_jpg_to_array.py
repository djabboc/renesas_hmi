"""验证图片转换产物、EXIF 方向和 C 文件更新边界，不访问串口。"""

from io import BytesIO
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from PIL import Image, JpegImagePlugin

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import jpg_to_array


class JpegArrayTests(unittest.TestCase):
    def test_progressive_input_becomes_baseline_and_array_roundtrips(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "彩色输入.jpg"
            Image.new("RGB", (128, 80), (230, 40, 15)).save(source, progressive=True)
            jpeg = jpg_to_array.prepare_jpeg(source, 64, 32, 90)
            generated = jpg_to_array.render_array(jpeg, "test_jpeg")
            array = generated.split("BSP_ALIGN_VARIABLE(8) =", 1)[1].split("};", 1)[0]
            restored = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", array))
            self.assertEqual(restored, jpeg)
            with Image.open(BytesIO(restored)) as decoded:
                self.assertEqual(decoded.size, (64, 32))
                self.assertEqual(decoded.mode, "RGB")
                self.assertFalse(decoded.info.get("progressive", False))
                self.assertEqual(JpegImagePlugin.get_sampling(decoded), 0)
                red, green, blue = decoded.getpixel((32, 16))
                self.assertGreater(red, 220)
                self.assertLess(green, 50)
                self.assertLess(blue, 25)
            patches = jpg_to_array.reference_patches(jpeg)
            self.assertEqual(len(patches), 12)
            self.assertTrue(all(red > green and red > blue for _, _, red, green, blue in patches))

    def test_exif_orientation_is_applied_before_resizing(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "rotated.jpg"
            image = Image.new("RGB", (32, 16), (255, 0, 0))
            image.paste((0, 0, 255), (16, 0, 32, 16))
            exif = Image.Exif()
            exif[274] = 6  # 旋转 90 度：左红右蓝应成为上红下蓝。
            image.save(source, exif=exif, quality=95, subsampling=0)
            jpeg = jpg_to_array.prepare_jpeg(source, 16, 32, 95)
            with Image.open(BytesIO(jpeg)) as decoded:
                self.assertGreater(decoded.getpixel((8, 4))[0], 230)
                self.assertGreater(decoded.getpixel((8, 28))[2], 230)

    def test_marker_update_preserves_code_and_rejects_ambiguous_markers(self):
        begin = "/* BEGIN GENERATED JPEG: test_jpeg */"
        end = "/* END GENERATED JPEG: test_jpeg */"
        source = "before\n" + begin + "\nold\n" + end + "\nafter\n"
        generated = begin + "\nnew\n" + end + "\n"
        self.assertEqual(jpg_to_array.replace_generated_block(source, generated, "test_jpeg"),
                         "before\n" + generated + "after\n")
        for invalid in ("no markers", source + begin, end + begin):
            with self.assertRaises(ValueError):
                jpg_to_array.replace_generated_block(invalid, generated, "test_jpeg")

    def test_invalid_size_quality_and_symbol_are_rejected(self):
        for width, height, quality in ((481, 272, 90), (480, 271, 90),
                                       (0, 272, 90), (480, 272, 100)):
            with self.assertRaises(ValueError):
                jpg_to_array.prepare_jpeg(Path("unused.jpg"), width, height, quality)
        with self.assertRaises(ValueError):
            jpg_to_array.render_array(b"", "bad;symbol")

    def test_cli_refuses_to_overwrite_input(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "keep.jpg"
            Image.new("RGB", (16, 16), (0, 200, 20)).save(source)
            original = source.read_bytes()
            completed = subprocess.run([sys.executable, jpg_to_array.__file__, str(source),
                                        "-o", str(source)], capture_output=True)
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(source.read_bytes(), original)

    def test_committed_firmware_array_matches_prepared_jpeg(self):
        root = Path(__file__).resolve().parents[2]
        jpeg = (root / "scripts/assets/landscape-480x272.jpg").read_bytes()
        source = (root / "src/test/test-graphics-jpeg.c").read_text(encoding="utf-8")
        # 检查提交的 C 数据与可见 JPG 一致，避免图片换了而固件数组忘记更新。
        generated = jpg_to_array.render_array(jpeg, "test_jpeg")
        self.assertIn(generated.rstrip(), source)


if __name__ == "__main__":
    unittest.main()
