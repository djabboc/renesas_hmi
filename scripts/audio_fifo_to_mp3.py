"""通过SWD临时运行CPU直读SSI FIFO的5秒录音对照，不打开COM8。

只构建：python scripts/audio_fifo_to_mp3.py --build-only
用户采集：python scripts/audio_fifo_to_mp3.py --run --output logs/mic-fifo-01

采集会暂停、覆盖原应用RAM，结束或异常时复位回Flash固件。
它保留官方时钟和SSI格式，绕过DTC、RT-Thread队列及板端滤波。
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib

from audio_record_to_mp3 import prepare_listening, write_wav
from build_flash import find_studio_tool
from mp3_to_array import find_ffmpeg, run_ffmpeg

ROOT = Path(__file__).resolve().parents[1]
FIRST_ADDRESS = 0x20000000
FIRST_FRAMES = 204800
SECOND_ADDRESS = 0x20065000
FRAMES = 240384
RATE = 48077
TEXT_ADDRESS = 0x20078000
DATA_ADDRESS = 0x2007A000
RAM_END = 0x20080000
SYMBOL_NAMES = (
    "R_GPT_Open", "R_GPT_Start", "R_SSI_Open", "R_SSI_Read", "R_IOPORT_PinCfg",
    "g_ioport_ctrl", "g_bsp_pin_cfg", "g_timer_cfg", "g_timer_ctrl", "g_i2s0_cfg", "g_i2s0_ctrl",
)
VERIFY_NAMES = (
    "R_GPT_Open", "R_GPT_Start", "R_SSI_Open", "R_SSI_Read", "R_IOPORT_PinCfg",
    "g_bsp_pin_cfg", "g_timer_cfg", "g_timer_extend", "g_i2s0_cfg", "g_i2s0_cfg_extend",
)


def read_elf(path: Path):
    """读取符号和装载段；复用本机既有依赖，不自动安装工具。"""
    sys.path.insert(0, str(ROOT / "logs/oled-validation/python"))
    from elftools.elf.elffile import ELFFile
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {symbol.name: (symbol["st_value"], symbol["st_size"])
                   for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
        segments = [(segment["p_vaddr"], segment.data(), segment["p_memsz"])
                    for segment in elf.iter_segments() if segment["p_type"] == "PT_LOAD"]
    return symbols, segments


def expected_symbol_bytes(name: str, symbols, segments) -> tuple[int, bytes]:
    """装载RAM程序前核对Flash函数/配置，拒绝与ELF不匹配的固件。"""
    address, size = symbols[name]
    address &= ~1
    if size == 0 or address >= 0x20000000:
        raise ValueError(f"不能核验Flash符号：{name}")
    for start, payload, _ in segments:
        offset = address - start
        if 0 <= offset and offset + size <= len(payload):
            return address, payload[offset:offset + size]
    raise ValueError(f"Flash符号未包含完整数据：{name}")


def build(output: Path, compiler: Path):
    flash_symbols, flash_segments = read_elf(ROOT / "Debug/rtthread.elf")
    includes = re.findall(r'-I"([^\"]+)"',
                          (ROOT / "Debug/src/test/subdir.mk").read_text(errors="replace"))
    elf_path = output / "fifo.elf"
    arguments = [str(compiler), "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard",
                 "-mfpu=fpv4-sp-d16", "-O2", "-Wall", "-Wextra", "-nostartfiles", "-std=gnu11"]
    arguments.extend("-I" + path for path in includes)
    arguments.extend(["-include", str(ROOT / "rtconfig_preinc.h"),
                      f"-Wl,-n,-Ttext={TEXT_ADDRESS:#x},-Tdata={DATA_ADDRESS:#x},-e,capture_fifo"])
    arguments.extend(f"-Wl,--defsym={name}={flash_symbols[name][0]:#x}" for name in SYMBOL_NAMES)
    arguments.extend([str(ROOT / "scripts/diagnostics/audio-fifo.c"), "-o", str(elf_path), "-lc", "-lnosys"])
    subprocess.run(arguments, cwd=ROOT, check=True)
    symbols, segments = read_elf(elf_path)
    initialization = []
    for index, (start, payload, size) in enumerate(segments):
        if not TEXT_ADDRESS <= start < start + size <= RAM_END:
            raise ValueError("临时代码或数据超出预留SRAM范围")
        if size > len(payload):
            zero_file = output / f"bss-{index}.bin"
            zero_file.write_bytes(bytes(size - len(payload)))
            initialization.append(f'load "{zero_file.as_posix()}" {start + len(payload):#x}')
    return elf_path, symbols, initialization, flash_symbols, flash_segments


def run_commander(pyocd: Path, commands: Path, timeout: int = 30):
    result = subprocess.run([str(pyocd), "commander", "-M", "attach", "-t", "r7fa6m3ah",
                             "-f", "1000000", "-x", str(commands)], cwd=pyocd.parent,
                            capture_output=True, text=True, errors="replace", timeout=timeout)
    commands.with_suffix(".log").write_text(result.stdout + result.stderr, encoding="utf-8")
    # commander可能在命令失败时仍返回0，因此同时检查文本错误和导出文件。
    if result.returncode != 0 or re.search(
        r"(?im)^\s*(?:Error:|Traceback|.*\bERROR\b|.*Failed to add data chunk)",
        result.stdout + result.stderr,
    ):
        raise RuntimeError(result.stdout + result.stderr)
    return result


def validate_report(payload: bytes) -> dict:
    if len(payload) != 96:
        raise ValueError("FIFO报告不完整")
    words = struct.unpack("<24I", payload)
    if words[0] != 0x4649464F or words[23] != 0x444F4E45 or words[1] != 3:
        raise ValueError("FIFO临时程序未完整运行；查看.swd/capture.log和report.bin")
    if any(words[2:9]) or words[18] != 0 or words[20] != 0:
        raise ValueError(f"FIFO初始化、接收或硬件错误：{words}")
    if words[14] != 144231 + FRAMES or words[15] != FRAMES:
        raise ValueError("FIFO采集帧数不足")
    record_cycles = (words[13] - words[12]) & 0xFFFFFFFF
    duration = record_cycles / 120000000
    if not 4.99 < duration < 5.01:
        raise ValueError(f"FIFO录音时长异常：{duration:.6f}s")
    minimum, maximum = struct.unpack("<ii", payload[64:72])
    return {"source": "CPU reads SSI FIFO; DTC and application queue bypassed",
            "source_frames": words[14], "saved_frames": words[15],
            "capture_duration_seconds": duration, "left24_minimum": minimum,
            "left24_maximum": maximum, "right24_peak": words[19],
            "ssicr": f"{words[9]:08X}", "ssiofr": f"{words[10]:08X}", "ssisr": f"{words[20]:08X}"}


def save_audio(prefix: Path, pcm: bytes, report: dict, ffmpeg: Path) -> None:
    if len(pcm) != FRAMES * 2:
        raise ValueError("FIFO样本文件不完整")
    listening, metrics = prepare_listening(pcm)
    raw_wav = Path(str(prefix) + ".wav")
    listen_wav = Path(str(prefix) + ".listen.wav")
    write_wav(raw_wav, pcm, RATE)
    write_wav(listen_wav, listening, RATE)
    for source, destination in ((raw_wav, Path(str(prefix) + ".raw.mp3")),
                                (listen_wav, Path(str(prefix) + ".mp3"))):
        run_ffmpeg(ffmpeg, ["-i", str(source), "-vn", "-ac", "1", "-ar", "48000",
                           "-c:a", "libmp3lame", "-b:a", "128k", str(destination)])
    metrics.update(report)
    metrics.update({"rate": RATE, "frames": FRAMES, "duration_seconds": FRAMES / RATE,
                    "pcm_crc32": f"{zlib.crc32(pcm):08X}",
                    "board_processing": "SSI LEFT PCM24 -> signed /256 -> PCM16; no filter/decimation",
                    "listening_processing": "remove whole-recording mean; one fixed gain; no noise reduction",
                    "acceptance": "Pending: recognizable spoken words required"})
    Path(str(prefix) + ".json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    print(json.dumps(metrics, indent=2), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--build-only", action="store_true")
    mode.add_argument("--run", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    studio = Path(os.environ.get("RTTHREAD_STUDIO", "C:/RT-ThreadStudio"))
    compiler = find_studio_tool(studio, "repo/Extract/ToolChain_Support_Packages/ARM/*/10.2.1/bin/arm-none-eabi-gcc.exe", "ARM GCC")
    if args.run and args.output is None:
        parser.error("--run需要新的--output编号")
    prefix = args.output.resolve() if args.run else ROOT / "logs/audio-fifo-build"
    work = Path(str(prefix) + ".swd")
    if args.run and (work.exists() or any(prefix.parent.glob(prefix.name + ".*"))):
        raise ValueError("输出前缀已存在，请换一个编号")
    work.mkdir(parents=True, exist_ok=True)
    elf_path, symbols, initialization, flash_symbols, flash_segments = build(work, compiler)
    print("FIFO diagnostic built; Flash firmware unchanged.", flush=True)
    if args.build_only:
        return 0
    ffmpeg = find_ffmpeg(None)
    pyocd = find_studio_tool(studio, "repo/Extract/Debugger_Support_Packages/RealThread/PyOCD/*/pyocd.exe", "PyOCD")
    expected = {}
    preflight_lines = ["reset", "go", "sleep 800", "halt", "read32 0x40000D08 8"]
    for name in VERIFY_NAMES:
        address, payload = expected_symbol_bytes(name, flash_symbols, flash_segments)
        file = work / ("flash-" + name + ".bin")
        expected[file] = payload
        preflight_lines.append(f'savemem {address:#x} {len(payload)} "{file.as_posix()}"')
    preflight = work / "preflight.txt"
    preflight.write_text("\n".join(preflight_lines) + "\n", encoding="utf-8")
    try:
        checked = run_commander(pyocd, preflight)
        for file, payload in expected.items():
            if not file.exists() or file.read_bytes() != payload:
                raise ValueError(f"已烧录Flash与当前ELF不匹配：{file.name}")
        match = re.search(r"40000d08:\s*([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})", checked.stdout, re.I)
        if match is None:
            raise ValueError("无法核实MSP栈范围")
        stack_low, stack_high = (int(word, 16) for word in match.groups())
        if not 0x20064000 <= stack_low <= stack_high < SECOND_ADDRESS:
            raise ValueError(f"MSP栈与预留区不符：{stack_low:#x}..{stack_high:#x}")
        report_file = work / "report.bin"
        first_file = work / "pcm-first.bin"
        second_file = work / "pcm-second.bin"
        commands = work / "capture.txt"
        commands.write_text("\n".join([
            "halt", f'load "{elf_path.as_posix()}"', *initialization,
            "wreg xpsr 0x01000000", "wreg control 0", "wreg primask 1", f"wreg sp {stack_high & ~7:#x}",
            f"wreg pc {symbols['capture_fifo'][0] | 1:#x}", "go", "sleep 9000", "halt", "reg",
            "read32 0xE000ED28 16",
            f'savemem {symbols["fifo_report"][0]:#x} 96 "{report_file.as_posix()}"',
            ""]), encoding="utf-8")
        print("即将采集：请以正常距离持续说话，直到采集完成提示。约3秒预热、保存后5秒。", flush=True)
        run_commander(pyocd, commands, timeout=25)
        report = validate_report(report_file.read_bytes())
        print("采集完成，可以停止说话；正在通过SWD导出已保存的5秒音频。", flush=True)
        export = work / "export.txt"
        export.write_text("\n".join([
            "halt",
            f'savemem {FIRST_ADDRESS:#x} {FIRST_FRAMES * 2} "{first_file.as_posix()}"',
            f'savemem {SECOND_ADDRESS:#x} {(FRAMES - FIRST_FRAMES) * 2} "{second_file.as_posix()}"',
            ""]), encoding="utf-8")
        run_commander(pyocd, export, timeout=60)
        first_pcm = first_file.read_bytes()
        second_pcm = second_file.read_bytes()
        if len(first_pcm) != FIRST_FRAMES * 2 or len(second_pcm) != (FRAMES - FIRST_FRAMES) * 2:
            raise ValueError("两段FIFO样本文件长度不符")
        pcm = first_pcm + second_pcm
    finally:
        # 无论探针、采集或读取在哪一步失败，都尝试恢复原Flash应用。
        recovery = work / "reset.txt"
        recovery.write_text("reset\ngo\n", encoding="utf-8")
        run_commander(pyocd, recovery, timeout=20)
    print("采集结束，已复位回原Flash固件；COM8未打开。正在保存音频。", flush=True)
    save_audio(prefix, pcm, report, ffmpeg)
    print(f"Saved: {prefix}.mp3 / .wav / .listen.wav / .raw.mp3 / .json", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
