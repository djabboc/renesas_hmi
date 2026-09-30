#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/i2c.h>
#include "hal_data.h"
#include "../third_party/lvgl/lvgl.h"

#define TL_LCD_WIDTH   DISPLAY_HSIZE_INPUT0
#define TL_LCD_HEIGHT  DISPLAY_VSIZE_INPUT0
#define TL_DRAW_LINES  20
#define TL_BL_EN       BSP_IO_PORT_01_PIN_05
#define TL_BL_PWM      BSP_IO_PORT_01_PIN_00

/* 与 lcd.c 使用相同的硬件配置；各示例只启用一个 INIT_APP_EXPORT。 */
static display_cfg_t tl_display_cfg;
static lv_color_t tl_draw_pixels[TL_LCD_WIDTH * TL_DRAW_LINES];
static lv_disp_draw_buf_t tl_draw_buffer;
static lv_disp_drv_t tl_display_driver;
static volatile rt_uint32_t tl_flush_count;
static volatile rt_bool_t tl_ready;

static void tl_lcd_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    int x1 = area->x1 < 0 ? 0 : area->x1;
    int y1 = area->y1 < 0 ? 0 : area->y1;
    int x2 = area->x2 >= TL_LCD_WIDTH ? TL_LCD_WIDTH - 1 : area->x2;
    int y2 = area->y2 >= TL_LCD_HEIGHT ? TL_LCD_HEIGHT - 1 : area->y2;
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
        tl_flush_count++;
    }
    /* CPU 拷贝已经结束，LVGL 可以复用这块局部绘图缓冲区。 */
    lv_disp_flush_ready(driver);
}

static fsp_err_t tl_lcd_init(void)
{
    fsp_err_t err;
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, TL_BL_EN, IOPORT_CFG_PORT_DIRECTION_OUTPUT);
    if (err != FSP_SUCCESS) return err;
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, TL_BL_PWM, IOPORT_CFG_PORT_DIRECTION_OUTPUT);
    if (err != FSP_SUCCESS) return err;

    rt_memset(fb_background, 0, sizeof(fb_background));
    tl_display_cfg = g_display0_cfg;
    tl_display_cfg.output.htiming.total_cyc = 531;
    tl_display_cfg.output.htiming.back_porch = 43;
    tl_display_cfg.output.htiming.sync_width = 2;
    tl_display_cfg.output.vtiming.total_cyc = 292;
    tl_display_cfg.output.vtiming.back_porch = 12;
    tl_display_cfg.output.vtiming.sync_width = 2;

    err = R_GLCDC_Open(&g_display0_ctrl, &tl_display_cfg);
    if (err != FSP_SUCCESS) return err;
    return R_GLCDC_Start(&g_display0_ctrl);
}

static lv_obj_t *tl_label(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE8EDF5), 0);
    return label;
}

static struct rt_i2c_bus_device *tl_bus;
static rt_uint16_t tl_address;
static lv_indev_drv_t tl_input_driver;
static lv_indev_t *tl_input;
static volatile rt_uint32_t tl_frames, tl_errors, tl_clicks;
static volatile int tl_x, tl_y, tl_points;
static volatile int tl_primary = -1;
static volatile rt_bool_t tl_pressed;
static rt_bool_t tl_wait_release;
static volatile unsigned int tl_color = 2, tl_level = 50;
static volatile rt_bool_t tl_enabled = RT_TRUE;
static const rt_uint32_t tl_colors[] = {0xED3545, 0x32D074, 0x307CF5};
static const char *const tl_names[] = {"RED", "GREEN", "BLUE"};
static lv_obj_t *tl_preview, *tl_preview_label, *tl_switch, *tl_switch_label;
static lv_obj_t *tl_slider, *tl_level_label, *tl_touch_label, *tl_counter_label;
static lv_obj_t *tl_buttons[3];
static volatile rt_bool_t tl_bench_requested;
static volatile rt_uint32_t tl_read_max_ms, tl_render_max_ms;

static void tl_monitor(lv_disp_drv_t *driver, uint32_t time, uint32_t pixels)
{
    RT_UNUSED(driver);
    RT_UNUSED(pixels);
    if (time > tl_render_max_ms) tl_render_max_ms = time;
}

static rt_err_t tl_read(rt_uint16_t reg, rt_uint8_t *data, rt_uint16_t len)
{
    rt_uint8_t address[] = {(rt_uint8_t)(reg >> 8), (rt_uint8_t)reg};
    struct rt_i2c_msg messages[2] =
    {
        {tl_address, RT_I2C_WR, sizeof(address), address},
        {tl_address, RT_I2C_RD, len, data}
    };
    return rt_i2c_transfer(tl_bus, messages, 2) == 2 ? RT_EOK : -RT_EIO;
}

