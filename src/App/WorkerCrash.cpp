/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

// A worker's crash, told to the pool (docs/WorkerProcess.md sec 35). The
// handler writes to the worker's stderr, which is its log in the pool's
// directory (WorkerPool::Private::spawn), in lines the pool reads back:
//
//   --- worker crash ---
//   signal 11 code 1 address 0x10 sender 0
//   at Pad
//   python
//   <the tracebacks, as faulthandler says them>
//   end of python
//   pc 0x0000000104b1a80                (not on macOS, whose walk starts there)
//   frame 0x0000000104b1a80 0x0000000104a00000 _ZN3App3FooEv 0x40 /path/libX.dylib
//   ...
//   --- end of worker crash ---
//
// Everything in the handler is async-signal-safe, or as near as a crash
// handler gets: write(), backtrace() (primed at install), dladdr() and
// Python's own traceback dump, the one faulthandler uses.
//
// On Windows (sec 41) the handler is the process's unhandled exception
// filter, and writes the same lines: the code is the exception's, in hex,
// the signal the one a POSIX worker would have died of; the frames are
// unwound from the exception's context by the images' unwind data, each
// named by its image's export table. Nothing of it allocates either.

#include "PreCompiled.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>

#if !defined(FC_OS_WIN32)
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, which it needs
#include <dbghelp.h>
#include <mutex>
#include <set>
#if defined(_MSC_VER)
#include <eh.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#endif

#include <Python.h>

#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

#include <Base/FileInfo.h>
#include <Base/Parameter.h>

#include "Application.h"
#include "WorkerCrash.h"

using namespace App;

namespace
{

constexpr const char* Begin = "--- worker crash ---";
constexpr const char* End = "--- end of worker crash ---";

// Where it is in the job, for the handler: written by the worker's one
// thread that makes jobs, read in a crash
char at[512] = {0};
// Python's traceback dump, faulthandler's: found at install
using DumpThreads = const char* (*)(int, void*, void*);
DumpThreads dumpThreads = nullptr;
bool systemReport = false;

#if !defined(FC_OS_WIN32)

volatile sig_atomic_t entered = 0;
// Its own stack: a crash for want of stack has none left to run on
constexpr std::size_t AltSize = 256 * 1024;
char altStack[AltSize];
constexpr int Signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};

#if defined(FC_OS_MACOSX) || defined(FC_OS_LINUX)

// Every thread its own stack for the handler (docs/WorkerProcess.md sec
// 75). An alternate stack is the thread's that set it: with the
// installing thread's alone, a thread of OCCT's pool, Qt's or Python's
// that ran out of stack faulted again in the handler, and the worker went
// with nothing said. A thread started once install() has run is given one
// as it starts, by pthread_create() interposed below -- through dyld's
// interpose section on macOS, by a definition ahead of libc's on Linux
// (sec 82); in a process that is no worker, install() never runs and
// nothing is given.
std::atomic<bool> threadStacks {false};

struct Start
{
    void* (*routine)(void*);
    void* arg;
};

void dropStack(void* stack)
{
    stack_t off;
    std::memset(&off, 0, sizeof(off));
    off.ss_flags = SS_DISABLE;
    sigaltstack(&off, nullptr);
    munmap(stack, AltSize);
}

