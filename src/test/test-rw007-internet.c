/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-rw007-internet.c
 * @brief 独立连接 2.4 GHz 热点并验证互联网访问。
 *
 * 热点通过运行参数传入；本次扫描、关联、DHCP、DNS、HTTP 校验并断开，不依赖扫描例程。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>
#include "lwip/init.h"
#include "lwip/sys.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

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

/* SPDX-License-Identifier: Apache-2.0
 * Internet reachability check over the existing lwIP 2.1.2 raw API.
 * No extra initializer: called by the selected example's sole network owner.
 */
typedef rt_err_t (*network_exchange_fn)(const void *frame, rt_uint16_t size);
/* lwIP 平台层负责一次性初始化内存池和协议定时器。 */
extern void hmi_lwip_initialize(void);

#define NET_QUEUE 8
#define NET_FRAME_MAX 1514
static struct netif internet_if;
static rt_bool_t active;
static rt_uint8_t station_mac[6];
static struct
{
    rt_uint16_t size;
    rt_uint8_t data[NET_FRAME_MAX];
} tx_queue[NET_QUEUE];
static unsigned int tx_read;
static unsigned int tx_write;
static unsigned int tx_count;
static rt_uint32_t rx_frames;
static rt_uint32_t tx_frames;
static rt_uint32_t drops;
static struct tcp_pcb *http_pcb;
static rt_bool_t dns_done;
static rt_bool_t http_done;
static rt_bool_t http_ok;
static ip_addr_t host_ip;
static rt_ubase_t dns_generation;
static char response[2048];
static rt_uint16_t response_size;
static int http_status;

/* lwIP 发送回调只把以太帧排队，避免在接收回调中递归操作总线。 */
static err_t internet_output(struct netif *netif, struct pbuf *p)
{
    RT_UNUSED(netif);
    if (!active || p->tot_len > NET_FRAME_MAX || tx_count == NET_QUEUE)
    {
        drops++;
        return ERR_MEM;
    }
    /* Queue rather than recursively exchanging SPI from an input callback. */
    tx_queue[tx_write].size = p->tot_len;
    pbuf_copy_partial(p, tx_queue[tx_write].data, p->tot_len, 0);
    tx_write = (tx_write + 1) % NET_QUEUE;
    tx_count++;
    return ERR_OK;
}

/* 配置本例网卡的 MAC、MTU、ARP 输出函数和以太帧发送回调。 */
static err_t internet_init(struct netif *netif)
{
    netif->name[0] = 'h';
    netif->name[1] = 'm';
    netif->hostname = "hmi-board";
    netif->hwaddr_len = 6;
    rt_memcpy(netif->hwaddr, station_mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = internet_output;
    return ERR_OK;
}

/* 复制收到的以太帧到 lwIP pbuf；协议栈接管后不能再次释放同一缓冲。 */
static void network_input(const rt_uint8_t *frame, rt_uint16_t size)
{
    struct pbuf *p;
    if (!active || size < 14 || size > NET_FRAME_MAX)
    {
        return;
    }
    p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!p)
    {
        drops++;
        return;
    }
    pbuf_take(p, frame, size);
    rx_frames++;
    if (internet_if.input(p, &internet_if) != ERR_OK)
    {
        pbuf_free(p);
        drops++;
    }
}

/* 推进协议定时器，每轮最多发送一个排队帧，再轮询接收；让出 CPU。 */
static rt_err_t internet_pump(network_exchange_fn exchange)
{
    rt_err_t result;
    sys_check_timeouts();
    if (tx_count)
    {
        unsigned int slot = tx_read;
        tx_read = (tx_read + 1) % NET_QUEUE;
        tx_count--;
        result = exchange(tx_queue[slot].data, tx_queue[slot].size);
        if (result == RT_EOK)
        {
            tx_frames++;
        }
    }
    else
    {
        result = exchange(RT_NULL, 0);
    }
    rt_thread_mdelay(1);
    return result;
}

/* DNS 异步回调只接受本次会话的结果，避免迟到响应污染下一次测试。 */
static void internet_dns(const char *name, const ip_addr_t *address, void *arg)
{
    RT_UNUSED(name);
    if ((rt_ubase_t)arg != dns_generation || !active)
    {
        return;
    }
    if (address)
    {
        host_ip = *address;
    }
    else
    {
        ip_addr_set_zero(&host_ip);
    }
    dns_done = RT_TRUE;
}

/* TCP 异常时协议栈已释放连接；清空指针并通知线程结束等待。 */
static void internet_tcp_error(void *arg, err_t error)
{
    RT_UNUSED(arg);
    http_pcb = RT_NULL;
    http_done = RT_TRUE;
    rt_kprintf("NET: TCP error=%d\n", error);
}

