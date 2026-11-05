"""Send G13 LCD frames, each filled with one byte value.

Usage: python -I tools\\lcd_test.py <delay seconds> <gap seconds> <hex byte> [<hex byte> ...]
Example: python -I tools\\lcd_test.py 5 0 FF

See docs/screen-diagnosis.md for what to expect.
"""
import ctypes
import ctypes.wintypes as w
import sys
import time

REPORT_LEN = 992
HEADER = 32

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
hid = ctypes.WinDLL("hid", use_last_error=True)
sapi = ctypes.WinDLL("setupapi", use_last_error=True)


class GUID(ctypes.Structure):
    _fields_ = [("a", w.DWORD), ("b", w.WORD), ("c", w.WORD), ("d", ctypes.c_ubyte * 8)]


class IFACE(ctypes.Structure):
    _fields_ = [("cbSize", w.DWORD), ("guid", GUID), ("flags", w.DWORD),
                ("reserved", ctypes.c_size_t)]


k32.CreateFileW.restype = w.HANDLE
k32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
k32.WriteFile.argtypes = [w.HANDLE, w.LPCVOID, w.DWORD, ctypes.POINTER(w.DWORD), w.LPVOID]
k32.CloseHandle.argtypes = [w.HANDLE]
sapi.SetupDiGetClassDevsW.restype = w.HANDLE
sapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), w.LPCWSTR, w.HWND, w.DWORD]
sapi.SetupDiEnumDeviceInterfaces.argtypes = [w.HANDLE, w.LPVOID, ctypes.POINTER(GUID), w.DWORD,
                                             ctypes.POINTER(IFACE)]
sapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [w.HANDLE, ctypes.POINTER(IFACE), w.LPVOID,
                                                  w.DWORD, ctypes.POINTER(w.DWORD), w.LPVOID]


def find_path():
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    devs = sapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, 0x12)
    i = 0
    while True:
        iface = IFACE(cbSize=ctypes.sizeof(IFACE))
        if not sapi.SetupDiEnumDeviceInterfaces(devs, None, ctypes.byref(guid), i,
                                                ctypes.byref(iface)):
            return None
        i += 1
        need = w.DWORD()
        sapi.SetupDiGetDeviceInterfaceDetailW(devs, ctypes.byref(iface), None, 0,
                                              ctypes.byref(need), None)
        buf = ctypes.create_string_buffer(need.value)
        ctypes.cast(buf, ctypes.POINTER(w.DWORD))[0] = 8
        if sapi.SetupDiGetDeviceInterfaceDetailW(devs, ctypes.byref(iface), buf, need, None, None):
            path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
            if "vid_046d&pid_c21c" in path.lower():
                return path


def filled(value):
    data = bytearray(REPORT_LEN)
    data[0] = 3
    for i in range(HEADER, REPORT_LEN):
        data[i] = value
    return ctypes.create_string_buffer(bytes(data), REPORT_LEN)


initial_delay = float(sys.argv[1])
gap = float(sys.argv[2])
sequence = [int(a, 16) for a in sys.argv[3:]]

h = k32.CreateFileW(find_path(), 0xC0000000, 3, None, 3, 0, None)
if h == w.HANDLE(-1).value:
    sys.exit(f"open failed, error {ctypes.get_last_error()}")
time.sleep(initial_delay)
n = w.DWORD()
for i, value in enumerate(sequence):
    frame = filled(value)
    ok = k32.WriteFile(h, frame, REPORT_LEN, ctypes.byref(n), None)
    print(f"{time.strftime('%H:%M:%S')} frame 0x{value:02X}: ok={bool(ok)} bytes={n.value}", flush=True)
    if i < len(sequence) - 1:
        time.sleep(gap)
k32.CloseHandle(h)
