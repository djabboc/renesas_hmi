#include <rtthread.h>
#include "hal_data.h"
#include "../third_party/lvgl/lvgl.h"

#define LVGL_LCD_WIDTH   DISPLAY_HSIZE_INPUT0
#define LVGL_LCD_HEIGHT  DISPLAY_VSIZE_INPUT0
#define LVGL_DRAW_LINES  20
#define LVGL_BL_EN       BSP_IO_PORT_01_PIN_05
#define LVGL_BL_PWM      BSP_IO_PORT_01_PIN_00

/* 与 lcd.c 使用相同的硬件配置；各示例只启用一个 INIT_APP_EXPORT。 */
static display_cfg_t lvgl_display_cfg;
static lv_color_t lvgl_draw_pixels[LVGL_LCD_WIDTH * LVGL_DRAW_LINES];
static lv_disp_draw_buf_t lvgl_draw_buffer;
static lv_disp_drv_t lvgl_display_driver;
static lv_obj_t *lvgl_progress;
static lv_obj_t *lvgl_value_label;
static lv_obj_t *lvgl_runtime_label;
static volatile rt_uint32_t lvgl_flush_count;
static volatile rt_bool_t lvgl_ready;
static rt_uint32_t lvgl_ui_updates;

static void lvgl_lcd_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    int x1 = area->x1 < 0 ? 0 : area->x1;
    int y1 = area->y1 < 0 ? 0 : area->y1;
    int x2 = area->x2 >= LVGL_LCD_WIDTH ? LVGL_LCD_WIDTH - 1 : area->x2;
    int y2 = area->y2 >= LVGL_LCD_HEIGHT ? LVGL_LCD_HEIGHT - 1 : area->y2;
    int source_stride = area->x2 - area->x1 + 1;
    int y;
    rt_uint16_t *framebuffer = (rt_uint16_t *) fb_background[0];

    if (x1 <= x2 && y1 <= y2)
    {
        for (y = y1; y <= y2; y++)
        {
            rt_memcpy(&framebuffer[y * DISPLAY_BUFFER_STRIDE_PIXELS_INPUT0 + x1],
                      &pixels[(y - area->y1) * source_stride + x1 - area->x1],
                      (x2 - x1 + 1) * sizeof(lv_color_t));
        }
        lvgl_flush_count++;
    }
    /* CPU 拷贝已经结束，LVGL 可以复用这块局部绘图缓冲区。 */
    lv_disp_flush_ready(driver);
}

static fsp_err_t lvgl_lcd_init(void)
{
    fsp_err_t err;
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, LVGL_BL_EN, IOPORT_CFG_PORT_DIRECTION_OUTPUT);
    if (err != FSP_SUCCESS) return err;
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, LVGL_BL_PWM, IOPORT_CFG_PORT_DIRECTION_OUTPUT);
    if (err != FSP_SUCCESS) return err;

    rt_memset(fb_background, 0, sizeof(fb_background));
    lvgl_display_cfg = g_display0_cfg;
    lvgl_display_cfg.output.htiming.total_cyc = 531;
    lvgl_display_cfg.output.htiming.back_porch = 43;
    lvgl_display_cfg.output.htiming.sync_width = 2;
    lvgl_display_cfg.output.vtiming.total_cyc = 292;
    lvgl_display_cfg.output.vtiming.back_porch = 12;
    lvgl_display_cfg.output.vtiming.sync_width = 2;

    err = R_GLCDC_Open(&g_display0_ctrl, &lvgl_display_cfg);
    if (err != FSP_SUCCESS) return err;
    return R_GLCDC_Start(&g_display0_ctrl);
}

static lv_obj_t *lvgl_label(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE8EDF5), 0);
    return label;
}

static void lvgl_update(lv_timer_t *timer)
{
    unsigned int phase, value;
    RT_UNUSED(timer);
    lvgl_ui_updates++;
    phase = lvgl_ui_updates % 200;
    value = phase <= 100 ? phase : 200 - phase;
    lv_bar_set_value(lvgl_progress, value, LV_ANIM_OFF);
    lv_label_set_text_fmt(lvgl_value_label, "Refresh test: %u%%", value);
    if (lvgl_ui_updates % 10 == 0)
        lv_label_set_text_fmt(lvgl_runtime_label, "Running: %lu s   Updates: %lu",
                             (unsigned long) (lvgl_ui_updates / 10),
                             (unsigned long) lvgl_ui_updates);
    if (lvgl_ui_updates % 50 == 0)
        rt_kprintf("LVGL: updates=%u flushes=%u\n", lvgl_ui_updates, lvgl_flush_count);
}

