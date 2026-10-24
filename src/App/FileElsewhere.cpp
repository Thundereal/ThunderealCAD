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

#include "PreCompiled.h"

#ifndef _PreComp_
# include <chrono>
# include <cstdio>
# include <ctime>
# include <sstream>
# include <thread>
#endif

#ifdef _WIN32
# include <windows.h>
#else
# include <cerrno>
# include <csignal>
# include <fcntl.h>
# include <sys/file.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSysInfo>

#include "FileElsewhere.h"

using namespace App;

namespace
{

std::string beside(const std::string& file, const char* suffix)
{
    QFileInfo fi(QString::fromStdString(file));
    return QDir(fi.absolutePath())
        .filePath(QStringLiteral(".~") + fi.fileName() + QString::fromLatin1(suffix))
        .toStdString();
}

std::string thisHost()
{
    std::string host = QSysInfo::machineHostName().toStdString();
    for (char& c : host) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            c = '_';
    }
    return host.empty() ? std::string("localhost") : host;
}

int64_t thisPid()
{
    return static_cast<int64_t>(QCoreApplication::applicationPid());
}

bool alive(int64_t pid)
{
    if (pid <= 0)
        return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return running;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

double nowSeconds()
{
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

/// The marker's lines, those of gone processes of this host left out.
std::vector<FileOpener> readMarker(const std::string& marker)
{
    std::vector<FileOpener> out;
    QFile in(QString::fromStdString(marker));
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    const std::string host = thisHost();
    std::istringstream lines(in.readAll().toStdString());
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        FileOpener o;
        if (!(fields >> o.host >> o.pid >> o.since))
            continue;
        if (o.host == host && !alive(o.pid))
            continue;
        out.push_back(o);
    }
    return out;
}

/// Written whole and renamed into place, so a reader sees one or the other.
void writeMarker(const std::string& marker, const std::vector<FileOpener>& openers)
{
    const QString path = QString::fromStdString(marker);
    if (openers.empty()) {
        QFile::remove(path);
        return;
    }
    std::ostringstream text;
    text.precision(17);
    for (const auto& o : openers)
        text << o.host << ' ' << o.pid << ' ' << o.since << '\n';
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    const std::string bytes = text.str();
    out.write(bytes.data(), static_cast<qint64>(bytes.size()));
    out.commit();
}

bool isMe(const FileOpener& o)
{
    return o.pid == thisPid() && o.host == thisHost();
}

}   // namespace

DiskStamp DiskStamp::of(const std::string& path)
{
    DiskStamp s;
    s.path = path;
    QString target = QString::fromStdString(path);
    if (QFileInfo(target).isDir())
        target += QStringLiteral("/Document.xml");
#ifdef _WIN32
    QFileInfo fi(target);
    if (!fi.exists())
        return s;
    s.size = static_cast<int64_t>(fi.size());
    s.mtime = static_cast<int64_t>(fi.lastModified().toMSecsSinceEpoch()) * 1000000;
#else
    struct stat st {};
    if (::stat(target.toUtf8().constData(), &st) != 0)
        return s;
    s.size = static_cast<int64_t>(st.st_size);
# ifdef __APPLE__
    s.mtime = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000
        + st.st_mtimespec.tv_nsec;
# else
    s.mtime = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
# endif
    s.inode = static_cast<uint64_t>(st.st_ino);
#endif
    return s;
}

SaveLock::SaveLock(const std::string& file)
    : _path(lockPath(file))
{}

SaveLock::~SaveLock()
{
    unlock();
}

bool SaveLock::tryOnce()
{
#ifdef _WIN32
    const std::wstring wide = QString::fromStdString(_path).toStdWString();
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // Held open, or being deleted by the last to close the document:
        // wait. Anything else means no lock can be made here.
        const DWORD e = GetLastError();
        return !(e == ERROR_SHARING_VIOLATION
                 || (e == ERROR_ACCESS_DENIED && QFileInfo::exists(QString::fromStdString(_path))));
    }
    OVERLAPPED at {};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) {
        CloseHandle(h);
        return false;
    }
    _handle = h;
    const std::string who = thisHost() + " " + std::to_string(thisPid()) + "\n";
    DWORD written = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    WriteFile(h, who.data(), static_cast<DWORD>(who.size()), &written, nullptr);
    SetEndOfFile(h);
