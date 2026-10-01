/*
 * SPDX-License-Identifier: Apache-2.0
 * RW007 firmware feature test for HMI-Board V3.1.
 * File spelling follows the requested rwoo7-demo.c name.
 * SPI transport follows this project's verified ble.c; protocol reference:
 * RT-Thread-packages/rw007 @ 94df57f856bc2e8661022a379aa1c3d6a3bc5149.
 * Tests discovery/query functions and an explicit one-shot internet request.
 * Credentials are supplied at runtime. No module firmware update.
 */
#include "peripheral-test.h"
#include <rtthread.h>
#include "hal_data.h"
#include "rw007-internet.h"

#define RW_CS BSP_IO_PORT_03_PIN_08
#define RW_MISO BSP_IO_PORT_03_PIN_09
#define RW_MOSI BSP_IO_PORT_03_PIN_10
#define RW_CLK BSP_IO_PORT_03_PIN_11
#define RW_RST BSP_IO_PORT_03_PIN_12
#define RW_INT BSP_IO_PORT_00_PIN_15
#define RW_PACKET_MAX 1528
#define RW_INFO 1
#define RW_WIFI 2
#define RW_BLE 3
#define RW_ADV 4
#define RW_ALL 5
#define RW_INTERNET 6

static spi_cfg_t rw_spi_config;
static sci_spi_extended_cfg_t rw_spi_extended;
static external_irq_cfg_t rw_irq_config;
static volatile rt_bool_t rw_spi_done;
static volatile spi_event_t rw_spi_event;
static volatile rt_uint32_t rw_irqs, rw_transfers, rw_errors;
static volatile rt_bool_t rw_ready, rw_advertising, rw_busy;
static rt_bool_t rw_spi_open, rw_irq_open;
static rt_uint16_t rw_sequence, rw_rx_length;
static rt_uint8_t rw_tx[RW_PACKET_MAX] BSP_ALIGN_VARIABLE(4);
static rt_uint8_t rw_rx[RW_PACKET_MAX] BSP_ALIGN_VARIABLE(4);
static struct rt_mailbox rw_jobs;
static rt_ubase_t rw_job_pool[4];
static volatile rt_bool_t rw_worker_ready;
static rt_uint32_t rw_rsp_command;
static rt_int32_t rw_rsp_result;
static rt_bool_t rw_rsp_seen;
static rt_uint8_t rw_value[96];
static rt_uint32_t rw_value_size;
static rt_bool_t rw_ble_seen;
static rt_uint16_t rw_ble_command, rw_ble_result;
static rt_bool_t rw_wifi_done, rw_scan_done;
static rt_int32_t rw_wifi_result, rw_scan_reason;
static volatile rt_uint32_t rw_wifi_reports, rw_ble_reports, rw_bad_packets;
static volatile rt_uint32_t rw_pass, rw_fail, rw_run;
static rt_tick_t rw_adv_started;
static volatile rt_bool_t rw_connected;
static char rw_version[64];
static char rw_net_ssid[33], rw_net_key[32];
static volatile rt_bool_t rw_net_pending;
static rt_uint8_t rw_target_info[60];
static rt_bool_t rw_target_found, rw_sta_done;
static rt_int32_t rw_sta_result;

static rt_uint16_t rw_get16(const rt_uint8_t *p)
{
    return p[0] | ((rt_uint16_t)p[1] << 8);
}

static rt_uint32_t rw_get32(const rt_uint8_t *p)
{
    return p[0] | ((rt_uint32_t)p[1] << 8) | ((rt_uint32_t)p[2] << 16) | ((rt_uint32_t)p[3] << 24);
}

static void rw_put16(rt_uint8_t *p, rt_uint16_t value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void rw_put32(rt_uint8_t *p, rt_uint32_t value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static void rw_spi_callback(spi_callback_args_t *args)
{
    rw_spi_event = args->event;
    rw_spi_done = RT_TRUE;
}

static void rw_irq_callback(external_irq_callback_args_t *args)
{
    RT_UNUSED(args);
    rw_irqs++;
}

static rt_err_t rw_exchange(const void *tx, void *rx, rt_uint32_t length)
{
    rt_tick_t start = rt_tick_get();
    fsp_err_t result;
    if (!length)
    {
        return RT_EOK;
    }
    rw_spi_done = RT_FALSE;
    result = R_SCI_SPI_WriteRead(&g_sci_spi3_ctrl, tx, rx, length, SPI_BIT_WIDTH_8_BITS);
    if (result != FSP_SUCCESS)
    {
        return -RT_EIO;
    }
    while (!rw_spi_done)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(100))
        {
            /* Stop IRQ/DMA access before the caller's header buffers go out of scope. */
            R_SCI_SPI_Close(&g_sci_spi3_ctrl);
            rw_spi_open = RT_FALSE;
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(1);
    }
    return rw_spi_event == SPI_EVENT_TRANSFER_COMPLETE ? RT_EOK : -RT_EIO;
}

static rt_err_t rw_wait_irq(rt_uint32_t previous)
{
    rt_tick_t start = rt_tick_get();
    while (rw_irqs == previous)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(100))
        {
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(1);
    }
    return RT_EOK;
}

