#ifndef HMI_RW007_INTERNET_H
#define HMI_RW007_INTERNET_H
#include <rtthread.h>
/* All raw lwIP calls run in one owner: RW007 worker OR peripheral suite worker.
 * The suite reuses this transaction with the Ethernet MAC exchange callback. */
typedef rt_err_t (*rw007_net_exchange_fn)(const void *frame, rt_uint16_t size);
rt_err_t rw007_internet_test(const rt_uint8_t mac[6], rw007_net_exchange_fn exchange);
void rw007_net_input(const rt_uint8_t *frame, rt_uint16_t size);
#endif