void* started(void* given)
{
    const Start start = *static_cast<Start*>(given);
    std::free(given);
    // Mapped, not allocated: a page costs nothing until a crash writes it
    void* stack = mmap(nullptr, AltSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (stack == MAP_FAILED) {
        return start.routine(start.arg);
    }
    stack_t ss;
    std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp = stack;
    ss.ss_size = AltSize;
    if (sigaltstack(&ss, nullptr) != 0) {
        munmap(stack, AltSize);
        return start.routine(start.arg);
    }
    void* result = nullptr;
    // Dropped however the thread ends: returning, pthread_exit(), cancelled
    pthread_cleanup_push(dropStack, stack);
    result = start.routine(start.arg);
    pthread_cleanup_pop(1);
    return result;
}

#endif  // FC_OS_MACOSX || FC_OS_LINUX

void put(const char* text)
{
    std::size_t left = std::strlen(text);
    while (left > 0) {
        const ssize_t done = ::write(STDERR_FILENO, text, left);
        if (done <= 0) {
            return;
        }
        text += done;
        left -= static_cast<std::size_t>(done);
    }
}

#else  // FC_OS_WIN32

volatile LONG entered = 0;
// Room kept back on a thread's stack for the report when the stack itself
// overflowed: Windows' way to an alternate stack
constexpr ULONG Guarantee = 64 * 1024;
// Every thread that room (docs/WorkerProcess.md sec 78), as a thread on
// macOS is given its own stack: with the installing thread's alone, a
// thread of OCCT's pool, Qt's or Python's that ran out of stack had the
// guard page's few KiB for the handler, and faulted again in it. A thread
// started once install() has run is given it as it starts, by the TLS
// callback at the end of this file; a process that is no worker is given
// nothing
std::atomic<bool> threadGuarantees {false};
// The worker's stderr, its log: taken at install, written with WriteFile
HANDLE errors = INVALID_HANDLE_VALUE;
// An image's path, for a frame: not on the heap, which a crash may have broken
char imagePath[2048];

void put(const char* text)
{
    if (errors == INVALID_HANDLE_VALUE || !errors) {
        return;
    }
    DWORD left = static_cast<DWORD>(std::strlen(text));
    while (left > 0) {
        DWORD done = 0;
        if (!::WriteFile(errors, text, left, &done, nullptr) || done == 0) {
            return;
        }
        text += done;
        left -= done;
    }
}

#endif  // FC_OS_WIN32

void putHex(unsigned long long value)
{
    char buf[19];
    buf[18] = 0;
    int i = 18;
    do {
        buf[--i] = "0123456789abcdef"[value & 0xf];
        value >>= 4;
    } while (value && i > 2);
    buf[--i] = 'x';
    buf[--i] = '0';
    put(buf + i);
}

void putNumber(long long value)
{
    char buf[24];
    buf[23] = 0;
    int i = 23;
    const bool negative = value < 0;
    unsigned long long v = negative ? 0ULL - static_cast<unsigned long long>(value)
                                    : static_cast<unsigned long long>(value);
    do {
        buf[--i] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v && i > 1);
    if (negative) {
        buf[--i] = '-';
    }
    put(buf + i);
}

#if !defined(FC_OS_WIN32)

void putFrame(void* address)
{
    Dl_info d;
    std::memset(&d, 0, sizeof(d));
    const bool known = dladdr(address, &d) != 0;
    const auto a = reinterpret_cast<unsigned long long>(address);
    put("frame ");
    putHex(a);
    put(" ");
    putHex(known ? reinterpret_cast<unsigned long long>(d.dli_fbase) : 0);
    put(" ");
    put(known && d.dli_sname ? d.dli_sname : "-");
    put(" ");
    putHex(known && d.dli_saddr ? a - reinterpret_cast<unsigned long long>(d.dli_saddr) : 0);
    put(" ");
    put(known && d.dli_fname ? d.dli_fname : "-");
    put("\n");
}

#if defined(FC_OS_MACOSX)
/** The frames of the thread that crashed, from where it was (`context`).
 *
 * Not backtrace(): run on the handler's own stack, which is not the thread's,
 * macOS's stops at its first frame and says nothing. Each frame record is
 * checked to lie in the thread's stack, above the last, before it is read.
 */
void putFrames(void* context)
{
    auto uc = static_cast<ucontext_t*>(context);
    if (!uc || !uc->uc_mcontext) {
        return;
    }
#if defined(__arm64__) || defined(__aarch64__)
    const auto& ss = uc->uc_mcontext->__ss;
    auto pc = static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_pc(ss));
    auto lr = static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_lr(ss));
    auto fp = static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_fp(ss));
#elif defined(__x86_64__)
    const auto& ss = uc->uc_mcontext->__ss;
    std::uintptr_t pc = ss.__rip;
    std::uintptr_t lr = 0;
    std::uintptr_t fp = ss.__rbp;
#else
    return;
#endif
    const auto top = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    const std::uintptr_t bottom = top - pthread_get_stacksize_np(pthread_self());
    auto onStack = [&](std::uintptr_t at) {
        return at >= bottom && at + 2 * sizeof(void*) <= top && at % sizeof(void*) == 0;
    };
    putFrame(reinterpret_cast<void*>(pc));
    // A return address signed for pointer authentication: its top bits off
    auto strip = [](std::uintptr_t a) {
        return a & 0x00007fffffffffffULL;
    };
    // The caller of a function that had not set up its frame, or had no
    // need of one, is in the link register only
    if (lr && (!onStack(fp) || strip(reinterpret_cast<std::uintptr_t*>(fp)[1]) != strip(lr))) {
        putFrame(reinterpret_cast<void*>(strip(lr)));
    }
    for (int i = 0; i < 128 && onStack(fp); ++i) {
        const auto* record = reinterpret_cast<std::uintptr_t*>(fp);
        const std::uintptr_t ret = strip(record[1]);
        if (!ret) {
            break;
        }
        putFrame(reinterpret_cast<void*>(ret));
        if (record[0] <= fp) {
            break;
        }
        fp = record[0];
    }
}
#endif