static rt_bool_t rw_valid_header(const rt_uint8_t *header, unsigned int phase)
{
    return rw_get32(header + 8) == 0x98BADCFE && rw_get32(header + 12) == 0x10325476 &&
           (header[1] >> 4) == phase;
}

/* Two-phase full-duplex protocol; CS remains low between data header and payload. */
static rt_err_t rw_transfer(rt_uint16_t tx_length)
{
    rt_uint8_t command[16] BSP_ALIGN_VARIABLE(4) = {0};
    rt_uint8_t response[16] BSP_ALIGN_VARIABLE(4) = {0};
    rt_uint32_t edge;
    rt_uint16_t wire_length;
    rt_err_t result;
    rw_rx_length = 0;
    command[1] = 0x11; /* command phase + host ready */
    if (++rw_sequence >= 65534)
    {
        rw_sequence = 1;
    }
    rw_put16(command + 4, rw_sequence);
    rw_put16(command + 6, tx_length);
    rw_put32(command + 8, 0x67452301);
    rw_put32(command + 12, 0xEFCDAB89);
    R_IOPORT_PinWrite(&g_ioport_ctrl, RW_CS, BSP_IO_LEVEL_LOW);
    result = rw_exchange(command, response, sizeof(command));
    edge = rw_irqs;
    R_IOPORT_PinWrite(&g_ioport_ctrl, RW_CS, BSP_IO_LEVEL_HIGH);
    if (result != RT_EOK)
    {
        goto failed;
    }
    if (!rw_valid_header(response, 3))
    {
        rt_kprintf("RW007: bad SPI header phase1 %08X %08X flags=%02X\n",
                   rw_get32(response + 8),
                   rw_get32(response + 12),
                   response[1]);
        result = -RT_EIO;
        goto failed;
    }
    result = rw_wait_irq(edge);
    if (result != RT_EOK)
    {
        goto failed;
    }

    command[1] = 0x21;
    R_IOPORT_PinWrite(&g_ioport_ctrl, RW_CS, BSP_IO_LEVEL_LOW);
    result = rw_exchange(command, response, sizeof(command));
    if (result != RT_EOK)
    {
        goto failed;
    }
    if (!rw_valid_header(response, 4) || rw_get16(response + 4) != rw_sequence)
    {
        rt_kprintf("RW007: bad SPI header phase2 seq=%u/%u flags=%02X\n",
                   rw_get16(response + 4),
                   rw_sequence,
                   response[1]);
        result = -RT_EIO;
        goto failed;
    }
    rw_rx_length = rw_get16(response + 6);
    if (rw_rx_length > RW_PACKET_MAX || (tx_length && !(response[1] & 1)))
    {
        result = -RT_EIO;
        goto failed;
    }
    wire_length = tx_length > rw_rx_length ? tx_length : rw_rx_length;
    wire_length = RT_ALIGN(wire_length, 4);
    rt_memset(rw_rx, 0, sizeof(rw_rx));
    result = rw_exchange(rw_tx, rw_rx, wire_length);
    edge = rw_irqs;
    R_IOPORT_PinWrite(&g_ioport_ctrl, RW_CS, BSP_IO_LEVEL_HIGH);
    if (result != RT_EOK)
    {
        goto failed;
    }
    result = rw_wait_irq(edge);
    if (result != RT_EOK)
    {
        goto failed;
    }
    rw_transfers++;
    return RT_EOK;
failed:
    R_IOPORT_PinWrite(&g_ioport_ctrl, RW_CS, BSP_IO_LEVEL_HIGH);
    rw_errors++;
    return result;
}

