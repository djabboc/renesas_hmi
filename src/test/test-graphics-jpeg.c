/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-graphics-jpeg.c
 * @brief 验证片上 JPEG 硬件解码，并在 LCD 显示解码结果。
 *
 * 内嵌 16×16 灰色 JPEG；先检查全部像素，再显示原尺寸与 10 倍放大图。
 * 不依赖 SD；显示 15 秒后熄屏，视觉效果由用户确认。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
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

/* 无符号 tick 差值允许系统计时回绕；等待始终有上限。 */
static int test_elapsed(rt_tick_t start, unsigned milliseconds)
{
    return (rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(milliseconds);
}

/* 内嵌测试图：16×16、RGB(128,128,128) 的灰色 baseline JPEG。 */
static const unsigned char test_jpeg[] BSP_ALIGN_VARIABLE(8) = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
    0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
    0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
    0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0b, 0x08, 0x09, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x08,
    0x0b, 0x0c, 0x0b, 0x0a, 0x0c, 0x09, 0x0a, 0x0a, 0x0a, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0a, 0x07, 0x06, 0x07, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0xff, 0xc0,
    0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
    0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23,
    0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00, 0x1f, 0x01, 0x00, 0x03,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
    0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
    0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0xfa, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x28,
    0x00, 0xa0, 0x02, 0x80, 0x0a, 0x00, 0xff, 0xd9};

#define GRAPHICS_TIMEOUT_MS 1000u
#define JPEG_SIDE_PIXELS 16u
#define LCD_HEIGHT_PIXELS 272u
#define LCD_OBSERVE_MS 15000u
#define LCD_ENABLE_PIN BSP_IO_PORT_01_PIN_05
#define LCD_BACKLIGHT_PIN BSP_IO_PORT_01_PIN_00
/* 输入与输出按八字节对齐，供 JPEG 硬件直接访问。 */

static uint16_t output_pixels[16 * 16] BSP_ALIGN_VARIABLE(8);
static uint8_t jpeg_input_buffer[4096] BSP_ALIGN_VARIABLE(8);
static volatile jpeg_status_t jpeg_events;

/* GLCDC 会保留配置指针；用静态对象保证关闭超时后指针仍然有效。 */
static display_cfg_t display_config;
static volatile unsigned lcd_frames;

/* 中断只记数，像素绘制与结果判断都由测试线程完成。 */
static void display_event(display_callback_args_t *arguments)
{
    if (arguments->event == DISPLAY_EVENT_LINE_DETECTION)
    {
        ++lcd_frames;
    }
}

/* 使用生成配置的行跨度定位像素；行跨度可能包含对齐填充。 */
static void fill_rectangle(unsigned left, unsigned top, unsigned width,
                           unsigned height, uint16_t color)
{
    uint16_t *pixels = (uint16_t *)fb_background[0];
    for (unsigned y = top; y < top + height; ++y)
    {
        for (unsigned x = left; x < left + width; ++x)
        {
            pixels[y * DISPLAY_BUFFER_STRIDE_PIXELS_INPUT0 + x] = color;
        }
    }
}

/* 最近邻放大：把一个解码像素复制为 scale×scale 的小方块。
 * 灰色来自硬件解码输出，白边仅用于在黑色背景上标出图像范围。 */
static void draw_decoded_image(unsigned left, unsigned top, unsigned scale)
{
    unsigned side = JPEG_SIDE_PIXELS * scale;
    fill_rectangle(left - 2, top - 2, side + 4, side + 4, 0xffff);
    for (unsigned y = 0; y < JPEG_SIDE_PIXELS; ++y)
    {
        for (unsigned x = 0; x < JPEG_SIDE_PIXELS; ++x)
        {
            uint16_t color = output_pixels[y * JPEG_SIDE_PIXELS + x];
            fill_rectangle(left + x * scale, top + y * scale, scale, scale, color);
        }
    }
}

/* Stop 提交停扫请求，Close 等待帧边界后释放外设；各等待最多 100ms。 */
static int close_display(void)
{
    fsp_err_t error;
    rt_tick_t start = rt_tick_get();
    do
    {
        error = R_GLCDC_Stop(&g_display0_ctrl);
        if (error == FSP_SUCCESS)
        {
            break;
        }
        rt_thread_mdelay(1);
    } while (!test_elapsed(start, 100));

    start = rt_tick_get();
    do
    {
        error = R_GLCDC_Close(&g_display0_ctrl);
        if (error == FSP_SUCCESS)
        {
            return TEST_PASS;
        }
        rt_thread_mdelay(1);
    } while (!test_elapsed(start, 100));

    rt_kprintf("JPEG LCD close=%d; reset before further display tests\n", error);
    return -RT_ERROR;
}

