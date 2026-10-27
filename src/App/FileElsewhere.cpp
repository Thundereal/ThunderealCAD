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
# include <algorithm>
# include <atomic>
# include <chrono>
# include <cstdio>
# include <ctime>
# include <filesystem>
# include <mutex>
# include <set>
# include <sstream>
# include <thread>
# include <vector>
#endif

#ifdef _WIN32
# include <io.h>
# include <windows.h>
#else
# include <cerrno>
# include <csignal>
# include <fcntl.h>
# include <sys/stat.h>
# include <unistd.h>
#endif

#include <boost/interprocess/exceptions.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/version.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSysInfo>

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/FileInfo.h>

#include "DocumentParams.h"
#include "FileElsewhere.h"
#include "FileHistory.h"

FC_LOG_LEVEL_INIT("App", true, true)

using namespace App;

namespace
{

std::string beside(const std::string& file, const char* suffix)
{
    // Beside the file however it is spelled (sec 38.21 P): through a link,
    // or with a slash after a folder's name, every process names the same
    // lock, marker and stage.
    QFileInfo fi(QString::fromStdString(FileHistory::canonicalPath(file)));
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

/// The marker's lines, those of gone processes of this host left out --
/// and those of another host not seen for FileOpenerExpiryDays, which a
/// process there that ended without closing left (sec 39.3, D4): kept, the
/// folder was never alone again, its blob files never pruned. They go into
/// `expired` when asked.
std::vector<FileOpener> readMarker(const std::string& marker,
                                   std::vector<FileOpener>* expired = nullptr)
{
    std::vector<FileOpener> out;
    QFile in(QString::fromStdString(marker));
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    const std::string host = thisHost();
    const long days = DocumentParams::getFileOpenerExpiryDays();
    const double now = nowSeconds();
    std::istringstream lines(in.readAll().toStdString());
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        FileOpener o;
        if (!(fields >> o.host >> o.pid >> o.since))
            continue;
        // Last seen: a line written before it was kept is seen when opened.
        if (!(fields >> o.seen))
            o.seen = o.since;
        if (o.host == host && !alive(o.pid))
            continue;
        if (o.host != host && days > 0 && now - o.seen > days * 86400.0) {
            if (expired)
                expired->push_back(o);
            continue;
        }
        out.push_back(o);
    }
    return out;
}

/// Written whole and renamed into place, so a reader sees one or the other.
/// On Windows the rename fails while another process reads the marker (a
/// reader holds no lock): tried again, not given up on.
void writeMarker(const std::string& marker, const std::vector<FileOpener>& openers)
{
    const QString path = QString::fromStdString(marker);
    std::ostringstream text;
    text.precision(17);
    for (const auto& o : openers)
        text << o.host << ' ' << o.pid << ' ' << o.since << ' ' << o.seen << '\n';
    const std::string bytes = text.str();
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (openers.empty()) {
            if (QFile::remove(path) || !QFileInfo::exists(path))
                return;
            continue;
        }
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
            return;   // a folder that takes no files: nothing to tell
        out.write(bytes.data(), static_cast<qint64>(bytes.size()));
        if (out.commit())
            return;
    }
}

bool isMe(const FileOpener& o)
{
    return o.pid == thisPid() && o.host == thisHost();
}

/// The lock files a SaveLock of this process holds. An fcntl lock belongs
/// to the process, so a second one taken here would not see the first.
std::mutex heldMutex;
std::set<std::string> heldPaths;

/// The marker read and rewritten under the file's lock, or two processes
/// opening the file at once each write the marker without the other's line
/// (three opened together, two were told: sec 29.8). None is taken where
/// this process holds it already -- a save adding its line, under its own.
/// One that stays held (a long save elsewhere) is waited for a while, then
/// the marker is changed without it, as before: it only tells the user.
class MarkerLock
{
public:
    explicit MarkerLock(const std::string& file)
    {
        {
            std::lock_guard<std::mutex> guard(heldMutex);
            if (heldPaths.count(SaveLock::lockPath(file)))
                return;
        }
        _lock = std::make_unique<SaveLock>(file);
        _lock->lock(10000);
    }
    /// Held by this process, here or by its caller.
    bool held() const
    {
        return !_lock || _lock->isLocked();
    }

private:
    std::unique_ptr<SaveLock> _lock;
};

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