static rt_err_t rw_hardware_init(void)
{
    rt_tick_t start;
    bsp_io_level_t level;
    const rt_uint32_t output_low = IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW;
    const rt_uint32_t sci_pin = IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_SCI1_3_5_7_9;
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, RW_RST, output_low) != FSP_SUCCESS ||
        /* Match the factory BSP: do not drive the module's boot straps during reset. */
        R_IOPORT_PinCfg(&g_ioport_ctrl, RW_CS, IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, RW_INT, IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rt_thread_mdelay(100);
    R_IOPORT_PinWrite(&g_ioport_ctrl, RW_RST, BSP_IO_LEVEL_HIGH);
    start = rt_tick_get();
    do
    {
        if (R_IOPORT_PinRead(&g_ioport_ctrl, RW_INT, &level) != FSP_SUCCESS)
        {
            return -RT_EIO;
        }
        if (level == BSP_IO_LEVEL_HIGH)
        {
            break;
        }
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(3000))
        {
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(5);
    } while (1);
    rt_thread_mdelay(1000);
    if (R_IOPORT_PinCfg(&g_ioport_ctrl,
                        RW_CS,
                        IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH) !=
            FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, RW_MISO, sci_pin) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, RW_MOSI, sci_pin) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl, RW_CLK, sci_pin) != FSP_SUCCESS ||
        R_IOPORT_PinCfg(&g_ioport_ctrl,
                        RW_INT,
                        IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_PULLUP_ENABLE |
                            IOPORT_CFG_IRQ_ENABLE) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rw_irq_config = g_external_irq13_cfg;
    rw_irq_config.trigger = EXTERNAL_IRQ_TRIG_FALLING;
    rw_irq_config.p_callback = rw_irq_callback;
    if (R_ICU_ExternalIrqOpen(&g_external_irq13_ctrl, &rw_irq_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rw_irq_open = RT_TRUE;
    if (R_ICU_ExternalIrqEnable(&g_external_irq13_ctrl) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rw_spi_config = g_sci_spi3_cfg;
    rw_spi_extended = *(const sci_spi_extended_cfg_t *)g_sci_spi3_cfg.p_extend;
    if (R_SCI_SPI_CalculateBitrate(1000000, &rw_spi_extended.clk_div, false) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rw_spi_config.p_extend = &rw_spi_extended;
    rw_spi_config.p_transfer_tx = RT_NULL;
    rw_spi_config.p_transfer_rx = RT_NULL;
    rw_spi_config.p_callback = rw_spi_callback;
    if (R_SCI_SPI_Open(&g_sci_spi3_ctrl, &rw_spi_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rw_spi_open = RT_TRUE;
    rt_kprintf("RW007: RW007 SCI3 mode0 1MHz, IRQ13 ready\n");
    return RT_EOK;
}

static void rw_shutdown(void)
{
    rw_ready = RT_FALSE;
    rw_advertising = RT_FALSE;
    if (rw_irq_open)
    {
        R_ICU_ExternalIrqClose(&g_external_irq13_ctrl);
    }
    rw_irq_open = RT_FALSE;
    if (rw_spi_open)
    {
        R_SCI_SPI_Close(&g_sci_spi3_ctrl);
    }
    rw_spi_open = RT_FALSE;
    R_IOPORT_PinWrite(&g_ioport_ctrl, RW_CS, BSP_IO_LEVEL_HIGH);
    R_IOPORT_PinWrite(&g_ioport_ctrl, RW_RST, BSP_IO_LEVEL_LOW);
}

static void rw_text(char *out, const rt_uint8_t *in, unsigned int length)
{
    unsigned int i;
    for (i = 0; i < length; i++)
    {
        out[i] = (in[i] < 32 || in[i] == 127) ? '.' : in[i];
    }
    out[length] = 0;
}

static rt_err_t rw_parse(void)
{
    rt_uint32_t length, type;
    const rt_uint8_t *p = rw_rx + 8;
    if (!rw_rx_length)
    {
        return RT_EOK;
    }
    if (rw_rx_length < 8)
    {
        return -RT_EIO;
    }
    length = rw_get32(rw_rx);
    type = rw_get32(rw_rx + 4);
    if (length > (rt_uint32_t)rw_rx_length - 8)
    {
        return -RT_EIO;
    }
    if (type == 1)
    {
        rw007_net_input(p, length);
    }
    else if (type == 5 || type == 6)
    {
        rt_uint32_t command, size;
        rt_int32_t result;
        if (length < 12)
        {
            return -RT_EIO;
        }
        command = rw_get32(p);
        size = rw_get32(p + 4);
        result = (rt_int32_t)rw_get32(p + 8);
        if (size > length - 12)
        {
            return -RT_EIO;
        }
        p += 12;
        if (type == 5)
        {
            rw_rsp_command = command;
            rw_rsp_result = result;
            rw_rsp_seen = RT_TRUE;
            rw_value_size = size;
            rt_memset(rw_value, 0, sizeof(rw_value));
            rt_memcpy(rw_value, p, size < sizeof(rw_value) ? size : sizeof(rw_value));
            rt_kprintf("RW007: command=%u result=%d bytes=%u\n", command, result, size);
        }
        else if (command == 9) /* RT_WLAN_DEV_EVT_SCAN_REPORT, RT-Thread 5 ABI */
        {
            char ssid[33];
            /* rt_wlan_info: security 0, band 4, rate 8, channel 12,
             * rssi 14, ssid.len 16, ssid.val 17, bssid 50, hidden 56. */
            if (size < 57 || p[16] > 32)
            {
                return -RT_EIO;
            }
            rw_wifi_reports++;
            rw_text(ssid, p + 17, p[16]);
            if (rw_net_pending && size >= sizeof(rw_target_info) &&
                p[16] == rt_strlen(rw_net_ssid) && !rt_memcmp(p + 17, rw_net_ssid, p[16]))
            {
                if (!rw_target_found ||
                    (rt_int16_t)rw_get16(p + 14) > (rt_int16_t)rw_get16(rw_target_info + 14))
                {
                    rt_memcpy(rw_target_info, p, sizeof(rw_target_info));
                }
                rw_target_found = RT_TRUE;
            }
            if (rw_wifi_reports <= 24)
            {
                rt_kprintf("WIFI[%u]: SSID=%s channel=%d RSSI=%d security=0x%08X\n",
                           rw_wifi_reports,
                           p[16] ? ssid : "<hidden>",
                           (rt_int16_t)rw_get16(p + 12),
                           (rt_int16_t)rw_get16(p + 14),
                           rw_get32(p));
            }
        }
        else if (command == 10) /* RT_WLAN_DEV_EVT_SCAN_DONE */
        {
            rw_wifi_done = RT_TRUE;
            rw_wifi_result = result;
            rt_kprintf("WIFI: scan complete result=%d reports=%u\n", result, rw_wifi_reports);
        }
        else
        {
            if (command == 1 || command == 2 || command == 3)
            {
                rw_sta_done = RT_TRUE;
                rw_sta_result = command == 1 ? result : -RT_ERROR;
            }
            rt_kprintf("WIFI: event=%u result=%d bytes=%u\n", command, result, size);
        }
    }
    else if (type == 7)
    {
        while (length)
        {
            rt_uint16_t response_type, command, size, result;
            const rt_uint8_t *data;
            if (length < 8)
            {
                return -RT_EIO;
            }
            response_type = rw_get16(p);
            command = rw_get16(p + 2);
            size = rw_get16(p + 4);
            result = rw_get16(p + 6);
            if (size > length - 8)
            {
                return -RT_EIO;
            }
            data = p + 8;
            if (response_type == 0)
            {
                rw_ble_seen = RT_TRUE;
                rw_ble_command = command;
                rw_ble_result = result;
                rt_kprintf("BLE: response=0x%02X result=%u bytes=%u\n", command, result, size);
                if (command == 0x51 && !result && size >= 12)
                {
                    rt_kprintf("BLE: public=%02X:%02X:%02X:%02X:%02X:%02X\n",
                               data[5],
                               data[4],
                               data[3],
                               data[2],
                               data[1],
                               data[0]);
                }
            }
            else if (response_type == 1 && command == 0xA2)
            {
                char name[33] = "<unnamed>";
                rt_uint32_t offset = 20, end;
                if (size < 20 || data[2] > size - 20)
                {
                    return -RT_EIO;
                }
                end = 20 + data[2];
                while (offset < end)
                {
                    unsigned int field_size = data[offset];
                    if (!field_size)
                    {
                        break;
                    }
                    if (field_size > end - offset - 1)
                    {
                        return -RT_EIO;
                    }
                    if (field_size >= 2 && (data[offset + 1] == 8 || data[offset + 1] == 9))
                    {
                        unsigned int n = field_size - 1;
                        rw_text(name, data + offset + 2, n < 32 ? n : 32);
                    }
                    offset += field_size + 1;
                }
                rw_ble_reports++;
                if (rw_ble_reports <= 24)
                {
                    rt_kprintf("BLE[%u]: %02X:%02X:%02X:%02X:%02X:%02X type=%u RSSI=%d name=%s\n",
                               rw_ble_reports,
                               data[11],
                               data[10],
                               data[9],
                               data[8],
                               data[7],
                               data[6],
                               data[4],
                               (rt_int8_t)data[3],
                               name);
                }
            }
            else if (response_type == 1 && command == 0xA6)
            {
                if (size < 8)
                {
                    return -RT_EIO;
                }
                rw_scan_done = RT_TRUE;
                rw_scan_reason = (rt_int32_t)rw_get32(data + 4);
                rt_kprintf(
                    "BLE: scan complete reason=%d reports=%u\n", rw_scan_reason, rw_ble_reports);
            }
            else if (response_type == 1 && command == 0xA7)
            {
                rw_advertising = RT_FALSE;
                rt_kprintf("BLE: advertising complete\n");
            }
            else if (response_type == 1 && command == 0xA3)
            {
                if (size < 8)
                {
                    return -RT_EIO;
                }
                rw_connected = rw_get32(data + 4) == 0;
                if (rw_connected)
                {
                    rw_advertising = RT_FALSE;
                }
                rt_kprintf("BLE: connection status=%d\n", (rt_int32_t)rw_get32(data + 4));
            }
            else if (response_type == 1 && command == 0xA4)
            {
                rw_connected = RT_FALSE;
                rt_kprintf("BLE: disconnected\n");
            }
            else
            {
                rt_kprintf("BLE: event=0x%02X result=%u bytes=%u\n", command, result, size);
            }
            p += size + 8;
            length -= size + 8;
        }
    }
    return RT_EOK;
}

static rt_err_t rw_receive(void)
{
    rt_err_t result;
    rt_memset(rw_tx, 0, sizeof(rw_tx));
    result = rw_transfer(0);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rw_parse();
    if (result != RT_EOK)
    {
        rw_bad_packets++;
    }
    return result;
}

static rt_err_t rw_command(rt_uint32_t command, const void *value, rt_uint32_t size)
{
    rt_tick_t start;
    rt_err_t result;
    int attempt;
    if (size > RW_PACKET_MAX - 16)
    {
        return -RT_EINVAL;
    }
    rt_memset(rw_tx, 0, sizeof(rw_tx));
    rw_put32(rw_tx, size + 8);
    rw_put32(rw_tx + 4, 4);
    rw_put32(rw_tx + 8, command);
    rw_put32(rw_tx + 12, size);
    if (size)
    {
        rt_memcpy(rw_tx + 16, value, size);
    }
    rw_rsp_seen = RT_FALSE;
    for (attempt = 0; attempt < 3; attempt++)
    {
        result = rw_transfer(size + 16);
        if (result == RT_EOK || !rw_spi_open)
        {
            break;
        }
        rt_thread_mdelay(10);
    }
    if (result != RT_EOK)
    {
        return result;
    }
    result = rw_parse();
    if (result != RT_EOK)
    {
        rw_bad_packets++;
        return result;
    }
    start = rt_tick_get();
    while (!rw_rsp_seen || rw_rsp_command != command)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(10000))
        {
            return -RT_ETIMEOUT;
        }
        rt_thread_mdelay(5);
        result = rw_receive();
        if (result != RT_EOK)
        {
            return result;
        }
    }
    return rw_rsp_result;
}

static rt_err_t
rw_ble_request(rt_uint16_t command, const void *value, rt_uint16_t size, rt_uint16_t expected)
{
    rt_tick_t start;
    rt_err_t result;
    if (size > RW_PACKET_MAX - 12)
    {
        return -RT_EINVAL;
    }
    rt_memset(rw_tx, 0, sizeof(rw_tx));
    rw_put32(rw_tx, size + 4);
    rw_put32(rw_tx + 4, 7);
    rw_put16(rw_tx + 8, command);
    rw_put16(rw_tx + 10, size);
    if (size)
    {
        rt_memcpy(rw_tx + 12, value, size);
    }
    rw_ble_seen = RT_FALSE;
    result = rw_transfer(size + 12);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rw_parse();
    if (result != RT_EOK)
    {
        rw_bad_packets++;
        return result;
    }
    /* Scan start is asynchronous on the on-board firmware. Its completion
     * and optional rejection response are checked by rw_ble_test. */
    if (!expected)
    {
        return RT_EOK;
    }
    start = rt_tick_get();
    while (!rw_ble_seen || rw_ble_command != expected)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >=
            rt_tick_from_millisecond(expected == 0x50 ? 300 : 3000))
        {
            if (expected != 0x50)
            {
                return -RT_ETIMEOUT;
            }
            rt_kprintf("BLE: init queued without ACK; following operation must confirm support\n");
            return RT_EOK;
        }
        rt_thread_mdelay(5);
        result = rw_receive();
        if (result != RT_EOK)
        {
            return result;
        }
    }
    return rw_ble_result ? -RT_ERROR : RT_EOK;
}

static rt_err_t rw_prepare(void)
{
    rt_err_t result;
    rw_shutdown();
    rw_connected = RT_FALSE;
    rw_sequence = 0;
    result = rw_hardware_init();
    if (result != RT_EOK)
    {
        return result;
    }
    result = rw_command(0, RT_NULL, 0);
    rw_ready = result == RT_EOK;
    return result;
}

static rt_err_t rw_info_test(void)
{
    rt_err_t result;
    char serial[65];
    result = rw_command(5, RT_NULL, 0);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!rw_value_size || rw_value_size >= sizeof(rw_version))
    {
        return -RT_EIO;
    }
    rw_value[rw_value_size] = 0;
    rw_text(rw_version, rw_value, rt_strlen((const char *)rw_value));
    rt_kprintf("INFO: firmware=%s\n", rw_version);
    result = rw_command(4, RT_NULL, 0);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!rw_value_size || rw_value_size >= sizeof(serial))
    {
        return -RT_EIO;
    }
    rw_value[rw_value_size] = 0;
    rw_text(serial, rw_value, rt_strlen((const char *)rw_value));
    rt_kprintf("INFO: serial=%s\n", serial);
    result = rw_command(2, RT_NULL, 0);
    if (result != RT_EOK)
    {
        return result;
    }
    if (rw_value_size < 6)
    {
        return -RT_EIO;
    }
    rt_kprintf("INFO: Wi-Fi MAC=%02X:%02X:%02X:%02X:%02X:%02X expected BLE name=RW007-%02X%02X\n",
               rw_value[0],
               rw_value[1],
               rw_value[2],
               rw_value[3],
               rw_value[4],
               rw_value[5],
               rw_value[4],
               rw_value[5]);
    return RT_EOK;
}

