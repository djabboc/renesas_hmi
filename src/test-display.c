/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-display.c
 * @brief ST7282 RGB 屏五色及背光测试。
 *
 * 沿用已验收的 480x272 时序。P105 为屏幕/背光共用使能，
 * P100 为背光 PWM。GLCDC 的停止请求须等待帧边界才能关闭。
 */
#include "peripheral-test.h"

static volatile unsigned lcd_irq_count;

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
    return error == FSP_SUCCESS ? TEST_PASS : -RT_ERROR;
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

int test_lcd(const char *stage)
{
    static display_cfg_t display_config;
    uint16_t *pixels = (uint16_t *)fb_background[0];
    int result = -RT_ERROR;
    int backlight_pwm_open = 0;
    if (strcmp(stage, "colors") && strcmp(stage, "backlight"))
    {
        return -RT_EINVAL;
    }
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
    if (R_GLCDC_Open(&g_display0_ctrl, &display_config))
    {
        return -RT_ERROR;
    }
    if (R_GLCDC_Start(&g_display0_ctrl))
    {
        goto close_lcd;
    }
    /* 先让 RGB 时序稳定，再依次打开共用使能与背光。 */
    rt_thread_mdelay(150);
    rt_pin_write(BSP_IO_PORT_01_PIN_05, 1);
    rt_thread_mdelay(10);
    rt_pin_write(BSP_IO_PORT_01_PIN_00, 1);
    show_test_colors(pixels);
    if (strcmp(stage, "backlight") == 0)
    {
        memset(pixels, 0xff, DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * 272);
        /* 恢复 P100 的 GPT5 复用，将恒亮 GPIO 切换为 PWM 调光。 */
        test_restore_pin(BSP_IO_PORT_01_PIN_00);
        if (R_GPT_Open(&g_timer5_ctrl, &g_timer5_cfg))
        {
            goto close_lcd;
        }
        backlight_pwm_open = 1;
        if (R_GPT_Start(&g_timer5_ctrl))
        {
            goto close_lcd;
        }
        for (unsigned step = 0; step <= 10 && !test_cancelled(); ++step)
        {
            unsigned brightness_percent = step <= 5 ? step * 20 : (10 - step) * 20;
            if (R_GPT_DutyCycleSet(&g_timer5_ctrl,
                                   g_timer5_cfg.period_counts * brightness_percent / 100,
                                   GPT_IO_PIN_GTIOCA))
            {
                goto close_lcd;
            }
            rt_kprintf("LCD brightness=%u%%\n", brightness_percent);
            rt_thread_mdelay(250);
        }
    }
    rt_kprintf("LCD interrupts=%u; color/brightness require visual confirmation\n", lcd_irq_count);
    result = lcd_irq_count ? TEST_WAIT : -RT_ERROR;
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
