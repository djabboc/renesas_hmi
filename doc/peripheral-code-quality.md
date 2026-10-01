# 外设测试代码可读性整改

日期：2026-10-01。触发原因：用户指出新测试代码语句压缩、可读性差、注释不足，要求先整改再继续实物验收。

## 阅读入口与职责

| 文件 | 职责与阅读顺序 |
| --- | --- |
| `src/peripheral-test.h` | 统一 PASS/WAIT/SKIP/负错误码语义，说明取消、tick 超时、引脚恢复和测试入口约束 |
| `src/peripheral-test.c` | 命令解析 → 单槽请求投递 → 唯一工作线程执行 → 阶段结果记录；状态/帮助独立输出 |
| `src/test-ethernet.c` | PHY 寄存器定义 → MAC 派生/控制器打开 → 链路等待 → 收发适配 → 身份/回环/联网校验 → 统一关闭 |
| `src/test-sd.c` | SDHI 事件等待和 FatFs 磁盘适配 → 只读扇区比较 → 独占文件写入/重挂载读回 → 阶段分发 |
| `src/test-can.c` | 中断收帧/计数 → 具名邮箱与位时序配置 → 有限等待 → 帧校验 → 关闭 |
| `src/test-gpio.c` | LED、按键观察、GPIO 跳线回环分别使用独立函数 |
| `src/test-adc.c` | 从 GPIO 模块独立出的 ADC 测试；定义采样数量、参考电压、端点误差和超时 |
| `src/test-display.c` | 五色输出、帧边界 Stop/Close 与背光阶段；注释解释 stride 和开启/关闭顺序 |
| `src/test-touch.c` | 从显示模块独立出的 GT911 测试；寄存器地址、数据就绪位、触点记录长度具名化，区分初始化和触点观察 |
| `src/test-audio.c` | 采集、PWM 中断、播放与统计；解释时钟推导、24 位符号扩展、限幅及停止顺序 |
| `src/test-pmod.c` | IRQ、I²C、SPI 各自独立，标明夹具要求、回调与配置生命周期 |
| `src/test-graphics.c` | G2D 与 JPEG 独立；解释 RGB565 校验、预取缓冲、DMA 超时保留内存的原因 |
| `src/test-rtc.c` | 解释晶振稳定等待、日历字段表示、闹钟匹配和中断向量对应关系 |
| `src/test-usb.c` | 描述符/中断 → 枚举等待 → 有限回显 → 关闭；区分排队发送与主机内容校验 |
| `src/rwoo7-demo.c` | 展开压缩语句，补全套件同步适配入口的互斥、观察窗口和关闭说明 |
| `scripts/test_peripherals.py` | 参数检查、就绪检查、单项执行、USB 主机比对、资源采集分别为独立函数 |
| `scripts/serial_test_port.py` | 明确 Win32 函数签名、115200/8N1 配置、读写时限及异常路径句柄关闭 |
| `scripts/select_example.py` | 目标存在性检查 → 切换入口 → 检查唯一有效入口 |

## 维护约定

- 使用四空格缩进、独立括号、完整控制分支；一行不堆叠多个执行语句。
- 用变量名表达用途：例如 `received_frame`、`playback_position`、`file_open`；使用带单位的超时/速率常量。
- 文件注释说明外设、依赖与测试边界；函数/局部注释解释硬件时序、异步缓冲生命周期、判定依据和清理原因。
- `goto` 只用于跳到本函数对应的资源清理点；硬件打开成功后，正常/失败路径均需审查关闭顺序。
- 维持已有 MSH 命令和串口结果标记，便于与之前的验收日志对照。
- 不对 FSP、TinyUSB、FatFs、LVGL 等供应商源码做全库格式化。根 `.clang-format` 供手写应用代码使用。

格式与脚本回归工具版本：clang-format 18.1.8、Black 24.10.0。格式工具只参与开发检查，不是固件构建依赖。

```powershell
$testSources = @(Get-ChildItem src/test-*.c,src/test-*.h,src/peripheral-test.c,src/peripheral-test.h,src/rwoo7-demo.c | ForEach-Object FullName)
clang-format --dry-run --Werror $testSources
python -m black --check --line-length 100 scripts/test_peripherals.py scripts/serial_test_port.py scripts/select_example.py scripts/configure_peripheral_tests.py scripts/tests/test_peripheral_tools.py
python -m unittest discover -s scripts/tests -v
python scripts/build_flash.py --build-only
```

## 验证记录

- 最终 Studio 构建及 PyOCD 烧录成功，0 errors / 8 warnings；8 个警告均为停用例程入口未使用。
- Flash 298408 B（整改前 298076 B），静态 RAM 355424 B（与整改前相同）。
- 11 项主机回归测试通过：串口分块结束标记、FAIL/WAIT/SKIP 判定、忙拒绝、超时取消、就绪标记、USB 缺少枚举/内容不符、二进制字节比较、所有 profile 的唯一入口及重复执行、目标缺失保护。
- clang-format/Black 检查、Python 语法检查、UTF-8 检查通过；JPEG 夹具字节与整改前完全一致。
- 源码与 ELF 均确认唯一应用初始化入口为 `peripheral_test_start`。
- 构建日志：`logs/readability-build-final.log`。主机测试使用内存串口/临时目录，不代替硬件验收。
- 用户释放 COM8 后，`hmi_test all`、PHY 回环、lwIP、JPEG、RTC 闹钟回归完成，无非预期 FAIL。网口再次取得 `192.168.0.102`，100Mbps 全双工，HTTP 200、正文匹配，tx=13/rx=8/drops=0。
- RTC 闹钟恰好 1 次；G2D/JPEG 像素比对通过；CAN 内部回环、麦克风采集、GT911 身份、RW007 信息通过。LCD/LED/悬空 ADC 为 WAIT；TF 无卡、系统 USB 未枚举为 SKIP，不能当作验收通过。
- 本批线程栈峰值：ptest 7% / 12288 B；堆可用 276256 B、峰值使用 89144 B。日志：`logs/peripherals_20261001_141723.log`，采集结束已释放 COM8。
- 补充阶段：背光阶梯完成且正常退出；提示音 8000/8000、回放 8192/8192；Wi-Fi 扫描 8 条、BLE 扫描 3 条 PASS。按键空闲电平 WAIT、外部 I²C 无设备 SKIP。补测后可用堆仍为 276256 B，日志 `logs/peripherals_20261001_141821.log`。
- 调度器实板回归：触点窗口运行期间投递 RTC 被 `TEST BUSY` 拒绝；发送 stop 后在本次总耗时 203 ms 时退出，输出 `FAIL code=-9`（预期的 RT_EINTR 取消结果）；随后触摸身份测试 PASS，状态恢复 `busy=0 cancel=0`。日志 `logs/readability-dispatch-regression.log`。该主动取消记录不是未解决的外设故障。
- 所有采集结束均关闭 COM8。未插 TF 卡、未枚举的系统 USB、无对端/跳线的 CAN 总线和扩展口仍需后续实物验收。

之前网口验收结果继续保留在 `peripheral-test-results.md`；重构版的硬件回归须另记，不以旧固件验收代替。
