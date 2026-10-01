"""板上验证一次性测试线程的互斥、停止和重复 GUI 运行后的资源回收。"""

import argparse
import codecs
from datetime import datetime
from pathlib import Path
import re
import time

from serial_test_port import SerialPort
from test_peripherals import wait_for_ready


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM8")
    args = parser.parse_args()
    log_path = (
        Path(__file__).resolve().parent.parent
        / "logs"
        / f"lifecycle_{datetime.now():%Y%m%d_%H%M%S}.log"
    )
    log_path.parent.mkdir(exist_ok=True)
    print(f"Log: {log_path}", flush=True)
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    with log_path.open("w", encoding="utf-8") as log, SerialPort(args.port) as port:

        def record(data):
            text = decoder.decode(data)
            print(text, end="", flush=True)
            log.write(text)
            log.flush()
            return text

        def receive_until(marker, timeout=8):
            output = ""
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                output += record(port.read())
                if "assertion failed" in output or "hard fault" in output:
                    raise RuntimeError("Board fault")
                if marker in output:
                    return output
            raise TimeoutError(f"Missing marker {marker}")

        def management(command):
            port.write((command + "\r").encode())
            return receive_until("msh >")

        def measure_idle_heap():
            # A host round trip allows the kernel to finish freeing the exited thread object.
            state = management("hmi_test status")
            assert "busy=0" in state, state
            threads = management("list thread")
            assert not re.search(r"^hmitest\s", threads, re.M), threads
            heap = management("free")
            return int(re.search(r"available:\s*(\d+)", heap)[1])

        wait_for_ready(port, record)
        invalid = management("hmi_test nonexistent")
        assert "TEST UNKNOWN" in invalid and "TEST BEGIN" not in invalid
        invalid = management("hmi_test eth-mac extra")
        assert "TEST BEGIN" not in invalid
        invalid = management("hmi_test rw007-internet")
        assert "TEST ERROR" in invalid and "TEST BEGIN" not in invalid
        snapshots = []
        try:
            for cycle in range(3):
                for name, marker in (
                    ("lcd-lvgl", "LVGL: ready"),
                    ("lcd-touch-lvgl", "TOUCH-LVGL: ready"),
                    ("lcd-touch", "LCD-TOUCH: ready"),
                ):
                    port.write(f"hmi_test {name}\r".encode())
                    receive_until(marker)
                    # Two commands in one write exercise overlapping requests while hardware is active.
                    port.write(b"hmi_test eth-mac\rhmi_test touch-info\r")
                    rejected = receive_until("TEST BUSY")
                    if rejected.count("TEST BUSY") < 2:
                        rejected += receive_until("TEST BUSY")
                    assert rejected.count("TEST BUSY") == 2 and "TEST BEGIN eth-mac" not in rejected
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        record(port.read())
                    port.write(b"hmi_test stop\r")
                    ended = receive_until("TEST IDLE")
                    assert f"TEST RESULT {name} FAIL code=-9" in ended, ended
                    assert "close failed" not in ended, ended
                    measure_idle_heap()
                snapshots.append(measure_idle_heap())
                record(f"\nHOST lifecycle cycle={cycle} available={snapshots[-1]}\n".encode())
            # The first cycle may populate LVGL shared caches. Later cycles must reach a plateau.
            assert snapshots[1] == snapshots[2], snapshots
            record(f"\nHOST LIFECYCLE PASS heap={snapshots}\n".encode())
        except Exception:
            port.write(b"hmi_test stop\r")
            raise
    print(f"{args.port} released", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