static rt_err_t lvgl_ui_create(void)
{
    static const rt_uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000};
    static const char *const names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *title;
    int i;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101925), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    title = lvgl_label(screen, 16, 10, "ST7282 / LVGL 8.3");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lvgl_label(screen, 16, 40, "480 x 272   |   RGB565   |   DISPLAY TEST");
    for (i = 0; i < 5; i++)
    {
        lv_obj_t *tile = lv_obj_create(screen);
        lv_obj_t *name;
        lv_obj_set_pos(tile, 16 + i * 90, 76);
        lv_obj_set_size(tile, 84, 54);
        lv_obj_set_style_bg_color(tile, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x778899), 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_radius(tile, 6, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        name = lvgl_label(tile, 0, 0, names[i]);
        lv_obj_set_style_text_color(name, i == 1 || i == 3 ? lv_color_black() : lv_color_white(), 0);
        lv_obj_center(name);
    }
    lvgl_progress = lv_bar_create(screen);
    lv_obj_set_pos(lvgl_progress, 16, 158);
    lv_obj_set_size(lvgl_progress, 448, 18);
    lv_bar_set_range(lvgl_progress, 0, 100);
    lv_obj_set_style_bg_color(lvgl_progress, lv_color_hex(0x2AD6B3), LV_PART_INDICATOR);
    lvgl_value_label = lvgl_label(screen, 16, 190, "Refresh test: 0%");
    lvgl_runtime_label = lvgl_label(screen, 16, 224, "Running: 0 s   Updates: 0");
    return lv_timer_create(lvgl_update, 100, RT_NULL) ? RT_EOK : -RT_ENOMEM;
}

static void lvgl_display_thread(void *parameter)
{
    fsp_err_t err;
    lv_disp_t *display;
    RT_UNUSED(parameter);
    err = lvgl_lcd_init();
    if (err != FSP_SUCCESS)
    {
        rt_kprintf("LVGL: GLCDC init failed: %d\n", (int) err);
        return;
    }

    lv_init();
    lv_disp_draw_buf_init(&lvgl_draw_buffer, lvgl_draw_pixels, RT_NULL,
                          LVGL_LCD_WIDTH * LVGL_DRAW_LINES);
    lv_disp_drv_init(&lvgl_display_driver);
    lvgl_display_driver.hor_res = LVGL_LCD_WIDTH;
    lvgl_display_driver.ver_res = LVGL_LCD_HEIGHT;
    lvgl_display_driver.draw_buf = &lvgl_draw_buffer;
    lvgl_display_driver.flush_cb = lvgl_lcd_flush;
    display = lv_disp_drv_register(&lvgl_display_driver);
    if (display == RT_NULL || lvgl_ui_create() != RT_EOK)
    {
        rt_kprintf("LVGL: display/UI allocation failed\n");
        return;
    }
    lv_refr_now(display);
    rt_thread_mdelay(150);
    err = R_IOPORT_PinWrite(&g_ioport_ctrl, LVGL_BL_EN, BSP_IO_LEVEL_HIGH);
    if (err == FSP_SUCCESS)
    {
        rt_thread_mdelay(10);
        err = R_IOPORT_PinWrite(&g_ioport_ctrl, LVGL_BL_PWM, BSP_IO_LEVEL_HIGH);
    }
    if (err != FSP_SUCCESS)
    {
        rt_kprintf("LVGL: backlight enable failed: %d\n", (int) err);
        return;
    }
    lvgl_ready = RT_TRUE;
    rt_kprintf("LVGL: ready 480x272 RGB565, draw buffer=%u bytes\n",
               (unsigned int) sizeof(lvgl_draw_pixels));
    for (;;)
    {
        lv_timer_handler();
        rt_thread_mdelay(5);
    }
}

static int lcd_lvgl_start(void)
{
    rt_err_t err;
    rt_thread_t thread = rt_thread_create("lcdlvgl", lvgl_display_thread, RT_NULL, 4096, 20, 10);
    if (thread == RT_NULL) return -RT_ENOMEM;
    err = rt_thread_startup(thread);
    if (err != RT_EOK) rt_thread_delete(thread);
    return err;
}
// INIT_APP_EXPORT(lcd_lvgl_start);

static void lvgl_status(void)
{
    rt_kprintf("LVGL: ready=%d flushes=%u\n", lvgl_ready, lvgl_flush_count);
}
MSH_CMD_EXPORT(lvgl_status, Show LVGL display status);
