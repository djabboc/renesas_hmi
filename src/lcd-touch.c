#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/i2c.h>
#include <drivers/pin.h>
#include "hal_data.h"

/* Standalone RGB565 paint demo: no LVGL calls or headers. */
#define LT_WIDTH       480
#define LT_HEIGHT      272
#define LT_CANVAS_TOP  80
#define LT_CANVAS_END  240
#define LT_INT         BSP_IO_PORT_00_PIN_04
#define LT_RST         BSP_IO_PORT_08_PIN_01
#define LT_BL_EN       BSP_IO_PORT_01_PIN_05
#define LT_BL_PWM      BSP_IO_PORT_01_PIN_00
#define LT_BG          0x18E3
#define LT_PAPER       0xC618
#define LT_WHITE       0xFFFF
#define LT_STATUS      0x814E
#define LT_POINTS      0x814F
#define LT_MAX_POINTS  5
#define LT_IDS         16

struct lt_point
{
    int x, y;
    rt_bool_t drawing;
};

static display_cfg_t lt_display_cfg;
static struct rt_i2c_bus_device *lt_bus;
static struct rt_semaphore lt_sem;
static rt_uint16_t lt_address;
static struct lt_point lt_previous[LT_IDS];
static rt_uint16_t lt_active;
static unsigned int lt_color;
static const rt_uint16_t lt_colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
static volatile rt_uint32_t lt_frames, lt_errors, lt_clears;
static volatile rt_bool_t lt_ready;
static volatile int lt_x, lt_y, lt_count;

/* Five columns per glyph, bit 0 at the top. Only uppercase ASCII is needed. */
static const rt_uint8_t lt_font[][5] =
{
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}
};

static void lt_rect(int x, int y, int width, int height, rt_uint16_t color)
{
    int xx, yy, right = x + width, bottom = y + height;
    rt_uint16_t *fb = (rt_uint16_t *)fb_background[0];
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > LT_WIDTH) right = LT_WIDTH;
    if (bottom > LT_HEIGHT) bottom = LT_HEIGHT;
    for (yy = y; yy < bottom; yy++)
        for (xx = x; xx < right; xx++)
            fb[yy * DISPLAY_BUFFER_STRIDE_PIXELS_INPUT0 + xx] = color;
}

static void lt_text(int x, int y, const char *text, int scale, rt_uint16_t color)
{
    for (; *text; text++, x += 6 * scale)
    {
        int column, row, index;
        if (*text >= '0' && *text <= '9') index = *text - '0';
        else if (*text >= 'A' && *text <= 'Z') index = *text - 'A' + 10;
        else continue;
        for (column = 0; column < 5; column++)
            for (row = 0; row < 7; row++)
                if (lt_font[index][column] & (1 << row))
                    lt_rect(x + column * scale, y + row * scale, scale, scale, color);
    }
}

