/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-main.c
 * @brief 唯一测试命令入口：检查命令、创建一次性线程、阻止测试重叠。
 *
 * 测试文件不认识命令行；这里只调用它们的标准 RT-Thread 线程入口。
 * 每次传入独立事件对象，bit0 请求停止；线程 error 字段返回结果。
 * 正常返回后由内核 idle 线程回收，cleanup 才释放测试占用状态。
 */
#include <rtthread.h>
#include <rthw.h>
#include <string.h>

#if IDLE_THREAD_STACK_SIZE < 2048
#error "Test cleanup requires IDLE_THREAD_STACK_SIZE >= 2048 in rtconfig.h"
#endif

#define TEST_STACK_BYTES 12288
#define TEST_PRIORITY 21
#define TEST_TIME_SLICE 10

extern void test_adc_high_thread(void *argument);
extern void test_adc_low_thread(void *argument);
extern void test_adc_sample_thread(void *argument);
extern void test_audio_mic_thread(void *argument);
extern void test_audio_raw_thread(void *argument);
extern void test_audio_replay_thread(void *argument);
extern void test_audio_song_thread(void *argument);
extern void test_audio_tone_thread(void *argument);
extern void test_can_bus_thread(void *argument);
extern void test_can_loop_thread(void *argument);
extern void test_eth_link_thread(void *argument);
extern void test_eth_lwip_thread(void *argument);
extern void test_eth_mac_thread(void *argument);
extern void test_eth_phy_thread(void *argument);
extern void test_eth_phyloop_thread(void *argument);
extern void test_gpio_inputs_thread(void *argument);
extern void test_gpio_irq_both_thread(void *argument);
extern void test_gpio_irq_rising_thread(void *argument);
extern void test_gpio_keys_thread(void *argument);
extern void test_gpio_led_thread(void *argument);
extern void test_gpio_loop_thread(void *argument);
extern void test_graphics_g2d_thread(void *argument);
extern void test_graphics_jpeg_thread(void *argument);
extern void test_lcd_backlight_thread(void *argument);
extern void test_lcd_colors_thread(void *argument);
extern void test_lcd_lvgl_thread(void *argument);
extern void test_lcd_touch_thread(void *argument);
extern void test_lcd_touch_lvgl_thread(void *argument);
extern void test_pmod_arduino_thread(void *argument);
extern void test_pmod_i2c_thread(void *argument);
extern void test_pmod_spi0_thread(void *argument);
extern void test_pmod_spi1_thread(void *argument);
extern void test_rtc_alarm_thread(void *argument);
extern void test_rtc_tick_thread(void *argument);
extern void test_rw007_adv_thread(void *argument);
extern void test_rw007_ble_thread(void *argument);
extern void test_rw007_info_thread(void *argument);
extern void test_rw007_internet_thread(void *argument);
extern void test_rw007_wifi_thread(void *argument);
extern void test_sd_file_thread(void *argument);
extern void test_sd_info_thread(void *argument);
extern void test_sd_read_thread(void *argument);
extern void test_touch_irq_thread(void *argument);
extern void test_touch_info_thread(void *argument);
extern void test_touch_points_thread(void *argument);
extern void test_usb_echo_thread(void *argument);
extern void test_usb_probe_thread(void *argument);

struct test_case
{
    const char *name;
    void (*entry)(void *argument);
    unsigned runs;
    int last_result;
};

