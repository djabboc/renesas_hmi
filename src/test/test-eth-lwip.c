/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-eth-lwip.c
 * @brief 独立验证有线 DHCP、DNS、TCP 和 HTTP 联网。
 *
 * 接可上网的路由器 LAN；只有 HTTP 200 且正文匹配才通过。
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

/* 无符号 tick 差值允许系统计时回绕；等待始终有上限。 */
static int test_elapsed(rt_tick_t start, unsigned milliseconds)
{
    return (rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(milliseconds);
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

/* FSP 3.5 在 r_ether_phy.c 导出这两个 Clause 22 MDIO 辅助函数。 */
extern uint32_t ether_phy_read(ether_phy_instance_ctrl_t *control, uint32_t register_number);
extern void
ether_phy_write(ether_phy_instance_ctrl_t *control, uint32_t register_number, uint32_t value);

#define PHY_REG_CONTROL 0u
#define PHY_REG_STATUS 1u
#define PHY_REG_ID_HIGH 2u
#define PHY_REG_ID_LOW 3u
#define ETH_RX_DESCRIPTOR_COUNT 4u
#define ETH_TX_DESCRIPTOR_COUNT 2u
#define ETH_BUFFER_COUNT (ETH_RX_DESCRIPTOR_COUNT + ETH_TX_DESCRIPTOR_COUNT)
#define ETH_LINK_TIMEOUT_MS 6000u
#define ETH_TX_TIMEOUT_MS 100u
#define ETH_RX_POLL_BUDGET 8u

static ether_cfg_t ethernet_config;
static ether_phy_api_t selected_phy_api;
static ether_phy_instance_t selected_phy;
static uint8_t mac_address[6] = {0x02, 0x48, 0x4d, 0x49, 0x00, 0x01};
static ether_instance_descriptor_t rx_descriptors[ETH_RX_DESCRIPTOR_COUNT] BSP_ALIGN_VARIABLE(16);
static ether_instance_descriptor_t tx_descriptors[ETH_TX_DESCRIPTOR_COUNT] BSP_ALIGN_VARIABLE(16);
static uint8_t dma_buffers[ETH_BUFFER_COUNT][1536] BSP_ALIGN_VARIABLE(32);
static uint8_t *dma_buffer_pointers[ETH_BUFFER_COUNT];

static uint8_t receive_frame[1536] BSP_ALIGN_VARIABLE(4);
static volatile unsigned ethernet_irq_count;

/* MAC 中断只累加计数，收帧和数据比较在测试线程执行。 */
static void ethernet_callback(ether_callback_args_t *arguments)
{
    RT_UNUSED(arguments);
    ++ethernet_irq_count;
}

/* 根据 MCU 唯一 ID 派生本地单播 MAC，避免测试板使用同一个固定地址。 */
static void derive_mac_address(void)
{
    const bsp_unique_id_t *unique_id = R_BSP_UniqueIdGet();
    uint32_t hash = 2166136261u;

    /* FNV-1a 压缩芯片 ID；首字节 02 保持本地管理、单播属性。 */
    for (unsigned index = 0; index < sizeof(unique_id->unique_id_bytes); ++index)
    {
        hash = (hash ^ unique_id->unique_id_bytes[index]) * 16777619u;
    }
    for (unsigned index = 0; index < 4; ++index)
    {
        mac_address[index + 2] = (uint8_t)(hash >> (index * 8));
    }
}

/* 复位 PHY，将本文件的回调、DMA 描述符和缓冲交给 FSP，再打开 MAC。 */
static int ethernet_open(void)
{
    fsp_err_t error;

    derive_mac_address();
    /* P400 是 PHY 低有效复位；释放后留出启动时间再访问 MDIO。 */
    R_IOPORT_PinCfg(&g_ioport_ctrl,
                    BSP_IO_PORT_04_PIN_00,
                    IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    rt_thread_mdelay(20);
    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_00, BSP_IO_LEVEL_HIGH);
    rt_thread_mdelay(100);

    selected_phy_api = g_ether_phy_on_ether_phy;

    selected_phy = g_ether_phy0;
    selected_phy.p_api = &selected_phy_api;
    ethernet_config = g_ether0_cfg;
    ethernet_config.p_ether_phy_instance = &selected_phy;
    ethernet_config.p_callback = ethernet_callback;
    ethernet_config.p_mac_address = mac_address;
    ethernet_config.p_rx_descriptors = rx_descriptors;
    ethernet_config.p_tx_descriptors = tx_descriptors;
    ethernet_config.num_rx_descriptors = ETH_RX_DESCRIPTOR_COUNT;
    ethernet_config.num_tx_descriptors = ETH_TX_DESCRIPTOR_COUNT;
    for (unsigned index = 0; index < ETH_BUFFER_COUNT; ++index)
    {
        dma_buffer_pointers[index] = dma_buffers[index];
    }
    ethernet_config.pp_ether_buffers = dma_buffer_pointers;
    ethernet_irq_count = 0;
    error = R_ETHER_Open(&g_ether0_ctrl, &ethernet_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("ETH open=%d\n", error);
        return -RT_ERROR;
    }
    rt_kprintf("ETH MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
               mac_address[0],
               mac_address[1],
               mac_address[2],
               mac_address[3],
               mac_address[4],
               mac_address[5]);
    return TEST_PASS;
}

/* 周期调用链路状态机；协商成功、超时或取消时结束等待。 */
static int wait_for_ethernet_link(unsigned timeout_ms)
{
    rt_tick_t start = rt_tick_get();
    do
    {
        R_ETHER_LinkProcess(&g_ether0_ctrl);
        if (g_ether0_ctrl.link_establish_status == ETHER_LINK_ESTABLISH_STATUS_UP)
        {
            return TEST_PASS;
        }
        rt_thread_mdelay(20);
    } while (!test_elapsed(start, timeout_ms) && !test_cancelled());
    return -RT_ETIMEOUT;
}

/* raw lwIP 的收发适配器。length=0 时只轮询接收；限制每次收帧数量，
 * 避免背景网络流量阻塞 lwIP 定时器和取消处理。
 */
static rt_err_t ethernet_exchange(const void *frame, rt_uint16_t length)
{
    rt_tick_t start = rt_tick_get();
    fsp_err_t error;

    if (test_cancelled())
    {
        return -RT_EINTR;
    }
    R_ETHER_LinkProcess(&g_ether0_ctrl);
    if (g_ether0_ctrl.link_establish_status != ETHER_LINK_ESTABLISH_STATUS_UP)
    {
        return -RT_EIO;
    }
    if (length != 0)
    {
        do
        {
            error = R_ETHER_Write(&g_ether0_ctrl, (void *)frame, length);
            if (error == FSP_SUCCESS)
            {
                break;
            }
            rt_thread_mdelay(1);
        } while (!test_elapsed(start, ETH_TX_TIMEOUT_MS) && !test_cancelled());
        if (error != FSP_SUCCESS)
        {
            return -RT_EIO;
        }
    }
    for (unsigned index = 0; index < ETH_RX_POLL_BUDGET; ++index)
    {
        uint32_t received_bytes = sizeof(receive_frame);
        error = R_ETHER_Read(&g_ether0_ctrl, receive_frame, &received_bytes);
        if (error != FSP_SUCCESS || received_bytes == 0)
        {
            break;
        }
        network_input(receive_frame, (rt_uint16_t)received_bytes);
    }
    return RT_EOK;
}

/* 通过 MDIO 读取 PHY 身份；全零或全 FF 表示管理接口没有有效应答。 */
static int verify_phy_identity(void)
{
    uint32_t id_high = ether_phy_read(&g_ether_phy0_ctrl, PHY_REG_ID_HIGH);
    uint32_t id_low = ether_phy_read(&g_ether_phy0_ctrl, PHY_REG_ID_LOW);

    rt_kprintf("ETH PHY id=%04X:%04X BMCR=%04X BMSR=%04X\n",
               id_high,
               id_low,
               ether_phy_read(&g_ether_phy0_ctrl, PHY_REG_CONTROL),
               ether_phy_read(&g_ether_phy0_ctrl, PHY_REG_STATUS));
    /* 全零/全 FF 通常表示 MDIO 未正常应答，不能作为有效身份。 */
    if ((id_high == 0 && id_low == 0) || id_high == 0xffff || id_low == 0xffff)
    {
        return -RT_ERROR;
    }
    return TEST_PASS;
}

/* 等待物理链路并报告协商结果；缺少网线时返回 SKIP。 */
static int verify_external_link(void)
{
    uint32_t speed = 0;
    uint32_t local_pause = 0;
    uint32_t remote_pause = 0;

    if (wait_for_ethernet_link(ETH_LINK_TIMEOUT_MS) != TEST_PASS)
    {
        rt_kprintf("ETH no physical link; connect router LAN cable\n");
        return TEST_SKIP;
    }
    R_ETHER_PHY_LinkPartnerAbilityGet(&g_ether_phy0_ctrl, &speed, &local_pause, &remote_pause);
    rt_kprintf("ETH physical link UP speed/duplex enum=%u ECMR=%08X\n", speed, R_ETHERC0->ECMR);

    return verify_internet(mac_address, ethernet_exchange);
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    int result;

    if (ethernet_open() != TEST_PASS)
    {
        return -RT_ERROR;
    }
    result = verify_phy_identity();
    if (result == TEST_PASS)
    {
        result = verify_external_link();
    }

    /* 任一阶段退出均关闭 MAC；回环模式还需恢复 PHY 自动协商。 */
    R_ETHERC0->ECMR_b.ILB = 0;

    R_ETHER_Close(&g_ether0_ctrl);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_eth_lwip_thread(void *argument)
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