static void lt_toolbar(void)
{
    static const char *const labels[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK", "CLEAR"};
    int i;
    for (i = 0; i < 6; i++)
    {
        int x = i * 80 + 3;
        lt_rect(x, 32, 74, 42, i == (int)lt_color ? 0xFFE0 : LT_WHITE);
        lt_rect(x + 3, 35, 68, 36, i < 5 ? lt_colors[i] : LT_BG);
        lt_text(x + (74 - (int)rt_strlen(labels[i]) * 6) / 2, 49,
                labels[i], 1, i == 1 || i == 3 ? 0 : LT_WHITE);
    }
}

static void lt_footer(rt_bool_t heartbeat)
{
    char text[40];
    lt_rect(0, 240, LT_WIDTH, 32, LT_BG);
    rt_snprintf(text, sizeof(text), "X %03d  Y %03d  N %d", lt_x, lt_y, lt_count);
    lt_text(8, 250, text, 2, LT_WHITE);
    lt_rect(462, 252, 10, 10, heartbeat ? 0x07E0 : LT_BG);
}

static void lt_brush(int x, int y)
{
    int top = y - 2, bottom = y + 3;
    if (top < LT_CANVAS_TOP) top = LT_CANVAS_TOP;
    if (bottom > LT_CANVAS_END) bottom = LT_CANVAS_END;
    lt_rect(x - 2, top, 5, bottom - top, lt_colors[lt_color]);
}

static void lt_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;)
    {
        int twice;
        lt_brush(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

static rt_err_t lt_read(rt_uint16_t reg, rt_uint8_t *data, rt_uint16_t len)
{
    rt_uint8_t address[] = {(rt_uint8_t)(reg >> 8), (rt_uint8_t)reg};
    struct rt_i2c_msg messages[2] =
    {
        {lt_address, RT_I2C_WR, sizeof(address), address},
        {lt_address, RT_I2C_RD, len, data}
    };
    return rt_i2c_transfer(lt_bus, messages, 2) == 2 ? RT_EOK : -RT_EIO;
}

static rt_err_t lt_ack(void)
{
    rt_uint8_t data[] = {0x81, 0x4E, 0};
    struct rt_i2c_msg message = {lt_address, RT_I2C_WR, sizeof(data), data};
    return rt_i2c_transfer(lt_bus, &message, 1) == 1 ? RT_EOK : -RT_EIO;
}

static void lt_irq(void *parameter)
{
    RT_UNUSED(parameter);
    rt_sem_release(&lt_sem);
}

static rt_err_t lt_touch_init(void)
{
    static const rt_uint16_t addresses[] = {0x5D, 0x14};
    rt_uint8_t id[4], range[4];
    unsigned int i;
    lt_bus = rt_i2c_bus_device_find("i2c1");
    if (lt_bus == RT_NULL) return -RT_ENOSYS;

    /* Configure individual pins: rt_pin_mode would reopen the whole IOPORT. */
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, LT_RST, IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                        IOPORT_CFG_PORT_OUTPUT_LOW) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, LT_INT, IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                        IOPORT_CFG_PORT_OUTPUT_HIGH) != FSP_SUCCESS) return -RT_ERROR;
    rt_thread_mdelay(10);
    if (R_IOPORT_PinWrite(&g_ioport_ctrl, LT_RST, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS)
        return -RT_ERROR;
    rt_thread_mdelay(100);
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, LT_INT, IOPORT_CFG_IRQ_ENABLE |
                        IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS) return -RT_ERROR;
    for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
        lt_address = addresses[i];
        if (lt_read(0x8140, id, sizeof(id)) == RT_EOK &&
            id[0] == '9' && id[1] == '1' && id[2] == '1' && id[3] == 0) break;
    }
    if (i == sizeof(addresses) / sizeof(addresses[0])) return -RT_EIO;
    if (lt_read(0x8048, range, sizeof(range)) != RT_EOK) return -RT_EIO;
    rt_kprintf("LCD-TOUCH: GT911 at 0x%02X, range=%ux%u\n", lt_address,
               range[0] | (range[1] << 8), range[2] | (range[3] << 8));
    if ((range[0] | (range[1] << 8)) != LT_WIDTH ||
        (range[2] | (range[3] << 8)) != LT_HEIGHT)
    {
        rt_kprintf("LCD-TOUCH: unexpected coordinate range; check GT911 configuration\n");
        return -RT_EINVAL;
    }
    if (lt_ack() != RT_EOK) return -RT_EIO;
    if (rt_pin_attach_irq(LT_INT, PIN_IRQ_MODE_FALLING, lt_irq, RT_NULL) != RT_EOK)
        return -RT_ERROR;
    if (rt_pin_irq_enable(LT_INT, RT_TRUE) != RT_EOK)
    {
        rt_pin_detach_irq(LT_INT);
        return -RT_ERROR;
    }
    return RT_EOK;
}

