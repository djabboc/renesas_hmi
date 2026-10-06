"""执行真实五秒录音回放 C 函数，验证算法、连续分块和完整生命周期。

使用 ARM 模拟器与 FSP 桩，不访问串口；模拟验证不能代替 SSI/DTC 实物采样与试听。
python scripts/validate_audio_replay.py --dependencies logs/oled-validation/python
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def extract_function(source: str, name: str) -> str:
    match = re.search(r"static (?:int|void|unsigned) " + name + r"\([^)]*\)\s*\{", source)
    if match is None:
        raise ValueError(f"Missing C function: {name}")
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[match.start():end]


STUB = r'''#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#define RT_UNUSED(value) (void)(value)
#define RT_NULL NULL
#define RT_ERROR 1
#define RT_EINTR 9
#define RT_EFULL 3
#define RT_ENOMEM 12
#define RT_ETIMEOUT 2
#define RT_TICK_PER_SECOND 1000
#define RT_EOK 0
#define RT_IPC_FLAG_FIFO 0
#define RT_THREAD_CTRL_CHANGE_PRIORITY 1
#define TEST_PASS 0
#define TEST_WAIT 1
#define FSP_SUCCESS 0
#define FSP_PRIV_CLOCK_PCLKD 0
#define FSP_PRIV_CLOCK_ICLK 1
#define CoreDebug_DEMCR_TRCENA_Msk (1u << 24)
#define DWT_CTRL_CYCCNTENA_Msk 1u
#define GPT_IO_PIN_GTIOCA 0
#define GPT_IO_PIN_GTIOCB 1
#define TIMER_SOURCE_DIV_1 0
#define TIMER_MODE_PERIODIC 0
#define I2S_EVENT_RX_FULL 2
#define I2S_EVENT_IDLE 0
#define I2S_PCM_WIDTH_24_BITS 24
#define I2S_WORD_LENGTH_32_BITS 32
#define I2S_WS_CONTINUE_OFF 1
#define BSP_IO_PORT_04_PIN_03 0x403
#define BSP_IO_PORT_04_PIN_04 0x404
#define BSP_IO_PORT_04_PIN_06 0x406
#define BSP_IO_PORT_07_PIN_02 0x702
#define BSP_IO_PORT_07_PIN_03 0x703
#define __DMB() __asm volatile ("" ::: "memory")
typedef unsigned rt_tick_t;
typedef uint8_t rt_uint8_t;
struct rt_thread { rt_uint8_t current_priority; };
typedef struct rt_thread *rt_thread_t;
struct rt_semaphore { unsigned active, value; };
static struct rt_thread mock_thread = {21};
static struct { unsigned DEMCR; } mock_debug;
static struct { unsigned CTRL, CYCCNT; } mock_dwt;
#define CoreDebug (&mock_debug)
#define DWT (&mock_dwt)
static unsigned semaphore_active, semaphore_fail, priority_fail, priority_calls, irq_nest, ipc_error, wake_delay;
static void rt_thread_mdelay(unsigned ms);
static unsigned rt_tick_from_millisecond(unsigned ms) { return ms; }
static rt_thread_t rt_thread_self(void) { return &mock_thread; }
static int rt_thread_control(rt_thread_t thread, unsigned command, void *value)
{
    ++priority_calls;
    if (priority_calls == priority_fail) { return -RT_ERROR; }
    thread->current_priority = *(rt_uint8_t *)value;
    return RT_EOK;
}
static void rt_interrupt_enter(void) { ++irq_nest; }
static void rt_interrupt_leave(void) { --irq_nest; }
static int rt_sem_init(struct rt_semaphore *sem, const char *name, unsigned value, unsigned flag)
{
    if (semaphore_fail) { return -RT_ERROR; }
    sem->active = 1; sem->value = value; semaphore_active = 1;
    return RT_EOK;
}
static int rt_sem_release(struct rt_semaphore *sem)
{
    if (!sem->active || irq_nest != 1) { ++ipc_error; }
    ++sem->value; return RT_EOK;
}
static int rt_sem_take(struct rt_semaphore *sem, unsigned timeout)
{
    if (wake_delay) { unsigned delay = wake_delay; wake_delay = 0; rt_thread_mdelay(delay); }
    for (unsigned wait = 0; sem->value == 0 && wait < timeout; ++wait) { rt_thread_mdelay(1); }
    if (sem->value == 0) { return -RT_ETIMEOUT; }
    --sem->value; return RT_EOK;
}
static int rt_sem_detach(struct rt_semaphore *sem)
{
    sem->active = 0; semaphore_active = 0; return RT_EOK;
}
typedef int fsp_err_t;
typedef struct { unsigned event; } i2s_callback_args_t;
typedef struct { unsigned unused; } timer_callback_args_t;
typedef struct { unsigned open, running; } gpt_instance_ctrl_t;
typedef struct
{
    unsigned period_counts, duty_cycle_counts, source_div, mode;
    void (*p_callback)(timer_callback_args_t *);
} timer_cfg_t;
typedef struct { unsigned open; } ssi_instance_ctrl_t;
typedef struct
{
    unsigned pcm_width, word_length, ws_continue;
    void (*p_callback)(i2s_callback_args_t *);
} i2s_cfg_t;
static gpt_instance_ctrl_t g_timer_ctrl, g_timer2_ctrl, g_timer6_ctrl;
static timer_cfg_t g_timer_cfg, g_timer2_cfg, g_timer6_cfg;
static ssi_instance_ctrl_t g_i2s0_ctrl;
static i2s_cfg_t g_i2s0_cfg;
static unsigned clock_ms, cancel_at, failed_operation, operations, force_pwm_error;
static unsigned ssi_reads, fail_read_at, ssi_stops, ssi_closes, ssi_active, ssi_idle_pending;
static unsigned ssi_stop_fail, ssi_no_idle, ssi_freeze, ssi_early_idle, ssi_burst, corrupt_guard;
static unsigned ssi_offset, absolute_frame, capture_phase, playback_phase, freeze_playback;
static unsigned malloc_calls, free_calls, allocation_fail, heap_bytes, max_log_bytes, overlap;
static unsigned close_order[3], timer_closes, pin_error, mute_calls, duties[2];
static unsigned payload[20036]; /* 80144 字节，覆盖录音与哨兵，不用主机 malloc。 */
static uint32_t *ssi_destination;
static void (*ssi_callback)(i2s_callback_args_t *);
static void (*sample_callback)(timer_callback_args_t *);
static int R_SSI_Read(ssi_instance_ctrl_t *ctrl, void *destination, unsigned bytes);
static int R_SSI_Stop(ssi_instance_ctrl_t *ctrl);
static void mock_capture_step(unsigned frames);
static unsigned rt_tick_get(void) { return clock_ms; }
static int test_elapsed(rt_tick_t start, unsigned ms) { return clock_ms - start >= ms; }
static int test_cancelled(void) { return cancel_at != 0 && clock_ms >= cancel_at; }
static unsigned R_FSP_SystemClockHzGet(unsigned clock) { return 120000000; }
static void rt_kprintf(const char *format, ...)
{
    char buffer[128]; va_list args;
    va_start(args, format); int length = vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    if (length > 0 && (unsigned)length > max_log_bytes) { max_log_bytes = (unsigned)length; }
}
static void *rt_malloc(unsigned bytes)
{
    ++malloc_calls; heap_bytes = bytes;
    if (allocation_fail || bytes > sizeof(payload)) { return NULL; }
    memset(payload, 0xA5, sizeof(payload));
    return payload;
}
static void rt_free(void *buffer) { ++free_calls; }
static int configure_audio_pin(unsigned pin) { return pin_error; }
static int mute_output_pins(void) { ++mute_calls; return TEST_PASS; }
static int R_GPT_Open(gpt_instance_ctrl_t *ctrl, const timer_cfg_t *cfg)
{
    ++operations;
    if (operations == failed_operation) { return 7; }
    ctrl->open = 1;
    if (ctrl == &g_timer2_ctrl) { sample_callback = cfg->p_callback; }
    return FSP_SUCCESS;
}
static int R_GPT_Start(gpt_instance_ctrl_t *ctrl)
{
    ++operations;
    if (operations == failed_operation) { return 7; }
    ctrl->running = 1;
    if (ctrl == &g_timer6_ctrl && g_i2s0_ctrl.open) { ++overlap; }
    return FSP_SUCCESS;
}
static int R_GPT_Stop(gpt_instance_ctrl_t *ctrl)
{
    ctrl->running = 0;
    if (ctrl == &g_timer2_ctrl) { sample_callback = NULL; }
    return FSP_SUCCESS;
}
static int R_GPT_Close(gpt_instance_ctrl_t *ctrl)
{
    if (!ctrl->open) { return 7; }
    if (timer_closes < 3)
    {
        close_order[timer_closes] = 1;
        if (ctrl == &g_timer2_ctrl) { close_order[timer_closes] = 2; }
        if (ctrl == &g_timer6_ctrl) { close_order[timer_closes] = 6; }
    }
    ++timer_closes; ctrl->open = 0;
    return FSP_SUCCESS;
}
static int R_GPT_DutyCycleSet(void *ctrl, unsigned duty, unsigned pin)
{
    duties[pin] = duty;
    return force_pwm_error;
}
static int R_SSI_Open(ssi_instance_ctrl_t *ctrl, const i2s_cfg_t *cfg)
{
    ++operations;
    if (operations == failed_operation) { return 7; }
    if (cfg->pcm_width != 24 || cfg->word_length != 32 || cfg->ws_continue != 1) { return 7; }
    if (g_timer6_ctrl.open) { ++overlap; }
    ctrl->open = 1; ssi_callback = cfg->p_callback;
    return FSP_SUCCESS;
}
static int R_SSI_Read(ssi_instance_ctrl_t *ctrl, void *destination, unsigned bytes)
{
    ++ssi_reads;
    if (ssi_reads == fail_read_at) { return 7; }
    if (bytes != 3072 || !ctrl->open) { return 7; }
    ssi_destination = destination; ssi_offset = 0; ssi_active = 1;
    return FSP_SUCCESS;
}
static int R_SSI_Stop(ssi_instance_ctrl_t *ctrl)
{
    ++ssi_stops; ssi_active = 0;
    if (ssi_stop_fail) { return 7; }
    ssi_idle_pending = 1;
    return FSP_SUCCESS;
}
static int R_SSI_Close(ssi_instance_ctrl_t *ctrl)
{
    ctrl->open = 0; ssi_active = 0; ssi_callback = NULL; ssi_idle_pending = 0; ++ssi_closes;
    return FSP_SUCCESS;
}
static void rt_thread_mdelay(unsigned ms)
{
    for (unsigned step = 0; step < ms; ++step)
    {
        ++clock_ms;
        DWT->CYCCNT += 120000;
        if (ssi_idle_pending && !ssi_no_idle && ssi_callback)
        {
            ssi_idle_pending = 0; i2s_callback_args_t event = {I2S_EVENT_IDLE}; ssi_callback(&event);
        }
        capture_phase += 120000;
        unsigned frames = capture_phase / 2496; capture_phase %= 2496;
        if (ssi_burst) { frames = 1536; }
        mock_capture_step(frames);
        playback_phase += 120000;
        frames = playback_phase / 7488; playback_phase %= 7488;
        if (g_timer2_ctrl.running && sample_callback && !freeze_playback)
        {
            for (unsigned index = 0; index < frames; ++index) { sample_callback(NULL); }
        }
    }
}
'''

HARNESS = r'''
unsigned test_error, tests_passed;
#define CHECK(condition) do { if (!(condition)) { test_error = __LINE__; return; } } while (0)
static void mock_capture_step(unsigned frames)
{
    if (ssi_freeze || !ssi_active || !ssi_callback) { return; }
    if (ssi_early_idle)
    {
        ssi_active = 0; i2s_callback_args_t event = {I2S_EVENT_IDLE}; ssi_callback(&event); return;
    }
    for (unsigned index = 0; index < frames && ssi_active; ++index)
    {
        int sample = 1024;
        if (absolute_frame / AUDIO_DECIMATION % 32 >= 16) { sample = -1024; }
        ssi_destination[ssi_offset * 2] = ((uint32_t)(sample * 256)) & 0xFFFFFF;
        ssi_destination[ssi_offset * 2 + 1] = 0;
        ++ssi_offset; ++absolute_frame;
        if (ssi_offset == AUDIO_CAPTURE_BLOCK_FRAMES)
        {
            if (corrupt_guard) { ssi_destination[-1] ^= 1; }
            ssi_active = 0; i2s_callback_args_t event = {I2S_EVENT_RX_FULL}; ssi_callback(&event);
        }
    }
}
static void reset(void)
{
    clock_ms = 0; cancel_at = 0; failed_operation = 0; operations = 0; force_pwm_error = 0;
    ssi_reads = 0; fail_read_at = 0; ssi_stops = 0; ssi_closes = 0; ssi_active = 0; ssi_idle_pending = 0;
    ssi_stop_fail = 0; ssi_no_idle = 0; ssi_freeze = 0; ssi_early_idle = 0; ssi_burst = 0; corrupt_guard = 0;
    ssi_offset = 0; absolute_frame = 0; capture_phase = 0; playback_phase = 0; freeze_playback = 0;
    malloc_calls = 0; free_calls = 0; allocation_fail = 0; overlap = 0;
    timer_closes = 0; pin_error = 0; mute_calls = 0; ssi_callback = NULL; sample_callback = NULL;
    g_timer_ctrl.open = 0; g_timer_ctrl.running = 0;
    g_timer2_ctrl.open = 0; g_timer2_ctrl.running = 0;
    g_timer6_ctrl.open = 0; g_timer6_ctrl.running = 0; g_i2s0_ctrl.open = 0;
    mock_thread.current_priority = 21; semaphore_active = 0; semaphore_fail = 0;
    priority_fail = 0; priority_calls = 0; irq_nest = 0; ipc_error = 0; wake_delay = 0;
    DWT->CYCCNT = 0;
}
static int hardware_closed(void)
{
    return !g_i2s0_ctrl.open && !g_timer_ctrl.open && !g_timer2_ctrl.open &&
           !g_timer6_ctrl.open && !ssi_active && !sample_callback && !semaphore_active &&
           mock_thread.current_priority == 21 && irq_nest == 0 && ipc_error == 0;
}
void validate(void)
{
    CHECK(encode_mulaw(0) == 0xFF && encode_mulaw(-1) == 0x7F);
    CHECK(encode_mulaw(1000) == 0xCE && encode_mulaw(-1000) == 0x4E);
    CHECK(encode_mulaw(32767) == 0x80 && encode_mulaw(-32768) == 0x00);
    CHECK(decode_mulaw(0xFF) == 0 && decode_mulaw(0x7F) == 0);
    CHECK(decode_mulaw(0xCE) == 988 && decode_mulaw(0x4E) == -988);
    for (unsigned code = 0; code < 256; ++code)
    {
        if (code != 0x7F) { CHECK(encode_mulaw(decode_mulaw(code)) == code); }
    }
    ++tests_passed;
    CHECK(pcm24_signed(0x007FFFFF) == 8388607 && pcm24_signed(0x00800000) == -8388608);
    CHECK(pcm24_signed(0xFFFFFFFF) == -1 && pcm24_signed(0xFF7FFFFF) == 8388607);
    CHECK(AUDIO_FRAMES == 80128 && AUDIO_BLOCK_COUNT == 626 && AUDIO_BLOCK_BYTES == 3072);
    CHECK(AUDIO_CLOCK_PERIOD == 39 && AUDIO_CAPTURE_FRAMES == 240384);
    CHECK(AUDIO_CAPTURE_BLOCK_FRAMES == 384 && AUDIO_SAMPLE_PERIOD == 7488);
    CHECK((uint64_t)AUDIO_FRAMES * AUDIO_SAMPLE_PERIOD * 1000 / AUDIO_CLOCK_HZ == 4999);
    ++tests_passed;

    reset(); uint8_t *samples = (uint8_t *)(payload + 1);
    wake_delay = 24; /* 延迟调度三块：队列保留内容，仍必须全部按序处理。 */
    payload[0] = AUDIO_GUARD; payload[(AUDIO_FRAMES + 4) / 4] = AUDIO_GUARD;
    CHECK(capture_audio(samples) == TEST_PASS);
    CHECK(capture_completed == 626 && capture_consumed == 626 && ssi_reads == 626);
    CHECK(ssi_stops == 1 && ssi_closes == 1 && capture_overruns == 0 && capture_early_idle == 0);
    CHECK(absolute_frame == AUDIO_CAPTURE_FRAMES && clock_ms >= 5000 && clock_ms <= 5002 && hardware_closed());
    CHECK(capture_pending_peak == 3 && capture_wait_max_cycles >= 1600000);
    int previous_input = 0;
    int previous_output = 0;
    int reference_history[31] = {0};
    for (unsigned index = 0; index < AUDIO_CAPTURE_FRAMES; ++index)
    {
        int input = 262144;
        if (index / 3 % 32 >= 16) { input = -262144; }
        int filtered = input - previous_input + previous_output - previous_output / 192;
        previous_input = input;
        previous_output = filtered;
        /* 独立移位历史作为参考，不复用固件的环形缓冲索引。 */
        for (unsigned tap = 30; tap > 0; --tap) { reference_history[tap] = reference_history[tap - 1]; }
        reference_history[0] = filtered;
        if (index % 3 == 2)
        {
            int64_t sum = 0;
            for (unsigned tap = 0; tap < 31; ++tap) { sum += (int64_t)reference_history[tap] * lowpass_coefficients[tap]; }
            unsigned expected = encode_mulaw((int)(sum / 32768) / 32);
            CHECK(samples[index / 3] == expected); /* 全部输出样本顺序一致，不能重复或漏块。 */
        }
    }
    unsigned total = 0;
    for (unsigned second = 0; second < 5; ++second)
    {
        CHECK(capture_statistics[second].right_peak == 0);
        CHECK(capture_statistics[second].minimum == -262144 && capture_statistics[second].maximum == 262144);
        total += capture_statistics[second].frames;
    }
    CHECK(total == AUDIO_CAPTURE_FRAMES && payload[0] == AUDIO_GUARD && payload[(AUDIO_FRAMES + 4) / 4] == AUDIO_GUARD);
    ++tests_passed;

    prepare_playback(samples); unsigned started = clock_ms;
    CHECK(play_audio(samples) == TEST_WAIT);
    CHECK(playback_position == AUDIO_FRAMES && clock_ms - started >= 4999 && clock_ms - started <= 5002);
    CHECK(playback_duty_min >= 682 && playback_duty_max <= 818 && !recording && hardware_closed());
    CHECK(close_order[0] == 1 && close_order[1] == 2 && close_order[2] == 6);
    CHECK(playback_limited < AUDIO_SECOND_FRAMES / 10 && overlap == 0);
    recording = samples;
    unsigned limited_before = playback_limited;
    for (unsigned index = AUDIO_SECOND_FRAMES; index < AUDIO_FRAMES; ++index)
    {
        int value = playback_sample(index);
        CHECK(value >= -AUDIO_OUTPUT_LIMIT && value <= AUDIO_OUTPUT_LIMIT);
    }
    CHECK(playback_limited == limited_before); /* 启动以后的稳态信号无需限幅。 */
    recording = NULL;
    ++tests_passed;

    reset(); CHECK(run_test() == TEST_WAIT);
    CHECK(clock_ms >= 13000 && clock_ms <= 13005 && malloc_calls == 1 && free_calls == 1);
    CHECK(heap_bytes == 80136 && overlap == 0 && hardware_closed());
    ++tests_passed;

    const unsigned cancel_times[] = {30, 3500, 8500};
    for (unsigned index = 0; index < 3; ++index)
    {
        reset(); cancel_at = cancel_times[index];
        CHECK(run_test() == -RT_EINTR && hardware_closed() && malloc_calls == free_calls);
        CHECK(clock_ms <= cancel_at + 12 && overlap == 0);
    }
    ++tests_passed;

    reset(); ssi_freeze = 1; CHECK(capture_audio(samples) == -RT_ETIMEOUT);
    CHECK(clock_ms <= 6512 && hardware_closed());
    reset(); ssi_early_idle = 1; CHECK(capture_audio(samples) == -RT_ERROR && hardware_closed());
    reset(); ssi_burst = 1; CHECK(capture_audio(samples) == -RT_EFULL);
    CHECK(capture_overruns == 1 && ssi_reads == 4 && hardware_closed());
    reset(); wake_delay = 34; CHECK(capture_audio(samples) == -RT_EFULL);
    CHECK(capture_pending_peak == 4 && ssi_reads == 4 && hardware_closed());
    ++tests_passed;

    const unsigned read_failures[] = {1, 5, 626};
    for (unsigned index = 0; index < 3; ++index)
    {
        reset(); fail_read_at = read_failures[index];
        CHECK(capture_audio(samples) == -RT_ERROR && hardware_closed() && ssi_stops == 1);
    }
    ++tests_passed;

    for (unsigned failure = 1; failure <= 3; ++failure)
    {
        reset(); failed_operation = failure;
        CHECK(capture_audio(samples) == -RT_ERROR && hardware_closed());
    }
    reset(); ssi_stop_fail = 1; CHECK(capture_audio(samples) == -RT_ERROR && hardware_closed());
    reset(); ssi_no_idle = 1; CHECK(capture_audio(samples) == -RT_ETIMEOUT && hardware_closed());
    ++tests_passed;

    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        reset(); failed_operation = failure;
        CHECK(play_audio(samples) == -RT_ERROR && hardware_closed() && !recording);
    }
    reset(); freeze_playback = 1; CHECK(play_audio(samples) == -RT_ETIMEOUT && hardware_closed());
    reset(); force_pwm_error = 7; CHECK(play_audio(samples) == -RT_ERROR && hardware_closed());
    reset(); pin_error = -RT_ERROR; CHECK(play_audio(samples) == -RT_ERROR && hardware_closed());
    ++tests_passed;

    reset(); corrupt_guard = 1;
    CHECK(run_test() == -RT_ERROR && free_calls == 1 && hardware_closed() && operations == 3);
    reset(); allocation_fail = 1;
    CHECK(run_test() == -RT_ENOMEM && free_calls == 0 && hardware_closed());
    ++tests_passed;

    reset(); CHECK(capture_audio(samples) == TEST_PASS);
    memset(samples, encode_mulaw(0), AUDIO_FRAMES); prepare_playback(samples);
    CHECK(playback_gain == 0 && playback_mean == 0);
    for (unsigned index = 0; index < AUDIO_FRAMES; ++index)
    {
        samples[index] = (uint8_t)encode_mulaw(8);
        if (index % 2) { samples[index] = (uint8_t)encode_mulaw(-8); }
    }
    prepare_playback(samples); CHECK(playback_gain == AUDIO_MAX_GAIN);
    recording = samples;
    CHECK(playback_sample(0) == 0 && playback_sample(AUDIO_FRAMES - 1) == 0);
    samples[320] = (uint8_t)encode_mulaw(32767);
    CHECK(playback_sample(320) == AUDIO_OUTPUT_LIMIT && playback_limited > 0);
    CHECK(max_log_bytes < 126);
    ++tests_passed;

    /* 启动直流应衰减；500Hz附近的语音信号应保留。
     * 输入为慢漂移叠加正弦，使用独立的幅度/平均值指标验证滤波目的。 */
    filter_previous_input = 0; filter_previous_output = 0;
    CHECK(filter_microphone(8388607) == 8388607);
    int filtered_dc = 0;
    for (unsigned index = 0; index < 16026; ++index) { filtered_dc = filter_microphone(8388607); }
    CHECK(filtered_dc >= 0 && filtered_dc < 192);
    const int sine[] = {0,1598,3135,4551,5793,6811,7568,8035,
                        8192,8035,7568,6811,5793,4551,3135,1598,
                        0,-1598,-3135,-4551,-5793,-6811,-7568,-8035,
                        -8192,-8035,-7568,-6811,-5793,-4551,-3135,-1598};
    filter_previous_input = 0; filter_previous_output = 0;
    int64_t filtered_sum = 0; uint64_t filtered_abs = 0;
    int voice_peak = 0;
    for (unsigned index = 0; index < 32000; ++index)
    {
        int value = filter_microphone((int)index * 8 + sine[index / 3 % 32]);
        if (index >= 16000)
        {
            filtered_sum += value;
            int magnitude = value;
            if (magnitude < 0) { magnitude = -magnitude; }
            filtered_abs += magnitude;
            if (magnitude > voice_peak) { voice_peak = magnitude; }
        }
    }
    CHECK(filtered_sum / 16000 > 1400 && filtered_sum / 16000 < 1700);
    CHECK(filtered_abs / 16000 > 5000 && filtered_abs / 16000 < 5500);
    CHECK(voice_peak > 9400 && voice_peak < 10000);
    ++tests_passed;

    /* 下采样前低通必须保留直流、抑制会折叠成直流的16kHz信号。
     * 首段等到 FIR 历史充满后再检查，避免把启动响应当作稳态。 */
    memset(lowpass_history, 0, sizeof(lowpass_history)); lowpass_position = 0;
    int coefficient_sum = 0;
    for (unsigned tap = 0; tap < 31; ++tap) { coefficient_sum += lowpass_coefficients[tap]; }
    CHECK(coefficient_sum == 32768);
    for (unsigned index = 0; index < 300; ++index)
    {
        int value = filter_downsample(123456, index);
        if (index >= 31 && index % 3 == 2) { CHECK(value == 123456); }
        if (index % 3 != 2) { CHECK(value == 0); }
    }
    memset(lowpass_history, 0, sizeof(lowpass_history)); lowpass_position = 0;
    const int high_frequency[] = {0,8660,-8660};
    for (unsigned index = 0; index < 300; ++index)
    {
        int value = filter_downsample(high_frequency[index % 3], index);
        if (index >= 31 && index % 3 == 2) { CHECK(value >= -5 && value <= 5); }
    }
    memset(lowpass_history, 0, sizeof(lowpass_history)); lowpass_position = 0;
    int lowpass_voice_peak = 0;
    for (unsigned index = 0; index < 3000; ++index)
    {
        int value = filter_downsample(sine[index / 3 % 32], index);
        if (index >= 31 && index % 3 == 2)
        {
            if (value < 0) { value = -value; }
            if (value > lowpass_voice_peak) { lowpass_voice_peak = value; }
        }
    }
    CHECK(lowpass_voice_peak > 7800 && lowpass_voice_peak < 8400);
    ++tests_passed;

    reset(); semaphore_fail = 1;
    CHECK(capture_audio(samples) == -RT_ERROR && operations == 0 && hardware_closed());
    reset(); priority_fail = 1;
    CHECK(capture_audio(samples) == -RT_ERROR && operations == 0 && hardware_closed());
    reset(); pin_error = -RT_ERROR;
    CHECK(capture_audio(samples) == -RT_ERROR && priority_calls == 2 && hardware_closed());
    ++tests_passed;
}
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies", type=Path)
    args = parser.parse_args()
    if args.dependencies is not None:
        sys.path.insert(0, str(args.dependencies.resolve()))
    from elftools.elf.elffile import ELFFile
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR

    source = (ROOT / "src/test/test-audio-replay.c").read_text(encoding="utf-8")
    definitions = source[source.index("#define AUDIO_CLOCK_HZ"):source.index("/* 24 位数据右对齐")]
    functions = "\n".join(extract_function(source, name) for name in (
        "pcm24_signed", "encode_mulaw", "decode_mulaw", "stop_capture", "mic_callback", "filter_microphone", "filter_downsample",
        "consume_block", "wait_capture_idle", "close_timer", "capture_samples", "capture_audio", "prepare_playback",
        "playback_sample", "audio_tick", "play_audio", "run_test"))
    output = ROOT / "logs/audio-validation"
    output.mkdir(parents=True, exist_ok=True)
    harness = output / "harness.c"
    harness.write_text(STUB + definitions + "\n" + functions + HARNESS, encoding="utf-8")
    studio = Path(os.environ.get("RTTHREAD_STUDIO", "C:/RT-ThreadStudio"))
    compiler = next(studio.glob("repo/Extract/ToolChain_Support_Packages/ARM/*/10.2.1/bin/arm-none-eabi-gcc.exe"))
    elf_path = output / "harness.elf"
    subprocess.run([str(compiler), "-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft", "-O1",
                    "-nostartfiles", "-Wl,-Ttext=0x10000,-Tdata=0x400000,-e,validate",
                    str(harness), "-o", str(elf_path), "-lc", "-lnosys"], check=True)
    machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    machine.mem_map(0, 0x810000)
    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        for segment in elf.iter_segments():
            if segment["p_type"] == "PT_LOAD":
                machine.mem_write(segment["p_vaddr"], segment.data())
        symbols = {symbol.name: symbol["st_value"]
                   for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
    machine.reg_write(UC_ARM_REG_SP, 0x7FF000)
    machine.reg_write(UC_ARM_REG_LR, 0xFFF01)
    machine.emu_start(symbols["validate"] | 1, 0xFFF00, timeout=60000000)
    error = int.from_bytes(machine.mem_read(symbols["test_error"], 4), "little")
    passed = int.from_bytes(machine.mem_read(symbols["tests_passed"], 4), "little")
    if error or passed != 15:
        raise RuntimeError(f"Audio validation failed: C line={error}, groups={passed}/15")
    print("PASS: 15 ARM groups; queued 48k capture / 16k playback, delayed scheduling/content order, overflow, IPC/priority cleanup, FIR/high-pass/G.711, 5s lifecycle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