static rt_err_t rw_wifi_test(void)
{
    rt_uint8_t mode[4] = {1, 0, 0, 0}; /* station */
    rt_tick_t start;
    rt_err_t result;
    rw_wifi_reports = 0;
    rw_wifi_done = RT_FALSE;
    result = rw_command(1, mode, sizeof(mode));
    if (result != RT_EOK)
    {
        return result;
    }
    result = rw_command(6, RT_NULL, 0);
    if (result != RT_EOK)
    {
        return result;
    }
    start = rt_tick_get();
    while (!rw_wifi_done)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(15000))
        {
            rw_command(12, RT_NULL, 0); /* stop scan; no success inferred from this ACK */
            return -RT_ETIMEOUT;
        }
        result = rw_receive();
        if (result != RT_EOK)
        {
            return result;
        }
        rt_thread_mdelay(5);
    }
    return rw_wifi_result;
}

static rt_err_t rw_ble_test(void)
{
    rt_uint8_t role[4] = {2, 0, 0, 0}; /* central */
    rt_uint8_t scan[12] = {0};
    rt_tick_t start;
    rt_err_t result;
    rw_ble_reports = 0;
    rw_scan_done = RT_FALSE;
    result = rw_ble_request(1, role, sizeof(role), 0x50);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rw_ble_request(2, RT_NULL, 0, 0x51);
    if (result != RT_EOK)
    {
        return result;
    }
    scan[1] = 1; /* passive: listen to advertising without scan requests */
    rw_put32(scan + 4, 3000);
    result = rw_ble_request(5, scan, sizeof(scan), 0);
    if (result != RT_EOK)
    {
        return result;
    }
    start = rt_tick_get();
    while (!rw_scan_done)
    {
        if (rw_ble_seen && rw_ble_command == 0x54 && rw_ble_result)
        {
            return -RT_ERROR;
        }
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(6000))
        {
            return -RT_ETIMEOUT;
        }
        result = rw_receive();
        if (result != RT_EOK)
        {
            return result;
        }
        rt_thread_mdelay(5);
    }
    /* NimBLE reports 0 for a completed discovery procedure. */
    return rw_scan_reason == 0 ? RT_EOK : -RT_ERROR;
}

