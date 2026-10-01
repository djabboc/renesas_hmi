"""有时限地运行 RW007 联网测试；凭据只从运行输入获取，日志按完整行脱敏。"""

import argparse
import codecs
from datetime import datetime
import getpass
from pathlib import Path
import sys

from serial_test_port import SerialPort
from test_peripherals import wait_for_ready, run_command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM8")
    parser.add_argument("--ssid", required=True)
    arguments = parser.parse_args()
    if sys.stdin.isatty():
        password = getpass.getpass("Hotspot password: ")
    else:
        password = sys.stdin.readline().rstrip("\r\n")
    if any(character in arguments.ssid + password for character in '\r\n"'):
        raise ValueError("Quotes and newlines are not supported")
    if not 1 <= len(arguments.ssid.encode()) <= 32 or not 8 <= len(password.encode()) <= 31:
        raise ValueError("SSID must be 1..32 bytes and password 8..31 bytes")
    log_directory = Path(__file__).resolve().parent.parent / "logs"
    log_directory.mkdir(exist_ok=True)
    log_path = log_directory / f"internet_{datetime.now():%Y%m%d_%H%M%S}.log"
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    pending_line = ""
    output = ""
    print(f"Log: {log_path}", flush=True)
    try:
        with log_path.open("w", encoding="utf-8") as log, SerialPort(arguments.port) as serial:

            def record(data):
                nonlocal pending_line, output
                text = decoder.decode(data)
                output += text
                pending_line += text
                # Passwords may straddle serial reads; redact only after assembling a complete line.
                while "\n" in pending_line:
                    line, pending_line = pending_line.split("\n", 1)
                    safe_line = line.replace(password, "[redacted]")
                    print(safe_line)
                    log.write(safe_line + "\n")
                    log.flush()
                return text

            wait_for_ready(serial, record)
            command = f'hmi_test rw007-internet "{arguments.ssid}" "{password}"'
            try:
                completed = run_command(serial, command, 110, None, record)
            except (OSError, RuntimeError, TimeoutError) as error:
                # Shared runner timeout messages contain the command; redact that path too.
                raise RuntimeError(str(error).replace(password, "[redacted]")) from None
            if completed and "TEST RESULT rw007-internet PASS code=0" in output:
                return 0
            return 1
    finally:
        print(f"{arguments.port} released", flush=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
