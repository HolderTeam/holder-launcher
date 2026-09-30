"""Inspect the real Holder.exe resource without executing the GUI application."""
import ctypes
from ctypes import wintypes
import json
import struct
import sys

binary, record_path = sys.argv[1:]
with open(record_path, encoding="utf-8") as stream:
    record = json.load(stream)
api = ctypes.WinDLL("version", use_last_error=True)
api.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
api.GetFileVersionInfoSizeW.restype = wintypes.DWORD
api.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
api.GetFileVersionInfoW.restype = wintypes.BOOL
api.VerQueryValueW.argtypes = [wintypes.LPCVOID, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
api.VerQueryValueW.restype = wintypes.BOOL
size = api.GetFileVersionInfoSizeW(binary, None)
assert size, ctypes.WinError(ctypes.get_last_error())
buffer = ctypes.create_string_buffer(size)
assert api.GetFileVersionInfoW(binary, 0, size, buffer)


def query(key):
    pointer, length = ctypes.c_void_p(), wintypes.UINT()
    assert api.VerQueryValueW(buffer, key, ctypes.byref(pointer), ctypes.byref(length)), key
    return pointer, length.value


pointer, size = query("\\")
fixed = struct.unpack("<13I", ctypes.string_at(pointer, 52))
assert fixed[0] == 0xFEEF04BD
major, minor, patch = map(int, record["version"].split("."))
assert fixed[2:4] == ((major << 16) | minor, patch << 16)
assert fixed[4:6] == fixed[2:4]
source = record["source"]["commit"] + ("-dirty" if record["source"]["dirty"] else "")
for key, expected in {"FileVersion": record["version"], "ProductVersion": record["version"],
                      "ProductName": "Holder Launcher", "Comments": "Source: " + source}.items():
    pointer, _ = query("\\StringFileInfo\\040904B0\\" + key)
    assert ctypes.wstring_at(pointer) == expected, key
print("Windows version resource matches captured build identity")
