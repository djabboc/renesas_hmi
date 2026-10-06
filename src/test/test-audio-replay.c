/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-audio-replay.c
 * @brief 独立录音 5 秒，再通过 J8 喇叭回放同一段录音 5 秒。
 *
 * SSI/DTC 连续采集 24 位双声道；线程提取左声道、去掉慢漂移，再编码为 mu-law。
 * 每样本存 1 字节，使 5 秒录音能放入现有堆；回放时还原为 PCM16。
 * 一个线程完成倒计时、采集、回放和清理，不调用其他测试文件。
 * 阅读顺序：线程入口 → run_test → capture_audio / play_audio → 中断回调。
 */
#include <rtthread.h>
#include "hal_data.h"
#include <string.h>

enum
{
    TEST_PASS = 0,
    TEST_WAIT = 1
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

/* 只恢复指定音频引脚的复用，避免重新配置整张 IOPORT 引脚表。 */
static int configure_audio_pin(bsp_io_port_pin_t pin)
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

/* GPT1 时钟：120MHz/117；双声道每帧 64 个位时钟。
 * GPT2 使用相同的 117*64 分频，录音与播放均约 16025.64 样本/秒。
 * 80128 帧对应 4.999987 秒，正好分为 626 个 128 帧块。 */
#define AUDIO_CLOCK_HZ 120000000u
#define AUDIO_CLOCK_PERIOD 117u
#define AUDIO_SAMPLE_PERIOD (AUDIO_CLOCK_PERIOD * 64u)
#define AUDIO_SECONDS 5u
#define AUDIO_FRAMES (AUDIO_CLOCK_HZ * AUDIO_SECONDS / AUDIO_SAMPLE_PERIOD)
#define AUDIO_BLOCK_FRAMES 128u
#define AUDIO_BLOCK_COUNT (AUDIO_FRAMES / AUDIO_BLOCK_FRAMES)
#define AUDIO_CHANNELS 2u
#define AUDIO_BLOCK_BYTES (AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS * sizeof(uint32_t))
#define AUDIO_SECOND_FRAMES 16026u
#define AUDIO_TIMEOUT_MS 6500u
#define AUDIO_GUARD 0x51A7C0DEu
#define AUDIO_PWM_PERIOD 1500u
#define AUDIO_PWM_CENTER 750
#define AUDIO_OUTPUT_LIMIT 3000
#define AUDIO_GAIN_SCALE 256
#define AUDIO_MAX_GAIN (64 * AUDIO_GAIN_SCALE)
#define AUDIO_FADE_FRAMES 160u
/* 一阶高通：截止频率约 40Hz，去掉启动后的慢漂移，保留语音频段。
 * 滤波仍使用原始 24 位精度；除以 32 后存储，比直接除以 256 多保留 3 位弱信号。
 * 超过 PCM16 范围的启动瞬态由 mu-law 编码器限幅，不影响 RAM 大小。 */
#define AUDIO_HIGH_PASS_DIVISOR 64
#define AUDIO_STORAGE_DIVISOR 32

/* DTC 写入当前块时，线程处理上一块；哨兵用于检查 DMA 是否越界。
 * 不在中断内编码或打印。线程若来不及处理，直接报错，避免静默丢块。 */
struct capture_buffer
{
    uint32_t before;
    uint32_t samples[AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS];
    uint32_t after;
};
static struct capture_buffer capture_buffers[2];

/* 原始 24 位数据按秒统计；sum 使用 64 位，防止累加溢出。
 * changed 只能证明数字变化，音频内容仍需实际试听。 */
struct capture_statistics
{
    int minimum;
    int maximum;
    int64_t sum;
    unsigned frames;
    unsigned changed;
    unsigned near_full;
    unsigned right_peak;
    int filtered_minimum;
    int filtered_maximum;
    uint64_t filtered_absolute_sum;
    unsigned storage_clipped;
};
static struct capture_statistics capture_statistics[AUDIO_SECONDS];
static int filter_previous_input;
static int filter_previous_output;
static volatile unsigned capture_completed;
static volatile unsigned capture_consumed;
static volatile unsigned capture_running;
static volatile unsigned capture_stop_requested;
static volatile unsigned capture_idle_events;
static volatile unsigned capture_overruns;
static volatile unsigned capture_early_idle;
static volatile int capture_error;
static volatile fsp_err_t capture_read_error;
static volatile fsp_err_t capture_stop_error;
static volatile rt_tick_t capture_finished_at;

/* 回放时保持录音缓冲有效，直到 GPT2 停止；中断只解码并更新占空比。 */
static const uint8_t *recording;
static volatile unsigned playback_position;
static volatile fsp_err_t playback_error;
static volatile unsigned playback_limited;
static volatile unsigned playback_duty_min;
static volatile unsigned playback_duty_max;
static int playback_mean;
static int playback_gain;

/* 24 位数据右对齐；先掩码，再显式扩展符号，不依赖负数右移行为。 */
static int pcm24_signed(uint32_t word)
{
    int value = (int)(word & 0x00FFFFFFu);
    if ((word & 0x00800000u) != 0)
    {
        value -= 0x01000000;
    }
    return value;
}

/* G.711 mu-law 对小幅声音保留较细量化，大幅声音使用较粗量化。
 * 一个字节保存符号、3 位段号和 4 位尾数；相比线性 PCM8 更适合语音。
 * 这是有损编码；它只节省录音 RAM，不是 MP3，也不替代正确采样。 */
static unsigned encode_mulaw(int sample)
{
    unsigned sign = 0;
    if (sample < 0)
    {
        sign = 0x80u;
        sample = -sample;
    }
    if (sample > 32635)
    {
        sample = 32635;
    }
    sample += 132;
    unsigned exponent = 0;
    unsigned boundary = 256;
    while ((unsigned)sample >= boundary && exponent < 7)
    {
        ++exponent;
        boundary *= 2;
    }
    unsigned mantissa = ((unsigned)sample >> (exponent + 3u)) & 0x0Fu;
    return (~(sign | (exponent << 4u) | mantissa)) & 0xFFu;
}

/* 反转编码位，按段号还原幅度，再减去编码时的偏置 132。 */
static int decode_mulaw(unsigned code)
{
    unsigned value = (~code) & 0xFFu;
    unsigned exponent = (value >> 4u) & 7u;
    int sample = (int)(((value & 0x0Fu) * 8u + 132u) << exponent) - 132;
    if ((value & 0x80u) != 0)
    {
        sample = -sample;
    }
    return sample;
}

/* 正常录满和错误退出都先请求停止，关闭 SSI 留在线程内完成。 */
static void stop_capture(void)
{
    capture_running = 0;
    capture_stop_requested = 1;
    capture_stop_error = R_SSI_Stop(&g_i2s0_ctrl);
}

/* FSP 允许在 RX_FULL 回调中提交下一个 Read；SSI 保持 REN 连续接收。
 * 下一块使用另一缓冲，不在块间 Stop/Open，避免人为制造采样间隙。 */
static void mic_callback(i2s_callback_args_t *arguments)
{
    if (arguments->event == I2S_EVENT_RX_FULL && capture_running)
    {
        __DMB();
        ++capture_completed;
        if (capture_completed == AUDIO_BLOCK_COUNT)
        {
            capture_finished_at = rt_tick_get();
            stop_capture();
            return;
        }
        if (capture_completed - capture_consumed >= 2u)
        {
            ++capture_overruns;
            capture_error = -RT_EFULL;
            stop_capture();
            return;
        }
        unsigned next = capture_completed % 2u;
        capture_read_error = R_SSI_Read(&g_i2s0_ctrl, capture_buffers[next].samples,
                                       AUDIO_BLOCK_BYTES);
        if (capture_read_error != FSP_SUCCESS)
        {
            capture_error = -RT_ERROR;
            stop_capture();
        }
    }
    else if (arguments->event == I2S_EVENT_IDLE)
    {
        ++capture_idle_events;
        if (capture_running)
        {
            ++capture_early_idle;
            capture_error = -RT_ERROR;
            capture_running = 0;
        }
    }
}

/* y[n] = x[n] - x[n-1] + (63/64)*y[n-1]。
 * 使用有符号除法，不依赖负数右移；状态跨 DMA 块连续，每次录音重新置零。
 * 常量和慢漂移逐渐衰减，不能把启动的大峰值误当成持续语音。 */
static int filter_microphone(int input)
{
    int output = input - filter_previous_input + filter_previous_output;
    output -= filter_previous_output / AUDIO_HIGH_PASS_DIVISOR;
    filter_previous_input = input;
    filter_previous_output = output;
    return output;
}

/* 只在线程中处理已完成块；原始统计与滤波统计都保留，便于定位故障。
 * 高通先于位宽转换和压缩，避免弱信号先被量化成零；采集期间不打印。 */
static void consume_block(uint8_t *destination, unsigned block)
{
    const uint32_t *samples = capture_buffers[block % 2u].samples;
    unsigned first = block * AUDIO_BLOCK_FRAMES;
    for (unsigned offset = 0; offset < AUDIO_BLOCK_FRAMES; ++offset)
    {
        unsigned frame = first + offset;
        int left = pcm24_signed(samples[offset * AUDIO_CHANNELS]);
        int right = pcm24_signed(samples[offset * AUDIO_CHANNELS + 1u]);
        struct capture_statistics *statistics = &capture_statistics[frame / AUDIO_SECOND_FRAMES];
        if (left < statistics->minimum)
        {
            statistics->minimum = left;
        }
        if (left > statistics->maximum)
        {
            statistics->maximum = left;
        }
        statistics->sum += left;
        ++statistics->frames;
        if (offset > 0 && left != pcm24_signed(samples[(offset - 1u) * AUDIO_CHANNELS]))
        {
            ++statistics->changed;
        }
        if (left <= -8386560 || left >= 8386560)
        {
            ++statistics->near_full;
        }
        if (right < 0)
        {
            right = -right;
        }
        if ((unsigned)right > statistics->right_peak)
        {
            statistics->right_peak = (unsigned)right;
        }
        int filtered = filter_microphone(left);
        if (filtered < statistics->filtered_minimum)
        {
            statistics->filtered_minimum = filtered;
        }
        if (filtered > statistics->filtered_maximum)
        {
            statistics->filtered_maximum = filtered;
        }
        int magnitude = filtered;
        if (magnitude < 0)
        {
            magnitude = -magnitude;
        }
        statistics->filtered_absolute_sum += (unsigned)magnitude;
        int stored = filtered / AUDIO_STORAGE_DIVISOR;
        if (stored < -32635 || stored > 32635)
        {
            ++statistics->storage_clipped;
        }
        destination[frame] = (uint8_t)encode_mulaw(stored);
    }
    /* 全部数据读取完成后再公布 consumed，避免 ISR 提前复用本块。 */
    __DMB();
    ++capture_consumed;
}

/* 清理等待不响应取消，确保停止请求不会跳过 SSI 的帧结束过程。 */
static int wait_capture_idle(void)
{
    rt_tick_t start = rt_tick_get();
    while (capture_idle_events == 0 && !test_elapsed(start, 20))
    {
        rt_thread_mdelay(1);
    }
    if (capture_idle_events == 0)
    {
        return -RT_ETIMEOUT;
    }
    return TEST_PASS;
}

/* 已成功打开的 GPT 必须先 Stop 再 Close，两个返回值都检查。 */
static int close_timer(const char *label, gpt_instance_ctrl_t *control)
{
    int result = TEST_PASS;
    fsp_err_t error = R_GPT_Stop(control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("REPLAY %s timer stop error=%d\n", label, error);
        result = -RT_ERROR;
    }
    error = R_GPT_Close(control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("REPLAY %s timer close error=%d\n", label, error);
        result = -RT_ERROR;
    }
    return result;
}

/* 录音期间扬声器保持静音。与独立 audio-mic 相同，从启动后的第一帧采集。
 * 所有 80128 帧连续保存，5 秒完成后关闭 SSI/DTC/GPT1，再进入回放。 */
static int capture_audio(uint8_t *destination)
{
    timer_cfg_t clock_config = g_timer_cfg;
    i2s_cfg_t microphone_config = g_i2s0_cfg;
    int result = -RT_ERROR;
    fsp_err_t error;
    rt_tick_t start = rt_tick_get();
    capture_completed = 0;
    capture_consumed = 0;
    capture_running = 0;
    capture_stop_requested = 0;
    capture_idle_events = 0;
    capture_overruns = 0;
    capture_early_idle = 0;
    capture_error = 0;
    capture_read_error = FSP_SUCCESS;
    capture_stop_error = FSP_SUCCESS;
    capture_finished_at = 0;
    filter_previous_input = 0;
    filter_previous_output = 0;
    memset(capture_statistics, 0, sizeof(capture_statistics));
    for (unsigned index = 0; index < AUDIO_SECONDS; ++index)
    {
        capture_statistics[index].minimum = 8388607;
        capture_statistics[index].maximum = -8388608;
        capture_statistics[index].filtered_minimum = INT32_MAX;
        capture_statistics[index].filtered_maximum = INT32_MIN;
    }
    for (unsigned index = 0; index < 2; ++index)
    {
        capture_buffers[index].before = AUDIO_GUARD;
        capture_buffers[index].after = AUDIO_GUARD;
    }
    if (configure_audio_pin(BSP_IO_PORT_04_PIN_03) != TEST_PASS ||
        configure_audio_pin(BSP_IO_PORT_04_PIN_04) != TEST_PASS ||
        configure_audio_pin(BSP_IO_PORT_04_PIN_06) != TEST_PASS)
    {
        rt_kprintf("MIC pin configuration failed\n");
        return -RT_ERROR;
    }
    clock_config.period_counts = AUDIO_CLOCK_PERIOD;
    clock_config.duty_cycle_counts = AUDIO_CLOCK_PERIOD / 2u;
    clock_config.source_div = TIMER_SOURCE_DIV_1;
    microphone_config.pcm_width = I2S_PCM_WIDTH_24_BITS;
    microphone_config.word_length = I2S_WORD_LENGTH_32_BITS;
    microphone_config.ws_continue = I2S_WS_CONTINUE_OFF;
    microphone_config.p_callback = mic_callback;
    error = R_GPT_Open(&g_timer_ctrl, &clock_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC clock open error=%d\n", error);
        return -RT_ERROR;
    }
    error = R_GPT_Start(&g_timer_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC clock start error=%d\n", error);
        goto clock_close;
    }
    error = R_SSI_Open(&g_i2s0_ctrl, &microphone_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC SSI open error=%d\n", error);
        goto clock_close;
    }
    rt_kprintf("MIC PCM24 slot32; %u frames, %u blocks of %u frames; output mono mu-law\n",
               AUDIO_FRAMES, AUDIO_BLOCK_COUNT, AUDIO_BLOCK_FRAMES);
    rt_kprintf("MIC filter: high-pass ~40Hz at PCM24; storage divisor=%d\n", AUDIO_STORAGE_DIVISOR);
    rt_kprintf("MIC RECORD NOW: 5 seconds; speak continuously until RECORD DONE\n");
    start = rt_tick_get();
    capture_running = 1;
    capture_read_error = R_SSI_Read(&g_i2s0_ctrl, capture_buffers[0].samples, AUDIO_BLOCK_BYTES);
    if (capture_read_error == FSP_SUCCESS)
    {
        while (capture_consumed < AUDIO_BLOCK_COUNT && capture_error == 0)
        {
            if (test_cancelled())
            {
                result = -RT_EINTR;
                break;
            }
            if (test_elapsed(start, AUDIO_TIMEOUT_MS))
            {
                result = -RT_ETIMEOUT;
                break;
            }
            if (capture_consumed < capture_completed)
            {
                consume_block(destination, capture_consumed);
            }
            else
            {
                rt_thread_mdelay(1);
            }
        }
        if (capture_error != 0)
        {
            result = capture_error;
        }
        else if (capture_consumed == AUDIO_BLOCK_COUNT)
        {
            result = TEST_PASS;
        }
    }
    if (!capture_stop_requested)
    {
        stop_capture();
    }
    if (capture_stop_error != FSP_SUCCESS)
    {
        result = -RT_ERROR;
    }
    else if (wait_capture_idle() != TEST_PASS)
    {
        result = -RT_ETIMEOUT;
    }
    unsigned elapsed_ms = (unsigned)((rt_tick_get() - start) * 1000u / RT_TICK_PER_SECOND);
    if (capture_completed == AUDIO_BLOCK_COUNT)
    {
        elapsed_ms = (unsigned)((capture_finished_at - start) * 1000u / RT_TICK_PER_SECOND);
    }
    error = R_SSI_Close(&g_i2s0_ctrl);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MIC SSI close error=%d\n", error);
        result = -RT_ERROR;
    }
    rt_kprintf("MIC RECORD DONE: frames=%u/%u blocks=%u/%u elapsed=%u ms\n",
               capture_consumed * AUDIO_BLOCK_FRAMES, AUDIO_FRAMES,
               capture_completed, AUDIO_BLOCK_COUNT,
               elapsed_ms);
    rt_kprintf("MIC errors: read=%d overrun=%u early_idle=%u stop=%d idle=%u result=%d\n",
               capture_read_error, capture_overruns, capture_early_idle,
               capture_stop_error, capture_idle_events, result);
clock_close:
    if (close_timer("MIC", &g_timer_ctrl) != TEST_PASS)
    {
        result = -RT_ERROR;
    }
    for (unsigned index = 0; index < 2; ++index)
    {
        if (capture_buffers[index].before != AUDIO_GUARD ||
            capture_buffers[index].after != AUDIO_GUARD)
        {
            rt_kprintf("MIC DMA buffer guard corrupted\n");
            result = -RT_ERROR;
        }
    }
    return result;
}

