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
// in messages of a JSON header and a binary body. What a worker prints
// goes to its log; what it says through the Console comes here too
// (docs/WorkerProcess.md sec 27).

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <fastsignals/signal.h>

#include <FCGlobal.h>

struct _object;  // PyObject

namespace Base
{
enum class LogStyle;
enum class IntendedRecipient;
enum class ContentType;
}  // namespace Base

namespace App
{

class Document;
class DocumentObject;

class AppExport WorkerPool
{
public:
    using JobId = long;

    /// What a worker said through the Console (docs/WorkerProcess.md sec 27)
    struct Message
    {
        Base::LogStyle level {};
        Base::IntendedRecipient recipient {};
        Base::ContentType content {};
        std::string notifier;
        std::string text;
        int worker = 0;  ///< its number in the pool
    };
    /// Said here, the worker named as its notifier ("worker 2", or
    /// "Pad (worker 2)")
    static void show(const Message& message);

    /// What became of a job, handed to its client once
    struct Result
    {
        enum class Status
        {
            Done,       ///< made: `output`, and `file` if it wrote one
            Failed,     ///< the handler threw, or its worker died twice over it
            Unfit,      ///< the worker cannot do it faithfully: do it in process
            /// The pool stopped, or the design changed under it, before it
            /// was made (`error` says why)
            Cancelled,
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
        /// Failed because its worker died over it, or was ended past the
        /// deadline: not the handler's own failure
        bool died = false;
        /// Where in the job each of the workers that died over it was, as
        /// the handler said (Request::at), first to last: empty where it
        /// had said nothing (docs/WorkerProcess.md sec 34)
        std::vector<std::string> diedAt;
        /// How the last of them ended: "crashed (signal 11)", "exited
        /// with code 3", ...
        std::string how;
        /// For a crash, what the last of them said of it (sec 35): the
        /// signal, the frames of the thread that crashed, Python's
        /// tracebacks; empty for a death that said nothing
        std::string stack;
        /// The crash log it is in, with the frames' lines where they could
        /// be found (Base::CrashLog), empty for none
        std::string crashLog;
        /// What its worker said while it made it (sec 27): said here after
        /// `done` for one Done or Failed, unless the client takes them --
        /// clears this, or what it says itself -- inside `done`. Those of
        /// one Unfit or Cancelled are not: it is done here, saying its own,
        /// or not wanted
        std::vector<Message> messages;
        double seconds = 0;  ///< the handler's time
        /// Seconds, on this process's clock: from its submit to its being
        /// handed to the worker that made it, and from then to its result
        /// read here (docs/WorkerProcess.md sec 53)
        double queued = 0;
        double returned = 0;
        /// Of `queued`, what that worker said its open or move of its copy
        /// took, when it was told one after the submit
        double setup = 0;
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
        /// The objects, by name, its result is made from: a change of one
        /// of them, or of anything they depend on, drops it -- handed back
        /// Cancelled -- and a change of anything else leaves it to run on
        /// the copy it was queued for (docs/WorkerProcess.md sec 4). Empty:
        /// any change of the design drops it
        std::vector<std::string> reads;
        /// Made on the copy standing when it was submitted, whatever changes
        /// after (docs/WorkerProcess.md sec 24): what an export writes is the
        /// design as it was when asked for. No change of the design drops it,
        /// `reads` is not looked at, and the copy is kept for it while it
        /// runs; a newer copy taken before a worker could be handed it hands
        /// it back Cancelled
        bool pinned = false;
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
        int adopted = 0;   ///< of them, taken standing (docs/WorkerProcess.md sec 69)
        /// Workers idle past IdleSeconds kept with their copy open for the
        /// next job, now (Workers/KeepOpen, sec 70)
        int keptOpen = 0;
        long keptOpenTimes = 0;  ///< times one was kept so
        long letGo = 0;          ///< of them, let go of for the memory
        int busy = 0;      ///< running a job
        std::size_t queued = 0;
        std::size_t resident = 0;  ///< the most a worker said it held
        long done = 0;
        long failed = 0;
        std::string reason;  ///< why the pool last stopped or did not start
        /// The pool's, for as long as it lives: the workers' logs, the
        /// files of their results, the blobs put out, and the copies
        std::string directory;
        std::string copy;  ///< the directory of the copy standing, empty for none
        long long version = 0;  ///< the log's version the copy is, 0 for a saved one
        /// The design changed since the copy was taken: the next start()
        /// or submit() takes another and moves the workers to it
        bool stale = false;
        long moves = 0;    ///< workers moved along the log to a newer copy
        long reopens = 0;  ///< workers that opened a newer copy whole
        /// Newer copies made while the document was quiet, before a job
        /// asked for one (Workers/MoveWhenIdleSeconds, docs/WorkerProcess.md
        /// sec 57)
        long idleCopies = 0;
        /// Values workers kept as they held them, moving across the rows
        /// their own result was put in as (sec 29)
        long kept = 0;
        /// Workers ended because the job they were making was dropped, and
        /// it had run longer than a worker takes to start (sec 26)
        long ended = 0;
        /// What a worker took from its start to its copy open, on average;
        /// 0 before one has
        double startSeconds = 0;
        /// How long a dropped job must have run for its worker to be ended:
        /// startSeconds, and at least EndDroppedAfter of Preferences/Workers
        double endAfter = 0;
        long leases = 0;   ///< ranges of string ids leased to workers (sec 15)
        long leasesAsked = 0;  ///< of them, asked for by a worker run out in a job
        bool readStore = false;  ///< the workers read it from this process's store
        std::string blobs;  ///< where the blobs they read are put out
        std::vector<long> pids;
    };