/* 文件名、命令名、线程入口一一对应；不在表中调用其他测试。 */
static struct test_case test_cases[] = {
    {"adc-high", test_adc_high_thread, 0, 0},
    {"adc-low", test_adc_low_thread, 0, 0},
    {"adc-sample", test_adc_sample_thread, 0, 0},
    {"audio-mic", test_audio_mic_thread, 0, 0},
    {"audio-raw", test_audio_raw_thread, 0, 0},
    {"audio-replay", test_audio_replay_thread, 0, 0},
    {"audio-song", test_audio_song_thread, 0, 0},
    {"audio-tone", test_audio_tone_thread, 0, 0},
    {"can-bus", test_can_bus_thread, 0, 0},
    {"can-loop", test_can_loop_thread, 0, 0},
    {"eth-link", test_eth_link_thread, 0, 0},
    {"eth-lwip", test_eth_lwip_thread, 0, 0},
    {"eth-mac", test_eth_mac_thread, 0, 0},
    {"eth-phy", test_eth_phy_thread, 0, 0},
    {"eth-phyloop", test_eth_phyloop_thread, 0, 0},
    {"gpio-inputs", test_gpio_inputs_thread, 0, 0},
    {"gpio-irq-both", test_gpio_irq_both_thread, 0, 0},
    {"gpio-irq-rising", test_gpio_irq_rising_thread, 0, 0},
    {"gpio-keys", test_gpio_keys_thread, 0, 0},
    {"gpio-led", test_gpio_led_thread, 0, 0},
    {"gpio-loop", test_gpio_loop_thread, 0, 0},
    {"graphics-g2d", test_graphics_g2d_thread, 0, 0},
    {"graphics-jpeg", test_graphics_jpeg_thread, 0, 0},
    {"lcd-backlight", test_lcd_backlight_thread, 0, 0},
    {"lcd-colors", test_lcd_colors_thread, 0, 0},
    {"lcd-lvgl", test_lcd_lvgl_thread, 0, 0},
    {"lcd-touch", test_lcd_touch_thread, 0, 0},
    {"lcd-touch-lvgl", test_lcd_touch_lvgl_thread, 0, 0},
    {"pmod-arduino", test_pmod_arduino_thread, 0, 0},
    {"pmod-i2c", test_pmod_i2c_thread, 0, 0},
    {"pmod-spi0", test_pmod_spi0_thread, 0, 0},
    {"pmod-spi1", test_pmod_spi1_thread, 0, 0},
    {"rtc-alarm", test_rtc_alarm_thread, 0, 0},
    {"rtc-tick", test_rtc_tick_thread, 0, 0},
    {"rw007-adv", test_rw007_adv_thread, 0, 0},
    {"rw007-ble", test_rw007_ble_thread, 0, 0},
    {"rw007-info", test_rw007_info_thread, 0, 0},
    {"rw007-internet", test_rw007_internet_thread, 0, 0},
    {"rw007-wifi", test_rw007_wifi_thread, 0, 0},
    {"sd-file", test_sd_file_thread, 0, 0},
    {"sd-info", test_sd_info_thread, 0, 0},
    {"sd-read", test_sd_read_thread, 0, 0},
    {"touch-irq", test_touch_irq_thread, 0, 0},
    {"touch-info", test_touch_info_thread, 0, 0},
    {"touch-points", test_touch_points_thread, 0, 0},
    {"usb-echo", test_usb_echo_thread, 0, 0},
    {"usb-probe", test_usb_probe_thread, 0, 0},
};
static rt_thread_t active_thread;
static rt_event_t stop_event;
static struct test_case *active_case;
static rt_tick_t started_at;
static int busy;
/* 运行参数由入口持有到线程回收，不保留在 Flash，也不写入测试文件。
 * user_data 使用两个字符串指针：SSID、密码；仅网络例程读取。 */
static char network_ssid[33];
static char network_password[32];
static const char *network_settings[2] = {network_ssid, network_password};

static const char *result_name(int result)
{
    switch (result)
    {
    case 0:
        return "PASS";
    case 1:
        return "WAIT";
    case 2:
        return "SKIP";
    default:
        return "FAIL";
    }
}

/* 此回调由内核在线程已经退出后执行；不能提前在测试函数末尾清 busy。
 * 否则测试函数尚未返回时，就可能创建下一条操作同一硬件的线程。 */
static void test_thread_cleanup(struct rt_thread *thread)
{
    int result = thread->error;
    active_case->last_result = result;
    active_case->runs++;
    rt_kprintf("TEST RESULT %s %s code=%d elapsed=%u ms\n",
               active_case->name,
               result_name(result),
               result,
               (unsigned)((rt_tick_get() - started_at) * 1000 / RT_TICK_PER_SECOND));
    /* 先撤销可见指针，防止停止命令发送到即将释放的事件。
     * 内存释放和串口输出可能获取互斥锁，必须在调度器可运行时执行。 */
    rt_base_t level = rt_hw_interrupt_disable();
    rt_event_t completed_event = stop_event;
    stop_event = RT_NULL;
    rt_hw_interrupt_enable(level);
    rt_event_delete(completed_event);
    memset(network_ssid, 0, sizeof(network_ssid));
    memset(network_password, 0, sizeof(network_password));
    level = rt_hw_interrupt_disable();
    active_thread = RT_NULL;
    active_case = RT_NULL;
    busy = 0;
    rt_hw_interrupt_enable(level);
    rt_kprintf("TEST IDLE\n");
}

