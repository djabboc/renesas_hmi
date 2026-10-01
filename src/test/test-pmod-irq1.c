/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-pmod-irq1.c
 * @brief 验证 Pmod1 GPIO 到外部中断的回环。
 *
 * J2 pin8/P710 经 1 kΩ 接 pin7/P709；翻转 16 次，检查双边沿计数。
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

#define PMOD_IRQ_EDGE_COUNT 16u

static volatile unsigned irq_edge_count;

/* 每个外部中断边沿累加一次，由线程核对总边沿数。 */
static void pmod_irq_callback(external_irq_callback_args_t *arguments)
{
    RT_UNUSED(arguments);
    ++irq_edge_count;
}

/* J1/J2 的 GPIO0 经跳线进入 IRQ；双边沿计数验证引脚和中断链路。 */
static int run_test(void)
{

    int result = -RT_ERROR;
    const external_irq_instance_t *irq_instance = &g_external_irq10;
    bsp_io_port_pin_t input_pin = BSP_IO_PORT_07_PIN_09;
    bsp_io_port_pin_t output_pin = BSP_IO_PORT_07_PIN_10;
    external_irq_cfg_t config = *irq_instance->p_cfg;
    config.trigger = EXTERNAL_IRQ_TRIG_BOTH_EDGE;
    config.p_callback = pmod_irq_callback;
    rt_pin_mode(output_pin, PIN_MODE_OUTPUT);
    rt_pin_write(output_pin, 0);
    R_IOPORT_PinCfg(&g_ioport_ctrl,
                    input_pin,
                    IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_IRQ_ENABLE |
                        IOPORT_CFG_PULLUP_ENABLE);
    if (R_ICU_ExternalIrqOpen(irq_instance->p_ctrl, &config) != FSP_SUCCESS)
    {
        goto irq_pins;
    }
    if (R_ICU_ExternalIrqEnable(irq_instance->p_ctrl) != FSP_SUCCESS)
    {
        goto irq_close;
    }
    /* 消化初始电平引起的中断，再开始统计人为翻转。 */
    rt_thread_mdelay(10);
    irq_edge_count = 0;
    for (unsigned index = 0; index < PMOD_IRQ_EDGE_COUNT && !test_cancelled(); ++index)
    {
        rt_pin_write(output_pin, (index + 1) & 1);
        rt_thread_mdelay(10);
    }
    if (irq_edge_count == PMOD_IRQ_EDGE_COUNT)
    {
        result = 0;
    }
    else
    {
        result = -RT_ERROR;
    }
    rt_kprintf("PMOD %s GPIO->IRQ edges=%u expected=16\n", "irq1", irq_edge_count);
    R_ICU_ExternalIrqDisable(irq_instance->p_ctrl);
irq_close:
    R_ICU_ExternalIrqClose(irq_instance->p_ctrl);
irq_pins:
    test_restore_pin(output_pin);
    test_restore_pin(input_pin);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_pmod_irq1_thread(void *argument)
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
