/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-pmod-i2c.c
 * @brief 用外接 SSD1306 128x64 OLED 验证 I2C 写入和持续刷新。
 *
 * 接线：3.3V、GND、P202/SCL、P203/SDA；仅适用四针 I2C 模块。
 * 独立寻找 0x3C/0x3D，初始化、显示边框/棋盘格/移动方块，结束关闭显示。
 * I2C 应答不代表图像正确，因此完整执行返回 WAIT，由用户观察屏幕验收。
 * 阅读顺序：线程入口 → run_test → 本文件的通信与绘图函数。
 */
#include <rtthread.h>
#include <rtdevice.h>
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

#define OLED_WIDTH 128u
#define OLED_HEIGHT 64u
#define OLED_PAGE_COUNT (OLED_HEIGHT / 8u)
#define OLED_BLOCK_SIZE 8u
#define OLED_ANIMATION_FRAMES 50u

/* SSD1306 每字节控制同一列上的 8 个纵向像素，整屏共 8 页、1024 字节。
 * 缓冲区与总线信息属于本次调用；不引用其他测试，也不创建后台线程。 */
struct oled_display
{
    struct rt_i2c_bus_device *bus;
    rt_uint16_t address;
    rt_uint8_t pixels[OLED_PAGE_COUNT][OLED_WIDTH];
    unsigned frames;
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

/* 每页先设置 B0~B7 页号以及列地址 0，再发送 128 字节。
 * 页间检查停止请求；总线由驱动互斥保护，本例程不关闭共享 i2c1。 */
static int refresh_oled(struct oled_display *display)
{
    rt_uint8_t command[] = {0x00, 0xB0, 0x00, 0x10};
    rt_uint8_t data[OLED_WIDTH + 1u];
    data[0] = 0x40;
    for (unsigned page = 0; page < OLED_PAGE_COUNT; ++page)
    {
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        command[1] = (rt_uint8_t)(0xB0u + page);
        if (oled_write(display, command, sizeof(command)) != TEST_PASS)
        {
            return -RT_EIO;
        }
        memcpy(&data[1], display->pixels[page], OLED_WIDTH);
        if (oled_write(display, data, sizeof(data)) != TEST_PASS)
        {
            return -RT_EIO;
        }
    }
    ++display->frames;
    return TEST_PASS;
}

/* (x,y) 转换为页号和字节内位号；先检查边界再访问缓冲区。 */
static void set_pixel(struct oled_display *display, unsigned x, unsigned y)
{
    if (x < OLED_WIDTH && y < OLED_HEIGHT)
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

/* 将观察等待拆成小段，让停止请求最多在一次 I2C 操作或 20 ms 后被检查。 */
static int wait_for_observation(unsigned milliseconds)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 20u)
    {
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        rt_thread_mdelay(20);
    }
    return TEST_PASS;
}

/* 独立完成地址选择、初始化、三种图案和退出清理；无初始化先后依赖。 */
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
    rt_kprintf("OLED animation: 8x8 block moves right then left, 50 frames\n");
    for (unsigned frame = 0; frame < OLED_ANIMATION_FRAMES; ++frame)
    {
        /* 0~24 向右，25~49 向左；保留外框，方块始终在框内。 */
        unsigned step = frame;
        if (frame >= OLED_ANIMATION_FRAMES / 2u)
        {
            step = OLED_ANIMATION_FRAMES - 1u - frame;
        }
        unsigned left = 1u + step * (OLED_WIDTH - OLED_BLOCK_SIZE - 2u) / 24u;
        unsigned top = (OLED_HEIGHT - OLED_BLOCK_SIZE) / 2u;
        draw_border(&display);
        for (unsigned y = top; y < top + OLED_BLOCK_SIZE; ++y)
        {
            for (unsigned x = left; x < left + OLED_BLOCK_SIZE; ++x)
            {
                set_pixel(&display, x, y);
            }
        }
        result = refresh_oled(&display);
        if (result != TEST_PASS)
        {
            goto turn_display_off;
        }
        result = wait_for_observation(80);
        if (result != TEST_PASS)
        {
            goto turn_display_off;
        }
    }
    rt_kprintf("OLED frames=%u; writes completed, visual confirmation required\n", display.frames);
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
