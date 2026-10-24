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
# include <mutex>
# include <set>
# include <sstream>
# include <thread>
#endif

#ifdef _WIN32
# include <windows.h>
#else
# include <cerrno>
# include <csignal>
# include <sys/stat.h>
#endif

#include <boost/interprocess/exceptions.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/version.hpp>

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

/// The lock files a SaveLock of this process holds. An fcntl lock belongs
/// to the process, so a second one taken here would not see the first.
std::mutex heldMutex;
std::set<std::string> heldPaths;

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
    : _file(file)
    , _path(lockPath(file))
{}

SaveLock::~SaveLock()
{
    unlock();
}

bool SaveLock::tryOnce()
{
    namespace bip = boost::interprocess;
    {
        std::lock_guard<std::mutex> guard(heldMutex);
        if (!heldPaths.insert(_path).second)
            return false;   // another SaveLock here has it
    }
    _held = true;
    auto letGo = [this](bool result) {
        std::lock_guard<std::mutex> guard(heldMutex);
        heldPaths.erase(_path);
        _held = false;
        return result;
    };

    const QString path = QString::fromStdString(_path);
    DiskStamp before = DiskStamp::of(_path);
    if (!before.valid()) {
        // file_lock opens and never creates. Closing this handle before the
        // lock is taken costs nothing: none is held here yet (heldPaths).
        QFile make(path);
        if (!make.open(QIODevice::ReadWrite)) {
            // A folder that takes files: the one there is being deleted by
            // the last to close the document (Windows), so wait. Otherwise
            // no lock can be made here; the save says what it meets.
            return letGo(!QFileInfo(QFileInfo(path).absolutePath()).isWritable());
        }
        make.close();
        before = DiskStamp::of(_path);
    }
    try {
#if defined(_WIN32) && BOOST_VERSION >= 107600
        auto lock = std::make_unique<bip::file_lock>(path.toStdWString().c_str());
#else
        auto lock = std::make_unique<bip::file_lock>(path.toUtf8().constData());
#endif
        if (!lock->try_lock())
            return letGo(false);
        // Taken on the file the name still names: a last process closing
        // the document may have deleted the one this opened. The file is
        // never written, so the stamp moves only when it is made anew.
        if (!before.valid() || !DiskStamp::of(_path).sameAs(before)) {
            lock->unlock();
            return letGo(false);
        }
        _lock = std::move(lock);
    }
    catch (const bip::interprocess_exception&) {
        // Gone since, or being deleted: try again. One that is there and
        // cannot be written means no lock can be made here.
        return letGo(QFileInfo::exists(path) && !QFileInfo(path).isWritable());
    }
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
        // The lock file says nothing (on Windows it cannot even be read
        // while held): who has the file open is what there is to tell.
        holder->clear();
        bool here = false;
        {
            std::lock_guard<std::mutex> guard(heldMutex);
            here = heldPaths.count(_path) > 0;
        }
        if (here)
            *holder = "this process (pid " + std::to_string(thisPid()) + ")";
        else {
            std::string open;
            for (const auto& o : FileOpeners::others(_file)) {
                if (!open.empty())
                    open += ", ";
                open += FileOpeners::describe(o);
            }
            *holder = "another process";
            if (!open.empty())
                *holder += " (it is open in " + open + ")";
        }
    }
    return false;
}

void SaveLock::unlock()
{
    if (_lock) {
        try {
            _lock->unlock();
        }
        catch (const boost::interprocess::interprocess_exception&) {
            // closing it below lets it go all the same
        }
        _lock.reset();
    }
    if (_held) {
        std::lock_guard<std::mutex> guard(heldMutex);
        heldPaths.erase(_path);
        _held = false;
    }
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