bool SaveLock::heldHere(const std::string& file)
{
    std::lock_guard<std::mutex> guard(heldMutex);
    return heldPaths.count(lockPath(file)) > 0;
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
    MarkerLock lock(file);
    std::vector<FileOpener> all;
    std::vector<FileOpener> others;
    std::vector<FileOpener> expired;
    FileOpener me;
    me.host = thisHost();
    me.pid = thisPid();
    me.since = nowSeconds();
    for (const auto& o : readMarker(marker, &expired)) {
        if (isMe(o)) {
            me.since = o.since;   // opened then; seen now
            continue;
        }
        all.push_back(o);
        others.push_back(o);
    }
    for (const auto& o : expired)
        Base::Console().Warning("'%s': %s was not seen for %ld days: taken as gone\n",
                                file.c_str(), describe(o).c_str(),
                                DocumentParams::getFileOpenerExpiryDays());
    me.seen = nowSeconds();
    all.push_back(me);
    writeMarker(marker, all);
    return others;
}

void FileOpeners::remove(const std::string& file)
{
    const std::string marker = markerPath(file);
    if (!QFileInfo::exists(QString::fromStdString(marker)))
        return;
    MarkerLock lock(file);
    std::vector<FileOpener> rest;
    for (const auto& o : readMarker(marker)) {
        if (!isMe(o))
            rest.push_back(o);
    }
    writeMarker(marker, rest);
    // The last to close it takes the lock file with it -- under the lock,
    // so that a save in another process is not between its look and its
    // write; one that opened the old file sees it is no longer named.
    if (rest.empty() && lock.held())
        QFile::remove(QString::fromStdString(SaveLock::lockPath(file)));
}

std::string FileOpeners::describe(const FileOpener& opener)
{
    const auto t = static_cast<std::time_t>(opener.since);
    char when[64] = {0};
    if (std::tm* tm = std::localtime(&t))
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
    return opener.host + " (pid " + std::to_string(opener.pid) + ") since " + when;
}

namespace
{

/// The stage's record: written last, through a rename, so that a stage that
/// has one holds a whole save.
const char* const commitName = ".~commit";
const char* const commitHeader = "FreeCAD folder save 1";

std::filesystem::path fsPath(const QString& path)
{
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

bool sameBytes(const QString& a, const QString& b)
{
    QFileInfo fa(a);
    QFileInfo fb(b);
    if (!fb.exists() || fa.size() != fb.size())
        return false;
    QFile ra(a);
    QFile rb(b);
    if (!ra.open(QIODevice::ReadOnly) || !rb.open(QIODevice::ReadOnly))
        return false;
    constexpr qint64 chunk = 1 << 16;
    while (!ra.atEnd()) {
        if (ra.read(chunk) != rb.read(chunk))
            return false;
    }
    return true;
}

/// Flush `path`'s bytes to the disk (docs/TransactionLogJournal.md sec 38.16, Q5).
/// Opened for writing: Windows flushes only a handle that may write.
bool syncFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
#ifdef _WIN32
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(f.handle()))) != 0;
#else
    return ::fsync(f.handle()) == 0;
#endif
}

/// syncFile() each of `paths`, several at a time when they are many: a
/// journal takes the flushes that come together in one commit, which made
/// 600 files 55 ms on 64 threads against 270 ms one by one (sec 38.16).
/// The first that failed, or empty.
QString syncFiles(const QStringList& paths)
{
    const int count = static_cast<int>(paths.size());
    const int threads = std::min(32, count / 4);
    std::atomic<int> next {0};
    std::atomic<int> failed {-1};
    auto work = [&]() {
        for (int i = next++; i < count; i = next++) {
            if (!syncFile(paths[i])) {
                int none = -1;
                failed.compare_exchange_strong(none, i);
            }
        }
    };
    if (threads < 2) {
        work();
    }
    else {
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (int t = 0; t < threads; ++t)
            pool.emplace_back(work);
        for (auto& t : pool)
            t.join();
    }
    return failed < 0 ? QString() : paths[failed];
}

