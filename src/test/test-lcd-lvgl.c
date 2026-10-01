/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-lcd-lvgl.c
 * @brief 验证 LVGL 标签、五色色块和进度条刷新。
 *
 * 每次自行创建显示及界面；运行 60 秒或收到停止事件后释放界面和定时器。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>
#include "../../third_party/lvgl/lvgl.h"
#include "../../third_party/lvgl/src/draw/sw/lv_draw_sw.h"

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

static rt_bool_t display_open;
static lv_timer_t *lvgl_ui_timer;

#define LVGL_LCD_WIDTH DISPLAY_HSIZE_INPUT0
#define LVGL_LCD_HEIGHT DISPLAY_VSIZE_INPUT0
#define LVGL_DRAW_LINES 20
#define LVGL_BL_EN BSP_IO_PORT_01_PIN_05
#define LVGL_BL_PWM BSP_IO_PORT_01_PIN_00

/* 显示配置对象在 GLCDC 异步扫描期间一直有效。 */
static display_cfg_t lvgl_display_cfg;
static lv_color_t lvgl_draw_pixels[LVGL_LCD_WIDTH * LVGL_DRAW_LINES];
static lv_disp_draw_buf_t lvgl_draw_buffer;
static lv_disp_drv_t lvgl_display_driver;
/* 自己持有绘图上下文，避免旧版 LVGL 删除显示时遗留动态分配。 */
static lv_draw_sw_ctx_t draw_context;
static lv_obj_t *lvgl_progress;
static lv_obj_t *lvgl_value_label;
static lv_obj_t *lvgl_runtime_label;
static volatile rt_uint32_t lvgl_flush_count;
static volatile rt_bool_t lvgl_ready;
static rt_uint32_t lvgl_ui_updates;

/* 将 LVGL 脏矩形复制到 RGB565 帧缓冲；裁剪边界后通知 LVGL 复用绘图缓冲。 */
static void lvgl_lcd_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    int x1;
    if (area->x1 < 0)
    {
        x1 = 0;
    }
    else
    {
        x1 = area->x1;
    }
    int y1;
    if (area->y1 < 0)
    {
        y1 = 0;
    }
    else
    {
        y1 = area->y1;
    }
    int x2;
    if (area->x2 >= LVGL_LCD_WIDTH)
    {
        x2 = LVGL_LCD_WIDTH - 1;
    }
    else
    {
        x2 = area->x2;
    }
    int y2;
    if (area->y2 >= LVGL_LCD_HEIGHT)
    {
        y2 = LVGL_LCD_HEIGHT - 1;
    }
    else
    {
        y2 = area->y2;
    }
    int source_stride = area->x2 - area->x1 + 1;
    int y;
    rt_uint16_t *framebuffer = (rt_uint16_t *)fb_background[0];

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

/* 先关背光，配置已验收的 ST7282 时序，再打开 GLCDC。 */
static fsp_err_t lvgl_lcd_init(void)
{
    fsp_err_t err;
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, LVGL_BL_EN, IOPORT_CFG_PORT_DIRECTION_OUTPUT);
    if (err != FSP_SUCCESS)
    {
        return err;
    }
    err = R_IOPORT_PinCfg(&g_ioport_ctrl, LVGL_BL_PWM, IOPORT_CFG_PORT_DIRECTION_OUTPUT);
    if (err != FSP_SUCCESS)
    {
        return err;
    }

    rt_memset(fb_background, 0, sizeof(fb_background));
    lvgl_display_cfg = g_display0_cfg;
    lvgl_display_cfg.output.htiming.total_cyc = 531;
    lvgl_display_cfg.output.htiming.back_porch = 43;
    lvgl_display_cfg.output.htiming.sync_width = 2;
    lvgl_display_cfg.output.vtiming.total_cyc = 292;
    lvgl_display_cfg.output.vtiming.back_porch = 12;
    lvgl_display_cfg.output.vtiming.sync_width = 2;

    err = R_GLCDC_Open(&g_display0_ctrl, &lvgl_display_cfg);
    if (err != FSP_SUCCESS)
    {
        return err;
    }
    display_open = RT_TRUE;
    return R_GLCDC_Start(&g_display0_ctrl);
}

/* 创建指定位置和文字颜色的标签，保持界面配置易于阅读。 */
static lv_obj_t *lvgl_label(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE8EDF5), 0);
    return label;
}

/* 每 100 ms 更新进度条；用递增计数确认界面持续刷新。 */
static void lvgl_update(lv_timer_t *timer)
{
    unsigned int phase;
    unsigned int value;
    RT_UNUSED(timer);
    lvgl_ui_updates++;
    phase = lvgl_ui_updates % 200;
    if (phase <= 100)
    {
        value = phase;
    }
    else
    {
        value = 200 - phase;
    }
    lv_bar_set_value(lvgl_progress, value, LV_ANIM_OFF);
    lv_label_set_text_fmt(lvgl_value_label, "Refresh test: %u%%", value);
    if (lvgl_ui_updates % 10 == 0)
    {
        lv_label_set_text_fmt(lvgl_runtime_label,
                              "Running: %lu s   Updates: %lu",
                              (unsigned long)(lvgl_ui_updates / 10),
                              (unsigned long)lvgl_ui_updates);
    }
    if (lvgl_ui_updates % 50 == 0)
    {
        rt_kprintf("LVGL: updates=%u flushes=%u\n", lvgl_ui_updates, lvgl_flush_count);
    }
}