static rt_err_t rw_advertise_test(void)
{
    rt_uint8_t role[4] = {1, 0, 0, 0};
    rt_uint8_t duration[4];
    rt_err_t result = rw_ble_request(1, role, sizeof(role), 0x50);
    if (result != RT_EOK)
    {
        return result;
    }
    rw_put32(duration, 60000);
    result = rw_command(25, duration, sizeof(duration));
    if (result == RT_EOK)
    {
        rw_advertising = RT_TRUE;
        rw_adv_started = rt_tick_get();
        rt_kprintf("ADV: command accepted for 60 seconds; verify discovery on phone\n");
    }
    return result;
}

static rt_err_t rw_net_exchange(const void *frame, rt_uint16_t size)
{
    rt_err_t result;
    if (!frame)
    {
        return rw_receive();
    }
    if (size > RW_PACKET_MAX - 8)
    {
        return -RT_EINVAL;
    }
    rt_memset(rw_tx, 0, sizeof(rw_tx));
    rw_put32(rw_tx, size);
    rw_put32(rw_tx + 4, 1); /* DATA_TYPE_STA_ETH_DATA */
    rt_memcpy(rw_tx + 8, frame, size);
    result = rw_transfer(size + 8);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rw_parse();
    if (result != RT_EOK)
    {
        rw_bad_packets++;
    }
    return result;
}

