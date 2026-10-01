/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-pmod-i2c.c
 * @brief 用外接 SSD1306 128x64 OLED 验证 I2C 写入和持续刷新。
 *
 * 接线：3.3V、GND、P202/SCL、P203/SDA；仅适用四针 I2C 模块。
 * 独立寻找 0x3C/0x3D；边框、棋盘格、移动方块、透视立方体、循环字幕。
 * 动画只更新变化区域，约 60 秒自动结束，也可通过停止事件提前退出。
 * I2C 应答不代表图像正确，因此完整执行返回 WAIT，由用户观察屏幕验收。
 * 阅读顺序：线程入口 → run_test → 本文件的通信与绘图函数。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <math.h>

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

#define OLED_WIDTH 128u
#define OLED_HEIGHT 64u
#define OLED_PAGE_COUNT (OLED_HEIGHT / 8u)
#define OLED_BLOCK_SIZE 8u
#define OLED_FRAME_PERIOD_MS 33u
#define OLED_BLOCK_DURATION_MS 4000u
#define OLED_CUBE_DURATION_MS 16000u
#define OLED_TEXT_DURATION_MS 36000u
#define OLED_TEXT_SCALE 2u
#define OLED_GLYPH_ADVANCE (6u * OLED_TEXT_SCALE)

/* SSD1306 每字节控制同一列上的 8 个纵向像素，整屏共 8 页、1024 字节。
 * 缓冲区与总线信息属于本次调用；不引用其他测试，也不创建后台线程。 */
struct oled_display
{
    struct rt_i2c_bus_device *bus;
    rt_uint16_t address;
    rt_uint8_t pixels[OLED_PAGE_COUNT][OLED_WIDTH];
    /* shadow 是成功写入屏幕的数据；第一帧未知，必须强制写满整屏。 */
    rt_uint8_t shadow[OLED_PAGE_COUNT][OLED_WIDTH];
    int shadow_valid;
    unsigned frames;
    unsigned data_bytes;
    rt_tick_t transfer_ticks;
    rt_tick_t max_transfer_ticks;
};

/* 第一个字节是 SSD1306 控制字节：0x00 后跟命令，0x40 后跟显存数据。
 * 使用一次 I2C 写事务发送整包，每包都检查结果；失败后不继续绘图。 */
static int oled_write(struct oled_display *display, const rt_uint8_t *packet, unsigned length)
{
    rt_ssize_t sent = rt_i2c_master_send(display->bus, display->address, 0, packet, length);
    if (sent != (rt_ssize_t)length)
    {
        rt_kprintf("OLED write failed addr=0x%02X sent=%d expected=%u\n",
                   display->address, (int)sent, length);
        return -RT_EIO;
    }
    return TEST_PASS;
}

/* NOP 不改变显示内容。只尝试两个常用 OLED 地址，不扫描板载 GT911 地址。
 * ACK 只能说明该地址有应答，不能自动鉴别 SSD1306 与其他控制器。 */
static int find_oled(struct oled_display *display)
{
    const rt_uint16_t addresses[] = {0x3C, 0x3D};
    const rt_uint8_t probe[] = {0x00, 0xE3};
    for (unsigned index = 0; index < sizeof(addresses) / sizeof(addresses[0]); ++index)
    {
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        display->address = addresses[index];
        if (rt_i2c_master_send(display->bus, display->address, 0, probe, sizeof(probe)) == sizeof(probe))
        {
            rt_kprintf("OLED ACK addr=0x%02X; using SSD1306 128x64 profile\n", display->address);
            return TEST_PASS;
        }
    }
    rt_kprintf("OLED no ACK at 0x3C/0x3D; check 3.3V/GND/P202 SCL/P203 SDA\n");
    return TEST_SKIP;
}

/* 四针模块自带上电复位电路；这里重设测试用的关键显示参数。
 * 内部电荷泵适用于常见 3.3V 供电 SSD1306 模块，不适用于 SH1106。 */