void handler(int sig, siginfo_t* info, void* context)
{
    if (entered) {
        // A crash in the handler: the default, and the system's report
        std::signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    entered = 1;
    put("\n");
    put(Begin);
    put("\nsignal ");
    putNumber(sig);
    put(" code ");
    putNumber(info ? info->si_code : 0);
    put(" address ");
    putHex(info ? reinterpret_cast<unsigned long long>(info->si_addr) : 0);
    put(" sender ");
    // A pid only for one sent, which si_code says: SI_USER, or on Linux
    // any code not above 0 (SI_TKILL, SI_QUEUE)
    const bool sent = info && (info->si_code == SI_USER || info->si_code <= 0);
    putNumber(sent ? static_cast<long long>(info->si_pid) : 0);
    put("\nat ");
    put(at);
    put("\n");
    // Python's first: the frames of a broken stack may crash their walk
    put("python\n");
    if (dumpThreads) {
        if (const char* failed = dumpThreads(STDERR_FILENO, nullptr, nullptr)) {
            put(failed);
            put("\n");
        }
    }
    put("end of python\n");
#if defined(FC_OS_MACOSX)
    putFrames(context);
#else
    // Where it was, for the pool to leave out the frames above it: the
    // handler's and the signal's trampoline, whose names dladdr() may not
    // know
    if (auto uc = static_cast<ucontext_t*>(context)) {
#if defined(__x86_64__)
        put("pc ");
        putHex(static_cast<unsigned long long>(uc->uc_mcontext.gregs[REG_RIP]));
        put("\n");
#elif defined(__aarch64__)
        put("pc ");
        putHex(static_cast<unsigned long long>(uc->uc_mcontext.pc));
        put("\n");
#endif
    }
    void* frames[128];
    const int count = backtrace(frames, 128);
    for (int i = 0; i < count; ++i) {
        putFrame(frames[i]);
    }
#endif
    put(End);
    put("\n");
    if (systemReport) {
        // Died of the signal, as a process does: the system's crash report
        // made too (macOS's ReportCrash, a core where they are on)
        struct sigaction act;
        std::memset(&act, 0, sizeof(act));
        act.sa_handler = SIG_DFL;
        sigaction(sig, &act, nullptr);
        raise(sig);
        return;
    }
    // Ended rather than killed by it: a death by the signal is what makes
    // macOS write a crash report for every worker that goes, and the pool
    // has this one
    _exit(128 + sig);
}

#else  // FC_OS_WIN32

/** The exported name nearest below `address` in the image `module`, from its
 * export table, and where that name starts (`start`); nullptr for none. A
 * function the image does not export is told as the export before it, as
 * dladdr() does; the lines (resolveLines()) name it from the PDB.
 */
const char* exportBelow(HMODULE module, ULONG_PTR address, ULONG_PTR& start)
{
    const auto* image = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) {
        return nullptr;
    }
    const auto* exports =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image + dir.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(image + exports->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(image + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(image + exports->AddressOfNameOrdinals);
    const ULONG_PTR rva = address - reinterpret_cast<ULONG_PTR>(image);
    DWORD best = 0;
    const char* name = nullptr;
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const WORD ordinal = ordinals[i];
        if (ordinal >= exports->NumberOfFunctions) {
            continue;
        }
        const DWORD at = functions[ordinal];
        // A forwarder names another image's export, not code here
        if (at >= dir.VirtualAddress && at < dir.VirtualAddress + dir.Size) {
            continue;
        }
        if (at <= rva && at > best) {
            best = at;
            name = reinterpret_cast<const char*>(image + names[i]);
        }
    }
    // An image exports few of its functions: the export below may be
    // megabytes away, and its name would mislead (FreeCADApp.dll's frames
    // read "PropertyLinkSubList::Save + 0x9c2f9a"). None past a function's
    // likely size
    constexpr DWORD Farthest = 0x10000;
    if (!name || rva - best > Farthest) {
        return nullptr;
    }
    start = reinterpret_cast<ULONG_PTR>(image) + best;
    return name;
}

void putFrame(ULONG_PTR address)
{
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address),
                              &module)) {
        module = nullptr;
    }
    ULONG_PTR start = 0;
    const char* name = module ? exportBelow(module, address, start) : nullptr;
    const DWORD length = module ? ::GetModuleFileNameA(module, imagePath, sizeof(imagePath)) : 0;
    put("frame ");
    putHex(address);
    put(" ");
    putHex(reinterpret_cast<ULONG_PTR>(module));
    put(" ");
    put(name && *name ? name : "-");
    put(" ");
    putHex(name ? address - start : 0);
    put(" ");
    put(length > 0 && length < sizeof(imagePath) ? imagePath : "-");
    put("\n");
}