/* 先打印逐秒原始及滤波统计，再计算播放幅度。全部 5 秒保留，不丢弃启动段。
 * 用后 4 秒的高通信号估计播放峰值，避免慢漂移压低真正的语音。
 * 增益最多 64 倍，最终峰值仍限在参考音曾验收过的 3000，底噪也可能放大。 */
static void prepare_playback(const uint8_t *samples)
{
    for (unsigned second = 0; second < AUDIO_SECONDS; ++second)
    {
        const struct capture_statistics *statistics = &capture_statistics[second];
        int mean = (int)(statistics->sum / statistics->frames);
        rt_kprintf("MIC second=%u frames=%u L24 min=%d max=%d mean=%d\n",
                   second + 1u, statistics->frames, statistics->minimum, statistics->maximum, mean);
        rt_kprintf("MIC second=%u changed_in_blocks=%u near_full=%u R24_peak=%u\n",
                   second + 1u, statistics->changed, statistics->near_full, statistics->right_peak);
        unsigned mean_absolute = (unsigned)(statistics->filtered_absolute_sum / statistics->frames);
        rt_kprintf("MIC HP24 second=%u min=%d max=%d mean_abs=%u storage_clipped=%u\n",
                   second + 1u, statistics->filtered_minimum, statistics->filtered_maximum,
                   mean_absolute, statistics->storage_clipped);
    }
    int64_t sum = 0;
    for (unsigned index = AUDIO_SECOND_FRAMES; index < AUDIO_FRAMES; ++index)
    {
        sum += decode_mulaw(samples[index]);
    }
    playback_mean = (int)(sum / (AUDIO_FRAMES - AUDIO_SECOND_FRAMES));
    int peak = 0;
    for (unsigned index = AUDIO_SECOND_FRAMES; index < AUDIO_FRAMES; ++index)
    {
        int magnitude = decode_mulaw(samples[index]) - playback_mean;
        if (magnitude < 0)
        {
            magnitude = -magnitude;
        }
        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }
    playback_gain = 0;
    if (peak > 0)
    {
        playback_gain = AUDIO_OUTPUT_LIMIT * AUDIO_GAIN_SCALE / peak;
        if (playback_gain > AUDIO_MAX_GAIN)
        {
            playback_gain = AUDIO_MAX_GAIN;
        }
    }
    rt_kprintf("PCM mu-law mono bytes=%u; stable_mean=%d stable_peak=%d gain_q8=%d\n",
               AUDIO_FRAMES, playback_mean, peak, playback_gain);
    rt_kprintf("PCM output limited to +/-3000; silence/noise does not prove a recorded voice\n");
}

