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

// Inside App only: the worker processes of an Animation (docs/Animation.md
// sec 4.2), shared by Animation.cpp and AnimationWorkers.cpp

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QObject>

#include "Animation.h"
#include "StringHasher.h"

class QProcess;

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

struct Animation::Workers
{
    explicit Workers(Animation& owner);
    ~Workers();
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    struct Proc
    {
        std::unique_ptr<QProcess> process;
        std::string buffer;     ///< output not read to a line's end yet
        long frame = -1;        ///< the frame it is making, -1 for none
        bool ready = false;     ///< the document is open
        bool quitting = false;  ///< told there is no more
        long made = 0;          ///< frames it made
        std::size_t resident = 0;  ///< what it holds, as it last said
    };

    /// Start the first process; false with `reason` set when it cannot be
    bool start();
    /// Kill every process; what they made and the cache took stays
    void stop(const std::string& why);
    /// The frames of `indices` not made yet, queued from `from` on
    void enqueue(const std::vector<long>& indices, long from);
    /// Frame `index` is wanted now: off the queue if it is still there,
    /// for Animation to make, or waited for if a worker is making it
    void claim(long index);
    /// Take in what came; with `ms` > 0 wait as long for more
    void poll(int ms);
    /// The frames a worker made, taken into the cache when that is safe
    void adoptArrived();
    /// Whether a worker is making `index` or it is queued
    bool owns(long index) const;
    int alive() const;
    std::string frameFile(long index) const;
    /// The string table a worker wrote under `hash`, read once
    StringHasherRef table(const std::string& hash);

    Animation& owner;
    QObject context;  ///< the connections to the processes
    std::string dir;      ///< the job: the document's copy, the settings, the logs
    std::string frames;   ///< where the frames go: the cache's directory
    std::string tables;   ///< where the string tables go
    std::string job;      ///< the job file the processes read
    std::string program;
    std::deque<long> queue;
    std::vector<std::unique_ptr<Proc>> procs;
    std::vector<std::unique_ptr<Proc>> gone;  ///< stopped inside one of their signals
    std::map<std::string, StringHasherRef> loaded;
    std::vector<std::pair<long, std::string>> arrived;
    std::size_t resident = 0;  ///< the most a worker said it held
    int limit = 1;
    int spawned = 0;
    int failedInRow = 0;
    long failed = 0;
    std::string reason;
    bool running = false;

    /// Remove the job's directory, once no process is left in it
    void cleanup();
    /// Delete the processes stopped inside one of their own signals
    void bury();

private:
    bool spawn();
    void read(Proc& proc);
    void line(Proc& proc, const std::string& text);
    void finished(Proc& proc);
    void dispatch(Proc& proc);
    void grow();
};

}  // namespace App

#endif  // APP_ANIMATION_WORKERS_H
