/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-pmod-irq1.c
 * @brief 验证 Pmod1 GPIO 到外部中断的回环。
 *
 * J2-4/P710 经 1 kΩ 接 J2-2/P709；翻转 16 次，检查双边沿计数。
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

#define PMOD_IRQ_EDGE_COUNT 16u
#define PMOD_INPUT_INDEX 0u
#define PMOD_OUTPUT_INDEX 1u
#define PMOD_NAME "irq1"

/* 第 0 项为 IRQ 输入，第 1 项为测试输出，其余是同 IRQ 通道的其他候选输入。
 * RA6M3 手册 20.2.5 要求：同编号 IRQ 只能有一个引脚的 ISEL=1。
 * IRQ10 的候选脚为 P709 和按键 P005。
 * 这些候选脚只临时清除 ISEL，不改变其方向、电平或其他外设功能。 */
static const bsp_io_port_pin_t test_pins[] = {
    BSP_IO_PORT_07_PIN_09,
    BSP_IO_PORT_07_PIN_10,
    BSP_IO_PORT_00_PIN_05
};
#define PMOD_PIN_COUNT (sizeof(test_pins) / sizeof(test_pins[0]))

static volatile unsigned irq_edge_count;

/* 每个外部中断边沿累加一次，串口输出与判定留在线程中完成。 */
static void pmod_irq_callback(external_irq_callback_args_t *arguments)
{
    RT_UNUSED(arguments);
    ++irq_edge_count;
}

/* 保存运行前的实际配置；退出时原样恢复，避免影响按键或 SD 等其他功能。 */
static uint32_t read_pin_configuration(bsp_io_port_pin_t pin)
{
    return R_PFS->PORT[pin >> 8].PIN[pin & 0xffu].PmnPFS;
}

static int configure_pin(bsp_io_port_pin_t pin, uint32_t configuration)
{
    fsp_err_t error = R_IOPORT_PinCfg(&g_ioport_ctrl, pin, configuration);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD %s pin=%04X config failed fsp=%d\n", PMOD_NAME, (unsigned)pin, (int)error);
        return -RT_ERROR;
    }
    return TEST_PASS;
}

/* 先检查 GPIO0 输出能否传到 IRQ 输入，不将无跳线误判成中断控制器故障。
 * 上拉输入使断线时的低电平检查容易失败。最后回到低电平，作为 16 次翻转的起点。 */
static int check_jumper(void)
{
    const unsigned levels[] = {0, 1, 0, 1, 0};
    for (unsigned index = 0; index < sizeof(levels) / sizeof(levels[0]); ++index)
    {
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        rt_pin_write(test_pins[PMOD_OUTPUT_INDEX], levels[index]);
        rt_thread_mdelay(2);
        int output_level = rt_pin_read(test_pins[PMOD_OUTPUT_INDEX]);
        int input_level = rt_pin_read(test_pins[PMOD_INPUT_INDEX]);
        rt_kprintf("PMOD %s wire set=%u output=%d input=%d\n",
                   PMOD_NAME, levels[index], output_level, input_level);
        if (output_level != (int)levels[index] || input_level != (int)levels[index])
        {
            rt_kprintf("PMOD %s wire FAIL: GPIO0 to IRQ; interrupt test not started\n", PMOD_NAME);
            return -RT_EIO;
        }
    }
    rt_kprintf("PMOD %s wire PASS\n", PMOD_NAME);
    return TEST_PASS;
}