/* 解码校验成功后才打开屏幕；退出时关闭背光、电源使能及 GLCDC。 */
static int show_decoded_image(void)
{
    int result = -RT_ERROR;
    int display_open = 0;
    rt_tick_t start;

    /* 直接配置这两个引脚，避免重新打开整张 IOPORT 引脚配置表。
     * P105 是共用电源/背光使能；P100 高电平提供固定全亮背光。 */
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, LCD_ENABLE_PIN,
                       IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, LCD_BACKLIGHT_PIN,
                       IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }

    /* 先完成整幅画面，再启动扫描，避免观察到填充过程。 */
    memset(fb_background[0], 0, DISPLAY_BUFFER_STRIDE_BYTES_INPUT0 * LCD_HEIGHT_PIXELS);
    draw_decoded_image(112, 128, 1);
    draw_decoded_image(240, 56, 10);

    /* ST7282 模组已验收的时序；总周期包含有效像素和消隐区。 */
    display_config = g_display0_cfg;
    display_config.output.htiming.total_cyc = 531;
    display_config.output.htiming.back_porch = 43;
    display_config.output.htiming.sync_width = 2;
    display_config.output.vtiming.total_cyc = 292;
    display_config.output.vtiming.back_porch = 12;
    display_config.output.vtiming.sync_width = 2;
    display_config.p_callback = display_event;
    lcd_frames = 0;
    if (R_GLCDC_Open(&g_display0_ctrl, &display_config) != FSP_SUCCESS)
    {
        goto display_done;
    }
    display_open = 1;
    if (R_GLCDC_Start(&g_display0_ctrl) != FSP_SUCCESS)
    {
        goto display_done;
    }
    rt_thread_mdelay(150);
    if (R_IOPORT_PinWrite(&g_ioport_ctrl, LCD_ENABLE_PIN, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS)
    {
        goto display_done;
    }
    rt_thread_mdelay(10);
    if (R_IOPORT_PinWrite(&g_ioport_ctrl, LCD_BACKLIGHT_PIN, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS)
    {
        goto display_done;
    }
    rt_kprintf("JPEG LCD: black background, white borders; left=16x16 right=160x160 gray\n");
    rt_kprintf("JPEG LCD: decoded pixels verified; observe for 15 seconds\n");
    start = rt_tick_get();
    while (!test_elapsed(start, LCD_OBSERVE_MS) && !test_cancelled())
    {
        rt_thread_mdelay(20);
    }
    rt_kprintf("JPEG LCD: frames=%u; visual confirmation required\n", lcd_frames);
    if (lcd_frames > 0)
    {
        result = TEST_WAIT;
    }

display_done:
    /* 即使显示失败也保持屏幕关闭；下一例程自行初始化所需引脚。 */
    if (R_IOPORT_PinWrite(&g_ioport_ctrl, LCD_BACKLIGHT_PIN, BSP_IO_LEVEL_LOW) != FSP_SUCCESS)
    {
        result = -RT_ERROR;
    }
    if (R_IOPORT_PinWrite(&g_ioport_ctrl, LCD_ENABLE_PIN, BSP_IO_LEVEL_LOW) != FSP_SUCCESS)
    {
        result = -RT_ERROR;
    }
    if (display_open && close_display() != TEST_PASS)
    {
        result = -RT_ERROR;
    }
    return result;
}

/* JPEG 中断累积尺寸就绪、解码完成和错误事件，线程负责判定。 */
static void jpeg_event(jpeg_callback_args_t *arguments)
{
    jpeg_events |= arguments->status;
}

/* 以固定 baseline JPEG 验证尺寸、完成事件和 RGB565 解码结果。 */
static int run_test(void)
{
    int result = -RT_ERROR;

    jpeg_cfg_t jpeg_config = g_jpeg0_cfg;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t decoded_lines = 0;
    jpeg_config.p_decode_callback = jpeg_event;
    jpeg_events = 0;
    memset(output_pixels, 0, sizeof(output_pixels));
    if (R_JPEG_Open(&g_jpeg0_ctrl, &jpeg_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    /* 完整 JPEG 拷贝到对齐 SRAM。输入计数设 0，避免小图头部预取
     * 在输出缓冲配置前暂停；4096 字节缓冲提供预取余量。 */
    memset(jpeg_input_buffer, 0, sizeof(jpeg_input_buffer));
    memcpy(jpeg_input_buffer, test_jpeg, sizeof(test_jpeg));
    if (R_JPEG_InputBufferSet(&g_jpeg0_ctrl, jpeg_input_buffer, 0) != FSP_SUCCESS)
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
    if (R_JPEG_DecodeImageSizeGet(&g_jpeg0_ctrl, &width, &height) != FSP_SUCCESS || width != 16 ||
        height != 16)
    {
        goto jpeg_done;
    }
    if (R_JPEG_DecodeHorizontalStrideSet(&g_jpeg0_ctrl, 16) != FSP_SUCCESS ||
        R_JPEG_OutputBufferSet(&g_jpeg0_ctrl, output_pixels, sizeof(output_pixels)) != FSP_SUCCESS)
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
    if (R_JPEG_DecodeLinesDecodedGet(&g_jpeg0_ctrl, &decoded_lines) != FSP_SUCCESS ||
        decoded_lines != 16)
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
    if (R_JPEG_Close(&g_jpeg0_ctrl) != FSP_SUCCESS)
    {
        result = -RT_ERROR;
    }
    if (result == TEST_PASS && !test_cancelled())
    {
        result = show_decoded_image();
    }
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_graphics_jpeg_thread(void *argument)
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
