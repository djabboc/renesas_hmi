/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-rw007-wifi.c
 * @brief 验证 RW007 的 2.4 GHz Wi-Fi 扫描。
 *
 * 查询附近 SSID、信道和 RSSI；仅扫描，不连接；没有收到报告时等待人工复查。
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

#define RW_CS BSP_IO_PORT_03_PIN_08
#define RW_MISO BSP_IO_PORT_03_PIN_09
#define RW_MOSI BSP_IO_PORT_03_PIN_10
#define RW_CLK BSP_IO_PORT_03_PIN_11
#define RW_RST BSP_IO_PORT_03_PIN_12
#define RW_INT BSP_IO_PORT_00_PIN_15
#define RW_PACKET_MAX 1528

static spi_cfg_t rw_spi_config;
static sci_spi_extended_cfg_t rw_spi_extended;
static external_irq_cfg_t rw_irq_config;
static volatile rt_bool_t rw_spi_done;
static volatile spi_event_t rw_spi_event;
static volatile rt_uint32_t rw_irqs;
static volatile rt_uint32_t rw_transfers;
static volatile rt_uint32_t rw_errors;
static volatile rt_bool_t rw_ready;
static volatile rt_bool_t rw_advertising;

static rt_bool_t rw_spi_open;
static rt_bool_t rw_irq_open;
static rt_uint16_t rw_sequence;
static rt_uint16_t rw_rx_length;
static rt_uint8_t rw_tx[RW_PACKET_MAX] BSP_ALIGN_VARIABLE(4);
static rt_uint8_t rw_rx[RW_PACKET_MAX] BSP_ALIGN_VARIABLE(4);

static rt_uint32_t rw_rsp_command;
static rt_int32_t rw_rsp_result;
static rt_bool_t rw_rsp_seen;
static rt_uint8_t rw_value[96];
static rt_uint32_t rw_value_size;

static rt_bool_t rw_wifi_done;

static rt_int32_t rw_wifi_result;

static volatile rt_uint32_t rw_wifi_reports;

static volatile rt_uint32_t rw_bad_packets;

static volatile rt_bool_t rw_connected;

static rt_bool_t rw_sta_done;
static rt_int32_t rw_sta_result;

/* 按小端顺序解码协议中的 16 位字段，不要求输入地址对齐。 */
static rt_uint16_t rw_get16(const rt_uint8_t *p)
{
    return p[0] | ((rt_uint16_t)p[1] << 8);
}

/* 按小端顺序解码协议中的 32 位字段，不直接强转未对齐指针。 */
static rt_uint32_t rw_get32(const rt_uint8_t *p)
{
    return p[0] | ((rt_uint32_t)p[1] << 8) | ((rt_uint32_t)p[2] << 16) | ((rt_uint32_t)p[3] << 24);
}

/* 把 16 位值写成协议要求的小端字节序。 */
static void rw_put16(rt_uint8_t *p, rt_uint16_t value)
{
    p[0] = value;
    p[1] = value >> 8;
}

/* 把 32 位值写成协议要求的小端字节序。 */
static void rw_put32(rt_uint8_t *p, rt_uint32_t value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

/* SPI 中断记录完成事件，实际协议解析留在线程中执行。 */
static void rw_spi_callback(spi_callback_args_t *args)
{
    rw_spi_event = args->event;
    rw_spi_done = RT_TRUE;
}

/* 统计模块 READY 中断边沿，用于两阶段 SPI 握手。 */
static void rw_irq_callback(external_irq_callback_args_t *args)
{
    RT_UNUSED(args);
    rw_irqs++;
}

/* 执行一次 SPI 全双工传输；有界等待完成事件并核对传输结果。 */
static rt_err_t rw_exchange(const void *tx, void *rx, rt_uint32_t length)
{
    if (test_cancelled())
    {
        return -RT_EINTR;
    }

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
    if (rw_spi_event == SPI_EVENT_TRANSFER_COMPLETE)
    {
        return RT_EOK;
    }
    else
    {
        return -RT_EIO;
    }
}

/* 等待模块产生新的握手边沿；超时或取消时返回错误。 */
static rt_err_t rw_wait_irq(rt_uint32_t previous)
{
    if (test_cancelled())
    {
        return -RT_EINTR;
    }

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

/* 核对固件帧头的两个 magic 和阶段编号，拒绝无效总线数据。 */
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
    rw_sequence++;
    if (rw_sequence >= 65534)
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
    if (tx_length > rw_rx_length)
    {
        wire_length = tx_length;
    }
    else
    {
        wire_length = rw_rx_length;
    }
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

/* 配置复位、CS、READY 和 SCI3 SPI，逐步记录已打开的硬件供错误路径清理。 */
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

/* 先关闭 READY 中断和 SPI，再拉低模块复位；可以从初始化失败路径调用。 */
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

/* 复制并终止字符串；替换控制字符，避免模块数据扰乱串口显示。 */
static void rw_text(char *out, const rt_uint8_t *in, unsigned int length)
{
    unsigned int i;
    for (i = 0; i < length; i++)
    {
        if ((in[i] < 32 || in[i] == 127))
        {
            out[i] = '.';
        }
        else
        {
            out[i] = in[i];
        }
    }
    out[length] = 0;
}

/* 先核对帧长，再解析本例需要的应答/事件；不把未知异步事件当成命令完成。 */
static rt_err_t rw_parse(void)
{
    rt_uint32_t length;
    rt_uint32_t type;
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
    if (type == 5 || type == 6)
    {
        rt_uint32_t command;
        rt_uint32_t size;
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
            if (size < sizeof(rw_value))
            {
                rt_memcpy(rw_value, p, size);
            }
            else
            {
                rt_memcpy(rw_value, p, sizeof(rw_value));
            }
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

            if (rw_wifi_reports <= 24)
            {
                if (p[16])
                {
                    rt_kprintf("WIFI[%u]: SSID=%s channel=%d RSSI=%d security=0x%08X\n",
                               rw_wifi_reports,
                               ssid,
                               (rt_int16_t)rw_get16(p + 12),
                               (rt_int16_t)rw_get16(p + 14),
                               rw_get32(p));
                }
                else
                {
                    rt_kprintf("WIFI[%u]: SSID=%s channel=%d RSSI=%d security=0x%08X\n",
                               rw_wifi_reports,
                               "<hidden>",
                               (rt_int16_t)rw_get16(p + 12),
                               (rt_int16_t)rw_get16(p + 14),
                               rw_get32(p));
                }
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
                if (command == 1)
                {
                    rw_sta_result = result;
                }
                else
                {
                    rw_sta_result = -RT_ERROR;
                }
            }
            rt_kprintf("WIFI: event=%u result=%d bytes=%u\n", command, result, size);
        }
    }
    else
    {
        return RT_EOK;
    }
    return RT_EOK;
}

/* 仅在模块就绪时轮询接收；解析错误单独计数，不伪造成功响应。 */
static rt_err_t rw_receive(void)
{
    if (test_cancelled())
    {
        return -RT_EINTR;
    }

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

/* 发送一个模块命令，并等待对应命令号的应答；异步报告不能代替应答。 */
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

/* 每次测试独立复位模块并发送同步命令，不沿用其他测试留下的连接状态。 */
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

/* 设置 station 模式并扫描，等待固件扫描完成事件；每次清零报告计数。 */
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

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    int result;
    result = rw_prepare();
    if (result == RT_EOK)
    {
        result = rw_wifi_test();
    }
    if (result == RT_EOK && rw_wifi_reports == 0)
    {
        result = TEST_WAIT;
    }
    rw_shutdown();
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_rw007_wifi_thread(void *argument)
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