static void start_test(struct test_case *test, int argc, char **argv)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (busy)
    {
        rt_hw_interrupt_enable(level);
        rt_kprintf("TEST BUSY\n");
        return;
    }
    busy = 1;
    rt_hw_interrupt_enable(level);

    stop_event = rt_event_create("tstop", RT_IPC_FLAG_FIFO);
    if (stop_event == RT_NULL)
    {
        busy = 0;
        rt_kprintf("TEST ERROR cannot allocate stop event\n");
        return;
    }
    /* 每条有效测试命令都新建线程；没有后台常驻的测试工作线程。 */
    active_thread = rt_thread_create(
        "hmitest", test->entry, stop_event, TEST_STACK_BYTES, TEST_PRIORITY, TEST_TIME_SLICE);
    if (active_thread == RT_NULL)
    {
        rt_event_delete(stop_event);
        stop_event = RT_NULL;
        busy = 0;
        rt_kprintf("TEST ERROR cannot allocate test thread\n");
        return;
    }
    if (argc == 4)
    {
        rt_strncpy(network_ssid, argv[2], sizeof(network_ssid) - 1);
        rt_strncpy(network_password, argv[3], sizeof(network_password) - 1);
        active_thread->user_data = (rt_ubase_t)network_settings;
    }
    active_case = test;
    started_at = rt_tick_get();
    active_thread->error = -RT_ERROR;
    active_thread->cleanup = test_thread_cleanup;
    rt_kprintf("TEST BEGIN %s\n", test->name);
    if (rt_thread_startup(active_thread) != RT_EOK)
    {
        /* 尚未执行硬件测试，直接回收未启动线程及事件。 */
        active_thread->cleanup = RT_NULL;
        rt_thread_delete(active_thread);
        rt_event_delete(stop_event);
        active_thread = RT_NULL;
        active_case = RT_NULL;
        stop_event = RT_NULL;
        busy = 0;
        memset(network_password, 0, sizeof(network_password));
        rt_kprintf("TEST ERROR cannot start test thread\n");
    }
}

static void print_help(void)
{
    rt_kprintf("hmi_test <test-name> | help | status | stop\n");
    rt_kprintf("hmi_test rw007-internet <ssid> <password> (SSID 1..32, password 8..31 bytes)\n");
    for (unsigned index = 0; index < sizeof(test_cases) / sizeof(test_cases[0]); ++index)
    {
        rt_kprintf("  %s\n", test_cases[index].name);
    }
}

static void print_status(void)
{
    rt_kprintf("TEST STATUS ready=1 busy=%d\n", busy);
    if (busy && active_case != RT_NULL)
    {
        rt_kprintf("TEST ACTIVE %s\n", active_case->name);
    }
    for (unsigned index = 0; index < sizeof(test_cases) / sizeof(test_cases[0]); ++index)
    {
        if (test_cases[index].runs != 0)
        {
            rt_kprintf("%s runs=%u last=%s\n",
                       test_cases[index].name,
                       test_cases[index].runs,
                       result_name(test_cases[index].last_result));
        }
    }
}

static void hmi_test(int argc, char **argv)
{
    if (argc != 2 && !(argc == 4 && strcmp(argv[1], "rw007-internet") == 0))
    {
        print_help();
        return;
    }
    if (strcmp(argv[1], "help") == 0)
    {
        print_help();
        return;
    }
    if (strcmp(argv[1], "status") == 0)
    {
        print_status();
        return;
    }
    if (strcmp(argv[1], "stop") == 0)
    {
        /* 临界区防止 idle 回收事件后继续发送；不强行删除正在访问 DMA 的线程。 */
        rt_enter_critical();
        if (busy && stop_event != RT_NULL)
        {
            rt_event_send(stop_event, 1);
        }
        rt_exit_critical();
        rt_kprintf("TEST STOP requested; waiting for hardware cleanup\n");
        return;
    }
    for (unsigned index = 0; index < sizeof(test_cases) / sizeof(test_cases[0]); ++index)
    {
        if (strcmp(argv[1], test_cases[index].name) == 0)
        {
            if (strcmp(argv[1], "rw007-internet") == 0)
            {
                if (argc != 4 || strlen(argv[2]) < 1 || strlen(argv[2]) > 32 ||
                    strlen(argv[3]) < 8 || strlen(argv[3]) > 31)
                {
                    rt_kprintf("TEST ERROR network settings: SSID 1..32 and password 8..31 bytes "
                               "required\n");
                    return;
                }
            }
            start_test(&test_cases[index], argc, argv);
            return;
        }
    }
    /* 无效命令不会分配线程，也不会触碰硬件。 */
    rt_kprintf("TEST UNKNOWN %s; use hmi_test help\n", argv[1]);
}
MSH_CMD_EXPORT(hmi_test, Run one independent HMI peripheral example);