static int initialize_oled(struct oled_display *display)
{
    const rt_uint8_t commands[] = {
        0x00,             /* 后续字节均为命令。 */
        0xAE,             /* 配置和清屏期间关闭显示。 */
        0xD5, 0x80,       /* 默认振荡频率、时钟分频 1。 */
        0xA8, 0x3F,       /* 64 行扫描，参数为行数减 1。 */
        0xD3, 0x00,       /* 显示垂直偏移为 0。 */
        0x40,             /* 显示起始行为 0。 */
        0x8D, 0x14,       /* 启用内部电荷泵。 */
        0x20, 0x02,       /* 页寻址：每次明确选择页和起始列。 */
        0xA1, 0xC8,       /* 常见模块的列映射和行扫描方向。 */
        0xDA, 0x12,       /* 128x64 面板的 COM 引脚配置。 */
        0x81, 0x7F,       /* 中等对比度，避免全亮高负载测试。 */
        0xD9, 0xF1,       /* 使用电荷泵时的预充电周期。 */
        0xDB, 0x40,       /* VCOMH 电平为 0.77 倍 VCC。 */
        0xA4, 0xA6,       /* 按显存显示，正常黑白极性。 */
        0x2E              /* 关闭可能残留的硬件滚动。 */
    };
    return oled_write(display, commands, sizeof(commands));
}

/* 比较当前画面和已发送画面，每页只传首个到最后一个变化字节。
 * 没变化的页完全跳过。先在 RAM 画完，再发送新旧像素的差异；不单独清屏，
 * 因此移动物体的旧位置也会在同一批更新中擦除。SSD1306 没有换帧信号，
 * 这种方式减少传输量和撕裂窗口，但无法保证面板扫描期间完全无撕裂。 */
static int refresh_oled(struct oled_display *display)
{
    rt_tick_t started = rt_tick_get();
    rt_uint8_t command[] = {0x00, 0xB0, 0x00, 0x10};
    rt_uint8_t data[OLED_WIDTH + 1u];
    data[0] = 0x40;
    for (unsigned page = 0; page < OLED_PAGE_COUNT; ++page)
    {
        unsigned first = 0;
        unsigned end = OLED_WIDTH;
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        if (display->shadow_valid)
        {
            while (first < end && display->pixels[page][first] == display->shadow[page][first])
            {
                ++first;
            }
            if (first == end)
            {
                continue;
            }
            while (end > first && display->pixels[page][end - 1u] == display->shadow[page][end - 1u])
            {
                --end;
            }
        }
        unsigned count = end - first;
        command[1] = (rt_uint8_t)(0xB0u + page);
        command[2] = (rt_uint8_t)(first & 0x0Fu);
        command[3] = (rt_uint8_t)(0x10u | (first >> 4));
        if (oled_write(display, command, sizeof(command)) != TEST_PASS)
        {
            display->shadow_valid = 0;
            return -RT_EIO;
        }
        memcpy(&data[1], &display->pixels[page][first], count);
        if (oled_write(display, data, count + 1u) != TEST_PASS)
        {
            display->shadow_valid = 0;
            return -RT_EIO;
        }
        /* 只在整包写入成功后更新副本，失败包绝不能被当作已显示。 */
        memcpy(&display->shadow[page][first], &display->pixels[page][first], count);
        display->data_bytes += count;
    }
    display->shadow_valid = 1;
    ++display->frames;
    rt_tick_t elapsed = rt_tick_get() - started;
    display->transfer_ticks += elapsed;
    if (elapsed > display->max_transfer_ticks)
    {
        display->max_transfer_ticks = elapsed;
    }
    return TEST_PASS;
}

/* (x,y) 转换为页号和字节内位号；先检查边界再访问缓冲区。 */
static void set_pixel(struct oled_display *display, int x, int y)
{
    if (x >= 0 && x < (int)OLED_WIDTH && y >= 0 && y < (int)OLED_HEIGHT)
    {
        display->pixels[y / 8u][x] |= (rt_uint8_t)(1u << (y % 8u));
    }
}

/* 四条边均落在面板最外侧，用于检查是否缺列、缺行或整幅偏移。 */
static void draw_border(struct oled_display *display)
{
    memset(display->pixels, 0, sizeof(display->pixels));
    for (unsigned x = 0; x < OLED_WIDTH; ++x)
    {
        set_pixel(display, x, 0);
        set_pixel(display, x, OLED_HEIGHT - 1u);
    }
    for (unsigned y = 0; y < OLED_HEIGHT; ++y)
    {
        set_pixel(display, 0, y);
        set_pixel(display, OLED_WIDTH - 1u, y);
    }
}

/* 8x8 棋盘格覆盖整屏，用于观察页边界和数据排列是否正确。 */
static void draw_checkerboard(struct oled_display *display)
{
    for (unsigned page = 0; page < OLED_PAGE_COUNT; ++page)
    {
        for (unsigned x = 0; x < OLED_WIDTH; ++x)
        {
            if (((x / 8u) + page) % 2u == 0)
            {
                display->pixels[page][x] = 0xFF;
            }
            else
            {
                display->pixels[page][x] = 0x00;
            }
        }
    }
}

