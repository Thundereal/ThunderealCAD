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

#ifndef APP_WORKER_POOL_H
#define APP_WORKER_POOL_H

// Worker processes, a facility any client uses (docs/WorkerProcess.md).
// A pool belongs to a document: its workers are console processes, each
// holding a copy of the document, that run jobs of registered kinds and
// answer with results. Control goes over a local socket of the pool's,
// in messages of a JSON header and a binary body; what a worker prints
// goes to its log and is never read.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <fastsignals/signal.h>

#include <FCGlobal.h>

struct _object;  // PyObject

namespace App
{

class Document;

class AppExport WorkerPool
{
public:
    using JobId = long;

    /// What became of a job, handed to its client once
    struct Result
    {
        enum class Status
        {
            Done,       ///< made: `output`, and `file` if it wrote one
            Failed,     ///< the handler threw, or its worker died twice over it
            Unfit,      ///< the worker cannot do it faithfully: do it in process
            Cancelled,  ///< the pool stopped (`error` says why) before it was made
        };
        JobId id = 0;
        std::string kind;
        std::string tag;
        Status status = Status::Failed;
        std::string error;
        std::string output = "{}";  ///< JSON, the handler's
        /// What the handler wrote, in the pool's directory: the client
        /// takes it (renames it, and clears this) inside `done`, or it is
        /// deleted after
        std::string file;
        double seconds = 0;  ///< the handler's time
        std::size_t resident = 0;  ///< what the worker held after it
    };

    struct Job
    {
        std::string kind;
        std::string inputs = "{}";  ///< JSON, the kind's own
        /// A Python module that registers the kind, imported by a worker
        /// before its first job of it; empty for a kind in C++
        std::string module;
        /// The client's: what cancel() and info() go by
        std::string tag;
        /// Called once, from the thread the pool runs in, unless the
        /// client cancels the job or claims it while it is queued
        std::function<void(Result&)> done;
    };

    enum class State
    {
        Unknown,   ///< no such job, or its result was handed over
        Queued,
        Running,
    };

    enum class Claim
    {
        Unknown,  ///< no such job: its result was handed over already
        Yours,    ///< it was queued and is not any more: do it yourself
        Done,     ///< it was running and its result has been handed over
        Failed,   ///< its worker died or ran past the deadline: do it yourself
    };

    struct Info
    {
        int workers = 0;   ///< alive
        int limit = 0;
        int spawned = 0;   ///< ever, this pool
        int busy = 0;      ///< running a job
        std::size_t queued = 0;
        std::size_t resident = 0;  ///< the most a worker said it held
        long done = 0;
        long failed = 0;
        std::string reason;  ///< why the pool last stopped or did not start
        std::string directory;
        std::vector<long> pids;
    };

    /// The pool of `doc`; made if `create`, else null when it has none
    static WorkerPool* of(Document& doc, bool create = true);
    /// Whether workers can be had: a build with processes, and Workers of
    /// Preferences/Workers not 0. `why` says why not
    static bool supported(std::string* why = nullptr);
    /// What this process holds in memory as the system counts it
    static std::size_t residentBytes();

    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Take the document's copy if there is none standing and start the
    /// first worker; false with reason() set when it cannot
    bool start();
    /// Whether it has a copy standing and workers may run on it
    bool started() const;
    JobId submit(Job job);
    /// The job is wanted now (docs/WorkerProcess.md sec 7): exactly once
    Claim claim(JobId id);
    State state(JobId id) const;
    /// Drop the jobs whose tag `which` takes, without telling their
    /// clients: queued ones go, a running one's result is thrown away
    std::size_t cancel(const std::function<bool(const std::string&)>& which);
    /// End every worker and drop the copy: every job is handed back
    /// Cancelled with `why`. The next start() takes a new copy
    void stop(const std::string& why);
    /// Take in what came, and with `ms` > 0 what comes for as long
    void poll(int ms = 0);
    /// Poll until no job is queued or running, or `seconds` pass (< 0:
    /// no end); true if none is
    bool wait(double seconds);
    Info info() const;
    const std::string& reason() const;
    Document& document() const;

    /// Changed: a worker came or went, a job was taken or finished
    static fastsignals::signal<void(const WorkerPool&)> signalChanged;

    // --- the worker's side

    struct Request
    {
        std::string kind;
        std::string inputs;  ///< JSON
        std::string out;     ///< a file the handler may write its result to
        Document* document = nullptr;  ///< the copy this worker holds
    };
    struct Reply
    {
        bool ok = true;
        bool unfit = false;
        std::string error;
        std::string output = "{}";  ///< JSON
    };
    using Handler = std::function<Reply(const Request&)>;
    /// A kind a worker runs in C++; a Python kind registers with
    /// freecad.worker.register()
    static bool registerKind(const std::string& kind, Handler handler);
    /// The worker's loop (freecad.worker.serve()): returns its exit code
    static int serve();

    /// FreeCAD.workerPool(doc): a handle on the pool of `doc`, new reference
    static ::_object* pyObject(Document& doc);

private:
    explicit WorkerPool(Document& doc);
    struct Private;
    std::unique_ptr<Private> d;
};

}  // namespace App

#endif  // APP_WORKER_POOL_H
