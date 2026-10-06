"""提取audio-record真实C函数，在ARM模拟器检查整段PCM/抗混叠/CRC/导出/生命周期。

python scripts/validate_audio_record.py --dependencies logs/oled-validation/python
FSP桩不替代麦克风实物响应；不打开COM8。
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys
from validate_audio_replay import STUB, HARNESS, extract_function

ROOT = Path(__file__).resolve().parents[1]

EXPORT_STUB = r'''
static unsigned export_begin, export_end, export_bytes, export_error;
static uint32_t export_crc;
static uint8_t exported[80128];
static unsigned second_payload[2000];
static void rt_kprintf(const char *format, ...)
{
    char buffer[128]; va_list args;
    va_start(args, format); int length = vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    if (length > 0 && (unsigned)length > max_log_bytes) { max_log_bytes = (unsigned)length; }
    if (strncmp(buffer, "MICREC BEGIN", 12) == 0)
    {
        ++export_begin;
        if (g_i2s0_ctrl.open || g_timer_ctrl.open || semaphore_active || mock_thread.current_priority != 21)
        { ++export_error; }
        unsigned rate, frames, bytes;
        if (sscanf(buffer, "MICREC BEGIN v=1 rate=%u frames=%u bits=16 channels=1 bytes=%u crc32=%x",
                   &rate, &frames, &bytes, &export_crc) != 4 ||
            rate != 8013 || frames != 40064 || bytes != 80128) { ++export_error; }
    }
    if (strncmp(buffer, "MICREC DATA", 11) == 0)
    {
        unsigned offset; char hex[65];
        if (sscanf(buffer, "MICREC DATA %u %64s", &offset, hex) != 2 || offset != export_bytes)
        { ++export_error; return; }
        unsigned count = strlen(hex) / 2;
        if (count != 32 || offset + count > sizeof(exported)) { ++export_error; return; }
        for (unsigned index = 0; index < count; ++index)
        {
            unsigned value;
            if (sscanf(hex + index * 2, "%2x", &value) != 1) { ++export_error; return; }
            exported[export_bytes++] = (uint8_t)value;
        }
    }
    if (strncmp(buffer, "MICREC END", 10) == 0)
    {
        ++export_end;
        unsigned bytes, crc;
        if (sscanf(buffer, "MICREC END bytes=%u crc32=%x", &bytes, &crc) != 2 ||
            bytes != export_bytes || crc != export_crc) { ++export_error; }
    }
}
static void *rt_malloc(unsigned bytes)
{
    ++malloc_calls; heap_bytes += bytes;
    if (allocation_fail == malloc_calls) { return NULL; }
    unsigned *buffer = payload;
    unsigned capacity = sizeof(payload);
    if (malloc_calls == 2) { buffer = second_payload; capacity = sizeof(second_payload); }
    if (bytes > capacity) { return NULL; }
    memset(buffer, 0xA5, capacity);
    return buffer;
}
'''

RAW_HARNESS = r'''
unsigned test_error, tests_passed;
#define CHECK(condition) do { if (!(condition)) { test_error = __LINE__; return; } } while (0)
static void setup(void)
{
    reset(); export_begin = 0; export_end = 0; export_bytes = 0; export_error = 0; heap_bytes = 0;
    capture_buffers = (struct capture_buffer *)second_payload;
}
void validate(void)
{
    CHECK(pcm24_signed(0x007FFFFF) == 8388607 && pcm24_signed(0x00800000) == -8388608);
    CHECK(pcm24_signed(0xFFFFFFFF) == -1 && pcm24_signed(0xFF7FFFFF) == 8388607);
    CHECK(raw_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    ++tests_passed;

    setup(); uint8_t *data = (uint8_t *)(payload + 1);
    payload[0] = AUDIO_GUARD; payload[(RAW_BYTES + 4) / 4] = AUDIO_GUARD;
    CHECK(capture_audio(data) == TEST_PASS && hardware_closed());
    CHECK(raw_stored == 40064 && absolute_frame == 288384 && capture_completed == 751);
    CHECK(capture_consumed == 751 && ssi_reads == 751 && capture_pending_peak == 1);
    CHECK(clock_ms >= 5998 && clock_ms <= 6001 && capture_overruns == 0);
    /* 独立整数卷积参考：输入为192帧周期方波，滤波后每32个输出重复。
     * 核对整段40064个PCM16，包括符号、字节序及预热后第一帧位置。 */
    int expected_pcm[32];
    for (unsigned phase = 0; phase < 32; ++phase)
    {
        int64_t sum = 0;
        unsigned input_frame = RAW_START_FRAME + phase * 6 + 5;
        for (unsigned tap = 0; tap < 127; ++tap)
        {
            int value = 262144;
            if ((input_frame - tap) / 6 % 32 >= 16) { value = -262144; }
            sum += (int64_t)value * lowpass_coefficients[tap];
        }
        expected_pcm[phase] = (int)(sum / 32768) / 32;
    }
    for (unsigned index = 0; index < RAW_FRAMES; ++index)
    {
        unsigned value = data[index * 2] | ((unsigned)data[index * 2 + 1] << 8);
        int actual = (int)(int16_t)value;
        CHECK(actual == expected_pcm[index % 32]);
    }
    CHECK(payload[0] == AUDIO_GUARD && payload[(RAW_BYTES + 4) / 4] == AUDIO_GUARD);
    unsigned frames = 0;
    for (unsigned second = 0; second < 5; ++second)
    {
        CHECK(capture_statistics[second].right_peak == 0 && capture_statistics[second].upper_nonzero == 0);
        frames += capture_statistics[second].frames;
    }
    CHECK(frames == 240384);
    ++tests_passed;

    CHECK(export_raw(data) == TEST_WAIT && export_begin == 1 && export_end == 1);
    CHECK(export_bytes == RAW_BYTES && memcmp(data, exported, RAW_BYTES) == 0 && !export_error);
    CHECK(export_crc == raw_crc32(data, RAW_BYTES) && max_log_bytes < 126);
    ++tests_passed;

    setup(); CHECK(run_test() == TEST_WAIT && hardware_closed() && capture_buffers == NULL);
    CHECK(malloc_calls == 2 && free_calls == 2 && heap_bytes == 86296);
    CHECK(export_begin == 1 && export_end == 1 && export_bytes == 80128 && !export_error);
    ++tests_passed;

    const unsigned cancel_times[] = {30, 3500, 9050};
    for (unsigned index = 0; index < 3; ++index)
    {
        setup(); cancel_at = cancel_times[index];
        CHECK(run_test() == -RT_EINTR && hardware_closed() && malloc_calls == free_calls);
        CHECK(clock_ms <= cancel_at + 12 && capture_buffers == NULL && export_end == 0);
    }
    ++tests_passed;

    for (unsigned failure = 1; failure <= 2; ++failure)
    {
        setup(); allocation_fail = failure;
        CHECK(run_test() == -RT_ENOMEM && hardware_closed() && free_calls == failure - 1);
    }
    setup(); corrupt_guard = 1;
    CHECK(run_test() == -RT_ERROR && hardware_closed() && free_calls == 2 && export_begin == 0);
    ++tests_passed;

    setup(); ssi_freeze = 1; CHECK(capture_audio(data) == -RT_ETIMEOUT && hardware_closed());
    setup(); ssi_early_idle = 1; CHECK(capture_audio(data) == -RT_ERROR && hardware_closed());
    setup(); wake_delay = 17; CHECK(capture_audio(data) == -RT_EFULL && hardware_closed());
    CHECK(capture_overruns == 1 && capture_pending_peak == 2 && ssi_reads == 2);
    const unsigned read_failures[] = {1, 5, 751};
    for (unsigned index = 0; index < 3; ++index)
    {
        setup(); fail_read_at = read_failures[index];
        CHECK(capture_audio(data) == -RT_ERROR && hardware_closed());
    }
    ++tests_passed;

    for (unsigned failure = 1; failure <= 3; ++failure)
    {
        setup(); failed_operation = failure;
        CHECK(capture_audio(data) == -RT_ERROR && hardware_closed());
    }
    setup(); ssi_stop_fail = 1; CHECK(capture_audio(data) == -RT_ERROR && hardware_closed());
    setup(); ssi_no_idle = 1; CHECK(capture_audio(data) == -RT_ETIMEOUT && hardware_closed());
    setup(); semaphore_fail = 1; CHECK(capture_audio(data) == -RT_ERROR && hardware_closed());
    setup(); priority_fail = 1; CHECK(capture_audio(data) == -RT_ERROR && hardware_closed());
    ++tests_passed;

    memset(lowpass_history, 0, sizeof(lowpass_history)); lowpass_position = 0;
    int sample = 0;
    for (unsigned frame = 0; frame < 384; ++frame)
    {
        downsample_pcm24(123456, frame, &sample);
    }
    CHECK(sample == 123456);
    memset(lowpass_history, 0, sizeof(lowpass_history)); lowpass_position = 0;
    for (unsigned frame = 0; frame < 384; ++frame)
    {
        int input = 100000;
        if (frame % 2 != 0) { input = -100000; }
        downsample_pcm24(input, frame, &sample);
    }
    CHECK(sample < 200 && sample > -200);
    ++tests_passed;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies", type=Path)
    args = parser.parse_args()
    if args.dependencies is not None:
        sys.path.insert(0, str(args.dependencies.resolve()))
    from elftools.elf.elffile import ELFFile
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR
    source = (ROOT / "src/test/test-audio-record.c").read_text(encoding="utf-8")
    definitions = source[source.index("#define AUDIO_CLOCK_HZ"):source.index("/* 右对齐")]
    functions = "\n".join(extract_function(source, name) for name in (
        "pcm24_signed", "downsample_pcm24", "stop_capture", "mic_callback", "consume_block", "wait_capture_idle",
        "close_timer", "capture_samples", "capture_audio", "raw_crc32", "export_raw", "run_test"))
    stub = STUB[:STUB.index("static void rt_kprintf")] + EXPORT_STUB + STUB[STUB.index("static void rt_free"):]
    mocks = HARNESS[HARNESS.index("static void mock_capture_step"):HARNESS.index("void validate(void)")]
    output = ROOT / "logs/audio-record-validation"
    output.mkdir(parents=True, exist_ok=True)
    harness = output / "harness.c"
    harness.write_text(stub + "\n" + definitions + functions + mocks + RAW_HARNESS, encoding="utf-8")
    studio = Path(os.environ.get("RTTHREAD_STUDIO", "C:/RT-ThreadStudio"))
    compiler = next(studio.glob("repo/Extract/ToolChain_Support_Packages/ARM/*/10.2.1/bin/arm-none-eabi-gcc.exe"))
    elf_path = output / "harness.elf"
    subprocess.run([str(compiler), "-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft", "-O2",
                    "-nostartfiles", "-Wl,-Ttext=0x10000,-Tdata=0x400000,-e,validate",
                    str(harness), "-o", str(elf_path), "-lc", "-lnosys"], check=True)
    machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    machine.mem_map(0, 0x810000)
    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        for segment in elf.iter_segments():
            if segment["p_type"] == "PT_LOAD":
                machine.mem_write(segment["p_vaddr"], segment.data())
        symbols = {symbol.name: symbol["st_value"] for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
    machine.reg_write(UC_ARM_REG_SP, 0x7FF000)
    machine.reg_write(UC_ARM_REG_LR, 0xFFF01)
    machine.emu_start(symbols["validate"] | 1, 0xFFF00, timeout=60000000)
    error = int.from_bytes(machine.mem_read(symbols["test_error"], 4), "little")
    passed = int.from_bytes(machine.mem_read(symbols["tests_passed"], 4), "little")
    if error or passed != 9:
        raise RuntimeError(f"Record validation failed: C line={error}, groups={passed}/9")
    print("PASS: 9 ARM groups; full capture, all PCM16 samples versus independent convolution, offset/CRC export, post-close export, heap/guards, cancel/error/IRQ/IPC/priority cleanup")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
