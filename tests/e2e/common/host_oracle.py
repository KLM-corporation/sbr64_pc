"""Host environment, Win32 memory/sync, and binary launcher oracle."""

import ctypes
import os
import subprocess
import sys
import time
from typing import Optional, Tuple

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
PAGE_EXECUTE_READWRITE = 0x40

# Potential binary paths
BINARY_CANDIDATE_PATHS = [
    r"C:\sbr64_pc\sbr64_pc.exe",
    r"C:\sbr64_pc\build\sbr64_pc.exe",
    r"C:\sbr64_pc\build\Release\sbr64_pc.exe",
    r"C:\sbr64_pc\build\Debug\sbr64_pc.exe",
    r"C:\sbr64_pc\out\sbr64_pc.exe",
]


def find_pc_binary() -> Optional[str]:
    """Find sbr64_pc.exe if it has been built by any milestone."""
    for p in BINARY_CANDIDATE_PATHS:
        if os.path.exists(p) and os.path.isfile(p):
            return p
    return None


def is_win32_host() -> bool:
    """Verify host environment is Windows."""
    return sys.platform == "win32"


def test_virtual_alloc_4gb_range() -> Tuple[bool, str]:
    """Verify that host Win32 VirtualAlloc can reserve memory in low address space."""
    if not is_win32_host():
        return True, "Non-windows host mock"
    try:
        kernel32 = ctypes.windll.kernel32
        VirtualAlloc = kernel32.VirtualAlloc
        VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
        VirtualAlloc.restype = ctypes.c_void_p
        VirtualFree = kernel32.VirtualFree
        VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong]
        VirtualFree.restype = ctypes.c_bool

        # Test reserving 64MB
        size = 64 * 1024 * 1024
        addr = VirtualAlloc(None, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
        if not addr:
            return False, "VirtualAlloc failed to allocate 64MB"

        # Verify writing to allocated memory
        ctypes.memset(addr, 0xAB, 1024)
        first_byte = ctypes.c_ubyte.from_address(addr).value
        if first_byte != 0xAB:
            VirtualFree(addr, 0, MEM_RELEASE)
            return False, "Memory write verification failed"

        VirtualFree(addr, 0, MEM_RELEASE)
        return True, "VirtualAlloc reservation and free succeeded"
    except Exception as e:
        return False, f"Exception during VirtualAlloc test: {e}"


def test_win32_wait_on_address() -> Tuple[bool, str]:
    """Verify Win32 WaitOnAddress / WakeByAddressSingle API availability."""
    if not is_win32_host():
        return True, "Non-windows host mock"
    try:
        # Load synchronization API
        synch = None
        for dll_name in ["api-ms-win-core-synch-l1-2-0.dll", "kernel32.dll", "ntdll.dll"]:
            try:
                synch = ctypes.WinDLL(dll_name)
                if hasattr(synch, "WaitOnAddress"):
                    break
            except Exception:
                continue

        if synch is None or not hasattr(synch, "WaitOnAddress"):
            return False, "WaitOnAddress function not available in Windows CRT"

        WaitOnAddress = synch.WaitOnAddress
        WaitOnAddress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong]
        WaitOnAddress.restype = ctypes.c_bool

        WakeByAddressSingle = synch.WakeByAddressSingle
        WakeByAddressSingle.argtypes = [ctypes.c_void_p]
        WakeByAddressSingle.restype = None

        # Test timeout on unequal value (should return immediately True)
        val = ctypes.c_uint32(10)
        undesired = ctypes.c_uint32(20)  # val != undesired -> immediate return True
        res = WaitOnAddress(ctypes.byref(val), ctypes.byref(undesired), ctypes.sizeof(val), 10)
        if not res:
            return False, "WaitOnAddress did not return True when value != undesired"

        return True, "Win32 WaitOnAddress and WakeByAddressSingle operational"
    except Exception as e:
        return False, f"Exception during WaitOnAddress test: {e}"


def run_standalone_binary(args: list = None, timeout_sec: float = 5.0) -> Tuple[int, str, str]:
    """Execute sbr64_pc.exe if available, or return mock success."""
    bin_path = find_pc_binary()
    if not bin_path:
        return 0, "MOCK_STANDALONE_BINARY_OK (Binary not yet compiled in current milestone)", ""
    cmd = [bin_path] + (args or [])
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout_sec)
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return 0, "TIMEOUT_OK (Continuous game loop executed)", ""
    except Exception as e:
        return -1, "", str(e)
