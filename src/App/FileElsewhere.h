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

#ifndef APP_FILE_ELSEWHERE_H
#define APP_FILE_ELSEWHERE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <FCGlobal.h>

namespace boost::interprocess
{
class file_lock;
}

namespace App
{

/** What a file on disk is, as one stat says it (docs/TransactionLogJournal.md sec
 * 29.8): enough to tell a file nobody else wrote since from one somebody
 * did, at the cost of a single system call. A write by rename -- what a save
 * is -- changes the inode; a write in place changes the size or the time.
 */
struct AppExport DiskStamp
{
    std::string path;
    int64_t size {-1};
    int64_t mtime {0};   ///< nanoseconds
    uint64_t inode {0};

    bool valid() const { return size >= 0; }
    bool sameAs(const DiskStamp& other) const
    {
        return size == other.size && mtime == other.mtime && inode == other.inode;
    }
    /// The file at `path` as it is now; not valid() when there is none. A
    /// project directory is stamped by its Document.xml.
    static DiskStamp of(const std::string& path);
};

/** The lock a save holds beside the file it writes, from the look at the
 * file through the pull to the write (sec 29.7 Q5): boost's file_lock on
 * `.~<name>.lock`, which is fcntl on POSIX and LockFileEx on Windows. A
 * process that finds it held waits. The system lets it go when the process
 * ends, however it ends, so none is ever left stale; the file stays while
 * the document is open, and goes with the marker of the last process that
 * has it open (FileOpeners::remove). An fcntl lock is the process's, not the
 * handle's, so the lock is also kept among the paths this process holds: a
 * second SaveLock here on the same file waits as one elsewhere would.
 */
class AppExport SaveLock
{
public:
    explicit SaveLock(const std::string& file);
    ~SaveLock();
    SaveLock(const SaveLock&) = delete;
    SaveLock& operator=(const SaveLock&) = delete;

    /// Take it, waiting up to `timeoutMs`. False, with `holder` saying who
    /// has it, when it stays held; true also where no lock can be made at
    /// all (a folder that cannot be written), which the save meets itself.
    bool lock(int timeoutMs, std::string* holder = nullptr);
    void unlock();
    bool isLocked() const;

    static std::string lockPath(const std::string& file);
    /// A SaveLock of this process holds the lock of `file` now.
    static bool heldHere(const std::string& file);

private:
    /// One try: true when taken, or when no lock can be made at all.
    bool tryOnce();

    std::string _file;
    std::string _path;
    std::unique_ptr<boost::interprocess::file_lock> _lock;
    bool _held {false};   ///< among the paths this process holds
    bool _locked {false};
};

/// A process that has a file open (sec 29.7 Q5): one line of the marker.
struct AppExport FileOpener
{
    std::string host;
    int64_t pid {0};
    double since {0};   ///< seconds since the epoch
};

/** The marker beside a file that says which processes have it open,
 * `.~<name>.open`, one line per process: written at open and at a save to a
 * new name, the line taken out at close and the file with the last. Read
 * only to tell the user; it blocks nothing. A line of a process of this
 * host that is gone is dropped when the marker is read. Each change reads
 * and rewrites the marker under the file's SaveLock (none taken where this
 * process holds it already: a save adding its line), so that two processes
 * opening the file at once do not write over each other's line.
 */
namespace FileOpeners
{
AppExport std::string markerPath(const std::string& file);
/// The other processes that have `file` open.
AppExport std::vector<FileOpener> others(const std::string& file);
/// This process has `file` open; the others it found there.
AppExport std::vector<FileOpener> add(const std::string& file);
/// This process no longer has `file` open.
AppExport void remove(const std::string& file);
/// "host (pid N) since <time>", for a message.
AppExport std::string describe(const FileOpener& opener);
}   // namespace FileOpeners

/** A document saved as a folder goes in whole (docs/TransactionLogJournal.md sec
 * 38). The save writes into a stage beside the folder, `.~<name>.save`,
 * laid out as the folder is. Once every entry is closed, commit() drops the
 * staged files whose bytes the folder has already, writes the stage's
 * record (what to delete), and only then moves the rest in, Document.xml
 * last. A save cut short before the record leaves a stage that is thrown
 * away; one cut short after it is moved in by the next open or save of the
 * folder (recover()). All of it under the folder's SaveLock.
 */
namespace FolderSave
{
/// The stage beside `folder` (its canonical path when it has one).
AppExport std::string stagePath(const std::string& folder);
/// A stage left by a save that did not finish is there.
AppExport bool pending(const std::string& folder);
/// An empty stage for a save of `folder`, after recover().
AppExport std::string freshStage(const std::string& folder);
/// Move the staged save in; `removals` are files of the folder, relative to
/// it, that the save no longer has. Throws Base::FileException when the
/// record cannot be written (the folder is then as it was) or a file cannot
/// be moved in (the next open or save finishes it).
AppExport void commit(const std::string& folder, const std::vector<std::string>& removals);
/// What a save cut short left: 1 a recorded stage moved in, -1 a stage with
/// no record deleted, 0 none there.
AppExport int recover(const std::string& folder);
}   // namespace FolderSave

}   // namespace App

#endif   // APP_FILE_ELSEWHERE_H