static fsp_err_t lt_lcd_init(void)
{
    fsp_err_t error;
    error = R_IOPORT_PinCfg(&g_ioport_ctrl, LT_BL_EN,
                            IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    if (error != FSP_SUCCESS) return error;
    error = R_IOPORT_PinCfg(&g_ioport_ctrl, LT_BL_PWM,
                            IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    if (error != FSP_SUCCESS) return error;
    lt_rect(0, 0, LT_WIDTH, LT_HEIGHT, LT_BG);
    lt_text(8, 8, "LCD TOUCH DRAW", 2, LT_WHITE);
    lt_toolbar();
    lt_rect(0, LT_CANVAS_TOP, LT_WIDTH, LT_CANVAS_END - LT_CANVAS_TOP, LT_PAPER);
    lt_footer(RT_FALSE);
    lt_display_cfg = g_display0_cfg;
    lt_display_cfg.output.htiming.total_cyc = 531;
    lt_display_cfg.output.htiming.back_porch = 43;
    lt_display_cfg.output.htiming.sync_width = 2;
    lt_display_cfg.output.vtiming.total_cyc = 292;
    lt_display_cfg.output.vtiming.back_porch = 12;
    lt_display_cfg.output.vtiming.sync_width = 2;
    error = R_GLCDC_Open(&g_display0_ctrl, &lt_display_cfg);
    if (error != FSP_SUCCESS) return error;
    error = R_GLCDC_Start(&g_display0_ctrl);
    if (error != FSP_SUCCESS) return error;
    rt_thread_mdelay(150);
    error = R_IOPORT_PinWrite(&g_ioport_ctrl, LT_BL_EN, BSP_IO_LEVEL_HIGH);
    if (error != FSP_SUCCESS) return error;
    rt_thread_mdelay(10);
    return R_IOPORT_PinWrite(&g_ioport_ctrl, LT_BL_PWM, BSP_IO_LEVEL_HIGH);
}

static void lt_forget_points(void)
{
    lt_active = 0;
    lt_count = 0;
    rt_memset(lt_previous, 0, sizeof(lt_previous));
}

/* Return true when the footer needs updating. Validate a whole frame before drawing. */
static rt_bool_t lt_sample(void)
{
    rt_uint8_t status, data[LT_MAX_POINTS * 8];
    rt_uint16_t seen = 0;
    int i, count;
    rt_bool_t cleared = RT_FALSE;
    if (lt_read(LT_STATUS, &status, 1) != RT_EOK) goto failed;
    if (!(status & 0x80)) return RT_FALSE;
    count = status & 0x0F;
    if (count > LT_MAX_POINTS)
    {
        lt_ack();
        goto failed;
    }
    /* Keep the frame pending on a read error, so the next poll can retry it. */
    if (count && lt_read(LT_POINTS, data, count * 8) != RT_EOK) goto failed;
    if (lt_ack() != RT_EOK) goto failed;
    for (i = 0; i < count; i++)
    {
        const rt_uint8_t *point = &data[i * 8];
        int id = point[0] & 15;
        int x = point[1] | (point[2] << 8), y = point[3] | (point[4] << 8);
        if (x >= LT_WIDTH || y >= LT_HEIGHT || (seen & (1u << id))) goto failed;
        seen |= 1u << id;
    }
    for (i = 0; i < count; i++)
    {
        const rt_uint8_t *point = &data[i * 8];
        int id = point[0] & 15;
        int x = point[1] | (point[2] << 8), y = point[3] | (point[4] << 8);
        if (i == 0) { lt_x = x; lt_y = y; }
        if (!(lt_active & (1u << id)))
        {
            rt_kprintf("LCD-TOUCH: DOWN id=%d x=%d y=%d\n", id, x, y);
            if (y >= 32 && y < 74)
            {
                if (x < 400)
                {
                    lt_color = x / 80;
                    lt_toolbar();
                    rt_kprintf("LCD-TOUCH: color=%u\n", lt_color);
                }
                else
                {
                    lt_rect(0, LT_CANVAS_TOP, LT_WIDTH, LT_CANVAS_END - LT_CANVAS_TOP, LT_PAPER);
                    lt_clears++;
                    cleared = RT_TRUE;
                    rt_kprintf("LCD-TOUCH: clear=%u\n", lt_clears);
                }
            }
        }
    }
    for (i = 0; i < LT_IDS; i++)
    {
        if ((lt_active & (1u << i)) && !(seen & (1u << i)))
            rt_kprintf("LCD-TOUCH: UP id=%d\n", i);
        if (!(seen & (1u << i)) || cleared) lt_previous[i].drawing = RT_FALSE;
    }
    for (i = 0; i < count; i++)
    {
        const rt_uint8_t *point = &data[i * 8];
        int id = point[0] & 15;
        int x = point[1] | (point[2] << 8), y = point[3] | (point[4] << 8);
        struct lt_point *previous = &lt_previous[id];
        if (!cleared && y >= LT_CANVAS_TOP && y < LT_CANVAS_END)
        {
            if (previous->drawing) lt_line(previous->x, previous->y, x, y);
            else lt_brush(x, y);
            previous->x = x;
            previous->y = y;
            previous->drawing = RT_TRUE;
        }
        else previous->drawing = RT_FALSE;
    }
    lt_active = seen;
    lt_count = count;
    lt_frames++;
    return RT_TRUE;
failed:
    lt_errors++;
    lt_forget_points();
    if (lt_errors == 1 || lt_errors % 100 == 0)
        rt_kprintf("LCD-TOUCH: I2C/frame errors=%u\n", lt_errors);
    return RT_TRUE;
}

static void lt_thread(void *parameter)
{
    rt_err_t result;
    fsp_err_t display_result;
    rt_tick_t heartbeat_tick;
    rt_bool_t heartbeat = RT_FALSE;
    RT_UNUSED(parameter);
    result = lt_touch_init();
    if (result != RT_EOK)
    {
        rt_kprintf("LCD-TOUCH: touch init failed=%d\n", result);
        rt_sem_detach(&lt_sem);
        return;
    }
    display_result = lt_lcd_init();
    if (display_result != FSP_SUCCESS)
    {
        rt_kprintf("LCD-TOUCH: display init failed=%d\n", (int)display_result);
        rt_pin_irq_enable(LT_INT, RT_FALSE);
        rt_pin_detach_irq(LT_INT);
        rt_sem_detach(&lt_sem);
        return;
    }
    lt_ready = RT_TRUE;
    heartbeat_tick = rt_tick_get();
    rt_kprintf("LCD-TOUCH: ready, direct RGB565; select color, draw, CLEAR\n");
    for (;;)
    {
        rt_bool_t dirty;
        /* IRQ for responsiveness; timeout also recovers a missed edge/release. */
        rt_sem_take(&lt_sem, rt_tick_from_millisecond(20));
        dirty = lt_sample();
        if ((rt_tick_t)(rt_tick_get() - heartbeat_tick) >= rt_tick_from_millisecond(500))
        {
            heartbeat_tick = rt_tick_get();
            heartbeat = !heartbeat;
            dirty = RT_TRUE;
        }
        if (dirty) lt_footer(heartbeat);
    }
}

static int lcd_touch_start(void)
{
    rt_thread_t thread;
    rt_err_t result = rt_sem_init(&lt_sem, "lcdtouch", 0, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK) return result;
    thread = rt_thread_create("lcdtouch", lt_thread, RT_NULL, 3072, 20, 10);
    if (thread == RT_NULL)
    {
        rt_sem_detach(&lt_sem);
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(thread);
    if (result != RT_EOK)
    {
        rt_thread_delete(thread);
        rt_sem_detach(&lt_sem);
    }
    return result;
}
INIT_APP_EXPORT(lcd_touch_start);

static void lcd_touch_status(void)
{
    rt_kprintf("LCD-TOUCH: ready=%d frames=%u errors=%u clears=%u points=%d x=%d y=%d\n",
               lt_ready, lt_frames, lt_errors, lt_clears, lt_count, lt_x, lt_y);
}
MSH_CMD_EXPORT(lcd_touch_status, Show direct LCD and GT911 demo status);
