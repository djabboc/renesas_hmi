> 结构迁移说明（2026-10-01）：本文保留原例程的实现与验收历史。当前单文件位置和 MSH 命令已统一迁移，使用方法见 `peripheral-tests.md`，完整映射见 `peripheral-code-quality.md`；不要再照旧文启用 INIT_APP_EXPORT 或常驻线程。

# LCD + GT911 画板（不使用 LVGL）

## 启用与操作

入口文件为 `src/lcd-touch.c`。只保留 `INIT_APP_EXPORT(lcd_touch_start)`，注释其他例程的 `INIT_APP_EXPORT(...)`，重新编译并烧录。此例程直接绘制 RGB565 帧缓冲，不包含 LVGL 头文件、不调用 LVGL；本次固件的符号检查也确认没有链接 `lv_init`、`lv_timer_handler`、`lv_disp_drv_register`。

屏幕顶部是红、绿、蓝、白、黑五个颜色按钮和 `CLEAR`，黄色边框表示所选颜色；灰色区域为画布（Y=80～239），底部 X/Y 显示本帧第一个触点的坐标，N 显示触点数。右下角绿点每 500 ms 闪烁，表明线程持续工作。

点击色块后在画布拖动绘图，点击 `CLEAR` 清除画布。按钮仅在新落指时触发；拖过工具栏不会重复触发按钮。抬手或离开画布后中断当前笔画，下次落笔不会连接旧位置。最多处理五指，按 GT911 的 4 位触点 ID 分别保存轨迹（ID 存储范围 0～15），所有手指使用当前选中的颜色。

## 实现要点

- 显示复用 `lcd.c` 已验证的 480 × 272 GLCDC 时序和 P105/P100 背光控制，启动时低电平，接收帧后拉高，固定全亮度。
- 使用 `fb_background[0]`，自行绘制小型 ASCII 字体、矩形和五像素宽轨迹，无图形库依赖；笔刷裁剪到画布，帧缓冲写入裁剪到屏幕边界。
- GT911 使用 I2C1，INT=P004，RST=P801。使用单引脚 FSP 配置，避免重开整个 IOPORT 影响显示。
- 自动尝试地址 0x5D/0x14，校验产品 ID `911`，读取 0x8048 的坐标范围，要求与 480 × 272 一致。坐标直接映射，不旋转、不交换 X/Y。
- IRQ 下降沿释放信号量，工作线程读取 0x814E/0x814F；20 ms 超时读取用于补偿漏边沿。完整读取后清状态；拒绝超过五点、重复 ID、越界坐标，发生读取错误时断开旧轨迹，防止误连线。
- 使用单线程绘图，3072 字节栈、优先级 20。单帧缓冲未做垂直同步交换，快速绘制时可能有瞬时撕裂。

## 构建、串口与验收

```powershell
python scripts/build_flash.py --build-only
python scripts/build_flash.py
python scripts/monitor_msh.py --port COM8
```

串口为 115200、8N1。MSH 命令 `lcd_touch_status` 显示 ready、有效帧数、错误计数、清屏次数、触点数和坐标；`list thread` 查看线程与栈，`free` 查看堆。每次落指/抬指、选色、清屏均有日志。

2026-10-01 自测：RT-Thread Studio 编译 0 错误、4 个停用例程的未使用函数警告，Flash 90460 字节、静态 RAM 267440 字节；DAP-LINK/PyOCD 烧录成功。GT911 实测为 0x5D，范围 480 × 272，输出 `ready, direct RGB565`。

交互后串口记录 `ready=1 frames=536 errors=0 clears=1 points=0`，观察到不同颜色选择、落指/抬指及清屏事件；显示线程栈峰值 19%，可用堆 373448 字节。ELF 中仅存在 `__rt_init_lcd_touch_start` 这一例程入口。另从板上读取 RGB565 帧缓冲，核对了文字、颜色按钮和绘图轨迹。调试日志为 `logs/msh_20261001_001649.log`（不提交）。

验收项目：颜色选择与轨迹一致；坐标与触摸位置对应；抬手换位置不误连线；双指拖动后 N 回到 0；CLEAR 清屏。用户于 2026-10-01 确认“全部正常，验收通过”。
