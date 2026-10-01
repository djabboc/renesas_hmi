# JPEG 风光测试素材

本目录图片用于 `graphics-jpeg` 的彩色硬件解码测试。图片是实拍全景照片，不是 AI 生成图。

- 作品：**Fronalpstock, Switzerland**（原文件名 `Fronalpstock big.jpg`）。
- 作者：**Hannes Röst**。
- 来源页面：https://commons.wikimedia.org/wiki/File:Fronalpstock_big.jpg
- 作者页面：https://commons.wikimedia.org/wiki/User:Hannes_R%C3%B6st
- 图片许可：**CC BY-SA 3.0**，https://creativecommons.org/licenses/by-sa/3.0/
- `fronalpstock-source.jpg`：Wikimedia 提供的 1280 像素宽缩略图，保留供离线重新转换。
- `landscape-480x272.jpg`：居中裁剪、缩放为 480×272，并重新编码为 quality=90、baseline、4:4:4 JPEG。由本工程转换，仍按 CC BY-SA 3.0 提供。
- `src/test/test-graphics-jpeg.c` 中对应 JPEG 压缩数组是上述图片的字节表示，沿用图片许可。转换脚本和测试逻辑不因此改变其代码许可。

原作由 12 张照片拼接，来源说明包含拼接过程中可能进行的融合、模糊、克隆、颜色及透视调整。

在仓库根目录重新生成数组与预览：

```powershell
python scripts/jpg_to_array.py scripts/assets/fronalpstock-source.jpg --update-c src/test/test-graphics-jpeg.c --prepared-jpeg scripts/assets/landscape-480x272.jpg
```

更换为自己的图片时，也应同步更新这里与 C 文件中的图片来源和许可说明。
