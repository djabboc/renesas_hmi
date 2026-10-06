/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-audio-replay.c
 * @brief 独立完成麦克风录音和扬声器回放。
 *
 * 先播放参考音，倒计时 3 秒后录音约 1 秒，再回放和重播参考音。
 * SSI 采集 16 位双声道，在同一缓冲内提取左声道、去直流并有限调整幅度。
 * 先停 DMA/定时器再释放缓冲，硬件完成仍需结合人工试听。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
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
#define AUDIO_REFERENCE_FRAMES 8192u
#define AUDIO_BUFFER_GUARD 0x51A7C0DEu
#define AUDIO_BUFFER_FILL_BYTE 0xA5u
#define AUDIO_CAPTURE_FILL_WORD 0xA5A5u
/* 这些完成标志/位置由中断更新，由测试线程轮询。 */
static volatile unsigned capture_complete;
static volatile unsigned capture_active;
static volatile unsigned capture_rx_events;
static volatile unsigned capture_idle_events;
static volatile unsigned capture_early_idle;
static volatile unsigned playback_position;
static const int16_t *playback_samples;
static unsigned playback_frame_count;
static volatile fsp_err_t playback_error;
static volatile unsigned playback_duty_min;
static volatile unsigned playback_duty_max;
static volatile unsigned playback_duty_nonzero;

/* 统计放在线程里完成，不占用音频采样中断的时间。
 * mean 是直流分量；ac_peak/mean_abs_ac 描述去直流后的峰值/平均绝对幅度。
 * changed 只能证明数字变化，不能单独证明这些数字是正确的声音。 */
struct pcm_statistics
{
    int minimum;
    int maximum;
    int mean;
    int ac_peak;
    unsigned mean_abs_ac;
    unsigned zero_samples;
    unsigned changed_samples;
    unsigned clipped_samples;
};

/* stride=2 读取交错的一个声道，stride=1 读取已经提取的单声道。
 * 调用者保证 frames>0，且缓冲覆盖 frames 个指定步长的样本。 */
static void measure_pcm(const int16_t *samples, unsigned frames, unsigned stride,
                        struct pcm_statistics *statistics)
{
    int64_t sum = 0;
    uint64_t absolute_sum = 0;
    memset(statistics, 0, sizeof(*statistics));
    statistics->minimum = INT16_MAX;
    statistics->maximum = INT16_MIN;
    for (unsigned index = 0; index < frames; ++index)
    {
        int sample = samples[index * stride];
        sum += sample;
        if (sample < statistics->minimum)
        {
            statistics->minimum = sample;
        }
        if (sample > statistics->maximum)
        {
            statistics->maximum = sample;
        }
        if (sample == 0)
        {
            ++statistics->zero_samples;
        }
        if (index > 0 && sample != samples[(index - 1u) * stride])
        {
            ++statistics->changed_samples;
        }
        if (sample <= -32760 || sample >= 32760)
        {
            ++statistics->clipped_samples;
        }
    }
    statistics->mean = (int)(sum / (int64_t)frames);
    for (unsigned index = 0; index < frames; ++index)
    {
        int magnitude = (int)samples[index * stride] - statistics->mean;
        if (magnitude < 0)
        {
            magnitude = -magnitude;
        }
        absolute_sum += (unsigned)magnitude;
        if (magnitude > statistics->ac_peak)
        {
            statistics->ac_peak = magnitude;
        }
    }
    statistics->mean_abs_ac = (unsigned)(absolute_sum / frames);
}

/* 同一格式比较安静/发声、左右声道和处理前后，避免只看单个最大值。 */
static void print_pcm_statistics(const char *label, const int16_t *samples,
                                 unsigned frames, unsigned stride)
{
    struct pcm_statistics statistics;
    measure_pcm(samples, frames, stride, &statistics);
    /* 当前工程串口格式化缓冲为 128 字节，拆行保留全部字段和换行。 */
    rt_kprintf("PCM %s n=%u min=%d max=%d mean=%d ac_peak=%d\n",
               label, frames, statistics.minimum, statistics.maximum, statistics.mean,
               statistics.ac_peak);
    rt_kprintf("PCM %s mean_abs_ac=%u zero=%u changed=%u clipped=%u\n",
               label, statistics.mean_abs_ac, statistics.zero_samples,
               statistics.changed_samples, statistics.clipped_samples);
}