/// The frames of the thread that crashed, unwound from where it was (`where`)
/// by the images' own unwind data: no dbghelp in the crashed process
void putFrames(const CONTEXT* where)
{
#if defined(_M_X64)
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    ::GetCurrentThreadStackLimits(&low, &high);
    CONTEXT c = *where;
    for (int i = 0; i < 128; ++i) {
        putFrame(static_cast<ULONG_PTR>(c.Rip));
        const DWORD64 sp = c.Rsp;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = ::RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (function) {
            PVOID data = nullptr;
            DWORD64 frame = 0;
            ::RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                               imageBase,
                               c.Rip,
                               function,
                               &c,
                               &data,
                               &frame,
                               nullptr);
        }
        else {
            // A leaf, or no code at all (a call through a null pointer): the
            // return address is on top of the stack
            if (c.Rsp < low || c.Rsp + sizeof(DWORD64) > high) {
                break;
            }
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
            c.Rsp += sizeof(DWORD64);
        }
        if (!c.Rip || c.Rsp <= sp || c.Rsp < low || c.Rsp > high) {
            break;
        }
    }
#else
    putFrame(0);
    (void)where;
#endif
}

/// The signal a POSIX worker would have died of, for an exception's code
int signalOf(DWORD code)
{
    switch (code) {
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
            return SIGILL;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case EXCEPTION_FLT_STACK_CHECK:
            return SIGFPE;
        default:  // an access violation, a page not read in, the stack overflowed
            return SIGSEGV;
    }
}

void putReport(int sig, DWORD code, ULONG_PTR address, const CONTEXT* where)
{
    put("\n");
    put(Begin);
    put("\nsignal ");
    putNumber(sig);
    put(" code ");
    putHex(code);
    put(" address ");
    putHex(address);
    put(" sender 0\nat ");
    put(at);
    put("\n");
    // Python's first: the frames of a broken stack may crash their walk
    put("python\n");
    if (dumpThreads) {
        if (const char* failed = dumpThreads(2, nullptr, nullptr)) {
            put(failed);
            put("\n");
        }
    }
    put("end of python\n");
    if (where) {
#if defined(_M_X64)
        put("pc ");
        putHex(static_cast<unsigned long long>(where->Rip));
        put("\n");
#endif
        putFrames(where);
    }
    put(End);
    put("\n");
}

/// Every exception nothing caught, in any thread of the worker
LONG WINAPI unhandled(EXCEPTION_POINTERS* pointers)
{
    if (::InterlockedExchange(&entered, 1)) {
        // A crash in the handler: the system's own handling
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* record = pointers ? pointers->ExceptionRecord : nullptr;
    const DWORD code = record ? record->ExceptionCode : 0;
    ULONG_PTR address = record ? reinterpret_cast<ULONG_PTR>(record->ExceptionAddress) : 0;
    // An access violation's second word is the address it was denied
    if (record && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
        && record->NumberParameters >= 2) {
        address = record->ExceptionInformation[1];
    }
    putReport(signalOf(code), code, address, pointers ? pointers->ContextRecord : nullptr);
    if (systemReport) {
        // On to Windows Error Reporting, as a process with no handler goes
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Ended with the exception's code: QProcess has it as a crash,
    // "crashed (0xc0000005)", and no system report is made of it
    ::TerminateProcess(::GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

/// abort(), std::terminate() among its callers: the CRT raises SIGABRT
void aborted(int sig)
{
    if (::InterlockedExchange(&entered, 1)) {
        return;
    }
    CONTEXT here;
    std::memset(&here, 0, sizeof(here));
    ::RtlCaptureContext(&here);
    putReport(sig, 0, 0, &here);
    if (systemReport) {
        // The CRT's own end of an abort, its report included
        return;
    }
    ::TerminateProcess(::GetCurrentProcess(), 3);
}

#endif  // FC_OS_WIN32

}  // namespace

void WorkerCrash::install()
{
#if !defined(FC_OS_WIN32)
    systemReport = GetApplication()
                       .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
                       ->GetBool("SystemCrashReport", false);
    // 3.12 has it as _Py_DumpTracebackThreads, 3.14 as the unstable API
    for (const char* name : {"PyUnstable_DumpTracebackThreads", "_Py_DumpTracebackThreads"}) {
        if (void* found = dlsym(RTLD_DEFAULT, name)) {
            dumpThreads = reinterpret_cast<DumpThreads>(found);
            break;
        }
    }
    // backtrace() loads what it needs on its first call, which allocates
    void* prime[1];
    backtrace(prime, 1);
    stack_t ss;
    std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altStack;
    ss.ss_size = AltSize;
    sigaltstack(&ss, nullptr);
#if defined(FC_OS_MACOSX) || defined(FC_OS_LINUX)
    threadStacks = true;
#endif
    struct sigaction act;
    std::memset(&act, 0, sizeof(act));
    act.sa_sigaction = handler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&act.sa_mask);
    for (int sig : Signals) {
        sigaction(sig, &act, nullptr);
    }
#else
    systemReport = GetApplication()
                       .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
                       ->GetBool("SystemCrashReport", false);
    errors = ::GetStdHandle(STD_ERROR_HANDLE);
    // Python's traceback dump, from the DLL this process has Python in
    HMODULE python = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&Py_Initialize),
                             &python)) {
        for (const char* name : {"PyUnstable_DumpTracebackThreads", "_Py_DumpTracebackThreads"}) {
            if (FARPROC found = ::GetProcAddress(python, name)) {
                dumpThreads = reinterpret_cast<DumpThreads>(found);
                break;
            }
        }
    }
    // Room for the report when the stack itself overflowed: kept back on the
    // thread that makes the jobs, this one, and on every thread started from
    // now on
    ULONG reserve = Guarantee;
    ::SetThreadStackGuarantee(&reserve);
    threadGuarantees = true;
    // Not a vectored handler: that one hears first-chance exceptions too,
    // which something may yet catch (ctypes does, for one)
    ::SetUnhandledExceptionFilter(unhandled);
    // What Application::init put in the way, for the GUI's sake: its SIGSEGV
    // handler, which the C runtime's frame around main() calls for an access
    // violation, throws "Illegal storage access!", and its SE translator turns
    // the exception into a C++ one. Either way a worker's crash came back as
    // a job that failed, from a worker that went on (measured: the job's
    // error was that text, Died false). A worker dies of it instead
    std::signal(SIGSEGV, SIG_DFL);
    std::signal(SIGILL, SIG_DFL);
    std::signal(SIGFPE, SIG_DFL);
    std::signal(SIGABRT, aborted);