static rt_err_t tl_ack(void)
{
    rt_uint8_t data[] = {0x81, 0x4E, 0};
    struct rt_i2c_msg message = {tl_address, RT_I2C_WR, sizeof(data), data};
    return rt_i2c_transfer(tl_bus, &message, 1) == 1 ? RT_EOK : -RT_EIO;
}

static rt_err_t tl_touch_init(void)
{
    static const rt_uint16_t addresses[] = {0x5D, 0x14};
    rt_uint8_t id[4], range[4];
    unsigned int i;
    tl_bus = rt_i2c_bus_device_find("i2c1");
    if (tl_bus == RT_NULL) return -RT_ENOSYS;
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_08_PIN_01,
                        IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_00_PIN_04,
                        IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH) != FSP_SUCCESS)
        return -RT_ERROR;
    rt_thread_mdelay(10);
    if (R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_08_PIN_01, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS)
        return -RT_ERROR;
    rt_thread_mdelay(100);
    /* LVGL reads the ready register every 8 ms; no second touch thread/IRQ owner. */
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_00_PIN_04,
                        IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS) return -RT_ERROR;
    for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
        tl_address = addresses[i];
        if (tl_read(0x8140, id, sizeof(id)) == RT_EOK &&
            id[0] == '9' && id[1] == '1' && id[2] == '1' && id[3] == 0) break;
    }
    if (i == sizeof(addresses) / sizeof(addresses[0])) return -RT_EIO;
    if (tl_read(0x8048, range, sizeof(range)) != RT_EOK) return -RT_EIO;
    rt_kprintf("TOUCH-LVGL: GT911 at 0x%02X, range=%ux%u\n", tl_address,
               range[0] | (range[1] << 8), range[2] | (range[3] << 8));
    if ((range[0] | (range[1] << 8)) != TL_LCD_WIDTH ||
        (range[2] | (range[3] << 8)) != TL_LCD_HEIGHT) return -RT_EINVAL;
    return tl_ack();
}

static void tl_touch_sample(void)
{
    rt_uint8_t status, points[5 * 8];
    rt_uint16_t seen = 0;
    int count, i, selected = -1;
    if (tl_read(0x814E, &status, 1) != RT_EOK) goto failed;
    if (!(status & 0x80)) return; /* Keep the last state while no new frame is ready. */
    count = status & 0x0F;
    if (count > 5)
    {
        tl_ack();
        goto failed;
    }
    if (count && tl_read(0x814F, points, count * 8) != RT_EOK) goto failed;
    if (tl_ack() != RT_EOK) goto failed;
    for (i = 0; i < count; i++)
    {
        const rt_uint8_t *point = &points[i * 8];
        int id = point[0] & 15;
        int x = point[1] | (point[2] << 8), y = point[3] | (point[4] << 8);
        if (x >= TL_LCD_WIDTH || y >= TL_LCD_HEIGHT || (seen & (1u << id))) goto failed;
        seen |= 1u << id;
        if (id == tl_primary) selected = i;
    }
    tl_frames++;
    tl_points = count;
    if (count == 0)
    {
        if (tl_pressed) rt_kprintf("TOUCH-LVGL: UP id=%d\n", tl_primary);
        tl_pressed = RT_FALSE;
        tl_wait_release = RT_FALSE;
        tl_primary = -1;
        return;
    }
    if (tl_wait_release) return;
    if (tl_primary < 0)
    {
        selected = 0;
        tl_primary = points[0] & 15;
        rt_kprintf("TOUCH-LVGL: DOWN id=%d\n", tl_primary);
    }
    if (selected < 0)
    {
        /* Never transfer an in-progress LVGL drag to another finger. */
        rt_kprintf("TOUCH-LVGL: primary UP; waiting for all fingers to release\n");
        tl_pressed = RT_FALSE;
        tl_primary = -1;
        tl_wait_release = RT_TRUE;
        return;
    }
    tl_x = points[selected * 8 + 1] | (points[selected * 8 + 2] << 8);
    tl_y = points[selected * 8 + 3] | (points[selected * 8 + 4] << 8);
    tl_pressed = RT_TRUE;
    return;
failed:
    tl_errors++;
    tl_primary = -1;
    tl_pressed = RT_FALSE;
    tl_points = 0;
    tl_wait_release = RT_TRUE;
    /* Cancel the gesture on corrupt input instead of generating a click. */
    if (tl_input != RT_NULL) lv_indev_reset(tl_input, RT_NULL);
    if (tl_errors == 1 || tl_errors % 100 == 0)
        rt_kprintf("TOUCH-LVGL: I2C/frame errors=%u\n", tl_errors);
}