/* 在压缩成单声道前观察原始 L/R 数据，分四段发现启动突变或发声时机问题。
 * 初始化的 A5A5 若大量残留，提示接收未覆盖缓冲；偶然同值不能判为未写入。 */
static void inspect_capture(const int16_t *samples)
{
    unsigned fill_words = 0;
    for (unsigned index = 0; index < AUDIO_FRAMES * AUDIO_CHANNELS; ++index)
    {
        if ((uint16_t)samples[index] == AUDIO_CAPTURE_FILL_WORD)
        {
            ++fill_words;
        }
    }
    rt_kprintf("MIC buffer fill_A5A5=%u/%u (large count suggests incomplete reception)\n",
               fill_words, AUDIO_FRAMES * AUDIO_CHANNELS);
    print_pcm_statistics("RAW-L", samples, AUDIO_FRAMES, AUDIO_CHANNELS);
    print_pcm_statistics("RAW-R", samples + 1, AUDIO_FRAMES, AUDIO_CHANNELS);
    for (unsigned part = 0; part < 4; ++part)
    {
        unsigned first = part * (AUDIO_FRAMES / 4u);
        rt_kprintf("MIC window=%u frames=%u..%u (~256ms)\n",
                   part, first, first + AUDIO_FRAMES / 4u - 1u);
        print_pcm_statistics("WIN-L", samples + first * AUDIO_CHANNELS,
                             AUDIO_FRAMES / 4u, AUDIO_CHANNELS);
        print_pcm_statistics("WIN-R", samples + first * AUDIO_CHANNELS + 1u,
                             AUDIO_FRAMES / 4u, AUDIO_CHANNELS);
        for (unsigned offset = 0; offset < 3; ++offset)
        {
            unsigned index = (first + offset) * AUDIO_CHANNELS;
            rt_kprintf("MIC raw frame=%u L=%04X/%d R=%04X/%d\n", first + offset,
                       (unsigned)(uint16_t)samples[index], samples[index],
                       (unsigned)(uint16_t)samples[index + 1u], samples[index + 1u]);
        }
    }
}

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
        ++capture_rx_events;
        capture_complete = 1;
    }
    else if (arguments->event == I2S_EVENT_IDLE)
    {
        ++capture_idle_events;
        if (capture_active && !capture_complete)
        {
            capture_early_idle = 1;
        }
    }
}

/* 只清理本次成功打开的 GPT；先停止再关闭，每个返回值都记录。 */
static int close_timer(const char *label, gpt_instance_ctrl_t *control)
{
    int result = TEST_PASS;
    fsp_err_t error = R_GPT_Stop(control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("AUDIO %s timer stop error=%d\n", label, error);
        result = -RT_ERROR;
    }
    error = R_GPT_Close(control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("AUDIO %s timer close error=%d\n", label, error);
        result = -RT_ERROR;
    }
    return result;
}

