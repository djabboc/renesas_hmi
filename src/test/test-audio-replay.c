/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-audio-replay.c
 * @brief 独立完成麦克风录音和扬声器回放。
 *
 * 倒计时 3 秒后录音约 1 秒，等待 1 秒再回放；不依赖其他例程的录音。
 * SSI 采集 16 位双声道，在同一缓冲内提取左声道、去直流并有限调整幅度。
 * 先停 DMA/定时器再释放缓冲，硬件完成仍需结合人工试听。
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

/* 只恢复本例使用的两个 PWM 引脚，避免重新配置整张 IOPORT 引脚表。 */
static int configure_pwm_pin(bsp_io_port_pin_t pin)
{
    for (unsigned index = 0; index < g_bsp_pin_cfg.number_of_pins; ++index)
    {
        if (g_bsp_pin_cfg.p_pin_cfg_data[index].pin == pin)
        {
            if (R_IOPORT_PinCfg(&g_ioport_ctrl, pin,
                               g_bsp_pin_cfg.p_pin_cfg_data[index].pin_cfg) == FSP_SUCCESS)
            {
                return TEST_PASS;
            }
            return -RT_ERROR;
        }
    }
    return -RT_ERROR;
}

/* 退出时让两路 PWM 控制脚都为低电平，关闭差分输出。 */
static int mute_output_pins(void)
{
    int result = TEST_PASS;
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_07_PIN_02,
                       IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) != FSP_SUCCESS)
    {
        result = -RT_ERROR;
    }
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_07_PIN_03,
                       IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) != FSP_SUCCESS)
    {
        result = -RT_ERROR;
    }
    return result;
}

#define AUDIO_FRAMES 16384u
#define AUDIO_CHANNELS 2u
#define AUDIO_CAPTURE_BYTES (AUDIO_FRAMES * AUDIO_CHANNELS * sizeof(int16_t))
#define AUDIO_CAPTURE_TIMEOUT_MS 2000u
#define AUDIO_PLAYBACK_TIMEOUT_MS 2000u
#define AUDIO_PWM_PERIOD_COUNTS 1500u
#define AUDIO_PWM_CENTER_COUNTS 750
#define AUDIO_SAMPLE_PERIOD_COUNTS 7488u
#define AUDIO_SAMPLE_LIMIT 4000
#define AUDIO_PLAYBACK_TARGET_PEAK 3000
#define AUDIO_GAIN_SCALE 256
#define AUDIO_MAX_GAIN (8 * AUDIO_GAIN_SCALE)
#define AUDIO_FADE_FRAMES 160u
/* 这些完成标志/位置由中断更新，由测试线程轮询。 */
static volatile unsigned capture_complete;
static volatile unsigned playback_position;
static const int16_t *playback_samples;
static unsigned playback_frame_count;
static volatile fsp_err_t playback_error;

/* 等待可被停止事件打断，倒计时和试听间隔都不会阻塞退出数秒。 */
static int wait_ms(unsigned milliseconds)
{
    rt_tick_t start = rt_tick_get();
    while (!test_elapsed(start, milliseconds))
    {
        if (test_cancelled())
        {
            return -RT_EINTR;
        }
        rt_thread_mdelay(10);
    }
    return TEST_PASS;
}

/* SSI 中断确认 DMA 接收完成；不在中断中处理音频数据。 */
static void mic_callback(i2s_callback_args_t *arguments)
{
    if (arguments->event == I2S_EVENT_RX_FULL)
    {
        capture_complete = 1;
    }
}