static void tl_input_read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    rt_tick_t start = rt_tick_get_millisecond();
    rt_tick_t elapsed;
    RT_UNUSED(driver);
    tl_touch_sample();
    elapsed = rt_tick_get_millisecond() - start;
    if (elapsed > tl_read_max_ms) tl_read_max_ms = elapsed;
    data->point.x = tl_x;
    data->point.y = tl_y;
    data->state = tl_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
}

static void tl_update_preview_fill(void)
{
    rt_uint32_t color = tl_colors[tl_color];
    unsigned int weight = tl_enabled ? 30 + tl_level * 225 / 100 : 15;
    unsigned int red = (((color >> 16) & 255) * weight + 0x10 * (255 - weight)) / 255;
    unsigned int green = (((color >> 8) & 255) * weight + 0x19 * (255 - weight)) / 255;
    unsigned int blue = ((color & 255) * weight + 0x25 * (255 - weight)) / 255;
    /* Preblend once per value, then use an opaque fill instead of per-pixel alpha. */
    lv_obj_set_style_bg_color(tl_preview, lv_color_hex((red << 16) | (green << 8) | blue), 0);
}

static void tl_update_preview(void)
{
    unsigned int i;
    tl_update_preview_fill();
    lv_label_set_text_fmt(tl_level_label, "Level: %u%%", tl_level);
    lv_label_set_text(tl_preview_label, tl_enabled ? tl_names[tl_color] : "OFF");
    lv_label_set_text(tl_switch_label, tl_enabled ? "ENABLED" : "DISABLED");
    for (i = 0; i < 3; i++)
        lv_obj_set_style_border_width(tl_buttons[i], i == tl_color ? 3 : 0, 0);
}

static void tl_color_event(lv_event_t *event)
{
    unsigned int i;
    for (i = 0; i < 3; i++)
        if (lv_event_get_target(event) == tl_buttons[i]) tl_color = i;
    tl_clicks++;
    tl_update_preview();
    rt_kprintf("TOUCH-LVGL: color=%s clicks=%u\n", tl_names[tl_color], tl_clicks);
}

static void tl_switch_event(lv_event_t *event)
{
    RT_UNUSED(event);
    tl_enabled = lv_obj_has_state(tl_switch, LV_STATE_CHECKED);
    tl_update_preview();
    rt_kprintf("TOUCH-LVGL: enabled=%d\n", tl_enabled);
}

static void tl_slider_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED)
    {
        tl_level = lv_slider_get_value(tl_slider);
        /* Keep the drag path small. Commit the large preview fill on release. */
        lv_label_set_text_fmt(tl_level_label, "Level: %u%%", tl_level);
    }
    else if (lv_event_get_code(event) == LV_EVENT_RELEASED)
    {
        tl_update_preview_fill();
        rt_kprintf("TOUCH-LVGL: level=%u\n", tl_level);
    }
}

static void tl_reset_event(lv_event_t *event)
{
    RT_UNUSED(event);
    tl_color = 2;
    tl_level = 50;
    tl_clicks = 0;
    tl_enabled = RT_TRUE;
    lv_obj_add_state(tl_switch, LV_STATE_CHECKED);
    lv_slider_set_value(tl_slider, 50, LV_ANIM_OFF);
    tl_update_preview();
    rt_kprintf("TOUCH-LVGL: RESET blue enabled level=50 clicks=0\n");
}

static void tl_ui_tick(lv_timer_t *timer)
{
    char text[96];
    RT_UNUSED(timer);
    rt_snprintf(text, sizeof(text), "%s  X:%03d Y:%03d  N:%d ID:%d",
                tl_wait_release ? "WAIT" : tl_pressed ? "DOWN" : "UP",
                tl_x, tl_y, tl_points, tl_primary);
    if (rt_strcmp(text, lv_label_get_text(tl_touch_label))) lv_label_set_text(tl_touch_label, text);
    rt_snprintf(text, sizeof(text), "Color clicks: %lu   Touch frames: %lu   Errors: %lu",
                (unsigned long)tl_clicks, (unsigned long)tl_frames, (unsigned long)tl_errors);
    if (rt_strcmp(text, lv_label_get_text(tl_counter_label))) lv_label_set_text(tl_counter_label, text);
}

