/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-audio-record.c
 * @brief 独立麦克风录音：预热1秒，保存5秒PCM，再打印导出给电脑。
 *
 * SSI接收约48kHz、24位双声道；U9接地选择左声道。
 * 127点低通后六抽一，保存约8kHz、16位单声道，共80128字节。
 * 只做固定除以32的位宽换算，不做高通、自动增益、mu-law或板端播放。
 * 电脑脚本校验导出后生成WAV和MP3；能听清人声才算声学验收通过。
 * 一个线程独立完成初始化、采集、导出和清理，不调用其他测试。
 * 阅读顺序：线程入口 → run_test → capture_audio → consume_block → export_raw。
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

/* 与回放使用相同的原始BSP时钟和SSI格式，便于对照输入内容。
 * 每块384帧约8ms；前125块预热约1秒，随后626块保存约5秒。 */
#define AUDIO_CLOCK_HZ 120000000u
#define AUDIO_CLOCK_PERIOD 39u
#define AUDIO_CAPTURE_PERIOD (AUDIO_CLOCK_PERIOD * 64u)
#define AUDIO_CAPTURE_BLOCK_FRAMES 384u
#define AUDIO_WARMUP_BLOCKS 125u
#define AUDIO_RECORD_BLOCKS 626u
#define AUDIO_BLOCK_COUNT (AUDIO_WARMUP_BLOCKS + AUDIO_RECORD_BLOCKS)
#define AUDIO_CAPTURE_FRAMES (AUDIO_BLOCK_COUNT * AUDIO_CAPTURE_BLOCK_FRAMES)
#define AUDIO_CHANNELS 2u
#define AUDIO_BUFFER_COUNT 2u
#define AUDIO_CAPTURE_PRIORITY 14u
#define AUDIO_BLOCK_BYTES (AUDIO_CAPTURE_BLOCK_FRAMES * AUDIO_CHANNELS * sizeof(uint32_t))
#define AUDIO_SECONDS 5u
#define AUDIO_CAPTURE_SECOND_FRAMES 48077u
#define AUDIO_TIMEOUT_MS 7500u
#define AUDIO_GUARD 0x51A7C0DEu
#define RAW_START_FRAME (AUDIO_WARMUP_BLOCKS * AUDIO_CAPTURE_BLOCK_FRAMES)
#define AUDIO_DECIMATION 6u
#define AUDIO_STORAGE_DIVISOR 32
#define RAW_FRAMES (AUDIO_RECORD_BLOCKS * AUDIO_CAPTURE_BLOCK_FRAMES / AUDIO_DECIMATION)
#define RAW_BYTES (RAW_FRAMES * 2u)
#define AUDIO_LOWPASS_TAPS 127u
#define AUDIO_LOWPASS_SCALE 32768
#define RAW_ROW_BYTES 32u

/* 系数和为32768，直流增益为1；127点Blackman窗低通，截止约3kHz。
 * 低通属于降采样所需的抗混叠处理，不用于去噪或判定有没有人声。 */
static const int lowpass_coefficients[AUDIO_LOWPASS_TAPS] =
{
    0, 0, 0, 0, -1, -1, -1, 0, 1, 3, 4, 6,
    7, 6, 4, 0, -6, -13, -20, -24, -26, -22, -13, 1,
    19, 39, 57, 69, 70, 59, 35, -2, -48, -95, -135, -161,
    -163, -136, -79, 4, 103, 204, 291, 343, 346, 289, 169, -5,
    -216, -434, -625, -749, -771, -661, -403, 6, 548, 1189, 1881, 2565,
    3180, 3668, 3981, 4094, 3981, 3668, 3180, 2565, 1881, 1189, 548, 6,
    -403, -661, -771, -749, -625, -434, -216, -5, 169, 289, 346, 343,
    291, 204, 103, 4, -79, -136, -163, -161, -135, -95, -48, -2,
    35, 59, 70, 69, 57, 39, 19, 1, -13, -22, -26, -24,
    -20, -13, -6, 0, 4, 6, 7, 6, 4, 3, 1, 0,
    -1, -1, -1, 0, 0, 0, 0
};
static int lowpass_history[AUDIO_LOWPASS_TAPS];
static unsigned lowpass_position;
static unsigned storage_clipped;

/* 一块由DTC写入、一块由线程快速复制；满队列立即报错，不覆盖旧数据。
 * 缓冲按需从堆申请，避免新例程永久占用其他例程的RAM。 */
