"""在 ARM 模拟器中执行实际回放代码的 PCM 处理与 PWM 回调。

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
#define RT_UNUSED(value) (void)(value)
#define RT_ERROR 1
#define RT_EINTR 9
#define TEST_PASS 0
#define FSP_SUCCESS 0
#define GPT_IO_PIN_GTIOCA 0
#define GPT_IO_PIN_GTIOCB 1
typedef int fsp_err_t;
typedef unsigned rt_tick_t;
typedef struct { int unused; } timer_callback_args_t;
static unsigned clock_ms, stop_at, duties[2], duty_calls;
static int force_pwm_error, g_timer6_ctrl;
static volatile unsigned playback_position;
static unsigned playback_frame_count;
static const int16_t *playback_samples;
static volatile fsp_err_t playback_error;
static rt_tick_t rt_tick_get(void) { return clock_ms; }
static int test_cancelled(void) { return stop_at && clock_ms >= stop_at; }
static int test_elapsed(rt_tick_t start, unsigned ms) { return clock_ms - start >= ms; }
static void rt_thread_mdelay(unsigned ms) { clock_ms += ms; }
static void rt_kprintf(const char *format, ...) { }
static fsp_err_t R_GPT_DutyCycleSet(void *timer, unsigned duty, unsigned pin)
{
    duties[pin] = duty;
    ++duty_calls;
    return force_pwm_error;
}
'''

HARNESS = r'''
unsigned test_error, tests_passed;
static struct { uint32_t before; int16_t samples[AUDIO_FRAMES * 2]; uint32_t after; } guarded;
#define CHECK(condition) do { if (!(condition)) { test_error = __LINE__; return; } } while (0)
void validate(void)
{
    int16_t *samples = guarded.samples;
    guarded.before = 0x12345678;
    guarded.after = 0x87654321;
    for (unsigned i = 0; i < AUDIO_FRAMES; ++i)
    {
        samples[i * 2] = 5000;
        if (i % 32 < 16) { samples[i * 2] += 1000; }
        else { samples[i * 2] -= 1000; }
        samples[i * 2 + 1] = -20000; /* 右声道不得影响左声道输出。 */
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
        samples[i * 2] = -1234;
        samples[i * 2 + 1] = (int16_t)i;
    }
    CHECK(prepare_playback(samples) == -RT_ERROR); /* 全直流没有可回放声音。 */
    ++tests_passed;

    for (unsigned i = 0; i < AUDIO_FRAMES; ++i)
    {
        samples[i * 2] = INT16_MIN;
        if (i % 2) { samples[i * 2] = INT16_MAX; }
        samples[i * 2 + 1] = 0;
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
        samples[i * 2] = -1001;
        if (i % 2) { samples[i * 2] = -999; }
        samples[i * 2 + 1] = 32767;
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
    functions = "\n".join(extract_function(source, name)
                          for name in ("wait_ms", "prepare_playback", "audio_tick"))
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
    machine.emu_start(symbols["validate"] | 1, 0xFFF00, timeout=30000000)
    error = int.from_bytes(machine.mem_read(symbols["test_error"], 4), "little")
    passed = int.from_bytes(machine.mem_read(symbols["tests_passed"], 4), "little")
    if error or passed != 6:
        raise RuntimeError(f"Audio validation failed: C line={error}, groups={passed}/6")
    print("PASS: 6 ARM checks; stereo/DC, silence, signed limits, gain cap, PWM, cancellation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
