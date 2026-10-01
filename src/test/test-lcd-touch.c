/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-lcd-touch.c
 * @brief 不用 LVGL 的 RGB565 多指绘画例程。
 *
 * 选择色块后画线，CLEAR 清屏；每指独立轨迹，抬手断线；60 秒后清理退出。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>
#include <drivers/i2c.h>
#include <drivers/pin.h>

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

static rt_bool_t display_open;

/* Standalone RGB565 paint demo: no LVGL calls or headers. */
#define LT_WIDTH 480
#define LT_HEIGHT 272
#define LT_CANVAS_TOP 80
#define LT_CANVAS_END 240
#define LT_INT BSP_IO_PORT_00_PIN_04
#define LT_RST BSP_IO_PORT_08_PIN_01
#define LT_BL_EN BSP_IO_PORT_01_PIN_05
#define LT_BL_PWM BSP_IO_PORT_01_PIN_00
#define LT_BG 0x18E3
#define LT_PAPER 0xC618
#define LT_WHITE 0xFFFF
#define LT_STATUS 0x814E
#define LT_POINTS 0x814F
#define LT_MAX_POINTS 5
#define LT_IDS 16

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
static volatile rt_uint32_t lt_frames;
static volatile rt_uint32_t lt_errors;
static volatile rt_uint32_t lt_clears;
static volatile rt_bool_t lt_ready;
static volatile int lt_x;
static volatile int lt_y;
static volatile int lt_count;

/* Five columns per glyph, bit 0 at the top. Only uppercase ASCII is needed. */
static const rt_uint8_t lt_font[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}};

/* 裁剪矩形范围后写 RGB565 像素；按实际行跨度寻址，避免跨行错位。 */
static void lt_rect(int x, int y, int width, int height, rt_uint16_t color)
{
    int xx;
    int yy;
    int right = x + width;
    int bottom = y + height;
    rt_uint16_t *fb = (rt_uint16_t *)fb_background[0];
    if (x < 0)
    {
        x = 0;
    }
    if (y < 0)
    {
        y = 0;
    }
    if (right > LT_WIDTH)
    {
        right = LT_WIDTH;
    }
    if (bottom > LT_HEIGHT)
    {
        bottom = LT_HEIGHT;
    }
    for (yy = y; yy < bottom; yy++)
    {
        for (xx = x; xx < right; xx++)
        {
            fb[yy * DISPLAY_BUFFER_STRIDE_PIXELS_INPUT0 + xx] = color;
        }
    }
}

