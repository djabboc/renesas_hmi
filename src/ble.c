/*
 * SPDX-License-Identifier: Apache-2.0
 * Minimal RW007 discovery demo for HMI-Board V3.1.
 * Wire protocol reference: RT-Thread-packages/rw007, inc/spi_wifi_rw007.h,
 * inc/spi_ble_rw007.h and src/spi_wifi_rw007.c (Apache-2.0).
 * Uses the module's existing firmware; does not program/upgrade that firmware.
 */
#include <rtthread.h>
#include "hal_data.h"

#define BLE_CS          BSP_IO_PORT_03_PIN_08
#define BLE_MISO        BSP_IO_PORT_03_PIN_09
#define BLE_MOSI        BSP_IO_PORT_03_PIN_10
#define BLE_CLK         BSP_IO_PORT_03_PIN_11
#define BLE_RST         BSP_IO_PORT_03_PIN_12
#define BLE_INT         BSP_IO_PORT_00_PIN_15
#define BLE_PACKET_MAX  1528
#define BLE_ADV_MS      60000
#define BLE_CMD_INIT    0
#define BLE_CMD_MAC     2
#define BLE_CMD_VERSION 5
#define BLE_CMD_ADV     25

static spi_cfg_t ble_spi_config;
static sci_spi_extended_cfg_t ble_spi_extended;
static external_irq_cfg_t ble_irq_config;
static volatile rt_bool_t ble_spi_done;
static volatile spi_event_t ble_spi_event;
static volatile rt_uint32_t ble_irqs, ble_transfers, ble_errors;
static volatile rt_bool_t ble_ready, ble_advertising;
static rt_bool_t ble_spi_open, ble_irq_open;
static rt_uint16_t ble_sequence;
static rt_uint8_t ble_tx[BLE_PACKET_MAX] BSP_ALIGN_VARIABLE(4);
static rt_uint8_t ble_rx[BLE_PACKET_MAX] BSP_ALIGN_VARIABLE(4);
static rt_uint16_t ble_rx_length;
static rt_uint32_t ble_response_cmd;
static rt_int32_t ble_response_result;
static rt_bool_t ble_response_seen, ble_init_seen;
static rt_uint16_t ble_init_result;
static char ble_version[64];
static rt_uint8_t ble_mac[6];
static rt_bool_t ble_mac_valid;
/* Transport probe only: observing a GATT property does not establish a UART path. */
static volatile rt_uint32_t ble_rx_packets, ble_ble_packets, ble_other_packets;
static volatile rt_uint32_t ble_probe_packets;

static void ble_probe_packet(const rt_uint8_t *data, rt_uint32_t length, rt_uint32_t type)
{
    static const char marker[] = "HMI_PROBE1";
    rt_uint32_t i;
    ble_rx_packets++;
    if (type == 7) ble_ble_packets++;
    else if (type != 5) ble_other_packets++;
    for (i = 0; i + sizeof(marker) - 1 <= length; i++)
    {
        if (rt_memcmp(data + i, marker, sizeof(marker) - 1) == 0)
        {
            ble_probe_packets++;
            rt_kprintf("BLE: probe marker reached MCU, packet_type=%u bytes=%u\n", type, length);
            break;
        }
    }
    if (type != 5 && type != 7)
        rt_kprintf("BLE: other module packet type=%u bytes=%u\n", type, length);
}

static rt_uint16_t ble_get16(const rt_uint8_t *p)
{
    return p[0] | ((rt_uint16_t)p[1] << 8);
}

static rt_uint32_t ble_get32(const rt_uint8_t *p)
{
    return p[0] | ((rt_uint32_t)p[1] << 8) | ((rt_uint32_t)p[2] << 16) | ((rt_uint32_t)p[3] << 24);
}

