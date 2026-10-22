# Poor man's native sampling profiler for one thread of another process (Windows x64).
#
#   python sampler.py <pid> <tid> <outfile> <stopfile> [interval_ms]
#
# Suspends the thread, walks its stack with dbghelp StackWalk64, resumes, and
# repeats until <stopfile> exists. Then resolves the PCs to module!function and
# writes inclusive / self counts and the hottest stacks to <outfile>.
# Writes <outfile>.ready once symbols are initialised, and <outfile>.done at the end.

import ctypes
import os
import sys
import time
from collections import Counter
from ctypes import wintypes as W

pid, tid = int(sys.argv[1]), int(sys.argv[2])
outfile, stopfile = sys.argv[3], sys.argv[4]
interval = float(sys.argv[5]) / 1000.0 if len(sys.argv) > 5 else 0.003

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
winmm = ctypes.WinDLL("winmm")
dbghelp_path = r"D:\works\sw\tools\dbg\dbghelp.dll"
dbg = ctypes.WinDLL(dbghelp_path if os.path.exists(dbghelp_path) else "dbghelp", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
THREAD_SUSPEND_RESUME = 0x0002
THREAD_GET_CONTEXT = 0x0008
THREAD_QUERY_INFORMATION = 0x0040

k32.OpenProcess.restype = W.HANDLE
k32.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k32.OpenThread.restype = W.HANDLE
k32.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k32.SuspendThread.argtypes = [W.HANDLE]
k32.SuspendThread.restype = W.DWORD
k32.ResumeThread.argtypes = [W.HANDLE]
k32.ResumeThread.restype = W.DWORD
k32.GetThreadContext.argtypes = [W.HANDLE, ctypes.c_void_p]
k32.GetThreadContext.restype = W.BOOL

hProc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
hThr = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, tid)
if not hProc or not hThr:
    open(outfile, "w").write("open failed %d\n" % ctypes.get_last_error())
    open(outfile + ".ready", "w").write("fail")
    open(outfile + ".done", "w").write("fail")
    sys.exit(1)

SYMOPT_UNDNAME = 0x2
SYMOPT_DEFERRED_LOADS = 0x4
SYMOPT_FAIL_CRITICAL_ERRORS = 0x200
SYMOPT_NO_PROMPTS = 0x80000
dbg.SymSetOptions.argtypes = [W.DWORD]
dbg.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS)
dbg.SymInitializeW.argtypes = [W.HANDLE, ctypes.c_wchar_p, W.BOOL]
dbg.SymInitializeW.restype = W.BOOL
if not dbg.SymInitializeW(hProc, None, True):
    open(outfile, "w").write("SymInitialize failed %d\n" % ctypes.get_last_error())

fta = ctypes.cast(dbg.SymFunctionTableAccess64, ctypes.c_void_p).value
gmb = ctypes.cast(dbg.SymGetModuleBase64, ctypes.c_void_p).value
dbg.StackWalk64.argtypes = [
    W.DWORD, W.HANDLE, W.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
]
dbg.StackWalk64.restype = W.BOOL

# CONTEXT (x64) must be 16-byte aligned.
ctx_raw = ctypes.create_string_buffer(1232 + 16)
ctx_addr = (ctypes.addressof(ctx_raw) + 15) & ~15
frame = ctypes.create_string_buffer(1024)
CONTEXT_FULL = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664

u64 = ctypes.c_uint64
u32 = ctypes.c_uint32
u16 = ctypes.c_uint16


def sample(maxdepth=96):
    ctypes.memset(ctx_addr, 0, 1232)
    u32.from_address(ctx_addr + 0x30).value = CONTEXT_FULL
    if k32.SuspendThread(hThr) == 0xFFFFFFFF:
        return None
    pcs = []
    try:
        if not k32.GetThreadContext(hThr, ctx_addr):
            return None
        rip = u64.from_address(ctx_addr + 0xF8).value
        rsp = u64.from_address(ctx_addr + 0x98).value
        rbp = u64.from_address(ctx_addr + 0xA0).value
        ctypes.memset(frame, 0, 1024)
        fa = ctypes.addressof(frame)
        u64.from_address(fa + 0).value = rip
        u32.from_address(fa + 12).value = 3
        u64.from_address(fa + 32).value = rbp
        u32.from_address(fa + 44).value = 3
        u64.from_address(fa + 48).value = rsp
        u32.from_address(fa + 60).value = 3
        for _ in range(maxdepth):
            ok = dbg.StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProc, hThr, fa, ctx_addr, None, fta, gmb, None)
            if not ok:
                break
            pc = u64.from_address(fa).value
            if pc == 0:
                break
            pcs.append(pc)
    finally:
        k32.ResumeThread(hThr)
    return tuple(pcs)