    /// The pool of `doc`; made if `create`, else null when it has none
    static WorkerPool* of(Document& doc, bool create = true);
    /// Whether workers can be had: a build with processes, and Workers of
    /// Preferences/Workers not 0. `why` says why not
    static bool supported(std::string* why = nullptr);
    /// What this process holds in memory as the system counts it
    static std::size_t residentBytes();

    /// The worker started ahead of need for any pool to take
    /// (Preferences/Workers: Standby, docs/WorkerProcess.md sec 69)
    struct StandbyInfo
    {
        long pid = 0;            ///< 0 for none standing
        bool connected = false;  ///< it said who it is
        long started = 0;
        long handedOver = 0;     ///< taken by a pool
        long retired = 0;        ///< ended standing: settings it read changed, or exit
    };
    static StandbyInfo standby();
    /// The one standing ended (at exit)
    static void stopStandby();

    /// The machine's memory as the pools judge it for keeping a worker's
    /// copy open (Preferences/Workers: KeepOpen, KeepOpenMemoryUse,
    /// KeepOpenPressure, docs/WorkerProcess.md sec 70)
    struct MemoryInfo
    {
        double use = -1;     ///< percent in use, -1 not known
        int pressure = 0;    ///< the system's word: 0 none, 1 low, 2 critical
        bool keepOpen = false;  ///< a worker may be kept open now
        std::string why;        ///< why not
    };
    static MemoryInfo memory();

    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Take the document's copy if there is none standing, or another if
    /// the design changed since (the workers moved to it, sec 4), and
    /// start the first worker; false with reason() set when it cannot
    bool start();
    /// Whether it has a copy standing and workers may run on it
    bool started() const;
    JobId submit(Job job);
    /// The job is wanted now (docs/WorkerProcess.md sec 7): exactly once.
    /// With Failed for its worker's death, `death` if given is told of it
    /// as a result would be (died, diedAt, how, stack, crashLog): the
    /// client that does the job itself is the one to know what it died in
    Claim claim(JobId id, Result* death = nullptr);
    State state(JobId id) const;
    /// Drop the jobs whose tag `which` takes, without telling their
    /// clients: queued ones go, a running one's result is thrown away --
    /// and its worker is ended if the job has run past Info::endAfter
    /// (docs/WorkerProcess.md sec 26)
    std::size_t cancel(const std::function<bool(const std::string&)>& which);
    /// End every worker and drop the copy: every job is handed back
    /// Cancelled with `why`. The next start() takes a new copy
    void stop(const std::string& why);
    /** The design changed (docs/WorkerProcess.md sec 4): the copy is stale,
     * and the jobs `obj` reaches -- those that read it or anything that
     * depends on it, and those that say nothing of what they read -- are
     * handed back Cancelled (a running one's worker ended as by cancel()).
     * Without an object, every job is. The pool
     * sees the document's own edits itself; this is for a change it does
     * not, an Animation's tracks.
     */
    void designChanged(const DocumentObject* obj);
    /** The result of `job` was put in here as the log's rows `fromSeq` to
     * `toSeq`, the values of `objects` the worker's own (docs/WorkerProcess.md
     * sec 29): the worker that made it, moving across those rows next,
     * keeps those values as it holds them rather than read them back.
     */
    void putIn(JobId job, long long fromSeq, long long toSeq,
               const std::vector<std::string>& objects);
    /** The document's transient directory `dir` is about to be renamed
     * (docs/WorkerProcess.md sec 45): the workers reading the store in it
     * are ended -- Windows renames no directory a file under which is open
     * -- those with nothing to make waited for, up to `ms`; one making a job
     * ends after it. The copy is stale, and the next one is read from where
     * the store went. The number still running
     */
    std::size_t releaseDirectory(const std::string& dir, int ms = 5000);
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
        /// Tell the pool where in the job the handler is (an object's
        /// name; empty for nowhere in particular), on the worker's own
        /// thread: what the job is put down to should the worker die
        /// (Result::diedAt). Cheap, and said only when it changes
        std::function<void(const std::string& where)> at;
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