static void ble_put16(rt_uint8_t *p, rt_uint16_t value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void ble_put32(rt_uint8_t *p, rt_uint32_t value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static void ble_spi_callback(spi_callback_args_t *args)
{
    ble_spi_event = args->event;
    ble_spi_done = RT_TRUE;
}

static void ble_irq_callback(external_irq_callback_args_t *args)
{
    RT_UNUSED(args);
    ble_irqs++;
}

static rt_err_t ble_exchange(const void *tx, void *rx, rt_uint32_t length)
{
    rt_tick_t start = rt_tick_get();
    fsp_err_t result;
    if (!length) return RT_EOK;
    ble_spi_done = RT_FALSE;
    result = R_SCI_SPI_WriteRead(&g_sci_spi3_ctrl, tx, rx, length, SPI_BIT_WIDTH_8_BITS);
    if (result != FSP_SUCCESS) return -RT_EIO;
    while (!ble_spi_done)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(100))
        {
            /* Stop IRQ/DMA access before the caller's header buffers go out of scope. */
            R_SCI_SPI_Close(&g_sci_spi3_ctrl);
            ble_spi_open = RT_FALSE;
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(1);
    }
    return ble_spi_event == SPI_EVENT_TRANSFER_COMPLETE ? RT_EOK : -RT_EIO;
}

static rt_err_t ble_wait_irq(rt_uint32_t previous)
{
    rt_tick_t start = rt_tick_get();
    while (ble_irqs == previous)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(100))
            return -RT_ETIMEOUT;
        rt_thread_mdelay(1);
    }
    return RT_EOK;
}

static rt_bool_t ble_valid_header(const rt_uint8_t *header, unsigned int phase)
{
    return ble_get32(header + 8) == 0x98BADCFE && ble_get32(header + 12) == 0x10325476 &&
           (header[1] >> 4) == phase;
}

/* Two-phase full-duplex protocol; CS remains low between data header and payload. */
static rt_err_t ble_transfer(rt_uint16_t tx_length)
{
    rt_uint8_t command[16] BSP_ALIGN_VARIABLE(4) = {0};
    rt_uint8_t response[16] BSP_ALIGN_VARIABLE(4) = {0};
    rt_uint32_t edge;
    rt_uint16_t wire_length;
    rt_err_t result;
    ble_rx_length = 0;
    command[1] = 0x11; /* command phase + host ready */
    if (++ble_sequence >= 65534) ble_sequence = 1;
    ble_put16(command + 4, ble_sequence);
    ble_put16(command + 6, tx_length);
    ble_put32(command + 8, 0x67452301);
    ble_put32(command + 12, 0xEFCDAB89);
    R_IOPORT_PinWrite(&g_ioport_ctrl, BLE_CS, BSP_IO_LEVEL_LOW);
    result = ble_exchange(command, response, sizeof(command));
    edge = ble_irqs;
    R_IOPORT_PinWrite(&g_ioport_ctrl, BLE_CS, BSP_IO_LEVEL_HIGH);
    if (result != RT_EOK) goto failed;
    if (!ble_valid_header(response, 3))
    {
        rt_kprintf("BLE: bad SPI header phase1 %08X %08X flags=%02X\n",
                   ble_get32(response + 8), ble_get32(response + 12), response[1]);
        result = -RT_EIO;
        goto failed;
    }
    result = ble_wait_irq(edge);
    if (result != RT_EOK) goto failed;

    command[1] = 0x21;
    R_IOPORT_PinWrite(&g_ioport_ctrl, BLE_CS, BSP_IO_LEVEL_LOW);
    result = ble_exchange(command, response, sizeof(command));
    if (result != RT_EOK) goto failed;
    if (!ble_valid_header(response, 4) || ble_get16(response + 4) != ble_sequence)
    {
        rt_kprintf("BLE: bad SPI header phase2 seq=%u/%u flags=%02X\n",
                   ble_get16(response + 4), ble_sequence, response[1]);
        result = -RT_EIO;
        goto failed;
    }
    ble_rx_length = ble_get16(response + 6);
    if (ble_rx_length > BLE_PACKET_MAX || (tx_length && !(response[1] & 1)))
    {
        result = -RT_EIO;
        goto failed;
    }
    wire_length = tx_length > ble_rx_length ? tx_length : ble_rx_length;
    wire_length = RT_ALIGN(wire_length, 4);
    rt_memset(ble_rx, 0, sizeof(ble_rx));
    result = ble_exchange(ble_tx, ble_rx, wire_length);
    edge = ble_irqs;
    R_IOPORT_PinWrite(&g_ioport_ctrl, BLE_CS, BSP_IO_LEVEL_HIGH);
    if (result != RT_EOK) goto failed;
    result = ble_wait_irq(edge);
    if (result != RT_EOK) goto failed;
    ble_transfers++;
    return RT_EOK;
failed:
    R_IOPORT_PinWrite(&g_ioport_ctrl, BLE_CS, BSP_IO_LEVEL_HIGH);
    ble_errors++;
    return result;
}

