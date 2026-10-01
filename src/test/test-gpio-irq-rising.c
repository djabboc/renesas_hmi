/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-gpio-irq-rising.c
 * @brief 验证 P009 输出到 P008/IRQ12 的上升沿中断。
 *
 * Arduino D9/P009 接 D2/P008；翻转 16 次，逐次检查电平与中断计数。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"

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

#define GPIO_IRQ_EDGE_COUNT 16u
#define GPIO_IRQ_INPUT_INDEX 0u
#define GPIO_IRQ_OUTPUT_INDEX 1u
#define GPIO_IRQ_NAME "rising"

/* 两个例程各自保存、配置和恢复这两个引脚，不调用其他测试。
 * P008 的外部中断固定为 IRQ12，P009 仅作普通 GPIO 输出。
 * P502 也可选择 IRQ12；运行前检查它未启用 ISEL，不改动其配置。 */
static const bsp_io_port_pin_t test_pins[] = {
    BSP_IO_PORT_00_PIN_08,
    BSP_IO_PORT_00_PIN_09
};
#define GPIO_IRQ_PIN_COUNT (sizeof(test_pins) / sizeof(test_pins[0]))

static volatile unsigned irq_edge_count;

/* 每个外部中断边沿累加一次，串口输出与判定留在线程中完成。 */
static void gpio_irq_callback(external_irq_callback_args_t *arguments)
{
    RT_UNUSED(arguments);
    ++irq_edge_count;
}

/* 保存运行前的实际配置；退出时原样恢复，还原这两个引脚之前的功能。 */
static uint32_t read_pin_configuration(bsp_io_port_pin_t pin)
{
    return R_PFS->PORT[pin >> 8].PIN[pin & 0xffu].PmnPFS;
}

static int configure_pin(bsp_io_port_pin_t pin, uint32_t configuration)
{
    fsp_err_t error = R_IOPORT_PinCfg(&g_ioport_ctrl, pin, configuration);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("GPIO IRQ %s pin=%04X config failed fsp=%d\n", GPIO_IRQ_NAME, (unsigned)pin, (int)error);
        return -RT_ERROR;
    }
    return TEST_PASS;
}

