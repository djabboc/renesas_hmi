/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-audio-replay.c
 * @brief 独立完成麦克风录音和扬声器回放。
 *
 * 本次先采集再回放，不依赖其他例程的录音；先停 DMA/定时器再释放缓冲。
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

#define AUDIO_FRAMES 8192u
#define AUDIO_CHANNELS 2u
#define AUDIO_CAPTURE_BYTES (AUDIO_FRAMES * AUDIO_CHANNELS * sizeof(int32_t))
#define AUDIO_CAPTURE_TIMEOUT_MS 2000u
#define AUDIO_PLAYBACK_TIMEOUT_MS 1500u
#define AUDIO_PWM_PERIOD_COUNTS 1500u
#define AUDIO_PWM_CENTER_COUNTS 750
#define AUDIO_SAMPLE_PERIOD_COUNTS 7488u
#define AUDIO_SAMPLE_LIMIT 4000
#define AUDIO_TONE_FRAMES 8000u
/* 这些完成标志/位置由中断更新，由测试线程轮询。 */
static volatile unsigned capture_complete;
static volatile unsigned playback_position;
static const int32_t *playback_samples;
static unsigned playback_frame_count;

/* SSI 中断确认 DMA 接收完成；不在中断中处理音频数据。 */
static void mic_callback(i2s_callback_args_t *arguments)
{
    if (arguments->event == I2S_EVENT_RX_FULL)
    {
        capture_complete = 1;
    }
}

/* 启动音频时钟和 SSI 采集；完成或超时后停止接收并关闭硬件。 */
static int capture_audio(int32_t *samples)
{
    i2s_cfg_t microphone_config = g_i2s0_cfg;
    timer_cfg_t audio_clock_config = g_timer_cfg;
    int result = -RT_ERROR;
    /* GPT1A 内部供 SSI 时钟：120MHz / 117 / 64 = 16025.64 双声道帧/秒。 */
    audio_clock_config.period_counts = 117;
    audio_clock_config.duty_cycle_counts = 58;
    microphone_config.p_callback = mic_callback;
    memset(samples, 0, AUDIO_CAPTURE_BYTES);
    capture_complete = 0;
    if (R_GPT_Open(&g_timer_ctrl, &audio_clock_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_GPT_Start(&g_timer_ctrl) != FSP_SUCCESS)
    {
        goto timer_close;
    }
    if (R_SSI_Open(&g_i2s0_ctrl, &microphone_config) != FSP_SUCCESS)
    {
        goto timer_close;
    }
    if (R_SSI_Read(&g_i2s0_ctrl, samples, AUDIO_CAPTURE_BYTES) == FSP_SUCCESS)
    {
        rt_tick_t start = rt_tick_get();
        while (!capture_complete && !test_elapsed(start, AUDIO_CAPTURE_TIMEOUT_MS) &&
               !test_cancelled())
        {
            rt_thread_mdelay(1);
        }
        if (capture_complete)
        {
            result = 0;
        }
    }
    R_SSI_Stop(&g_i2s0_ctrl);
    rt_thread_mdelay(3);
    R_SSI_Close(&g_i2s0_ctrl);
timer_close:
    R_GPT_Stop(&g_timer_ctrl);
    R_GPT_Close(&g_timer_ctrl);
    return result;
}

/* GPT2 中断只更新占空比，不分配内存、不输出日志。 */
static void audio_tick(timer_callback_args_t *arguments)
{
    int pcm_sample = 0;
    RT_UNUSED(arguments);
    if (playback_position < playback_frame_count)
    {

        /* 右对齐 24 位左声道：符号扩展并降为 16 位，再衰减为 1/8。 */
        pcm_sample = ((int32_t)((uint32_t)playback_samples[playback_position * 2] << 8) >> 16) / 8;

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

/* 只播放本次采集的左声道；调用者保持 samples 有效，直到停止定时器。 */
static int play_audio(const int32_t *samples)
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
    playback_samples = samples;

    playback_frame_count = AUDIO_FRAMES;

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
    playback_samples = NULL;
    return result;
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    int result;
    int32_t *samples;

    samples = rt_malloc(AUDIO_CAPTURE_BYTES);
    if (!samples)
    {
        return -RT_ENOMEM;
    }
    rt_kprintf("MIC capture 8192 stereo frames at ~16026Hz; microphone on LEFT\n");
    result = capture_audio(samples);
    if (!result)
    {
        int32_t minimum = INT32_MAX;
        int32_t maximum = INT32_MIN;
        unsigned changed_samples = 0;
        for (unsigned frame_index = 0; frame_index < AUDIO_FRAMES; ++frame_index)
        {
            /* SSI PDTA 右对齐 24 位样本，统计前必须显式符号扩展。 */
            int32_t pcm_sample = (int32_t)((uint32_t)samples[frame_index * 2] << 8) >> 8;
            if (pcm_sample < minimum)
            {
                minimum = pcm_sample;
            }
            if (pcm_sample > maximum)
            {
                maximum = pcm_sample;
            }
            if (frame_index && samples[frame_index * 2] != samples[(frame_index - 1) * 2])
            {
                ++changed_samples;
            }
        }
        rt_kprintf("MIC DMA complete left min=%d max=%d changed=%u/%u; acoustic response needs "
                   "speaking test\n",
                   minimum,
                   maximum,
                   changed_samples,
                   AUDIO_FRAMES - 1);
        if (!changed_samples)
        {
            result = -RT_ERROR;
        }
        else
        {
            result = play_audio(samples);
        }
    }
    rt_free(samples);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_audio_replay_thread(void *argument)
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
