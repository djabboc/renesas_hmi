/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-lcd-colors.c
 * @brief 验证 ST7282 的 RGB565 五种纯色。
 *
 * 480×272；P105 共用电源/背光使能，P100 背光；结束熄屏并关闭 GLCDC。
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

/* 按生成帧缓冲的 stride 写入，不能假设每行恰好 480 个像素。 */
static void show_test_colors(uint16_t *pixels)
{
    static const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0};
    for (unsigned step = 0; step < 5 && !test_cancelled(); ++step)
    {
        for (unsigned y = 0; y < 272; ++y)
        {
            for (unsigned x = 0; x < 480; ++x)
            {
                pixels[y * DISPLAY_BUFFER_STRIDE_PIXELS_INPUT0 + x] = colors[step];
            }
        }
        rt_kprintf("LCD color=%04X\n", colors[step]);
        rt_thread_mdelay(350);
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
    show_test_colors(pixels);

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
void test_lcd_colors_thread(void *argument)
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