/* 等待拆成至多 10 ms 小段；传输中则在页边界检查停止事件。 */
static int wait_for_observation(unsigned milliseconds)
{
    unsigned remaining = milliseconds;
    while (remaining > 0)
    {
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        unsigned step = remaining;
        if (step > 10u)
        {
            step = 10u;
        }
        rt_thread_mdelay(step);
        remaining -= step;
    }
    return TEST_PASS;
}

/* 5x7 点阵字库，每个字节是一列，最低位是上方像素。
 * 只保留广告文字使用的基础大写字母和数字，便于学习且不引入字体库。 */
static const rt_uint8_t font_letters[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}  /* Z */
};
static const rt_uint8_t font_digits[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}
};

static rt_uint8_t glyph_column(char character, unsigned column)
{
    if (column >= 5u)
    {
        return 0; /* 第六列留空，作为字间距。 */
    }
    if (character >= 'A' && character <= 'Z')
    {
        return font_letters[character - 'A'][column];
    }
    if (character >= '0' && character <= '9')
    {
        return font_digits[character - '0'][column];
    }
    if (character == '-')
    {
        return 0x08;
    }
    if (character == '*')
    {
        const rt_uint8_t star[] = {0x14, 0x08, 0x3E, 0x08, 0x14};
        return star[column];
    }
    return 0; /* 空格或未收录字符显示为空白。 */
}

/* 小字标题按原始 5x7 尺寸绘制；文字内容只需修改字符串。 */
static void draw_text(struct oled_display *display, int left, int top, const char *text)
{
    for (unsigned index = 0; text[index] != '\0'; ++index)
    {
        for (unsigned column = 0; column < 5u; ++column)
        {
            rt_uint8_t bits = glyph_column(text[index], column);
            for (unsigned row = 0; row < 7u; ++row)
            {
                if ((bits & (1u << row)) != 0)
                {
                    set_pixel(display, left + (int)(index * 6u + column), top + (int)row);
                }
            }
        }
    }
}

/* Bresenham 整数画线：累计误差决定何时沿另一坐标轴迈一步。
 * 远侧边每两点留一个空隙，配合透视帮助辨别立方体的前后关系。 */
static void draw_line(struct oled_display *display, int x0, int y0, int x1, int y1, int dotted)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int step_x = 1;
    int step_y = 1;
    if (dx < 0)
    {
        dx = -dx;
        step_x = -1;
    }
    if (dy < 0)
    {
        dy = -dy;
        step_y = -1;
    }
    int error = dx - dy;
    unsigned index = 0;
    while (1)
    {
        if (!dotted || index % 3u != 2u)
        {
            set_pixel(display, x0, y0);
        }
        if (x0 == x1 && y0 == y1)
        {
            break;
        }
        int twice_error = error * 2;
        if (twice_error > -dy)
        {
            error -= dy;
            x0 += step_x;
        }
        if (twice_error < dx)
        {
            error += dx;
            y0 += step_y;
        }
        ++index;
    }
}

/* 用真实经过时间计算位置；即使某帧传输慢，物体速度也不会随帧率改变。 */
static void draw_moving_block(struct oled_display *display, unsigned milliseconds)
{
    unsigned phase = milliseconds % 4000u;
    if (phase > 2000u)
    {
        phase = 4000u - phase;
    }
    unsigned left = 1u + phase * (OLED_WIDTH - OLED_BLOCK_SIZE - 2u) / 2000u;
    unsigned top = (OLED_HEIGHT - OLED_BLOCK_SIZE) / 2u;
    draw_border(display);
    for (unsigned y = top; y < top + OLED_BLOCK_SIZE; ++y)
    {
        for (unsigned x = left; x < left + OLED_BLOCK_SIZE; ++x)
        {
            set_pixel(display, (int)x, (int)y);
        }
    }
}

/* 8 个三维顶点先绕 Y 轴、再绕 X 轴旋转，最后投影到 128x64 屏幕。
 * 屏幕是平面单色 OLED；近大远小、旋转和虚实线产生视觉纵深，无双眼视差。
 * 摄像机距离 4，顶点到原点距离约 1.47，所以透视分母始终大于零。 */