struct capture_buffer
{
    uint32_t before;
    uint32_t samples[AUDIO_CAPTURE_BLOCK_FRAMES * AUDIO_CHANNELS];
    uint32_t after;
};
static struct capture_buffer *capture_buffers;
static struct rt_semaphore capture_signal;

/* 原始统计只用于观察稳定性，不把数据变化自动判定为人声。 */
struct capture_statistics
{
    int minimum;
    int maximum;
    int64_t sum;
    unsigned frames;
    unsigned right_peak;
    unsigned upper_nonzero;
};
static struct capture_statistics capture_statistics[AUDIO_SECONDS];
static unsigned raw_stored;
static uint32_t raw_preview[8][2];
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
static volatile unsigned capture_pending_peak;
static volatile uint32_t capture_ready_cycles[AUDIO_BUFFER_COUNT];
static uint32_t capture_process_max_cycles;
static uint32_t capture_wait_max_cycles;
static uint64_t capture_process_total_cycles;

/* 右对齐的24位数据显式扩展符号；高8位另行统计，不混入音频值。 */
static int pcm24_signed(uint32_t word)
{
    int value = (int)(word & 0x00FFFFFFu);
    if ((word & 0x00800000u) != 0)
    {
        value -= 0x01000000;
    }
    return value;
}

/* 正常录满与错误退出都请求停止，SSI关闭留在线程内完成。 */
static void stop_capture(void)
{
    capture_running = 0;
    capture_stop_requested = 1;
    capture_stop_error = R_SSI_Stop(&g_i2s0_ctrl);
}

/* 回调只交接缓冲并唤醒线程，采集期间不打印或转换音频。 */
static void mic_callback(i2s_callback_args_t *arguments)
{
    /* FSP 的 SSI ISR 未调用 RT-Thread 中断进出接口；使用内核 IPC 前补上。 */
    rt_interrupt_enter();
    if (arguments->event == I2S_EVENT_RX_FULL && capture_running)
    {
        __DMB();
        ++capture_completed;
        unsigned pending = capture_completed - capture_consumed;
        if (pending > capture_pending_peak)
        {
            capture_pending_peak = pending;
        }
        capture_ready_cycles[(capture_completed - 1u) % AUDIO_BUFFER_COUNT] = DWT->CYCCNT;
        if (capture_completed == AUDIO_BLOCK_COUNT)
        {
            capture_finished_at = rt_tick_get();
            stop_capture();
            goto notify;
        }
        if (pending >= AUDIO_BUFFER_COUNT)
        {
            ++capture_overruns;
            capture_error = -RT_EFULL;
            stop_capture();
            goto notify;
        }
        unsigned next = capture_completed % AUDIO_BUFFER_COUNT;
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
notify:
    /* 先提交下一块接收，再唤醒线程；中断中只交接所有权，不处理音频。 */
    rt_sem_release(&capture_signal);
    rt_interrupt_leave();
}

/* 保存全部原始帧的滤波历史；每六帧只计算一次输出。
 * 用64位累加，避免24位样本乘定点系数时溢出。预热期也更新历史。
 * 这里只做抗混叠低通；左右原始统计与预览在滤波前完成。 */
#if defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize ("O2")
#endif
static int downsample_pcm24(int left, unsigned frame, int *output)
{
    lowpass_history[lowpass_position] = left;
    ++lowpass_position;
    if (lowpass_position == AUDIO_LOWPASS_TAPS)
    {
        lowpass_position = 0;
    }
    if (frame % AUDIO_DECIMATION != AUDIO_DECIMATION - 1u)
    {
        return 0;
    }
    int64_t sum = 0;
    unsigned position = lowpass_position;
    for (unsigned tap = 0; tap < AUDIO_LOWPASS_TAPS; ++tap)
    {
        if (position == 0)
        {
            position = AUDIO_LOWPASS_TAPS;
        }
        --position;
        sum += (int64_t)lowpass_history[position] * lowpass_coefficients[tap];
    }
    *output = (int)(sum / AUDIO_LOWPASS_SCALE);
    return 1;
}

/* 前1秒只预热，不保存；随后5秒完整保存，不再只选半秒片段。
 * PCM24固定除以32后限幅为PCM16，相当于保留比常规除以256多3位的弱输入。
 * 不依据本轮峰值改变比例；限幅次数单独打印，不能把限幅当作发声。 */
static void consume_block(uint8_t *destination, unsigned block)
{
    const uint32_t *samples = capture_buffers[block % AUDIO_BUFFER_COUNT].samples;
    unsigned first = block * AUDIO_CAPTURE_BLOCK_FRAMES;
    for (unsigned offset = 0; offset < AUDIO_CAPTURE_BLOCK_FRAMES; ++offset)
    {
        unsigned frame = first + offset;
        uint32_t word = samples[offset * AUDIO_CHANNELS];
        int left = pcm24_signed(word);
        int right = pcm24_signed(samples[offset * AUDIO_CHANNELS + 1u]);
        int filtered = 0;
        int output_ready = downsample_pcm24(left, frame, &filtered);
        if (frame < RAW_START_FRAME)
        {
            continue;
        }
        unsigned record_frame = frame - RAW_START_FRAME;
        struct capture_statistics *statistics = &capture_statistics[record_frame / AUDIO_CAPTURE_SECOND_FRAMES];
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
        if ((word & 0xFF000000u) != 0)
        {
            ++statistics->upper_nonzero;
        }
        if (right < 0)
        {
            right = -right;
        }
        if ((unsigned)right > statistics->right_peak)
        {
            statistics->right_peak = (unsigned)right;
        }
        if (record_frame < 8u)
        {
            raw_preview[record_frame][0] = word;
            raw_preview[record_frame][1] = samples[offset * AUDIO_CHANNELS + 1u];
        }
        if (output_ready)
        {
            int value = filtered / AUDIO_STORAGE_DIVISOR;
            if (value > 32767)
            {
                value = 32767;
                ++storage_clipped;
            }
            else if (value < -32768)
            {
                value = -32768;
                ++storage_clipped;
            }
            unsigned encoded = (unsigned)(uint16_t)value;
            destination[raw_stored * 2u] = (uint8_t)encoded;
            destination[raw_stored * 2u + 1u] = (uint8_t)(encoded >> 8u);
            ++raw_stored;
        }
    }
    /* 读取完成后才允许中断复用本块。 */
    __DMB();
    ++capture_consumed;
}
#if defined(__GNUC__)
#pragma GCC pop_options
#endif

/* 停止后最多等20ms让SSI完成帧结束；取消不能跳过清理。 */
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

/* 已打开的GPT先停止再关闭，两次返回值都检查。 */
static int close_timer(const char *label, gpt_instance_ctrl_t *control)
{
    int result = TEST_PASS;
    fsp_err_t error = R_GPT_Stop(control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MICREC %s timer stop error=%d\n", label, error);
        result = -RT_ERROR;
    }
    error = R_GPT_Close(control);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("MICREC %s timer close error=%d\n", label, error);
        result = -RT_ERROR;
    }
    return result;
}

