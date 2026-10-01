"""有时限地执行 hmi_test 并保留日志；结束或异常时释放所有串口。"""

import argparse
import codecs
from datetime import datetime
from pathlib import Path
import re
import time
from typing import Callable

from serial_test_port import SerialPort

READY_TIMEOUT_SECONDS = 4
USB_ECHO_TIMEOUT_SECONDS = 5
STATUS_COMMANDS = {"hmi_test status", "hmi_test help", "hmi_test stop"}
Record = Callable[[bytes], str]


def parse_arguments() -> argparse.Namespace:
    """只接受测试命令，避免把其他 MSH 命令误发到板卡。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM8")
    parser.add_argument(
        "--command",
        action="append",
        help='e.g. "hmi_test eth-mac"; repeat for sequential tests',
    )
    parser.add_argument(
        "--timeout", type=float, default=120, help="Per-command time limit, seconds"
    )
    parser.add_argument("--usb-port", help="System USB CDC port for binary echo comparison")
    arguments = parser.parse_args()
    if not 1 <= arguments.timeout <= 300:
        parser.error("timeout must be 1..300")
    baseline = [
        "eth-phy",
        "eth-mac",
        "eth-phyloop",
        "sd-info",
        "sd-read",
        "can-loop",
        "audio-mic",
        "usb-probe",
        "gpio-led",
        "gpio-inputs",
        "rtc-tick",
        "rtc-alarm",
        "adc-sample",
        "lcd-colors",
        "touch-info",
        "graphics-g2d",
        "graphics-jpeg",
        "rw007-info",
    ]
    arguments.command = arguments.command or [f"hmi_test {name}" for name in baseline]
    for command in arguments.command:
        if not re.fullmatch(r"hmi_test [a-z0-9 -]+", command) or len(command) > 78:
            parser.error("Only hmi_test commands accepted")
    return arguments


def wait_for_ready(serial: SerialPort, record: Record) -> None:
    """先确认板卡空闲；随后每条命令都由板卡创建一条独立测试线程。"""
    serial.write(b"\rhmi_test status\r")
    started_at = time.monotonic()
    output = ""
    while time.monotonic() - started_at < READY_TIMEOUT_SECONDS:
        output += record(serial.read())
        if "TEST STATUS ready=1 busy=0" in output:
            return
    raise RuntimeError("Peripheral suite is not ready/idle; no test sent")


def verify_usb_echo(port: str, record: Record) -> bool:
    """从独立系统 USB 串口核对二进制回显，包含 NUL/FF 与跨包数据。"""
    payload = bytes(range(256)) + b"HMI\x00\xff\r\n" * 37
    with SerialPort(port) as usb:
        usb.write(payload)
        received = bytearray()
        started_at = time.monotonic()
        while (
            len(received) < len(payload)
            and time.monotonic() - started_at < USB_ECHO_TIMEOUT_SECONDS
        ):
            received.extend(usb.read())
    passed = received == payload
    verdict = "PASS" if passed else "FAIL"
    record(f"\nHOST USB ECHO {verdict} bytes={len(received)}/{len(payload)}\n".encode())
    return passed


def run_command(
    serial: SerialPort, command: str, timeout: float, usb_port: str | None, record: Record
) -> bool:
    """执行一个阶段；WAIT/SKIP 仅记录，只有明确 FAIL/协议错误使脚本失败。"""
    serial.write((command + "\r").encode())
    started_at = time.monotonic()
    output = ""
    echo_checked = False
    echo_passed = True

    while time.monotonic() - started_at < timeout:
        output += record(serial.read())
        if usb_port and not echo_checked and "USB CDC configured" in output:
            echo_passed = verify_usb_echo(usb_port, record)
            echo_checked = True
        if "assertion failed" in output or "HardFault" in output:
            raise RuntimeError("Board fault; serial port will be released")
        if "TEST UNKNOWN" in output or "TEST ERROR" in output:
            raise RuntimeError("Board rejected the test command")
        if "TEST BUSY" in output:
            raise RuntimeError("Board rejected command as busy")

        # 普通测试由工作线程结束；status/help/stop 在 MSH 中直接返回。
        completion_marker = "msh >" if command in STATUS_COMMANDS else "TEST IDLE"
        if completion_marker in output:
            break
    else:
        serial.write(b"hmi_test stop\r")
        raise TimeoutError(
            f"{command} timed out; stop requested; COM released " "(reset if driver is stuck)"
        )

    if command not in STATUS_COMMANDS and "TEST RESULT " not in output:
        raise RuntimeError("Missing test result before IDLE")
    if re.search(r"TEST RESULT .* FAIL ", output):
        return False
    if usb_port and command == "hmi_test usb-echo":
        return echo_checked and echo_passed
    return echo_passed


def collect_resource_status(serial: SerialPort, record: Record) -> None:
    """仅短暂采集线程栈和堆信息，不留下后台串口监视器。"""
    serial.write(b"list thread\rfree\r")
    started_at = time.monotonic()
    while time.monotonic() - started_at < 0.5:
        record(serial.read())


def main() -> int:
    arguments = parse_arguments()
    log_directory = Path(__file__).resolve().parent.parent / "logs"
    log_directory.mkdir(exist_ok=True)
    log_path = log_directory / f"peripherals_{datetime.now():%Y%m%d_%H%M%S}.log"
    print(f"Log: {log_path}", flush=True)
    all_passed = True

    # 内层异常也会触发 SerialPort.__exit__，确保 COM8 不被持续占用。
    with log_path.open("w", encoding="utf-8") as log, SerialPort(arguments.port) as serial:
        decoder = codecs.getincrementaldecoder("utf-8")(errors="backslashreplace")

        def record(data: bytes) -> str:
            # 中文 SSID 可能跨 ReadFile 分块，用增量解码器保留半个字符。
            text = decoder.decode(data)
            print(text, end="", flush=True)
            log.write(text)
            log.flush()
            return text

        wait_for_ready(serial, record)
        for command in arguments.command:
            passed = run_command(serial, command, arguments.timeout, arguments.usb_port, record)
            all_passed = passed and all_passed
        collect_resource_status(serial, record)
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