#else
    const int fd = ::open(_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return true;   // no lock can be made here; the save says what it meets
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    // Taken on the file the name still names: a last process closing the
    // document may have unlinked the one this opened.
    struct stat mine {};
    struct stat named {};
    if (::fstat(fd, &mine) != 0 || ::stat(_path.c_str(), &named) != 0
            || mine.st_ino != named.st_ino || mine.st_dev != named.st_dev) {
        ::close(fd);
        return false;
    }
    _fd = fd;
    // Who has it, for one that waits to say; not synced, it is a courtesy.
    const std::string who = thisHost() + " " + std::to_string(thisPid()) + "\n";
    if (::ftruncate(fd, 0) == 0 && ::pwrite(fd, who.data(), who.size(), 0) < 0) {
        // nothing to do: the lock is what matters
    }
#endif
    _locked = true;
    return true;
}

bool SaveLock::lock(int timeoutMs, std::string* holder)
{
    if (_locked)
        return true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (tryOnce())
            return true;
        if (std::chrono::steady_clock::now() >= until)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (holder) {
        holder->clear();
        QFile in(QString::fromStdString(_path));
        if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::istringstream fields(in.readAll().toStdString());
            std::string host;
            int64_t pid = 0;
            if (fields >> host >> pid)
                *holder = host + " (pid " + std::to_string(pid) + ")";
        }
        if (holder->empty())
            *holder = "another process";
    }
    return false;
}

void SaveLock::unlock()
{
#ifdef _WIN32
    if (_handle) {
        OVERLAPPED at {};
        UnlockFileEx(static_cast<HANDLE>(_handle), 0, 1, 0, &at);
        CloseHandle(static_cast<HANDLE>(_handle));
        _handle = nullptr;
    }
#else
    if (_fd >= 0) {
        ::flock(_fd, LOCK_UN);
        ::close(_fd);
        _fd = -1;
    }
#endif
    _locked = false;
}

bool SaveLock::isLocked() const
{
    return _locked;
}

std::string SaveLock::lockPath(const std::string& file)
{
    return beside(file, ".lock");
}

std::string FileOpeners::markerPath(const std::string& file)
{
    return beside(file, ".open");
}

std::vector<FileOpener> FileOpeners::others(const std::string& file)
{
    std::vector<FileOpener> out;
    for (const auto& o : readMarker(markerPath(file))) {
        if (!isMe(o))
            out.push_back(o);
    }
    return out;
}

std::vector<FileOpener> FileOpeners::add(const std::string& file)
{
    const std::string marker = markerPath(file);
    std::vector<FileOpener> all;
    std::vector<FileOpener> others;
    for (const auto& o : readMarker(marker)) {
        if (isMe(o))
            continue;
        all.push_back(o);
        others.push_back(o);
    }
    FileOpener me;
    me.host = thisHost();
    me.pid = thisPid();
    me.since = nowSeconds();
    all.push_back(me);
    writeMarker(marker, all);
    return others;
}

void FileOpeners::remove(const std::string& file)
{
    const std::string marker = markerPath(file);
    if (!QFileInfo::exists(QString::fromStdString(marker)))
        return;
    std::vector<FileOpener> rest;
    for (const auto& o : readMarker(marker)) {
        if (!isMe(o))
            rest.push_back(o);
    }
    writeMarker(marker, rest);
    // The last to close it takes the lock file with it -- under the lock,
    // so that a save in another process is not between its look and its
    // write; one that opened the old file sees it is no longer named.
    if (rest.empty()) {
        SaveLock lock(file);
        if (lock.lock(0)) {
            QFile::remove(QString::fromStdString(SaveLock::lockPath(file)));
            lock.unlock();
        }
    }
}

std::string FileOpeners::describe(const FileOpener& opener)
{
    const auto t = static_cast<std::time_t>(opener.since);
    char when[64] = {0};
    if (std::tm* tm = std::localtime(&t))
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
    return opener.host + " (pid " + std::to_string(opener.pid) + ") since " + when;
}
