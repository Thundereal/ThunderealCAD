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

#include "PreCompiled.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>

#if !defined(FC_OS_WIN32)
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/ucontext.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

#if !defined(FC_OS_WIN32)

// Where it is in the job, for the handler: written by the worker's one
// thread that makes jobs, read in a crash
char at[512] = {0};
// Python's traceback dump, faulthandler's: found at install
using DumpThreads = const char* (*)(int, void*, void*);
DumpThreads dumpThreads = nullptr;
bool systemReport = false;
volatile sig_atomic_t entered = 0;
// Its own stack: a crash for want of stack has none left to run on
constexpr std::size_t AltSize = 256 * 1024;
char altStack[AltSize];
constexpr int Signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};

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

#endif  // !FC_OS_WIN32

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
    struct sigaction act;
    std::memset(&act, 0, sizeof(act));
    act.sa_sigaction = handler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&act.sa_mask);
    for (int sig : Signals) {
        sigaction(sig, &act, nullptr);
    }
#else
    // No handler of its own here yet. But no Windows Error Reporting dialog
    // either: one would hold the worker's end until someone answered it,
    // and the pool would wait for a worker that is gone (sec 35)
    if (!GetApplication()
             .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers")
             ->GetBool("SystemCrashReport", false)) {
        ::SetErrorMode(::GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_MSC_VER)
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    }
#endif
}

void WorkerCrash::setAt(const std::string& where)
{
#if !defined(FC_OS_WIN32)
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
#else
    (void)where;
#endif
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
#endif
    return text;
}

namespace
{

unsigned long long hex(const std::string& s)
{
    return std::strtoull(s.c_str(), nullptr, 16);
}

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
                    report.code = std::atoi(value.c_str());
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

}  // namespace

void WorkerCrash::resolveLines(Report& report)
{
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