#if defined(_MSC_VER)
    _set_se_translator(nullptr);
#endif
    // No Windows Error Reporting dialog: one would hold the worker's end
    // until someone answered it, and the pool would wait for a worker that
    // is gone. Workers/SystemCrashReport has the report made, dialog and all
    if (!systemReport) {
        ::SetErrorMode(::GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER)
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    }
#endif
}

void WorkerCrash::setAt(const std::string& where)
{
    const std::size_t n = std::min(where.size(), sizeof(at) - 1);
    // Emptied first and its first letter put back last: a crash in the
    // middle reads an empty name, not one half the last
    at[0] = 0;
    if (n > 1) {
        std::memcpy(at + 1, where.c_str() + 1, n - 1);
    }
    at[n] = 0;
    if (n > 0) {
        at[0] = where[0];
    }
}

std::string WorkerCrash::signalName(int sig)
{
    std::string text = "signal " + std::to_string(sig);
#if !defined(FC_OS_WIN32)
    if (const char* name = strsignal(sig)) {
        // macOS ends it in the number: "Segmentation fault: 11"
        std::string s = name;
        text += ": " + s.substr(0, s.find(':'));
    }
#else
    // No strsignal() here: the ones a worker's report can name
    switch (sig) {
        case SIGSEGV:
            text += ": Segmentation fault";
            break;
        case SIGILL:
            text += ": Illegal instruction";
            break;
        case SIGFPE:
            text += ": Floating point exception";
            break;
        case SIGABRT:
            text += ": Abort";
            break;
        default:
            break;
    }
#endif
    return text;
}