/* 连续采集约6秒，前1秒预热、后5秒保存；SSI关闭后才允许导出。 */
static int capture_samples(uint8_t *destination)
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
    capture_pending_peak = 0;
    capture_process_max_cycles = 0;
    capture_wait_max_cycles = 0;
    capture_process_total_cycles = 0;
    memset((void *)capture_ready_cycles, 0, sizeof(capture_ready_cycles));
    raw_stored = 0;
    storage_clipped = 0;
    lowpass_position = 0;
    memset(lowpass_history, 0, sizeof(lowpass_history));
    memset(raw_preview, 0, sizeof(raw_preview));
    memset(capture_statistics, 0, sizeof(capture_statistics));
    for (unsigned index = 0; index < AUDIO_SECONDS; ++index)
    {
        capture_statistics[index].minimum = 8388607;
        capture_statistics[index].maximum = -8388608;
    }
    for (unsigned index = 0; index < AUDIO_BUFFER_COUNT; ++index)
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
    rt_kprintf("MIC BCLK=%u Hz rate~48077; GPT1 period=%u (original BSP clock)\n",
               AUDIO_CLOCK_HZ / AUDIO_CLOCK_PERIOD, AUDIO_CLOCK_PERIOD);
    rt_kprintf("MIC PCM24 slot32: source_frames=%u; warmup_frames=%u; PCM16_frames=%u\n",
               AUDIO_CAPTURE_FRAMES, RAW_START_FRAME, RAW_FRAMES);
    rt_kprintf("MIC queue: buffers=%u block_period_us=%u capture_priority=%u\n",
               AUDIO_BUFFER_COUNT, AUDIO_CAPTURE_BLOCK_FRAMES * AUDIO_CAPTURE_PERIOD / 120u,
               AUDIO_CAPTURE_PRIORITY);
    rt_kprintf("MIC RECORD NOW: 6 seconds; first 1s warmup, then save 5s; keep speaking\n");
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
                uint32_t work_start = DWT->CYCCNT;
                uint32_t waiting = work_start - capture_ready_cycles[capture_consumed % AUDIO_BUFFER_COUNT];
                if (waiting > capture_wait_max_cycles)
                {
                    capture_wait_max_cycles = waiting;
                }
                consume_block(destination, capture_consumed);
                uint32_t processing = DWT->CYCCNT - work_start;
                capture_process_total_cycles += processing;
                if (processing > capture_process_max_cycles)
                {
                    capture_process_max_cycles = processing;
                }
            }
            else
            {
                /* 完成中断立即唤醒；10ms 上限保证无中断时仍能检查停止/超时。 */
                rt_sem_take(&capture_signal, rt_tick_from_millisecond(10));
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
               capture_consumed * AUDIO_CAPTURE_BLOCK_FRAMES, AUDIO_CAPTURE_FRAMES,
               capture_completed, AUDIO_BLOCK_COUNT,
               elapsed_ms);
    rt_kprintf("MIC errors: read=%d overrun=%u early_idle=%u stop=%d idle=%u result=%d\n",
               capture_read_error, capture_overruns, capture_early_idle,
               capture_stop_error, capture_idle_events, result);
    unsigned cycles_per_us = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK) / 1000000u;
    unsigned average_us = 0;
    if (capture_consumed != 0)
    {
        average_us = (unsigned)(capture_process_total_cycles / capture_consumed / cycles_per_us);
    }
    rt_kprintf("MIC timing: process_max_us=%u avg_us=%u ready_wait_max_us=%u\n",
               capture_process_max_cycles / cycles_per_us, average_us,
               capture_wait_max_cycles / cycles_per_us);
    rt_kprintf("MIC queue peak=%u/%u (includes processing block)\n",
               capture_pending_peak, AUDIO_BUFFER_COUNT);
