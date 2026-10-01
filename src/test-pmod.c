/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-pmod.c
 * @brief Pmod/Arduino SPI、IRQ 和 I2C 外接夹具测试。
 *
 * 先按 doc/peripheral-tests.md 接线；这些阶段不进入 all 批次。
 * 控制器配置取自 FSP，局部配置在关闭硬件后才离开作用域。
 */
#include "peripheral-test.h"

#define PMOD_SPI_BITRATE 1000000u
#define PMOD_SPI_TIMEOUT_MS 500u
#define PMOD_SPI_PATTERN_COUNT 8u
#define PMOD_IRQ_EDGE_COUNT 16u
#define PMOD_I2C_FIXTURE_ADDRESS 0x50u

static volatile unsigned spi_transfer_complete;
static volatile unsigned spi_transfer_error;
static volatile unsigned irq_edge_count;

static void pmod_irq_callback(external_irq_callback_args_t *arguments)
{
    RT_UNUSED(arguments);
    ++irq_edge_count;
}

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

/* J1/J2 的 GPIO0 经跳线进入 IRQ；双边沿计数验证引脚和中断链路。 */
static int verify_irq_loopback(const char *stage)
{
    int use_pmod0 = strcmp(stage, "irq0") == 0;
    int result = -RT_ERROR;
    const external_irq_instance_t *irq_instance = use_pmod0 ? &g_external_irq11 : &g_external_irq10;
    bsp_io_port_pin_t input_pin = use_pmod0 ? BSP_IO_PORT_07_PIN_08 : BSP_IO_PORT_07_PIN_09;
    bsp_io_port_pin_t output_pin = use_pmod0 ? BSP_IO_PORT_02_PIN_11 : BSP_IO_PORT_07_PIN_10;
    external_irq_cfg_t config = *irq_instance->p_cfg;
    config.trigger = EXTERNAL_IRQ_TRIG_BOTH_EDGE;
    config.p_callback = pmod_irq_callback;
    rt_pin_mode(output_pin, PIN_MODE_OUTPUT);
    rt_pin_write(output_pin, 0);
    R_IOPORT_PinCfg(&g_ioport_ctrl,
                    input_pin,
                    IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_IRQ_ENABLE |
                        IOPORT_CFG_PULLUP_ENABLE);
    if (R_ICU_ExternalIrqOpen(irq_instance->p_ctrl, &config))
    {
        goto irq_pins;
    }
    if (R_ICU_ExternalIrqEnable(irq_instance->p_ctrl))
    {
        goto irq_close;
    }
    /* 消化初始电平引起的中断，再开始统计人为翻转。 */
    rt_thread_mdelay(10);
    irq_edge_count = 0;
    for (unsigned index = 0; index < PMOD_IRQ_EDGE_COUNT && !test_cancelled(); ++index)
    {
        rt_pin_write(output_pin, (index + 1) & 1);
        rt_thread_mdelay(10);
    }
    result = irq_edge_count == PMOD_IRQ_EDGE_COUNT ? 0 : -RT_ERROR;
    rt_kprintf("PMOD %s GPIO->IRQ edges=%u expected=16\n", stage, irq_edge_count);
    R_ICU_ExternalIrqDisable(irq_instance->p_ctrl);
irq_close:
    R_ICU_ExternalIrqClose(irq_instance->p_ctrl);
irq_pins:
    test_restore_pin(output_pin);
    test_restore_pin(input_pin);
    return result;
}

/* 只读取已知夹具地址；ACK 通过不代表外接设备的内容正确。 */
static int verify_i2c_fixture(void)
{
    struct rt_i2c_bus_device *bus = (struct rt_i2c_bus_device *)rt_device_find("i2c1");
    uint8_t byte;
    if (!bus)
    {
        return -RT_ENOSYS;
    }
    /* 固定地址 0x50，只读当前字节；与 GT911 共用总线，不做写扫描。 */
    if (rt_i2c_master_recv(bus, PMOD_I2C_FIXTURE_ADDRESS, 0, &byte, 1) != 1)
    {
        rt_kprintf("I2C no response at external fixture 0x50\n");
        return TEST_SKIP;
    }
    rt_kprintf("I2C external 0x50 byte=%02X; device content not checked\n", byte);
    return 0;
}

/* MOSI 与 MISO 短接后收发不同载荷；没有跳线应返回 FAIL。 */
static int verify_spi_loopback(const char *stage)
{
    const spi_instance_t *spi_instance;
    bsp_io_port_pin_t chip_select;
    if (strcmp(stage, "spi0") == 0)
    {
        spi_instance = &g_sci_spi6;
        chip_select = BSP_IO_PORT_03_PIN_07;
    }
    else if (strcmp(stage, "spi1") == 0)
    {
        spi_instance = &g_sci_spi7;
        chip_select = BSP_IO_PORT_06_PIN_11;
    }
    else if (strcmp(stage, "arduino") == 0)
    {
        spi_instance = &g_sci_spi4;
        chip_select = BSP_IO_PORT_07_PIN_12;
    }
    else
    {
        return -RT_EINVAL;
    }
    spi_cfg_t config = *spi_instance->p_cfg;
    sci_spi_extended_cfg_t spi_extension = *(const sci_spi_extended_cfg_t *)config.p_extend;
    uint8_t transmit_buffer[64] BSP_ALIGN_VARIABLE(4);
    uint8_t receive_buffer[64] BSP_ALIGN_VARIABLE(4);
    int result = -RT_ERROR;
    config.p_callback = spi_callback;
    config.p_extend = &spi_extension;
    if (R_SCI_SPI_CalculateBitrate(PMOD_SPI_BITRATE, &spi_extension.clk_div, false))
    {
        return -RT_ERROR;
    }
    rt_pin_mode(chip_select, PIN_MODE_OUTPUT);
    rt_pin_write(chip_select, 1);
    if (R_SCI_SPI_Open(spi_instance->p_ctrl, &config))
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
                                SPI_BIT_WIDTH_8_BITS))
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
            memcmp(transmit_buffer, receive_buffer, sizeof(transmit_buffer)))
        {
            goto done;
        }
    }
    rt_kprintf("PMOD %s MOSI/MISO loop 8x64 bytes MATCH\n", stage);
    result = 0;
done:
    R_SCI_SPI_Close(spi_instance->p_ctrl);
pins:
    rt_pin_write(chip_select, 1);
    test_restore_pin(chip_select);
    return result;
}

int test_pmod(const char *stage)
{
    if (strcmp(stage, "irq0") == 0 || strcmp(stage, "irq1") == 0)
    {
        return verify_irq_loopback(stage);
    }
    if (strcmp(stage, "i2c") == 0)
    {
        return verify_i2c_fixture();
    }
    return verify_spi_loopback(stage);
}