/* 逐样本解码、去直流、有限增益、限幅，再做首尾 10ms 淡入淡出。 */
static int playback_sample(unsigned index)
{
    int sample = (decode_mulaw(recording[index]) - playback_mean) * playback_gain / AUDIO_GAIN_SCALE;
    if (sample > AUDIO_OUTPUT_LIMIT)
    {
        sample = AUDIO_OUTPUT_LIMIT;
        ++playback_limited;
    }
    if (sample < -AUDIO_OUTPUT_LIMIT)
    {
        sample = -AUDIO_OUTPUT_LIMIT;
        ++playback_limited;
    }
    if (index < AUDIO_FADE_FRAMES)
    {
        sample = sample * (int)index / (int)AUDIO_FADE_FRAMES;
    }
    unsigned remaining = AUDIO_FRAMES - 1u - index;
    if (remaining < AUDIO_FADE_FRAMES)
    {
        sample = sample * (int)remaining / (int)AUDIO_FADE_FRAMES;
    }
    return sample;
}

/* 录音已关闭，GPT2 中断只访问不可变的录音缓冲，不打印或分配内存。 */
static void audio_tick(timer_callback_args_t *arguments)
{
    int sample = 0;
    RT_UNUSED(arguments);
    if (playback_position < AUDIO_FRAMES)
    {
        sample = playback_sample(playback_position);
        ++playback_position;
    }
    int offset = sample * AUDIO_PWM_CENTER / 32768;
    unsigned duty_a = (unsigned)(AUDIO_PWM_CENTER + offset);
    if (duty_a < playback_duty_min)
    {
        playback_duty_min = duty_a;
    }
    if (duty_a > playback_duty_max)
    {
        playback_duty_max = duty_a;
    }
    fsp_err_t error = R_GPT_DutyCycleSet(&g_timer6_ctrl, duty_a, GPT_IO_PIN_GTIOCA);
    if (error != FSP_SUCCESS)
    {
        playback_error = error;
    }
    error = R_GPT_DutyCycleSet(&g_timer6_ctrl, AUDIO_PWM_CENTER - offset, GPT_IO_PIN_GTIOCB);
    if (error != FSP_SUCCESS)
    {
        playback_error = error;
    }
}