namespace
{

unsigned long long hex(const std::string& s)
{
    return std::strtoull(s.c_str(), nullptr, 16);
}

#if defined(FC_OS_WIN32)
/// dbghelp is not thread safe: every call of it here is made under this
std::mutex& dbghelpLock()
{
    static std::mutex lock;
    return lock;
}
#endif

std::string demangle(const std::string& name)
{
#if !defined(FC_OS_WIN32)
    int status = -1;
    // Mach-O names have one more underscore than the C++ ABI's
    for (const char* tried : {name.c_str(), name.size() > 1 && name[0] == '_' ? name.c_str() + 1
                                                                              : nullptr}) {
        if (!tried) {
            continue;
        }
        char* out = abi::__cxa_demangle(tried, nullptr, nullptr, &status);
        if (status == 0 && out) {
            std::string result = out;
            std::free(out);
            return result;
        }
        std::free(out);
    }
#else
    // An MSVC name, "?serve@WorkerPool@App@@QEAAHXZ": its name only, as the
    // frame's line has no room for the signature
    if (!name.empty() && name[0] == '?') {
        char out[1024];
        std::lock_guard<std::mutex> lock(dbghelpLock());
        if (::UnDecorateSymbolName(name.c_str(), out, sizeof(out), UNDNAME_NAME_ONLY) > 0) {
            return out;
        }
    }
#endif
    return name;
}

std::string baseName(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// The frames to leave out: the handler's own, up to the signal's trampoline
std::size_t firstOfTheCrash(const std::vector<WorkerCrash::Frame>& frames)
{
    for (std::size_t i = 0; i < frames.size() && i < 8; ++i) {
        const std::string& s = frames[i].symbol;
        if (s == "_sigtramp" || s == "__restore_rt" || s == "__kernel_rt_sigreturn") {
            return i + 1;
        }
    }
    return 0;
}

}  // namespace

WorkerCrash::Report WorkerCrash::read(const std::string& log)
{
    Report report;
    const auto begin = log.rfind(Begin);
    if (begin == std::string::npos) {
        return report;
    }
    report.found = true;
    std::istringstream in(log.substr(begin + std::strlen(Begin)));
    std::string line;
    bool python = false;
    unsigned long long pc = 0;
    while (std::getline(in, line)) {
        // Python writes its traceback through the C runtime, which on
        // Windows ends a line in "\r\n"
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == End) {
            report.whole = true;
            break;
        }
        if (python) {
            if (line == "end of python") {
                python = false;
            }
            else {
                report.python += line + "\n";
            }
            continue;
        }
        std::istringstream words(line);
        std::string word;
        words >> word;
        if (word == "signal") {
            std::string key;
            std::string value;
            words >> report.signal;
            while (words >> key >> value) {
                if (key == "code") {
                    // si_code in decimal, or a Windows exception's in hex
                    report.code = static_cast<int>(
                        static_cast<unsigned int>(std::strtoll(value.c_str(), nullptr, 0)));
                }
                else if (key == "address") {
                    report.address = hex(value);
                }
                else if (key == "sender") {
                    report.sender = std::atol(value.c_str());
                }
            }
        }
        else if (word == "at") {
            report.at = line.size() > 3 ? line.substr(3) : std::string();
        }
        else if (word == "frame") {
            Frame f;
            std::string address;
            std::string base;
            std::string symbol;
            std::string offset;
            words >> address >> base >> symbol >> offset;
            std::string image;
            std::getline(words, image);
            if (!image.empty() && image[0] == ' ') {
                image.erase(0, 1);
            }
            f.address = hex(address);
            f.base = hex(base);
            f.symbol = symbol == "-" ? std::string() : demangle(symbol);
            f.offset = hex(offset);
            f.image = image == "-" ? std::string() : image;
            report.frames.push_back(std::move(f));
        }
        else if (word == "python") {
            python = true;
        }
        else if (word == "pc") {
            std::string value;
            words >> value;
            pc = hex(value);
        }
    }
    std::size_t first = firstOfTheCrash(report.frames);
    // The frame of where it was, when the handler said: above it are the
    // handler's own
    for (std::size_t i = 0; pc && i < report.frames.size() && i < 8; ++i) {
        const auto a = report.frames[i].address;
        if (a + 1 >= pc && a <= pc + 1) {
            first = i;
            break;
        }
    }
    report.frames.erase(report.frames.begin(),
                        report.frames.begin() + static_cast<std::ptrdiff_t>(first));
    return report;
}

namespace
{

/// What a tool says, with a deadline: an empty string for nothing
std::string runTool(const QString& program, const QStringList& args)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, args);
    if (!p.waitForStarted(5000)) {
        return {};
    }
    p.closeWriteChannel();
    if (!p.waitForFinished(20000)) {
        p.kill();
        p.waitForFinished(1000);
        return {};
    }
    return p.readAllStandardOutput().toStdString();
}

/// "Document.cpp:18989" out of what atos or addr2line say of a frame
std::string lineOf(const std::string& said)
{
    // atos: "fn (in lib) (File.cpp:12)"; addr2line: "/path/File.cpp:12",
    // or "/path/File.cpp:12 (discriminator 3)"
    std::string where = said;
    const auto discriminator = where.find(" (discriminator");
    if (discriminator != std::string::npos) {
        where.erase(discriminator);
    }
    if (!where.empty() && where.back() == ')') {
        const auto open = where.rfind('(');
        if (open != std::string::npos) {
            where = where.substr(open + 1, where.size() - open - 2);
        }
    }
    if (where.find(':') == std::string::npos || where.rfind("??", 0) == 0
        || where.find(" (in ") != std::string::npos) {
        return {};
    }
    return baseName(where);
}

QString atos()
{
#if defined(FC_OS_MACOSX)
    // Only where the developer tools are: /usr/bin/atos without them asks
    // the user to install them, in a dialog
    for (const char* dir : {"/Library/Developer/CommandLineTools/usr/bin/atos",
                            "/Applications/Xcode.app/Contents/Developer/usr/bin/atos"}) {
        if (Base::FileInfo(dir).exists()) {
            return QString::fromLatin1(dir);
        }
    }
    return {};
#else
    return QStandardPaths::findExecutable(QStringLiteral("addr2line"));
#endif
}

