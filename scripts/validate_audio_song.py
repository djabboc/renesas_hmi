"""提取实际歌曲例程，在 ARM 模拟器验证 PWM、播放完成、取消、超时和清理。

python scripts/validate_audio_song.py --dependencies logs/oled-validation/python
FSP 使用可注入错误的桩；不访问 COM8，不替代 J8 实际听音验收。
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
from validate_audio_replay import extract_function

ROOT = Path(__file__).resolve().parents[1]

STUB = r'''
#include <stdint.h>
#include <stddef.h>
#define RT_UNUSED(value) (void)(value)
#define RT_ERROR 1
#define RT_EINTR 9
#define RT_ETIMEOUT 2
#define TEST_PASS 0
#define TEST_WAIT 1
#define FSP_SUCCESS 0
#define GPT_IO_PIN_GTIOCA 0
#define GPT_IO_PIN_GTIOCB 1
#define TIMER_SOURCE_DIV_1 0
#define TIMER_MODE_PERIODIC 0
#define FSP_PRIV_CLOCK_PCLKD 0
#define BSP_IO_PORT_07_PIN_02 0x702
#define BSP_IO_PORT_07_PIN_03 0x703
#define IOPORT_CFG_PORT_DIRECTION_OUTPUT 1
#define IOPORT_CFG_PORT_OUTPUT_LOW 0
#define SONG_SAMPLE_COUNT 8u
#define SONG_SAMPLE_RATE 16000u
#define SONG_PCM_PEAK 3000u
typedef int fsp_err_t;
typedef unsigned rt_tick_t;
typedef int bsp_io_port_pin_t;
typedef struct { int unused; } timer_callback_args_t;
typedef struct { int open; } gpt_instance_ctrl_t;
typedef struct
{
    unsigned source_div, mode, period_counts, duty_cycle_counts;
    void (*p_callback)(timer_callback_args_t *);
} timer_cfg_t;
static gpt_instance_ctrl_t g_timer6_ctrl, g_timer2_ctrl;
static timer_cfg_t g_timer6_cfg, g_timer2_cfg;
static int g_ioport_ctrl;
static const struct { unsigned number_of_pins; const struct { int pin; unsigned pin_cfg; } p_pin_cfg_data[2]; }
g_bsp_pin_cfg = {2, {{0x702, 8}, {0x703, 8}}};
static const int16_t song_pcm[SONG_SAMPLE_COUNT] = {-32768, -3000, 0, 3000, 32767, 2000, -2000, 0};
static volatile unsigned playback_position;
static volatile fsp_err_t playback_error;
static unsigned clock_ms, stop_at, duties[2], duty_calls, muted, open_calls, close_calls;
static unsigned fake_clock, failed_operation, operation, freeze_samples;
static int force_pwm_error;
static void (*sample_callback)(timer_callback_args_t *);
static int test_cancelled(void) { return stop_at && clock_ms >= stop_at; }
static int test_elapsed(unsigned start, unsigned ms) { return clock_ms - start >= ms; }
static unsigned rt_tick_get(void) { return clock_ms; }
static void rt_thread_mdelay(unsigned ms)
{
    clock_ms += ms;
    if (!freeze_samples && sample_callback)
    {
        for (unsigned i = 0; i < ms * 16; ++i) { sample_callback(NULL); }
    }
}
static void rt_kprintf(const char *format, ...) { }
static unsigned R_FSP_SystemClockHzGet(unsigned clock) { return fake_clock; }
static int R_IOPORT_PinCfg(void *ctrl, unsigned pin, unsigned cfg)
{
    if (cfg == IOPORT_CFG_PORT_DIRECTION_OUTPUT) { ++muted; }
    return 0;
}
static int R_GPT_DutyCycleSet(void *ctrl, unsigned duty, unsigned pin)
{
    duties[pin] = duty;
    ++duty_calls;
    return force_pwm_error;
}
static int R_GPT_Open(gpt_instance_ctrl_t *ctrl, const timer_cfg_t *config)
{
    ++operation;
    if (operation == failed_operation) { return 7; }
    ctrl->open = 1;
    ++open_calls;
    if (ctrl == &g_timer2_ctrl) { sample_callback = config->p_callback; }
    return 0;
}
static int R_GPT_Start(gpt_instance_ctrl_t *ctrl)
{
    ++operation;
    if (operation == failed_operation) { return 7; }
    return 0;
}
static int R_GPT_Stop(gpt_instance_ctrl_t *ctrl)
{
    if (ctrl == &g_timer2_ctrl) { sample_callback = NULL; }
    return 0;
}
static int R_GPT_Close(gpt_instance_ctrl_t *ctrl)
{
    if (!ctrl->open) { return 7; }
    ctrl->open = 0;
    ++close_calls;
    return 0;
}
'''

HARNESS = r'''
unsigned test_error, tests_passed;
#define CHECK(value) do { if (!(value)) { test_error = __LINE__; return; } } while (0)
static void reset(void)
{
    clock_ms = 0; stop_at = 0; duty_calls = 0; muted = 0;
    open_calls = 0; close_calls = 0; fake_clock = AUDIO_TIMER_CLOCK_HZ;
    failed_operation = 0; operation = 0; freeze_samples = 0; force_pwm_error = 0;
    playback_position = 0; playback_error = 0; sample_callback = NULL;
}
void validate(void)
{
    reset();
    audio_tick(NULL);
    CHECK(duties[0] == 682 && duties[1] == 818);
    audio_tick(NULL); audio_tick(NULL);
    CHECK(duties[0] == 750 && duties[1] == 750);
    audio_tick(NULL);
    CHECK(duties[0] == 818 && duties[1] == 682);
    for (unsigned i = 0; i < 20; ++i) { audio_tick(NULL); }
    CHECK(playback_position == SONG_SAMPLE_COUNT);
    CHECK(duties[0] == 750 && duties[1] == 750);
    ++tests_passed;

    reset();
    CHECK(run_test() == TEST_WAIT);
    CHECK(playback_position == SONG_SAMPLE_COUNT && playback_error == 0);
    CHECK(open_calls == 2 && close_calls == 2 && muted == 2 && !sample_callback);
    CHECK(!g_timer2_ctrl.open && !g_timer6_ctrl.open);
    ++tests_passed;

    reset(); stop_at = 1;
    freeze_samples = 1;
    CHECK(run_test() == -RT_EINTR);
    CHECK(open_calls == close_calls && muted == 2 && !sample_callback);
    reset(); freeze_samples = 1;
    CHECK(run_test() == -RT_ETIMEOUT && clock_ms == AUDIO_PLAYBACK_TIMEOUT_MS);
    CHECK(open_calls == close_calls && muted == 2 && !sample_callback);
    ++tests_passed;

    reset(); force_pwm_error = 7;
    CHECK(run_test() == -RT_ERROR && playback_error == 7);
    CHECK(open_calls == close_calls && muted == 2 && !sample_callback);
    ++tests_passed;

    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        reset(); failed_operation = failure;
        CHECK(run_test() == -RT_ERROR);
        CHECK(open_calls == close_calls && muted == 2 && !sample_callback);
        CHECK(!g_timer2_ctrl.open && !g_timer6_ctrl.open);
    }
    reset(); fake_clock = 60000000;
    CHECK(run_test() == -RT_ERROR && open_calls == 0 && muted == 2);
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

    source = (ROOT / "src/test/test-audio-song.c").read_text(encoding="utf-8")
    definitions = "\n".join(re.findall(r"^#define AUDIO_.*$", source, re.M))
    functions = "\n".join(extract_function(source, name)
                          for name in ("configure_pwm_pin", "mute_output_pins", "audio_tick",
                                       "close_timer", "run_test"))
    output = ROOT / "logs/audio-song-validation"
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
    machine.emu_start(symbols["validate"] | 1, 0xFFF00, timeout=30000000)
    error = int.from_bytes(machine.mem_read(symbols["test_error"], 4), "little")
    passed = int.from_bytes(machine.mem_read(symbols["tests_passed"], 4), "little")
    if error or passed != 5:
        raise RuntimeError(f"Song validation failed: C line={error}, groups={passed}/5")
    print("PASS: 5 ARM groups; signed PWM/EOF, completion, cancel/timeout, PWM error, startup cleanup/clock")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