static rt_err_t tl_ui_create(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *title, *reset;
    int i;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101925), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    title = tl_label(screen, 16, 10, "LCD + TOUCH / LVGL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    tl_touch_label = tl_label(screen, 16, 43, "UP  X:000 Y:000  N:0 ID:-1");
    for (i = 0; i < 3; i++)
    {
        lv_obj_t *label;
        tl_buttons[i] = lv_btn_create(screen);
        lv_obj_set_pos(tl_buttons[i], 16 + i * 112, 74);
        lv_obj_set_size(tl_buttons[i], 100, 42);
        lv_obj_set_style_bg_color(tl_buttons[i], lv_color_hex(tl_colors[i]), 0);
        lv_obj_set_style_border_color(tl_buttons[i], lv_color_white(), 0);
        label = lv_label_create(tl_buttons[i]);
        lv_label_set_text(label, tl_names[i]);
        lv_obj_center(label);
        lv_obj_add_event_cb(tl_buttons[i], tl_color_event, LV_EVENT_CLICKED, RT_NULL);
    }
    reset = lv_btn_create(screen);
    lv_obj_set_pos(reset, 352, 74);
    lv_obj_set_size(reset, 112, 42);
    title = lv_label_create(reset);
    lv_label_set_text(title, "RESET");
    lv_obj_center(title);
    lv_obj_add_event_cb(reset, tl_reset_event, LV_EVENT_CLICKED, RT_NULL);

    tl_preview = lv_obj_create(screen);
    lv_obj_set_pos(tl_preview, 16, 132);
    lv_obj_set_size(tl_preview, 184, 92);
    lv_obj_set_style_bg_opa(tl_preview, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tl_preview, 0, 0);
    lv_obj_clear_flag(tl_preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    tl_preview_label = tl_label(tl_preview, 0, 0, "BLUE");
    lv_obj_set_style_text_font(tl_preview_label, &lv_font_montserrat_20, 0);
    lv_obj_center(tl_preview_label);
    tl_switch = lv_switch_create(screen);
    lv_obj_set_pos(tl_switch, 224, 135);
    lv_obj_set_size(tl_switch, 50, 26);
    lv_obj_add_state(tl_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(tl_switch, tl_switch_event, LV_EVENT_VALUE_CHANGED, RT_NULL);
    tl_switch_label = tl_label(screen, 288, 140, "ENABLED");
    tl_slider = lv_slider_create(screen);
    lv_obj_set_pos(tl_slider, 236, 186);
    lv_obj_set_size(tl_slider, 212, 14);
    lv_slider_set_range(tl_slider, 0, 100);
    lv_obj_set_style_anim_time(tl_slider, 0, LV_PART_MAIN);
    lv_slider_set_value(tl_slider, 50, LV_ANIM_OFF);
    lv_obj_add_event_cb(tl_slider, tl_slider_event, LV_EVENT_VALUE_CHANGED, RT_NULL);
    lv_obj_add_event_cb(tl_slider, tl_slider_event, LV_EVENT_RELEASED, RT_NULL);
    tl_level_label = tl_label(screen, 224, 213, "Level: 50%");
    tl_counter_label = tl_label(screen, 16, 248, "Color clicks: 0   Touch frames: 0   Errors: 0");
    tl_update_preview();
    return lv_timer_create(tl_ui_tick, 200, RT_NULL) ? RT_EOK : -RT_ENOMEM;
}

/* Runs in the GUI thread. Measures rendering, not physical finger-to-photon latency. */
static void tl_benchmark(lv_disp_t *display)
{
    int value;
    unsigned int saved_level = tl_level;
    rt_uint32_t total = 0, maximum = 0;
    for (value = 0; value <= 100; value += 2)
    {
        rt_tick_t start = rt_tick_get_millisecond(), elapsed;
        lv_slider_set_value(tl_slider, value, LV_ANIM_OFF);
        lv_event_send(tl_slider, LV_EVENT_VALUE_CHANGED, RT_NULL);
        lv_refr_now(display);
        elapsed = rt_tick_get_millisecond() - start;
        total += elapsed;
        if (elapsed > maximum) maximum = elapsed;
        rt_thread_mdelay(16);
    }
    lv_slider_set_value(tl_slider, saved_level, LV_ANIM_OFF);
    lv_event_send(tl_slider, LV_EVENT_VALUE_CHANGED, RT_NULL);
    tl_update_preview_fill();
    lv_refr_now(display);
    rt_kprintf("TOUCH-LVGL BENCH: 51 steps total=%u ms max=%u ms (update+render only)\n",
               total, maximum);
}

static void tl_thread(void *parameter)
{
    fsp_err_t error;
    rt_err_t touch_error;
    lv_disp_t *display;
    RT_UNUSED(parameter);
    touch_error = tl_touch_init();
    if (touch_error != RT_EOK)
    {
        rt_kprintf("TOUCH-LVGL: touch init failed=%d\n", touch_error);
        return;
    }
    error = tl_lcd_init();
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("TOUCH-LVGL: GLCDC init failed=%d\n", (int)error);
        return;
    }
    lv_init();
    lv_disp_draw_buf_init(&tl_draw_buffer, tl_draw_pixels, RT_NULL, TL_LCD_WIDTH * TL_DRAW_LINES);
    lv_disp_drv_init(&tl_display_driver);
    tl_display_driver.hor_res = TL_LCD_WIDTH;
    tl_display_driver.ver_res = TL_LCD_HEIGHT;
    tl_display_driver.draw_buf = &tl_draw_buffer;
    tl_display_driver.flush_cb = tl_lcd_flush;
    tl_display_driver.monitor_cb = tl_monitor;
    display = lv_disp_drv_register(&tl_display_driver);
    if (display == RT_NULL || tl_ui_create() != RT_EOK)
    {
        rt_kprintf("TOUCH-LVGL: display/UI allocation failed\n");
        return;
    }
    lv_indev_drv_init(&tl_input_driver);
    tl_input_driver.type = LV_INDEV_TYPE_POINTER;
    tl_input_driver.disp = display;
    tl_input_driver.read_cb = tl_input_read;
    tl_input = lv_indev_drv_register(&tl_input_driver);
    if (tl_input == RT_NULL || tl_input_driver.read_timer == RT_NULL)
    {
        rt_kprintf("TOUCH-LVGL: input allocation failed\n");
        return;
    }
    lv_timer_set_period(tl_input_driver.read_timer, 8);
    lv_timer_set_period(display->refr_timer, 16);
    lv_refr_now(display);
    rt_thread_mdelay(150);
    error = R_IOPORT_PinWrite(&g_ioport_ctrl, TL_BL_EN, BSP_IO_LEVEL_HIGH);
    if (error == FSP_SUCCESS)
    {
        rt_thread_mdelay(10);
        error = R_IOPORT_PinWrite(&g_ioport_ctrl, TL_BL_PWM, BSP_IO_LEVEL_HIGH);
    }
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("TOUCH-LVGL: backlight failed=%d\n", (int)error);
        return;
    }
    tl_ready = RT_TRUE;
    rt_kprintf("TOUCH-LVGL: ready, color buttons / switch / slider / RESET\n");
    for (;;)
    {
        if (tl_bench_requested)
        {
            tl_bench_requested = RT_FALSE;
            if (!tl_pressed && !tl_points) tl_benchmark(display);
            else rt_kprintf("TOUCH-LVGL: benchmark skipped while touching\n");
        }
        lv_timer_handler();
        rt_thread_mdelay(1);
    }
}