clock_close:
    if (close_timer("MIC", &g_timer_ctrl) != TEST_PASS)
    {
        result = -RT_ERROR;
    }
    for (unsigned index = 0; index < AUDIO_BUFFER_COUNT; ++index)
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

/* 采集阶段临时提高当前线程优先级；清理后恢复优先级和释放信号量。 */
static int capture_audio(uint8_t *destination)
{
    rt_thread_t thread = rt_thread_self();
    rt_uint8_t original_priority = thread->current_priority;
    rt_uint8_t capture_priority = AUDIO_CAPTURE_PRIORITY;
    if (rt_sem_init(&capture_signal, "micrx", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (rt_thread_control(thread, RT_THREAD_CTRL_CHANGE_PRIORITY, &capture_priority) != RT_EOK)
    {
        rt_sem_detach(&capture_signal);
        return -RT_ERROR;
    }
    /* 保留原计数值，使用无符号差值处理回绕；计时包含处理中的抢占时间。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    int result = capture_samples(destination);
    if (rt_thread_control(thread, RT_THREAD_CTRL_CHANGE_PRIORITY, &original_priority) != RT_EOK)
    {
        result = -RT_ERROR;
    }
    if (rt_sem_detach(&capture_signal) != RT_EOK)
    {
        result = -RT_ERROR;
    }
    return result;
}

/* 标准CRC-32/IEEE，初始和末尾均取反；电脑端用zlib.crc32核对。
 * 校验只在采集硬件关闭后执行，不增加接收中断或块处理负担。 */
static uint32_t raw_crc32(const uint8_t *data, unsigned bytes)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned index = 0; index < bytes; ++index)
    {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8u; ++bit)
        {
            if ((crc & 1u) != 0)
            {
                crc = (crc >> 1u) ^ 0xEDB88320u;
            }
            else
            {
                crc >>= 1u;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* 打印的是已保存的PCM16字节；每行带偏移，BEGIN/END带长度和CRC。
 * 漏行、错序或截断不能生成有效WAV。
 * 一行不超过控制台128字节缓冲；导出期间不操作其他外设，也不播放音频。 */
static int export_raw(const uint8_t *data)
{
    static const char hex_digits[] = "0123456789ABCDEF";
    uint32_t checksum = raw_crc32(data, RAW_BYTES);
    rt_kprintf("MICREC BEGIN v=1 rate=8013 frames=%u bits=16 channels=1 bytes=%u crc32=%08X\n",
               RAW_FRAMES, RAW_BYTES, (unsigned)checksum);
    for (unsigned offset = 0; offset < RAW_BYTES; offset += RAW_ROW_BYTES)
    {
        if (test_cancelled())
        {
            rt_kprintf("MICREC ABORT offset=%u\n", offset);
            return -RT_EINTR;
        }
        unsigned count = RAW_BYTES - offset;
        if (count > RAW_ROW_BYTES)
        {
            count = RAW_ROW_BYTES;
        }
        char line[RAW_ROW_BYTES * 2u + 1u];
        for (unsigned index = 0; index < count; ++index)
        {
            unsigned value = data[offset + index];
            line[index * 2u] = hex_digits[value >> 4u];
            line[index * 2u + 1u] = hex_digits[value & 0x0Fu];
        }
        line[count * 2u] = '\0';
        rt_kprintf("MICREC DATA %u %s\n", offset, line);
        if (offset % (RAW_ROW_BYTES * 16u) == 0)
        {
            rt_thread_mdelay(1);
        }
    }
    rt_kprintf("MICREC END bytes=%u crc32=%08X\n", RAW_BYTES, (unsigned)checksum);
    return TEST_WAIT;
}

/* 录音80128字节加8字节哨兵，双DMA缓冲6160字节，合计86296字节。
 * 按需申请，退出全部释放；没有SD卡或USB依赖，也不永久保留录音。 */
static int run_test(void)
{
    if (R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKD) != AUDIO_CLOCK_HZ)
    {
        rt_kprintf("MICREC clock unsupported\n");
        return -RT_ERROR;
    }
    int result = mute_output_pins();
    if (result != TEST_PASS)
    {
        return result;
    }
    uint32_t *allocation = rt_malloc(RAW_BYTES + 8u);
    if (allocation == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    capture_buffers = rt_malloc(AUDIO_BUFFER_COUNT * sizeof(struct capture_buffer));
    if (capture_buffers == RT_NULL)
    {
        rt_free(allocation);
        return -RT_ENOMEM;
    }
    uint8_t *data = (uint8_t *)(allocation + 1);
    uint32_t *tail = (uint32_t *)(data + RAW_BYTES);
    allocation[0] = AUDIO_GUARD;
    *tail = AUDIO_GUARD;
    rt_kprintf("MICREC v1: warmup 1s + record 5s -> print PCM16, no speaker playback\n");
    rt_kprintf("MICREC LPF 3kHz -> decimate 6 -> divide 32 -> PCM16; no automatic gain\n");
    for (unsigned seconds = 3; seconds > 0; --seconds)
    {
        rt_kprintf("MIC recording starts in %u...\n", seconds);
        rt_tick_t start = rt_tick_get();
        while (!test_elapsed(start, 1000))
        {
            if (test_cancelled())
            {
                result = -RT_EINTR;
                goto release;
            }
            rt_thread_mdelay(10);
        }
    }
    result = capture_audio(data);
    if (allocation[0] != AUDIO_GUARD || *tail != AUDIO_GUARD)
    {
        rt_kprintf("MICREC snapshot guard corrupted\n");
        result = -RT_ERROR;
    }
    if (result == TEST_PASS && raw_stored != RAW_FRAMES)
    {
        rt_kprintf("MICREC incomplete snapshot=%u/%u\n", raw_stored, RAW_FRAMES);
        result = -RT_ERROR;
    }
    if (result == TEST_PASS)
    {
        for (unsigned second = 0; second < AUDIO_SECONDS; ++second)
        {
            const struct capture_statistics *statistics = &capture_statistics[second];
            rt_kprintf("MIC INPUT second=%u n=%u min=%d max=%d mean=%d\n",
                       second + 1u, statistics->frames, statistics->minimum,
                       statistics->maximum, (int)(statistics->sum / statistics->frames));
            rt_kprintf("MIC INPUT second=%u R24_peak=%u L_upper_nonzero=%u\n",
                       second + 1u, statistics->right_peak, statistics->upper_nonzero);
        }
        for (unsigned index = 0; index < 8u; ++index)
        {
            rt_kprintf("MIC INPUT preview=%u L32=%08X R32=%08X L24=%d\n", index,
                       (unsigned)raw_preview[index][0], (unsigned)raw_preview[index][1],
                       pcm24_signed(raw_preview[index][0]));
        }
        rt_kprintf("MICREC exporting after SSI/GPT closed; wait about 21s at 115200 baud\n");
        rt_kprintf("MICREC PCM16 samples=%u/%u bytes=%u storage_clipped=%u\n",
                   raw_stored, RAW_FRAMES, RAW_BYTES, storage_clipped);
        result = export_raw(data);
    }
release:
    rt_free(capture_buffers);
    capture_buffers = RT_NULL;
    rt_free(allocation);
    return result;
}

/* 本文件唯一线程入口；命令检查和线程创建由总入口负责。 */
void test_audio_record_thread(void *argument)
{
    RT_UNUSED(argument);
    int result = run_test();
    if (test_cancelled())
    {
        result = -RT_EINTR;
    }
    rt_thread_self()->error = result;
}