static void draw_cube(struct oled_display *display, unsigned milliseconds)
{
    static const float vertices[8][3] = {
        {-0.85f,-0.85f,-0.85f}, { 0.85f,-0.85f,-0.85f},
        { 0.85f, 0.85f,-0.85f}, {-0.85f, 0.85f,-0.85f},
        {-0.85f,-0.85f, 0.85f}, { 0.85f,-0.85f, 0.85f},
        { 0.85f, 0.85f, 0.85f}, {-0.85f, 0.85f, 0.85f}
    };
    static const unsigned edges[12][2] = {
        {0,1}, {1,2}, {2,3}, {3,0}, {4,5}, {5,6},
        {6,7}, {7,4}, {0,4}, {1,5}, {2,6}, {3,7}
    };
    float seconds = (float)milliseconds / 1000.0f;
    float cy = cosf(seconds * 0.9f);
    float sy = sinf(seconds * 0.9f);
    float cx = cosf(seconds * 0.6f);
    float sx = sinf(seconds * 0.6f);
    int screen_x[8];
    int screen_y[8];
    float depth[8];
    memset(display->pixels, 0, sizeof(display->pixels));
    draw_text(display, 31, 2, "PERSPECTIVE");
    for (unsigned index = 0; index < 8u; ++index)
    {
        float x = vertices[index][0] * cy + vertices[index][2] * sy;
        float z = -vertices[index][0] * sy + vertices[index][2] * cy;
        float y = vertices[index][1] * cx - z * sx;
        depth[index] = vertices[index][1] * sx + z * cx;
        float scale = 54.0f / (4.0f + depth[index]);
        screen_x[index] = (int)(64.0f + x * scale + 0.5f);
        screen_y[index] = (int)(36.0f + y * scale + 0.5f);
    }
    for (unsigned edge = 0; edge < 12u; ++edge)
    {
        unsigned first = edges[edge][0];
        unsigned second = edges[edge][1];
        int far_edge = depth[first] + depth[second] > 0.0f;
        draw_line(display, screen_x[first], screen_y[first],
                  screen_x[second], screen_y[second], far_edge);
    }
}

/* 字幕是一条首尾相连的虚拟灯带。对每个屏幕列取模定位字形，
 * 因此跨过消息尾部时直接显示开头，不会清屏或突然跳回。
 * 可改此字符串，当前字库支持大写英文、数字、空格、减号和星号。 */
static const char marquee_text[] = "HELLO SSD1306  *  RT-THREAD  *  HMI DEMO  *  ";

static void draw_marquee(struct oled_display *display, unsigned milliseconds)
{
    unsigned strip_width = (sizeof(marquee_text) - 1u) * OLED_GLYPH_ADVANCE;
    unsigned offset = (milliseconds * 36u / 1000u) % strip_width;
    memset(display->pixels, 0, sizeof(display->pixels));
    draw_text(display, 31, 2, "NEON SCROLL");
    draw_text(display, 40, 55, "I2C OLED");
    for (unsigned x = 0; x < OLED_WIDTH; ++x)
    {
        unsigned position = (offset + x) % strip_width;
        unsigned letter = position / OLED_GLYPH_ADVANCE;
        unsigned column = (position % OLED_GLYPH_ADVANCE) / OLED_TEXT_SCALE;
        rt_uint8_t bits = glyph_column(marquee_text[letter], column);
        for (unsigned row = 0; row < 7u; ++row)
        {
            if ((bits & (1u << row)) != 0)
            {
                for (unsigned repeat = 0; repeat < OLED_TEXT_SCALE; ++repeat)
                {
                    set_pixel(display, (int)x, (int)(24u + row * OLED_TEXT_SCALE + repeat));
                }
            }
        }
        /* 上下跑马灯反向追逐；不通过整屏开关或反色制造闪烁。 */
        unsigned chase = milliseconds / 120u;
        if ((x + chase) % 8u < 3u)
        {
            set_pixel(display, (int)x, 18);
        }
        if ((x + 8u - chase % 8u) % 8u < 3u)
        {
            set_pixel(display, (int)x, 46);
        }
    }
}

enum oled_scene
{
    OLED_SCENE_BLOCK,
    OLED_SCENE_CUBE,
    OLED_SCENE_TEXT
};

static unsigned ticks_to_ms(rt_tick_t ticks)
{
    return (unsigned)((rt_uint64_t)ticks * 1000u / RT_TICK_PER_SECOND);
}

/* 目标帧间隔 33 ms 包含绘图和传输时间；只等待剩余时间。
 * 若硬件传输已超时，不再额外等待 33 ms，只让出一个系统节拍。
 * 每段打印实际帧率与传输耗时，目标帧率不作为已达到的测量结果。 */
