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

#ifndef APP_ANIMATION_WORKERS_H
#define APP_ANIMATION_WORKERS_H

// Inside App only: an Animation as a client of its document's worker pool
// (docs/Animation.md sec 4.2, docs/WorkerProcess.md sec 8), shared by
// Animation.cpp and AnimationWorkers.cpp

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Animation.h"
#include "StringHasher.h"
#include "WorkerPool.h"

namespace App
{

/// What the worker making a frame is told to write, and what it wrote
struct Animation::FrameOut
{
    long index = 0;
    std::string file;
    std::string tables;
    FrameWritten written;
    std::string error;
    bool done = false;
};

/// The frames an Animation handed to the pool, as jobs of the kind
/// "animation.frame"
struct Animation::Workers
{
    explicit Workers(Animation& owner);
    ~Workers();
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    /// The pool of the document it handed its frames to, if it stands
    WorkerPool* pool() const;
    /// The frames of `indices` not made or handed over yet, from `from` on
    void enqueue(const std::vector<long>& indices, long from);
    /// Frame `index` is wanted now: taken back if still queued, for the
    /// Animation to make, or waited for if a worker is making it
    void claim(long index);
    /// Its jobs dropped, what came and was not taken in deleted
    void cancel(const std::string& why);
    /// The frames a worker made, taken into the cache when that is safe
    void adoptArrived();
    bool running() const
    {
        return !jobs.empty();
    }
    /// The string table a worker wrote under `hash`, read once
    StringHasherRef table(const std::string& hash);
    /** What a frame's result is made from, by name (docs/WorkerProcess.md
     * sec 4, 8): what the tracks write -- through strips, in the
     * Animations they play -- what reads Time, what is touched already, and
     * every object that depends on one of them: what a frame's recompute
     * takes in. Empty when every object is recomputed for a frame, and any
     * change reaches it.
     */
    std::vector<std::string> readsNow() const;
    /// Whether an edit reaching `reached` reaches the frames handed over
    bool reaches(const std::set<DocumentObject*>& reached) const;

    Animation& owner;
    Document* doc = nullptr;  ///< whose pool the jobs are in
    std::string tag;          ///< its jobs', unique to it
    std::string tables;       ///< where the workers write the string tables
    std::map<long, WorkerPool::JobId> jobs;
    std::vector<std::string> reads;  ///< the jobs', as readsNow() said at the last enqueue
    std::map<std::string, StringHasherRef> loaded;
    std::vector<std::pair<long, std::string>> arrived;
    std::size_t resident = 0;  ///< the most a worker said it held
    long failed = 0;
    std::string reason;

private:
    void done(long index, WorkerPool::JobId id, WorkerPool::Result& result);
};

}  // namespace App

#endif  // APP_ANIMATION_WORKERS_H
