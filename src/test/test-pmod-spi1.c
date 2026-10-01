/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-pmod-spi1.c
 * @brief 验证 Pmod1 SCI7 的 SPI 收发回环。
 *
 * J2 pin2/P613 接 pin3/P614；1 MHz，比较八组 64 字节变化载荷。
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

#define PMOD_SPI_BITRATE 1000000u
#define PMOD_SPI_TIMEOUT_MS 500u
#define PMOD_SPI_PATTERN_COUNT 8u

static volatile unsigned spi_transfer_complete;
static volatile unsigned spi_transfer_error;

/* 记录 SPI 传输完成或错误事件；线程负责比较接收数据。 */
static void spi_callback(spi_callback_args_t *arguments)
{
    if (arguments->event == SPI_EVENT_TRANSFER_COMPLETE)
    {
        spi_transfer_complete = 1;
    }
    else
    {
        spi_transfer_error = 1;
    }
}

/* MOSI 与 MISO 短接后收发不同载荷；没有跳线应返回 FAIL。 */
static int run_test(void)
{
    const spi_instance_t *spi_instance;
    bsp_io_port_pin_t chip_select;

    spi_instance = &g_sci_spi7;
    chip_select = BSP_IO_PORT_06_PIN_11;

    spi_cfg_t config = *spi_instance->p_cfg;
    sci_spi_extended_cfg_t spi_extension = *(const sci_spi_extended_cfg_t *)config.p_extend;
    uint8_t transmit_buffer[64] BSP_ALIGN_VARIABLE(4);
    uint8_t receive_buffer[64] BSP_ALIGN_VARIABLE(4);
    int result = -RT_ERROR;
    config.p_callback = spi_callback;
    config.p_extend = &spi_extension;
    if (R_SCI_SPI_CalculateBitrate(PMOD_SPI_BITRATE, &spi_extension.clk_div, false) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rt_pin_mode(chip_select, PIN_MODE_OUTPUT);
    rt_pin_write(chip_select, 1);
    if (R_SCI_SPI_Open(spi_instance->p_ctrl, &config) != FSP_SUCCESS)
    {
        goto pins;
    }
    for (unsigned pattern_index = 0; pattern_index < PMOD_SPI_PATTERN_COUNT; ++pattern_index)
    {
        for (unsigned index = 0; index < sizeof(transmit_buffer); ++index)
        {
            transmit_buffer[index] = (uint8_t)(index * 17 + pattern_index * 29);
        }
        memset(receive_buffer, 0, sizeof(receive_buffer));
        spi_transfer_complete = 0;
        spi_transfer_error = 0;
        rt_pin_write(chip_select, 0);
        if (R_SCI_SPI_WriteRead(spi_instance->p_ctrl,
                                transmit_buffer,
                                receive_buffer,
                                sizeof(transmit_buffer),
                                SPI_BIT_WIDTH_8_BITS) != FSP_SUCCESS)
        {
            goto done;
        }
        rt_tick_t start = rt_tick_get();
        while (!spi_transfer_complete && !spi_transfer_error &&
               !test_elapsed(start, PMOD_SPI_TIMEOUT_MS) && !test_cancelled())
        {
            rt_thread_mdelay(1);
        }
        rt_pin_write(chip_select, 1);
        if (!spi_transfer_complete || spi_transfer_error ||
            memcmp(transmit_buffer, receive_buffer, sizeof(transmit_buffer)) != 0)
        {
            goto done;
        }
    }
    rt_kprintf("PMOD %s MOSI/MISO loop 8x64 bytes MATCH\n", "spi1");
    result = 0;
done:
    R_SCI_SPI_Close(spi_instance->p_ctrl);
pins:
    rt_pin_write(chip_select, 1);
    test_restore_pin(chip_select);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_pmod_spi1_thread(void *argument)
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