/// Make what `dir` lists durable, where that is a step of its own; on
/// Windows a file's flush covers its entry.
void syncDir(const QString& dir)
{
#ifndef _WIN32
    const int fd = ::open(QFile::encodeName(dir).constData(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)dir;
#endif
}

/// The staged files, relative to the stage, the record left out.
QStringList stagedFiles(const QString& stage)
{
    QStringList files;
    QDir root(stage);
    QDirIterator it(stage, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString rel = root.relativeFilePath(it.next());
        if (rel != QLatin1String(commitName))
            files.push_back(rel);
    }
    return files;
}

/// A rename over a file another program holds open fails on Windows (a sync
/// client, a scanner): retried a while, as the open marker's is.
bool moveOver(const QString& from, const QString& to, std::string& why)
{
    for (int i = 0; i < 50; ++i) {
        std::error_code ec;
        std::filesystem::rename(fsPath(from), fsPath(to), ec);
        if (!ec)
            return true;
        why = ec.message();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/// Move a recorded stage in. False when it has no record. With `sync`, the
/// folder's entries are made durable before the record goes.
bool rollForward(const QString& folder, const QString& stage, bool sync)
{
    QFile record(QDir(stage).filePath(QLatin1String(commitName)));
    if (!record.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QStringList removals;
    bool header = true;
    while (!record.atEnd()) {
        QString line = QString::fromUtf8(record.readLine());
        if (line.endsWith(QLatin1Char('\n')))
            line.chop(1);
        if (header) {
            header = false;
            if (line != QLatin1String(commitHeader))
                return false;
            continue;
        }
        if (line.startsWith(QLatin1String("remove "))) {
            // A name in the folder, as commit() writes it (sec 38.21 F4): a
            // record naming anything outside it was not written by a save,
            // and is taken for no record, so the stage is thrown away.
            const QString rel = line.mid(7);
            const QString clean = QDir::cleanPath(rel);
            if (rel.isEmpty() || QDir::isAbsolutePath(rel) || clean == QLatin1String("..")
                    || clean.startsWith(QLatin1String("../")) || rel.contains(QLatin1Char('\\'))
                    || rel.contains(QLatin1Char(':'))) {
                FC_WARN("the save record beside '" << folder.toStdString()
                        << "' names a file outside it, '" << rel.toStdString()
                        << "'; the stage is thrown away");
                return false;
            }
            removals.push_back(rel);
        }
    }
    record.close();

    // Document.xml last: what the folder is reads it first, so a reader that
    // stamped it before reading sees, by the stamp after, that a save came
    // in while it read (sec 38).
    QStringList files = stagedFiles(stage);
    const QString docXml = QStringLiteral("Document.xml");
    if (files.removeAll(docXml))
        files.push_back(docXml);
    QDir target(folder);
    std::set<QString> moved;
    for (const QString& rel : files) {
        const QString to = target.filePath(rel);
        moved.insert(QFileInfo(to).absolutePath());
        target.mkpath(QFileInfo(to).absolutePath());
        std::string why;
        if (!moveOver(QDir(stage).filePath(rel), to, why)) {
            THROWM(Base::FileException,
                   ("the save is whole beside the folder but '" + rel.toStdString()
                    + "' cannot be moved in (" + why
                    + "); the next open or save of the folder finishes it")
                       .c_str(),
                   Base::FileInfo(folder.toStdString()));
        }
    }
    if (sync) {
        for (const QString& dir : moved)
            syncDir(dir);
    }
    for (const QString& rel : removals)
        QFile::remove(target.filePath(rel));
    record.remove();
    QDir(stage).removeRecursively();
    return true;
}

}   // namespace

std::string FolderSave::stagePath(const std::string& folder)
{
    QFileInfo fi(QString::fromStdString(folder));
    const QString canonical = fi.canonicalFilePath();
    return beside(canonical.isEmpty() ? folder : canonical.toStdString(), ".save");
}

bool FolderSave::pending(const std::string& folder)
{
    return QFileInfo::exists(QString::fromStdString(stagePath(folder)));
}

int FolderSave::recover(const std::string& folder)
{
    const QString stage = QString::fromStdString(stagePath(folder));
    if (!QFileInfo::exists(stage))
        return 0;
    QFileInfo fi(QString::fromStdString(folder));
    const QString canonical = fi.canonicalFilePath();
    if (rollForward(canonical.isEmpty() ? fi.absoluteFilePath() : canonical, stage,
                    App::DocumentParams::getFolderSaveSync()))
        return 1;
    QDir(stage).removeRecursively();
    return -1;
}

std::string FolderSave::freshStage(const std::string& folder)
{
    recover(folder);
    const std::string stage = stagePath(folder);
    if (!QDir().mkpath(QString::fromStdString(stage))) {
        // A folder on a medium that takes no files (sec 38.9): it opens and
        // is edited as any other, and is saved elsewhere.
        THROWM(Base::FileException, "cannot make the stage to save the folder in"
               " (read-only?); nothing was written",
               Base::FileInfo(stage));
    }
    return stage;
}

void FolderSave::commit(const std::string& folder, const std::vector<std::string>& removals)
{
    const QString stage = QString::fromStdString(stagePath(folder));
    QFileInfo fi(QString::fromStdString(folder));
    const QString canonical = fi.canonicalFilePath();
    const QString target = canonical.isEmpty() ? fi.absoluteFilePath() : canonical;

    // What the folder holds already stays as it is: not moved, not touched,
    // its time unchanged for whatever watches the folder.
    for (const QString& rel : stagedFiles(stage)) {
        const QString staged = QDir(stage).filePath(rel);
        if (sameBytes(staged, QDir(target).filePath(rel)))
            QFile::remove(staged);
    }

    // Q5 (sec 38.16): what is moved in is on the disk before the record
    // says the stage is whole, so that a power loss leaves either the
    // previous save or this one. A file that cannot be flushed fails the
    // save with the folder as it was.
    const bool sync = App::DocumentParams::getFolderSaveSync();
    if (sync) {
        std::set<QString> dirs {stage};
        QStringList staged;
        for (const QString& rel : stagedFiles(stage)) {
            staged.push_back(QDir(stage).filePath(rel));
            dirs.insert(QFileInfo(staged.back()).absolutePath());
        }
        const QString failed = syncFiles(staged);
        if (!failed.isEmpty()) {
            THROWM(Base::FileException,
                   ("cannot flush '" + QDir(stage).relativeFilePath(failed).toStdString()
                    + "' of the folder's save to the disk; nothing was moved in")
                       .c_str(),
                   Base::FileInfo(folder));
        }
        for (const QString& dir : dirs)
            syncDir(dir);
    }

    QSaveFile record(QDir(stage).filePath(QLatin1String(commitName)));
    if (!record.open(QIODevice::WriteOnly | QIODevice::Text)) {
        THROWM(Base::FileException, "cannot write the record of the folder's save",
               Base::FileInfo(folder));
    }
    QByteArray bytes(commitHeader);
    bytes += '\n';
    for (const auto& rel : removals) {
        bytes += "remove ";
        bytes += QByteArray::fromStdString(rel);
        bytes += '\n';
    }
    if (record.write(bytes) != bytes.size() || !record.commit()) {
        THROWM(Base::FileException, "cannot write the record of the folder's save",
               Base::FileInfo(folder));
    }
    // The record is in, so the save goes in below whatever this answers.
    if (sync) {
        syncFile(QDir(stage).filePath(QLatin1String(commitName)));
        syncDir(stage);
    }
    rollForward(target, stage, sync);
}