static rt_err_t rw_internet_run(void)
{
    rt_uint8_t join[92] = {0}; /* rt_wlan_info (60) + firmware password field (32) */
    rt_uint8_t mac[6];
    rt_tick_t start;
    rt_err_t result;
    rw_target_found = RT_FALSE;
    result = rw_wifi_test();
    if (result != RT_EOK)
    {
        goto clear_key;
    }
    if (!rw_target_found)
    {
        rt_kprintf("NET: requested SSID was not found\n");
        result = -RT_ERROR;
        goto clear_key;
    }
    result = rw_command(2, RT_NULL, 0);
    if (result != RT_EOK || rw_value_size < 6)
    {
        result = -RT_ERROR;
        goto clear_key;
    }
    rt_memcpy(mac, rw_value, 6);
    rt_memcpy(join, rw_target_info, sizeof(rw_target_info));
    rt_memcpy(join + 60, rw_net_key, sizeof(rw_net_key));
    rt_memset(rw_net_key, 0, sizeof(rw_net_key));
    rw_sta_done = RT_FALSE;
    rt_kprintf("NET: joining SSID=%s\n", rw_net_ssid);
    result = rw_command(7, join, sizeof(join));
    rt_memset(join, 0, sizeof(join));
    rt_memset(rw_tx, 0, sizeof(rw_tx));
    if (result != RT_EOK)
    {
        goto clear_key;
    }
    start = rt_tick_get();
    while (!rw_sta_done)
    {
        if ((rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(20000))
        {
            result = -RT_ETIMEOUT;
            rt_kprintf("NET: association timed out\n");
            goto clear_key;
        }
        result = rw_receive();
        if (result != RT_EOK)
        {
            goto clear_key;
        }
        rt_thread_mdelay(5);
    }
    if (rw_sta_result)
    {
        result = rw_sta_result;
        goto clear_key;
    }
    rt_kprintf("NET: hotspot associated\n");
    result = rw007_internet_test(mac, rw_net_exchange);
clear_key:
    rt_memset(join, 0, sizeof(join));
    rt_memset(rw_net_key, 0, sizeof(rw_net_key));
    /* End the one-shot network session, including after a failed test. */
    rw_shutdown();
    return result;
}

static void rw_run_test(unsigned int test)
{
    static const char *names[] = {
        "", "info", "wifi scan", "ble scan", "advertise command", "all", "internet"};
    rt_err_t result;
    rt_kprintf("\nRW007: testing %s (module reset)\n", names[test]);
    result = rw_prepare();
    if (result == RT_EOK)
    {
        switch (test)
        {
        case RW_INFO:
            result = rw_info_test();
            break;
        case RW_WIFI:
            result = rw_wifi_test();
            break;
        case RW_BLE:
            result = rw_ble_test();
            break;
        case RW_ADV:
            result = rw_advertise_test();
            break;
        case RW_INTERNET:
            result = rw_internet_run();
            break;
        }
    }
    if (result == RT_EOK)
    {
        rw_pass |= 1u << test;
        rw_fail &= ~(1u << test);
    }
    else
    {
        rw_fail |= 1u << test;
        rw_pass &= ~(1u << test);
        rw_shutdown();
    }
    if (test == RW_INTERNET)
    {
        rt_memset(rw_net_key, 0, sizeof(rw_net_key));
        rw_net_pending = RT_FALSE;
    }
    rt_kprintf(
        "RESULT: %s %s result=%d\n", names[test], result == RT_EOK ? "PASS" : "FAIL", result);
    if ((test == RW_WIFI && !rw_wifi_reports) || (test == RW_BLE && !rw_ble_reports))
    {
        rt_kprintf("RW007: no reports observed; completion alone does not prove reception\n");
    }
}

static void rw_worker(void *parameter)
{
    rt_ubase_t job = RW_ALL;
    RT_UNUSED(parameter);
    for (;;)
    {
        if (job)
        {
            unsigned int first = job == RW_ALL ? RW_INFO : job;
            unsigned int last = job == RW_ALL ? RW_ADV : job;
            unsigned int test;
            rw_busy = RT_TRUE;
            rw_run++;
            for (test = first; test <= last; test++)
            {
                rw_run_test(test);
            }
            rw_busy = RT_FALSE;
            rt_kprintf("RW007: run=%u finished; pass_mask=0x%02X fail_mask=0x%02X\n",
                       rw_run,
                       rw_pass,
                       rw_fail);
            rt_kprintf("RW007: use rw007_demo status | all | info | wifi | ble | adv\n");
            job = 0;
        }
        if (rt_mb_recv(&rw_jobs, &job, rt_tick_from_millisecond(20)) == RT_EOK)
        {
            continue;
        }
        job = 0;
        if (rw_ready)
        {
            rt_err_t result = rw_receive();
            if (result != RT_EOK)
            {
                rt_kprintf("RW007: receive stopped result=%d\n", result);
                rw_shutdown();
            }
            if (rw_advertising &&
                (rt_tick_t)(rt_tick_get() - rw_adv_started) > rt_tick_from_millisecond(65000))
            {
                rw_advertising = RT_FALSE;
                rt_kprintf("ADV: confirmation window expired; use rw007_demo adv to repeat\n");
            }
        }
    }
}

static int rwoo7_demo_start(void)
{
    rt_err_t result;
    rt_thread_t thread;
    result = rt_mb_init(&rw_jobs,
                        "rwjobs",
                        rw_job_pool,
                        sizeof(rw_job_pool) / sizeof(rw_job_pool[0]),
                        RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        return result;
    }
    thread = rt_thread_create("rw007", rw_worker, RT_NULL, 8192, 20, 10);
    if (thread == RT_NULL)
    {
        rt_mb_detach(&rw_jobs);
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(thread);
    if (result != RT_EOK)
    {
        rt_thread_delete(thread);
        rt_mb_detach(&rw_jobs);
        return result;
    }
    rw_worker_ready = RT_TRUE;
    return RT_EOK;
}
// INIT_APP_EXPORT(rwoo7_demo_start);

static void rw007_demo(int argc, char **argv)
{
    rt_ubase_t job = 0;
    if (argc == 2 && !rt_strcmp(argv[1], "status"))
    {
        rt_kprintf("RW007: ready=%d busy=%d run=%u adv_command_active=%d connected=%d\n",
                   rw_ready,
                   rw_busy,
                   rw_run,
                   rw_advertising,
                   rw_connected);
        rt_kprintf("RW007: pass_mask=0x%02X fail_mask=0x%02X (info=02 wifi=04 ble=08 adv_cmd=10 "
                   "internet=40)\n",
                   rw_pass,
                   rw_fail);
        rt_kprintf("RW007: wifi_reports=%u ble_reports=%u transfers=%u errors=%u malformed=%u\n",
                   rw_wifi_reports,
                   rw_ble_reports,
                   rw_transfers,
                   rw_errors,
                   rw_bad_packets);
        return;
    }
    if (argc == 4 && !rt_strcmp(argv[1], "internet"))
    {
        rt_size_t ssid_size = rt_strlen(argv[2]), key_size = rt_strlen(argv[3]);
        if (!ssid_size || ssid_size > 32 || key_size < 8 || key_size > 31)
        {
            rt_kprintf(
                "NET: SSID must be 1..32 bytes; WPA key 8..31 bytes for this firmware ABI\n");
            return;
        }
        if (!rw_worker_ready || rw_busy || rw_net_pending)
        {
            rt_kprintf("RW007: not ready or busy\n");
            return;
        }
        rw_net_pending = RT_TRUE;
        rt_memset(rw_net_ssid, 0, sizeof(rw_net_ssid));
        rt_memset(rw_net_key, 0, sizeof(rw_net_key));
        rt_memcpy(rw_net_ssid, argv[2], ssid_size);
        rt_memcpy(rw_net_key, argv[3], key_size);
        if (rt_mb_send(&rw_jobs, RW_INTERNET) != RT_EOK)
        {
            rt_memset(rw_net_key, 0, sizeof(rw_net_key));
            rw_net_pending = RT_FALSE;
            rt_kprintf("RW007: queue full\n");
        }
        else
        {
            rt_kprintf("NET: internet test queued (credentials held only in RAM)\n");
        }
        return;
    }
    if (argc == 2)
    {
        if (!rt_strcmp(argv[1], "info"))
        {
            job = RW_INFO;
        }
        else if (!rt_strcmp(argv[1], "wifi"))
        {
            job = RW_WIFI;
        }
        else if (!rt_strcmp(argv[1], "ble"))
        {
            job = RW_BLE;
        }
        else if (!rt_strcmp(argv[1], "adv"))
        {
            job = RW_ADV;
        }
        else if (!rt_strcmp(argv[1], "all"))
        {
            job = RW_ALL;
        }
    }
    if (!job)
    {
        rt_kprintf("Usage: rw007_demo status | all | info | wifi | ble | adv\n");
        rt_kprintf("       rw007_demo internet <ssid> <password>\n");
        rt_kprintf(
            "Tests reset the module and disconnect any phone. Scans print up to 24 reports.\n");
        return;
    }
    if (!rw_worker_ready)
    {
        rt_kprintf("RW007: demo not started\n");
        return;
    }
    if (rw_busy)
    {
        rt_kprintf("RW007: busy; retry after run completes\n");
        return;
    }
    if (rt_mb_send(&rw_jobs, job) != RT_EOK)
    {
        rt_kprintf("RW007: queue full\n");
    }
    else
    {
        rt_kprintf("RW007: test queued\n");
    }
}
MSH_CMD_EXPORT(rw007_demo, Test RW007 firmware info WiFi scan BLE scan and advertising);

/**
 * @brief 统一外设套件对 RW007 的同步适配入口。
 *
 * 只允许 ptest 线程调用；原 rw_worker 启用时拒绝执行，防止两条线程
 * 争用 SCI3/SPI 和模块复位。每个阶段都 prepare -> test -> shutdown。
 */
int test_rw007(const char *stage)
{
    const unsigned advertise_window_ms = 15000;
    rt_err_t result;

    if (rw_worker_ready || rw_busy)
    {
        return -RT_EBUSY;
    }
    if (strcmp(stage, "info") != 0 && strcmp(stage, "wifi") != 0 && strcmp(stage, "ble") != 0 &&
        strcmp(stage, "adv") != 0)
    {
        return -RT_EINVAL;
    }

    rw_busy = RT_TRUE;
    result = rw_prepare();
    if (result == RT_EOK)
    {
        if (strcmp(stage, "info") == 0)
        {
            result = rw_info_test();
        }
        else if (strcmp(stage, "wifi") == 0)
        {
            result = rw_wifi_test();
        }
        else if (strcmp(stage, "ble") == 0)
        {
            result = rw_ble_test();
        }
        else
        {
            result = rw_advertise_test();
            if (result == RT_EOK)
            {
                /* 广播命令成功只表示固件接受，继续处理事件供手机观察。 */
                rt_tick_t start = rt_tick_get();
                while ((rt_tick_t)(rt_tick_get() - start) <
                       rt_tick_from_millisecond(advertise_window_ms))
                {
                    result = rw_receive();
                    if (result != RT_EOK)
                    {
                        break;
                    }
                    rt_thread_mdelay(10);
                }
                if (result == RT_EOK)
                {
                    result = TEST_WAIT; /* 手机发现仍需人工确认。 */
                }
            }
        }
    }
    rw_shutdown();
    rw_busy = RT_FALSE;
    return result;
}
