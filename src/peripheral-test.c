/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file peripheral-test.c
 * @brief 全外设测试的命令调度与结果记录。
 *
 * MSH 仅投递一个请求，ptest 线程独占硬件执行测试。这样 LCD、触摸、
 * 扩展口及两种网卡不会并发争用资源。启动后不自动执行测试。
 */
#include "peripheral-test.h"
#include <rthw.h>

#define TEST_NAME_CAPACITY 20
#define TEST_WORKER_STACK_SIZE 12288
#define TEST_WORKER_PRIORITY 21
#define TEST_WORKER_TIME_SLICE 10

struct test_entry
{
    const char *name;
    int (*run)(const char *stage);
    const char *baseline_stage; /* NULL 表示需要外接跳线，不进入 all 批次。 */
    const char *help;
    int last_result;
    unsigned run_count;
    char last_stage[TEST_NAME_CAPACITY];
};

/* 未显式初始化的计数和最近阶段均为零；表顺序也是 all 的执行顺序。 */
static struct test_entry test_entries[] = {
    {.name = "eth",
     .run = test_eth,
     .baseline_stage = "mac",
     .help = "phy | mac | phyloop | link | lwip"},
    {.name = "sd", .run = test_sd, .baseline_stage = "read", .help = "info | read | file"},
    {.name = "can", .run = test_can, .baseline_stage = "loop", .help = "loop | bus"},
    {.name = "audio", .run = test_audio, .baseline_stage = "mic", .help = "mic | tone | replay"},
    {.name = "usb", .run = test_usb, .baseline_stage = "probe", .help = "probe | echo"},
    {.name = "gpio",
     .run = test_gpio,
     .baseline_stage = "led",
     .help = "led | keys | inputs | loop"},
    {.name = "rtc", .run = test_rtc, .baseline_stage = "tick", .help = "tick | alarm"},
    {.name = "adc", .run = test_adc, .baseline_stage = "sample", .help = "sample | low | high"},
    {.name = "pmod",
     .run = test_pmod,
     .baseline_stage = NULL,
     .help = "spi0 | spi1 | arduino | irq0 | irq1 | i2c"},
    {.name = "lcd", .run = test_lcd, .baseline_stage = "colors", .help = "colors | backlight"},
    {.name = "touch", .run = test_touch, .baseline_stage = "info", .help = "info | points"},
    {.name = "graphics", .run = test_graphics, .baseline_stage = "g2d", .help = "g2d | jpeg"},
    {.name = "rw007",
     .run = test_rw007,
     .baseline_stage = "info",
     .help = "info | wifi | ble | adv"},
};

static struct rt_semaphore request_ready;
static volatile int worker_busy;
static volatile int cancel_requested;
static int worker_ready;
static char requested_device[TEST_NAME_CAPACITY];
static char requested_stage[TEST_NAME_CAPACITY];

int test_cancelled(void)
{
    return cancel_requested;
}

