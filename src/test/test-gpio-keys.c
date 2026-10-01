/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-gpio-keys.c
 * @brief 观察三个按键的消抖、按下和释放。
 *
 * 15 秒内操作三个按键；5 ms 采样、连续四次一致确认，800 ms 区分长短按。
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

/* 无符号 tick 差值允许系统计时回绕；等待始终有上限。 */
static int test_elapsed(rt_tick_t start, unsigned milliseconds)
{
    return (rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(milliseconds);
}

/* 恢复 FSP 生成的复用配置；未配置的引脚退回高阻输入。 */
static void test_restore_pin(bsp_io_port_pin_t pin)
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

#define KEY_COUNT 3u
#define KEY_SCAN_INTERVAL_MS 5u
#define KEY_DEBOUNCE_SAMPLES 4u
#define KEY_LONG_PRESS_MS 800u
#define KEY_WINDOW_MS 15000u

static const bsp_io_port_pin_t key_pins[] = {
    BSP_IO_PORT_00_PIN_05, BSP_IO_PORT_00_PIN_06, BSP_IO_PORT_00_PIN_07};

/* 5ms 采样、连续四次同电平才确认边沿；800ms 区分长按和短按。 */
static int run_test(void)
{
    unsigned pin_index;

    unsigned press_count[KEY_COUNT] = {0};
    unsigned release_count[KEY_COUNT] = {0};
    int stable_level[KEY_COUNT];
    int candidate_level[KEY_COUNT];
    unsigned stable_samples[KEY_COUNT] = {0};
    rt_tick_t pressed_at[KEY_COUNT] = {0};
    for (pin_index = 0; pin_index < KEY_COUNT; ++pin_index)
    {
        rt_pin_mode(key_pins[pin_index], PIN_MODE_INPUT_PULLUP);
        stable_level[pin_index] = rt_pin_read(key_pins[pin_index]);
        candidate_level[pin_index] = stable_level[pin_index];
    }
    rt_kprintf("KEY levels P005=%d P006=%d P007=%d (pressed=0)\n",
               stable_level[0],
               stable_level[1],
               stable_level[2]);

    rt_tick_t start = rt_tick_get();
    while (!test_elapsed(start, KEY_WINDOW_MS) && !test_cancelled())
    {
        for (pin_index = 0; pin_index < KEY_COUNT; ++pin_index)
        {
            int sampled_level = rt_pin_read(key_pins[pin_index]);
            if (sampled_level != candidate_level[pin_index])
            {
                candidate_level[pin_index] = sampled_level;
                stable_samples[pin_index] = 0;
            }
            stable_samples[pin_index]++;
            if (stable_samples[pin_index] > KEY_DEBOUNCE_SAMPLES)
            {
                stable_samples[pin_index] = KEY_DEBOUNCE_SAMPLES;
            }
            if (stable_samples[pin_index] == KEY_DEBOUNCE_SAMPLES &&
                stable_level[pin_index] != sampled_level)
            {
                stable_level[pin_index] = sampled_level;
                if (!sampled_level)
                {
                    ++press_count[pin_index];
                    pressed_at[pin_index] = rt_tick_get();
                    rt_kprintf("KEY %u DOWN\n", pin_index);
                }
                else
                {
                    ++release_count[pin_index];
                    if (test_elapsed(pressed_at[pin_index], KEY_LONG_PRESS_MS))
                    {
                        rt_kprintf("KEY %u UP %s\n", pin_index, "LONG");
                    }
                    else
                    {
                        rt_kprintf("KEY %u UP %s\n", pin_index, "SHORT");
                    }
                }
            }
        }
        rt_thread_mdelay(KEY_SCAN_INTERVAL_MS);
    }
    for (pin_index = 0; pin_index < KEY_COUNT; ++pin_index)
    {
        rt_kprintf("KEY %u press=%u release=%u\n",
                   pin_index,
                   press_count[pin_index],
                   release_count[pin_index]);
        test_restore_pin(key_pins[pin_index]);
    }
    if (press_count[0] && press_count[1] && press_count[2] && release_count[0] &&
        release_count[1] && release_count[2])
    {
        return 0;
    }
    else
    {
        return TEST_WAIT;
    }
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_gpio_keys_thread(void *argument)
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