open(outfile + ".ready", "w").write("ok")
winmm.timeBeginPeriod(1)
stacks = Counter()
nsamp = 0
t0 = time.perf_counter()
while not os.path.exists(stopfile):
    s = sample()
    if s:
        stacks[s] += 1
        nsamp += 1
    time.sleep(interval)
elapsed = time.perf_counter() - t0
winmm.timeEndPeriod(1)


# ---- resolve
class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", u64 * 2),
        ("Index", W.ULONG), ("Size", W.ULONG), ("ModBase", u64), ("Flags", W.ULONG),
        ("Value", u64), ("Address", u64), ("Register", W.ULONG), ("Scope", W.ULONG),
        ("Tag", W.ULONG), ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG),
        ("Name", ctypes.c_char * 1024),
    ]


dbg.SymFromAddr.argtypes = [W.HANDLE, u64, ctypes.POINTER(u64), ctypes.c_void_p]
dbg.SymFromAddr.restype = W.BOOL

mods = {}
arr = (W.HMODULE * 4096)()
need = W.DWORD()
psapi.EnumProcessModules.argtypes = [W.HANDLE, ctypes.c_void_p, W.DWORD, ctypes.POINTER(W.DWORD)]
psapi.GetModuleBaseNameW.argtypes = [W.HANDLE, W.HMODULE, ctypes.c_wchar_p, W.DWORD]
if psapi.EnumProcessModules(hProc, arr, ctypes.sizeof(arr), ctypes.byref(need)):
    for i in range(need.value // ctypes.sizeof(W.HMODULE)):
        nm = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(hProc, arr[i], nm, 260)
        if arr[i]:
            mods[arr[i]] = nm.value

cache = {}


def name(pc):
    r = cache.get(pc)
    if r:
        return r
    si = SYMBOL_INFO()
    si.SizeOfStruct = 88
    si.MaxNameLen = 1024
    disp = u64()
    mb = dbg.SymGetModuleBase64(hProc, u64(pc)) if False else 0
    if dbg.SymFromAddr(hProc, pc, ctypes.byref(disp), ctypes.byref(si)):
        mod = mods.get(si.ModBase, "?")
        r = "%s!%s" % (mod.rsplit(".", 1)[0], si.Name.decode(errors="replace"))
    else:
        base = max([b for b in mods if b <= pc] or [0])
        r = "%s!0x%x" % (mods.get(base, "?").rsplit(".", 1)[0], pc - base)
    cache[pc] = r
    return r


incl = Counter()
selfc = Counter()
fstacks = Counter()
for st, c in stacks.items():
    names = [name(pc) for pc in st]
    if names:
        selfc[names[0]] += c
    for n in set(names):
        incl[n] += c
    # collapse consecutive duplicates (recursion) for the stack listing
    coll = []
    for n in names:
        if not coll or coll[-1] != n:
            coll.append(n)
    fstacks[tuple(coll[:14])] += c

with open(outfile, "w") as f:
    f.write("samples %d over %.2fs (%.1f/s)\n\n" % (nsamp, elapsed, nsamp / max(elapsed, 1e-9)))
    f.write("== inclusive (top 70)\n")
    for n, c in incl.most_common(70):
        f.write("%6.1f%%  %6d  %s\n" % (100.0 * c / max(nsamp, 1), c, n[:220]))
    f.write("\n== self (top 40)\n")
    for n, c in selfc.most_common(40):
        f.write("%6.1f%%  %6d  %s\n" % (100.0 * c / max(nsamp, 1), c, n[:220]))
    f.write("\n== hottest stacks (leaf first, top 14 frames)\n")
    for st, c in fstacks.most_common(15):
        f.write("\n%6.1f%%  %d\n" % (100.0 * c / max(nsamp, 1), c))
        for n in st:
            f.write("        %s\n" % n[:200])
open(outfile + ".done", "w").write("ok")
