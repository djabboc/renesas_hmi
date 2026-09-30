/* SPDX-License-Identifier: Apache-2.0
 * Internet reachability check over the existing lwIP 2.1.2 raw API.
 * No extra initializer: called only by rwoo7-demo.c's worker.
 */
#include "rw007-internet.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include <string.h>

#define NET_QUEUE 8
#define NET_FRAME_MAX 1514
static struct netif internet_if;
static rt_bool_t stack_initialized, active;
static rt_uint8_t station_mac[6];
static struct { rt_uint16_t size; rt_uint8_t data[NET_FRAME_MAX]; } tx_queue[NET_QUEUE];
static unsigned int tx_read, tx_write, tx_count;
static rt_uint32_t rx_frames, tx_frames, drops, random_state;
static struct tcp_pcb *http_pcb;
static rt_bool_t dns_done, http_done, http_ok;
static ip_addr_t host_ip;
static rt_ubase_t dns_generation;
static char response[2048];
static rt_uint16_t response_size;
static int http_status;

u32_t sys_now(void)
{
    return (u32_t)((rt_uint64_t)rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
}

unsigned int rw007_net_random(void)
{
    if (!random_state) random_state = 0x7865a21u ^ rt_tick_get();
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

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

static err_t internet_init(struct netif *netif)
{
    netif->name[0] = 'r'; netif->name[1] = 'w';
    netif->hostname = "hmi-rw007";
    netif->hwaddr_len = 6;
    rt_memcpy(netif->hwaddr, station_mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = internet_output;
    return ERR_OK;
}

void rw007_net_input(const rt_uint8_t *frame, rt_uint16_t size)
{
    struct pbuf *p;
    if (!active || size < 14 || size > NET_FRAME_MAX) return;
    p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (!p) { drops++; return; }
    pbuf_take(p, frame, size);
    rx_frames++;
    if (internet_if.input(p, &internet_if) != ERR_OK) { pbuf_free(p); drops++; }
}

static rt_err_t internet_pump(rw007_net_exchange_fn exchange)
{
    rt_err_t result;
    sys_check_timeouts();
    if (tx_count)
    {
        unsigned int slot = tx_read;
        tx_read = (tx_read + 1) % NET_QUEUE;
        tx_count--;
        result = exchange(tx_queue[slot].data, tx_queue[slot].size);
        if (result == RT_EOK) tx_frames++;
    }
    else result = exchange(RT_NULL, 0);
    rt_thread_mdelay(1);
    return result;
}

static void internet_dns(const char *name, const ip_addr_t *address, void *arg)
{
    RT_UNUSED(name);
    if ((rt_ubase_t)arg != dns_generation || !active) return;
    if (address) host_ip = *address;
    else ip_addr_set_zero(&host_ip);
    dns_done = RT_TRUE;
}

static void internet_tcp_error(void *arg, err_t error)
{
    RT_UNUSED(arg);
    http_pcb = RT_NULL;
    http_done = RT_TRUE;
    rt_kprintf("NET: TCP error=%d\n", error);
}

static err_t internet_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t error)
{
    static const char expected[] = "Microsoft Connect Test";
    const char *body;
    RT_UNUSED(arg);
    if (!p) { http_done = RT_TRUE; return ERR_OK; }
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
        if (response_size >= 12 && (!strncmp(response, "HTTP/1.1 ", 9) || !strncmp(response, "HTTP/1.0 ", 9)))
            http_status = (response[9] - '0') * 100 + (response[10] - '0') * 10 + response[11] - '0';
        body += 4;
        if (http_status != 200) http_done = RT_TRUE;
        else if ((response + response_size - body) >= (int)sizeof(expected) - 1)
        {
            http_ok = !memcmp(body, expected, sizeof(expected) - 1);
            http_done = RT_TRUE;
        }
    }
    return ERR_OK;
}

static err_t internet_connected(void *arg, struct tcp_pcb *pcb, err_t error)
{
    static const char request[] = "GET /connecttest.txt HTTP/1.1\r\n"
        "Host: www.msftconnecttest.com\r\nUser-Agent: HMI-RW007\r\nConnection: close\r\n\r\n";
    RT_UNUSED(arg);
    if (error != ERR_OK) { http_done = RT_TRUE; return error; }
    rt_kprintf("NET: TCP connected, requesting public connectivity endpoint\n");
    error = tcp_write(pcb, request, sizeof(request) - 1, TCP_WRITE_FLAG_COPY);
    if (error == ERR_OK) error = tcp_output(pcb);
    if (error != ERR_OK) http_done = RT_TRUE;
    return error;
}

rt_err_t rw007_internet_test(const rt_uint8_t mac[6], rw007_net_exchange_fn exchange)
{
    rt_err_t result = -RT_ERROR;
    rt_uint32_t start;
    err_t err;
    ip4_addr_t zero;
    ip4_addr_set_zero(&zero);
    rt_memcpy(station_mac, mac, 6);
    if (!stack_initialized)
    {
        lwip_init();
        if (!netif_add(&internet_if, &zero, &zero, &zero, RT_NULL, internet_init, ethernet_input)) return -RT_ENOMEM;
        stack_initialized = RT_TRUE;
    }
    rx_frames = tx_frames = drops = tx_count = tx_read = tx_write = 0;
    dns_done = http_done = http_ok = RT_FALSE;
    response_size = 0; http_status = 0; http_pcb = RT_NULL;
    ip_addr_set_zero(&host_ip);
    active = RT_TRUE;
    dns_generation++;
    netif_set_addr(&internet_if, &zero, &zero, &zero);
    netif_set_default(&internet_if);
    netif_set_up(&internet_if);
    netif_set_link_up(&internet_if);
    if (dhcp_start(&internet_if) != ERR_OK) goto cleanup;
    rt_kprintf("NET: waiting for DHCP\n");
    start = sys_now();
    while (!dhcp_supplied_address(&internet_if))
    {
        if (sys_now() - start > 30000) { result = -RT_ETIMEOUT; rt_kprintf("NET: DHCP timed out\n"); goto cleanup; }
        result = internet_pump(exchange);
        if (result != RT_EOK) goto cleanup;
    }
    rt_kprintf("NET: IP=%s\n", ip4addr_ntoa(netif_ip4_addr(&internet_if)));
    rt_kprintf("NET: gateway=%s\n", ip4addr_ntoa(netif_ip4_gw(&internet_if)));
    rt_kprintf("NET: DNS=%s\n", ipaddr_ntoa(dns_getserver(0)));
    err = dns_gethostbyname("www.msftconnecttest.com", &host_ip, internet_dns, (void *)dns_generation);
    if (err == ERR_OK) dns_done = RT_TRUE;
    else if (err != ERR_INPROGRESS) { result = -RT_ERROR; goto cleanup; }
    start = sys_now();
    while (!dns_done)
    {
        if (sys_now() - start > 15000) { result = -RT_ETIMEOUT; rt_kprintf("NET: DNS timed out\n"); goto cleanup; }
        result = internet_pump(exchange);
        if (result != RT_EOK) goto cleanup;
    }
    if (ip_addr_isany(&host_ip)) { result = -RT_ERROR; rt_kprintf("NET: DNS failed\n"); goto cleanup; }
    rt_kprintf("NET: www.msftconnecttest.com=%s\n", ipaddr_ntoa(&host_ip));
    http_pcb = tcp_new();
    if (!http_pcb) { result = -RT_ENOMEM; goto cleanup; }
    tcp_recv(http_pcb, internet_recv);
    tcp_err(http_pcb, internet_tcp_error);
    if (tcp_connect(http_pcb, &host_ip, 80, internet_connected) != ERR_OK) { result = -RT_ERROR; goto cleanup; }
    start = sys_now();
    while (!http_done)
    {
        if (sys_now() - start > 20000) { result = -RT_ETIMEOUT; rt_kprintf("NET: HTTP timed out\n"); goto cleanup; }
        result = internet_pump(exchange);
        if (result != RT_EOK) goto cleanup;
    }
    result = http_ok ? RT_EOK : -RT_ERROR;
    rt_kprintf("NET: HTTP status=%d received=%u expected_body=%s\n", http_status, response_size, http_ok ? "MATCH" : "NO MATCH");
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
        if (internet_pump(exchange) != RT_EOK) break;
    active = RT_FALSE;
    tx_count = 0;
    rt_kprintf("NET: tx=%u rx=%u drops=%u; INTERNET %s result=%d\n",
               tx_frames, rx_frames, drops, result == RT_EOK ? "PASS" : "FAIL", result);
    return result;
}
