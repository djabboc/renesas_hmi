# LCD + LVGL 显示例程

## 入口和依赖

`src/lcd-lvgl.c` 使用 LVGL v8.3.11，在 RA6M3 的 ST7282 480 × 272 RGB 屏上显示五色色块、文字、往返进度条和运行计数。此例程不接入触摸。

LVGL 官方源代码和 MIT 许可证位于 `third_party/lvgl/`，来源、版本和下载校验值见该目录的 `UPSTREAM.md`。配置位于 `third_party/lv_conf.h`，RT-Thread 自动识别配置的适配头为 `board/lv_rt_thread_conf.h`。

仅保留当前例程的 `INIT_APP_EXPORT(lcd_lvgl_start)`，将其他例程的 `INIT_APP_EXPORT(...)` 全部注释。切换例程后重新编译、烧录并复位。不要启用旧的 `BSP_USING_LCD` / `BSP_USING_LVGL` 自动初始化，以免重复打开 GLCDC。

## 显示实现

- 复用 `lcd.c` 已验证的 GLCDC 配置：RGB565，水平总周期 531、后肩 43、同步宽度 2；垂直总周期 292、后肩 12、同步宽度 2。
- 使用生成的 `fb_background[0]` 帧缓冲；LVGL 局部绘图缓冲为 480 × 20 × 2 = 19200 字节。刷新回调按行复制并裁剪到屏幕范围。
- P105 为背光使能，P100 为背光 PWM/EN，均高电平有效。本例程输出持续高电平，使用全亮度。
- 引脚使用 `R_IOPORT_PinCfg` 单独配置，避免 `rt_pin_mode` 重新初始化其他引脚。
- 单一 `lcdlvgl` 线程调用 LVGL，栈 4096 字节，优先级 20，主循环间隔 5 ms；界面定时器每 100 ms 更新一次。
- LVGL 使用 RT-Thread 堆和毫秒时钟。单帧缓冲直接更新，未实现垂直同步交换，快速变化画面可能出现撕裂。

## 构建和检查

```powershell
python scripts/build_flash.py --build-only
python scripts/build_flash.py
python scripts/monitor_msh.py --port COM8
```

串口为 115200、8N1。MSH 命令：`lvgl_status` 查看就绪与刷新次数，`list thread` 查看线程和栈峰值，`free` 查看堆内存。每 50 次界面更新输出一次更新/刷新计数。

验收时应看到红、绿、蓝、白、黑五块及对应文字，进度条在 0～100% 之间往返，运行计数持续增加。运行秒数按界面更新次数折算，用于展示刷新，不能作为高精度计时器。

## 实测与验收

2026-09-30 至 2026-10-01：RT-Thread Studio 编译 0 错误、3 个已停用例程的未使用函数警告；Flash 406596 字节，静态 RAM 287832 字节。DAP-LINK / PyOCD 烧录成功，COM8 输出 `LVGL: ready 480x272 RGB565, draw buffer=19200 bytes`。观察到 850 次界面更新和 2645 次刷新，未见断言或重启。

`lcdlvgl` 栈峰值为 55%；两次堆检查可用内存为 342736 / 342096 字节，最大使用量均为 25568 字节。原始日志为 `logs/msh_20260930_235919.log`（不纳入 Git）。

2026-10-01 用户确认：“五色色块和文字显示正常，进度条持续往返，运行计数持续增加。”本项验收通过。
