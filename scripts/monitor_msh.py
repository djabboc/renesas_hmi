"""Read an RT-Thread MSH serial console using only the Python standard library."""

from __future__ import annotations

import argparse
import codecs
import ctypes
from datetime import datetime
import locale
import msvcrt
import os
import sys
from pathlib import Path


class DCB(ctypes.Structure):
    _fields_ = [
        ("DCBlength", ctypes.c_uint32),
        ("BaudRate", ctypes.c_uint32),
        ("Flags", ctypes.c_uint32),
        ("wReserved", ctypes.c_uint16),
        ("XonLim", ctypes.c_uint16),
        ("XoffLim", ctypes.c_uint16),
        ("ByteSize", ctypes.c_uint8),
        ("Parity", ctypes.c_uint8),
        ("StopBits", ctypes.c_uint8),
        ("XonChar", ctypes.c_char),
        ("XoffChar", ctypes.c_char),
        ("ErrorChar", ctypes.c_char),
        ("EofChar", ctypes.c_char),
        ("EvtChar", ctypes.c_char),
        ("wReserved1", ctypes.c_uint16),
    ]


class COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [
        ("ReadIntervalTimeout", ctypes.c_uint32),
        ("ReadTotalTimeoutMultiplier", ctypes.c_uint32),
        ("ReadTotalTimeoutConstant", ctypes.c_uint32),
        ("WriteTotalTimeoutMultiplier", ctypes.c_uint32),
        ("WriteTotalTimeoutConstant", ctypes.c_uint32),
    ]


def win_error(action: str) -> OSError:
    return ctypes.WinError(ctypes.get_last_error(), action)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM8", help="Serial port (default: COM8)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    args = parser.parse_args()

    if os.name != "nt":
        parser.error("This script uses the Windows COM port API and must run on Windows.")

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.restype = ctypes.c_void_p
    kernel32.CreateFileW.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    ]
    kernel32.GetCommState.argtypes = [ctypes.c_void_p, ctypes.POINTER(DCB)]
    kernel32.SetCommState.argtypes = [ctypes.c_void_p, ctypes.POINTER(DCB)]
    kernel32.SetCommTimeouts.argtypes = [ctypes.c_void_p, ctypes.POINTER(COMMTIMEOUTS)]
    kernel32.SetupComm.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
    kernel32.PurgeComm.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.ReadFile.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_void_p,
    ]
    kernel32.WriteFile.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.c_void_p,
    ]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    port_path = args.port if args.port.startswith("\\\\.\\") else rf"\\.\{args.port}"
    handle = kernel32.CreateFileW(port_path, 0x80000000 | 0x40000000, 0, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise win_error(f"Could not open {args.port}; close other serial terminal windows")

    try:
        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not kernel32.GetCommState(handle, ctypes.byref(dcb)):
            raise win_error("GetCommState failed")
        dcb.BaudRate = args.baud
        dcb.Flags = 1  # Binary mode, no parity or flow control.
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        if not kernel32.SetCommState(handle, ctypes.byref(dcb)):
            raise win_error("SetCommState failed")

        timeouts = COMMTIMEOUTS(0xFFFFFFFF, 0, 100, 0, 100)
        if not kernel32.SetCommTimeouts(handle, ctypes.byref(timeouts)):
            raise win_error("SetCommTimeouts failed")
        if not kernel32.SetupComm(handle, 4096, 4096):
            raise win_error("SetupComm failed")
        kernel32.PurgeComm(handle, 0x000F)

        encoding = sys.stdout.encoding or locale.getpreferredencoding(False)
        decoder = codecs.getincrementaldecoder(encoding)(errors="replace")
        interactive = sys.stdin.isatty()
        input_encoding = locale.getpreferredencoding(False)
        mode = " Type MSH commands here." if interactive else ""
        log_dir = Path(__file__).resolve().parent.parent / "logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        log_path = log_dir / f"msh_{datetime.now():%Y%m%d_%H%M%S}.log"
        log_file = log_path.open("w", encoding="utf-8", newline="")
        tx_line = ""
        print(f"Listening on {args.port} at {args.baud} 8N1.{mode} Press Ctrl+C to stop.")
        print(f"Logging to {log_path}", flush=True)
        log_file.write(f"[{datetime.now():%Y-%m-%d %H:%M:%S}] Opened {args.port} at {args.baud} 8N1\n")
        log_file.flush()
        buffer = ctypes.create_string_buffer(1024)
        count = ctypes.c_uint32()
        while True:
            if not kernel32.ReadFile(handle, buffer, len(buffer), ctypes.byref(count), None):
                raise win_error("ReadFile failed")
            if count.value:
                received = decoder.decode(buffer.raw[: count.value])
                sys.stdout.write(received)
                sys.stdout.flush()
                log_file.write(received)
                log_file.flush()
            while interactive and msvcrt.kbhit():
                char = msvcrt.getwch()
                if char in ("\x00", "\xe0"):
                    msvcrt.getwch()
                    continue
                data = b"\r" if char == "\r" else char.encode(input_encoding, errors="replace")
                if data:
                    if char == "\r":
                        log_file.write(f"\n[{datetime.now():%H:%M:%S}] TX: {tx_line}\n")
                        log_file.flush()
                        tx_line = ""
                    elif char == "\b":
                        tx_line = tx_line[:-1]
                    elif char.isprintable():
                        tx_line += char
                    sent = ctypes.c_uint32()
                    payload = ctypes.create_string_buffer(data)
                    if not kernel32.WriteFile(handle, payload, len(data), ctypes.byref(sent), None):
                        raise win_error("WriteFile failed")
    except KeyboardInterrupt:
        print("\nSerial monitor stopped.")
        return 0
    finally:
        if "log_file" in locals():
            log_file.write(f"\n[{datetime.now():%Y-%m-%d %H:%M:%S}] Monitor stopped\n")
            log_file.close()
        kernel32.CloseHandle(handle)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
