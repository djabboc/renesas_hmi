/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-pmod-spi0.c
 * @brief 验证 Pmod0 SCI6 的 SPI 收发回环。
 *
 * J1-3/P305（MOSI）接 J1-5/P304（MISO）；先查接线，再以 1 MHz 比较八组 64 字节。
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
#define PMOD_MOSI BSP_IO_PORT_03_PIN_05
#define PMOD_MISO BSP_IO_PORT_03_PIN_04
#define PMOD_CLOCK BSP_IO_PORT_03_PIN_06
#define PMOD_SELECT BSP_IO_PORT_03_PIN_07

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
        spi_transfer_error = (unsigned)arguments->event;
    }
}

/* 引脚配置失败时报告 FSP 原始错误码，避免所有失败只显示为 -1。 */
static int configure_pin(bsp_io_port_pin_t pin, uint32_t configuration)
{
    fsp_err_t error = R_IOPORT_PinCfg(&g_ioport_ctrl, pin, configuration);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD spi0 pin=%04X config failed fsp=%d\n", (unsigned)pin, (int)error);
        return -RT_ERROR;
    }
    return TEST_PASS;
}

/* 先用慢速 GPIO 检查短接线，再切换到 SCI6。
 * MISO 使用上拉：未接线时应读到高电平，输出低电平的步骤即可发现断路。
 * 此检查只驱动 MOSI；不向其他排的 IRQ/GPIO 脚输出信号。 */
