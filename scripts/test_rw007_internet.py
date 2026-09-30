"""Run one internet test, redact credentials, and always release the COM port."""
import argparse
import ctypes
from datetime import datetime
import getpass
from pathlib import Path
import sys
import time
from monitor_msh import DCB, COMMTIMEOUTS, win_error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM8")
    parser.add_argument("--ssid", required=True)
    args = parser.parse_args()
    password = getpass.getpass("Hotspot password: ") if sys.stdin.isatty() else sys.stdin.readline().rstrip("\r\n")
    if any(c in args.ssid + password for c in '\r\n"'):
        raise ValueError("Quotes and newlines are not supported in this console test")
    if not 1 <= len(args.ssid.encode()) <= 32 or not 8 <= len(password.encode()) <= 31:
        raise ValueError("SSID must be 1..32 bytes and password 8..31 bytes")
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = ctypes.c_void_p
    k.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    k.GetCommState.argtypes = [ctypes.c_void_p, ctypes.POINTER(DCB)]
    k.SetCommState.argtypes = [ctypes.c_void_p, ctypes.POINTER(DCB)]
    k.SetCommTimeouts.argtypes = [ctypes.c_void_p, ctypes.POINTER(COMMTIMEOUTS)]
    k.CloseHandle.argtypes = [ctypes.c_void_p]
    for name in ("ReadFile", "WriteFile"):
        getattr(k, name).argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    handle = k.CreateFileW(rf"\\.\{args.port}", 0xC0000000, 0, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise win_error(f"Cannot open {args.port}; another terminal may own it")
    try:
        dcb = DCB(); dcb.DCBlength = ctypes.sizeof(DCB)
        if not k.GetCommState(handle, ctypes.byref(dcb)): raise win_error("GetCommState")
        dcb.BaudRate = 115200; dcb.Flags = 1; dcb.ByteSize = 8; dcb.Parity = 0; dcb.StopBits = 0
        if not k.SetCommState(handle, ctypes.byref(dcb)): raise win_error("SetCommState")
        timeouts = COMMTIMEOUTS(0xFFFFFFFF, 0, 100, 0, 100)
        if not k.SetCommTimeouts(handle, ctypes.byref(timeouts)): raise win_error("SetCommTimeouts")
        logdir = Path(__file__).resolve().parent.parent / "logs"
        logdir.mkdir(exist_ok=True)
        logpath = logdir / f"internet_{datetime.now():%Y%m%d_%H%M%S}.log"
        print(f"Bounded internet test on {args.port}; log={logpath}", flush=True)
        def send(text):
            data = text.encode("utf-8")
            count = ctypes.c_uint32()
            if not k.WriteFile(handle, data, len(data), ctypes.byref(count), None) or count.value != len(data):
                raise win_error("WriteFile")
        buffer = ctypes.create_string_buffer(4096)
        count = ctypes.c_uint32()
        pending = b""
        sent = False; acknowledged = False; success = False; finish = None
        start = time.monotonic(); next_status = start
        with logpath.open("w", encoding="utf-8") as log:
            while time.monotonic() - start < 110:
                now = time.monotonic()
                if finish is not None and now >= finish: break
                if not sent and now >= next_status:
                    send("\rrw007_demo status\r")
                    next_status = now + 3
                if not k.ReadFile(handle, buffer, len(buffer), ctypes.byref(count), None): raise win_error("ReadFile")
                pending += buffer.raw[:count.value]
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").rstrip("\r")
                    if not sent and "RW007: ready=" in text and "busy=0" in text:
                        send(f'rw007_demo internet "{args.ssid}" "{password}"\r')
                        sent = True
                        continue
                    if "NET: internet test queued" in text: acknowledged = True
                    if acknowledged:
                        safe = text.replace(password, "[redacted]")
                        print(safe, flush=True); log.write(safe + "\n"); log.flush()
                    if "RESULT: internet " in text:
                        success = "PASS result=0" in text
                        finish = time.monotonic() + 2
                        send("rw007_demo status\rlist thread\rfree\r")
            if finish is None:
                print("Test did not finish within 110 seconds", flush=True)
        return 0 if success else 1
    finally:
        k.CloseHandle(handle)
        print(f"{args.port} released", flush=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
