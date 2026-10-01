# JPG 转数组与彩色风光显示

`scripts/jpg_to_array.py` 把图片转换成适合本工程的 JPEG 压缩字节数组。开发板使用 RA6M3 JCU 硬件解码。数组保存 JPEG 压缩字节；Python 的软件解码仅用于生成 12 处区域颜色均值参考。

## 准备 Python

```powershell
python -m pip install Pillow
```

本次转换和测试使用 Pillow 12.3.0。以下命令在工程根目录执行，带空格的路径用双引号包围。

## 只生成数组文本

```powershell
python scripts/jpg_to_array.py "D:/Pictures/my-photo.jpg" -o logs/my-photo-array.txt --prepared-jpeg logs/my-photo-480x272.jpg
```

输出文本包含尺寸宏、`static const unsigned char test_jpeg[]` 和 12 个颜色参考区域，可以直接放进使用 FSP 的 C 文件。使用 `.txt` 是为了避免 Studio 将一个单独的素材 `.c` 误纳入构建。

脚本先按 EXIF 方向摆正图片，再转 RGB、居中裁剪到目标比例并缩放。默认输出 480×272、质量 90、baseline（非渐进式）、YCbCr 4:4:4 JPEG。输入也可为 Pillow 支持的其他图片格式。居中裁剪会裁掉画面边缘，请检查 `--prepared-jpeg` 保存的实际结果。

## 更新当前独立例程

```powershell
python scripts/jpg_to_array.py scripts/assets/fronalpstock-source.jpg --update-c src/test/test-graphics-jpeg.c --prepared-jpeg scripts/assets/landscape-480x272.jpg
python scripts/build_flash.py
```

更换照片时只需替换第一个图片路径。`--update-c` 仅更新 `BEGIN GENERATED JPEG: test_jpeg` 与 `END GENERATED JPEG: test_jpeg` 之间的块，其他测试逻辑保留；标记缺失、重复或顺序错误时拒绝修改。输入图片、输出文本/C 文件与预览 JPG 不能使用同一路径，防止覆盖原图。

常用参数：

| 参数 | 默认值 | 作用 |
| --- | --- | --- |
| `--width` / `--height` | 480 / 272 | 宽 16～480、高 16～272，均须为 16 的整数倍 |
| `--quality` | 90 | JPEG 质量 1～95；超过 65536 压缩字节时可降低质量或尺寸 |
| `--symbol` | `test_jpeg` | 生成的 C 数组名称；现有例程保持默认，改名需另行调整引用 |
| `--prepared-jpeg` | 不保存 | 保存真正嵌入固件的 JPEG，便于预览和比对 |

尺寸、颜色参考和数组必须一起更新。小于屏幕的图片显示在左上角，其余区域为黑色。当前尺寸与字节上限是本例程的内存和简化流程约束，不代表 JCU 的全部硬件能力。

## 板端做了什么

1. 将 Flash 中的 JPEG 数组复制到 8 字节对齐的 SRAM 输入缓冲，末尾保留清零的 4096 字节预取余量。
2. 以完整输入模式启动 JCU，等待尺寸就绪，核对尺寸与生成宏一致。
3. 将 JCU 输出直接指向已有 LCD RGB565 帧缓冲，设置真实行跨度，避免额外分配一份 261120 字节画面。
4. 等待解码完成并核对行数。检查 12 个 8×8 区域的 RGB565 通道均值，与 Python 对最终 JPEG 的软件解码参考比较，R5/G6/B5 容差分别为 2/4/2。
5. 校验成功后启动 GLCDC，显示 30 秒，随后熄屏并释放显示外设。`decode/check` 日志单独计量解码与校验时间，不包含观察窗口。

区域均值能发现明显色序、行跨度或数据错误，不能代替整图逐像素证明。硬件和软件的 IDCT 舍入、色彩转换存在差异，因此采用容差而非完全相等。真实细节、偏色、撕裂仍需要人工对照预览。该例程没有使用 LVGL 或调用其他测试文件。

## 人工验收

```text
hmi_test graphics-jpeg
```

默认素材应全屏显示蓝天白云、绿色山坡、岩壁和山脚房屋。对照 `scripts/assets/landscape-480x272.jpg`，检查方向、颜色及细节，无条带、花屏或明显色偏。日志预期为 `color patches=12/12`、`480x272`、`lines=272`，约 30 秒后 `WAIT`、`TEST IDLE`；`WAIT` 仍表示等待视觉确认。提前结束用 `hmi_test stop`。

素材作者及授权见 `scripts/assets/README.md`。历史灰图验收已通过；本次彩色大图的板端运行与视觉效果需要另行验收。
