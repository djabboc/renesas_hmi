"""通过 SWD 在 Cortex-M4 SRAM 执行真实录音处理函数，测量每块 CPU 周期。

不打开 COM8。运行期间暂停应用、装载临时 SRAM 程序，结束后复位原 Flash 固件。
分别测量 -O0 和 -O2；这是算法吞吐量检查，不代替 SSI/DTC 连续采集及听音。
python scripts/benchmark_audio_replay.py --dependencies logs/oled-validation/python
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import sys

from build_flash import find_studio_tool
from validate_audio_replay import extract_function

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <stdint.h>
#include <string.h>
typedef unsigned rt_tick_t;
typedef int fsp_err_t;
struct rt_semaphore { unsigned unused; };
#define __DMB() __asm volatile ("dmb" ::: "memory")
'''

BENCHMARK = r'''
volatile uint32_t benchmark_report[6] = {0x41554442, 0, 0, 0, 0, 0};
static uint8_t benchmark_recording[AUDIO_FRAMES];
void benchmark(void)
{
    __asm volatile ("cpsid i");
    volatile uint32_t *demcr = (uint32_t *)0xE000EDFC;
    volatile uint32_t *counter = (uint32_t *)0xE0001004;
    volatile uint32_t *control = (uint32_t *)0xE0001000;
    *demcr |= 1u << 24;
    *control |= 1u;
    memset(capture_statistics, 0, sizeof(capture_statistics));
    memset(lowpass_history, 0, sizeof(lowpass_history));
    lowpass_position = 0;
    filter_previous_input = 0;
    filter_previous_output = 0;
    capture_consumed = 0;
    uint32_t random = 1;
    for (unsigned block = 0; block < 32; ++block)
    {
        for (unsigned frame = 0; frame < AUDIO_CAPTURE_BLOCK_FRAMES; ++frame)
        {
            random = random * 1664525u + 1013904223u;
            capture_buffers[block % AUDIO_BUFFER_COUNT].samples[frame * 2u] = random & 0x00FFFFFFu;
            capture_buffers[block % AUDIO_BUFFER_COUNT].samples[frame * 2u + 1u] = 0;
        }
        uint32_t start = *counter;
        consume_block(benchmark_recording, block);
        uint32_t elapsed = *counter - start;
        if (elapsed > benchmark_report[2])
        {
            benchmark_report[2] = elapsed;
        }
        benchmark_report[3] += elapsed;
        ++benchmark_report[1];
    }
    benchmark_report[4] = capture_consumed;
    benchmark_report[5] = 0x50415353;
    __asm volatile ("bkpt 0");
    for (;;) {}
}
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies", type=Path)
    args = parser.parse_args()
    if args.dependencies is not None:
        sys.path.insert(0, str(args.dependencies.resolve()))
    from elftools.elf.elffile import ELFFile

    source = (ROOT / "src/test/test-audio-replay.c").read_text(encoding="utf-8")
    definitions = source[source.index("#define AUDIO_CLOCK_HZ"):source.index("/* 24 位数据右对齐")]
    functions = "\n".join(extract_function(source, name) for name in (
        "pcm24_signed", "encode_mulaw", "filter_microphone", "filter_downsample", "consume_block"))
    output = ROOT / "logs/audio-benchmark"
    output.mkdir(parents=True, exist_ok=True)
    harness = output / "benchmark.c"
    harness.write_text(PREFIX + definitions + functions + BENCHMARK, encoding="utf-8")
    studio = Path(os.environ.get("RTTHREAD_STUDIO", "C:/RT-ThreadStudio"))
    compiler = find_studio_tool(studio, "repo/Extract/ToolChain_Support_Packages/ARM/*/10.2.1/bin/arm-none-eabi-gcc.exe", "ARM GCC")
    pyocd = find_studio_tool(studio, "repo/Extract/Debugger_Support_Packages/RealThread/PyOCD/*/pyocd.exe", "PyOCD")
    for optimization in ("O0", "O2"):
        elf_path = output / (optimization + ".elf")
        subprocess.run([str(compiler), "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft",
                        "-" + optimization, "-nostartfiles",
                        "-Wl,-Ttext=0x20010000,-Tdata=0x20020000,-e,benchmark",
                        str(harness), "-o", str(elf_path), "-lc", "-lnosys"], check=True)
        with elf_path.open("rb") as stream:
            elf = ELFFile(stream)
            symbols = {symbol.name: symbol["st_value"]
                       for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
            initialization = []
            for index, segment in enumerate(elf.iter_segments()):
                if segment["p_type"] == "PT_LOAD" and segment["p_memsz"] > segment["p_filesz"]:
                    zero_file = output / f"{optimization}-bss-{index}.bin"
                    zero_file.write_bytes(bytes(segment["p_memsz"] - segment["p_filesz"]))
                    initialization.append('load "' + zero_file.as_posix() + '" ' + hex(segment["p_vaddr"] + segment["p_filesz"]))
        # 保持 BSP 的 MSP 监控范围，使用当前芯片给出的栈顶，不关闭栈保护。
        limits = subprocess.run([str(pyocd), "commander", "-M", "attach", "-t", "r7fa6m3ah",
                                 "-c", "read32 0x40000D08 8"], cwd=pyocd.parent,
                                capture_output=True, text=True, errors="replace", timeout=15, check=True)
        match = re.search(r"40000d08:\s*([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})", limits.stdout, re.I)
        if match is None:
            raise RuntimeError("Cannot read BSP stack bounds: " + limits.stdout)
        stack_top = int(match.group(2), 16) & ~7
        address = symbols["benchmark_report"]
        commands = output / (optimization + ".txt")
        commands.write_text("\n".join((
            "halt", 'load "' + elf_path.as_posix() + '"', *initialization, "wreg control 0", "wreg primask 1",
            "wreg sp " + hex(stack_top), "wreg pc " + hex(symbols["benchmark"] | 1),
            "go", "sleep 1000", "halt", "reg", "read32 0xE000ED28 24", "read32 0x40006140 16",
            "read32 " + hex(address) + " 24", "reset", "go", "")), encoding="utf-8")
        try:
            result = subprocess.run([str(pyocd), "commander", "-M", "attach", "-t", "r7fa6m3ah",
                                     "-f", "1000000", "-x", str(commands)], cwd=pyocd.parent,
                                    capture_output=True, text=True, errors="replace", timeout=25)
        finally:
            # 无论读取/执行是否成功，都复位回 Flash 固件，清除临时 RAM 状态。
            subprocess.run([str(pyocd), "commander", "-M", "attach", "-t", "r7fa6m3ah",
                            "-c", "reset"], cwd=pyocd.parent, capture_output=True, timeout=15, check=True)
        (output / (optimization + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode != 0:
            raise RuntimeError("SWD benchmark failed: " + result.stdout + result.stderr)
        words = []
        for line in result.stdout.splitlines():
            if re.match(r"\s*200200[01]0:", line):
                words.extend(int(word, 16) for word in re.findall(r"\b[0-9a-fA-F]{8}\b", line)[1:])
        if len(words) != 6 or words[0] != 0x41554442 or words[1] != 32 or words[4] != 32 or words[5] != 0x50415353:
            raise RuntimeError("Hardware benchmark incomplete: " + result.stdout + result.stderr)
        print(f"Cortex-M4 {optimization}: blocks={words[1]} max_cycles={words[2]} max_us@120MHz={words[2]/120:.1f} avg_us={words[3]/words[1]/120:.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