/* 同一 80128 帧以相同采样率回放一次，约 5 秒后停止采样中断和 PWM。 */
static int play_audio(const uint8_t *samples)
{
    timer_cfg_t pwm_config = g_timer6_cfg;
    timer_cfg_t sample_config = g_timer2_cfg;
    int result = -RT_ERROR;
    fsp_err_t error;
    rt_tick_t start = rt_tick_get();
    recording = samples;
    playback_position = 0;
    playback_error = FSP_SUCCESS;
    playback_limited = 0;
    playback_duty_min = AUDIO_PWM_CENTER;
    playback_duty_max = AUDIO_PWM_CENTER;
    if (test_cancelled())
    {
        result = -RT_EINTR;
        goto pins;
    }
    if (configure_audio_pin(BSP_IO_PORT_07_PIN_02) != TEST_PASS ||
        configure_audio_pin(BSP_IO_PORT_07_PIN_03) != TEST_PASS)
    {
        rt_kprintf("AUDIO pin configuration failed\n");
        goto pins;
    }
    pwm_config.source_div = TIMER_SOURCE_DIV_1;
    pwm_config.mode = TIMER_MODE_PERIODIC;
    pwm_config.period_counts = AUDIO_PWM_PERIOD;
    pwm_config.duty_cycle_counts = AUDIO_PWM_CENTER;
    sample_config.source_div = TIMER_SOURCE_DIV_1;
    sample_config.mode = TIMER_MODE_PERIODIC;
    sample_config.period_counts = AUDIO_SAMPLE_PERIOD;
    sample_config.duty_cycle_counts = AUDIO_SAMPLE_PERIOD / 2u;
    sample_config.p_callback = audio_tick;
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
        goto pwm_close;
    }
    error = R_GPT_Open(&g_timer2_ctrl, &sample_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("AUDIO sample timer open error=%d\n", error);
        goto pwm_close;
    }
    rt_kprintf("AUDIO PLAY NOW: replaying the recorded 5 seconds\n");
    start = rt_tick_get();
    error = R_GPT_Start(&g_timer2_ctrl);
    if (error == FSP_SUCCESS)
    {
        while (playback_position < AUDIO_FRAMES && playback_error == FSP_SUCCESS &&
               !test_cancelled() && !test_elapsed(start, AUDIO_TIMEOUT_MS))
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
        else if (playback_position == AUDIO_FRAMES)
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
    if (close_timer("sample", &g_timer2_ctrl) != TEST_PASS)
    {
        result = -RT_ERROR;
    }
pwm_close:
    if (close_timer("PWM", &g_timer6_ctrl) != TEST_PASS)
    {
        result = -RT_ERROR;
    }
pins:
    if (mute_output_pins() != TEST_PASS)
    {
        result = -RT_ERROR;
    }
    recording = NULL;
    rt_kprintf("AUDIO PLAY DONE: samples=%u/%u elapsed=%u ms pwm_error=%d limited=%u\n",
               playback_position, AUDIO_FRAMES,
               (unsigned)((rt_tick_get() - start) * 1000u / RT_TICK_PER_SECOND),
               playback_error, playback_limited);
    rt_kprintf("AUDIO duty_A=%u..%u result=%d; listening confirmation required\n",
               playback_duty_min, playback_duty_max, result);
    return result;
}

