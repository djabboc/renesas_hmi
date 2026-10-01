#!/usr/bin/env python3
"""把图片转为本工程 JCU 可解码的 JPEG，并输出或更新 C 数组。

依赖：python -m pip install Pillow
示例：
  python scripts/jpg_to_array.py photo.jpg -o logs/photo-array.txt
  python scripts/jpg_to_array.py photo.jpg --update-c src/test/test-graphics-jpeg.c

输出保存的是 JPEG 压缩字节，不是 RGB565 像素。使用 RGB、baseline、
4:4:4 编码和 16 的整数倍尺寸，便于 RA6M3 JCU 全帧解码。
"""

import argparse
import hashlib
from io import BytesIO
from pathlib import Path
import re
import sys

try:
    from PIL import Image, ImageOps
except ImportError:
    raise SystemExit("缺少 Pillow，请运行：python -m pip install Pillow")


MAX_JPEG_BYTES = 65536
PATCH_SIDE = 8


def prepare_jpeg(source: Path, width: int, height: int, quality: int) -> bytes:
    """按 EXIF 方向摆正，居中裁剪到屏幕比例，转为非渐进 JPEG。"""
    if width < 16 or width > 480 or height < 16 or height > 272:
        raise ValueError("本工程支持宽 16～480、高 16～272 像素")
    if width % 16 != 0 or height % 16 != 0:
        raise ValueError("宽、高必须是 16 的整数倍")
    if quality < 1 or quality > 95:
        raise ValueError("quality 必须在 1～95 之间")

    with Image.open(source) as original:
        upright = ImageOps.exif_transpose(original)
        # 先转 RGB，避免 CMYK、灰度及透明通道进入硬件 JPEG 解码路径。
        rgb = upright.convert("RGB")
        fitted = ImageOps.fit(rgb, (width, height), method=Image.Resampling.LANCZOS)
        stream = BytesIO()
        fitted.save(stream, format="JPEG", quality=quality, subsampling=0,
                    progressive=False, optimize=False)
    jpeg = stream.getvalue()
    if len(jpeg) > MAX_JPEG_BYTES:
        raise ValueError("JPEG 超过 65536 字节，请降低 quality 或图片尺寸")
    return jpeg