/* 启动音频时钟和 SSI 采集；完成或超时后停止接收并关闭硬件。 */
static int capture_audio(int16_t *samples)
{
    i2s_cfg_t microphone_config = g_i2s0_cfg;
    timer_cfg_t audio_clock_config = g_timer_cfg;
    int result = -RT_ERROR;
    fsp_err_t error;
    rt_tick_t start = rt_tick_get();
    /* GPT1A 内部供 SSI 时钟：120MHz / 117 / 64 = 16025.64 双声道帧/秒。 */
    audio_clock_config.period_counts = 117;
    audio_clock_config.duty_cycle_counts = 58;
    /* 麦克风每个 32 位时隙发送 24 位数据；SSI 取最高 16 位。
     * 时隙仍为 32 位，双声道帧率不变；16 位 FIFO/DTC 访问使每帧占 4 字节。
     * 16384 帧仍只分配 65536 字节，录音时长从半秒增加到约 1.02 秒。 */
    microphone_config.pcm_width = I2S_PCM_WIDTH_16_BITS;
    microphone_config.word_length = I2S_WORD_LENGTH_32_BITS;
    microphone_config.p_callback = mic_callback;
    memset(samples, AUDIO_BUFFER_FILL_BYTE, AUDIO_CAPTURE_BYTES);
    capture_complete = 0;
    capture_active = 0;
    capture_rx_events = 0;
    capture_idle_events = 0;
    capture_early_idle = 0;
    rt_kprintf("MIC config: PCM=16 slot=32 channels=2 bytes=%u expected=1022ms\n",
               (unsigned)AUDIO_CAPTURE_BYTES);
    rt_kprintf("MIC clocks: PCLKD=%u GPT1 period=117 BCLK~1025641Hz LRCLK~16026Hz\n",
               R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKD));
    audio_clock_config.source_div = TIMER_SOURCE_DIV_1;
    error = R_GPT_Open(&g_timer_ctrl, &audio_clock_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC clock timer open error=%d\n", error);
        return -RT_ERROR;
    }
    error = R_GPT_Start(&g_timer_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC clock timer start error=%d\n", error);
        goto timer_close;
    }
    error = R_SSI_Open(&g_i2s0_ctrl, &microphone_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC SSI open error=%d\n", error);
        goto timer_close;
    }
    rt_kprintf("MIC SSI opened: SSICR=%08X SSIFCR=%08X FIFO_access=%u bytes RX_IRQ=%d\n",
               (unsigned)R_SSI0->SSICR, (unsigned)R_SSI0->SSIFCR,
               1u << g_i2s0_ctrl.fifo_access_size, microphone_config.rxi_irq);
    if (microphone_config.p_transfer_rx != NULL)
    {
        rt_kprintf("MIC DTC access=%u bytes block=%u src=%08X\n",
                   1u << microphone_config.p_transfer_rx->p_cfg->p_info->size,
                   microphone_config.p_transfer_rx->p_cfg->p_info->length,
                   (unsigned)microphone_config.p_transfer_rx->p_cfg->p_info->p_src);
    }
    rt_kprintf("MIC RECORD NOW: keep quiet OR sustain AH for this entire 1 second\n");
    start = rt_tick_get();
    capture_active = 1;
    error = R_SSI_Read(&g_i2s0_ctrl, samples, AUDIO_CAPTURE_BYTES);
    if (error == FSP_SUCCESS)
    {
        while (!capture_complete && !test_elapsed(start, AUDIO_CAPTURE_TIMEOUT_MS) &&
               !capture_early_idle && !test_cancelled())
        {
            rt_thread_mdelay(1);
        }
        if (test_cancelled())
        {
            result = -RT_EINTR;
        }
        else if (capture_early_idle)
        {
            result = -RT_ERROR;
        }
        else if (capture_complete)
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
    capture_active = 0;
    /* 先保存完成时的寄存器并马上停止接收，再打印较长日志。
     * 否则打印期间继续接收，会人为制造 FIFO 溢出并干扰诊断。 */
    unsigned elapsed_ms = (unsigned)((rt_tick_get() - start) * 1000u / RT_TICK_PER_SECOND);
    unsigned end_ssicr = R_SSI0->SSICR;
    unsigned end_ssisr = R_SSI0->SSISR;
    unsigned end_ssifsr = R_SSI0->SSIFSR;
    error = R_SSI_Stop(&g_i2s0_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC SSI stop error=%d\n", error);
        result = -RT_ERROR;
    }
    rt_kprintf("MIC receive: result=%d complete=%u rx_events=%u early_idle=%u\n",
               result, capture_complete, capture_rx_events, capture_early_idle);
    rt_kprintf("MIC receive: elapsed=%u ms remaining_CPU_words=%u\n", elapsed_ms,
               (unsigned)g_i2s0_ctrl.rx_dest_samples);
    rt_kprintf("MIC SSI end: SSICR=%08X SSISR=%08X SSIFSR=%08X\n",
               end_ssicr, end_ssisr, end_ssifsr);
    if (microphone_config.p_transfer_rx != NULL)
    {
        transfer_properties_t properties;
        error = microphone_config.p_transfer_rx->p_api->infoGet(
            microphone_config.p_transfer_rx->p_ctrl, &properties);
        if (error == FSP_SUCCESS)
        {
            rt_kprintf("MIC DTC end: remaining_blocks=%u remaining_length=%u\n",
                       (unsigned)properties.block_count_remaining,
                       (unsigned)properties.transfer_length_remaining);
        }
        else
        {
            rt_kprintf("MIC DTC info error=%d\n", error);
        }
    }
    rt_thread_mdelay(3);
    error = R_SSI_Close(&g_i2s0_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC SSI close error=%d\n", error);
        result = -RT_ERROR;
    }
    rt_kprintf("MIC SSI closed: idle_events=%u result=%d\n", capture_idle_events, result);
timer_close:
    if (close_timer("MIC-clock", &g_timer_ctrl) != TEST_PASS)
    {
        result = -RT_ERROR;
    }
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
    rt_kprintf("MIC DMA complete: frames=%u min=%d max=%d mean=%d ac_peak=%d\n",
               AUDIO_FRAMES, minimum, maximum, mean, peak);
    rt_kprintf("MIC DMA complete: changed=%u clipped=%u\n", changed, capture_clipped);
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
    print_pcm_statistics("PREPARED-L", samples, AUDIO_FRAMES, 1);
    rt_kprintf("AUDIO gain reason: target_peak=3000 max_gain=8x; use WIN-L stats to check startup outliers\n");
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
    unsigned duty_a = (unsigned)(AUDIO_PWM_CENTER_COUNTS + duty_offset);
    if (duty_a < playback_duty_min)
    {
        playback_duty_min = duty_a;
    }
    if (duty_a > playback_duty_max)
    {
        playback_duty_max = duty_a;
    }
    if (duty_offset != 0)
    {
        ++playback_duty_nonzero;
    }
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

/* 参考音和录音共用这一条播放路径；samples 必须有效且 frames>0。
 * 调用者保持缓冲有效，直到本函数停止并关闭采样定时器。 */
static int play_audio(const char *label, const int16_t *samples, unsigned frames)
{
    timer_cfg_t pwm_config = g_timer6_cfg;
    timer_cfg_t sample_timer_config = g_timer2_cfg;
    int result = -RT_ERROR;
    fsp_err_t error;
    rt_tick_t start = rt_tick_get();
    pwm_config.source_div = TIMER_SOURCE_DIV_1;
    pwm_config.mode = TIMER_MODE_PERIODIC;
    pwm_config.period_counts = AUDIO_PWM_PERIOD_COUNTS;
    pwm_config.duty_cycle_counts = AUDIO_PWM_CENTER_COUNTS;
    /* GPT2 采样更新率 120MHz/7488，与录音帧率相同。 */
    sample_timer_config.period_counts = AUDIO_SAMPLE_PERIOD_COUNTS;
    sample_timer_config.source_div = TIMER_SOURCE_DIV_1;
    sample_timer_config.mode = TIMER_MODE_PERIODIC;
    sample_timer_config.duty_cycle_counts = AUDIO_SAMPLE_PERIOD_COUNTS / 2;
    sample_timer_config.p_callback = audio_tick;
    playback_samples = samples;
    playback_frame_count = frames;
    playback_position = 0;
    playback_error = FSP_SUCCESS;
    playback_duty_min = AUDIO_PWM_CENTER_COUNTS;
    playback_duty_max = AUDIO_PWM_CENTER_COUNTS;
    playback_duty_nonzero = 0;
    print_pcm_statistics(label, samples, frames, 1);
    rt_kprintf("AUDIO %s start: frames=%u GPT2_period=%u PWM_period=%u\n",
               label, frames, AUDIO_SAMPLE_PERIOD_COUNTS, AUDIO_PWM_PERIOD_COUNTS);
    if (configure_pwm_pin(BSP_IO_PORT_07_PIN_02) != TEST_PASS ||
        configure_pwm_pin(BSP_IO_PORT_07_PIN_03) != TEST_PASS)
    {
        goto pins;
    }
    error = R_GPT_Open(&g_timer6_ctrl, &pwm_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("AUDIO PWM open error=%d\n", error);
        goto pins;
    }
    error = R_GPT_Start(&g_timer6_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("AUDIO PWM start error=%d\n", error);
        goto carrier_close;
    }
    error = R_GPT_Open(&g_timer2_ctrl, &sample_timer_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("AUDIO sample timer open error=%d\n", error);
        goto carrier_close;
    }
    start = rt_tick_get();
    error = R_GPT_Start(&g_timer2_ctrl);
    if (error == FSP_SUCCESS)
    {
        while (playback_position < playback_frame_count &&
               playback_error == FSP_SUCCESS &&
               !test_elapsed(start, AUDIO_PLAYBACK_TIMEOUT_MS) && !test_cancelled())
        {
            rt_thread_mdelay(1);
        }
        if (test_cancelled())
        {
            result = -RT_EINTR;
        }
        else if (playback_error != FSP_SUCCESS)
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
    else
    {
        rt_kprintf("AUDIO sample timer start error=%d\n", error);
    }
    /* 先停采样更新中断，再关闭 PWM，确保 ISR 不访问已关闭的定时器。 */
    if (close_timer("sample", &g_timer2_ctrl) != TEST_PASS)
    {
        result = -RT_ERROR;
    }
carrier_close:
    if (close_timer("PWM", &g_timer6_ctrl) != TEST_PASS)
    {
        result = -RT_ERROR;
    }
pins:
    if (mute_output_pins() != TEST_PASS)
    {
        result = -RT_ERROR;
    }
    rt_kprintf("AUDIO %s output samples=%u/%u pwm_error=%d result=%d\n",
               label, playback_position, playback_frame_count, playback_error,
               result);
    rt_kprintf("AUDIO %s duty_A=%u..%u active_duty_samples=%u elapsed=%u ms\n",
               label, playback_duty_min, playback_duty_max, playback_duty_nonzero,
               (unsigned)((rt_tick_get() - start) * 1000u / RT_TICK_PER_SECOND));
    playback_samples = NULL;
    return result;
}

/* 生成 ~501Hz 正弦参考音，复用同一 RAM 和 play_audio，不调用提示音例程。
 * 这样参考音能验证本文件自己的 RAM 读样本、GPT2 中断和 GPT6 输出路径。 */
static int play_reference(int16_t *samples, const char *label)
{
    static const int16_t sine[32] =
    {
        0, 585, 1148, 1667, 2121, 2494, 2772, 2942,
        3000, 2942, 2772, 2494, 2121, 1667, 1148, 585,
        0, -585, -1148, -1667, -2121, -2494, -2772, -2942,
        -3000, -2942, -2772, -2494, -2121, -1667, -1148, -585
    };
    for (unsigned index = 0; index < AUDIO_REFERENCE_FRAMES; ++index)
    {
        int sample = sine[index % 32u];
        if (index < AUDIO_FADE_FRAMES)
        {
            sample = sample * (int)index / (int)AUDIO_FADE_FRAMES;
        }
        unsigned remaining = AUDIO_REFERENCE_FRAMES - 1u - index;
        if (remaining < AUDIO_FADE_FRAMES)
        {
            sample = sample * (int)remaining / (int)AUDIO_FADE_FRAMES;
        }
        samples[index] = (int16_t)sample;
    }
    return play_audio(label, samples, AUDIO_REFERENCE_FRAMES);
}

/* 倒计时、录音、停声等待、回放；所有阶段使用本次采集的同一个缓冲。 */
static int run_test(void)
{
    if (R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKD) != 120000000u)
    {
        rt_kprintf("REPLAY unsupported timer clock; expected PCLKD=120MHz\n");
        return -RT_ERROR;
    }
    int result = mute_output_pins();
    if (result != TEST_PASS)
    {
        return result;
    }
    /* 两侧哨兵帮助检测 DTC/数据处理是否越过缓冲边界，仍保持 4 字节对齐。 */
    uint32_t *allocation = rt_malloc(AUDIO_CAPTURE_BYTES + 2u * sizeof(uint32_t));
    if (allocation == RT_NULL)
    {
        rt_kprintf("AUDIO allocation failed: need %u bytes including guards\n",
                   (unsigned)(AUDIO_CAPTURE_BYTES + 2u * sizeof(uint32_t)));
        return -RT_ENOMEM;
    }
    int16_t *samples = (int16_t *)(allocation + 1);
    uint32_t *tail_guard = (uint32_t *)((uint8_t *)samples + AUDIO_CAPTURE_BYTES);
    allocation[0] = AUDIO_BUFFER_GUARD;
    *tail_guard = AUDIO_BUFFER_GUARD;
    rt_kprintf("REPLAY diagnostic v1: REF-BEFORE -> countdown -> capture -> inspect -> replay -> REF-AFTER\n");
    rt_kprintf("REPLAY stage 1: reference beep BEFORE; remember whether you hear it\n");
    result = play_reference(samples, "REF-BEFORE");
    if (result != TEST_WAIT)
    {
        goto done;
    }
    rt_kprintf("REPLAY stage 2: keep quiet for baseline OR sustain AH during recording\n");
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
    if (allocation[0] != AUDIO_BUFFER_GUARD || *tail_guard != AUDIO_BUFFER_GUARD)
    {
        rt_kprintf("MIC buffer guard CORRUPTED; aborting before PCM conversion\n");
        result = -RT_ERROR;
        goto done;
    }
    rt_kprintf("MIC buffer guards MATCH\n");
    if (result != TEST_PASS)
    {
        inspect_capture(samples);
        goto done;
    }
    rt_kprintf("MIC recording finished; stop speaking. REPLAY stage 3: inspect L/R and prepare\n");
    inspect_capture(samples);
    result = prepare_playback(samples);
    if (result != TEST_PASS)
    {
        rt_kprintf("MIC PCM preparation failed; replay skipped, reference AFTER still runs\n");
        int diagnostic_result = play_reference(samples, "REF-AFTER");
        rt_kprintf("REPLAY failed preparation=%d reference_after=%d\n", result, diagnostic_result);
        goto done;
    }
    result = wait_ms(1000);
    if (result != TEST_PASS)
    {
        goto done;
    }
    rt_kprintf("REPLAY stage 4: AUDIO REPLAY NOW (recorded LEFT channel)\n");
    result = play_audio("RECORDED-L", samples, AUDIO_FRAMES);
    if (result != TEST_WAIT)
    {
        goto done;
    }
    result = wait_ms(500);
    if (result != TEST_PASS)
    {
        goto done;
    }
    rt_kprintf("REPLAY stage 5: reference beep AFTER; compare with the first beep\n");
    result = play_reference(samples, "REF-AFTER");
done:
    if (allocation[0] != AUDIO_BUFFER_GUARD || *tail_guard != AUDIO_BUFFER_GUARD)
    {
        rt_kprintf("REPLAY final buffer guard CORRUPTED\n");
        result = -RT_ERROR;
    }
    rt_kprintf("REPLAY diagnostic finished result=%d; report both beeps AND recorded voice\n", result);
    rt_free(allocation);
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