/* 每次独立选择 IRQ 输入、检查跳线、验证 16 个双边沿，再关闭中断并恢复引脚。 */
static int run_test(void)
{
    const external_irq_instance_t *irq_instance = &g_external_irq10;
    external_irq_cfg_t config = *irq_instance->p_cfg;
    uint32_t saved_configuration[PMOD_PIN_COUNT];
    unsigned matched_levels = 0;
    int result = -RT_ERROR;
    fsp_err_t error;

    config.trigger = EXTERNAL_IRQ_TRIG_BOTH_EDGE;
    config.p_callback = pmod_irq_callback;
    for (unsigned index = 0; index < PMOD_PIN_COUNT; ++index)
    {
        saved_configuration[index] = read_pin_configuration(test_pins[index]);
    }
    /* 不调用 rt_pin_mode：本工程该接口会重新初始化整张引脚表，重新使能按键 ISEL。 */
    for (unsigned index = 2; index < PMOD_PIN_COUNT; ++index)
    {
        uint32_t isolated = saved_configuration[index] & ~(uint32_t)IOPORT_CFG_IRQ_ENABLE;
        if (configure_pin(test_pins[index], isolated) != TEST_PASS)
        {
            goto restore_pins;
        }
        rt_kprintf("PMOD %s IRQ%u peer pin=%04X ISEL=%u->0\n", PMOD_NAME, config.channel,
                   (unsigned)test_pins[index],
                   (unsigned)((saved_configuration[index] & IOPORT_CFG_IRQ_ENABLE) != 0));
    }
    if (configure_pin(test_pins[PMOD_INPUT_INDEX],
                      IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_IRQ_ENABLE |
                          IOPORT_CFG_PULLUP_ENABLE) != TEST_PASS ||
        configure_pin(test_pins[PMOD_OUTPUT_INDEX],
                      IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) != TEST_PASS)
    {
        goto restore_pins;
    }
    rt_kprintf("PMOD %s GPIO0 pin=%04X -> IRQ pin=%04X, channel=%u\n", PMOD_NAME,
               (unsigned)test_pins[PMOD_OUTPUT_INDEX], (unsigned)test_pins[PMOD_INPUT_INDEX], config.channel);
    result = check_jumper();
    if (result != TEST_PASS)
    {
        goto restore_pins;
    }
    result = -RT_ERROR;
    error = R_ICU_ExternalIrqOpen(irq_instance->p_ctrl, &config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD %s IRQ open failed fsp=%d\n", PMOD_NAME, (int)error);
        goto restore_pins;
    }
    /* 跳线检查结束时输入已稳定为低；Enable 清除旧请求后才开始统计。 */
    irq_edge_count = 0;
    error = R_ICU_ExternalIrqEnable(irq_instance->p_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD %s IRQ enable failed fsp=%d\n", PMOD_NAME, (int)error);
        goto close_irq;
    }
    rt_kprintf("PMOD %s IRQCR=%02X NVIC=%u ISEL=%u; BOTH_EDGE\n", PMOD_NAME,
               (unsigned)R_ICU->IRQCR[config.channel], (unsigned)NVIC_GetEnableIRQ(config.irq),
               (unsigned)((read_pin_configuration(test_pins[PMOD_INPUT_INDEX]) & IOPORT_CFG_IRQ_ENABLE) != 0));
    unsigned level = 0;
    for (unsigned index = 0; index < PMOD_IRQ_EDGE_COUNT; ++index)
    {
        if (test_cancelled())
        {
            break;
        }
        if (level == 0)
        {
            level = 1;
        }
        else
        {
            level = 0;
        }
        rt_pin_write(test_pins[PMOD_OUTPUT_INDEX], level);
        rt_thread_mdelay(10);
        int output_level = rt_pin_read(test_pins[PMOD_OUTPUT_INDEX]);
        int input_level = rt_pin_read(test_pins[PMOD_INPUT_INDEX]);
        if (output_level == (int)level && input_level == (int)level)
        {
            ++matched_levels;
        }
        else
        {
            rt_kprintf("PMOD %s level mismatch step=%u set=%u output=%d input=%d\n",
                       PMOD_NAME, index + 1, level, output_level, input_level);
        }
    }
    error = R_ICU_ExternalIrqDisable(irq_instance->p_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD %s IRQ disable failed fsp=%d\n", PMOD_NAME, (int)error);
        goto close_irq;
    }
    rt_kprintf("PMOD %s GPIO->IRQ edges=%u expected=16 levels=%u/16\n",
               PMOD_NAME, irq_edge_count, matched_levels);
    if (test_cancelled())
    {
        result = -RT_EINTR;
    }
    else if (matched_levels == PMOD_IRQ_EDGE_COUNT && irq_edge_count == PMOD_IRQ_EDGE_COUNT)
    {
        result = TEST_PASS;
    }
    else
    {
        rt_kprintf("PMOD %s FAIL: compare level readback and interrupt count\n", PMOD_NAME);
        result = -RT_EIO;
    }
close_irq:
    error = R_ICU_ExternalIrqClose(irq_instance->p_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD %s IRQ close failed fsp=%d\n", PMOD_NAME, (int)error);
        result = -RT_ERROR;
    }
restore_pins:
    /* 先恢复目标输入，再恢复同通道候选脚；所有失败、取消路径也必须还原配置。 */
    for (unsigned index = 0; index < PMOD_PIN_COUNT; ++index)
    {
        if (configure_pin(test_pins[index], saved_configuration[index]) != TEST_PASS)
        {
            result = -RT_ERROR;
        }
    }
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
