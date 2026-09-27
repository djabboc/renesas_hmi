#include <rtthread.h>
#include "hal_data.h"

#define LCD_BL_EN_PIN   BSP_IO_PORT_01_PIN_05
#define LCD_BL_PWM_PIN  BSP_IO_PORT_01_PIN_00
#define LCD_PIXEL_COUNT (DISPLAY_HSIZE_INPUT0 * DISPLAY_VSIZE_INPUT0)

static display_cfg_t lcd_test_cfg;

static void lcd_test_thread(void *parameter)
{
    static const struct
    {
        rt_uint16_t rgb565;
        const char *name;
    } colors[] =
    {
        {0xF800, "red"},
        {0x07E0, "green"},
        {0x001F, "blue"},
        {0xFFFF, "white"},
        {0x0000, "black"},
    };
    rt_uint16_t *framebuffer = (rt_uint16_t *) fb_background[0];
    fsp_err_t err;
    rt_size_t color_index;
    rt_size_t pixel;

    RT_UNUSED(parameter);

    /* Both backlight controls are active high on this board. */
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, LCD_BL_EN_PIN,
                          IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    if (FSP_SUCCESS != err)
    {
        rt_kprintf("LCD: P105 setup failed: %d\n", (int) err);
        return;
    }

    err = R_IOPORT_PinCfg(&g_ioport_ctrl, LCD_BL_PWM_PIN,
                          IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    if (FSP_SUCCESS != err)
    {
        rt_kprintf("LCD: P100 setup failed: %d\n", (int) err);
        return;
    }

    rt_memset(fb_background, 0, sizeof(fb_background));
    lcd_test_cfg = g_display0_cfg;

    /* ST7282 parallel RGB timing: 531x292 total, 480x272 active. */
    lcd_test_cfg.output.htiming.total_cyc = 531;
    lcd_test_cfg.output.htiming.back_porch = 43;
    lcd_test_cfg.output.htiming.sync_width = 2;
    lcd_test_cfg.output.vtiming.total_cyc = 292;
    lcd_test_cfg.output.vtiming.back_porch = 12;
    lcd_test_cfg.output.vtiming.sync_width = 2;

    err = R_GLCDC_Open(&g_display0_ctrl, &lcd_test_cfg);
    if (FSP_SUCCESS != err)
    {
        rt_kprintf("LCD: GLCDC open failed: %d\n", (int) err);
        return;
    }

    err = R_GLCDC_Start(&g_display0_ctrl);
    if (FSP_SUCCESS != err)
    {
        rt_kprintf("LCD: GLCDC start failed: %d\n", (int) err);
        return;
    }

    /* Let the panel receive several frames before powering the LED driver. */
    rt_thread_mdelay(150);
    R_IOPORT_PinWrite(&g_ioport_ctrl, LCD_BL_EN_PIN, BSP_IO_LEVEL_HIGH);
    rt_thread_mdelay(10);
    R_IOPORT_PinWrite(&g_ioport_ctrl, LCD_BL_PWM_PIN, BSP_IO_LEVEL_HIGH);
    rt_kprintf("LCD: GLCDC running, backlight enabled\n");

    for (color_index = 0; ; color_index = (color_index + 1) % (sizeof(colors) / sizeof(colors[0])))
    {
        for (pixel = 0; pixel < LCD_PIXEL_COUNT; pixel++)
        {
            framebuffer[pixel] = colors[color_index].rgb565;
        }
        rt_kprintf("LCD: %s (RGB565 0x%04X)\n", colors[color_index].name,
                   colors[color_index].rgb565);
        rt_thread_mdelay(1500);
    }
}

static int lcd_test_start(void)
{
    rt_thread_t thread = rt_thread_create("lcd_test", lcd_test_thread, RT_NULL,
                                          1024, 15, 10);
    if (RT_NULL == thread)
    {
        rt_kprintf("LCD: test thread creation failed\n");
        return -RT_ENOMEM;
    }

    return rt_thread_startup(thread);
}
INIT_APP_EXPORT(lcd_test_start);
