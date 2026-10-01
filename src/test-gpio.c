/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-gpio.c
 * @brief 板载 LED、用户按键与 Arduino GPIO 跳线测试。
 *
 * 每项结束恢复引脚配置；LED 视觉与长按效果仍需人工验收。
 */
#include "peripheral-test.h"

#define KEY_COUNT 3u
#define KEY_SCAN_INTERVAL_MS 5u
#define KEY_DEBOUNCE_SAMPLES 4u
#define KEY_LONG_PRESS_MS 800u
#define KEY_WINDOW_MS 15000u

static const bsp_io_port_pin_t led_pins[] = {
    BSP_IO_PORT_02_PIN_09, BSP_IO_PORT_02_PIN_10, BSP_IO_PORT_02_PIN_04};
static const bsp_io_port_pin_t key_pins[] = {
    BSP_IO_PORT_00_PIN_05, BSP_IO_PORT_00_PIN_06, BSP_IO_PORT_00_PIN_07};

/* 两个红灯低有效、D13 蓝灯高有效；结束恢复生成的引脚配置。 */
static int exercise_leds(void)
{
    unsigned pin_index;

    for (pin_index = 0; pin_index < TEST_ARRAY_SIZE(led_pins); ++pin_index)
    {
        rt_kprintf("LED %u pin=P%u%02u active=%u\n",
                   pin_index,
                   led_pins[pin_index] >> 8,
                   led_pins[pin_index] & 255,
                   pin_index == 2 ? 1 : 0);
        rt_pin_mode(led_pins[pin_index], PIN_MODE_OUTPUT);
        for (unsigned toggle_index = 0; toggle_index < 4 && !test_cancelled(); ++toggle_index)
        {
            rt_pin_write(led_pins[pin_index], toggle_index & 1);
            rt_thread_mdelay(180);
        }
        rt_pin_write(led_pins[pin_index], pin_index == 2 ? 0 : 1);
        test_restore_pin(led_pins[pin_index]);
    }
    rt_kprintf("LED output sequence finished; visual confirmation required\n");
    return TEST_WAIT;
}

/* 5ms 采样、连续四次同电平才确认边沿；800ms 区分长按和短按。 */
static int observe_keys(const char *stage)
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
    if (strcmp(stage, "inputs") == 0)
    {
        for (pin_index = 0; pin_index < KEY_COUNT; ++pin_index)
        {
            test_restore_pin(key_pins[pin_index]);
        }
        return TEST_WAIT;
    }
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
            if (++stable_samples[pin_index] > KEY_DEBOUNCE_SAMPLES)
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
                    rt_kprintf("KEY %u UP %s\n",
                               pin_index,
                               test_elapsed(pressed_at[pin_index], KEY_LONG_PRESS_MS) ? "LONG"
                                                                                      : "SHORT");
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
    return press_count[0] && press_count[1] && press_count[2] && release_count[0] &&
                   release_count[1] && release_count[2]
               ? 0
               : TEST_WAIT;
}

/* Arduino D2 -> D9，建议串联 1kΩ；所有电平均需在输入端读回一致。 */
static int verify_gpio_loopback(void)
{
    unsigned pin_index;

    /* Arduino D2=P008 输出，D9=P009 输入，建议串联 1kΩ。 */
    int result = 0;
    rt_pin_mode(BSP_IO_PORT_00_PIN_09, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(BSP_IO_PORT_00_PIN_08, PIN_MODE_OUTPUT);
    for (pin_index = 0; pin_index < 16; ++pin_index)
    {
        rt_pin_write(BSP_IO_PORT_00_PIN_08, pin_index & 1);
        rt_thread_mdelay(2);
        if (rt_pin_read(BSP_IO_PORT_00_PIN_09) != (int)(pin_index & 1))
        {
            result = -RT_ERROR;
        }
    }
    test_restore_pin(BSP_IO_PORT_00_PIN_08);
    test_restore_pin(BSP_IO_PORT_00_PIN_09);
    return result;
}

int test_gpio(const char *stage)
{
    if (strcmp(stage, "led") == 0)
    {
        return exercise_leds();
    }
    if (strcmp(stage, "inputs") == 0 || strcmp(stage, "keys") == 0)
    {
        return observe_keys(stage);
    }
    if (strcmp(stage, "loop") == 0)
    {
        return verify_gpio_loopback();
    }
    return -RT_EINVAL;
}
