"""主机工具回归：日志分块、失败/超时判定、USB 比对与例程唯一入口。

这些测试使用内存串口和临时工程，不访问 COM8、不修改真实例程。
运行：python -m unittest discover -s scripts/tests -v
"""

from contextlib import redirect_stdout
import io
from pathlib import Path
import sys
import re
import hashlib
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import select_example
import test_peripherals


class MemorySerial:
    """按给定分块重放设备字节流，并记录主机发送内容。"""

    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.writes = []

    def write(self, data):
        self.writes.append(data)

    def read(self):
        return self.chunks.pop(0) if self.chunks else b""


def decode(data):
    return data.decode("utf-8")


class CommandProtocolTests(unittest.TestCase):
    def test_waits_for_fragmented_idle_after_result(self):
        port = MemorySerial(
            [
                b"msh >TEST RESULT eth-mac PASS code=0\r\nTEST I",
                b"DLE\r\n",
            ]
        )
        self.assertTrue(test_peripherals.run_command(port, "hmi_test eth-mac", 1, None, decode))
        self.assertEqual(port.writes, [b"hmi_test eth-mac\r"])
        self.assertFalse(port.chunks)

    def test_reports_board_failure(self):
        port = MemorySerial([b"TEST RESULT eth-mac FAIL code=-1\r\nTEST IDLE\r\n"])
        self.assertFalse(test_peripherals.run_command(port, "hmi_test eth-mac", 1, None, decode))

    def test_wait_and_skip_do_not_claim_failure(self):
        for verdict in ("WAIT", "SKIP"):
            with self.subTest(verdict=verdict):
                port = MemorySerial(
                    [f"TEST RESULT usb-probe {verdict} code=2\nTEST IDLE\n".encode()]
                )
                self.assertTrue(
                    test_peripherals.run_command(port, "hmi_test usb-probe", 1, None, decode)
                )

    def test_busy_rejection_is_an_error(self):
        port = MemorySerial([b"TEST BUSY\r\n"])
        with self.assertRaisesRegex(RuntimeError, "busy"):
            test_peripherals.run_command(port, "hmi_test eth-mac", 1, None, decode)

    def test_invalid_command_or_fault_is_not_a_success(self):
        for message in ("TEST UNKNOWN xyz", "TEST ERROR allocation", "assertion failed"):
            with self.subTest(message=message):
                port = MemorySerial([(message + "\n").encode()])
                with self.assertRaises(RuntimeError):
                    test_peripherals.run_command(port, "hmi_test eth-mac", 1, None, decode)

    def test_idle_without_result_is_not_a_success(self):
        port = MemorySerial([b"TEST IDLE\n"])
        with self.assertRaisesRegex(RuntimeError, "Missing test result"):
            test_peripherals.run_command(port, "hmi_test eth-mac", 1, None, decode)

    def test_timeout_requests_stop_before_raising(self):
        port = MemorySerial([])
        with patch.object(test_peripherals.time, "monotonic", side_effect=[0, 2]):
            with self.assertRaises(TimeoutError):
                test_peripherals.run_command(port, "hmi_test eth-mac", 1, None, decode)
        self.assertEqual(port.writes[-1], b"hmi_test stop\r")

    def test_ready_marker_may_span_serial_reads(self):
        port = MemorySerial([b"TEST STATUS ready=1 ", b"busy=0 cancel=0\n"])
        test_peripherals.wait_for_ready(port, decode)
        self.assertEqual(port.writes, [b"\rhmi_test status\r"])

    def test_missing_usb_mount_cannot_pass_host_echo(self):
        port = MemorySerial([b"TEST RESULT usb-echo SKIP code=2\nTEST IDLE\n"])
        self.assertFalse(
            test_peripherals.run_command(port, "hmi_test usb-echo", 1, "COM12", decode)
        )

    def test_host_mismatch_overrides_device_wait(self):
        port = MemorySerial([b"USB CDC configured\nTEST RESULT usb-echo WAIT code=1\nTEST IDLE\n"])
        with patch.object(test_peripherals, "verify_usb_echo", return_value=False) as echo:
            self.assertFalse(
                test_peripherals.run_command(port, "hmi_test usb-echo", 1, "COM12", decode)
            )
        echo.assert_called_once_with("COM12", decode)

    def test_binary_echo_compares_all_bytes_and_closes_port(self):
        payload = bytes(range(256)) + b"HMI\x00\xff\r\n" * 37
        for received, expected in ((payload, True), (payload[:-1] + b"x", False)):
            with self.subTest(expected=expected):
                port = MemorySerial([received[:64], received[64:]])
                with patch.object(test_peripherals, "SerialPort") as factory:
                    factory.return_value.__enter__.return_value = port
                    self.assertEqual(test_peripherals.verify_usb_echo("COM12", decode), expected)
                    factory.return_value.__exit__.assert_called_once()
                self.assertEqual(port.writes, [payload])


