"""在 ARM 模拟器中执行实际回放代码的 PCM24 转换、采集生命周期与 PWM 回调。

验证软件算术与缓冲边界，不访问串口，也不替代 SSI/DTC 或实际听音。
依赖 unicorn、pyelftools；可复用 OLED 验证环境：
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
    """提取实际 C 函数，不在验证脚本中重新实现被测算法。"""
    match = re.search(r"static (?:int|void) " + name + r"\([^)]*\)\s*\{", source)
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
#define RT_ERROR 1
#define RT_EINTR 9
#define TEST_PASS 0
#define TEST_WAIT 1
#define RT_ETIMEOUT 2
#define RT_TICK_PER_SECOND 1000
#define FSP_SUCCESS 0
#define GPT_IO_PIN_GTIOCA 0
#define GPT_IO_PIN_GTIOCB 1
#define TIMER_SOURCE_DIV_1 0
#define TIMER_MODE_PERIODIC 0
#define BSP_IO_PORT_07_PIN_02 0x702
#define BSP_IO_PORT_07_PIN_03 0x703
#define I2S_EVENT_RX_FULL 2
#define I2S_EVENT_IDLE 0
#define I2S_PCM_WIDTH_24_BITS 24
#define I2S_WORD_LENGTH_32_BITS 32
#define I2S_WS_CONTINUE_ON 0
#define BSP_IO_PORT_04_PIN_03 0x403
#define BSP_IO_PORT_04_PIN_04 0x404
#define BSP_IO_PORT_04_PIN_06 0x406
#define FSP_PRIV_CLOCK_PCLKD 0
typedef int fsp_err_t;
typedef unsigned rt_tick_t;
typedef struct { int unused; } timer_callback_args_t;
typedef struct { unsigned event; } i2s_callback_args_t;
typedef struct { unsigned open; } gpt_instance_ctrl_t;
typedef struct
{
    unsigned source_div, mode, period_counts, duty_cycle_counts;
    void (*p_callback)(timer_callback_args_t *);
} timer_cfg_t;
typedef struct { unsigned block_count_remaining, transfer_length_remaining; } transfer_properties_t;
typedef struct { unsigned size, length; const void *p_src; } transfer_info_t;
typedef struct { const transfer_info_t *p_info; } transfer_cfg_t;
typedef struct { int (*infoGet)(void *, transfer_properties_t *); } transfer_api_t;
typedef struct { const transfer_cfg_t *p_cfg; const transfer_api_t *p_api; void *p_ctrl; } transfer_instance_t;
typedef struct
{
    unsigned pcm_width, word_length, ws_continue;
    int rxi_irq;
    void (*p_callback)(i2s_callback_args_t *);
    const transfer_instance_t *p_transfer_rx;
} i2s_cfg_t;
typedef struct { unsigned rx_dest_samples, fifo_access_size, open; } ssi_instance_ctrl_t;
static struct { unsigned SSICR, SSISR, SSIFSR, SSIFCR, SSIOFR; } mock_ssi;
#define R_SSI0 (&mock_ssi)
static struct { struct { struct { unsigned PmnPFS; } PIN[16]; } PORT[8]; } mock_pfs;
#define R_PFS (&mock_pfs)
static ssi_instance_ctrl_t g_i2s0_ctrl;
static i2s_cfg_t g_i2s0_cfg;
static gpt_instance_ctrl_t g_timer_ctrl;
static timer_cfg_t g_timer_cfg;
static volatile fsp_err_t capture_stop_error;
static volatile rt_tick_t capture_finished_at;
static volatile unsigned capture_end_ssicr, capture_end_ssisr, capture_end_ssifsr, capture_end_cpu_words;
static unsigned ssi_reads, ssi_stops, ssi_closes, ssi_active, ssi_started, ssi_pending_idle;
static unsigned ssi_freeze, ssi_no_idle, ssi_early_idle, ssi_read_fail, ssi_open_fail, ssi_stop_fail;
static unsigned continuous_seen;
static uint32_t *ssi_buffer;
static void (*ssi_callback)(i2s_callback_args_t *);
static int R_SSI_Stop(ssi_instance_ctrl_t *ctrl);
static void mock_capture_step(void);
static unsigned R_FSP_SystemClockHzGet(unsigned clock) { return 120000000; }
static unsigned clock_ms, stop_at, duties[2], duty_calls;
static int force_pwm_error;
static gpt_instance_ctrl_t g_timer6_ctrl, g_timer2_ctrl;
static timer_cfg_t g_timer6_cfg, g_timer2_cfg;
static void (*sample_callback)(timer_callback_args_t *);
static unsigned open_calls, close_calls, mute_calls, operation, failed_operation, freeze_samples;
static unsigned closed_order[2];
static unsigned max_log_bytes;
static volatile unsigned capture_complete, capture_active, capture_rx_events, capture_idle_events, capture_early_idle;
static volatile unsigned playback_position;
static unsigned playback_frame_count;
static const int16_t *playback_samples;
static volatile fsp_err_t playback_error;
static volatile unsigned playback_duty_min, playback_duty_max, playback_duty_nonzero;
static rt_tick_t rt_tick_get(void) { return clock_ms; }
static int test_cancelled(void) { return stop_at && clock_ms >= stop_at; }
static int test_elapsed(rt_tick_t start, unsigned ms) { return clock_ms - start >= ms; }
static void rt_thread_mdelay(unsigned ms)
{
    clock_ms += ms;
    mock_capture_step();
    if (sample_callback && !freeze_samples)
    {
        for (unsigned index = 0; index < ms * 16; ++index) { sample_callback(NULL); }
    }
}
static void rt_kprintf(const char *format, ...)
{
    char buffer[128];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length > 0 && (unsigned)length > max_log_bytes) { max_log_bytes = (unsigned)length; }
}
static fsp_err_t R_GPT_DutyCycleSet(void *timer, unsigned duty, unsigned pin)
{
    duties[pin] = duty;
    ++duty_calls;
    return force_pwm_error;
}
static int configure_audio_pin(unsigned pin) { return TEST_PASS; }
static int mute_output_pins(void) { ++mute_calls; return TEST_PASS; }
static int R_GPT_Open(gpt_instance_ctrl_t *ctrl, const timer_cfg_t *cfg)
{
    ++operation;
    if (operation == failed_operation) { return 7; }
    ctrl->open = 1; ++open_calls;
    if (ctrl == &g_timer2_ctrl) { sample_callback = cfg->p_callback; }
    return FSP_SUCCESS;
}
static int R_GPT_Start(gpt_instance_ctrl_t *ctrl)
{
    ++operation;
    if (operation == failed_operation) { return 7; }
    return FSP_SUCCESS;
}
static int R_GPT_Stop(gpt_instance_ctrl_t *ctrl)
{
    if (ctrl == &g_timer2_ctrl) { sample_callback = NULL; }
    return FSP_SUCCESS;
}
static int R_GPT_Close(gpt_instance_ctrl_t *ctrl)
{
    if (!ctrl->open) { return 7; }
    ctrl->open = 0;
    if (close_calls < 2) { closed_order[close_calls] = 6; if (ctrl == &g_timer2_ctrl) { closed_order[close_calls] = 2; } }
    ++close_calls;
    return FSP_SUCCESS;
}
static int R_SSI_Open(ssi_instance_ctrl_t *ctrl, const i2s_cfg_t *cfg)
{
    if (ssi_open_fail) { return 7; }
    if (cfg->pcm_width != I2S_PCM_WIDTH_24_BITS || cfg->word_length != I2S_WORD_LENGTH_32_BITS) { return 7; }
    ctrl->open = 1; ctrl->fifo_access_size = 2; ssi_callback = cfg->p_callback;
    mock_ssi.SSIOFR = 0;
    if (cfg->ws_continue == I2S_WS_CONTINUE_ON) { mock_ssi.SSIOFR = 0x100; }
    return FSP_SUCCESS;
}
static int R_SSI_Read(ssi_instance_ctrl_t *ctrl, void *buffer, unsigned bytes)
{
    ++ssi_reads;
    if (ssi_reads == 2 && (mock_ssi.SSIOFR & 0x100) && ctrl->open) { continuous_seen = 1; }
    if (ssi_read_fail == ssi_reads) { return 7; }
    ssi_buffer = buffer; ssi_active = 1; ssi_started = clock_ms;
    return FSP_SUCCESS;
}
static int R_SSI_Stop(ssi_instance_ctrl_t *ctrl)
{
    ++ssi_stops; ssi_active = 0; ctrl->rx_dest_samples = 0;
    if (ssi_stop_fail) { return 7; }
    ssi_pending_idle = 1;
    return FSP_SUCCESS;
}
static int R_SSI_Close(ssi_instance_ctrl_t *ctrl)
{
    if (!ctrl->open) { return 7; }
    ctrl->open = 0; ++ssi_closes; ssi_active = 0; ssi_pending_idle = 0;
    ssi_callback = NULL; mock_ssi.SSIOFR = 0;
    return FSP_SUCCESS;
}
static void mock_capture_step(void)
{
    if (!ssi_callback) { return; }
    i2s_callback_args_t args = {I2S_EVENT_IDLE};
    if (ssi_pending_idle && !ssi_no_idle)
    {
        ssi_pending_idle = 0; ssi_callback(&args);
    }
    if (ssi_active && ssi_early_idle)
    {
        ssi_active = 0; ssi_callback(&args); return;
    }
    if (ssi_active && !ssi_freeze && clock_ms - ssi_started >= 512)
    {
        for (unsigned index = 0; index < 16384; ++index)
        {
            ssi_buffer[index] = 0;
            if (index % 2 == 0) { ssi_buffer[index] = 256000; }
        }
        args.event = I2S_EVENT_RX_FULL; ssi_callback(&args);
    }
}

'''

HARNESS = r'''
unsigned test_error, tests_passed;
static struct
{
    uint32_t before;
    union { int16_t samples[AUDIO_CAPTURE_BYTES / sizeof(int16_t)]; uint32_t raw[AUDIO_FRAMES * 2]; };
    uint32_t after;
} guarded;
#define CHECK(condition) do { if (!(condition)) { test_error = __LINE__; return; } } while (0)
static void reset_capture(void)
{
    clock_ms = 0; stop_at = 0; operation = 0; failed_operation = 0;
    open_calls = 0; close_calls = 0;
    ssi_reads = 0; ssi_stops = 0; ssi_closes = 0; ssi_active = 0; ssi_pending_idle = 0;
    ssi_freeze = 0; ssi_no_idle = 0; ssi_early_idle = 0; ssi_read_fail = 0;
    ssi_open_fail = 0; ssi_stop_fail = 0; continuous_seen = 0; ssi_callback = NULL;
}
static void reset_playback(void)
{
    clock_ms = 0; stop_at = 0; duties[0] = 0; duties[1] = 0; duty_calls = 0;
    open_calls = 0; close_calls = 0; mute_calls = 0; operation = 0; failed_operation = 0;
    freeze_samples = 0; force_pwm_error = 0; sample_callback = NULL;
}
void validate(void)
{
    int16_t *samples = guarded.samples;
    guarded.before = 0x12345678;
    guarded.after = 0x87654321;
    for (unsigned i = 0; i < AUDIO_FRAMES; ++i)
    {
        samples[i] = 5000;
        if (i % 32 < 16) { samples[i] += 1000; }
        else { samples[i] -= 1000; }
    }
    CHECK(prepare_playback(samples) == TEST_PASS);
    CHECK(samples[320] == 3000 && samples[336] == -3000);
    CHECK(samples[0] == 0 && samples[AUDIO_FRAMES - 1] == 0);
    CHECK(samples[1] > 0 && samples[1] < samples[320]);
    CHECK(samples[17] < 0); /* 负样本淡入不能变成无符号算术。 */
    CHECK(guarded.before == 0x12345678 && guarded.after == 0x87654321);
    ++tests_passed;

    for (unsigned i = 0; i < AUDIO_FRAMES; ++i)
    {
        samples[i] = -1234;
    }
    CHECK(prepare_playback(samples) == -RT_ERROR); /* 全直流没有可回放声音。 */
    ++tests_passed;

    for (unsigned i = 0; i < AUDIO_FRAMES; ++i)
    {
        samples[i] = INT16_MIN;
        if (i % 2) { samples[i] = INT16_MAX; }
    }
    CHECK(prepare_playback(samples) == TEST_PASS);
    CHECK(samples[320] < 0 && samples[321] > 0);
    for (unsigned i = 0; i < AUDIO_FRAMES; ++i)
    {
        CHECK(samples[i] >= -AUDIO_SAMPLE_LIMIT && samples[i] <= AUDIO_SAMPLE_LIMIT);
    }
    ++tests_passed;

    for (unsigned i = 0; i < AUDIO_FRAMES; ++i)
    {
        samples[i] = -1001;
        if (i % 2) { samples[i] = -999; }
    }
    CHECK(prepare_playback(samples) == TEST_PASS);
    CHECK(samples[320] == -8 && samples[321] == 8); /* 只允许最多 8 倍增益。 */
    ++tests_passed;

    playback_samples = samples;
    playback_position = 0;
    playback_frame_count = 2;
    samples[0] = 32767;
    samples[1] = -32768;
    audio_tick(NULL);
    CHECK(duties[0] == 841 && duties[1] == 659);
    audio_tick(NULL);
    CHECK(duties[0] == 659 && duties[1] == 841);
    audio_tick(NULL);
    CHECK(duties[0] == 750 && duties[1] == 750 && playback_position == 2);
    CHECK(duty_calls == 6);
    force_pwm_error = 7;
    audio_tick(NULL);
    CHECK(playback_error == 7);
    ++tests_passed;

    CHECK(wait_ms(100) == TEST_PASS && clock_ms == 100);
    stop_at = 130;
    CHECK(wait_ms(1000) == -RT_EINTR && clock_ms == 130);
    ++tests_passed;

    struct pcm_statistics statistics;
    const int16_t stereo[] = {1000, -32768, 3000, 32767, 1000, -32768, 3000, 32767};
    measure_pcm(stereo, 4, 2, &statistics);
    CHECK(statistics.minimum == 1000 && statistics.maximum == 3000 && statistics.mean == 2000);
    CHECK(statistics.ac_peak == 1000 && statistics.mean_abs_ac == 1000 && statistics.changed_samples == 3);
    measure_pcm(stereo + 1, 4, 2, &statistics);
    CHECK(statistics.minimum == -32768 && statistics.maximum == 32767 && statistics.clipped_samples == 4);
    CHECK(statistics.ac_peak == 32768 && statistics.mean_abs_ac == 32767);
    const int16_t silent[] = {1234, 1234, 1234, 1234};
    measure_pcm(silent, 4, 1, &statistics);
    CHECK(statistics.mean == 1234 && statistics.ac_peak == 0 && statistics.mean_abs_ac == 0);
    ++tests_passed;

    capture_active = 1; capture_complete = 0;
    i2s_callback_args_t event = {I2S_EVENT_IDLE};
    mic_callback(&event);
    CHECK(capture_idle_events == 1 && capture_early_idle == 1);
    capture_early_idle = 0;
    event.event = I2S_EVENT_RX_FULL;
    mic_callback(&event);
    event.event = I2S_EVENT_IDLE;
    mic_callback(&event);
    CHECK(capture_complete == 1 && capture_rx_events == 1 && capture_early_idle == 0);
    CHECK(capture_active == 0 && ssi_stops == 1 && capture_stop_error == FSP_SUCCESS);
    event.event = I2S_EVENT_RX_FULL; mic_callback(&event);
    CHECK(ssi_stops == 1); /* 重复 RX 通知不能重复 Stop。 */
    ++tests_passed;

    reset_playback();
    CHECK(play_reference(samples, "REFERENCE") == TEST_WAIT);
    CHECK(playback_frame_count == AUDIO_REFERENCE_FRAMES && playback_position == AUDIO_REFERENCE_FRAMES);
    CHECK(samples[0] == 0 && samples[AUDIO_REFERENCE_FRAMES - 1] == 0);
    CHECK(samples[328] == 3000 && samples[344] == -3000);
    CHECK(playback_duty_min == 682 && playback_duty_max == 818 && playback_duty_nonzero > 0);
    CHECK(close_calls == 2 && closed_order[0] == 2 && closed_order[1] == 6);
    CHECK(guarded.before == 0x12345678 && guarded.after == 0x87654321 && !sample_callback);
    ++tests_passed;

    reset_playback(); freeze_samples = 1; stop_at = 3;
    CHECK(play_audio("CANCEL", samples, 32) == -RT_EINTR);
    CHECK(open_calls == close_calls && mute_calls == 1 && !sample_callback);
    reset_playback(); freeze_samples = 1;
    CHECK(play_audio("TIMEOUT", samples, 32) == -RT_ETIMEOUT);
    CHECK(clock_ms == AUDIO_PLAYBACK_TIMEOUT_MS && open_calls == close_calls && !sample_callback);
    reset_playback(); force_pwm_error = 7;
    CHECK(play_audio("PWM-ERROR", samples, 32) == -RT_ERROR && playback_error == 7);
    CHECK(open_calls == close_calls && !sample_callback);
    ++tests_passed;

    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        reset_playback(); failed_operation = failure;
        CHECK(play_audio("STARTUP-ERROR", samples, 32) == -RT_ERROR);
        CHECK(open_calls == close_calls && !g_timer2_ctrl.open && !g_timer6_ctrl.open && !sample_callback);
        CHECK(mute_calls == 1);
    }
    ++tests_passed;
    reset_capture();
    CHECK(capture_audio(guarded.raw) == TEST_PASS);
    CHECK(ssi_reads == 2 && ssi_stops == 2 && ssi_closes == 1 && continuous_seen == 1);
    CHECK(clock_ms >= 4024 && clock_ms < 4080 && capture_rx_events == 1);
    CHECK(guarded.raw[0] == 256000 && guarded.raw[1] == 0);
    CHECK(!g_i2s0_ctrl.open && !g_timer_ctrl.open && !ssi_active);
    CHECK(guarded.before == 0x12345678 && guarded.after == 0x87654321);
    ++tests_passed;

    const unsigned cancel_times[] = {3, 2000, 3600};
    for (unsigned index = 0; index < 3; ++index)
    {
        reset_capture(); stop_at = cancel_times[index];
        CHECK(capture_audio(guarded.raw) == -RT_EINTR);
        CHECK(ssi_closes == 1 && !g_i2s0_ctrl.open && !g_timer_ctrl.open && !ssi_active);
        CHECK(open_calls == close_calls);
    }
    ++tests_passed;

    reset_capture(); ssi_freeze = 1;
    CHECK(capture_audio(guarded.raw) == -RT_ETIMEOUT && clock_ms < 2050);
    CHECK(ssi_stops == 1 && ssi_closes == 1 && !ssi_active && open_calls == close_calls);
    reset_capture(); ssi_early_idle = 1;
    CHECK(capture_audio(guarded.raw) == -RT_ERROR);
    CHECK(ssi_closes == 1 && !ssi_active && open_calls == close_calls);
    ++tests_passed;

    for (unsigned failure = 1; failure <= 2; ++failure)
    {
        reset_capture(); ssi_read_fail = failure;
        CHECK(capture_audio(guarded.raw) == -RT_ERROR);
        CHECK(ssi_closes == 1 && !ssi_active && open_calls == close_calls);
        reset_capture(); failed_operation = failure;
        CHECK(capture_audio(guarded.raw) == -RT_ERROR);
        CHECK(!g_timer_ctrl.open && !g_i2s0_ctrl.open && open_calls == close_calls);
    }
    reset_capture(); ssi_open_fail = 1;
    CHECK(capture_audio(guarded.raw) == -RT_ERROR && ssi_closes == 0 && open_calls == close_calls);
    ++tests_passed;

    reset_capture(); ssi_stop_fail = 1;
    CHECK(capture_audio(guarded.raw) == -RT_ERROR && ssi_closes == 1 && !ssi_active);
    reset_capture(); ssi_no_idle = 1;
    CHECK(capture_audio(guarded.raw) == -RT_ETIMEOUT && clock_ms < 1100);
    CHECK(ssi_closes == 1 && !ssi_active && open_calls == close_calls);
    ++tests_passed;
    CHECK(pcm24_signed(0x00000000) == 0 && pcm24_signed(0x007FFFFF) == 8388607);
    CHECK(pcm24_signed(0x00800000) == -8388608 && pcm24_signed(0xFFFFFFFF) == -1);
    CHECK(pcm24_signed(0xFF7FFFFF) == 8388607 && pcm24_signed(0xAA800000) == -8388608);
    const uint32_t raw_stereo[] = {0x007FFFFF, 0, 0x00800000, 0, 0x007FFFFF, 0, 0x00800000, 0};
    measure_pcm24(raw_stereo, 4, 2, &statistics);
    CHECK(statistics.minimum == -8388608 && statistics.maximum == 8388607 && statistics.mean == 0);
    CHECK(statistics.ac_peak == 8388608 && statistics.mean_abs_ac == 8388607 && statistics.clipped_samples == 4);
    measure_pcm24(raw_stereo + 1, 4, 2, &statistics);
    CHECK(statistics.mean_abs_ac == 0 && statistics.changed_samples == 0 && statistics.zero_samples == 4);
    ++tests_passed;

    for (unsigned index = 0; index < AUDIO_FRAMES; ++index)
    {
        guarded.raw[index * 2] = 0x007FFFFF;
        if (index % 2) { guarded.raw[index * 2] = 0x00800000; }
        guarded.raw[index * 2 + 1] = index; /* 右声道不能串入单声道转换结果。 */
    }
    guarded.raw[4] = 0x00FFFF00; /* -256 -> -1 */
    guarded.raw[6] = 0x00FFFF01; /* -255 -> 0，按整数除法向零取整。 */
    inspect_capture(guarded.raw);
    convert_left_pcm24(guarded.raw);
    CHECK(samples[0] == 32767 && samples[1] == -32768 && samples[2] == -1 && samples[3] == 0);
    for (unsigned index = 4; index < AUDIO_FRAMES; ++index)
    {
        if (index % 2) { CHECK(samples[index] == -32768); }
        else { CHECK(samples[index] == 32767); }
    }
    CHECK(guarded.before == 0x12345678 && guarded.after == 0x87654321);
    CHECK(prepare_playback(samples) == TEST_PASS && samples[320] > 0 && samples[321] < 0);
    ++tests_passed;
    CHECK(max_log_bytes < 126);
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
    definitions = "\n".join(re.findall(r"^#define AUDIO_.*$", source, flags=re.MULTILINE))
    statistics_type = re.search(r"struct pcm_statistics\s*\{.*?\};", source, re.S).group(0)
    functions = "\n".join(extract_function(source, name)
                          for name in ("measure_pcm", "print_pcm_statistics", "pcm24_signed", "measure_pcm24",
                                       "print_pcm24_statistics", "inspect_capture", "convert_left_pcm24", "wait_ms", "mic_callback",
                                       "close_timer", "wait_capture_idle", "capture_block", "capture_audio",
                                       "prepare_playback", "audio_tick", "play_audio", "play_reference"))
    output = ROOT / "logs/audio-validation"
    output.mkdir(parents=True, exist_ok=True)
    harness = output / "harness.c"
    harness.write_text(STUB + definitions + "\n" + statistics_type + "\n" + functions + HARNESS, encoding="utf-8")
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
    machine.emu_start(symbols["validate"] | 1, 0xFFF00, timeout=30000000)
    error = int.from_bytes(machine.mem_read(symbols["test_error"], 4), "little")
    passed = int.from_bytes(machine.mem_read(symbols["tests_passed"], 4), "little")
    if error or passed != 19:
        raise RuntimeError(f"Audio validation failed: C line={error}, groups={passed}/19")
    print("PASS: 19 ARM groups; PCM24 sign/alignment/in-place conversion; warmup/record lifecycle, cancellation/timeouts/startup cleanup; PCM/DC/gain/PWM, channel stats, SSI callback, reference/EOF, errors/cleanup, log length")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
