/* SPDX-License-Identifier: Apache-2.0
 * One owner for all bounded hardware tests. Never execute hardware from msh.
 */
#include "peripheral-test.h"
#include <rthw.h>
static struct rt_semaphore wake;
static volatile int busy, cancel;
static int ready;
static char requested[20], stage[20];
struct test_entry { const char *name; int (*run)(const char *); const char *automatic; const char *help; int result; unsigned runs; char last_stage[20]; };
static struct test_entry tests[] = {
    {"eth", test_eth, "mac", "phy | mac | phyloop | link | lwip", 0, 0},
    {"sd", test_sd, "read", "info | read | file", 0, 0},
    {"can", test_can, "loop", "loop | bus", 0, 0},
    {"audio", test_audio, "mic", "mic | tone | replay", 0, 0},
    {"usb", test_usb, "probe", "probe | echo", 0, 0},
    {"gpio", test_gpio, "led", "led | keys | inputs | loop", 0, 0},
    {"rtc", test_rtc, "tick", "tick | alarm", 0, 0},
    {"adc", test_adc, "sample", "sample | low | high", 0, 0},
    {"pmod", test_pmod, NULL, "spi0 | spi1 | arduino | irq0 | irq1 | i2c", 0, 0},
    {"lcd", test_lcd, "colors", "colors | backlight", 0, 0},
    {"touch", test_touch, "info", "info | points", 0, 0},
    {"graphics", test_graphics, "g2d", "g2d | jpeg", 0, 0},
    {"rw007", test_rw007, "info", "info | wifi | ble | adv", 0, 0},
};
int test_cancelled(void) { return cancel; }
int test_elapsed(rt_tick_t start, unsigned ms)
{ return (rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(ms); }
void test_restore_pin(bsp_io_port_pin_t pin)
{
    unsigned i;
    for (i = 0; i < g_bsp_pin_cfg.number_of_pins; ++i)
        if (g_bsp_pin_cfg.p_pin_cfg_data[i].pin == pin) {
            R_IOPORT_PinCfg(&g_ioport_ctrl, pin, g_bsp_pin_cfg.p_pin_cfg_data[i].pin_cfg); return;
        }
    R_IOPORT_PinCfg(&g_ioport_ctrl, pin, IOPORT_CFG_PORT_DIRECTION_INPUT);
}
static const char *result_name(int result)
{ return result == 0 ? "PASS" : result == TEST_WAIT ? "WAIT" : result == TEST_SKIP ? "SKIP" : "FAIL"; }
static void run_one(struct test_entry *test, const char *which)
{
    rt_tick_t start = rt_tick_get();
    rt_kprintf("TEST BEGIN %s %s\n", test->name, which);
    test->result = test->run(which);
    if (cancel) test->result = -RT_EINTR;
    test->runs++;
    rt_strncpy(test->last_stage,which,sizeof(test->last_stage)-1);
    rt_kprintf("TEST RESULT %s %s %s code=%d elapsed=%u ms\n", test->name, which,
               result_name(test->result), test->result,
               (unsigned)((rt_tick_get() - start) * 1000 / RT_TICK_PER_SECOND));
}
static void worker(void *arg)
{
    unsigned i;
    RT_UNUSED(arg);
    for (;;) {
        rt_sem_take(&wake, RT_WAITING_FOREVER);
        if (!strcmp(requested, "all")) {
            for (i = 0; i < sizeof(tests)/sizeof(tests[0]) && !cancel; ++i)
                if (tests[i].automatic) run_one(&tests[i], tests[i].automatic);
        } else {
            for (i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i)
                if (!strcmp(requested, tests[i].name)) { run_one(&tests[i], stage); break; }
        }
        busy = 0;
        rt_kprintf("TEST IDLE\n");
    }
}
static int peripheral_test_start(void)
{
    rt_thread_t thread;
    if (rt_sem_init(&wake, "ptest", 0, RT_IPC_FLAG_FIFO) != RT_EOK) return -RT_ERROR;
    thread = rt_thread_create("ptest", worker, NULL, 12288, 21, 10);
    if (!thread) { rt_sem_detach(&wake); return -RT_ENOMEM; }
    if (rt_thread_startup(thread) != RT_EOK) { rt_thread_delete(thread); rt_sem_detach(&wake); return -RT_ERROR; }
    ready = 1;
    rt_kprintf("HMI peripheral tests ready. hmi_test help; no test starts automatically.\n");
    return 0;
}
INIT_APP_EXPORT(peripheral_test_start);

static void hmi_test(int argc, char **argv)
{
    unsigned i;
    rt_base_t level;
    if (argc == 2 && !strcmp(argv[1], "stop")) { cancel = 1; return; }
    if (argc == 2 && !strcmp(argv[1], "status")) {
        rt_kprintf("TEST STATUS ready=%d busy=%d cancel=%d\n", ready, busy, cancel);
        for (i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i)
            rt_kprintf("%s runs=%u stage=%s last=%s\n", tests[i].name, tests[i].runs, tests[i].last_stage,
                       tests[i].runs ? result_name(tests[i].result) : "NOT_RUN");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "all")) goto enqueue;
    if (argc == 3) {
        for (i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i)
            if (!strcmp(argv[1], tests[i].name)) goto enqueue;
    }
    rt_kprintf("hmi_test all | status | stop; or hmi_test <device> <stage>\n");
    for (i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i)
        rt_kprintf("  %s: %s\n", tests[i].name, tests[i].help);
    return;
enqueue:
    if (!ready) { rt_kprintf("TEST: select peripheral profile and rebuild first\n"); return; }
    if (strlen(argv[1]) >= sizeof(requested) || (argc == 3 && strlen(argv[2]) >= sizeof(stage))) return;
    level = rt_hw_interrupt_disable();
    if (busy) { rt_hw_interrupt_enable(level); rt_kprintf("TEST BUSY\n"); return; }
    busy = 1; cancel = 0;
    rt_hw_interrupt_enable(level);
    strcpy(requested, argv[1]); strcpy(stage, argc == 3 ? argv[2] : "");
    rt_kprintf("TEST QUEUED %s %s\n", requested, stage);
    rt_sem_release(&wake);
}
MSH_CMD_EXPORT(hmi_test, Bounded HMI board peripheral tests);