static int check_jumper(void)
{
    const unsigned levels[] = {0, 1, 0, 1};
    int result;

    result = configure_pin(PMOD_MISO, IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_PULLUP_ENABLE);
    if (result != TEST_PASS)
    {
        return result;
    }
    result = configure_pin(PMOD_MOSI, IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    if (result != TEST_PASS)
    {
        return result;
    }
    for (unsigned index = 0; index < sizeof(levels) / sizeof(levels[0]); ++index)
    {
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        rt_pin_write(PMOD_MOSI, levels[index]);
        rt_thread_mdelay(2);
        int received = rt_pin_read(PMOD_MISO);
        rt_kprintf("PMOD spi0 wire P305=%u P304=%d\n", levels[index], received);
        if (received != (int)levels[index])
        {
            rt_kprintf("PMOD spi0 wire FAIL: check MOSI/P305 to MISO/P304 (J1-3 to J1-5)\n");
            return -RT_EIO;
        }
    }
    rt_kprintf("PMOD spi0 wire PASS; switching to SCI6\n");
    return TEST_PASS;
}

/* 每次独立配置引脚、打开 SCI6 并核对数据；结束时关闭 SCI6、恢复四个引脚。
 * 保留工程生成的 DTC 配置，以便区分接线、驱动启动、回调和字节校验失败。 */
static int run_test(void)
{
    const spi_instance_t *spi_instance = &g_sci_spi6;
    spi_cfg_t config = *spi_instance->p_cfg;
    sci_spi_extended_cfg_t spi_extension = *(const sci_spi_extended_cfg_t *)config.p_extend;
    const uint32_t peripheral = IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_SCI0_2_4_6_8;
    uint8_t transmit_buffer[64] BSP_ALIGN_VARIABLE(4);
    uint8_t receive_buffer[64] BSP_ALIGN_VARIABLE(4);
    fsp_err_t error;
    int result = -RT_ERROR;

    config.p_callback = spi_callback;
    config.p_extend = &spi_extension;
    error = R_SCI_SPI_CalculateBitrate(PMOD_SPI_BITRATE, &spi_extension.clk_div, false);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD spi0 bitrate failed fsp=%d\n", (int)error);
        return -RT_ERROR;
    }
    result = configure_pin(PMOD_SELECT, IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH);
    if (result != TEST_PASS)
    {
        goto restore_pins;
    }
    result = check_jumper();
    if (result != TEST_PASS)
    {
        goto restore_pins;
    }
    result = -RT_ERROR;
    if (configure_pin(PMOD_MISO, peripheral) != TEST_PASS ||
        configure_pin(PMOD_MOSI, peripheral) != TEST_PASS ||
        configure_pin(PMOD_CLOCK, peripheral) != TEST_PASS)
    {
        goto restore_pins;
    }
    rt_kprintf("PMOD spi0 SCI6 1MHz mode0 DTC tx=%u rx=%u\n",
               (unsigned)(config.p_transfer_tx != NULL),
               (unsigned)(config.p_transfer_rx != NULL));
    error = R_SCI_SPI_Open(spi_instance->p_ctrl, &config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD spi0 open failed fsp=%d\n", (int)error);
        goto restore_pins;
    }
    for (unsigned pattern_index = 0; pattern_index < PMOD_SPI_PATTERN_COUNT; ++pattern_index)
    {
        if (test_cancelled())
        {
            result = -RT_EINTR;
            goto close_spi;
        }
        for (unsigned index = 0; index < sizeof(transmit_buffer); ++index)
        {
            transmit_buffer[index] = (uint8_t)(index * 17 + pattern_index * 29);
        }
        memset(receive_buffer, 0, sizeof(receive_buffer));
        spi_transfer_complete = 0;
        spi_transfer_error = 0;
        rt_pin_write(PMOD_SELECT, 0);
        error = R_SCI_SPI_WriteRead(spi_instance->p_ctrl,
                                   transmit_buffer,
                                   receive_buffer,
                                   sizeof(transmit_buffer),
                                   SPI_BIT_WIDTH_8_BITS);
        if (error != FSP_SUCCESS)
        {
            rt_kprintf("PMOD spi0 start failed pattern=%u fsp=%d\n", pattern_index, (int)error);
            goto close_spi;
        }
        rt_tick_t start = rt_tick_get();
        while (spi_transfer_complete == 0 && spi_transfer_error == 0 &&
               !test_elapsed(start, PMOD_SPI_TIMEOUT_MS) && !test_cancelled())
        {
            rt_thread_mdelay(1);
        }
        rt_pin_write(PMOD_SELECT, 1);
        if (test_cancelled())
        {
            result = -RT_EINTR;
            goto close_spi;
        }
        if (spi_transfer_error != 0)
        {
            rt_kprintf("PMOD spi0 transfer error pattern=%u event=%u\n", pattern_index, spi_transfer_error);
            result = -RT_EIO;
            goto close_spi;
        }
        if (spi_transfer_complete == 0)
        {
            rt_kprintf("PMOD spi0 timeout pattern=%u after %u ms\n", pattern_index, PMOD_SPI_TIMEOUT_MS);
            result = -RT_ETIMEOUT;
            goto close_spi;
        }
        /* 报告首个不一致字节；传输完成回调并不代表数据正确。 */
        for (unsigned index = 0; index < sizeof(transmit_buffer); ++index)
        {
            if (receive_buffer[index] != transmit_buffer[index])
            {
                rt_kprintf("PMOD spi0 mismatch pattern=%u byte=%u TX=%02X RX=%02X\n",
                           pattern_index, index,
                           (unsigned)transmit_buffer[index], (unsigned)receive_buffer[index]);
                result = -RT_EIO;
                goto close_spi;
            }
        }
    }
    rt_kprintf("PMOD spi0 MOSI/MISO loop 8x64 bytes MATCH\n");
    result = TEST_PASS;
close_spi:
    /* 超时或取消时也先停传输，再恢复引脚，避免外设继续驱动引脚。 */
    rt_pin_write(PMOD_SELECT, 1);
    error = R_SCI_SPI_Close(spi_instance->p_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("PMOD spi0 close failed fsp=%d\n", (int)error);
        result = -RT_ERROR;
    }
restore_pins:
    test_restore_pin(PMOD_SELECT);
    test_restore_pin(PMOD_CLOCK);
    test_restore_pin(PMOD_MOSI);
    test_restore_pin(PMOD_MISO);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_pmod_spi0_thread(void *argument)
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