class IndependentExampleTests(unittest.TestCase):
    root = Path(__file__).resolve().parents[2]

    def test_each_file_has_one_entry_registered_exactly_once(self):
        main = (self.root / "src/test-main.c").read_text(encoding="utf-8")
        files = list((self.root / "src/test").glob("*.c"))
        self.assertEqual(len(files), 47)
        for path in files:
            with self.subTest(path=path.name):
                source = path.read_text(encoding="utf-8")
                command = path.stem.removeprefix("test-")
                function = "test_" + command.replace("-", "_") + "_thread"
                entries = re.findall(r"^void (test_\w+_thread)\(void \*argument\)", source, re.M)
                self.assertEqual(entries, [function])
                self.assertEqual(main.count(f'{{"{command}", {function},'), 1)

    def test_examples_contain_no_shell_or_thread_creation(self):
        for path in (self.root / "src/test").glob("*.c"):
            with self.subTest(path=path.name):
                source = path.read_text(encoding="utf-8")
                for token in (
                    "MSH",
                    "FINSH",
                    "INIT_APP_EXPORT",
                    "rt_thread_create",
                    "rt_thread_startup",
                    "rt_thread_delete",
                    "const char *stage",
                ):
                    self.assertNotIn(token, source)
                includes = re.findall(r'#include [<"]([^>"]+)', source)
                self.assertFalse(any("test-" in header for header in includes))
        self.assertFalse(list((self.root / "src/test").glob("*.h")))

    def test_no_inter_example_entry_calls(self):
        for path in (self.root / "src/test").glob("*.c"):
            source = path.read_text(encoding="utf-8")
            # A file only contains its own entry definition, never another example invocation.
            self.assertEqual(len(re.findall(r"\btest_\w+_thread\s*\(", source)), 1, path.name)

    def test_hal_entry_is_byte_for_byte_original(self):
        expected = "e611bd83335e84d7fefa2002cc1db1f80e954d192c3152687d13b8146203cdb7"
        actual = hashlib.sha256((self.root / "src/hal_entry.c").read_bytes()).hexdigest()
        self.assertEqual(actual, expected)

    def test_dispatcher_is_only_shell_entry_and_thread_creator(self):
        application = list((self.root / "src").rglob("*.c"))
        exports = [p.name for p in application if "MSH_CMD_EXPORT" in p.read_text(encoding="utf-8")]
        creators = [
            p.name for p in application if "rt_thread_create(" in p.read_text(encoding="utf-8")
        ]
        self.assertEqual(exports, ["test-main.c"])
        self.assertEqual(creators, ["test-main.c"])

    def test_old_profile_tool_is_read_only_guidance(self):
        with redirect_stdout(io.StringIO()) as output:
            select_example.select("lcd-touch-lvgl")
        self.assertIn("hmi_test lcd-touch-lvgl", output.getvalue())


if __name__ == "__main__":
    unittest.main()
