/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-lcd-backlight.c
 * @brief 验证 ST7282 背光 PWM 明暗变化。
 *
 * 先建立 RGB 时序，再切换 P100 为 GPT5 PWM，按 0→100→0% 调光。
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

/* 无符号 tick 差值允许系统计时回绕；等待始终有上限。 */
static int test_elapsed(rt_tick_t start, unsigned milliseconds)
{
    return (rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(milliseconds);
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

static volatile unsigned lcd_irq_count;

/* GLCDC 扫描中断计数，用于确认显示时序持续运行。 */
static void display_event(display_callback_args_t *arguments)
{
    RT_UNUSED(arguments);
    ++lcd_irq_count;
}

/* 必须在背光关闭后调用；Stop 和 Close 各自有 100ms 等待上限。 */
static int close_display(void)
{
    rt_tick_t start = rt_tick_get();
    fsp_err_t error;
    do
    {
        error = R_GLCDC_Stop(&g_display0_ctrl);
        if (!error)
        {
            break;
        }
        rt_thread_mdelay(1);
    } while (!test_elapsed(start, 100));
    /* Stop 仅提交请求；Close 需等下一帧停稳。静态配置对象保证超时后仍有效。 */
    start = rt_tick_get();
    do
    {
        error = R_GLCDC_Close(&g_display0_ctrl);
        if (!error)
        {
            break;
        }
        rt_thread_mdelay(1);
    } while (!test_elapsed(start, 100));
    if (error)
    {
        rt_kprintf("LCD close=%d; reset before further display tests\n", error);
    }
    if (error == FSP_SUCCESS)
    {
        return TEST_PASS;
    }
    else
    {
        return -RT_ERROR;
    }
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    static display_cfg_t display_config;
    uint16_t *pixels = (uint16_t *)fb_background[0];
    int result = -RT_ERROR;
    int backlight_pwm_open = 0;

    /* 模组已验收时序：有效区 480x272，下面是包含消隐的总周期。 */
    display_config = g_display0_cfg;
    display_config.output.htiming.total_cyc = 531;
    display_config.output.htiming.back_porch = 43;
    display_config.output.htiming.sync_width = 2;
    display_config.output.vtiming.total_cyc = 292;
    display_config.output.vtiming.back_porch = 12;
    display_config.output.vtiming.sync_width = 2;
    display_config.p_callback = display_event;
    lcd_irq_count = 0;
    rt_pin_mode(BSP_IO_PORT_01_PIN_05, PIN_MODE_OUTPUT);
    rt_pin_write(BSP_IO_PORT_01_PIN_05, 0);
    rt_pin_mode(BSP_IO_PORT_01_PIN_00, PIN_MODE_OUTPUT);
    rt_pin_write(BSP_IO_PORT_01_PIN_00, 0);
    memset(pixels, 0, DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * 272);
    if (R_GLCDC_Open(&g_display0_ctrl, &display_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_GLCDC_Start(&g_display0_ctrl) != FSP_SUCCESS)
    {
        goto close_lcd;
    }
    /* 先让 RGB 时序稳定，再依次打开共用使能与背光。 */
    rt_thread_mdelay(150);
    rt_pin_write(BSP_IO_PORT_01_PIN_05, 1);
    rt_thread_mdelay(10);
    rt_pin_write(BSP_IO_PORT_01_PIN_00, 1);

    memset(pixels, 0xff, DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * 272);
    /* 恢复 P100 的 GPT5 复用，将恒亮 GPIO 切换为 PWM 调光。 */
    test_restore_pin(BSP_IO_PORT_01_PIN_00);
    if (R_GPT_Open(&g_timer5_ctrl, &g_timer5_cfg) != FSP_SUCCESS)
    {
        goto close_lcd;
    }
    backlight_pwm_open = 1;
    if (R_GPT_Start(&g_timer5_ctrl) != FSP_SUCCESS)
    {
        goto close_lcd;
    }
    for (unsigned step = 0; step <= 10 && !test_cancelled(); ++step)
    {
        unsigned brightness_percent;
        if (step <= 5)
        {
            brightness_percent = step * 20;
        }
        else
        {
            brightness_percent = (10 - step) * 20;
        }
        if (R_GPT_DutyCycleSet(&g_timer5_ctrl,
                               g_timer5_cfg.period_counts * brightness_percent / 100,
                               GPT_IO_PIN_GTIOCA) != FSP_SUCCESS)
        {
            goto close_lcd;
        }
        rt_kprintf("LCD brightness=%u%%\n", brightness_percent);
        rt_thread_mdelay(250);
    }

    rt_kprintf("LCD interrupts=%u; color/brightness require visual confirmation\n", lcd_irq_count);
    if (lcd_irq_count)
    {
        result = TEST_WAIT;
    }
    else
    {
        result = -RT_ERROR;
    }
close_lcd:
    if (backlight_pwm_open)
    {
        R_GPT_Stop(&g_timer5_ctrl);
        R_GPT_Close(&g_timer5_ctrl);
    }
    rt_pin_mode(BSP_IO_PORT_01_PIN_00, PIN_MODE_OUTPUT);
    rt_pin_write(BSP_IO_PORT_01_PIN_00, 0);
    rt_pin_write(BSP_IO_PORT_01_PIN_05, 0);
    if (close_display() != TEST_PASS)
    {
        result = -RT_ERROR;
    }
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_lcd_backlight_thread(void *argument)
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
