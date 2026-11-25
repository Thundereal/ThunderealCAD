// SPDX-License-Identifier: LGPL-2.1-or-later

// The folders a process leaves in the user cache when it is killed or
// crashes: docs/WorkerProcess.md sec 49.

#include "PreCompiled.h"

#include <cerrno>
#include <cstdlib>

#ifdef _WIN32
# include <windows.h>
#else
# include <csignal>
# ifdef __APPLE__
#  include <sys/sysctl.h>
#  include <sys/types.h>
# endif
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QThreadPool>

#include <Base/Console.h>
#include <Base/FileInfo.h>

#include "Application.h"
#include "CacheSweep.h"
#include "ShuttingDown.h"

namespace
{

/// When the process \a pid started, in msecs since the epoch: 0 when it
/// runs and that is not known, -1 when no such process runs
qint64 startOf(long long pid)
{
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!handle) {
        // Anything but "no such process" -- access denied, mostly -- runs
        return GetLastError() == ERROR_INVALID_PARAMETER ? -1 : 0;
    }
    qint64 start = 0;
    DWORD code = 0;
    FILETIME created, exited, kernel, user;
    if (GetExitCodeProcess(handle, &code) && code != STILL_ACTIVE) {
        start = -1;  // ended, and something still holds a handle to it
    }
    else if (GetProcessTimes(handle, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER ticks;
        ticks.LowPart = created.dwLowDateTime;
        ticks.HighPart = created.dwHighDateTime;
        // 100 ns ticks since 1601-01-01 UTC
        start = static_cast<qint64>(ticks.QuadPart / 10000) - Q_INT64_C(11644473600000);
    }
    CloseHandle(handle);
    return start;
#else
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
        return -1;
    }
# ifdef __APPLE__
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && size == sizeof(info)) {
        const struct timeval& tv = info.kp_proc.p_starttime;
        return static_cast<qint64>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    }
# endif
    return 0;
#endif
}

/// Whether the folder \a folder of a process \a pid may be that of a process
/// that runs: true when that cannot be told. Pids are reused: a process
/// started after the folder was last changed did not make it, and the
/// second of slack only ever keeps one.
bool mayRun(long long pid, const QFileInfo& folder)
{
    const qint64 start = startOf(pid);
    if (start < 0) {
        return false;
    }
    return start == 0 || start <= folder.lastModified().toMSecsSinceEpoch() + 1000;
}

/// The process id a folder's name ends with, 0 for none
long long pidOf(const QString& name)
{
    bool ok = false;
    const long long pid = name.mid(name.lastIndexOf(QLatin1Char('_')) + 1).toLongLong(&ok);
    return ok ? pid : 0;
}

}  // namespace

App::CacheSweep::Counts App::CacheSweep::run(const std::string& cache,
                                              const std::string& exe,
                                              double before,
                                              const std::function<bool()>& stopped)
{
    Counts counts;
    const QDir dir(QString::fromStdString(cache));
    const QString name = QString::fromStdString(exe);
    const QString reader = name + QStringLiteral("_Reader_");
    const QString document = name + QStringLiteral("_Doc_");
    const qint64 ownPid = QCoreApplication::applicationPid();
    const qint64 last = static_cast<qint64>(before * 1000.0);
    const QFileInfoList folders =
        dir.entryInfoList(QStringList() << reader + QStringLiteral("*") << document + QStringLiteral("*"),
                          QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& folder : folders) {
        if (stopped && stopped()) {
            break;
        }
        const QString fileName = folder.fileName();
        const long long pid = pidOf(fileName);
        if (pid <= 0 || pid == ownPid || folder.lastModified().toMSecsSinceEpoch() >= last
            || mayRun(pid, folder)) {
            continue;
        }
        const std::string path = folder.absoluteFilePath().toStdString();
        if (fileName.startsWith(reader)) {
            // A copy of what a store had, read from: nothing of its own
            if (Base::FileInfo(path).deleteDirectoryRecursive()) {
                ++counts.readers;
            }
            else {
                ++counts.failed;
            }
        }
        else if (QDir(folder.absoluteFilePath()).isEmpty()) {
            if (Base::FileInfo(path).deleteDirectory()) {
                ++counts.documents;
            }
            else if (QFileInfo::exists(folder.absoluteFilePath())) {
                ++counts.failed;
            }
        }
    }
    return counts;
}

void App::CacheSweep::start()
{
    if (std::getenv("THUNDEREAL_WORKER_SERVER")) {
        return;
    }
    const std::string cache = Application::getUserCachePath();
    const std::string exe = Application::getExecutableName();
    const double before = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    QThreadPool::globalInstance()->start([cache, exe, before]() {
        Counts counts;
        try {
            counts = run(cache, exe, before, []() {
                return App::isShuttingDown() || QCoreApplication::closingDown();
            });
        }
        catch (...) {
            return;
        }
        if (counts.readers || counts.documents || counts.failed) {
            Base::Console().log("Removed %d reader and %d empty document folders of ended "
                                "processes from the cache, %d would not go\n",
                                counts.readers,
                                counts.documents,
                                counts.failed);
        }
    });
}
