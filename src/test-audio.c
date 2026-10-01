/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-audio.c
 * @brief SSI/DTC 麦克风采集及 GPT PWM 扬声器输出。
 *
 * 缓冲由测试线程分配，SSI/GPT 中断运行期间保持有效；
 * 先停采样/播放硬件，再释放缓冲，返回 WAIT 表示音质尚待试听。
 */
#include "peripheral-test.h"

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
/* 这些完成标志/位置由中断更新，由 ptest 线程轮询。 */
static volatile unsigned capture_complete;
static volatile unsigned playback_position;
static const int32_t *playback_samples;
static unsigned playback_frame_count;

static void mic_callback(i2s_callback_args_t *arguments)
{
    if (arguments->event == I2S_EVENT_RX_FULL)
    {
        capture_complete = 1;
    }
}

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
    if (R_GPT_Open(&g_timer_ctrl, &audio_clock_config))
    {
        return -RT_ERROR;
    }
    if (R_GPT_Start(&g_timer_ctrl))
    {
        goto timer_close;
    }
    if (R_SSI_Open(&g_i2s0_ctrl, &microphone_config))
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
        if (playback_samples)
        {
            /* 右对齐 24 位左声道：符号扩展并降为 16 位，再衰减为 1/8。 */
            pcm_sample =
                ((int32_t)((uint32_t)playback_samples[playback_position * 2] << 8) >> 16) / 8;
        }
        else
        {
            pcm_sample = (playback_position % 32 < 16) ? 1200 : -1200;
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

/* samples=NULL 播提示音；非空则播放刚采集的左声道。 */
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
    playback_frame_count = samples ? AUDIO_FRAMES : AUDIO_TONE_FRAMES;
    playback_position = 0;
    if (R_GPT_Open(&g_timer6_ctrl, &pwm_config))
    {
        goto pins;
    }
    if (R_GPT_Start(&g_timer6_ctrl))
    {
        goto carrier_close;
    }
    if (R_GPT_Open(&g_timer2_ctrl, &sample_timer_config))
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
        result = playback_position == playback_frame_count ? TEST_WAIT : -RT_ETIMEOUT;
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

int test_audio(const char *stage)
{
    int result;
    int32_t *samples;
    if (strcmp(stage, "tone") == 0)
    {
        return play_audio(NULL);
    }
    if (strcmp(stage, "mic") && strcmp(stage, "replay"))
    {
        return -RT_EINVAL;
    }
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
        else if (strcmp(stage, "replay") == 0)
        {
            result = play_audio(samples);
        }
    }
    rt_free(samples);
    return result;
}
