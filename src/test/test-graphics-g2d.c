/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-graphics-g2d.c
 * @brief 验证 D/AVE 2D 硬件填色。
 *
 * 在 16×16 缓冲画红底绿框并逐像素比对；DMA 超时保留内存并要求复位。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>
#include "dave_driver.h"
#include "dave_base.h"

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

#define GRAPHICS_TIMEOUT_MS 1000u
/* D/AVE 状态 bit0/1 为渲染/写回忙，bit3 为显示列表活动。 */
#define DAVE_BUSY_MASK 0x0bu

/* D/AVE 使用 RT-Thread 堆，便于统一查看分配与释放统计。 */
void *d1_malloc(size_t size)
{
    return rt_malloc(size);
}

/* D/AVE 释放回调，与本文件的 rt_malloc 分配配对。 */
void d1_free(void *ptr)
{
    rt_free(ptr);
}

static uint16_t output_pixels[16 * 16] BSP_ALIGN_VARIABLE(8);

static int g2d_requires_reset;

/* 硬件完成后逐像素比对红底绿框；DMA 超时不得释放硬件仍可能使用的缓冲。 */
static int run_test(void)
{
    int result = -RT_ERROR;

    if (g2d_requires_reset)
    {
        rt_kprintf("G2D previous hardware timeout: reset required\n");
        return -RT_ERROR;
    }
    d2_device *device = d2_opendevice(0);
    d2_renderbuffer *render_buffer = NULL;
    if (!device)
    {
        return -RT_ENOMEM;
    }
    if (d2_inithw(device, 0) != D2_OK)
    {
        d2_closedevice(device);
        return -RT_ERROR;
    }
    render_buffer = d2_newrenderbuffer(device, 20, 20);
    if (!render_buffer)
    {
        goto g2d_done;
    }
    memset(output_pixels, 0, sizeof(output_pixels));
    if (d2_selectrenderbuffer(device, render_buffer) != D2_OK ||
        d2_framebuffer(device, output_pixels, 16, 16, 16, d2_mode_rgb565) != D2_OK ||
        d2_cliprect(device, 0, 0, 15, 15) != D2_OK || d2_clear(device, 0xff0000) != D2_OK ||
        d2_setcolor(device, 0, 0x00ff00) != D2_OK || d2_setalpha(device, 255) != D2_OK ||
        d2_renderbox(device, 4 * 16, 4 * 16, 8 * 16, 8 * 16) != D2_OK ||
        d2_executerenderbuffer(device, render_buffer, 0) != D2_OK)
    {
        goto g2d_done;
    }
    /* 供应商 flush 内部无限等待；先有限轮询忙位，再调用 flush。 */
    rt_tick_t start = rt_tick_get();
    while ((uint32_t)d1_getregister(d2_level1interface(device), D1_DAVE2D, 0) & DAVE_BUSY_MASK)
    {
        if (test_elapsed(start, GRAPHICS_TIMEOUT_MS) || test_cancelled())
        {
            /* 硬件可能仍引用缓冲，超时路径不能释放，必须复位回收。 */
            g2d_requires_reset = 1;
            rt_kprintf("G2D timeout: allocations retained, reset required\n");
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(1);
    }
    d2_flushframe(device);
    result = 0;
    for (unsigned y = 0; y < 16; ++y)
    {
        for (unsigned x = 0; x < 16; ++x)
        {
            uint16_t expected;
            if ((x >= 4 && x < 12 && y >= 4 && y < 12))
            {
                expected = 0x07e0;
            }
            else
            {
                expected = 0xf800;
            }
            if (output_pixels[y * 16 + x] != expected)
            {
                result = -RT_ERROR;
            }
        }
    }
    rt_kprintf("G2D hardware red clear + green rectangle: corners=%04X center=%04X\n",
               output_pixels[0],
               output_pixels[8 * 16 + 8]);
g2d_done:
    if (render_buffer)
    {
        d2_freerenderbuffer(device, render_buffer);
    }
    d2_deinithw(device);
    d2_closedevice(device);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_graphics_g2d_thread(void *argument)
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