/* 按多次 TCP 回调拼接 HTTP 响应；状态码 200 和预期正文都匹配才通过。 */
static err_t internet_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t error)
{
    static const char expected[] = "Microsoft Connect Test";
    const char *body;
    RT_UNUSED(arg);
    if (!p)
    {
        http_done = RT_TRUE;
        return ERR_OK;
    }
    if (error != ERR_OK || p->tot_len > sizeof(response) - 1 - response_size)
    {
        pbuf_free(p);
        http_done = RT_TRUE;
        return ERR_OK;
    }
    pbuf_copy_partial(p, response + response_size, p->tot_len, 0);
    response_size += p->tot_len;
    response[response_size] = 0;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    body = strstr(response, "\r\n\r\n");
    if (body)
    {
        if (response_size >= 12 &&
            (!strncmp(response, "HTTP/1.1 ", 9) || !strncmp(response, "HTTP/1.0 ", 9)))
        {
            http_status =
                (response[9] - '0') * 100 + (response[10] - '0') * 10 + response[11] - '0';
        }
        body += 4;
        if (http_status != 200)
        {
            http_done = RT_TRUE;
        }
        else if ((response + response_size - body) >= (int)sizeof(expected) - 1)
        {
            http_ok = !memcmp(body, expected, sizeof(expected) - 1);
            http_done = RT_TRUE;
        }
    }
    return ERR_OK;
}

/* TCP 握手成功后发送 HTTP 请求，数据使用 COPY 标志由协议栈保存。 */
static err_t internet_connected(void *arg, struct tcp_pcb *pcb, err_t error)
{
    static const char request[] =
        "GET /connecttest.txt HTTP/1.1\r\n"
        "Host: www.msftconnecttest.com\r\nUser-Agent: HMI-Board\r\nConnection: close\r\n\r\n";
    RT_UNUSED(arg);
    if (error != ERR_OK)
    {
        http_done = RT_TRUE;
        return error;
    }
    rt_kprintf("NET: TCP connected, requesting public connectivity endpoint\n");
    error = tcp_write(pcb, request, sizeof(request) - 1, TCP_WRITE_FLAG_COPY);
    if (error == ERR_OK)
    {
        error = tcp_output(pcb);
    }
    if (error != ERR_OK)
    {
        http_done = RT_TRUE;
    }
    return error;
}

