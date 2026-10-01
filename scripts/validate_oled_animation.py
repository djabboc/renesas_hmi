"""Compile the actual OLED example with fake RT/I2C and execute it in an ARM emulator.

No serial port or board access. Requires unicorn and pyelftools, and the Studio ARM GCC.
Run: python scripts/validate_oled_animation.py --dependencies logs/oled-validation/python
The fake SSD1306 RAM checks address packets, erased pixels, retries and text wrap.
This validates logic, not physical timing, I2C signalling or visible OLED persistence.
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
STUB = r'''#ifndef OLED_RT_STUB_H
#define OLED_RT_STUB_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef uint32_t rt_uint32_t;
typedef uint64_t rt_uint64_t;
typedef uint32_t rt_tick_t;
typedef int rt_ssize_t;
typedef void *rt_event_t;
struct rt_thread { void *parameter; int error; };
struct rt_i2c_bus_device { int dummy; };
#define RT_NULL NULL
#define RT_UNUSED(x) (void)(x)
#define RT_EVENT_FLAG_OR 1
#define RT_EOK 0
#define RT_EIO 8
#define RT_EINTR 9
#define RT_ENOSYS 10
#define RT_TICK_PER_SECOND 1000
struct rt_thread *rt_thread_self(void);
int rt_event_recv(rt_event_t, int, int, int, rt_uint32_t *);
rt_tick_t rt_tick_get(void);
void rt_thread_mdelay(unsigned);
void rt_thread_delay(unsigned);
void rt_kprintf(const char *, ...);
void *rt_device_find(const char *);
rt_ssize_t rt_i2c_master_send(struct rt_i2c_bus_device *, rt_uint16_t, int, const void *, unsigned);
#endif
'''
HARNESS = r'''#include "rtthread.h"
#include "oled_source.c"
static struct rt_thread thread;
static struct rt_i2c_bus_device bus;
static unsigned clock_ticks, calls, fail_call, cancelled, page, column;
static unsigned invalid_packet, data_bytes;
static unsigned char ram[8][128];
unsigned test_error, tests_passed;
unsigned char snapshots[3][1024];
static struct { unsigned before; struct oled_display screen; unsigned after; } guarded;
#define CHECK(condition) do { if (!(condition)) { test_error = __LINE__; return; } } while (0)
struct rt_thread *rt_thread_self(void) { return &thread; }
int rt_event_recv(rt_event_t e, int b, int f, int t, rt_uint32_t *r) { return cancelled ? 0 : -1; }
rt_tick_t rt_tick_get(void) { return clock_ticks; }
void rt_thread_mdelay(unsigned ms) { clock_ticks += ms; }
void rt_thread_delay(unsigned ticks) { clock_ticks += ticks; }
void rt_kprintf(const char *format, ...) { }
void *rt_device_find(const char *name) { return &bus; }
rt_ssize_t rt_i2c_master_send(struct rt_i2c_bus_device *b, rt_uint16_t addr, int flags,
                            const void *buffer, unsigned length)
{
    const unsigned char *packet = buffer;
    ++calls;
    ++clock_ticks;
    if (calls == fail_call || addr != 0x3C) { return 0; }
    if (packet[0] == 0 && length == 4 && (packet[1] & 0xF8) == 0xB0)
    {
        page = packet[1] & 7;
        column = (packet[2] & 15) | ((packet[3] & 15) << 4);
    }
    else if (packet[0] == 0x40)
    {
        if (page >= 8 || column + length - 1 > 128)
        {
            invalid_packet = 1;
            return 0;
        }
        memcpy(&ram[page][column], packet + 1, length - 1);
        data_bytes += length - 1;
    }
    return length;
}
void validate(void)
{
    struct oled_display *d = &guarded.screen;
    guarded.before = 0x12345678;
    guarded.after = 0x87654321;
    d->bus = &bus;
    d->address = 0x3C;
    memset(ram, 0xAA, sizeof(ram));
    CHECK(refresh_oled(d) == 0 && data_bytes == 1024);
    CHECK(memcmp(ram, d->pixels, 1024) == 0);
    unsigned previous = calls;
    CHECK(refresh_oled(d) == 0 && calls == previous);
    ++tests_passed; /* Unknown initial RAM forced full, unchanged frame sends nothing. */

    draw_moving_block(d, 0);
    CHECK(refresh_oled(d) == 0);
    previous = data_bytes;
    draw_moving_block(d, 33);
    CHECK(refresh_oled(d) == 0 && data_bytes - previous <= 32);
    CHECK(memcmp(ram, d->pixels, 1024) == 0);
    for (unsigned t = 66; t < 4000; t += 33)
    {
        draw_moving_block(d, t);
        CHECK(refresh_oled(d) == 0 && memcmp(ram, d->pixels, 1024) == 0);
    }
    ++tests_passed; /* Moving pixels also erase their former positions. */

    draw_checkerboard(d);
    fail_call = calls + 2; /* Fail the data packet after accepting its address command. */
    CHECK(refresh_oled(d) == -RT_EIO && d->shadow_valid == 0);
    fail_call = 0;
    previous = data_bytes;
    CHECK(refresh_oled(d) == 0 && data_bytes - previous == 1024);
    CHECK(memcmp(ram, d->pixels, 1024) == 0);
    ++tests_passed;

    cancelled = 1;
    thread.parameter = &thread;
    previous = calls;
    CHECK(refresh_oled(d) == -RT_EINTR && calls == previous);
    cancelled = 0;
    ++tests_passed;

    draw_cube(d, 1500);
    memcpy(snapshots[0], d->pixels, 1024);
    draw_cube(d, 7000);
    memcpy(snapshots[1], d->pixels, 1024);
    CHECK(memcmp(snapshots[0], snapshots[1], 1024) != 0);
    for (unsigned t = 0; t < 16000; t += 137)
    {
        draw_cube(d, t);
        CHECK(refresh_oled(d) == 0 && memcmp(ram, d->pixels, 1024) == 0);
    }
    ++tests_passed;

    draw_marquee(d, 0);
    unsigned char first_text[256];
    memcpy(first_text, d->pixels[3], 256);
    draw_marquee(d, 28); /* One pixel at 36 pixels/second. */
    CHECK(memcmp(first_text, d->pixels[3], 256) != 0);
    unsigned width = (sizeof(marquee_text) - 1) * OLED_GLYPH_ADVANCE;
    draw_marquee(d, width * 1000); /* Exactly 36 full text cycles, independent of width. */
    CHECK(memcmp(first_text, d->pixels[3], 256) == 0);
    draw_marquee(d, 2200);
    memcpy(snapshots[2], d->pixels, 1024);
    for (unsigned t = 0; t < 36000; t += 257)
    {
        draw_marquee(d, t);
        CHECK(refresh_oled(d) == 0 && memcmp(ram, d->pixels, 1024) == 0);
    }
    ++tests_passed;

    CHECK(guarded.before == 0x12345678 && guarded.after == 0x87654321 && !invalid_packet);
    CHECK(play_animation(d, OLED_SCENE_BLOCK, 100) == 0);
    CHECK(play_animation(d, OLED_SCENE_CUBE, 100) == 0);
    CHECK(play_animation(d, OLED_SCENE_TEXT, 100) == 0);
    ++tests_passed;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependencies", type=Path)
    parser.add_argument("--compiler", type=Path)
    args = parser.parse_args()
    if args.dependencies:
        sys.path.insert(0, str(args.dependencies.resolve()))
    from elftools.elf.elffile import ELFFile
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR

    compiler = args.compiler
    if compiler is None:
        studio = Path(os.environ.get("RTTHREAD_STUDIO", "C:/RT-ThreadStudio"))
        compiler = next(studio.glob("repo/Extract/ToolChain_Support_Packages/ARM/*/10.2.1/bin/arm-none-eabi-gcc.exe"))
    output = ROOT / "logs/oled-validation"
    output.mkdir(parents=True, exist_ok=True)
    for name in ("rtthread.h", "rtdevice.h"):
        (output / name).write_text(STUB, encoding="utf-8")
    (output / "oled_source.c").write_bytes((ROOT / "src/test/test-pmod-i2c.c").read_bytes())
    (output / "harness.c").write_text(HARNESS, encoding="utf-8")
    elf_path = output / "harness.elf"
    subprocess.run([str(compiler), "-mcpu=cortex-m3", "-mthumb", "-mfloat-abi=soft", "-O1",
                    "-nostartfiles", "-Wl,-Ttext=0x10000,-Tdata=0x400000,-e,validate",
                    "-I", str(output), str(output / "harness.c"), "-o", str(elf_path), "-lm", "-lc", "-lnosys"], check=True)
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
    def read_integer(name):
        return int.from_bytes(machine.mem_read(symbols[name], 4), "little")
    error = read_integer("test_error")
    passed = read_integer("tests_passed")
    if error or passed != 7:
        raise RuntimeError(f"OLED validation failed: C harness line={error}, groups passed={passed}/7")
    (output / "snapshots.bin").write_bytes(machine.mem_read(symbols["snapshots"], 3 * 1024))
    print("PASS: 7 ARM execution checks; delta erase, retry, cancellation, cube, wrap and packet bounds")
    print("Framebuffers: logs/oled-validation/snapshots.bin (three 128x64 page-layout frames)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
