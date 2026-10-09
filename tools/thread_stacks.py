"""Print the call stack of every thread in a running process, symbolized from our own PDB.

    python tools/thread_stacks.py <pid> [symbol directory]

Written on 2026-10-07 for a freeze: after a texture pack was reloaded mid-game the renderer stopped
presenting while the game thread ran on, and nothing crashed, so the crash handler had nothing to
say. A frozen program is a set of threads each waiting on something; this names the something.

No debugger is installed on this machine and nothing is installed by this: it drives Windows' own
dbghelp.dll through ctypes. Each thread is suspended only for as long as its stack takes to read
and is always resumed, so the process carries on exactly as it was. It reads code addresses and
symbol names, never memory contents, the same rule the crash handler keeps (CLAUDE.md: no memory
contents in a log).

Build-time tooling, never shipped.
"""

import ctypes
import ctypes.wintypes as wt
import sys

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
THREAD_GET_CONTEXT = 0x0008
THREAD_SUSPEND_RESUME = 0x0002
THREAD_QUERY_INFORMATION = 0x0040
TH32CS_SNAPTHREAD = 0x00000004
CONTEXT_FULL_AMD64 = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664
ADDR_MODE_FLAT = 3
SYMOPT_UNDNAME = 0x2
SYMOPT_DEFERRED_LOADS = 0x4
SYMOPT_FAIL_CRITICAL_ERRORS = 0x200

# The x64 CONTEXT is 1232 bytes and must sit on a 16 byte boundary. Only four fields are needed,
# so it is a raw buffer read at their documented offsets rather than a 60 field structure.
CONTEXT_SIZE = 1232
OFF_FLAGS = 0x30
OFF_RSP = 0x98
OFF_RBP = 0xA0
OFF_RIP = 0xF8


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", ctypes.c_uint16), ("Mode", ctypes.c_int)]


class STACKFRAME64(ctypes.Structure):
    # KdHelp is only read for kernel stacks; the trailing pad gives it more room than any SDK's
    # KDHELP64 needs, which is harmless and saves tracking its version.
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_uint64 * 4), ("Far", wt.BOOL), ("Virtual", wt.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", ctypes.c_byte * 256)]


kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenThread.restype = wt.HANDLE
kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.SuspendThread.restype = wt.DWORD
kernel32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbghelp.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbghelp.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.SymGetModuleBase64.restype = ctypes.c_uint64
dbghelp.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_uint64]
dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
dbghelp.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64), ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

psapi = ctypes.WinDLL("psapi")
psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]


def module_name(process, base):
    buf = ctypes.create_unicode_buffer(260)
    if base and psapi.GetModuleBaseNameW(process, wt.HMODULE(base), buf, 260):
        return buf.value
    return "?"


def describe(process, address):
    # SYMBOL_INFO: SizeOfStruct is 88 on x64, MaxNameLen at 80, NameLen at 76, Name at 84.
    info = ctypes.create_string_buffer(88 + 512)
    ctypes.c_uint32.from_buffer(info, 0).value = 88
    ctypes.c_uint32.from_buffer(info, 80).value = 255
    displacement = ctypes.c_uint64(0)
    base = dbghelp.SymGetModuleBase64(process, address)
    module = module_name(process, base)
    if dbghelp.SymFromAddr(process, address, ctypes.byref(displacement), info):
        length = ctypes.c_uint32.from_buffer(info, 76).value
        name = info.raw[84:84 + length].decode("ascii", "replace")
        return f"{module}!{name}+0x{displacement.value:X}"
    return f"{module}+0x{address - base:X}" if base else f"0x{address:X}"


def main():
    pid = int(sys.argv[1])
    symbols = sys.argv[2] if len(sys.argv) > 2 else None
    process = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not process:
        sys.exit(f"cannot open process {pid}: error {ctypes.get_last_error()}")
    dbghelp.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS)
    if not dbghelp.SymInitializeW(process, symbols, True):
        sys.exit(f"SymInitialize failed: error {ctypes.get_last_error()}")

    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(THREADENTRY32)
    threads = []
    ok = kernel32.Thread32First(snapshot, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            threads.append(entry.th32ThreadID)
        ok = kernel32.Thread32Next(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)

    raw = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
    aligned = (ctypes.addressof(raw) + 15) & ~15
    table_access = ctypes.cast(dbghelp.SymFunctionTableAccess64, ctypes.c_void_p)
    module_base = ctypes.cast(dbghelp.SymGetModuleBase64, ctypes.c_void_p)

    for tid in threads:
        thread = kernel32.OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, False, tid)
        if not thread:
            print(f"\n== thread {tid}: cannot open")
            continue
        lines = []
        if kernel32.SuspendThread(thread) != 0xFFFFFFFF:
            try:
                ctypes.memset(aligned, 0, CONTEXT_SIZE)
                ctypes.c_uint32.from_address(aligned + OFF_FLAGS).value = CONTEXT_FULL_AMD64
                if kernel32.GetThreadContext(thread, aligned):
                    frame = STACKFRAME64()
                    frame.AddrPC.Offset = ctypes.c_uint64.from_address(aligned + OFF_RIP).value
                    frame.AddrFrame.Offset = ctypes.c_uint64.from_address(aligned + OFF_RBP).value
                    frame.AddrStack.Offset = ctypes.c_uint64.from_address(aligned + OFF_RSP).value
                    for a in (frame.AddrPC, frame.AddrFrame, frame.AddrStack):
                        a.Mode = ADDR_MODE_FLAT
                    for _ in range(48):
                        if not dbghelp.StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, ctypes.byref(frame),
                                                   aligned, None, table_access, module_base, None):
                            break
                        if frame.AddrPC.Offset == 0:
                            break
                        lines.append(describe(process, frame.AddrPC.Offset))
                else:
                    lines.append(f"GetThreadContext failed: error {ctypes.get_last_error()}")
            finally:
                kernel32.ResumeThread(thread)
        kernel32.CloseHandle(thread)
        print(f"\n== thread {tid}")
        for i, line in enumerate(lines):
            print(f"  {i:2d}  {line}")

    dbghelp.SymCleanup(process)
    kernel32.CloseHandle(process)


if __name__ == "__main__":
    main()
