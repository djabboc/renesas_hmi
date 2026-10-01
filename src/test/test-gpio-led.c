/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-gpio-led.c
 * @brief 依次翻转板载三个 LED。
 *
 * P209/P210 低有效、P204 高有效；输出序列后恢复引脚，亮灭效果需要目视验收。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>

/* 结果：0=断言通过，1=等待人工观察，2=缺少测试条件，负值=错误。 */
enum
{
    TEST_PASS = 0,
    TEST_WAIT = 1,
    TEST_SKIP = 2
};
#define TEST_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/* 参数可为 NULL；提供 RT-Thread 事件对象时，bit0 表示请求协作退出。
 * 不清除事件位，让初始化、收发和清理路径都能看见同一次停止请求。 */
static int test_cancelled(void)
{
    rt_event_t stop_event = (rt_event_t)rt_thread_self()->parameter;
    rt_uint32_t received = 0;
    if (stop_event == RT_NULL)
    {
        return 0;
    }
    return rt_event_recv(stop_event, 1, RT_EVENT_FLAG_OR, 0, &received) == RT_EOK;
}

/* 恢复 FSP 生成的复用配置；未配置的引脚退回高阻输入。 */
static void test_restore_pin(bsp_io_port_pin_t pin)
{
    for (unsigned index = 0; index < g_bsp_pin_cfg.number_of_pins; ++index)
    {
        if (g_bsp_pin_cfg.p_pin_cfg_data[index].pin == pin)
        {
            R_IOPORT_PinCfg(&g_ioport_ctrl, pin, g_bsp_pin_cfg.p_pin_cfg_data[index].pin_cfg);
            return;
        }
    }
    R_IOPORT_PinCfg(&g_ioport_ctrl, pin, IOPORT_CFG_PORT_DIRECTION_INPUT);
}

static const bsp_io_port_pin_t led_pins[] = {
    BSP_IO_PORT_02_PIN_09, BSP_IO_PORT_02_PIN_10, BSP_IO_PORT_02_PIN_04};

/* 两个红灯低有效、D13 蓝灯高有效；结束恢复生成的引脚配置。 */
static int run_test(void)
{
    unsigned pin_index;

    for (pin_index = 0; pin_index < TEST_ARRAY_SIZE(led_pins); ++pin_index)
    {
        if (pin_index == 2)
        {
            rt_kprintf("LED %u pin=P%u%02u active=%u\n",
                       pin_index,
                       led_pins[pin_index] >> 8,
                       led_pins[pin_index] & 255,
                       1);
        }
        else
        {
            rt_kprintf("LED %u pin=P%u%02u active=%u\n",
                       pin_index,
                       led_pins[pin_index] >> 8,
                       led_pins[pin_index] & 255,
                       0);
        }
        rt_pin_mode(led_pins[pin_index], PIN_MODE_OUTPUT);
        for (unsigned toggle_index = 0; toggle_index < 4 && !test_cancelled(); ++toggle_index)
        {
            rt_pin_write(led_pins[pin_index], toggle_index & 1);
            rt_thread_mdelay(180);
        }
        if (pin_index == 2)
        {
            rt_pin_write(led_pins[pin_index], 0);
        }
        else
        {
            rt_pin_write(led_pins[pin_index], 1);
        }
        test_restore_pin(led_pins[pin_index]);
    }
    rt_kprintf("LED output sequence finished; visual confirmation required\n");
    return TEST_WAIT;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_gpio_led_thread(void *argument)
{
    int result;
    RT_UNUSED(argument);
    result = run_test();
    if (test_cancelled())
    {
        result = -RT_EINTR;
    }
    rt_thread_self()->error = result;
}