static rt_err_t ble_parse(void)
{
    rt_uint32_t length, type;
    const rt_uint8_t *data = ble_rx + 8;
    if (!ble_rx_length) return RT_EOK;
    if (ble_rx_length < 8) return -RT_EIO;
    length = ble_get32(ble_rx);
    type = ble_get32(ble_rx + 4);
    if (length > (rt_uint32_t)(ble_rx_length - 8)) return -RT_EIO;
    ble_probe_packet(data, length, type);
    if (type == 5) /* module command response */
    {
        rt_uint32_t command, size;
        if (length < 12) return -RT_EIO;
        command = ble_get32(data);
        size = ble_get32(data + 4);
        if (size > length - 12) return -RT_EIO;
        ble_response_cmd = command;
        ble_response_result = (rt_int32_t)ble_get32(data + 8);
        ble_response_seen = RT_TRUE;
        rt_kprintf("BLE: command=%u result=%d data=%u bytes\n", command, ble_response_result, size);
        if (command == BLE_CMD_VERSION && ble_response_result == 0)
        {
            if (size >= sizeof(ble_version)) size = sizeof(ble_version) - 1;
            rt_memcpy(ble_version, data + 12, size);
            ble_version[size] = 0;
            rt_kprintf("BLE: RW007 firmware=%s\n", ble_version);
        }
        if (command == BLE_CMD_MAC && ble_response_result == 0 && size >= 6)
        {
            rt_memcpy(ble_mac, data + 12, 6);
            ble_mac_valid = RT_TRUE;
            rt_kprintf("BLE: Wi-Fi MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                       ble_mac[0], ble_mac[1], ble_mac[2], ble_mac[3], ble_mac[4], ble_mac[5]);
        }
    }
    else if (type == 7) /* BLE responses/notifications can share one packet */
    {
        while (length)
        {
            rt_uint16_t response_type, command, size, result;
            if (length < 8) return -RT_EIO;
            response_type = ble_get16(data);
            command = ble_get16(data + 2);
            size = ble_get16(data + 4);
            result = ble_get16(data + 6);
            if (size > length - 8) return -RT_EIO;
            rt_kprintf("BLE: event type=%u cmd=0x%02X result=%u bytes=%u\n",
                       response_type, command, result, size);
            if (response_type == 0 && command == 0x50)
            {
                ble_init_seen = RT_TRUE;
                ble_init_result = result;
            }
            if (response_type == 1 && command == 0xA7)
            {
                ble_advertising = RT_FALSE;
                if (size >= 8)
                    rt_kprintf("BLE: advertising ended, reason=%d; restart scheduled\n",
                               (rt_int32_t)ble_get32(data + 12));
            }
            data += size + 8;
            length -= size + 8;
        }
    }
    return RT_EOK;
}

static rt_err_t ble_command(rt_uint32_t command, const void *value, rt_uint32_t size)
{
    rt_tick_t start;
    rt_err_t result;
    int attempt;
    if (size > BLE_PACKET_MAX - 16) return -RT_EINVAL;
    rt_memset(ble_tx, 0, sizeof(ble_tx));
    ble_put32(ble_tx, size + 8);
    ble_put32(ble_tx + 4, 4);
    ble_put32(ble_tx + 8, command);
    ble_put32(ble_tx + 12, size);
    if (size) rt_memcpy(ble_tx + 16, value, size);
    ble_response_seen = RT_FALSE;
    for (attempt = 0; attempt < 3; attempt++)
    {
        result = ble_transfer(size + 16);
        if (result == RT_EOK || !ble_spi_open) break;
        rt_thread_mdelay(10);
    }
    if (result != RT_EOK || (result = ble_parse()) != RT_EOK) return result;
    start = rt_tick_get();
    while (!ble_response_seen || ble_response_cmd != command)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(3000))
            return -RT_ETIMEOUT;
        rt_thread_mdelay(10);
        rt_memset(ble_tx, 0, sizeof(ble_tx));
        result = ble_transfer(0);
        if (result != RT_EOK || (result = ble_parse()) != RT_EOK) return result;
    }
    return ble_response_result;
}