/* 启动音频时钟和 SSI 采集；完成或超时后停止接收并关闭硬件。 */
static int capture_audio(int16_t *samples)
{
    i2s_cfg_t microphone_config = g_i2s0_cfg;
    timer_cfg_t audio_clock_config = g_timer_cfg;
    int result = -RT_ERROR;
    fsp_err_t error;
    /* GPT1A 内部供 SSI 时钟：120MHz / 117 / 64 = 16025.64 双声道帧/秒。 */
    audio_clock_config.period_counts = 117;
    audio_clock_config.duty_cycle_counts = 58;
    /* 麦克风每个 32 位时隙发送 24 位数据；SSI 取最高 16 位。
     * 时隙仍为 32 位，双声道帧率不变；16 位 FIFO/DTC 访问使每帧占 4 字节。
     * 16384 帧仍只分配 65536 字节，录音时长从半秒增加到约 1.02 秒。 */
    microphone_config.pcm_width = I2S_PCM_WIDTH_16_BITS;
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
    rt_kprintf("MIC RECORD NOW: speak for about 1 second\n");
    error = R_SSI_Read(&g_i2s0_ctrl, samples, AUDIO_CAPTURE_BYTES);
    if (error == FSP_SUCCESS)
    {
        rt_tick_t start = rt_tick_get();
        while (!capture_complete && !test_elapsed(start, AUDIO_CAPTURE_TIMEOUT_MS) &&
               !test_cancelled())
        {
            rt_thread_mdelay(1);
        }
        if (capture_complete)
        {
            result = TEST_PASS;
        }
        else
        {
            result = -RT_ETIMEOUT;
        }
    }
    else
    {
        rt_kprintf("MIC read error=%d\n", error);
    }
    R_SSI_Stop(&g_i2s0_ctrl);
    rt_thread_mdelay(3);
    R_SSI_Close(&g_i2s0_ctrl);
timer_close:
    R_GPT_Stop(&g_timer_ctrl);
    R_GPT_Close(&g_timer_ctrl);
    return result;
}

/* 原地把 L,R,L,R 双声道压成 L,L 单声道。
 * 写入位置总在当前读取位置之前，不会覆盖尚未读取的左声道数据。 */
static int prepare_playback(int16_t *samples)
{
    int64_t sum = 0;
    int minimum = INT16_MAX;
    int maximum = INT16_MIN;
    unsigned changed = 0;
    unsigned capture_clipped = 0;
    for (unsigned index = 0; index < AUDIO_FRAMES; ++index)
    {
        int sample = samples[index * AUDIO_CHANNELS];
        if (index > 0 && sample != samples[index - 1])
        {
            ++changed;
        }
        samples[index] = (int16_t)sample;
        sum += sample;
        if (sample < minimum)
        {
            minimum = sample;
        }
        if (sample > maximum)
        {
            maximum = sample;
        }
        if (sample <= -32760 || sample >= 32760)
        {
            ++capture_clipped;
        }
    }
    int mean = (int)(sum / (int64_t)AUDIO_FRAMES);
    int peak = maximum - mean;
    if (mean - minimum > peak)
    {
        peak = mean - minimum;
    }
    rt_kprintf("MIC DMA complete: frames=%u min=%d max=%d mean=%d ac_peak=%d changed=%u clipped=%u\n",
               AUDIO_FRAMES, minimum, maximum, mean, peak, changed, capture_clipped);
    if (peak == 0 || changed == 0)
    {
        rt_kprintf("MIC no changing audio; check microphone capture\n");
        return -RT_ERROR;
    }

    /* 去除直流偏置后，按峰值调整幅度；最多放大 8 倍，目标峰值保持低幅。
     * 极弱输入不会被无限放大，最后仍以 AUDIO_SAMPLE_LIMIT 限幅。 */
    int gain = AUDIO_PLAYBACK_TARGET_PEAK * AUDIO_GAIN_SCALE / peak;
    if (gain > AUDIO_MAX_GAIN)
    {
        gain = AUDIO_MAX_GAIN;
    }
    unsigned limited = 0;
    int output_peak = 0;
    for (unsigned index = 0; index < AUDIO_FRAMES; ++index)
    {
        int sample = ((int)samples[index] - mean) * gain / AUDIO_GAIN_SCALE;
        if (sample > AUDIO_SAMPLE_LIMIT)
        {
            sample = AUDIO_SAMPLE_LIMIT;
            ++limited;
        }
        if (sample < -AUDIO_SAMPLE_LIMIT)
        {
            sample = -AUDIO_SAMPLE_LIMIT;
            ++limited;
        }
        /* 开头和结尾各约 10ms 淡入淡出，减小突然启停的爆音。 */
        if (index < AUDIO_FADE_FRAMES)
        {
            sample = sample * (int)index / (int)AUDIO_FADE_FRAMES;
        }
        unsigned frames_left = AUDIO_FRAMES - 1u - index;
        if (frames_left < AUDIO_FADE_FRAMES)
        {
            sample = sample * (int)frames_left / (int)AUDIO_FADE_FRAMES;
        }
        samples[index] = (int16_t)sample;
        int magnitude = sample;
        if (magnitude < 0)
        {
            magnitude = -magnitude;
        }
        if (magnitude > output_peak)
        {
            output_peak = magnitude;
        }
    }
    rt_kprintf("AUDIO prepared: gain_q8=%d (256=1x) output_peak=%d limited=%u\n",
               gain, output_peak, limited);
    return TEST_PASS;
}

