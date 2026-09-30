"""Small bounded Windows serial transport; context manager always releases COM."""
import ctypes
from monitor_msh import DCB, COMMTIMEOUTS, win_error


class SerialPort:
    def __init__(self, port, baud=115200):
        self.port, self.baud, self.handle = port, baud, None

    def __enter__(self):
        k = self.k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.CreateFileW.restype = ctypes.c_void_p
        k.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                 ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        k.GetCommState.argtypes = [ctypes.c_void_p, ctypes.POINTER(DCB)]
        k.SetCommState.argtypes = [ctypes.c_void_p, ctypes.POINTER(DCB)]
        k.SetCommTimeouts.argtypes = [ctypes.c_void_p, ctypes.POINTER(COMMTIMEOUTS)]
        k.CloseHandle.argtypes = [ctypes.c_void_p]
        for name in ('ReadFile', 'WriteFile'):
            getattr(k, name).argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                       ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
        self.handle = k.CreateFileW(rf'\\.\{self.port}', 0xC0000000, 0, None, 3, 0, None)
        if self.handle == ctypes.c_void_p(-1).value:
            self.handle = None
            raise win_error(f'Cannot open {self.port}; close other terminals')
        try:
            dcb = DCB(); dcb.DCBlength = ctypes.sizeof(DCB)
            if not k.GetCommState(self.handle, ctypes.byref(dcb)): raise win_error('GetCommState')
            dcb.BaudRate = self.baud; dcb.Flags = 1; dcb.ByteSize = 8; dcb.Parity = 0; dcb.StopBits = 0
            if not k.SetCommState(self.handle, ctypes.byref(dcb)): raise win_error('SetCommState')
            limits = COMMTIMEOUTS(0xFFFFFFFF, 0, 50, 0, 500)
            if not k.SetCommTimeouts(self.handle, ctypes.byref(limits)): raise win_error('SetCommTimeouts')
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def read(self):
        buf = ctypes.create_string_buffer(4096); count = ctypes.c_uint32()
        if not self.k.ReadFile(self.handle, buf, len(buf), ctypes.byref(count), None): raise win_error('ReadFile')
        return buf.raw[:count.value]

    def write(self, data):
        count = ctypes.c_uint32()
        if not self.k.WriteFile(self.handle, data, len(data), ctypes.byref(count), None) or count.value != len(data):
            raise win_error('WriteFile')

    def __exit__(self, *unused):
        if self.handle is not None:
            self.k.CloseHandle(self.handle); self.handle = None
            print(f'{self.port} released', flush=True)