static rt_err_t ble_hardware_init(void)
{
    rt_tick_t start;
    bsp_io_level_t level;
    const rt_uint32_t output_low = IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW;
    const rt_uint32_t sci_pin = IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_SCI1_3_5_7_9;
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, BLE_RST, output_low) != FSP_SUCCESS ||
        /* Match the factory BSP: do not drive the module's boot straps during reset. */
        R_IOPORT_PinCfg(&g_ioport_ctrl, BLE_CS, IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, BLE_INT, IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS)
        return -RT_ERROR;
    rt_thread_mdelay(100);
    R_IOPORT_PinWrite(&g_ioport_ctrl, BLE_RST, BSP_IO_LEVEL_HIGH);
    start = rt_tick_get();
    do
    {
        if (R_IOPORT_PinRead(&g_ioport_ctrl, BLE_INT, &level) != FSP_SUCCESS) return -RT_EIO;
        if (level == BSP_IO_LEVEL_HIGH) break;
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(3000)) return -RT_ETIMEOUT;
        rt_thread_mdelay(5);
    } while (1);
    rt_thread_mdelay(1000);
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, BLE_CS, IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                        IOPORT_CFG_PORT_OUTPUT_HIGH) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, BLE_MISO, sci_pin) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, BLE_MOSI, sci_pin) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, BLE_CLK, sci_pin) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, BLE_INT, IOPORT_CFG_PORT_DIRECTION_INPUT |
                        IOPORT_CFG_PULLUP_ENABLE | IOPORT_CFG_IRQ_ENABLE) != FSP_SUCCESS) return -RT_ERROR;
    ble_irq_config = g_external_irq13_cfg;
    ble_irq_config.trigger = EXTERNAL_IRQ_TRIG_FALLING;
    ble_irq_config.p_callback = ble_irq_callback;
    if (R_ICU_ExternalIrqOpen(&g_external_irq13_ctrl, &ble_irq_config) != FSP_SUCCESS) return -RT_ERROR;
    ble_irq_open = RT_TRUE;
    if (R_ICU_ExternalIrqEnable(&g_external_irq13_ctrl) != FSP_SUCCESS) return -RT_ERROR;
    ble_spi_config = g_sci_spi3_cfg;
    ble_spi_extended = *(const sci_spi_extended_cfg_t *)g_sci_spi3_cfg.p_extend;
    if (R_SCI_SPI_CalculateBitrate(1000000, &ble_spi_extended.clk_div, false) != FSP_SUCCESS)
        return -RT_ERROR;
    ble_spi_config.p_extend = &ble_spi_extended;
    ble_spi_config.p_transfer_tx = RT_NULL;
    ble_spi_config.p_transfer_rx = RT_NULL;
    ble_spi_config.p_callback = ble_spi_callback;
    if (R_SCI_SPI_Open(&g_sci_spi3_ctrl, &ble_spi_config) != FSP_SUCCESS) return -RT_ERROR;
    ble_spi_open = RT_TRUE;
    rt_kprintf("BLE: RW007 SCI3 mode0 1MHz, IRQ13 ready\n");
    return RT_EOK;
}

static void ble_shutdown(void)
{
    ble_ready = RT_FALSE;
    ble_advertising = RT_FALSE;
    if (ble_irq_open) R_ICU_ExternalIrqClose(&g_external_irq13_ctrl);
    if (ble_spi_open) R_SCI_SPI_Close(&g_sci_spi3_ctrl);
    R_IOPORT_PinWrite(&g_ioport_ctrl, BLE_CS, BSP_IO_LEVEL_HIGH);
    R_IOPORT_PinWrite(&g_ioport_ctrl, BLE_RST, BSP_IO_LEVEL_LOW);
}

