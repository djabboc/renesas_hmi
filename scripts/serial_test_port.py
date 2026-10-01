"""Windows 串口上下文：独占打开、有限读写超时、退出必定关闭。"""

import ctypes

from monitor_msh import DCB, COMMTIMEOUTS, win_error

GENERIC_READ_WRITE = 0xC0000000
OPEN_EXISTING = 3
MAXDWORD = 0xFFFFFFFF
READ_TIMEOUT_MS = 50
WRITE_TIMEOUT_MS = 500
READ_BUFFER_BYTES = 4096


def configure_win32_signatures(kernel32) -> None:
    """显式声明 HANDLE 指针宽度，避免 64 位 Python 截断 Windows 句柄。"""
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
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    for function_name in ("ReadFile", "WriteFile"):
        getattr(kernel32, function_name).argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_void_p,
        ]


class SerialPort:
    """使用 with SerialPort(...)；不抢占其他终端，也不修改板卡复位线。"""

    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.handle = None
        self.kernel32 = None

    def __enter__(self):
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        configure_win32_signatures(self.kernel32)
        self.handle = self.kernel32.CreateFileW(
            rf"\\.\{self.port}", GENERIC_READ_WRITE, 0, None, OPEN_EXISTING, 0, None
        )
        if self.handle == ctypes.c_void_p(-1).value:
            self.handle = None
            raise win_error(f"Cannot open {self.port}; close other terminals")
        try:
            self._configure_port()
            return self
        except BaseException:
            # __enter__ 失败时 Python 不会自动调用 __exit__，须主动关闭。
            self.__exit__(None, None, None)
            raise

    def _configure_port(self) -> None:
        configuration = DCB()
        configuration.DCBlength = ctypes.sizeof(DCB)
        if not self.kernel32.GetCommState(self.handle, ctypes.byref(configuration)):
            raise win_error("GetCommState")
        configuration.BaudRate = self.baud
        configuration.Flags = 1  # fBinary=1，其余流控位关闭。
        configuration.ByteSize = 8
        configuration.Parity = 0
        configuration.StopBits = 0
        if not self.kernel32.SetCommState(self.handle, ctypes.byref(configuration)):
            raise win_error("SetCommState")

        # MAXDWORD 间隔配合固定超时：有数据立即返回，否则有限等待。
        timeouts = COMMTIMEOUTS(MAXDWORD, 0, READ_TIMEOUT_MS, 0, WRITE_TIMEOUT_MS)
        if not self.kernel32.SetCommTimeouts(self.handle, ctypes.byref(timeouts)):
            raise win_error("SetCommTimeouts")

    def read(self) -> bytes:
        buffer = ctypes.create_string_buffer(READ_BUFFER_BYTES)
        received_bytes = ctypes.c_uint32()
        if not self.kernel32.ReadFile(
            self.handle, buffer, len(buffer), ctypes.byref(received_bytes), None
        ):
            raise win_error("ReadFile")
        return buffer.raw[: received_bytes.value]

    def write(self, data: bytes) -> None:
        written_bytes = ctypes.c_uint32()
        succeeded = self.kernel32.WriteFile(
            self.handle, data, len(data), ctypes.byref(written_bytes), None
        )
        if not succeeded or written_bytes.value != len(data):
            raise win_error("WriteFile")

    def __exit__(self, *unused):
        if self.handle is not None:
            self.kernel32.CloseHandle(self.handle)
            self.handle = None
            print(f"{self.port} released", flush=True)