#if defined(FC_OS_WIN32)
std::string narrow(const wchar_t* text)
{
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

/** The frames' functions and lines from the images' PDBs, by dbghelp in this
 * process: each image loaded at the base it had in the worker, on a session
 * of its own. A frame whose image has no PDB keeps what the worker said.
 */
void resolveByPdb(WorkerCrash::Report& report)
{
    std::lock_guard<std::mutex> lock(dbghelpLock());
    // Any value unique to this session, not a process: nothing is read of one
    static const char tag = 0;
    const HANDLE session = reinterpret_cast<HANDLE>(const_cast<char*>(&tag));
    const DWORD options = ::SymGetOptions();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS
                    | SYMOPT_NO_PROMPTS);
    if (!::SymInitializeW(session, nullptr, FALSE)) {
        ::SymSetOptions(options);
        return;
    }
    std::set<unsigned long long> loaded;
    for (std::size_t i = 0; i < report.frames.size(); ++i) {
        WorkerCrash::Frame& f = report.frames[i];
        if (f.image.empty() || !f.base || !Base::FileInfo(f.image).exists()) {
            continue;
        }
        if (loaded.insert(f.base).second) {
            const int size = ::MultiByteToWideChar(CP_ACP, 0, f.image.c_str(), -1, nullptr, 0);
            std::wstring path(static_cast<std::size_t>(std::max(size, 1)), L'\0');
            ::MultiByteToWideChar(CP_ACP, 0, f.image.c_str(), -1, path.data(), size);
            ::SymLoadModuleExW(session, nullptr, path.c_str(), nullptr, f.base, 0, nullptr, 0);
        }
        // A return address is the instruction after the call: one back is
        // the call's line
        const DWORD64 address = f.address - (i ? 1 : 0);
        alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)];
        auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
        std::memset(buffer, 0, sizeof(buffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol->MaxNameLen = 512;
        DWORD64 displacement = 0;
        if (::SymFromAddrW(session, address, &displacement, symbol)) {
            f.symbol = narrow(symbol->Name);
            f.offset = f.address - symbol->Address;
        }
        IMAGEHLP_LINEW64 line;
        std::memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD column = 0;
        if (::SymGetLineFromAddrW64(session, address, &column, &line) && line.FileName) {
            f.line = baseName(narrow(line.FileName)) + ":" + std::to_string(line.LineNumber);
        }
    }
    ::SymCleanup(session);
    ::SymSetOptions(options);
}
#endif

}  // namespace

void WorkerCrash::resolveLines(Report& report)
{
#if defined(FC_OS_WIN32)
    resolveByPdb(report);
    return;
#endif
    const QString tool = atos();
    if (tool.isEmpty()) {
        return;
    }
    // One run of the tool for each image, its frames in order
    std::map<std::string, std::vector<std::size_t>> byImage;
    for (std::size_t i = 0; i < report.frames.size(); ++i) {
        const Frame& f = report.frames[i];
        // The system's own are in the shared cache, not on disk
        if (f.image.empty() || !f.base || f.image.rfind("/usr/lib/", 0) == 0
            || f.image.rfind("/System/", 0) == 0 || !Base::FileInfo(f.image).exists()) {
            continue;
        }
        byImage[f.image].push_back(i);
    }
    for (auto& [image, which] : byImage) {
        QStringList args;
        std::ostringstream ss;
#if defined(FC_OS_MACOSX)
        ss << "0x" << std::hex << report.frames[which.front()].base;
        args << QStringLiteral("-o") << QString::fromStdString(image) << QStringLiteral("-l")
             << QString::fromStdString(ss.str());
        for (std::size_t i : which) {
            // A return address is the instruction after the call: one back
            // is the call's line
            std::ostringstream a;
            a << "0x" << std::hex << report.frames[i].address - (i ? 1 : 0);
            args << QString::fromStdString(a.str());
        }
#else
        args << QStringLiteral("-e") << QString::fromStdString(image);
        for (std::size_t i : which) {
            std::ostringstream a;
            a << "0x" << std::hex
              << report.frames[i].address - report.frames[i].base - (i ? 1 : 0);
            args << QString::fromStdString(a.str());
        }
#endif
        std::istringstream said(runTool(tool, args));
        std::string line;
        for (std::size_t i : which) {
            if (!std::getline(said, line)) {
                break;
            }
            report.frames[i].line = lineOf(line);
        }
    }
}

std::string WorkerCrash::what(const Report& report)
{
    std::ostringstream ss;
#if defined(FC_OS_WIN32)
    // A Windows exception is told by its code, as the system tells it
    if (report.code) {
        const auto code = static_cast<unsigned int>(report.code);
        ss << "exception 0x" << std::hex << code;
        const char* name = nullptr;
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:
                name = "access violation";
                break;
            case EXCEPTION_IN_PAGE_ERROR:
                name = "page not read in";
                break;
            case EXCEPTION_STACK_OVERFLOW:
                name = "stack overflow";
                break;
            case EXCEPTION_ILLEGAL_INSTRUCTION:
                name = "illegal instruction";
                break;
            case EXCEPTION_PRIV_INSTRUCTION:
                name = "privileged instruction";
                break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
                name = "integer divide by zero";
                break;
            default:
                break;
        }
        if (name) {
            ss << " (" << name << ")";
        }
        if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
            ss << ", at address 0x" << std::hex << report.address;
        }
        return ss.str();
    }
#endif
    ss << signalName(report.signal);
    if (report.sender > 0) {
        ss << ", sent by process " << report.sender;
    }
