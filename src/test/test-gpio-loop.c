/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-gpio-loop.c
 * @brief 验证 Arduino 数字引脚输出到输入的回环。
 *
 * D2/P008 经 1 kΩ 接 D9/P009；翻转 16 次并逐次读回。
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

/* Arduino D2 -> D9，建议串联 1kΩ；所有电平均需在输入端读回一致。 */
static int run_test(void)
{
    unsigned pin_index;

    /* Arduino D2=P008 输出，D9=P009 输入，建议串联 1kΩ。 */
    int result = 0;
    rt_pin_mode(BSP_IO_PORT_00_PIN_09, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(BSP_IO_PORT_00_PIN_08, PIN_MODE_OUTPUT);
    for (pin_index = 0; pin_index < 16; ++pin_index)
    {
        rt_pin_write(BSP_IO_PORT_00_PIN_08, pin_index & 1);
        rt_thread_mdelay(2);
        if (rt_pin_read(BSP_IO_PORT_00_PIN_09) != (int)(pin_index & 1))
        {
            result = -RT_ERROR;
        }
    }
    test_restore_pin(BSP_IO_PORT_00_PIN_08);
    test_restore_pin(BSP_IO_PORT_00_PIN_09);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_gpio_loop_thread(void *argument)
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