static int play_animation(struct oled_display *display, enum oled_scene scene, unsigned duration_ms)
{
    const char *name = "block";
    if (scene == OLED_SCENE_CUBE)
    {
        name = "cube";
    }
    else if (scene == OLED_SCENE_TEXT)
    {
        name = "marquee";
    }
    rt_kprintf("OLED scene=%s duration=%u ms target_period=33 ms\n", name, duration_ms);
    unsigned first_frame = display->frames;
    unsigned first_bytes = display->data_bytes;
    rt_tick_t first_transfer = display->transfer_ticks;
    display->max_transfer_ticks = 0;
    rt_tick_t started = rt_tick_get();
    while (ticks_to_ms(rt_tick_get() - started) < duration_ms)
    {
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        rt_tick_t frame_started = rt_tick_get();
        unsigned time_ms = ticks_to_ms(frame_started - started);
        if (scene == OLED_SCENE_BLOCK)
        {
            draw_moving_block(display, time_ms);
        }
        else if (scene == OLED_SCENE_CUBE)
        {
            draw_cube(display, time_ms);
        }
        else
        {
            draw_marquee(display, time_ms);
        }
        int result = refresh_oled(display);
        if (result != TEST_PASS)
        {
            return result;
        }
        unsigned used_ms = ticks_to_ms(rt_tick_get() - frame_started);
        if (used_ms < OLED_FRAME_PERIOD_MS)
        {
            result = wait_for_observation(OLED_FRAME_PERIOD_MS - used_ms);
            if (result != TEST_PASS)
            {
                return result;
            }
        }
        else
        {
            rt_thread_delay(1);
        }
    }
    unsigned elapsed_ms = ticks_to_ms(rt_tick_get() - started);
    unsigned frames = display->frames - first_frame;
    if (frames > 0 && elapsed_ms > 0)
    {
        unsigned fps10 = frames * 10000u / elapsed_ms;
        rt_kprintf("OLED scene=%s frames=%u fps=%u.%u data_bytes/frame=%u tx_ms_avg=%u tx_ms_max=%u\n",
                   name, frames, fps10 / 10u, fps10 % 10u,
                   (display->data_bytes - first_bytes) / frames,
                   ticks_to_ms(display->transfer_ticks - first_transfer) / frames,
                   ticks_to_ms(display->max_transfer_ticks));
    }
    return TEST_PASS;
}

/* 独立完成初始化、静态检查、三段动画和退出清理；无初始化先后依赖。 */
static int run_test(void)
{
    struct oled_display display = {0};
    const rt_uint8_t turn_on[] = {0x00, 0xAF};
    const rt_uint8_t turn_off[] = {0x00, 0xAE};
    int result;

    display.bus = (struct rt_i2c_bus_device *)rt_device_find("i2c1");
    if (display.bus == RT_NULL)
    {
        rt_kprintf("OLED i2c1 bus missing\n");
        return -RT_ENOSYS;
    }
    result = wait_for_observation(100);
    if (result != TEST_PASS)
    {
        return result;
    }
    result = find_oled(&display);
    if (result != TEST_PASS)
    {
        return result;
    }
    result = initialize_oled(&display);
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    draw_border(&display);
    result = refresh_oled(&display);
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    result = oled_write(&display, turn_on, sizeof(turn_on));
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    rt_kprintf("OLED border: all four edges should be visible (2 seconds)\n");
    result = wait_for_observation(2000);
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    draw_checkerboard(&display);
    result = refresh_oled(&display);
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    rt_kprintf("OLED checkerboard: 8x8 squares across whole screen (2 seconds)\n");
    result = wait_for_observation(2000);
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    result = play_animation(&display, OLED_SCENE_BLOCK, OLED_BLOCK_DURATION_MS);
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    result = play_animation(&display, OLED_SCENE_CUBE, OLED_CUBE_DURATION_MS);
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    result = play_animation(&display, OLED_SCENE_TEXT, OLED_TEXT_DURATION_MS);
    if (result != TEST_PASS)
    {
        goto turn_display_off;
    }
    rt_kprintf("OLED frames=%u data_bytes=%u; visual confirmation required\n",
               display.frames, display.data_bytes);
    result = TEST_WAIT;

turn_display_off:
    /* 成功、失败和取消均尝试关闭显示；即使已收到停止请求也执行清理写入。 */
    if (oled_write(&display, turn_off, sizeof(turn_off)) != TEST_PASS)
    {
        result = -RT_EIO;
    }
    return result;
}

/* 本文件唯一线程入口：测试结束自然返回，error 保存结果供调度器读取。 */
void test_pmod_i2c_thread(void *argument)
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