/* 创建五色色块、标签和进度条，并保存刷新定时器以便退出时删除。 */
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
        if (i == 1 || i == 3)
        {
            lv_obj_set_style_text_color(name, lv_color_black(), 0);
        }
        else
        {
            lv_obj_set_style_text_color(name, lv_color_white(), 0);
        }
        lv_obj_center(name);
    }
    lvgl_progress = lv_bar_create(screen);
    lv_obj_set_pos(lvgl_progress, 16, 158);
    lv_obj_set_size(lvgl_progress, 448, 18);
    lv_bar_set_range(lvgl_progress, 0, 100);
    lv_obj_set_style_bg_color(lvgl_progress, lv_color_hex(0x2AD6B3), LV_PART_INDICATOR);
    lvgl_value_label = lvgl_label(screen, 16, 190, "Refresh test: 0%");
    lvgl_runtime_label = lvgl_label(screen, 16, 224, "Running: 0 s   Updates: 0");
    lvgl_ui_timer = lv_timer_create(lvgl_update, 100, RT_NULL);
    if (lvgl_ui_timer == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    return RT_EOK;
}

/* GLCDC 停止请求在帧边界生效，轮询 Close，避免立即关闭导致下一次打开失败。 */
static int close_display(void)
{
    fsp_err_t error;
    rt_tick_t start = rt_tick_get();
    R_GLCDC_Stop(&g_display0_ctrl);
    do
    {
        error = R_GLCDC_Close(&g_display0_ctrl);
        if (error == FSP_SUCCESS)
        {
            return TEST_PASS;
        }
        rt_thread_mdelay(1);
    } while (!test_elapsed(start, 100));
    rt_kprintf("LCD close failed=%d; reset required\n", error);
    return -RT_ERROR;
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    fsp_err_t err;
    lv_disp_t *display = RT_NULL;
    int outcome = -RT_ERROR;
    rt_tick_t started_at = rt_tick_get();
    display_open = RT_FALSE;
    lvgl_ready = RT_FALSE;
    lvgl_ui_updates = 0;
    lvgl_flush_count = 0;
    lvgl_ui_timer = RT_NULL;

    err = lvgl_lcd_init();
    if (err != FSP_SUCCESS)
    {
        rt_kprintf("LVGL: GLCDC init failed: %d\n", (int)err);
        goto cleanup;
    }

    if (!lv_is_initialized())
    {
        lv_init();
    }
    lv_disp_draw_buf_init(
        &lvgl_draw_buffer, lvgl_draw_pixels, RT_NULL, LVGL_LCD_WIDTH * LVGL_DRAW_LINES);
    lv_disp_drv_init(&lvgl_display_driver);
    lv_draw_sw_init_ctx(&lvgl_display_driver, &draw_context.base_draw);
    lvgl_display_driver.draw_ctx = &draw_context.base_draw;
    lvgl_display_driver.hor_res = LVGL_LCD_WIDTH;
    lvgl_display_driver.ver_res = LVGL_LCD_HEIGHT;
    lvgl_display_driver.draw_buf = &lvgl_draw_buffer;
    lvgl_display_driver.flush_cb = lvgl_lcd_flush;
    display = lv_disp_drv_register(&lvgl_display_driver);
    if (display == RT_NULL || lvgl_ui_create() != RT_EOK)
    {
        rt_kprintf("LVGL: display/UI allocation failed\n");
        goto cleanup;
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
        rt_kprintf("LVGL: backlight enable failed: %d\n", (int)err);
        goto cleanup;
    }
    lvgl_ready = RT_TRUE;
    rt_kprintf("LVGL: ready 480x272 RGB565, draw buffer=%u bytes\n",
               (unsigned int)sizeof(lvgl_draw_pixels));
    outcome = TEST_WAIT;
    while (!test_elapsed(started_at, 60000) && !test_cancelled())
    {
        lv_timer_handler();
        rt_thread_mdelay(5);
    }

cleanup:
    /* 先关闭背光和异步外设，再释放界面、输入和信号量。 */
    rt_pin_write(LVGL_BL_PWM, 0);
    rt_pin_write(LVGL_BL_EN, 0);
    if (lvgl_ui_timer != RT_NULL)
    {
        lv_timer_del(lvgl_ui_timer);
        lvgl_ui_timer = RT_NULL;
    }
    if (display != RT_NULL)
    {
        lv_disp_remove(display);
    }
    if (display_open && close_display() != TEST_PASS)
    {
        outcome = -RT_ERROR;
    }
    lvgl_ready = RT_FALSE;
    return outcome;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_lcd_lvgl_thread(void *argument)
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
