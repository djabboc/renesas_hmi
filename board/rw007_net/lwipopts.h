/* Dedicated raw lwIP: one owner, selected RW007 worker or peripheral suite. */
#ifndef HMI_RW007_LWIPOPTS_H
#define HMI_RW007_LWIPOPTS_H
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ETHERNET 1
#define LWIP_ARP 1
#define LWIP_ICMP 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_RAW 0
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_DHCP_CHECK_LINK_UP 1
#define LWIP_DHCP_MAX_DNS_SERVERS 2
#define LWIP_NETIF_HOSTNAME 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE (32 * 1024)
#define MEMP_NUM_PBUF 16
#define PBUF_POOL_SIZE 16
#define PBUF_POOL_BUFSIZE 512
#define MEMP_NUM_TCP_PCB 4
#define MEMP_NUM_TCP_SEG 24
#define TCP_MSS 1460
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define LWIP_STATS 0
#define LWIP_DNS_SECURE 7
#endif
