"""主机工具回归：日志分块、失败/超时判定、USB 比对与例程唯一入口。

这些测试使用内存串口和临时工程，不访问 COM8、不修改真实例程。
运行：python -m unittest discover -s scripts/tests -v
"""

from contextlib import redirect_stdout
import io
from pathlib import Path
import sys
import tempfile
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
                b"msh >TEST RESULT eth mac PASS code=0\r\nTEST I",
                b"DLE\r\n",
            ]
        )
        self.assertTrue(test_peripherals.run_command(port, "hmi_test eth mac", 1, None, decode))
        self.assertEqual(port.writes, [b"hmi_test eth mac\r"])
        self.assertFalse(port.chunks)

    def test_reports_board_failure(self):
        port = MemorySerial([b"TEST RESULT eth mac FAIL code=-1\r\nTEST IDLE\r\n"])
        self.assertFalse(test_peripherals.run_command(port, "hmi_test eth mac", 1, None, decode))

    def test_wait_and_skip_do_not_claim_failure(self):
        for verdict in ("WAIT", "SKIP"):
            with self.subTest(verdict=verdict):
                port = MemorySerial(
                    [f"TEST RESULT usb probe {verdict} code=2\nTEST IDLE\n".encode()]
                )
                self.assertTrue(
                    test_peripherals.run_command(port, "hmi_test usb probe", 1, None, decode)
                )

    def test_busy_rejection_is_an_error(self):
        port = MemorySerial([b"TEST BUSY\r\n"])
        with self.assertRaisesRegex(RuntimeError, "busy"):
            test_peripherals.run_command(port, "hmi_test eth mac", 1, None, decode)

    def test_timeout_requests_stop_before_raising(self):
        port = MemorySerial([])
        with patch.object(test_peripherals.time, "monotonic", side_effect=[0, 2]):
            with self.assertRaises(TimeoutError):
                test_peripherals.run_command(port, "hmi_test eth mac", 1, None, decode)
        self.assertEqual(port.writes[-1], b"hmi_test stop\r")

    def test_ready_marker_may_span_serial_reads(self):
        port = MemorySerial([b"TEST STATUS ready=1 ", b"busy=0 cancel=0\n"])
        test_peripherals.wait_for_ready(port, decode)
        self.assertEqual(port.writes, [b"\rhmi_test status\r"])

    def test_missing_usb_mount_cannot_pass_host_echo(self):
        port = MemorySerial([b"TEST RESULT usb echo SKIP code=2\nTEST IDLE\n"])
        self.assertFalse(
            test_peripherals.run_command(port, "hmi_test usb echo", 1, "COM12", decode)
        )

    def test_host_mismatch_overrides_device_wait(self):
        port = MemorySerial([b"USB CDC configured\nTEST RESULT usb echo WAIT code=1\nTEST IDLE\n"])
        with patch.object(test_peripherals, "verify_usb_echo", return_value=False) as echo:
            self.assertFalse(
                test_peripherals.run_command(port, "hmi_test usb echo", 1, "COM12", decode)
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


class ProfileSelectionTests(unittest.TestCase):
    def test_every_profile_leaves_exactly_one_entry_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "board").mkdir()
            for filename, function in select_example.PROFILES.values():
                (root / "src" / filename).write_text(
                    f"// INIT_APP_EXPORT({function});\n", encoding="utf-8"
                )
            with patch.object(select_example, "ROOT", root), redirect_stdout(io.StringIO()):
                for profile, expected in select_example.PROFILES.items():
                    select_example.select(profile)
                    active = []
                    for source in (root / "src").glob("*.c"):
                        active.extend(
                            (source.name, name)
                            for name in select_example.ACTIVE_PATTERN.findall(
                                source.read_text(encoding="utf-8")
                            )
                        )
                    self.assertEqual(active, [expected])
                    snapshot = {
                        file: file.read_bytes() for file in root.rglob("*") if file.is_file()
                    }
                    select_example.select(profile)
                    self.assertTrue(
                        all(file.read_bytes() == data for file, data in snapshot.items())
                    )
                    mode = int(profile == "peripheral")
                    self.assertIn(
                        f"#define HMI_TEST_SUITE {mode}",
                        (root / "board/test-profile.h").read_text(),
                    )

    def test_missing_target_does_not_disable_existing_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            source = root / "src" / "lcd.c"
            original = "INIT_APP_EXPORT(lcd_test_start);\n"
            source.write_text(original)
            with patch.object(select_example, "ROOT", root):
                with self.assertRaisesRegex(RuntimeError, "Missing source"):
                    select_example.select("peripheral")
            self.assertEqual(source.read_text(), original)


if __name__ == "__main__":
    unittest.main()
