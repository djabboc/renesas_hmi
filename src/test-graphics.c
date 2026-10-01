/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-graphics.c
 * @brief JPEG 解码及 D/AVE 2D 硬件像素校验。
 *
 * 使用固定小图避免依赖屏幕观察；输出为 RGB565。
 * G2D 超时保留可能被 DMA 引用的内存，后续操作需复位。
 */
#include "peripheral-test.h"
#include "dave_driver.h"
#include "dave_base.h"
#include "test-jpeg-data.h"

#define GRAPHICS_TIMEOUT_MS 1000u
/* D/AVE 状态 bit0/1 为渲染/写回忙，bit3 为显示列表活动。 */
#define DAVE_BUSY_MASK 0x0bu

/* D/AVE 使用 RT-Thread 堆，便于统一查看分配与释放统计。 */
void *d1_malloc(size_t size)
{
    return rt_malloc(size);
}

void d1_free(void *ptr)
{
    rt_free(ptr);
}

static uint16_t output_pixels[16 * 16] BSP_ALIGN_VARIABLE(8);
static uint8_t jpeg_input_buffer[4096] BSP_ALIGN_VARIABLE(8);
static volatile jpeg_status_t jpeg_events;
static int g2d_requires_reset;

static void jpeg_event(jpeg_callback_args_t *arguments)
{
    jpeg_events |= arguments->status;
}

/* 硬件完成后逐像素比对红底绿框；DMA 超时不得释放硬件仍可能使用的缓冲。 */
static int verify_g2d_render(void)
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
            uint16_t expected = (x >= 4 && x < 12 && y >= 4 && y < 12) ? 0x07e0 : 0xf800;
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

/* 以固定 baseline JPEG 验证尺寸、完成事件和 RGB565 解码结果。 */
static int verify_jpeg_decode(void)
{
    int result = -RT_ERROR;

    jpeg_cfg_t jpeg_config = g_jpeg0_cfg;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t decoded_lines = 0;
    jpeg_config.p_decode_callback = jpeg_event;
    jpeg_events = 0;
    memset(output_pixels, 0, sizeof(output_pixels));
    if (R_JPEG_Open(&g_jpeg0_ctrl, &jpeg_config))
    {
        return -RT_ERROR;
    }
    /* 完整 JPEG 拷贝到对齐 SRAM。输入计数设 0，避免小图头部预取
     * 在输出缓冲配置前暂停；4096 字节缓冲提供预取余量。 */
    memset(jpeg_input_buffer, 0, sizeof(jpeg_input_buffer));
    memcpy(jpeg_input_buffer, test_jpeg, sizeof(test_jpeg));
    if (R_JPEG_InputBufferSet(&g_jpeg0_ctrl, jpeg_input_buffer, 0))
    {
        goto jpeg_done;
    }
    rt_tick_t start = rt_tick_get();
    while (!(jpeg_events & (JPEG_STATUS_IMAGE_SIZE_READY | JPEG_STATUS_ERROR)) &&
           !test_elapsed(start, GRAPHICS_TIMEOUT_MS) && !test_cancelled())
    {
        rt_thread_mdelay(1);
    }
    if (!(jpeg_events & JPEG_STATUS_IMAGE_SIZE_READY) || (jpeg_events & JPEG_STATUS_ERROR))
    {
        goto jpeg_done;
    }
    if (R_JPEG_DecodeImageSizeGet(&g_jpeg0_ctrl, &width, &height) || width != 16 || height != 16)
    {
        goto jpeg_done;
    }
    if (R_JPEG_DecodeHorizontalStrideSet(&g_jpeg0_ctrl, 16) ||
        R_JPEG_OutputBufferSet(&g_jpeg0_ctrl, output_pixels, sizeof(output_pixels)))
    {
        goto jpeg_done;
    }
    start = rt_tick_get();
    while (!(jpeg_events & (JPEG_STATUS_OPERATION_COMPLETE | JPEG_STATUS_ERROR)) &&
           !test_elapsed(start, GRAPHICS_TIMEOUT_MS) && !test_cancelled())
    {
        rt_thread_mdelay(1);
    }
    if (!(jpeg_events & JPEG_STATUS_OPERATION_COMPLETE) || (jpeg_events & JPEG_STATUS_ERROR))
    {
        goto jpeg_done;
    }
    if (R_JPEG_DecodeLinesDecodedGet(&g_jpeg0_ctrl, &decoded_lines) || decoded_lines != 16)
    {
        goto jpeg_done;
    }
    result = 0;
    /* 固定图为 RGB(128,128,128)，各 RGB565 通道允许一个量化步误差。 */
    for (unsigned pixel_index = 0; pixel_index < 256; ++pixel_index)
    {
        unsigned red = output_pixels[pixel_index] >> 11;
        unsigned green = (output_pixels[pixel_index] >> 5) & 63;
        unsigned blue = output_pixels[pixel_index] & 31;
        if (red < 15 || red > 17 || green < 31 || green > 33 || blue < 15 || blue > 17)
        {
            result = -RT_ERROR;
        }
    }
jpeg_done:
    rt_kprintf("JPEG %ux%u lines=%u status=%X pixel=%04X\n",
               width,
               height,
               decoded_lines,
               jpeg_events,
               output_pixels[0]);
    R_JPEG_Close(&g_jpeg0_ctrl);
    return result;
}

int test_graphics(const char *stage)
{
    if (strcmp(stage, "g2d") == 0)
    {
        return verify_g2d_render();
    }
    if (strcmp(stage, "jpeg") == 0)
    {
        return verify_jpeg_decode();
    }
    return -RT_EINVAL;
}
