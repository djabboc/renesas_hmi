/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HMI_PERIPHERAL_TEST_H
#define HMI_PERIPHERAL_TEST_H
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>
#define TEST_WAIT 1
#define TEST_SKIP 2
int test_cancelled(void);
int test_elapsed(rt_tick_t start, unsigned ms);
int test_eth(const char *stage);
int test_can(const char *stage);
int test_sd(const char *stage);
int test_audio(const char *stage);
int test_usb(const char *stage);
int test_gpio(const char *stage);
int test_rtc(const char *stage);
int test_adc(const char *stage);
int test_pmod(const char *stage);
int test_lcd(const char *stage);
int test_touch(const char *stage);
int test_graphics(const char *stage);
int test_rw007(const char *stage);
void test_restore_pin(bsp_io_port_pin_t pin);
#endif