/* 本次独立创建网卡并完成 DHCP、DNS、TCP、HTTP；退出注销网卡和 DHCP 状态。 */
static rt_err_t verify_internet(const rt_uint8_t mac[6], network_exchange_fn exchange)
{
    rt_err_t result = -RT_ERROR;
    rt_uint32_t start;
    err_t err;
    ip4_addr_t zero;
    ip4_addr_set_zero(&zero);
    rt_memcpy(station_mac, mac, 6);
    hmi_lwip_initialize();
    if (!netif_add(&internet_if, &zero, &zero, &zero, RT_NULL, internet_init, ethernet_input))
    {
        return -RT_ENOMEM;
    }
    rx_frames = 0;
    tx_frames = 0;
    drops = 0;
    tx_count = 0;
    tx_read = 0;
    tx_write = 0;
    dns_done = RT_FALSE;
    http_done = RT_FALSE;
    http_ok = RT_FALSE;
    response_size = 0;
    http_status = 0;
    http_pcb = RT_NULL;
    ip_addr_set_zero(&host_ip);
    active = RT_TRUE;
    dns_generation++;
    netif_set_addr(&internet_if, &zero, &zero, &zero);
    netif_set_default(&internet_if);
    netif_set_up(&internet_if);
    netif_set_link_up(&internet_if);
    if (dhcp_start(&internet_if) != ERR_OK)
    {
        goto cleanup;
    }
    rt_kprintf("NET: waiting for DHCP\n");
    start = sys_now();
    while (!dhcp_supplied_address(&internet_if))
    {
        if (sys_now() - start > 30000)
        {
            result = -RT_ETIMEOUT;
            rt_kprintf("NET: DHCP timed out\n");
            goto cleanup;
        }
        result = internet_pump(exchange);
        if (result != RT_EOK)
        {
            goto cleanup;
        }
    }
    rt_kprintf("NET: IP=%s\n", ip4addr_ntoa(netif_ip4_addr(&internet_if)));
    rt_kprintf("NET: gateway=%s\n", ip4addr_ntoa(netif_ip4_gw(&internet_if)));
    rt_kprintf("NET: DNS=%s\n", ipaddr_ntoa(dns_getserver(0)));
    err = dns_gethostbyname(
        "www.msftconnecttest.com", &host_ip, internet_dns, (void *)dns_generation);
    if (err == ERR_OK)
    {
        dns_done = RT_TRUE;
    }
    else if (err != ERR_INPROGRESS)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    start = sys_now();
    while (!dns_done)
    {
        if (sys_now() - start > 15000)
        {
            result = -RT_ETIMEOUT;
            rt_kprintf("NET: DNS timed out\n");
            goto cleanup;
        }
        result = internet_pump(exchange);
        if (result != RT_EOK)
        {
            goto cleanup;
        }
    }
    if (ip_addr_isany(&host_ip))
    {
        result = -RT_ERROR;
        rt_kprintf("NET: DNS failed\n");
        goto cleanup;
    }
    rt_kprintf("NET: www.msftconnecttest.com=%s\n", ipaddr_ntoa(&host_ip));
    http_pcb = tcp_new();
    if (!http_pcb)
    {
        result = -RT_ENOMEM;
        goto cleanup;
    }
    tcp_recv(http_pcb, internet_recv);
    tcp_err(http_pcb, internet_tcp_error);
    if (tcp_connect(http_pcb, &host_ip, 80, internet_connected) != ERR_OK)
    {
        result = -RT_ERROR;
        goto cleanup;
    }
    start = sys_now();
    while (!http_done)
    {
        if (sys_now() - start > 20000)
        {
            result = -RT_ETIMEOUT;
            rt_kprintf("NET: HTTP timed out\n");
            goto cleanup;
        }
        result = internet_pump(exchange);
        if (result != RT_EOK)
        {
            goto cleanup;
        }
    }
    if (http_ok)
    {
        result = RT_EOK;
    }
    else
    {
        result = -RT_ERROR;
    }
    if (http_ok)
    {
        rt_kprintf("NET: HTTP status=%d received=%u expected_body=%s\n",
                   http_status,
                   response_size,
                   "MATCH");
    }
    else
    {
        rt_kprintf("NET: HTTP status=%d received=%u expected_body=%s\n",
                   http_status,
                   response_size,
                   "NO MATCH");
    }
cleanup:
    if (http_pcb)
    {
        tcp_err(http_pcb, RT_NULL);
        tcp_recv(http_pcb, RT_NULL);
        tcp_abort(http_pcb);
        http_pcb = RT_NULL;
    }
    dhcp_release_and_stop(&internet_if);
    netif_set_link_down(&internet_if);
    netif_set_down(&internet_if);
    /* Give queued final control frames a bounded chance to leave. */
    start = sys_now();
    while (tx_count && sys_now() - start < 500)
    {
        if (internet_pump(exchange) != RT_EOK)
        {
            break;
        }
    }
    dhcp_cleanup(&internet_if);
    netif_remove(&internet_if);
    active = RT_FALSE;
    tx_count = 0;
    if (result == RT_EOK)
    {
        rt_kprintf("NET: tx=%u rx=%u drops=%u; INTERNET %s result=%d\n",
                   tx_frames,
                   rx_frames,
                   drops,
                   "PASS",
                   result);
    }
    else
    {
        rt_kprintf("NET: tx=%u rx=%u drops=%u; INTERNET %s result=%d\n",
                   tx_frames,
                   rx_frames,
                   drops,
                   "FAIL",
                   result);
    }
    return result;
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

static char rw_net_ssid[33];
static char rw_net_key[32];

static rt_uint8_t rw_target_info[60];
static rt_bool_t rw_target_found;
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
    if (type == 1)
    {
        network_input(p, length);
    }
    else if (type == 5 || type == 6)
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
            if (size >= sizeof(rw_target_info) && p[16] == rt_strlen(rw_net_ssid) &&
                !rt_memcmp(p + 17, rw_net_ssid, p[16]))
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

/* 将 lwIP 以太帧封装进 SPI；空帧表示仅轮询接收，不发送空数据包。 */
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

/* 本次扫描目标 SSID、关联热点并验证互联网；所有出口都清除密钥缓冲。 */
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
    result = verify_internet(mac, rw_net_exchange);
clear_key:
    rt_memset(join, 0, sizeof(join));
    rt_memset(rw_net_key, 0, sizeof(rw_net_key));
    /* End the one-shot network session, including after a failed test. */
    rw_shutdown();
    return result;
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    int result;
    /* 运行参数是两个只读字符串指针：SSID、密码；没有命令解析。
     * 单独使用此入口时，创建线程后将 user_data 指向同样的数组。 */
    const char *const *settings = (const char *const *)rt_thread_self()->user_data;
    if (settings == RT_NULL || settings[0] == RT_NULL || settings[1] == RT_NULL)
    {
        rt_kprintf("NET: network settings missing\n");
        return TEST_SKIP;
    }
    rt_strncpy(rw_net_ssid, settings[0], sizeof(rw_net_ssid) - 1);
    rt_strncpy(rw_net_key, settings[1], sizeof(rw_net_key) - 1);
    result = rw_prepare();
    if (result == RT_EOK)
    {
        result = rw_internet_run();
    }
    rw_shutdown();
    memset(rw_net_key, 0, sizeof(rw_net_key));
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_rw007_internet_thread(void *argument)
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