static int lcd_touch_lvgl_start(void)
{
    rt_err_t error;
    rt_thread_t thread = rt_thread_create("touchlv", tl_thread, RT_NULL, 4096, 20, 10);
    if (thread == RT_NULL) return -RT_ENOMEM;
    error = rt_thread_startup(thread);
    if (error != RT_EOK) rt_thread_delete(thread);
    return error;
}
INIT_APP_EXPORT(lcd_touch_lvgl_start);

static void touch_lvgl_status(void)
{
    rt_kprintf("TOUCH-LVGL: ready=%d frames=%u errors=%u flushes=%u points=%d id=%d pressed=%d x=%d y=%d\n",
               tl_ready, tl_frames, tl_errors, tl_flush_count, tl_points, tl_primary, tl_pressed, tl_x, tl_y);
    rt_kprintf("TOUCH-LVGL: color=%s enabled=%d level=%u clicks=%u\n",
               tl_names[tl_color], tl_enabled, tl_level, tl_clicks);
    rt_kprintf("TOUCH-LVGL: read_max=%u ms render_max=%u ms\n", tl_read_max_ms, tl_render_max_ms);
}
MSH_CMD_EXPORT(touch_lvgl_status, Show LCD touch LVGL demo status);

static void touch_lvgl_bench(void)
{
    if (tl_ready) tl_bench_requested = RT_TRUE;
    else rt_kprintf("TOUCH-LVGL: not ready\n");
}
MSH_CMD_EXPORT(touch_lvgl_bench, Measure slider rendering with an automatic sweep);