int test_elapsed(rt_tick_t start, unsigned ms)
{
    return (rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(ms);
}

void test_restore_pin(bsp_io_port_pin_t pin)
{
    for (unsigned index = 0; index < g_bsp_pin_cfg.number_of_pins; ++index)
    {
        if (g_bsp_pin_cfg.p_pin_cfg_data[index].pin == pin)
        {
            R_IOPORT_PinCfg(&g_ioport_ctrl, pin, g_bsp_pin_cfg.p_pin_cfg_data[index].pin_cfg);
            return;
        }
    }
    R_IOPORT_PinCfg(&g_ioport_ctrl, pin, IOPORT_CFG_PORT_DIRECTION_INPUT);
}

static const char *test_result_name(int result)
{
    switch (result)
    {
    case TEST_PASS:
        return "PASS";
    case TEST_WAIT:
        return "WAIT";
    case TEST_SKIP:
        return "SKIP";
    default:
        return "FAIL";
    }
}

static struct test_entry *find_test(const char *name)
{
    for (unsigned index = 0; index < TEST_ARRAY_SIZE(test_entries); ++index)
    {
        if (strcmp(name, test_entries[index].name) == 0)
        {
            return &test_entries[index];
        }
    }
    return NULL;
}

/* 统一记账和日志格式；主机脚本依赖 TEST RESULT / TEST IDLE 标记。 */
static void run_one_test(struct test_entry *entry, const char *stage)
{
    rt_tick_t start = rt_tick_get();

    rt_kprintf("TEST BEGIN %s %s\n", entry->name, stage);
    entry->last_result = entry->run(stage);
    if (test_cancelled())
    {
        entry->last_result = -RT_EINTR;
    }
    entry->run_count++;
    rt_strncpy(entry->last_stage, stage, sizeof(entry->last_stage) - 1);
    entry->last_stage[sizeof(entry->last_stage) - 1] = '\0';
    rt_kprintf("TEST RESULT %s %s %s code=%d elapsed=%u ms\n",
               entry->name,
               stage,
               test_result_name(entry->last_result),
               entry->last_result,
               (unsigned)((rt_tick_get() - start) * 1000 / RT_TICK_PER_SECOND));
}

static void test_worker(void *argument)
{
    RT_UNUSED(argument);
    for (;;)
    {
        rt_sem_take(&request_ready, RT_WAITING_FOREVER);
        if (strcmp(requested_device, "all") == 0)
        {
            for (unsigned index = 0; index < TEST_ARRAY_SIZE(test_entries) && !test_cancelled();
                 ++index)
            {
                struct test_entry *entry = &test_entries[index];
                if (entry->baseline_stage != NULL)
                {
                    run_one_test(entry, entry->baseline_stage);
                }
            }
        }
        else
        {
            struct test_entry *entry = find_test(requested_device);
            if (entry != NULL)
            {
                run_one_test(entry, requested_stage);
            }
        }
        /* 主机看到 IDLE 后即可投递下一条，必须先解除 busy。 */
        worker_busy = 0;
        rt_kprintf("TEST IDLE\n");
    }
}

static int peripheral_test_start(void)
{
    rt_thread_t thread;

    if (rt_sem_init(&request_ready, "ptest", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return -RT_ERROR;
    }
    thread = rt_thread_create("ptest",
                              test_worker,
                              NULL,
                              TEST_WORKER_STACK_SIZE,
                              TEST_WORKER_PRIORITY,
                              TEST_WORKER_TIME_SLICE);
    if (thread == NULL)
    {
        rt_sem_detach(&request_ready);
        return -RT_ENOMEM;
    }
    if (rt_thread_startup(thread) != RT_EOK)
    {
        rt_thread_delete(thread);
        rt_sem_detach(&request_ready);
        return -RT_ERROR;
    }
    worker_ready = 1;
    rt_kprintf("HMI peripheral tests ready. hmi_test help; no test starts automatically.\n");
    return RT_EOK;
}
INIT_APP_EXPORT(peripheral_test_start);

static void print_test_status(void)
{
    rt_kprintf(
        "TEST STATUS ready=%d busy=%d cancel=%d\n", worker_ready, worker_busy, cancel_requested);
    for (unsigned index = 0; index < TEST_ARRAY_SIZE(test_entries); ++index)
    {
        const struct test_entry *entry = &test_entries[index];
        const char *result = entry->run_count ? test_result_name(entry->last_result) : "NOT_RUN";
        rt_kprintf("%s runs=%u stage=%s last=%s\n",
                   entry->name,
                   entry->run_count,
                   entry->last_stage,
                   result);
    }
}

static void print_test_help(void)
{
    rt_kprintf("hmi_test all | status | stop; or hmi_test <device> <stage>\n");
    for (unsigned index = 0; index < TEST_ARRAY_SIZE(test_entries); ++index)
    {
        rt_kprintf("  %s: %s\n", test_entries[index].name, test_entries[index].help);
    }
}

/* 单槽请求：先保留槽位、再写请求、最后发信号量；busy 期间禁止覆盖。 */
static void queue_test(const char *device, const char *stage)
{
    rt_base_t interrupt_level;

    if (!worker_ready)
    {
        rt_kprintf("TEST: select peripheral profile and rebuild first\n");
        return;
    }
    if (strlen(device) >= sizeof(requested_device) || strlen(stage) >= sizeof(requested_stage))
    {
        return;
    }
    interrupt_level = rt_hw_interrupt_disable();
    if (worker_busy)
    {
        rt_hw_interrupt_enable(interrupt_level);
        rt_kprintf("TEST BUSY\n");
        return;
    }
    worker_busy = 1;
    cancel_requested = 0;
    rt_hw_interrupt_enable(interrupt_level);

    strcpy(requested_device, device);
    strcpy(requested_stage, stage);
    rt_kprintf("TEST QUEUED %s %s\n", requested_device, requested_stage);
    rt_sem_release(&request_ready);
}

static void hmi_test(int argc, char **argv)
{
    if (argc == 2)
    {
        if (strcmp(argv[1], "stop") == 0)
        {
            cancel_requested = 1;
            return;
        }
        if (strcmp(argv[1], "status") == 0)
        {
            print_test_status();
            return;
        }
        if (strcmp(argv[1], "all") == 0)
        {
            queue_test("all", "");
            return;
        }
    }
    if (argc == 3 && find_test(argv[1]) != NULL)
    {
        queue_test(argv[1], argv[2]);
        return;
    }
    print_test_help();
}
MSH_CMD_EXPORT(hmi_test, Bounded HMI board peripheral tests);