#if defined(SIGBUS)  // not on Windows
    else if (report.signal == SIGSEGV || report.signal == SIGBUS) {
#else
    else if (report.signal == SIGSEGV) {
#endif
        ss << ", at address 0x" << std::hex << report.address;
    }
    return ss.str();
}

std::string WorkerCrash::text(const Report& report)
{
    std::ostringstream ss;
    ss << what(report);
    if (!report.at.empty()) {
        ss << ", in " << report.at;
    }
    ss << "\nthe thread that crashed:\n";
    for (std::size_t i = 0; i < report.frames.size(); ++i) {
        const Frame& f = report.frames[i];
        ss << "#" << i << "  ";
        if (!f.symbol.empty()) {
            ss << f.symbol << " + 0x" << std::hex << f.offset << std::dec;
        }
        else {
            ss << "0x" << std::hex << f.address << std::dec;
        }
        if (!f.image.empty()) {
            ss << "  (" << baseName(f.image);
            if (f.base) {
                ss << " +0x" << std::hex << f.address - f.base << std::dec;
            }
            ss << ")";
        }
        if (!f.line.empty()) {
            ss << "  " << f.line;
        }
        ss << "\n";
    }
    if (!report.python.empty()) {
        ss << "Python:\n" << report.python;
    }
    if (!report.whole) {
        ss << "(the report ends here: the handler did not get to its end)\n";
    }
    return ss.str();
}

#if defined(FC_OS_MACOSX)

/// pthread_create() as every image of the process calls it: dyld puts this
/// in its place in all the images loaded at launch but this one, so the
/// call below is the system's own (docs/WorkerProcess.md sec 77)
extern "C" int fcWorkerCrashPthreadCreate(pthread_t* thread, const pthread_attr_t* attr,
                                          void* (*routine)(void*), void* arg)
{
    if (!threadStacks.load(std::memory_order_relaxed)) {
        return pthread_create(thread, attr, routine, arg);
    }
    auto start = static_cast<Start*>(std::malloc(sizeof(Start)));
    if (!start) {
        return pthread_create(thread, attr, routine, arg);
    }
    start->routine = routine;
    start->arg = arg;
    const int result = pthread_create(thread, attr, started, start);
    if (result != 0) {
        std::free(start);
    }
    return result;
}

namespace
{
struct Interpose
{
    const void* replacement;
    const void* replacee;
};
__attribute__((used)) const Interpose interposed[] __attribute__((section("__DATA,__interpose"))) = {
    {reinterpret_cast<const void*>(&fcWorkerCrashPthreadCreate),
     reinterpret_cast<const void*>(&pthread_create)},
};
}  // namespace

#endif  // FC_OS_MACOSX

#if defined(FC_OS_LINUX)

/// pthread_create() as every library of the process calls it
/// (docs/WorkerProcess.md sec 82). ELF has no interpose section: a call
/// binds to the first definition in the process's global search order,
/// and FreeCADApp, which the executables link, comes ahead of libc there.
/// So this definition is the one every library calls -- OCCT's pool, Qt,
/// Python -- and the system's is the next one (RTLD_NEXT).  Outside a
/// worker it passes the call straight on
extern "C" __attribute__((visibility("default"))) int
pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*routine)(void*),
               void* arg) noexcept
{
    using Create = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    static const Create system = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    if (!system) {
        return EAGAIN;
    }
    if (!threadStacks.load(std::memory_order_relaxed)) {
        return system(thread, attr, routine, arg);
    }
    auto start = static_cast<Start*>(std::malloc(sizeof(Start)));
    if (!start) {
        return system(thread, attr, routine, arg);
    }
    start->routine = routine;
    start->arg = arg;
    const int result = system(thread, attr, started, start);
    if (result != 0) {
        std::free(start);
    }
    return result;
}

#endif  // FC_OS_LINUX

#if defined(FC_OS_WIN32) && defined(_MSC_VER) && defined(_WIN64)

namespace
{
/// Called by the loader on each thread as it starts, before the thread's
/// routine, on that thread: the place to keep its room back
/// (docs/WorkerProcess.md sec 78)
void NTAPI threadStarted(PVOID /*image*/, DWORD reason, PVOID /*reserved*/)
{
    if (reason == DLL_THREAD_ATTACH && threadGuarantees.load(std::memory_order_relaxed)) {
        ULONG reserve = Guarantee;
        ::SetThreadStackGuarantee(&reserve);
    }
}
}  // namespace

// The image's TLS directory, and this callback kept in it: neither is
// referred to, so each is named to the linker
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:fcWorkerCrashThreadStarted")
#pragma const_seg(".CRT$XLF")
extern "C" const PIMAGE_TLS_CALLBACK fcWorkerCrashThreadStarted = threadStarted;
#pragma const_seg()

#endif  // FC_OS_WIN32