/* GPT2 中断只更新占空比，不分配内存、不输出日志。 */
static void audio_tick(timer_callback_args_t *arguments)
{
    int pcm_sample = 0;
    RT_UNUSED(arguments);
    if (playback_position < playback_frame_count)
    {

        /* 线程已完成去直流、幅度调整和限幅，中断直接取单声道样本。 */
        pcm_sample = playback_samples[playback_position];

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
    fsp_err_t error = R_GPT_DutyCycleSet(&g_timer6_ctrl,
                                        AUDIO_PWM_CENTER_COUNTS + duty_offset, GPT_IO_PIN_GTIOCA);
    if (error != FSP_SUCCESS)
    {
        playback_error = error;
    }
    error = R_GPT_DutyCycleSet(&g_timer6_ctrl,
                             AUDIO_PWM_CENTER_COUNTS - duty_offset, GPT_IO_PIN_GTIOCB);
    if (error != FSP_SUCCESS)
    {
        playback_error = error;
    }
}

/* 只播放本次采集的左声道；调用者保持 samples 有效，直到停止定时器。 */
static int play_audio(const int16_t *samples)
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
    playback_samples = samples;
    playback_frame_count = AUDIO_FRAMES;
    playback_position = 0;
    playback_error = FSP_SUCCESS;
    if (configure_pwm_pin(BSP_IO_PORT_07_PIN_02) != TEST_PASS ||
        configure_pwm_pin(BSP_IO_PORT_07_PIN_03) != TEST_PASS)
    {
        goto pins;
    }
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
               playback_error == FSP_SUCCESS &&
               !test_elapsed(start, AUDIO_PLAYBACK_TIMEOUT_MS) && !test_cancelled())
        {
            rt_thread_mdelay(1);
        }
        if (playback_error != FSP_SUCCESS)
        {
            result = -RT_ERROR;
        }
        else if (playback_position == playback_frame_count)
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
    if (mute_output_pins() != TEST_PASS)
    {
        result = -RT_ERROR;
    }
    rt_kprintf("AUDIO output samples=%u/%u pwm_error=%d; listening confirmation required\n",
               playback_position, playback_frame_count, playback_error);
    playback_samples = NULL;
    return result;
}

/* 倒计时、录音、停声等待、回放；所有阶段使用本次采集的同一个缓冲。 */
static int run_test(void)
{
    int result = mute_output_pins();
    if (result != TEST_PASS)
    {
        return result;
    }
    int16_t *samples = rt_malloc(AUDIO_CAPTURE_BYTES);
    if (samples == RT_NULL)
    {
        rt_kprintf("AUDIO allocation failed: need %u bytes\n", (unsigned)AUDIO_CAPTURE_BYTES);
        return -RT_ENOMEM;
    }
    for (unsigned seconds = 3; seconds > 0; --seconds)
    {
        rt_kprintf("MIC recording starts in %u...\n", seconds);
        result = wait_ms(1000);
        if (result != TEST_PASS)
        {
            goto done;
        }
    }
    result = capture_audio(samples);
    if (result != TEST_PASS)
    {
        goto done;
    }
    rt_kprintf("MIC recording finished; stop speaking. Replay starts in 1 second\n");
    result = prepare_playback(samples);
    if (result != TEST_PASS)
    {
        goto done;
    }
    result = wait_ms(1000);
    if (result != TEST_PASS)
    {
        goto done;
    }
    rt_kprintf("AUDIO REPLAY NOW\n");
    result = play_audio(samples);
done:
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