def reference_patches(jpeg: bytes) -> list[tuple[int, int, int, int, int]]:
    """对最终 JPEG 软件解码，在 4×3 网格中取 8×8 区域的 RGB565 通道均值。

    硬件与软件的舍入可能不同，C 例程使用容差比较，不做逐字节强相等。
    参考来自最终 JPEG，不能使用压缩前原图，否则会混入 JPEG 有损误差。
    """
    with Image.open(BytesIO(jpeg)) as decoded:
        rgb = decoded.convert("RGB")
        references = []
        for row in range(3):
            top = (row + 1) * rgb.height // 4 - PATCH_SIDE // 2
            for column in range(4):
                left = (column + 1) * rgb.width // 5 - PATCH_SIDE // 2
                left = max(0, min(left, rgb.width - PATCH_SIDE))
                top = max(0, min(top, rgb.height - PATCH_SIDE))
                red_sum = 0
                green_sum = 0
                blue_sum = 0
                for y in range(top, top + PATCH_SIDE):
                    for x in range(left, left + PATCH_SIDE):
                        red, green, blue = rgb.getpixel((x, y))
                        red_sum += red >> 3
                        green_sum += green >> 2
                        blue_sum += blue >> 3
                count = PATCH_SIDE * PATCH_SIDE
                references.append((left, top, red_sum // count,
                                   green_sum // count, blue_sum // count))
    return references


def render_array(jpeg: bytes, symbol: str) -> str:
    """输出可粘贴到独立 C 文件中的数组、尺寸和采样参考。"""
    if re.fullmatch(r"[a-z][a-z0-9_]*_jpeg", symbol) is None:
        raise ValueError("symbol 请使用小写字母开头、以 _jpeg 结尾的 C 标识符")
    with Image.open(BytesIO(jpeg)) as image:
        width, height = image.size
    prefix = symbol.upper()
    lines = [
        f"/* BEGIN GENERATED JPEG: {symbol} */",
        "/* 由 scripts/jpg_to_array.py 生成；此块中的数据、尺寸和参考值须同步更新。",
        " * 图片版权和授权由输入图片决定，转换不改变其许可。",
        f" * JPEG SHA256: {hashlib.sha256(jpeg).hexdigest()} */",
        f"#define {prefix}_WIDTH {width}u",
        f"#define {prefix}_HEIGHT {height}u",
        f"static const unsigned char {symbol}[] BSP_ALIGN_VARIABLE(8) =",
        "{",
    ]
    for offset in range(0, len(jpeg), 16):
        values = ", ".join(f"0x{value:02x}" for value in jpeg[offset:offset + 16])
        lines.append(f"    {values},")
    lines.extend([
        "};",
        "/* 每行：8×8 区域左上角 x、y，以及 R5、G6、B5 平均值。 */",
        f"static const uint16_t {symbol}_reference[][5] =",
        "{",
    ])
    for patch in reference_patches(jpeg):
        lines.append("    {" + ", ".join(str(value) for value in patch) + "},")
    lines.extend(["};", f"/* END GENERATED JPEG: {symbol} */", ""])
    return "\n".join(lines)


def replace_generated_block(source: str, generated: str, symbol: str) -> str:
    """只替换成对标记内的数据块，缺失或重复标记时拒绝修改文件。"""
    begin = f"/* BEGIN GENERATED JPEG: {symbol} */"
    end = f"/* END GENERATED JPEG: {symbol} */"
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("C 文件必须恰有一对匹配的 BEGIN/END GENERATED JPEG 标记")
    start = source.index(begin)
    finish = source.index(end)
    if finish < start:
        raise ValueError("C 文件中的 JPEG 标记顺序错误")
    return source[:start] + generated.rstrip("\n") + source[finish + len(end):]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="输入 JPG 或 Pillow 支持的图片")
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument("-o", "--output", type=Path, help="输出数组文本，例如 logs/photo-array.txt")
    destination.add_argument("--update-c", type=Path, help="仅更新已有 C 文件的 JPEG 标记块")
    parser.add_argument("--prepared-jpeg", type=Path, help="同时保存实际嵌入的 JPEG，便于预览")
    parser.add_argument("--width", type=int, default=480)
    parser.add_argument("--height", type=int, default=272)
    parser.add_argument("--quality", type=int, default=90)
    parser.add_argument("--symbol", default="test_jpeg")
    args = parser.parse_args()

    try:
        # 先检查所有路径和内容，再写文件，避免错误参数破坏原图或 C 文件。
        target = args.output
        if args.update_c is not None:
            target = args.update_c
        paths = [args.input.resolve(), target.resolve()]
        if args.prepared_jpeg is not None:
            paths.append(args.prepared_jpeg.resolve())
        if len(paths) != len(set(paths)):
            raise ValueError("输入图片、数组/C 文件和预览 JPG 必须使用不同路径")
        jpeg = prepare_jpeg(args.input, args.width, args.height, args.quality)
        text = render_array(jpeg, args.symbol)
        if args.update_c is not None:
            source = target.read_text(encoding="utf-8")
            text = replace_generated_block(source, text, args.symbol)
        target.parent.mkdir(parents=True, exist_ok=True)
        if args.prepared_jpeg is not None:
            args.prepared_jpeg.parent.mkdir(parents=True, exist_ok=True)
            args.prepared_jpeg.write_bytes(jpeg)
        target.write_text(text, encoding="utf-8", newline="\n")
    except (OSError, ValueError, Image.DecompressionBombError) as error:
        parser.exit(1, f"转换失败：{error}\n")

    print(f"JPEG: {args.width}x{args.height}, baseline RGB/YCbCr 4:4:4, {len(jpeg)} bytes")
    print(f"数组：{target}")
    print("RGB565 解码缓冲按实际行跨度分配；本例 480x272 为 261120 字节。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
