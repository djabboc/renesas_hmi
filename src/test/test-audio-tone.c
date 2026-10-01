/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-audio-tone.c
 * @brief 通过 GPT6 差分 PWM 播放固定提示音。
 *
 * GPT2 按采样率更新占空比；播放约半秒后关闭两个定时器，结果等待试听。
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

#define AUDIO_PLAYBACK_TIMEOUT_MS 1500u
#define AUDIO_PWM_PERIOD_COUNTS 1500u
#define AUDIO_PWM_CENTER_COUNTS 750
#define AUDIO_SAMPLE_PERIOD_COUNTS 7488u
#define AUDIO_SAMPLE_LIMIT 4000
#define AUDIO_TONE_FRAMES 8000u
/* 这些完成标志/位置由中断更新，由测试线程轮询。 */

static volatile unsigned playback_position;
static unsigned playback_frame_count;

/* GPT2 中断只更新占空比，不分配内存、不输出日志。 */
static void audio_tick(timer_callback_args_t *arguments)
{
    int pcm_sample = 0;
    RT_UNUSED(arguments);
    if (playback_position < playback_frame_count)
    {

        if ((playback_position % 32 < 16))
        {
            pcm_sample = 1200;
        }
        else
        {
            pcm_sample = -1200;
        }

        ++playback_position;
    }
    /* 两桥臂以 50% 为中心差分输出，限幅避免测试音量过大。 */
    if (pcm_sample > AUDIO_SAMPLE_LIMIT)
    {
        pcm_sample = AUDIO_SAMPLE_LIMIT;
    }
    if (pcm_sample < -AUDIO_SAMPLE_LIMIT)
    {
        pcm_sample = -AUDIO_SAMPLE_LIMIT;
    }
    int duty_offset = pcm_sample * AUDIO_PWM_CENTER_COUNTS / 32768;
    R_GPT_DutyCycleSet(&g_timer6_ctrl, AUDIO_PWM_CENTER_COUNTS + duty_offset, GPT_IO_PIN_GTIOCA);
    R_GPT_DutyCycleSet(&g_timer6_ctrl, AUDIO_PWM_CENTER_COUNTS - duty_offset, GPT_IO_PIN_GTIOCB);
}

/* 配置固定提示音的 PWM 和采样定时器，播放结束依次关闭。 */
static int run_test(void)
{
    timer_cfg_t pwm_config = g_timer6_cfg;
    timer_cfg_t sample_timer_config = g_timer2_cfg;
    int result = -RT_ERROR;
    pwm_config.period_counts = AUDIO_PWM_PERIOD_COUNTS;
    pwm_config.duty_cycle_counts = AUDIO_PWM_CENTER_COUNTS;
    /* GPT2 采样更新率 120MHz/7488，与录音帧率相同。 */
    sample_timer_config.period_counts = AUDIO_SAMPLE_PERIOD_COUNTS;
    sample_timer_config.duty_cycle_counts = AUDIO_SAMPLE_PERIOD_COUNTS / 2;
    sample_timer_config.p_callback = audio_tick;
    test_restore_pin(BSP_IO_PORT_07_PIN_02);
    test_restore_pin(BSP_IO_PORT_07_PIN_03);

    playback_frame_count = AUDIO_TONE_FRAMES;

    playback_position = 0;
    if (R_GPT_Open(&g_timer6_ctrl, &pwm_config) != FSP_SUCCESS)
    {
        goto pins;
    }
    if (R_GPT_Start(&g_timer6_ctrl) != FSP_SUCCESS)
    {
        goto carrier_close;
    }
    if (R_GPT_Open(&g_timer2_ctrl, &sample_timer_config) != FSP_SUCCESS)
    {
        goto carrier_close;
    }
    if (R_GPT_Start(&g_timer2_ctrl) == FSP_SUCCESS)
    {
        rt_tick_t start = rt_tick_get();
        while (playback_position < playback_frame_count &&
               !test_elapsed(start, AUDIO_PLAYBACK_TIMEOUT_MS) && !test_cancelled())
        {
            rt_thread_mdelay(1);
        }
        if (playback_position == playback_frame_count)
        {
            result = TEST_WAIT;
        }
        else
        {
            result = -RT_ETIMEOUT;
        }
    }
    /* 先停采样更新中断，再关闭 PWM，确保 ISR 不访问已关闭的定时器。 */
    R_GPT_Stop(&g_timer2_ctrl);
    R_GPT_Close(&g_timer2_ctrl);
carrier_close:
    R_GPT_Stop(&g_timer6_ctrl);
    R_GPT_Close(&g_timer6_ctrl);
pins:
    rt_pin_mode(BSP_IO_PORT_07_PIN_02, PIN_MODE_OUTPUT);
    rt_pin_write(BSP_IO_PORT_07_PIN_02, 0);
    rt_pin_mode(BSP_IO_PORT_07_PIN_03, PIN_MODE_OUTPUT);
    rt_pin_write(BSP_IO_PORT_07_PIN_03, 0);
    rt_kprintf("AUDIO output samples=%u/%u; listening confirmation required\n",
               playback_position,
               playback_frame_count);
    return result;
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_audio_tone_thread(void *argument)
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