/* 用内嵌的 5×7 点阵绘制大写字母和数字，不依赖图形库。 */
static void lt_text(int x, int y, const char *text, int scale, rt_uint16_t color)
{
    for (; *text; text++, x += 6 * scale)
    {
        int column;
        int row;
        int index;
        if (*text >= '0' && *text <= '9')
        {
            index = *text - '0';
        }
        else if (*text >= 'A' && *text <= 'Z')
        {
            index = *text - 'A' + 10;
        }
        else
        {
            continue;
        }
        for (column = 0; column < 5; column++)
        {
            for (row = 0; row < 7; row++)
            {
                if (lt_font[index][column] & (1 << row))
                {
                    lt_rect(x + column * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

/* 绘制颜色按钮和 CLEAR；选中颜色使用不同边框。 */
static void lt_toolbar(void)
{
    static const char *const labels[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK", "CLEAR"};
    int i;
    for (i = 0; i < 6; i++)
    {
        int x = i * 80 + 3;
        if (i == (int)lt_color)
        {
            lt_rect(x, 32, 74, 42, 0xFFE0);
        }
        else
        {
            lt_rect(x, 32, 74, 42, LT_WHITE);
        }
        if (i < 5)
        {
            lt_rect(x + 3, 35, 68, 36, lt_colors[i]);
        }
        else
        {
            lt_rect(x + 3, 35, 68, 36, LT_BG);
        }
        if (i == 1 || i == 3)
        {
            lt_text(x + (74 - (int)rt_strlen(labels[i]) * 6) / 2, 49, labels[i], 1, 0);
        }
        else
        {
            lt_text(x + (74 - (int)rt_strlen(labels[i]) * 6) / 2, 49, labels[i], 1, LT_WHITE);
        }
    }
}

/* 显示当前坐标、触点数和心跳，便于观察刷新是否持续。 */
static void lt_footer(rt_bool_t heartbeat)
{
    char text[40];
    lt_rect(0, 240, LT_WIDTH, 32, LT_BG);
    rt_snprintf(text, sizeof(text), "X %03d  Y %03d  N %d", lt_x, lt_y, lt_count);
    lt_text(8, 250, text, 2, LT_WHITE);
    if (heartbeat)
    {
        lt_rect(462, 252, 10, 10, 0x07E0);
    }
    else
    {
        lt_rect(462, 252, 10, 10, LT_BG);
    }
}

/* 绘制小笔刷并限制在画布区域内，避免覆盖工具栏。 */
static void lt_brush(int x, int y)
{
    int top = y - 2;
    int bottom = y + 3;
    if (top < LT_CANVAS_TOP)
    {
        top = LT_CANVAS_TOP;
    }
    if (bottom > LT_CANVAS_END)
    {
        bottom = LT_CANVAS_END;
    }
    lt_rect(x - 2, top, 5, bottom - top, lt_colors[lt_color]);
}

/* 用整数 Bresenham 算法补齐两次采样间的像素，快速拖动时也保持连续。 */
static void lt_line(int x0, int y0, int x1, int y1)
{
    int dx;
    if (x1 > x0)
    {
        dx = x1 - x0;
    }
    else
    {
        dx = x0 - x1;
    }
    int dy;
    if (y1 > y0)
    {
        dy = y0 - y1;
    }
    else
    {
        dy = y1 - y0;
    }
    int sx;
    if (x0 < x1)
    {
        sx = 1;
    }
    else
    {
        sx = -1;
    }
    int sy;
    if (y0 < y1)
    {
        sy = 1;
    }
    else
    {
        sy = -1;
    }
    int error = dx + dy;
    for (;;)
    {
        int twice;
        lt_brush(x0, y0);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }
        twice = 2 * error;
        if (twice >= dy)
        {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx)
        {
            error += dx;
            y0 += sy;
        }
    }
}

/* 先发送大端寄存器地址，再以重复起始读取 GT911 数据。 */
static rt_err_t lt_read(rt_uint16_t reg, rt_uint8_t *data, rt_uint16_t len)
{
    rt_uint8_t address[] = {(rt_uint8_t)(reg >> 8), (rt_uint8_t)reg};
    struct rt_i2c_msg messages[2] = {{lt_address, RT_I2C_WR, sizeof(address), address},
                                     {lt_address, RT_I2C_RD, len, data}};
    if (rt_i2c_transfer(lt_bus, messages, 2) == 2)
    {
        return RT_EOK;
    }
    else
    {
        return -RT_EIO;
    }
}

/* 向 0x814E 写 0，允许 GT911 发布下一帧触点。 */
static rt_err_t lt_ack(void)
{
    rt_uint8_t data[] = {0x81, 0x4E, 0};
    struct rt_i2c_msg message = {lt_address, RT_I2C_WR, sizeof(data), data};
    if (rt_i2c_transfer(lt_bus, &message, 1) == 1)
    {
        return RT_EOK;
    }
    else
    {
        return -RT_EIO;
    }
}

/* IRQ 仅释放信号量唤醒线程；不在中断中进行 I2C 传输。 */
static void lt_irq(void *parameter)
{
    RT_UNUSED(parameter);
    rt_sem_release(&lt_sem);
}

/* 复位 GT911、探测地址和坐标范围，然后注册 P004 下降沿中断。 */
static rt_err_t lt_touch_init(void)
{
    static const rt_uint16_t addresses[] = {0x5D, 0x14};
    rt_uint8_t id[4];
    rt_uint8_t range[4];
    unsigned int i;
    lt_bus = rt_i2c_bus_device_find("i2c1");
    if (lt_bus == RT_NULL)
    {
        return -RT_ENOSYS;
    }

    /* Configure individual pins: rt_pin_mode would reopen the whole IOPORT. */
    if (R_IOPORT_PinCfg(&g_ioport_ctrl,
                        LT_RST,
                        IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) !=
            FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl,
                        LT_INT,
                        IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH) !=
            FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rt_thread_mdelay(10);
    if (R_IOPORT_PinWrite(&g_ioport_ctrl, LT_RST, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rt_thread_mdelay(100);
    if (R_IOPORT_PinCfg(&g_ioport_ctrl,
                        LT_INT,
                        IOPORT_CFG_IRQ_ENABLE | IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++)
    {
        lt_address = addresses[i];
        if (lt_read(0x8140, id, sizeof(id)) == RT_EOK && id[0] == '9' && id[1] == '1' &&
            id[2] == '1' && id[3] == 0)
        {
            break;
        }
    }
    if (i == sizeof(addresses) / sizeof(addresses[0]))
    {
        return -RT_EIO;
    }
    if (lt_read(0x8048, range, sizeof(range)) != RT_EOK)
    {
        return -RT_EIO;
    }
    rt_kprintf("LCD-TOUCH: GT911 at 0x%02X, range=%ux%u\n",
               lt_address,
               range[0] | (range[1] << 8),
               range[2] | (range[3] << 8));
    if ((range[0] | (range[1] << 8)) != LT_WIDTH || (range[2] | (range[3] << 8)) != LT_HEIGHT)
    {
        rt_kprintf("LCD-TOUCH: unexpected coordinate range; check GT911 configuration\n");
        return -RT_EINVAL;
    }
    if (lt_ack() != RT_EOK)
    {
        return -RT_EIO;
    }
    if (rt_pin_attach_irq(LT_INT, PIN_IRQ_MODE_FALLING, lt_irq, RT_NULL) != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (rt_pin_irq_enable(LT_INT, RT_TRUE) != RT_EOK)
    {
        rt_pin_detach_irq(LT_INT);
        return -RT_ERROR;
    }
    return RT_EOK;
}

/* 绘制初始画布并启动 GLCDC，等待扫描稳定后开启背光。 */
static fsp_err_t lt_lcd_init(void)
{
    fsp_err_t error;
    error = R_IOPORT_PinCfg(
        &g_ioport_ctrl, LT_BL_EN, IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    if (error != FSP_SUCCESS)
    {
        return error;
    }
    error = R_IOPORT_PinCfg(
        &g_ioport_ctrl, LT_BL_PWM, IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    if (error != FSP_SUCCESS)
    {
        return error;
    }
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
    if (error != FSP_SUCCESS)
    {
        return error;
    }
    display_open = RT_TRUE;
    display_open = RT_TRUE;
    error = R_GLCDC_Start(&g_display0_ctrl);
    if (error != FSP_SUCCESS)
    {
        return error;
    }
    rt_thread_mdelay(150);
    error = R_IOPORT_PinWrite(&g_ioport_ctrl, LT_BL_EN, BSP_IO_LEVEL_HIGH);
    if (error != FSP_SUCCESS)
    {
        return error;
    }
    rt_thread_mdelay(10);
    return R_IOPORT_PinWrite(&g_ioport_ctrl, LT_BL_PWM, BSP_IO_LEVEL_HIGH);
}

/* 清空所有指头的上次轨迹，抬手或坏帧后避免跨位置误连线。 */
static void lt_forget_points(void)
{
    lt_active = 0;
    lt_count = 0;
    rt_memset(lt_previous, 0, sizeof(lt_previous));
}

/* Return true when the footer needs updating. Validate a whole frame before drawing. */
static rt_bool_t lt_sample(void)
{
    rt_uint8_t status;
    rt_uint8_t data[LT_MAX_POINTS * 8];
    rt_uint16_t seen = 0;
    int i;
    int count;
    rt_bool_t cleared = RT_FALSE;
    if (lt_read(LT_STATUS, &status, 1) != RT_EOK)
    {
        goto failed;
    }
    if (!(status & 0x80))
    {
        return RT_FALSE;
    }
    count = status & 0x0F;
    if (count > LT_MAX_POINTS)
    {
        lt_ack();
        goto failed;
    }
    /* Keep the frame pending on a read error, so the next poll can retry it. */
    if (count && lt_read(LT_POINTS, data, count * 8) != RT_EOK)
    {
        goto failed;
    }
    if (lt_ack() != RT_EOK)
    {
        goto failed;
    }
    for (i = 0; i < count; i++)
    {
        const rt_uint8_t *point = &data[i * 8];
        int id = point[0] & 15;
        int x = point[1] | (point[2] << 8);
        int y = point[3] | (point[4] << 8);
        if (x >= LT_WIDTH || y >= LT_HEIGHT || (seen & (1u << id)))
        {
            goto failed;
        }
        seen |= 1u << id;
    }
    for (i = 0; i < count; i++)
    {
        const rt_uint8_t *point = &data[i * 8];
        int id = point[0] & 15;
        int x = point[1] | (point[2] << 8);
        int y = point[3] | (point[4] << 8);
        if (i == 0)
        {
            lt_x = x;
            lt_y = y;
        }
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
        {
            rt_kprintf("LCD-TOUCH: UP id=%d\n", i);
        }
        if (!(seen & (1u << i)) || cleared)
        {
            lt_previous[i].drawing = RT_FALSE;
        }
    }
    for (i = 0; i < count; i++)
    {
        const rt_uint8_t *point = &data[i * 8];
        int id = point[0] & 15;
        int x = point[1] | (point[2] << 8);
        int y = point[3] | (point[4] << 8);
        struct lt_point *previous = &lt_previous[id];
        if (!cleared && y >= LT_CANVAS_TOP && y < LT_CANVAS_END)
        {
            if (previous->drawing)
            {
                lt_line(previous->x, previous->y, x, y);
            }
            else
            {
                lt_brush(x, y);
            }
            previous->x = x;
            previous->y = y;
            previous->drawing = RT_TRUE;
        }
        else
        {
            previous->drawing = RT_FALSE;
        }
    }
    lt_active = seen;
    lt_count = count;
    lt_frames++;
    return RT_TRUE;
failed:
    lt_errors++;
    lt_forget_points();
    if (lt_errors == 1 || lt_errors % 100 == 0)
    {
        rt_kprintf("LCD-TOUCH: I2C/frame errors=%u\n", lt_errors);
    }
    return RT_TRUE;
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
    rt_err_t result;
    fsp_err_t display_result;
    rt_tick_t heartbeat_tick;
    rt_bool_t heartbeat = RT_FALSE;
    int outcome = -RT_ERROR;
    rt_tick_t started_at = rt_tick_get();
    display_open = RT_FALSE;
    lt_ready = RT_FALSE;
    lt_color = 0;
    lt_frames = 0;
    lt_errors = 0;
    lt_clears = 0;
    lt_count = 0;
    lt_active = 0;
    memset(lt_previous, 0, sizeof(lt_previous));
    if (rt_sem_init(&lt_sem, "paint", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return -RT_ERROR;
    }

    result = lt_touch_init();
    if (result != RT_EOK)
    {
        rt_kprintf("LCD-TOUCH: touch init failed=%d\n", result);

        goto cleanup;
    }
    display_result = lt_lcd_init();
    if (display_result != FSP_SUCCESS)
    {
        rt_kprintf("LCD-TOUCH: display init failed=%d\n", (int)display_result);

        goto cleanup;
    }
    lt_ready = RT_TRUE;
    heartbeat_tick = rt_tick_get();
    rt_kprintf("LCD-TOUCH: ready, direct RGB565; select color, draw, CLEAR\n");
    outcome = TEST_WAIT;
    while (!test_elapsed(started_at, 60000) && !test_cancelled())
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
        if (dirty)
        {
            lt_footer(heartbeat);
        }
    }

cleanup:
    /* 先关闭背光和异步外设，再释放界面、输入和信号量。 */
    rt_pin_write(LT_BL_PWM, 0);
    rt_pin_write(LT_BL_EN, 0);
    rt_pin_irq_enable(LT_INT, RT_FALSE);
    rt_pin_detach_irq(LT_INT);
    rt_sem_detach(&lt_sem);
    if (display_open && close_display() != TEST_PASS)
    {
        outcome = -RT_ERROR;
    }
    test_restore_pin(BSP_IO_PORT_00_PIN_04);
    test_restore_pin(BSP_IO_PORT_08_PIN_01);
    lt_ready = RT_FALSE;
    return outcome;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_lcd_touch_thread(void *argument)
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