static void ble_thread(void *parameter)
{
    rt_err_t result;
    rt_tick_t started;
    rt_uint8_t duration[4];
    RT_UNUSED(parameter);
    result = ble_hardware_init();
    if (result != RT_EOK) goto failed;
    result = ble_command(BLE_CMD_VERSION, RT_NULL, 0);
    if (result != RT_EOK) goto failed;
    result = ble_command(BLE_CMD_INIT, RT_NULL, 0);
    if (result != RT_EOK) goto failed;
    result = ble_command(BLE_CMD_MAC, RT_NULL, 0);
    if (result != RT_EOK) goto failed;

    /* BLE peripheral init request: packet type=7, request=1, role=1. */
    rt_memset(ble_tx, 0, sizeof(ble_tx));
    ble_put32(ble_tx, 8);
    ble_put32(ble_tx + 4, 7);
    ble_put16(ble_tx + 8, 1);
    ble_put16(ble_tx + 10, 4);
    ble_put32(ble_tx + 12, 1);
    result = ble_transfer(16);
    if (result != RT_EOK || (result = ble_parse()) != RT_EOK) goto failed;
    started = rt_tick_get();
    while (!ble_init_seen)
    {
        if ((rt_tick_t)(rt_tick_get() - started) >= rt_tick_from_millisecond(300))
        {
            /* Upstream init only queues this request; some firmware has no init ACK.
             * The following synchronous advertising command must still succeed. */
            rt_kprintf("BLE: peripheral init queued; checking advertising command response\n");
            break;
        }
        rt_thread_mdelay(10);
        rt_memset(ble_tx, 0, sizeof(ble_tx));
        result = ble_transfer(0);
        if (result != RT_EOK || (result = ble_parse()) != RT_EOK) goto failed;
    }
    if (ble_init_seen && ble_init_result != 0) { result = -RT_ERROR; goto failed; }
    ble_put32(duration, BLE_ADV_MS);
    result = ble_command(BLE_CMD_ADV, duration, sizeof(duration));
    if (result != RT_EOK) goto failed;
    ble_ready = RT_TRUE;
    ble_advertising = RT_TRUE;
    rt_kprintf("BLE: firmware accepted 60s advertising; scan for RW007-xxxx on phone\n");
    for (;;)
    {
        /* This firmware can ACK a renewal while the old timer still expires.
         * Restart after its completion event, with a bounded fallback timeout. */
        started = rt_tick_get();
        while (ble_advertising && (rt_tick_t)(rt_tick_get() - started) < rt_tick_from_millisecond(BLE_ADV_MS + 5000))
        {
            rt_thread_mdelay(100);
            rt_memset(ble_tx, 0, sizeof(ble_tx));
            result = ble_transfer(0);
            if (result != RT_EOK || (result = ble_parse()) != RT_EOK) goto failed;
        }
        result = ble_command(BLE_CMD_ADV, duration, sizeof(duration));
        if (result != RT_EOK) goto failed;
        ble_advertising = RT_TRUE;
        rt_kprintf("BLE: advertising restarted\n");
    }
failed:
    rt_kprintf("BLE: stopped result=%d transfers=%u errors=%u irqs=%u\n",
               result, ble_transfers, ble_errors, ble_irqs);
    ble_shutdown();
}

static int ble_start(void)
{
    rt_err_t result;
    rt_thread_t thread = rt_thread_create("ble", ble_thread, RT_NULL, 3072, 20, 10);
    if (thread == RT_NULL) return -RT_ENOMEM;
    result = rt_thread_startup(thread);
    if (result != RT_EOK) rt_thread_delete(thread);
    return result;
}
// INIT_APP_EXPORT(ble_start);

static void ble_status(void)
{
    rt_kprintf("BLE: ready=%d adv_command_ok=%d firmware=%s transfers=%u errors=%u irqs=%u\n",
               ble_ready, ble_advertising, ble_version, ble_transfers, ble_errors, ble_irqs);
    if (ble_mac_valid) rt_kprintf("BLE: expected name RW007-%02X%02X (confirm in phone scan)\n", ble_mac[4], ble_mac[5]);
}
MSH_CMD_EXPORT(ble_status, Show RW007 BLE discovery demo status);

static void ble_probe(void)
{
    rt_kprintf("BLE probe: packets=%u ble=%u other=%u marker_packets=%u\n",
               ble_rx_packets, ble_ble_packets, ble_other_packets, ble_probe_packets);
    rt_kprintf("BLE probe: phone write ASCII HMI_PROBE1 to FF01; marker detection is per SPI packet\n");
}
MSH_CMD_EXPORT(ble_probe, Show phone-to-MCU transport probe counters);