/* 倒计时可被停止命令打断；采集期间不播放提示音，以便独立观察麦克风。 */
static int run_test(void)
{
    if (R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKD) != AUDIO_CLOCK_HZ ||
        AUDIO_FRAMES != 80128u || AUDIO_FRAMES % AUDIO_BLOCK_FRAMES != 0)
    {
        rt_kprintf("REPLAY clock or sample-count configuration unsupported\n");
        return -RT_ERROR;
    }
    int result = mute_output_pins();
    if (result != TEST_PASS)
    {
        return result;
    }
    uint32_t *allocation = rt_malloc(AUDIO_FRAMES + 2u * sizeof(uint32_t));
    if (allocation == RT_NULL)
    {
        rt_kprintf("REPLAY allocation failed: need %u bytes\n", AUDIO_FRAMES + 8u);
        return -RT_ENOMEM;
    }
    uint8_t *samples = (uint8_t *)(allocation + 1);
    uint32_t *tail = (uint32_t *)(samples + AUDIO_FRAMES);
    allocation[0] = AUDIO_GUARD;
    *tail = AUDIO_GUARD;
    rt_kprintf("REPLAY v5: independent MIC 5s -> recorded playback 5s; PCM24 high-pass / mu-law\n");
    for (unsigned seconds = 3; seconds > 0; --seconds)
    {
        rt_kprintf("MIC recording starts in %u...\n", seconds);
        rt_tick_t start = rt_tick_get();
        while (!test_elapsed(start, 1000))
        {
            if (test_cancelled())
            {
                result = -RT_EINTR;
                goto done;
            }
            rt_thread_mdelay(10);
        }
    }
    result = capture_audio(samples);
    if (allocation[0] != AUDIO_GUARD || *tail != AUDIO_GUARD)
    {
        result = -RT_ERROR;
    }
    if (result == TEST_PASS)
    {
        prepare_playback(samples);
        result = play_audio(samples);
    }
done:
    if (allocation[0] != AUDIO_GUARD || *tail != AUDIO_GUARD)
    {
        rt_kprintf("REPLAY recording buffer guard corrupted\n");
        result = -RT_ERROR;
    }
    rt_free(allocation);
    return result;
}

/* 本文件唯一线程入口；总入口负责创建线程与防止例程重叠。 */
void test_audio_replay_thread(void *argument)
{
    RT_UNUSED(argument);
    int result = run_test();
    if (test_cancelled())
    {
        result = -RT_EINTR;
    }
    rt_thread_self()->error = result;
}
