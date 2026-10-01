/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HMI_PERIPHERAL_TEST_H
#define HMI_PERIPHERAL_TEST_H

#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>

/* 所有测试使用同一结果约定；负值使用 RT-Thread 错误码。 */
enum peripheral_test_result
{
    TEST_PASS = 0, /* 当前阶段的程序断言全部满足。 */
    TEST_WAIT = 1, /* 已执行，仍需视觉、听觉或对端比对。 */
    TEST_SKIP = 2, /* 缺少卡、链路、USB 枚举等测试前提。 */
};

#define TEST_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/* 仅在测试线程/其调用链中轮询；取消不会强行打断 FSP 驱动。 */
int test_cancelled(void);
/* 无符号 tick 差值允许计数器回绕；ms 是从 start 起的等待上限。 */
int test_elapsed(rt_tick_t start, unsigned ms);
/* 恢复 FSP 生成的引脚配置；没有生成配置时退回高阻输入。 */
void test_restore_pin(bsp_io_port_pin_t pin);

/* 以下入口只允许调度线程串行调用。
 * stage 对应 hmi_test help 中的子命令；返回前关闭本阶段打开的外设。
 * 特例：G2D 硬件超时会保留 DMA 引用内存，并要求复位。
 */
int test_eth(const char *stage);
int test_can(const char *stage);
int test_sd(const char *stage);
int test_audio(const char *stage);
int test_usb(const char *stage);
int test_gpio(const char *stage);
int test_rtc(const char *stage);
int test_adc(const char *stage);
int test_pmod(const char *stage);
int test_lcd(const char *stage);
int test_touch(const char *stage);
int test_graphics(const char *stage);
int test_rw007(const char *stage);

#endif /* HMI_PERIPHERAL_TEST_H */