/* 先检查 P009 输出能否传到 IRQ 输入，不将无跳线误判成中断控制器故障。
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
        rt_pin_write(test_pins[GPIO_IRQ_OUTPUT_INDEX], levels[index]);
        rt_thread_mdelay(2);
        int output_level = rt_pin_read(test_pins[GPIO_IRQ_OUTPUT_INDEX]);
        int input_level = rt_pin_read(test_pins[GPIO_IRQ_INPUT_INDEX]);
        rt_kprintf("GPIO IRQ %s wire set=%u output=%d input=%d\n",
                   GPIO_IRQ_NAME, levels[index], output_level, input_level);
        if (output_level != (int)levels[index] || input_level != (int)levels[index])
        {
            rt_kprintf("GPIO IRQ %s wire FAIL: connect P009 to P008; interrupt test not started\n", GPIO_IRQ_NAME);
            return -RT_EIO;
        }
    }
    rt_kprintf("GPIO IRQ %s wire PASS\n", GPIO_IRQ_NAME);
    return TEST_PASS;
}

/* 每次独立初始化 IRQ12；先确认接线，再逐步验证选定边沿，最后释放硬件。 */
static int run_test(void)
{
    icu_instance_ctrl_t irq_control = {0};
    /* NVIC 向量 43 接到硬件 IRQ12；向量与事件的映射在 vector_data.c 中。
     * 配置和控制块属于本例程，关闭中断后才离开此函数。 */
    const external_irq_cfg_t config = {
        .channel = 12,
        .ipl = 12,
        .irq = HMI_GPIO_IRQ12_IRQn,
        .trigger = EXTERNAL_IRQ_TRIG_RISING,
        .pclk_div = EXTERNAL_IRQ_PCLK_DIV_BY_64,
        .filter_enable = false,
        .p_callback = gpio_irq_callback,
        .p_context = NULL,
        .p_extend = NULL
    };
    uint32_t saved_configuration[GPIO_IRQ_PIN_COUNT];
    unsigned matched_levels = 0;
    int result = -RT_ERROR;
    fsp_err_t error;

    /* 同编号 IRQ 只能选择一个输入。若 P502 已占用 IRQ12，直接退出。
     * 无需修改按键 P005/P006，也不借用其 IRQ10/IRQ11 控制块。 */
    if ((read_pin_configuration(BSP_IO_PORT_05_PIN_02) & IOPORT_CFG_IRQ_ENABLE) != 0 ||
        NVIC_GetEnableIRQ(config.irq) != 0)
    {
        rt_kprintf("GPIO IRQ %s IRQ12 busy: P502 ISEL or NVIC enabled\n", GPIO_IRQ_NAME);
        return -RT_EBUSY;
    }
    for (unsigned index = 0; index < GPIO_IRQ_PIN_COUNT; ++index)
    {
        saved_configuration[index] = read_pin_configuration(test_pins[index]);
    }
    /* 不调用 rt_pin_mode：本工程该接口会重新初始化整张引脚表。
     * 直接调用 PinCfg，只配置本例程使用的 P008 和 P009。 */
    if (configure_pin(test_pins[GPIO_IRQ_INPUT_INDEX],
                      IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_IRQ_ENABLE |
                          IOPORT_CFG_PULLUP_ENABLE) != TEST_PASS ||
        configure_pin(test_pins[GPIO_IRQ_OUTPUT_INDEX],
                      IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) != TEST_PASS)
    {
        goto restore_pins;
    }
    rt_kprintf("GPIO IRQ %s output pin=%04X -> IRQ pin=%04X, channel=%u\n", GPIO_IRQ_NAME,
               (unsigned)test_pins[GPIO_IRQ_OUTPUT_INDEX], (unsigned)test_pins[GPIO_IRQ_INPUT_INDEX], config.channel);
    result = check_jumper();
    if (result != TEST_PASS)
    {
        goto restore_pins;
    }
    result = -RT_ERROR;
    error = R_ICU_ExternalIrqOpen(&irq_control, &config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("GPIO IRQ %s IRQ open failed fsp=%d\n", GPIO_IRQ_NAME, (int)error);
        goto restore_pins;
    }
    /* 跳线检查结束时输入已稳定为低；Enable 清除旧请求后才开始统计。 */
    irq_edge_count = 0;
    error = R_ICU_ExternalIrqEnable(&irq_control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("GPIO IRQ %s IRQ enable failed fsp=%d\n", GPIO_IRQ_NAME, (int)error);
        goto close_irq;
    }
    rt_kprintf("GPIO IRQ %s IRQCR=%02X NVIC=%u ISEL=%u; RISING\n", GPIO_IRQ_NAME,
               (unsigned)R_ICU->IRQCR[config.channel], (unsigned)NVIC_GetEnableIRQ(config.irq),
               (unsigned)((read_pin_configuration(test_pins[GPIO_IRQ_INPUT_INDEX]) & IOPORT_CFG_IRQ_ENABLE) != 0));
    unsigned level = 0;
    unsigned expected_edges = 0;
    unsigned matched_edges = 0;
    for (unsigned index = 0; index < GPIO_IRQ_EDGE_COUNT; ++index)
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
        /* 上升沿模式下，下降沿也必须检查：此时累计中断数应保持不变。 */
        if (level == 1)
        {
            ++expected_edges;
        }
        rt_pin_write(test_pins[GPIO_IRQ_OUTPUT_INDEX], level);
        rt_thread_mdelay(10);
        if (irq_edge_count == expected_edges)
        {
            ++matched_edges;
        }
        else
        {
            rt_kprintf("GPIO IRQ %s step=%u edges=%u expected=%u\n",
                       GPIO_IRQ_NAME, index + 1, irq_edge_count, expected_edges);
        }
        int output_level = rt_pin_read(test_pins[GPIO_IRQ_OUTPUT_INDEX]);
        int input_level = rt_pin_read(test_pins[GPIO_IRQ_INPUT_INDEX]);
        if (output_level == (int)level && input_level == (int)level)
        {
            ++matched_levels;
        }
        else
        {
            rt_kprintf("GPIO IRQ %s level mismatch step=%u set=%u output=%d input=%d\n",
                       GPIO_IRQ_NAME, index + 1, level, output_level, input_level);
        }
    }
    error = R_ICU_ExternalIrqDisable(&irq_control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("GPIO IRQ %s IRQ disable failed fsp=%d\n", GPIO_IRQ_NAME, (int)error);
        goto close_irq;
    }
    rt_kprintf("GPIO IRQ %s GPIO->IRQ edges=%u expected=%u levels=%u/16 edge_checks=%u/16\n",
               GPIO_IRQ_NAME, irq_edge_count, expected_edges, matched_levels, matched_edges);
    if (test_cancelled())
    {
        result = -RT_EINTR;
    }
    else if (matched_levels == GPIO_IRQ_EDGE_COUNT && matched_edges == GPIO_IRQ_EDGE_COUNT &&
             irq_edge_count == expected_edges)
    {
        result = TEST_PASS;
    }
    else
    {
        rt_kprintf("GPIO IRQ %s FAIL: compare level readback and interrupt count\n", GPIO_IRQ_NAME);
        result = -RT_EIO;
    }
close_irq:
    error = R_ICU_ExternalIrqClose(&irq_control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("GPIO IRQ %s IRQ close failed fsp=%d\n", GPIO_IRQ_NAME, (int)error);
        result = -RT_ERROR;
    }
restore_pins:
    /* 中断关闭后恢复 P008/P009；失败和取消路径也执行同样的清理。 */
    for (unsigned index = 0; index < GPIO_IRQ_PIN_COUNT; ++index)
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
void test_gpio_irq_rising_thread(void *argument)
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
