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

// Worker processes (docs/WorkerProcess.md sec 2, 7). The pool listens on a
// local socket of its own; each worker is a ThunderealCmd running
// freecad.worker.serve(), which connects to it and says who it is. Every
// message either way is
//
//   u32 header length, u32 body length (little endian), the header -- a
//   JSON object, its "type" saying what it is -- and the body
//
// The pool says:   setup {document, proxies, ids,      open the copy, or
//                         store, version, blobs,       move to a newer one
//                         from, fromSeq, toSeq}        (sec 4)
//                  job {job, kind, inputs, module, out}
//                  ids {first, count}                  the answer to ids
//                  quit
// A worker says:   hello {token, pid}
//                  ready {resident, moved, seconds} | unfit {error}
//                  ids                                 more string ids, in
//                                                      a job (sec 15)
//                  result {job, ok, unfit, error, output, seconds, resident}
//
// What a worker prints goes to its log in the pool's directory and is
// never read.

#include "PreCompiled.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <mutex>

#include <nlohmann/json.hpp>

#if defined(__APPLE__)
# include <mach/mach.h>
# include <sys/sysctl.h>
#elif defined(_WIN32)
# include <windows.h>
# include <psapi.h>
#else
# include <unistd.h>
#endif

#include <CXX/Extensions.hxx>

#include <Base/Console.h>
#include <Base/CrashLog.h>
#include <Base/FileInfo.h>
#include <Base/Stream.h>
#include <Base/Interpreter.h>
#include <Base/Parameter.h>
#include <Base/PyWrapParseTupleAndKeywords.h>
#include <Base/Tools.h>

#include "Animation.h"
#include "Application.h"
#include "Document.h"
#include "DocumentObject.h"
#include "DocumentObjectPy.h"
#include "ExpressionEvaluator.h"
#include "ExpressionSecurityRuntime.h"
#include "FileBlobManager.h"
#include "FileHistory.h"
#include "PropertyPythonObject.h"
#include "ShuttingDown.h"
#include "WorkerCrash.h"
#include "WorkerRecompute.h"
#include "StringHasher.h"
#include "LogMoveKeep.h"
#include "TransactionLog.h"
#include "WorkerPool.h"
#include "WriteScope.h"

FC_LOG_LEVEL_INIT("Workers", true, true)

using namespace App;
using json = nlohmann::json;

fastsignals::signal<void(const WorkerPool&)> WorkerPool::signalChanged;

namespace
{

constexpr const char* ServerVariable = "THUNDEREAL_WORKER_SERVER";
constexpr const char* TokenVariable = "THUNDEREAL_WORKER_TOKEN";

ParameterGrp::handle params()
{
    return GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Workers");
}

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::size_t totalMemory()
{
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<std::size_t>(status.ullTotalPhys);
    }
    return 0;
#elif defined(__APPLE__)
    int64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0 && bytes > 0) {
        return static_cast<std::size_t>(bytes);
    }
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? static_cast<std::size_t>(pages) * static_cast<std::size_t>(size)
                                 : 0;
#endif
}

/// What can be had without the machine swapping, or 0 when not known
std::size_t availableMemory()
{
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<std::size_t>(status.ullAvailPhys);
    }
    return 0;
#elif defined(__APPLE__)
    // Free, and what the kernel gives up without writing it out first
    mach_port_t host = mach_host_self();
    vm_size_t page = 0;
    vm_statistics64_data_t vm {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_page_size(host, &page) != KERN_SUCCESS
        || host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count)
            != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::size_t>(vm.free_count + vm.inactive_count + vm.purgeable_count)
        * static_cast<std::size_t>(page);
#else
    if (std::FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[128];
        std::size_t kb = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (std::sscanf(line, "MemAvailable: %zu kB", &kb) == 1) {
                break;
            }
            kb = 0;
        }
        std::fclose(f);
        return kb * 1024;
    }
    return 0;
#endif
}

/// The console program beside this one: ThunderealCmd, or what Command of
/// Preferences/Workers names
std::string workerProgram()
{
    std::string set = params()->GetASCII("Command", "");
    if (!set.empty()) {
        return set;
    }
    std::string exe = Application::Config()["ExeName"];
    if (exe.size() > 3 && exe.compare(exe.size() - 3, 3, "Cmd") == 0) {
        exe.resize(exe.size() - 3);
    }
    if (exe.empty()) {
        exe = "Thundereal";
    }
    std::string home = Application::getHomePath();
    if (!home.empty() && home.back() != '/' && home.back() != '\\') {
        home += '/';
    }
    for (const char* dir : {"bin/", "MacOS/", ""}) {
        for (const char* name : {"Cmd", "Cmd.exe"}) {
            std::string path = home + dir + exe + name;
            Base::FileInfo info(path);
            if (info.exists() && info.isFile()) {
                return path;
            }
        }
    }
    return {};
}

int workerLimit()
{
    long setting = params()->GetInt("Workers", -1);
    if (setting < 0) {
        return std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 8);
    }
    return static_cast<int>(std::min<long>(setting, 64));
}

/// Whether nothing but this pool's own waits takes in its connections:
/// with an event loop running, its server takes them itself, and taking one
/// here first leaves the server's notifier to block in accept() for one
/// that is not there
bool noEventLoop()
{
    return QThread::currentThread()->loopLevel() == 0;
}

/// A console program has no QCoreApplication, which a socket wants for
/// its notifiers
void ensureCore()
{
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "ThunderealCmd";
        static char* argv[] = {name, nullptr};
        new QCoreApplication(argc, argv);  // NOLINT: lives as long as the process
    }
}

void put32(QByteArray& out, std::uint32_t v)
{
    const char bytes[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
                           static_cast<char>((v >> 16) & 0xff),
                           static_cast<char>((v >> 24) & 0xff)};
    out.append(bytes, 4);
}

std::uint32_t get32(const char* p)
{
    auto b = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8)
        | (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

QByteArray message(const json& header, const std::string& body = std::string())
{
    const std::string text = header.dump();
    QByteArray out;
    out.reserve(static_cast<qsizetype>(8 + text.size() + body.size()));
    put32(out, static_cast<std::uint32_t>(text.size()));
    put32(out, static_cast<std::uint32_t>(body.size()));
    out.append(text.data(), static_cast<qsizetype>(text.size()));
    out.append(body.data(), static_cast<qsizetype>(body.size()));
    return out;
}

/// One whole message off the front of `buffer`: 1 taken, 0 not all there
/// yet, -1 not a message
int take(std::string& buffer, json& header, std::string& body)
{
    if (buffer.size() < 8) {
        return 0;
    }
    const std::size_t head = get32(buffer.data());
    const std::size_t size = get32(buffer.data() + 4);
    if (buffer.size() < 8 + head + size) {
        return 0;
    }
    header = json::parse(buffer.begin() + 8, buffer.begin() + 8 + static_cast<long>(head),
                         nullptr, false);
    body.assign(buffer, 8 + head, size);
    buffer.erase(0, 8 + head + size);
    return header.is_object() ? 1 : -1;
}

/// Out now: with an event loop a socket writes when the loop next runs,
/// and a job waited for inside an evaluation would wait for a worker
/// never told it
bool send(QLocalSocket& socket, const json& header)
{
    if (socket.state() != QLocalSocket::ConnectedState) {
        return false;
    }
    QByteArray bytes = message(header);
    if (socket.write(bytes) != bytes.size()) {
        return false;
    }
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(2000)) {
            return false;
        }
    }
    return true;
}

/// What a worker says through the Console, sent to its pool as it is said
/// (docs/WorkerProcess.md sec 27); what it prints still goes to its log.
/// Said on another thread, it waits for the next said on the worker's own,
/// or for the job's result: the socket is that thread's
class Relay: public Base::ILogger
{
public:
    explicit Relay(QLocalSocket& socket)
        : socket(socket)
        , main(std::this_thread::get_id())
    {
        bLog = false;  // unless the pool asks for them
    }
    void SendLog(const std::string& notifier,
                 const std::string& msg,
                 Base::LogStyle level,
                 Base::IntendedRecipient recipient,
                 Base::ContentType content) override
    {
        json m {{"type", "log"},
                {"level", static_cast<int>(level)},
                {"recipient", static_cast<int>(recipient)},
                {"content", static_cast<int>(content)},
                {"notifier", notifier},
                {"text", msg}};
        if (std::this_thread::get_id() != main) {
            std::lock_guard<std::mutex> lock(mutex);
            if (queued.size() < 1000) {
                queued.push_back(std::move(m));
            }
            return;
        }
        if (sending) {
            return;  // what sending says of itself
        }
        sending = true;
        flushQueued();
        send(socket, m);
        sending = false;
    }
    /// What other threads said: on the worker's own thread
    void flush()
    {
        if (!sending) {
            sending = true;
            flushQueued();
            sending = false;
        }
    }
    const char* Name() override
    {
        return "WorkerRelay";
    }

private:
    void flushQueued()
    {
        std::vector<json> list;
        {
            std::lock_guard<std::mutex> lock(mutex);
            list.swap(queued);
        }
        for (auto& m : list) {
            send(socket, m);
        }
    }
    QLocalSocket& socket;
    const std::thread::id main;
    std::mutex mutex;
    std::vector<json> queued;
    bool sending = false;
};

template<Base::LogStyle S, Base::IntendedRecipient R>
void showAs(Base::ContentType content, const std::string& notifier, const std::string& text)
{
    switch (content) {
        case Base::ContentType::Translated:
            Base::Console().sendMsg<S, R, Base::ContentType::Translated>(notifier, text);
            break;
        case Base::ContentType::Untranslatable:
            Base::Console().sendMsg<S, R, Base::ContentType::Untranslatable>(notifier, text);
            break;
        default:
            Base::Console().sendMsg<S, R, Base::ContentType::Untranslated>(notifier, text);
            break;
    }
}

template<Base::LogStyle S>
void showAs(const WorkerPool::Message& m, const std::string& notifier)
{
    switch (m.recipient) {
        case Base::IntendedRecipient::User:
            showAs<S, Base::IntendedRecipient::User>(m.content, notifier, m.text);
            break;
        case Base::IntendedRecipient::Developer:
            showAs<S, Base::IntendedRecipient::Developer>(m.content, notifier, m.text);
            break;
        default:
            showAs<S, Base::IntendedRecipient::All>(m.content, notifier, m.text);
            break;
    }
}

/// A message as a worker sent it; false for what is not one
bool readMessage(const json& header, int worker, WorkerPool::Message& m)
{
    const int level = header.value("level", -1);
    const int recipient = header.value("recipient", 0);
    const int content = header.value("content", 0);
    if (level < static_cast<int>(Base::LogStyle::Warning)
        || level > static_cast<int>(Base::LogStyle::Notification) || recipient < 0
        || recipient > static_cast<int>(Base::IntendedRecipient::Developer) || content < 0
        || content > static_cast<int>(Base::ContentType::Untranslatable)) {
        return false;
    }
    m.level = static_cast<Base::LogStyle>(level);
    m.recipient = static_cast<Base::IntendedRecipient>(recipient);
    m.content = static_cast<Base::ContentType>(content);
    m.notifier = header.value("notifier", std::string());
    m.text = header.value("text", std::string());
    m.worker = worker;
    return true;
}

/// The most of a job's messages kept: a worker saying more is said to
const std::size_t MostMessages = 500;

std::map<std::string, WorkerPool::Handler>& kinds()
{
    static std::map<std::string, WorkerPool::Handler> map;
    return map;
}

std::map<const Document*, std::unique_ptr<WorkerPool>>& pools()
{
    static std::map<const Document*, std::unique_ptr<WorkerPool>> map;
    return map;
}

/// The results of the jobs Python submitted, kept until asked for
std::map<const WorkerPool*, std::map<WorkerPool::JobId, WorkerPool::Result>>& keptResults()
{
    static std::map<const WorkerPool*, std::map<WorkerPool::JobId, WorkerPool::Result>> map;
    return map;
}

std::string randomToken()
{
    std::random_device device;
    std::mt19937_64 gen((static_cast<std::uint64_t>(device()) << 32) ^ device()
                        ^ static_cast<std::uint64_t>(now() * 1e9));
    char text[33];
    std::snprintf(text, sizeof(text), "%016llx%016llx",
                  static_cast<unsigned long long>(gen()), static_cast<unsigned long long>(gen()));
    return text;
}

/** What an ended worker leaves in the user cache: the transient directory of
 * its copy of the document and the one of its reader of this process's
 * store (FileHistory::openReader), each named after its pid. A worker that
 * quits removes them as it closes; one killed -- every worker as its
 * document closes, or the pool stops -- does not, and they were never
 * removed (docs/WorkerProcess.md sec 48)
 */
void removeLeftovers(long pid)
{
    if (pid <= 0) {
        return;
    }
    const std::string cache = Application::getUserCachePath();
    const std::string exe = Application::getExecutableName();
    const std::string tail = "_" + std::to_string(pid);
    for (auto& entry : Base::FileInfo(cache).getDirectoryContent()) {
        const std::string name = entry.fileName();
        const bool ours = name.rfind(exe + "_Doc_", 0) == 0 || name.rfind(exe + "_Reader_", 0) == 0;
        if (ours && name.size() > tail.size()
            && name.compare(name.size() - tail.size(), tail.size(), tail) == 0 && entry.isDir()) {
            entry.deleteDirectoryRecursive();
        }
    }
}

}  // namespace

// --------------------------------------------------------------------------

struct WorkerPool::Private
{
    struct Proc
    {
        std::unique_ptr<QProcess> process;
        std::unique_ptr<QLocalSocket> socket;
        std::string buffer;
        int number = 0;
        long pid = 0;
        JobId job = 0;          ///< running, 0 for none
        double began = 0;       ///< when it was handed the job
        double spawnedAt = 0;   ///< when it was started
        double idleSince = 0;   ///< when it last had nothing, 0 while busy
        bool ready = false;     ///< the copy is open
        bool quitting = false;  ///< told there is no more
        bool over = false;      ///< finished() ran
        /// finished() is running: what it reads may say the socket went,
        /// whose handler would end it again inside
        bool ending = false;
        bool killed = false;    ///< ended by the pool (kill())
        long made = 0;
        std::size_t resident = 0;
        /// What it was last told to open: the log's version and its row, in
        /// which store, from which copy's directory
        int64_t version = 0;
        int64_t versionSeq = 0;
        std::string store;
        std::string copyDir;     ///< the copy it holds, once it said ready
        std::string nextCopyDir; ///< the copy it was told to open
        long leaseCount = 0;     ///< the last range of string ids it was leased
        /// The expression permissions it decides by, as last sent
        /// (docs/WorkerProcess.md sec 23)
        std::string security;
        JobId lastJob = 0;  ///< the last it made
        bool setUpSince = false;  ///< told to open or move since
        /// Its last open or move: how long it said it took, and when it
        /// said it was ready
        double setupSeconds = 0;
        double setupAt = 0;
        /// Its last job's result as put in here (putIn), for its next move
        json own;
    };
    /// A connection that has not said who it is yet
    struct Stranger
    {
        std::unique_ptr<QLocalSocket> socket;
        std::string buffer;
    };
    struct Entry
    {
        Job job;
        int tries = 0;
        bool running = false;
        bool dropped = false;  ///< cancelled while running: its result is thrown away
        /// The copy it was submitted on: a pinned job is made there only
        std::string copyDir;
        /// What its workers said while making it (sec 27), and how many
        /// more there were than were kept
        std::vector<Message> messages;
        std::size_t messagesLost = 0;
        /// Where in it its worker said it is (Request::at), and where each
        /// worker that died over it was (sec 34)
        std::string at;
        std::vector<std::string> diedAt;
        std::string how;
        /// What the last worker to crash over it said of it (sec 35)
        std::string stack;
        std::string crashLog;
        /// When it was submitted and last handed to a worker, and how long
        /// the worker it went to was opening or moving its copy since it
        /// was submitted (Result::queued, Result::returned, Result::setup)
        double submittedAt = 0;
        double sentAt = 0;
        double setup = 0;
    };
    /// A copy the design moved past that a pinned job is still made on: its
    /// version stays held until no job is on it (Job::pinned)
    struct Held
    {
        std::weak_ptr<TransactionLogCore> core;
        int64_t version = 0;
    };
    std::map<std::string, Held> heldCopies;
    /// The file a job's handler may write
    std::string outFile(JobId id) const
    {
        return root + "/out/" + std::to_string(id);
    }
    /// Counts the calls in: what went is deleted only on the way into a
    /// call from outside (`entry`), never inside a signal of its own
    struct Depth
    {
        explicit Depth(Private& d, bool entry = false)
            : d(d)
        {
            if (entry && d.depth == 0) {
                d.gone.clear();
                d.strangersGone.clear();
            }
            ++d.depth;
        }
        ~Depth()
        {
            --d.depth;
        }
        Depth(const Depth&) = delete;
        Depth& operator=(const Depth&) = delete;
        Private& d;
    };

    Private(WorkerPool& pool, Document& doc)
        : pool(pool)
        , doc(doc)
    {}

    WorkerPool& pool;
    Document& doc;
    QObject context;  ///< owns the connections to the processes and sockets
    QTimer idleTimer;
    /// The copy gone stale and the document quiet for MoveWhenIdleSeconds:
    /// the newer copy made then, and the workers moved to it, before a job
    /// waits for both (docs/WorkerProcess.md sec 57)
    QTimer moveTimer;
    long idleCopies = 0;
    void armMove();
    void moveWhenIdle();
    std::unique_ptr<QLocalServer> server;
    std::string token;
    std::string dir;   ///< the copy's: root/copy-<k>
    /// The pool's directory, for as long as it lives: the settings, the
    /// logs, the results, the blobs put out, and the copies
    std::string root;
    std::string blobs;  ///< root/blobs: what the store keeps in files, by content
    std::string copy;  ///< the document's copy, empty when none stands
    std::string store;  ///< the store the workers read the version from, if they do
    std::weak_ptr<TransactionLogCore> leased;  ///< the log holding `version` for them
    int64_t version = 0;  ///< the log's version the copy is, 0 for a saved copy
    int64_t versionSeq = 0;  ///< the row the version's state is at
    /// The design changed since the copy was taken (sec 4)
    bool stale = false;
    /// And by a write no transaction recorded -- undo off, a script with
    /// no transaction: the log's rows do not add up to the next copy, and
    /// the workers open it whole
    bool unrecorded = false;
    long moves = 0;
    long reopens = 0;
    long ended = 0;
    long kept = 0;
    /// Seconds and count of the workers' starts, to their copy open
    std::pair<double, long> starts {0.0, 0};
    long leases = 0;
    long leasesAsked = 0;
    /// Copies before this one, gone once no worker is on one
    std::vector<std::string> oldCopies;
    std::vector<std::string> proxies;
    std::string program;
    std::deque<JobId> queue;
    std::map<JobId, Entry> jobs;
    std::vector<std::unique_ptr<Proc>> procs;
    std::vector<std::unique_ptr<Proc>> gone;
    std::vector<std::unique_ptr<Stranger>> strangers;
    std::vector<std::unique_ptr<Stranger>> strangersGone;
    std::map<std::string, std::pair<double, long>> costs;  ///< seconds and count, by kind
    std::size_t resident = 0;
    JobId next = 1;
    int limit = 1;
    int spawned = 0;
    int failedInRow = 0;
    long done = 0;
    long failed = 0;
    std::string reason;
    bool copying = false;
    int depth = 0;
    double lastClaim = -1e9;
    bool starved = false;
    fastsignals::scoped_connection changed;
    /// An object made or deleted, a property added or removed: no value
    /// changes, and the copy is stale all the same
    std::vector<fastsignals::scoped_connection> made;

    double meanCost(const std::string& kind) const
    {
        auto it = costs.find(kind);
        return it == costs.end() || !it->second.second
            ? 0.0
            : it->second.first / static_cast<double>(it->second.second);
    }

    bool listen();
    bool takeCopy();
    json setupFor(Proc& proc);
    bool lease(Proc& proc, json& range, bool stale = false);
    void pruneCopies();
    void dropReached(const DocumentObject* obj, const std::string& why,
                     const Property* prop = nullptr);
    bool exportBlobs(TransactionLogCore& core, int64_t num);
    void dropCopy();
    bool spawn();
    void accept();
    void readStranger(Stranger& s);
    void read(Proc& proc);
    void handle(Proc& proc, const json& header);
    void finished(Proc& proc);
    /// How `proc`'s process ended, once it has
    static std::string endedHow(const Proc& proc);
    /// What a worker that went said of its crash in its log (sec 35), put in
    /// the crash log: the report's text, empty for none
    std::string crashOf(const Proc& proc, JobId job, std::string& how, std::string& crashLog);
    void dispatch(Proc& proc);
    /// Hand job `id` to `proc`, which has none
    void sendJob(Proc& proc, JobId id);
    /// The pinned jobs of the copy at `was` (Job::pinned), the design moved
    /// past it: each queued one handed to a worker still on it, or back
    /// Cancelled; true if one is made there now
    bool keepPinned(const std::string& was, std::vector<std::pair<std::function<void(Result&)>, Result>>& told);
    void grow();
    void deliver(JobId id, Result& result);
    /// What the job's workers said, into its result
    static void takeMessages(Entry& entry, Result& result)
    {
        result.messages = std::move(entry.messages);
        entry.messages.clear();
        if (entry.messagesLost) {
            Message more;
            more.level = Base::LogStyle::Warning;
            more.recipient = Base::IntendedRecipient::Developer;
            more.content = Base::ContentType::Untranslatable;
            more.worker = result.messages.empty() ? 0 : result.messages.back().worker;
            more.text = std::to_string(entry.messagesLost)
                + " more of what it said while making a job (" + entry.job.kind
                + ") are in its log\n";
            result.messages.push_back(std::move(more));
            entry.messagesLost = 0;
        }
    }
    void checkIdle();
    void kill(Proc& proc);
    /** A running job was dropped (docs/WorkerProcess.md sec 26): its
     * worker is ended if the job has run past endAfter(), so what it goes
     * on making does not hold it. One that has run less is let finish: it
     * is as likely near its end as not, and a new worker -- started,
     * opening the copy whole, importing what its jobs need -- costs more
     * than it saves. Edits drop short jobs all the time
     */
    void endDropped(const std::vector<JobId>& ids);
    double startSeconds() const
    {
        return starts.second ? starts.first / static_cast<double>(starts.second) : 0.0;
    }
    double endAfter() const
    {
        return std::max(startSeconds(), params()->GetFloat("EndDroppedAfter", 2.0));
    }
    Proc* making(JobId id) const;
    void changedNotice()
    {
        WorkerPool::signalChanged(pool);
    }
};

bool WorkerPool::Private::listen()
{
    if (server && server->isListening()) {
        return true;
    }
    ensureCore();
    server = std::make_unique<QLocalServer>();
    // Only this user's processes may connect, and only those told the token
    server->setSocketOptions(QLocalServer::UserAccessOption);
    token = randomToken();
    static int serial = 0;
    const QString name = QStringLiteral("thundereal-%1-%2-%3")
                             .arg(QCoreApplication::applicationPid())
                             .arg(++serial)
                             .arg(QString::fromStdString(token.substr(0, 8)));
    QLocalServer::removeServer(name);
    if (!server->listen(name)) {
        reason = "cannot listen for workers: " + server->errorString().toStdString();
        server.reset();
        return false;
    }
    QObject::connect(server.get(), &QLocalServer::newConnection, &context, [this]() {
        Depth guard(*this);
        accept();
    });
    return true;
}

namespace
{
/// The newest version of the document's branch that nothing since changed
/// the document from -- after an open, the open's own -- or 0
/// (docs/WorkerProcess.md sec 21): a copy of it is the document as it is,
/// and the snapshot that would make one again (every shape's sharing
/// analysed again, 0.35-0.5 s in the console, 1.3-1.5 s in the GUI) is not
/// taken. The rows after it are records: the version's own, a save's, a
/// recompute's.
int64_t unchangedVersion(Document& doc)
{
    TransactionLog* log = doc.getTransactionLog();
    if (!log || doc.hasPendingTransaction() || doc.isPerformingTransaction()
        || doc.testStatus(Document::PartialDoc) || doc.testStatus(Document::Restoring)) {
        return 0;
    }
    auto& core = TransactionLogCore::of(doc.getFileHistory());
    core.flush();
    auto& store = core.store();
    const int64_t head = log->head();
    LogVersion best;
    for (const auto& v : store.versions()) {
        if (v.branch == log->branch() && v.seq <= head && v.num > best.num) {
            best = v;
        }
    }
    if (best.num <= 0) {
        return 0;
    }
    if (best.seq > 0) {
        return core.unchangedSince(head, best.seq) ? best.num : 0;
    }
    // At the root, before any row: every row of the branch a record
    for (const auto& t : store.chain(head, 0)) {
        if (!store.ops(t.seq).empty()) {
            return 0;
        }
    }
    return best.num;
}
}  // namespace

bool WorkerPool::Private::takeCopy()
{
    if (!copy.empty() && !stale) {
        return true;
    }
    // A copy standing that the design moved past: the workers on it are
    // moved to the new one (docs/WorkerProcess.md sec 4)
    const bool again = !copy.empty();
    program = workerProgram();
    if (program.empty()) {
        reason = "no console program found to run the workers";
        return false;
    }
    if (doc.getDependentDocuments(false).size() > 1) {
        reason = "the document links to others, which a worker would not find";
        return false;
    }
    if (root.empty()) {
        // The pool's, for as long as it lives: what every copy shares -- the
        // blobs put out for the workers, by content, their logs, their
        // results
        static int serial = 0;
        std::string base = Application::getTempPath();
        if (!base.empty() && base.back() != '/' && base.back() != '\\') {
            base += '/';
        }
        root = base + "Workers-" + std::to_string(QCoreApplication::applicationPid()) + "-"
            + std::to_string(++serial);
        Base::FileInfo old(root);
        if (old.exists()) {
            old.deleteDirectoryRecursive();
        }
    }
    if (!Base::FileInfo(root + "/out").createDirectories()) {
        reason = "cannot make " + root;
        return false;
    }
    static int copies = 0;
    const std::string where = root + "/copy-" + std::to_string(++copies);
    {
        Base::FileInfo info(where);
        if (info.exists()) {
            info.deleteDirectoryRecursive();
        }
        if (!info.createDirectories()) {
            reason = "cannot make " + where;
            return false;
        }
    }
    std::string file;
    std::string failure;
    std::string readFrom;
    int64_t num = 0;
    int64_t seq = 0;
    bool held = false;
    {
        Base::FlagToggler<bool> flag(copying);
        // A version of the log (docs/WorkerProcess.md sec 3): the design as
        // a save serialises it, with no save -- nothing put back, nothing
        // recomputed, no archive -- written out as an unpacked project with
        // the file's strings, which a version does not carry
        if (params()->GetBool("FromLog", true)) {
            try {
                // What a script or an event that is no command wrote is
                // committed first, as a save commits it: its row is what a
                // worker moves along. An explicit transaction stays open,
                // and the copy is then saved
                doc.commitImplicitTransaction();
                num = params()->GetBool("ReuseVersion", true) ? unchangedVersion(doc) : 0;
                if (num > 0) {
                    FC_LOG(doc.getName() << ": the workers copy version " << num
                                         << ", unchanged since");
                }
                else {
                    num = doc._snapshotToLog("workers");
                }
                if (num > 0) {
                    auto& core = TransactionLogCore::of(doc.getFileHistory());
                    core.flush();
                    for (const auto& v : core.store().versions()) {
                        if (v.num == num) {
                            seq = v.seq;
                        }
                    }
                    if (params()->GetBool("ReadStore", true) && exportBlobs(core, num)) {
                        // The workers read the version from this process's
                        // store themselves (sec 3): only the blobs it keeps
                        // in files of its own are put out, once each.
                        // Held while the copy stands: a worker started
                        // later reads it too
                        readFrom = doc.getFileHistory().directory() + "/history/log.db";
                        core.holdVersion(num, true);
                        held = true;
                    }
                }
                if (num > 0 && readFrom.empty()) {
                    auto& core = TransactionLogCore::of(doc.getFileHistory());
                    const std::string folder = where + "/version";
                    Document::materialiseVersion(core, num, folder, false);
                    StringHasherRef hasher = doc.getStringHasher();
                    Base::ofstream out(Base::FileInfo(folder + "/" + Document::stringTableName()),
                                       std::ios::out | std::ios::binary);
                    const std::string table = hasher ? hasher->saveTable() : std::string();
                    out.write(table.data(), static_cast<std::streamsize>(table.size()));
                    if (!out) {
                        THROWM(Base::FileException, "cannot write the string table");
                    }
                    file = folder;
                }
            }
            catch (Base::Exception& e) {
                FC_LOG(doc.getName() << ": no version of the log for the workers: " << e.what());
                if (held) {
                    TransactionLogCore::of(doc.getFileHistory()).holdVersion(num, false);
                }
                held = false;
                num = 0;
                seq = 0;
                readFrom.clear();
                file.clear();
            }
        }
        if (file.empty() && readFrom.empty()) {
            // A transaction is open, or the log is off: a copy saved as a
            // save writes it, which puts an Animation's rest values back
            // and its pose again
            num = 0;
            seq = 0;
            // Named as the document, which an open names itself by
            file = where + "/" + doc.getName() + ".FCStd";
            try {
                if (!doc.saveCopy(file.c_str(), false)) {
                    file.clear();
                }
            }
            catch (Base::Exception& e) {
                failure = e.what();
                file.clear();
            }
        }
    }
    if (file.empty() && readFrom.empty()) {
        Base::FileInfo(where).deleteDirectoryRecursive();
        reason = "the document could not be copied for the workers"
            + (failure.empty() ? std::string() : ": " + failure);
        return false;
    }
    // The copy before is let go of: its version is not held any more -- a
    // worker moving from it reads the rows after it, and the values of the
    // new one -- and its directory goes once no worker is on it. Unless a
    // pinned job is made on it: handed to a worker before that worker is
    // told to move (a socket keeps order), the version held until it is done
    const std::string was = dir;
    const Held before {leased, version};
    leased.reset();
    if (!dir.empty()) {
        oldCopies.push_back(dir);
    }
    dir = where;
    version = num;
    versionSeq = seq;
    store = readFrom;
    if (held) {
        leased = doc.getFileHistory().logCore();
    }
    std::vector<std::pair<std::function<void(Result&)>, Result>> told;
    if (!was.empty() && keepPinned(was, told) && !before.core.expired()) {
        heldCopies[was] = before;
    }
    else if (auto core = before.core.lock()) {
        core->holdVersion(before.version, false);
    }
    // Its own copy of the settings: a console program writes them back as
    // it exits, which would race this one's
    GetApplication().GetUserParameter().SaveDocument((root + "/user.cfg").c_str());
    GetApplication().GetSystemParameter().SaveDocument((root + "/system.cfg").c_str());
    // The objects with a Python class here: one a worker has not got would
    // recompute to nothing there
    proxies.clear();
    {
        Base::PyGILStateLocker lock;
        for (auto obj : doc.getObjects()) {
            auto proxy = freecad_cast<PropertyPythonObject*>(obj->getPropertyByName("Proxy"));
            if (proxy && !proxy->getValue().isNone()) {
                proxies.emplace_back(obj->getNameInDocument());
            }
        }
    }
    copy = store.empty() ? file : store;
    stale = false;
    if (!changed.connected()) {
        // A change of the design makes the copy stale, and drops the jobs
        // it reaches. An Animation's own properties are its business: it
        // says when its frames change
        changed = doc.signalChangedObject.connect([this](const DocumentObject& obj,
                                                         const Property& prop) {
            if (copying || copy.empty() || doc.testStatus(Document::Recomputing)
                || (WriteScope::causes() & WriteCause::Animation)
                || obj.isDerivedFrom<Animation>()
                || !(doc.writeEffects(prop) & WriteEffect::Dirty)) {
                return;
            }
            dropReached(&obj, "the design changed", &prop);
        });
        auto object = [this](const DocumentObject& obj) {
            if (copying || copy.empty() || obj.isDerivedFrom<Animation>()) {
                return;
            }
            dropReached(&obj, "the design changed");
        };
        made.emplace_back(doc.signalNewObject.connect(object));
        made.emplace_back(doc.signalDeletedObject.connect(object));
        auto property = [this](const Property& prop) {
            auto obj = freecad_cast<DocumentObject*>(prop.getContainer());
            if (copying || copy.empty() || !obj || obj->getDocument() != &doc
                || obj->isDerivedFrom<Animation>()) {
                return;
            }
            dropReached(obj, "the design changed");
        };
        made.emplace_back(GetApplication().signalAppendDynamicProperty.connect(property));
        made.emplace_back(GetApplication().signalRemoveDynamicProperty.connect(property));
    }
    if (again) {
        // Told in turn, after whatever job each is on: a socket keeps order,
        // so a job sent after this is made on the new copy
        std::vector<Proc*> list;
        for (auto& proc : procs) {
            list.push_back(proc.get());
        }
        for (auto proc : list) {
            if (proc->over || proc->quitting || !proc->socket) {
                continue;
            }
            json setup = setupFor(*proc);
            proc->ready = false;
            if (!send(*proc->socket, setup)) {
                kill(*proc);
            }
        }
    }
    unrecorded = false;
    pruneCopies();
    for (auto& [done, result] : told) {
        try {
            done(result);
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
        catch (std::exception& e) {
            FC_ERR("a worker job's client: " << e.what());
        }
    }
    return true;
}

json WorkerPool::Private::setupFor(Proc& proc)
{
    // The document's name and label: an export names what it writes by them
    // (docs/WorkerProcess.md sec 24)
    json setup {{"type", "setup"},
                {"proxies", proxies},
                {"name", doc.getName()},
                {"label", doc.Label.getStrValue()},
                {"relayLog", params()->GetBool("RelayLog", false)}};
    if (!store.empty()) {
        // A version read from this process's store, into a folder of its own
        setup["store"] = store;
        setup["version"] = version;
        setup["blobs"] = blobs;
        setup["document"] = dir + "/worker" + std::to_string(proc.number);
        if (proc.version > 0 && proc.versionSeq > 0 && versionSeq > 0 && proc.store == store
            && !unrecorded) {
            // Where it is: it moves from there along the log's rows
            setup["from"] = proc.version;
            setup["fromSeq"] = proc.versionSeq;
            setup["toSeq"] = versionSeq;
            if (!proc.own.is_null()) {
                setup["own"] = proc.own;
            }
        }
    }
    else {
        setup["document"] = copy;
    }
    // The ids it gives new strings (sec 15): a range this process's table
    // will never give, leased once; the worker asks for more as it runs out
    json range;
    if (!proc.leaseCount && lease(proc, range)) {
        setup["ids"] = range;
    }
    // How this table spells a name's index (StringHasher::setIndexedNames):
    // a copy opened from the log takes the new-document mode, and would
    // spell every name it makes otherwise than this table -- each reference
    // here to what it made then missed, and was searched for by geometry
    // (docs/WorkerProcess.md sec 20)
    if (StringHasherRef hasher = doc.getStringHasher()) {
        setup["indexed"] = hasher->getIndexedNames();
        // Spelled as this table spells, by digests where it does
        // (docs/StringTableContentIds.md sec 3.2)
        if (hasher->getContentIds()) {
            setup["contentids"] = true;
            setup["registry"] = StringHasher::registryToText(hasher->spellingRegistry());
        }
    }
    // Decided as here: what this process grants is what the worker grants,
    // not whatever its own read of grants.json says (docs/WorkerProcess.md
    // sec 23)
    proc.security = ExpressionSecurity::Runtime::instance().exportState();
    setup["security"] = proc.security;
    proc.version = store.empty() ? 0 : version;
    proc.versionSeq = store.empty() ? 0 : versionSeq;
    proc.store = store;
    proc.nextCopyDir = dir;
    proc.own = json();
    proc.setUpSince = true;
    return setup;
}

bool WorkerPool::Private::lease(Proc& proc, json& range, bool stale)
{
    // Out of this process's own counter (docs/WorkerProcess.md sec 15): a
    // string the worker gives one of these comes back under it, and none
    // this table gives itself, now or after a save, is one of them. Small
    // to start, doubling as a worker asks again: what goes unused is never
    // given, and an id is 32 bits on Windows
    StringHasherRef hasher = doc.getStringHasher();
    if (!hasher) {
        return false;
    }
    const long first = std::max<long>(16, params()->GetInt("IdBlock", 1L << 14));
    // A range let go for being below ids the worker has read since, not
    // for want of ids (sec 32): a small one in its place, which doubles
    // again should the job run out of it
    const long count = stale ? std::min<long>(first, 1L << 10)
        : proc.leaseCount ? std::max(proc.leaseCount, std::min(proc.leaseCount * 2, 1L << 20))
                          : first;
    const long at = hasher->reserveBlock(count);
    if (!at) {
        return false;
    }
    proc.leaseCount = count;
    ++leases;
    range = json {{"first", at}, {"count", count}};
    return true;
}

void WorkerPool::Private::pruneCopies()
{
    for (auto it = oldCopies.begin(); it != oldCopies.end();) {
        bool used = false;
        for (auto& proc : procs) {
            if (!proc->over && (proc->copyDir == *it || proc->nextCopyDir == *it)) {
                used = true;
            }
        }
        // A pinned job made on it (Job::pinned): the worker making it is
        // told to move once it is done, the copy goes after that
        for (const auto& kv : jobs) {
            if (kv.second.job.pinned && kv.second.running && kv.second.copyDir == *it) {
                used = true;
            }
        }
        if (used) {
            ++it;
            continue;
        }
        auto held = heldCopies.find(*it);
        if (held != heldCopies.end()) {
            if (auto core = held->second.core.lock()) {
                core->holdVersion(held->second.version, false);
            }
            heldCopies.erase(held);
        }
        if (!params()->GetBool("KeepJob", false)) {
            Base::FileInfo(*it).deleteDirectoryRecursive();
        }
        it = oldCopies.erase(it);
    }
}

void WorkerPool::Private::armMove()
{
    const double delay = params()->GetFloat("MoveWhenIdleSeconds", 2.0);
    if (delay <= 0 || !stale || copy.empty()) {
        return;
    }
    // Only workers there are: none is started for it
    bool live = false;
    for (auto& proc : procs) {
        live = live || (!proc->over && !proc->quitting && proc->socket);
    }
    if (live) {
        moveTimer.start(static_cast<int>(delay * 1000));
    }
}

void WorkerPool::Private::moveWhenIdle()
{
    if (!stale || copy.empty()) {
        return;
    }
    // Not in the middle of something: what it is doing makes the copy stale
    // again, or a job takes the copy itself. What a script or an event wrote
    // is committed, as takeCopy() and the GUI's closer commit it; a
    // transaction still open is a command's, and would have the copy saved
    // whole, not written as a version
    const bool busy = doc.testStatus(Document::Recomputing)
        || doc.testStatus(Document::Restoring) || doc.isPerformingTransaction();
    if (!busy) {
        doc.commitImplicitTransaction();
    }
    if (busy || doc.hasPendingTransaction()) {
        armMove();
        return;
    }
    if (!queue.empty()) {
        return;
    }
    for (auto& [id, entry] : jobs) {
        if (entry.running) {
            return;
        }
    }
    // Still a worker to move: one that went meanwhile (IdleSeconds) leaves
    // nothing to make the copy for
    bool live = false;
    for (auto& proc : procs) {
        live = live || (!proc->over && !proc->quitting && proc->socket);
    }
    if (!live) {
        return;
    }
    if (!takeCopy()) {
        // The next job tries again, and says why it cannot be had
        FC_LOG(doc.getName() << ": no newer copy for the workers while quiet: " << reason);
        return;
    }
    ++idleCopies;
    FC_LOG(doc.getName() << ": the workers moved to version " << version << " while quiet");
    changedNotice();
}

void WorkerPool::Private::dropReached(const DocumentObject* obj, const std::string& why,
                                      const Property* prop)
{
    stale = !copy.empty();
    armMove();
    if (!doc.hasPendingTransaction() && !doc.isPerformingTransaction()) {
        unrecorded = true;
    }
    // What `obj` reaches: itself and every object whose result depends on
    // it, through any link, an expression's hidden ones included -- what an
    // Animation drops its frames by. Of a change of what the view shows,
    // only what reads it (docs/WorkerProcess.md sec 17), and when nothing
    // does no job's result changes: the copy is behind, the jobs stand.
    // Otherwise looked for only if a job says what it reads
    std::set<std::string> reached;
    if (obj && prop && obj->getNameInDocument()) {
        auto objs = writeReaches(*obj, *prop);
        if (objs.empty()) {
            changedNotice();
            return;
        }
        for (auto o : objs) {
            if (o && o->getNameInDocument()) {
                reached.insert(o->getNameInDocument());
            }
        }
    }
    else if (obj) {
        const bool wanted = std::any_of(jobs.begin(), jobs.end(), [](const auto& kv) {
            return !kv.second.dropped && !kv.second.job.reads.empty();
        });
        if (wanted) {
            if (obj->getNameInDocument()) {
                reached.insert(obj->getNameInDocument());
            }
            for (auto o : obj->getInListEx(true)) {
                if (o && o->getNameInDocument()) {
                    reached.insert(o->getNameInDocument());
                }
            }
        }
    }
    std::vector<std::pair<std::function<void(Result&)>, Result>> told;
    std::vector<JobId> running;
    for (auto it = jobs.begin(); it != jobs.end();) {
        Entry& entry = it->second;
        bool hit = !entry.dropped && !entry.job.pinned
            && (!obj || entry.job.reads.empty()
                || std::any_of(entry.job.reads.begin(), entry.job.reads.end(),
                               [&](const std::string& name) { return reached.count(name) > 0; }));
        if (!hit) {
            ++it;
            continue;
        }
        Result result;
        result.id = it->first;
        result.kind = entry.job.kind;
        result.tag = entry.job.tag;
        result.status = Result::Status::Cancelled;
        result.error = why;
        auto done = std::move(entry.job.done);
        entry.job.done = nullptr;
        if (entry.running) {
            entry.dropped = true;  // its result, when it comes, is thrown away
            running.push_back(it->first);
            ++it;
        }
        else {
            const JobId id = it->first;
            it = jobs.erase(it);
            queue.erase(std::remove(queue.begin(), queue.end(), id), queue.end());
        }
        if (done) {
            told.emplace_back(std::move(done), std::move(result));
        }
    }
    endDropped(running);
    // Told once the pool is as it will be: a client may cancel or submit
    for (auto& [done, result] : told) {
        try {
            done(result);
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
        catch (std::exception& e) {
            FC_ERR("a worker job's client: " << e.what());
        }
    }
    changedNotice();
}

bool WorkerPool::Private::exportBlobs(TransactionLogCore& core, int64_t num)
{
    // Every entity the version reaches whose bytes are a file of this
    // process's blob store, which a worker cannot read: through a delta's
    // base and an attachment too
    blobs = root + "/blobs";
    if (!Base::FileInfo(blobs).createDirectories()) {
        return false;
    }
    auto& st = core.store();
    std::set<std::string> seen;
    std::vector<std::string> todo;
    for (const auto& e : st.manifest(num)) {
        todo.push_back(e.hash);
    }
    std::size_t linked = 0;
    std::size_t written = 0;
    while (!todo.empty()) {
        std::string hash = std::move(todo.back());
        todo.pop_back();
        if (hash.empty() || !seen.insert(hash).second) {
            continue;
        }
        LogEntity entity;
        if (!st.getEntity(hash, entity)) {
            return false;
        }
        if (entity.enc == "delta") {
            todo.push_back(entity.base);
        }
        for (const auto& r : entity.refs) {
            todo.push_back(r.target);
        }
        if (entity.enc != "file") {
            continue;
        }
        const std::string target = blobs + "/" + hash;
        if (Base::FileInfo(target).exists()) {
            continue;  // put out for an earlier copy: the same content
        }
        FileBlobHandle blob = core.heldBlob(hash);
        if (!blob) {
            blob = doc.getFileBlobManager().find(hash);
        }
        if (!blob) {
            FC_LOG(doc.getName() << ": blob " << hash << " of version " << num << " not found");
            return false;
        }
        if (!blob->inPack() && !blob->inArchive()) {
            // A file of its own: the same file under a second name, which
            // stays whatever the store does with its own
            std::error_code ec;
            std::filesystem::create_hard_link(blob->path(), target, ec);
            if (!ec) {
                ++linked;
                continue;
            }
        }
        std::string bytes;
        if (!blob->read(bytes)) {
            return false;
        }
        const std::string part = target + ".part";
        {
            Base::ofstream out(Base::FileInfo(part), std::ios::out | std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                return false;
            }
        }
        if (!Base::FileInfo(part).renameFile(target.c_str())) {
            return false;
        }
        ++written;
    }
    FC_LOG(doc.getName() << ": version " << num << " for the workers: " << linked
                         << " blobs linked, " << written << " written");
    return true;
}

void WorkerPool::Private::dropCopy()
{
    if (auto core = leased.lock()) {
        core->holdVersion(version, false);
    }
    leased.reset();
    for (auto& [where, held] : heldCopies) {
        if (auto core = held.core.lock()) {
            core->holdVersion(held.version, false);
        }
    }
    heldCopies.clear();
    store.clear();
    changed.disconnect();
    made.clear();
    copy.clear();
    proxies.clear();
    stale = false;
    unrecorded = false;
    version = 0;
    versionSeq = 0;
    if (!dir.empty()) {
        oldCopies.push_back(dir);
    }
    dir.clear();
    if (!params()->GetBool("KeepJob", false)) {
        for (auto& old : oldCopies) {
            Base::FileInfo info(old);
            if (info.exists()) {
                info.deleteDirectoryRecursive();
            }
        }
    }
    oldCopies.clear();
}

bool WorkerPool::Private::spawn()
{
    if (!listen()) {
        return false;
    }
    auto proc = std::make_unique<Proc>();
    proc->number = spawned;
    auto process = std::make_unique<QProcess>();
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QString::fromLatin1(ServerVariable), server->fullServerName());
    env.insert(QString::fromLatin1(TokenVariable), QString::fromStdString(token));
    process->setProcessEnvironment(env);
    // What it says goes to its log, kept with the pool for when one fails
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setStandardOutputFile(
        QString::fromStdString(root + "/worker" + std::to_string(spawned) + ".log"));
    process->setStandardInputFile(QProcess::nullDevice());
    QStringList args;
    args << QStringLiteral("-u") << QString::fromStdString(root + "/user.cfg")
         << QStringLiteral("-s") << QString::fromStdString(root + "/system.cfg")
         << QStringLiteral("-c") << QStringLiteral("from freecad import worker; worker.serve()");
    Proc* raw = proc.get();
    QObject::connect(process.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &context, [this, raw](int, QProcess::ExitStatus) {
                         Depth guard(*this);
                         finished(*raw);
                     });
    process->start(QString::fromStdString(program), args);
    if (!process->waitForStarted(10000)) {
        reason = "cannot start " + program;
        QObject::disconnect(process.get(), nullptr, &context, nullptr);
        return false;
    }
    proc->pid = static_cast<long>(process->processId());
    proc->spawnedAt = now();
    proc->process = std::move(process);
    procs.push_back(std::move(proc));
    ++spawned;
    changedNotice();
    return true;
}

void WorkerPool::Private::accept()
{
    while (server && server->hasPendingConnections()) {
        QLocalSocket* socket = server->nextPendingConnection();
        if (!socket) {
            break;
        }
        socket->setParent(nullptr);
        auto stranger = std::make_unique<Stranger>();
        stranger->socket.reset(socket);
        Stranger* raw = stranger.get();
        QObject::connect(socket, &QLocalSocket::readyRead, &context, [this, raw]() {
            Depth guard(*this);
            readStranger(*raw);
        });
        strangers.push_back(std::move(stranger));
        readStranger(*raw);
    }
}

void WorkerPool::Private::readStranger(Stranger& s)
{
    // Read already, inside a wait that said so: it has been told apart
    if (!s.socket) {
        return;
    }
    QByteArray bytes = s.socket->readAll();
    s.buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    json header;
    std::string body;
    int got = take(s.buffer, header, body);
    if (got == 0) {
        return;
    }
    Proc* proc = nullptr;
    if (got > 0 && header.value("type", "") == "hello" && header.value("token", "") == token) {
        const long pid = header.value("pid", 0L);
        for (auto& p : procs) {
            if (p->pid == pid && !p->socket && !p->over) {
                proc = p.get();
            }
        }
    }
    auto it = std::find_if(strangers.begin(), strangers.end(),
                           [&s](auto& one) { return one.get() == &s; });
    if (it == strangers.end()) {
        return;
    }
    std::unique_ptr<Stranger> held = std::move(*it);
    strangers.erase(it);
    QObject::disconnect(held->socket.get(), nullptr, &context, nullptr);
    if (!proc) {
        FC_WARN("a connection to the workers' socket that is not one of them, closed");
        held->socket->abort();
        strangersGone.push_back(std::move(held));
        return;
    }
    proc->socket = std::move(held->socket);
    proc->buffer = std::move(held->buffer);
    strangersGone.push_back(std::move(held));
    QLocalSocket* socket = proc->socket.get();
    QObject::connect(socket, &QLocalSocket::readyRead, &context, [this, proc]() {
        Depth guard(*this);
        read(*proc);
    });
    QObject::connect(socket, &QLocalSocket::disconnected, &context, [this, proc]() {
        Depth guard(*this);
        if (!proc->quitting) {
            // Gone without being told: what it was doing is not coming. A
            // process that died closed its socket on the way out: a moment
            // for it to be reaped, so that how it ended is known (sec 34)
            if (proc->process && proc->process->state() != QProcess::NotRunning) {
                proc->process->waitForFinished(1000);
            }
            kill(*proc);
        }
    });
    send(*socket, setupFor(*proc));
    read(*proc);
}

void WorkerPool::Private::read(Proc& proc)
{
    if (!proc.socket) {
        return;
    }
    // Until nothing is left: a socket tells of new data once, and what came
    // while a message was handled here would sit untold. A read inside a
    // message's handling -- a client waiting in its callback -- reads on:
    // a message is taken off the buffer whole, one at a time
    while (proc.socket && !proc.over
           && (proc.socket->bytesAvailable() > 0 || !proc.buffer.empty())) {
        QByteArray bytes = proc.socket->readAll();
        proc.buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        json header;
        std::string body;
        int got = take(proc.buffer, header, body);
        if (got == 0) {
            if (proc.socket->bytesAvailable() == 0) {
                break;
            }
            continue;
        }
        if (got < 0) {
            FC_WARN("worker " << proc.number << " said what is not a message; let go");
            kill(proc);
            return;
        }
        handle(proc, header);
    }
}

void WorkerPool::Private::handle(Proc& proc, const json& header)
{
    const std::string type = header.value("type", "");
    if (type == "ready") {
        proc.resident = header.value("resident", std::size_t(0));
        resident = std::max(resident, proc.resident);
        proc.ready = true;
        if (proc.copyDir.empty() && proc.spawnedAt > 0) {
            starts.first += now() - proc.spawnedAt;
            ++starts.second;
        }
        kept += header.value("kept", 0L);
        proc.setupSeconds = header.value("seconds", 0.0);
        proc.setupAt = now();
        auto moved = header.find("moved");
        if (moved != header.end() && moved->is_boolean()) {
            // A newer copy, along the log or opened whole (sec 4)
            ++(moved->get<bool>() ? moves : reopens);
            FC_LOG(doc.getName() << ": worker " << proc.number
                                 << (moved->get<bool>() ? " moved along the log" : " reopened")
                                 << " to version " << proc.version << " in "
                                 << header.value("seconds", 0.0) << " s");
        }
        proc.copyDir = proc.nextCopyDir;
        pruneCopies();
        dispatch(proc);
        changedNotice();
    }
    else if (type == "at") {
        // Where in its job it is (sec 34): what a death is put down to
        auto it = proc.job ? jobs.find(proc.job) : jobs.end();
        if (it != jobs.end() && header.value("job", JobId(0)) == proc.job) {
            it->second.at = header.value("what", std::string());
        }
    }
    else if (type == "log") {
        // Said through its Console (sec 27): with the job it makes, said
        // here when its result is; else now
        Message m;
        if (!readMessage(header, proc.number, m)) {
            return;
        }
        auto it = proc.job ? jobs.find(proc.job) : jobs.end();
        if (it == jobs.end()) {
            if (!proc.job) {
                WorkerPool::show(m);
            }
            return;
        }
        if (it->second.dropped) {
            return;
        }
        if (it->second.messages.size() < MostMessages) {
            it->second.messages.push_back(std::move(m));
        }
        else {
            ++it->second.messagesLost;
        }
    }
    else if (type == "ids") {
        // A job ran out of the ids it was leased: more, or none (count 0),
        // and the job fails there
        json answer {{"type", "ids"}, {"first", 0}, {"count", 0}};
        ++leasesAsked;
        json range;
        if (lease(proc, range, header.value("stale", false))) {
            answer["first"] = range["first"];
            answer["count"] = range["count"];
        }
        if (proc.socket && !send(*proc.socket, answer)) {
            kill(proc);
        }
    }
    else if (type == "unfit") {
        // The copy cannot be worked on there: no worker would do better
        std::string why = header.value("error", std::string());
        pool.stop(why.empty() ? std::string("a worker cannot work on the document") : why);
    }
    else if (type == "result") {
        const JobId id = header.value("job", JobId(0));
        if (proc.job == id) {
            proc.job = 0;
        }
        proc.lastJob = id;
        proc.setUpSince = false;
        proc.own = json();
        proc.resident = header.value("resident", std::size_t(0));
        resident = std::max(resident, proc.resident);
        ++proc.made;
        const std::string out = outFile(id);
        auto it = jobs.find(id);
        if (it == jobs.end() || it->second.dropped) {
            if (it != jobs.end()) {
                jobs.erase(it);
            }
            Base::FileInfo(out).deleteFile();
            dispatch(proc);
            changedNotice();
            return;
        }
        Result result;
        result.id = id;
        result.kind = it->second.job.kind;
        result.tag = it->second.job.tag;
        const bool ok = header.value("ok", false);
        const bool unfit = header.value("unfit", false);
        result.status = ok ? Result::Status::Done
                           : (unfit ? Result::Status::Unfit : Result::Status::Failed);
        result.error = header.value("error", std::string());
        auto output = header.find("output");
        if (output != header.end()) {
            result.output = output->dump();
        }
        result.seconds = header.value("seconds", 0.0);
        result.queued = it->second.sentAt - it->second.submittedAt;
        result.returned = now() - it->second.sentAt;
        result.setup = it->second.setup;
        result.resident = proc.resident;
        takeMessages(it->second, result);
        if (Base::FileInfo(out).exists()) {
            result.file = out;
        }
        if (ok) {
            auto& cost = costs[result.kind];
            cost.first += result.seconds;
            ++cost.second;
            ++done;
            failedInRow = 0;
        }
        else if (!unfit) {
            ++failed;
            ++failedInRow;
            FC_LOG(doc.getName() << ": worker " << proc.number << " failed job " << id << " ("
                                 << result.kind << "): " << result.error);
        }
        deliver(id, result);
        if (failedInRow >= 3) {
            pool.stop("the workers fail: " + result.error);
            return;
        }
        pruneCopies();
        dispatch(proc);
        grow();
        changedNotice();
    }
}

void WorkerPool::Private::deliver(JobId id, Result& result)
{
    auto it = jobs.find(id);
    if (it == jobs.end()) {
        return;
    }
    auto done = std::move(it->second.job.done);
    jobs.erase(it);
    queue.erase(std::remove(queue.begin(), queue.end(), id), queue.end());
    if (done) {
        try {
            done(result);
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
        catch (std::exception& e) {
            FC_ERR("a worker job's client: " << e.what());
        }
    }
    if (!result.file.empty()) {
        Base::FileInfo(result.file).deleteFile();
    }
    // What its worker said, unless the client took it: not for one done
    // here instead, or not wanted (Result::messages)
    if (result.status == Result::Status::Done || result.status == Result::Status::Failed) {
        for (auto& m : result.messages) {
            WorkerPool::show(m);
        }
    }
}

void WorkerPool::show(const Message& m)
{
    const std::string worker = "worker " + std::to_string(m.worker);
    const std::string notifier = m.notifier.empty() ? worker : m.notifier + " (" + worker + ")";
    switch (m.level) {
        case Base::LogStyle::Warning:
            showAs<Base::LogStyle::Warning>(m, notifier);
            break;
        case Base::LogStyle::Message:
            showAs<Base::LogStyle::Message>(m, notifier);
            break;
        case Base::LogStyle::Error:
            showAs<Base::LogStyle::Error>(m, notifier);
            break;
        case Base::LogStyle::Log:
            showAs<Base::LogStyle::Log>(m, notifier);
            break;
        case Base::LogStyle::Critical:
            showAs<Base::LogStyle::Critical>(m, notifier);
            break;
        case Base::LogStyle::Notification:
            showAs<Base::LogStyle::Notification>(m, notifier);
            break;
    }
}

WorkerPool::Private::Proc* WorkerPool::Private::making(JobId id) const
{
    for (auto& proc : procs) {
        if (proc->job == id) {
            return proc.get();
        }
    }
    return nullptr;
}

void WorkerPool::Private::kill(Proc& proc)
{
    // Being ended already: not killed by the pool, which would leave its
    // crash unread
    if (proc.over || proc.ending) {
        return;
    }
    if (proc.process && proc.process->state() != QProcess::NotRunning) {
        QObject::disconnect(proc.process.get(), nullptr, &context, nullptr);
        proc.killed = true;
        proc.process->kill();
        proc.process->waitForFinished(2000);
    }
    finished(proc);
}

void WorkerPool::Private::endDropped(const std::vector<JobId>& ids)
{
    if (ids.empty() || !params()->GetBool("EndDropped", true)) {
        return;
    }
    const double after = endAfter();
    bool any = false;
    for (JobId id : ids) {
        Proc* proc = making(id);
        if (!proc || proc->over || proc->quitting) {
            continue;
        }
        const double ran = now() - proc->began;
        if (ran < after) {
            continue;
        }
        auto it = jobs.find(id);
        const std::string kind = it != jobs.end() ? it->second.job.kind : std::string();
        FC_LOG(doc.getName() << ": worker " << proc->number << " ended, its job " << id << " ("
                             << kind << ") dropped after " << ran << " s");
        proc->quitting = true;  // not a failure: finished() says nothing of it
        ++ended;
        any = true;
        kill(*proc);
    }
    if (any) {
        grow();
    }
}

std::string WorkerPool::Private::endedHow(const Proc& proc)
{
    if (proc.killed) {
        return "ended by the pool";
    }
    if (!proc.process || proc.process->state() != QProcess::NotRunning) {
        return "gone";
    }
    const int code = proc.process->exitCode();
    if (proc.process->exitStatus() == QProcess::CrashExit) {
#ifdef FC_OS_WIN32
        std::ostringstream ss;
        ss << "crashed (0x" << std::hex << static_cast<unsigned int>(code) << ")";
        return ss.str();
#else
        // On a crash QProcess has the signal as the exit code
        std::string how = "crashed (signal " + std::to_string(code);
        if (const char* name = strsignal(code)) {
            // macOS ends it in the number: "Segmentation fault: 11"
            std::string text = name;
            const auto colon = text.find(':');
            how += ": " + text.substr(0, colon);
        }
        return how + ")";
#endif
    }
    return "exited with code " + std::to_string(code);
}

std::string WorkerPool::Private::crashOf(const Proc& proc, JobId job, std::string& how,
                                        std::string& crashLog)
{
    const std::string path = root + "/worker" + std::to_string(proc.number) + ".log";
    std::string log;
    {
        Base::ifstream in(Base::FileInfo(path), std::ios::in | std::ios::binary);
        if (!in) {
            return {};
        }
        std::ostringstream all;
        all << in.rdbuf();
        log = all.str();
    }
    WorkerCrash::Report report = WorkerCrash::read(log);
    if (!report.found) {
        return {};
    }
    // Ended by its handler, not killed by the signal: it says which
    how = "crashed (" + WorkerCrash::signalName(report.signal) + ")";
#ifdef FC_OS_WIN32
    // An exception is told by its code there, as endedHow() does (sec 41)
    if (report.code) {
        std::ostringstream ss;
        ss << "crashed (0x" << std::hex << static_cast<unsigned int>(report.code) << ")";
        how = ss.str();
    }
#endif
    std::string kind;
    auto it = jobs.find(job);
    if (it != jobs.end()) {
        kind = it->second.job.kind;
    }
    std::ostringstream title;
    title << doc.getName() << ": worker " << proc.number << " (pid " << proc.pid
          << ") crashed making job " << job << " (" << kind << ")";
    const std::string stack = WorkerCrash::text(report);
    {
        Base::CrashLog::Entry entry(Base::CrashLog::Severity::Caught, title.str());
        entry.line(stack);
    }
    crashLog = Base::CrashLog::fileName();
    FC_LOG(title.str() << "\n" << stack);
    // The frames' lines take the tools seconds: found away from here, and
    // put in the log when they are
    if (params()->GetBool("CrashLines", true)) {
        if (QThreadPool* threads = QThreadPool::globalInstance()) {
            std::string again = title.str() + ", the frames with their lines";
            threads->start([report, again]() mutable {
                if (App::isShuttingDown()) {
                    return;
                }
                WorkerCrash::resolveLines(report);
                bool any = false;
                for (auto& f : report.frames) {
                    any = any || !f.line.empty();
                }
                if (any && !App::isShuttingDown()) {
                    Base::CrashLog::Entry entry(Base::CrashLog::Severity::Caught, again);
                    entry.line(WorkerCrash::text(report));
                }
            });
        }
    }
    return stack;
}

void WorkerPool::Private::finished(Proc& proc)
{
    if (proc.over || proc.ending) {
        return;
    }
    // The waits below deliver the socket's signals: its `disconnected`
    // ends it (kill()), which would run all of this inside this, and leave
    // this one to count a worker gone with no job as one that failed
    proc.ending = true;
    // The process reaped before its socket was read: what it said last is
    // still with the system. A worker its crash handler ends goes at once,
    // and its last `at` came after the process's end (sec 35)
    if (proc.socket && proc.socket->state() == QLocalSocket::ConnectedState) {
        for (int i = 0; i < 100 && proc.socket->waitForReadyRead(10); ++i) {
        }
    }
    // What it said before it went
    read(proc);
    proc.over = true;
    proc.ready = false;
    if (proc.socket) {
        QObject::disconnect(proc.socket.get(), nullptr, &context, nullptr);
    }
    if (proc.process) {
        QObject::disconnect(proc.process.get(), nullptr, &context, nullptr);
    }
    const JobId lost = proc.job;
    proc.job = 0;
    // Where in the job it died: a death in an object's execute() is that
    // object's, not the workers' (sec 34)
    bool inObject = false;
    if (lost) {
        auto it = jobs.find(lost);
        inObject = it != jobs.end() && !it->second.at.empty();
    }
    // How it ended, for the job it was making: the socket may say it went
    // before the process is reaped, which is a moment away
    std::string how;
    if (lost && proc.process) {
        if (proc.process->state() != QProcess::NotRunning) {
            proc.process->waitForFinished(2000);
        }
        how = endedHow(proc);
    }
    std::string stack;
    std::string crashLog;
    if (lost && !proc.killed) {
        stack = crashOf(proc, lost, how, crashLog);
    }
    if (!proc.process || proc.process->state() == QProcess::NotRunning) {
        removeLeftovers(proc.pid);
    }
    for (auto it = procs.begin(); it != procs.end(); ++it) {
        if (it->get() == &proc) {
            gone.push_back(std::move(*it));  // not deleted inside its own signal
            procs.erase(it);
            break;
        }
    }
    const bool quiet = proc.quitting;
    if (lost) {
        Base::FileInfo(outFile(lost)).deleteFile();
        auto it = jobs.find(lost);
        if (it != jobs.end()) {
            it->second.running = false;
            it->second.diedAt.push_back(it->second.at);
            it->second.at.clear();
            it->second.how = how;
            it->second.stack = stack;
            it->second.crashLog = crashLog;
            if (it->second.dropped) {
                jobs.erase(it);
            }
            else if (++it->second.tries < 2
                     && (!it->second.job.pinned || it->second.copyDir == dir)) {
                // Once more, first in line: the death may not be the job's.
                // A pinned one only while its copy is the one standing
                queue.push_front(lost);
            }
            else {
                ++failed;
                Result result;
                result.id = lost;
                result.kind = it->second.job.kind;
                result.tag = it->second.job.tag;
                result.status = Result::Status::Failed;
                result.error = "its worker died over it";
                if (!how.empty()) {
                    result.error += " (" + how + ")";
                }
                if (!it->second.diedAt.back().empty()) {
                    result.error += ", at " + it->second.diedAt.back();
                }
                if (!crashLog.empty()) {
                    result.error += "; what was on its stack is in " + crashLog;
                }
                takeMessages(it->second, result);
                result.died = true;
                result.diedAt = it->second.diedAt;
                result.how = how;
                result.stack = stack;
                result.crashLog = crashLog;
                deliver(lost, result);
            }
        }
    }
    if (!quiet) {
        FC_LOG(doc.getName() << ": worker " << proc.number << " (pid " << proc.pid << ") ended"
                             << (lost ? " in the middle of a job" : ""));
        if (!inObject && ++failedInRow >= 3) {
            pool.stop("the workers exited");
            return;
        }
    }
    pruneCopies();
    // Work left and nobody to do it
    if (!copy.empty() && procs.empty() && !queue.empty()) {
        if (!spawn()) {
            pool.stop(reason);
            return;
        }
    }
    changedNotice();
}

void WorkerPool::Private::dispatch(Proc& proc)
{
    if (copy.empty() || !proc.ready || proc.job || proc.quitting || proc.over || !proc.socket) {
        return;
    }
    for (auto at = queue.begin(); at != queue.end();) {
        const JobId id = *at;
        auto it = jobs.find(id);
        if (it == jobs.end() || it->second.running) {
            at = queue.erase(at);
            continue;
        }
        if (it->second.job.pinned && it->second.copyDir != proc.nextCopyDir) {
            // Made on its own copy only, which this worker is not on
            ++at;
            continue;
        }
        queue.erase(at);
        json inputs = json::parse(it->second.job.inputs, nullptr, false);
        if (inputs.is_discarded()) {
            Result result;
            result.id = id;
            result.kind = it->second.job.kind;
            result.tag = it->second.job.tag;
            result.error = "its inputs are not JSON";
            ++failed;
            deliver(id, result);
            at = queue.begin();
            continue;
        }
        sendJob(proc, id);
        return;
    }
    // Nothing for it: let go of it once it has been idle long enough
    if (!proc.idleSince) {
        proc.idleSince = now();
    }
    const double idle = params()->GetFloat("IdleSeconds", 30.0);
    if (idle <= 0) {
        checkIdle();
    }
    else if (!idleTimer.isActive()) {
        idleTimer.start(static_cast<int>(idle * 1000) + 50);
    }
}

void WorkerPool::Private::sendJob(Proc& proc, JobId id)
{
    auto it = jobs.find(id);
    if (it == jobs.end()) {
        return;
    }
    json inputs = json::parse(it->second.job.inputs, nullptr, false);
    json header {{"type", "job"},
                 {"job", id},
                 {"kind", it->second.job.kind},
                 {"inputs", inputs},
                 {"module", it->second.job.module},
                 {"out", outFile(id)}};
    // A grant given or taken back since it was last told reaches it
    // before the job does: a job it runs is never decided by an answer
    // this process no longer gives
    std::string security = ExpressionSecurity::Runtime::instance().exportState();
    if (security != proc.security) {
        header["security"] = security;
        proc.security = std::move(security);
    }
    it->second.running = true;
    it->second.sentAt = now();
    it->second.setup = proc.setupSeconds > 0 && proc.setupAt >= it->second.submittedAt
        ? proc.setupSeconds
        : 0;
    proc.job = id;
    proc.began = it->second.sentAt;
    proc.idleSince = 0;
    if (!send(*proc.socket, header)) {
        kill(proc);
    }
}

bool WorkerPool::Private::keepPinned(
    const std::string& was, std::vector<std::pair<std::function<void(Result&)>, Result>>& told)
{
    bool there = false;
    for (const auto& kv : jobs) {
        if (kv.second.job.pinned && kv.second.running && kv.second.copyDir == was) {
            there = true;
        }
    }
    const std::deque<JobId> waiting = queue;
    for (JobId id : waiting) {
        auto it = jobs.find(id);
        if (it == jobs.end() || it->second.running || !it->second.job.pinned
            || it->second.copyDir != was) {
            continue;
        }
        // A worker on that copy, or told to open it, with nothing to do: the
        // job goes before the word to move, and is made there
        Proc* free = nullptr;
        for (auto& proc : procs) {
            if (!proc->over && !proc->quitting && proc->socket && !proc->job
                && proc->nextCopyDir == was) {
                free = proc.get();
                break;
            }
        }
        if (free) {
            queue.erase(std::remove(queue.begin(), queue.end(), id), queue.end());
            sendJob(*free, id);
            if (jobs.count(id) && jobs[id].running) {
                there = true;
            }
            continue;
        }
        Result result;
        result.id = id;
        result.kind = it->second.job.kind;
        result.tag = it->second.job.tag;
        result.status = Result::Status::Cancelled;
        result.error = "the design changed before a worker could make it";
        auto done = std::move(it->second.job.done);
        jobs.erase(it);
        queue.erase(std::remove(queue.begin(), queue.end(), id), queue.end());
        if (done) {
            told.emplace_back(std::move(done), std::move(result));
        }
    }
    return there;
}

void WorkerPool::Private::checkIdle()
{
    const double idle = params()->GetFloat("IdleSeconds", 30.0);
    const double t = now();
    double soonest = -1;
    std::vector<Proc*> list;
    for (auto& proc : procs) {
        list.push_back(proc.get());
    }
    for (auto proc : list) {
        if (!proc->ready || proc->job || proc->quitting || !proc->idleSince) {
            continue;
        }
        const double left = proc->idleSince + idle - t;
        if (left <= 0 || !queue.empty()) {
            if (!queue.empty()) {
                dispatch(*proc);
                continue;
            }
            // It goes, and its memory with it
            proc->quitting = true;
            if (proc->socket) {
                send(*proc->socket, json {{"type", "quit"}});
            }
            changedNotice();
        }
        else if (soonest < 0 || left < soonest) {
            soonest = left;
        }
    }
    if (soonest >= 0) {
        idleTimer.start(static_cast<int>(soonest * 1000) + 50);
    }
}

void WorkerPool::Private::grow()
{
    if (copy.empty() || static_cast<int>(procs.size()) >= limit || queue.empty()) {
        return;
    }
    // One at a time, and once each has done a job: then what one holds is
    // known, and the memory left says whether another fits
    std::size_t biggest = 0;
    for (auto& proc : procs) {
        if (!proc->made || proc->quitting) {
            return;
        }
        biggest = std::max(biggest, proc->resident);
    }
    if (!biggest) {
        return;
    }
    // With a client taking the results as they come, another worker only
    // when it had to wait for one or do one itself; with none, when there
    // are more jobs, running and queued, than workers -- a recompute split
    // in two has one running and one waiting (docs/WorkerProcess.md sec 42)
    const bool consuming = now() - lastClaim < 2.0;
    std::size_t running = 0;
    for (auto& proc : procs) {
        running += proc->job ? 1 : 0;
    }
    if (consuming ? !starved : queue.size() + running <= procs.size()) {
        return;
    }
    const std::size_t total = totalMemory();
    const std::size_t reserve = std::max<std::size_t>(std::size_t(1) << 30, total / 7);
    const std::size_t available = availableMemory();
    const std::size_t need = biggest + biggest / 4;
    if (available) {
        if (available < reserve + need) {
            return;
        }
    }
    else if (total && (procs.size() + 1) * biggest > total / 2) {
        return;
    }
    starved = false;
    if (!spawn()) {
        FC_LOG(doc.getName() << ": " << reason);
    }
}

// --------------------------------------------------------------------------

WorkerPool::WorkerPool(Document& doc)
    : d(std::make_unique<Private>(*this, doc))
{
    d->idleTimer.setSingleShot(true);
    QObject::connect(&d->idleTimer, &QTimer::timeout, &d->context, [this]() {
        Private::Depth guard(*d, true);  // a timer of its own, no socket's signal
        d->checkIdle();
    });
    d->moveTimer.setSingleShot(true);
    QObject::connect(&d->moveTimer, &QTimer::timeout, &d->context, [this]() {
        Private::Depth guard(*d, true);
        d->moveWhenIdle();
    });
}

WorkerPool::~WorkerPool()
{
    // No client is told: the document, and they, are going
    for (auto& proc : d->procs) {
        if (proc->socket) {
            QObject::disconnect(proc->socket.get(), nullptr, &d->context, nullptr);
        }
        if (proc->process) {
            QObject::disconnect(proc->process.get(), nullptr, &d->context, nullptr);
            if (proc->process->state() != QProcess::NotRunning) {
                proc->process->kill();
                proc->process->waitForFinished(2000);
            }
            if (proc->process->state() == QProcess::NotRunning) {
                removeLeftovers(proc->pid);
            }
        }
    }
    d->procs.clear();
    d->strangers.clear();
    d->jobs.clear();
    d->dropCopy();
    if (!d->root.empty() && !params()->GetBool("KeepJob", false)) {
        Base::FileInfo(d->root).deleteDirectoryRecursive();
    }
    keptResults().erase(this);
}

WorkerPool* WorkerPool::of(Document& doc, bool create)
{
    auto& map = pools();
    auto it = map.find(&doc);
    if (it != map.end()) {
        return it->second.get();
    }
    if (!create) {
        return nullptr;
    }
    static bool watching = false;
    if (!watching) {
        watching = true;
        GetApplication().signalDeleteDocument.connect(
            [](const Document& d) { pools().erase(&d); });
    }
    auto pool = std::unique_ptr<WorkerPool>(new WorkerPool(doc));
    auto raw = pool.get();
    map[&doc] = std::move(pool);
    return raw;
}

bool WorkerPool::supported(std::string* why)
{
#if defined(__EMSCRIPTEN__)
    if (why) {
        *why = "this build runs no processes";
    }
    return false;
#else
    if (params()->GetInt("Workers", -1) == 0) {
        if (why) {
            *why = "workers are off (Workers of Preferences/Workers)";
        }
        return false;
    }
    return true;
#endif
}

std::size_t WorkerPool::residentBytes()
{
#if defined(__APPLE__)
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count)
        == KERN_SUCCESS) {
        return static_cast<std::size_t>(info.phys_footprint);
    }
    return 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters {};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        return static_cast<std::size_t>(counters.WorkingSetSize);
    }
    return 0;
#else
    std::size_t pages = 0;
    std::size_t resident = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%zu %zu", &pages, &resident) != 2) {
            resident = 0;
        }
        std::fclose(f);
    }
    return resident * static_cast<std::size_t>(sysconf(_SC_PAGE_SIZE));
#endif
}

bool WorkerPool::start()
{
    Private::Depth guard(*d, true);
    if (!supported(&d->reason)) {
        return false;
    }
    d->limit = workerLimit();
    if (!d->takeCopy()) {
        if (started()) {
            // A newer copy could not be had: the one standing is not the
            // design, and nothing more is made on it
            const std::string why = d->reason;
            stop(why);
        }
        return false;
    }
    if (d->procs.empty() && !d->spawn()) {
        d->dropCopy();
        return false;
    }
    return true;
}

bool WorkerPool::started() const
{
    return !d->copy.empty();
}

WorkerPool::JobId WorkerPool::submit(Job job)
{
    Private::Depth guard(*d, true);
    if (d->copy.empty()) {
        return 0;
    }
    // The design changed since the copy: a new one, the workers moved to it
    if (d->stale && !d->takeCopy()) {
        const std::string why = d->reason;
        stop(why);
        return 0;
    }
    const JobId id = d->next++;
    Private::Entry entry;
    entry.job = std::move(job);
    entry.copyDir = d->dir;
    entry.submittedAt = now();
    d->jobs.emplace(id, std::move(entry));
    d->queue.push_back(id);
    if (d->procs.empty() && !d->spawn()) {
        d->jobs.erase(id);
        d->queue.pop_back();
        return 0;
    }
    std::vector<Private::Proc*> list;
    for (auto& proc : d->procs) {
        list.push_back(proc.get());
    }
    for (auto proc : list) {
        d->dispatch(*proc);
    }
    d->grow();
    return id;
}

WorkerPool::State WorkerPool::state(JobId id) const
{
    auto it = d->jobs.find(id);
    if (it == d->jobs.end() || it->second.dropped) {
        return State::Unknown;
    }
    return it->second.running ? State::Running : State::Queued;
}

WorkerPool::Claim WorkerPool::claim(JobId id)
{
    Private::Depth guard(*d, true);
    poll(0);
    auto it = d->jobs.find(id);
    if (it == d->jobs.end() || it->second.dropped) {
        return Claim::Unknown;
    }
    d->lastClaim = now();
    if (!it->second.running) {
        // Wanted before a worker got to it: the client does it
        d->jobs.erase(it);
        d->queue.erase(std::remove(d->queue.begin(), d->queue.end(), id), d->queue.end());
        if (!d->procs.empty()) {
            d->starved = true;
            d->grow();
        }
        return Claim::Yours;
    }
    d->starved = true;
    d->grow();
    Private::Proc* proc = d->making(id);
    if (!proc) {
        return Claim::Unknown;
    }
    // Waited for: it is on its way, and nearer done than one begun here.
    // Not for ever: a worker that hangs is let go and the client does it
    const std::string kind = it->second.job.kind;
    const double most = std::max(30.0, 20.0 * d->meanCost(kind));
    const double began = now();
    while (!proc->over && proc->job == id) {
        d->read(*proc);
        if (proc->over || proc->job != id) {
            break;
        }
        // The others are not kept waiting for their next job meanwhile
        for (auto& other : d->procs) {
            if (other.get() != proc && other->socket && other->socket->waitForReadyRead(0)) {
                d->read(*other);
            }
        }
        if (d->server && noEventLoop()) {
            d->server->waitForNewConnection(0);
        }
        if (proc->over || proc->job != id) {
            break;
        }
        if (proc->socket) {
            proc->socket->waitForReadyRead(20);
            d->read(*proc);
        }
        else if (proc->process) {
            proc->process->waitForFinished(20);
        }
        if (proc->process && proc->process->state() == QProcess::NotRunning) {
            d->finished(*proc);
            break;
        }
        if (now() - began > most && !proc->over && proc->job == id) {
            FC_WARN(d->doc.getName() << ": a worker took " << static_cast<int>(now() - began)
                                     << " s over a job (" << kind << "), let go");
            auto entry = d->jobs.find(id);
            if (entry != d->jobs.end()) {
                d->jobs.erase(entry);  // the client does it, no one is told
            }
            ++d->failed;
            d->kill(*proc);
            return Claim::Failed;
        }
    }
    it = d->jobs.find(id);
    if (it == d->jobs.end()) {
        return Claim::Done;
    }
    // Its worker died and it is queued again: the client does it
    d->jobs.erase(it);
    d->queue.erase(std::remove(d->queue.begin(), d->queue.end(), id), d->queue.end());
    return Claim::Failed;
}

void WorkerPool::putIn(JobId job, long long fromSeq, long long toSeq,
                       const std::vector<std::string>& objects)
{
    if (!job || fromSeq <= 0 || toSeq < fromSeq || objects.empty()) {
        return;
    }
    for (auto& proc : d->procs) {
        // Only while what it holds is that job's: no job and no move since
        if (proc->lastJob == job && !proc->job && !proc->over && !proc->setUpSince) {
            proc->own = json {{"job", job}, {"from", fromSeq}, {"to", toSeq}, {"objects", objects}};
        }
    }
}

std::size_t WorkerPool::cancel(const std::function<bool(const std::string&)>& which)
{
    Private::Depth guard(*d, true);
    std::size_t count = 0;
    std::vector<JobId> running;
    for (auto it = d->jobs.begin(); it != d->jobs.end();) {
        if (it->second.dropped || !which(it->second.job.tag)) {
            ++it;
            continue;
        }
        ++count;
        if (it->second.running) {
            it->second.dropped = true;
            it->second.job.done = nullptr;
            running.push_back(it->first);
            ++it;
        }
        else {
            const JobId id = it->first;
            it = d->jobs.erase(it);
            d->queue.erase(std::remove(d->queue.begin(), d->queue.end(), id), d->queue.end());
        }
    }
    d->endDropped(running);
    if (count) {
        d->changedNotice();
    }
    return count;
}

void WorkerPool::stop(const std::string& why)
{
    Private::Depth guard(*d, true);
    if (!why.empty() && (started() || !d->procs.empty())) {
        d->reason = why;
    }
    auto list = std::move(d->procs);
    d->procs.clear();
    for (auto& proc : list) {
        proc->over = true;
        if (proc->socket) {
            QObject::disconnect(proc->socket.get(), nullptr, &d->context, nullptr);
        }
        if (proc->process) {
            QObject::disconnect(proc->process.get(), nullptr, &d->context, nullptr);
            if (proc->process->state() != QProcess::NotRunning) {
                proc->process->kill();
                proc->process->waitForFinished(2000);
            }
            if (proc->process->state() == QProcess::NotRunning) {
                removeLeftovers(proc->pid);
            }
        }
        d->gone.push_back(std::move(proc));
    }
    for (auto& s : d->strangers) {
        QObject::disconnect(s->socket.get(), nullptr, &d->context, nullptr);
        s->socket->abort();
        d->strangersGone.push_back(std::move(s));
    }
    d->strangers.clear();
    d->idleTimer.stop();
    d->failedInRow = 0;
    auto jobs = std::move(d->jobs);
    d->jobs.clear();
    d->queue.clear();
    d->dropCopy();
    for (auto& [id, entry] : jobs) {
        if (entry.dropped || !entry.job.done) {
            continue;
        }
        Result result;
        result.id = id;
        result.kind = entry.job.kind;
        result.tag = entry.job.tag;
        result.status = Result::Status::Cancelled;
        result.error = d->reason;
        try {
            entry.job.done(result);
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
    }
    d->changedNotice();
}

std::size_t WorkerPool::releaseDirectory(const std::string& dir, int ms)
{
    Private::Depth guard(*d, true);
    if (dir.empty()) {
        return 0;
    }
    auto under = [&dir](const std::string& path) {
        return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0
            && (path[dir.size()] == '/' || path[dir.size()] == '\\');
    };
    // The next job's copy is read from where the store goes, and a worker
    // set up on it opens it whole: the old path is no more
    if (under(d->store)) {
        d->stale = true;
    }
    std::vector<Private::Proc*> waited;
    std::size_t left = 0;
    for (auto& proc : d->procs) {
        if (proc->over || !under(proc->store)) {
            continue;
        }
        if (!proc->quitting) {
            proc->quitting = true;
            if (!proc->socket || !send(*proc->socket, json {{"type", "quit"}})) {
                d->kill(*proc);
                continue;
            }
        }
        if (proc->job) {
            // Read after its job, whose result comes as ever: not waited for
            // here, inside the document's change
            ++left;
        }
        else {
            waited.push_back(proc.get());
        }
    }
    QElapsedTimer timer;
    timer.start();
    for (auto proc : waited) {
        // Its end runs finished() through the process's signal; `gone` keeps
        // it until the next call in
        if (!proc->over && proc->process) {
            const int rest = std::max(0, ms - static_cast<int>(timer.elapsed()));
            if (!proc->process->waitForFinished(rest)) {
                if (!proc->over) {
                    ++left;
                }
            }
            else if (!proc->over) {
                d->finished(*proc);
            }
        }
    }
    if (!waited.empty() || left) {
        FC_LOG(d->doc.getName() << ": " << waited.size() << " worker(s) ended for the move of "
                                << dir << (left ? ", some still running" : ""));
        d->changedNotice();
    }
    return left;
}

void WorkerPool::designChanged(const DocumentObject* obj)
{
    Private::Depth guard(*d, true);
    if (d->copy.empty()) {
        return;
    }
    d->dropReached(obj, "the design changed");
}

void WorkerPool::poll(int ms)
{
    Private::Depth guard(*d, true);
    QElapsedTimer timer;
    timer.start();
    do {
        if (d->server && noEventLoop()) {
            d->server->waitForNewConnection(0);
            d->accept();
        }
        else if (ms > 0 && d->server) {
            // With an event loop a connection is taken by the server's own
            // notifier, which runs only in the loop: a wait -- waitWorkers()
            // from a script in the GUI -- lets it. Not a poll of none
            // (claim()'s, inside an evaluation), and no input: a click
            // handled in here would act inside the caller
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
        std::vector<Private::Stranger*> strangers;
        for (auto& s : d->strangers) {
            strangers.push_back(s.get());
        }
        for (auto s : strangers) {
            // The wait reads it itself, through readyRead, when it can
            if (s->socket && (s->socket->waitForReadyRead(0) || s->socket->bytesAvailable() > 0)) {
                d->readStranger(*s);
            }
        }
        std::vector<Private::Proc*> list;
        for (auto& proc : d->procs) {
            list.push_back(proc.get());
        }
        const int slice = ms > 0 && !list.empty()
            ? std::max(1, std::min(20, ms / static_cast<int>(list.size())))
            : 0;
        for (auto proc : list) {
            if (proc->over) {
                continue;
            }
            d->read(*proc);
            if (proc->over) {
                continue;
            }
            if (proc->quitting && proc->process) {
                // Told to go: what is left is its end
                if (proc->process->waitForFinished(slice) && !proc->over) {
                    d->finished(*proc);
                }
                continue;
            }
            if (proc->socket) {
                if (proc->socket->waitForReadyRead(slice)) {
                    d->read(*proc);
                }
                else if (proc->socket->state() != QLocalSocket::ConnectedState
                         && !proc->quitting) {
                    d->kill(*proc);
                    continue;
                }
            }
            // Without an event loop a process's end is seen only when asked
            if (proc->process && !proc->over && proc->process->waitForFinished(0)) {
                d->finished(*proc);
            }
            else if (proc->process && !proc->over
                     && proc->process->state() == QProcess::NotRunning) {
                d->finished(*proc);
            }
        }
        if (list.empty() && ms > 0 && d->server && d->strangers.empty() && noEventLoop()) {
            d->server->waitForNewConnection(std::min(ms, 20));
        }
        else if (list.empty() && ms > 0) {
            QThread::msleep(5);
        }
        d->checkIdle();
    } while (ms > 0 && timer.elapsed() < ms);
}

bool WorkerPool::wait(double seconds)
{
    Private::Depth guard(*d, true);
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        bool pending = false;
        for (auto& [id, entry] : d->jobs) {
            if (!entry.dropped) {
                pending = true;
                break;
            }
        }
        if (!pending) {
            return true;
        }
        if (seconds >= 0 && timer.elapsed() >= static_cast<qint64>(seconds * 1000)) {
            return false;
        }
        poll(seconds == 0 ? 0 : 50);
    }
}

WorkerPool::Info WorkerPool::info() const
{
    Info info;
    info.workers = static_cast<int>(d->procs.size());
    info.limit = started() ? d->limit : workerLimit();
    info.spawned = d->spawned;
    for (auto& proc : d->procs) {
        if (proc->job) {
            ++info.busy;
        }
        info.pids.push_back(proc->pid);
    }
    info.queued = d->queue.size();
    info.resident = d->resident;
    info.done = d->done;
    info.failed = d->failed;
    info.reason = d->reason;
    info.directory = d->root;
    info.copy = d->dir;
    info.version = d->version;
    info.stale = d->stale;
    info.moves = d->moves;
    info.idleCopies = d->idleCopies;
    info.reopens = d->reopens;
    info.ended = d->ended;
    info.kept = d->kept;
    info.startSeconds = d->startSeconds();
    info.endAfter = d->endAfter();
    info.leases = d->leases;
    info.leasesAsked = d->leasesAsked;
    info.readStore = !d->store.empty();
    info.blobs = d->blobs;
    return info;
}

const std::string& WorkerPool::reason() const
{
    return d->reason;
}

Document& WorkerPool::document() const
{
    return d->doc;
}

// --------------------------------------------------------------------------
// The worker's side

bool WorkerPool::registerKind(const std::string& kind, Handler handler)
{
    kinds()[kind] = std::move(handler);
    return true;
}

namespace
{

/// A Python kind, through freecad.worker._run, which says what came of it
/// as the reply's JSON
WorkerPool::Reply runPython(const WorkerPool::Request& request, const std::string& module)
{
    WorkerPool::Reply reply;
    Base::PyGILStateLocker lock;
    try {
        Py::Module worker(PyImport_ImportModule("freecad.worker"), true);
        Py::Callable run(worker.getAttr("_run"));
        Py::Tuple args(5);
        args.setItem(0, Py::String(request.kind));
        args.setItem(1, Py::String(request.document ? request.document->getName() : ""));
        args.setItem(2, Py::String(request.inputs));
        args.setItem(3, Py::String(request.out));
        args.setItem(4, Py::String(module));
        Py::Object said = run.apply(args);
        json answer = json::parse(Py::String(said).as_std_string("utf-8"), nullptr, false);
        if (!answer.is_object()) {
            reply.ok = false;
            reply.error = "freecad.worker answered what is not JSON";
            return reply;
        }
        reply.ok = answer.value("ok", false);
        reply.unfit = answer.value("unfit", false);
        reply.error = answer.value("error", std::string());
        auto output = answer.find("output");
        if (output != answer.end()) {
            reply.output = output->dump();
        }
    }
    catch (Py::Exception&) {
        Base::PyException e;
        reply.ok = false;
        reply.error = e.what();
    }
    return reply;
}

std::string unfitFor(Document& doc, const json& proxies)
{
    if (!proxies.is_array()) {
        return {};
    }
    Base::PyGILStateLocker lock;
    for (auto& item : proxies) {
        if (!item.is_string()) {
            continue;
        }
        const std::string name = item.get<std::string>();
        auto obj = doc.getObject(name.c_str());
        auto proxy = obj ? freecad_cast<PropertyPythonObject*>(obj->getPropertyByName("Proxy"))
                         : nullptr;
        if (!proxy || proxy->getValue().isNone()) {
            return (obj ? std::string(obj->Label.getValue()) : name)
                + " has no Python class here";
        }
    }
    return {};
}

}  // namespace

int WorkerPool::serve()
{
    ensureCore();
    // A crash says where it was, what was on the stack, to the pool (sec 35)
    WorkerCrash::install();
    const QByteArray name = qgetenv(ServerVariable);
    const QByteArray token = qgetenv(TokenVariable);
    if (name.isEmpty()) {
        Base::Console().error("No pool to serve: %s is not set\n", ServerVariable);
        return 2;
    }
    QLocalSocket socket;
    socket.connectToServer(QString::fromLocal8Bit(name));
    if (!socket.waitForConnected(10000)) {
        Base::Console().error("Cannot reach the pool: %s\n",
                              socket.errorString().toStdString().c_str());
        return 2;
    }
    send(socket, json {{"type", "hello"},
                       {"token", token.toStdString()},
                       {"pid", static_cast<long>(QCoreApplication::applicationPid())}});
    // The sandbox's guest booted beside the copy's open rather than by the
    // first expression of the first job, which waited 1.7 s for it on the
    // mini (docs/WorkerProcess.md sec 66, 67): nothing where expressions are
    // not routed. The host's lock lets an evaluation meanwhile wait for the
    // boot without holding Python's (ExpressionImageHost.cpp, HostLock)
    struct Boot
    {
        std::thread thread;
        ~Boot()
        {
            if (thread.joinable()) {
                if (Py_IsInitialized() && PyGILState_Check()) {
                    Base::PyGILStateRelease unlock;
                    thread.join();
                }
                else {
                    thread.join();
                }
            }
        }
    } boot;
    if (params()->GetBool("BootSandbox", true)) {
        boot.thread = std::thread([] {
            try {
                ExpressionSandbox::evaluationRouted();
            }
            catch (...) {
            }
        });
    }
    // What it says through the Console goes to the pool too (sec 27), until
    // the socket goes: retired before it, last in first out
    auto relay = new Relay(socket);
    Base::Console().AttachObserver(relay);
    struct Retire
    {
        Relay* relay;
        ~Retire()
        {
            Base::Console().RetireObserver(relay);
        }
    } retire {relay};
    std::string buffer;
    Document* doc = nullptr;
    // The pool's process's store, read for the version and to move along
    // its log (docs/WorkerProcess.md sec 3, 4)
    std::shared_ptr<FileHistory> reader;
    std::string readerStore;
    int64_t held = 0;  ///< the version `doc` is, read from `reader`; 0 for none
    std::string folder;  ///< the folder `doc` was read from, its files with it
    // The last job made, and whether it was set up since: a move right after
    // it may keep the values that job made as they are held (sec 29)
    JobId lastJob = 0;
    bool setUpSince = true;
    auto closeDoc = [&]() {
        if (doc) {
            GetApplication().closeDocument(doc->getName());
        }
        doc = nullptr;
        held = 0;
        // Gone with the document, unless kept, as the pool keeps its own,
        // for a look
        if (!folder.empty() && !params()->GetBool("KeepJob", false)) {
            Base::FileInfo(folder).deleteDirectoryRecursive();
        }
        folder.clear();
    };
    // A message from the pool: one set aside first, then the socket's. 0
    // for one, -1 when it is not a message, -2 when the pool has gone
    std::deque<std::pair<json, std::string>> pending;
    auto receive = [&](json& header, std::string& body, bool keep) {
        if (keep && !pending.empty()) {
            header = std::move(pending.front().first);
            body = std::move(pending.front().second);
            pending.pop_front();
            return 0;
        }
        int got = 0;
        while ((got = take(buffer, header, body)) == 0) {
            if (socket.bytesAvailable() == 0) {
                if (socket.state() != QLocalSocket::ConnectedState) {
                    return -2;
                }
                bool ready = false;
                // Python's lock let go while nothing comes: the guest's
                // boot on its own thread takes it for what it asks of the
                // host
                if (Py_IsInitialized() && PyGILState_Check()) {
                    Base::PyGILStateRelease unlock;
                    ready = socket.waitForReadyRead(1000);
                }
                else {
                    ready = socket.waitForReadyRead(1000);
                }
                if (!ready) {
                    continue;
                }
            }
            QByteArray bytes = socket.readAll();
            buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        }
        return got < 0 ? -1 : 0;
    };
    // New strings are given ids the pool's table leased this worker (sec
    // 15): a result's strings go back under them, and what is read in later
    // from the store, under that table's ids, never meets one given here.
    // Shared by every table the copy uses, and kept when it is reopened
    std::shared_ptr<StringIDLease> lease;
    // The expression permissions the pool's process decides by, which this
    // one decides by alone from the first setup on (sec 23): a state that
    // cannot be read leaves the job, or the copy, refused
    auto adoptSecurity = [](const json& header) {
        auto it = header.find("security");
        if (it == header.end() || !it->is_string()) {
            return;
        }
        std::string why;
        if (!ExpressionSecurity::Runtime::instance().adoptState(it->get<std::string>(), &why)) {
            THROWM(Base::RuntimeError, "cannot take the expression permissions: " + why);
        }
    };
    auto useLease = [&lease](const StringHasherRef& hasher) {
        if (hasher && lease) {
            hasher->setLease(lease);
        }
    };
    auto startLease = [&](const json& range) {
        if (lease || !range.is_object()) {
            return;
        }
        const long first = range.value("first", 0L);
        const long count = range.value("count", 0L);
        if (first <= 0 || count <= 0) {
            return;
        }
        lease = std::make_shared<StringIDLease>();
        lease->add(first, count);
        // Run out in a job: asked for, and waited for, what else the pool
        // says meanwhile kept for after it
        lease->more = [&](long& next, long& n) {
            if (!send(socket, json {{"type", "ids"}, {"stale", lease->stale}})) {
                return false;
            }
            for (;;) {
                json header;
                std::string body;
                if (receive(header, body, false) < 0) {
                    return false;
                }
                if (header.value("type", "") == "ids") {
                    next = header.value("first", 0L);
                    n = header.value("count", 0L);
                    return next > 0 && n > 0;
                }
                pending.emplace_back(std::move(header), std::move(body));
            }
        };
    };
    // Move `doc`, version `from` at row `fromSeq`, to the state at row
    // `toSeq` through the rows between: only what they changed is restored,
    // nothing recomputed (sec 4). False, nothing done, when they cannot be
    // folded: a value the store no longer has, a row trimmed
    auto moveAlong = [&](int64_t from, int64_t fromSeq, int64_t toSeq) {
        TransactionLogCore& core = *reader->logCore();
        core.refreshReader();
        LogVersion at;
        at.num = from;
        at.seq = fromSeq;
        TransactionLog through(*doc, &at);
        return doc->moveAlongLogQuietly(fromSeq, toSeq, &through);
    };
    for (;;) {
        json header;
        std::string body;
        const int got = receive(header, body, true);
        if (got < 0) {
            break;  // the pool has gone, or said what is not a message
        }
        const std::string type = header.value("type", "");
        if (type == "quit") {
            break;
        }
        if (type == "setup") {
            std::string why;
            const bool later = doc != nullptr;
            bool moved = false;
            const double began = now();
            long kept = 0;
            relay->bLog = header.value("relayLog", false);
            try {
                adoptSecurity(header);
                const std::string file = header.value("document", std::string());
                const std::string store = header.value("store", std::string());
                const int64_t num = header.value("version", int64_t(0));
                if (auto ids = header.find("ids"); ids != header.end()) {
                    startLease(*ids);
                }
                const int64_t from = header.value("from", int64_t(0));
                const int64_t fromSeq = header.value("fromSeq", int64_t(0));
                const int64_t toSeq = header.value("toSeq", int64_t(0));
                if (doc && reader && held > 0 && held == from && fromSeq > 0 && toSeq > 0
                    && store == readerStore) {
                    try {
                        // Across the rows its last result was put in as:
                        // what it sent is kept as it holds it, not read back
                        // as the other process wrote it again (sec 29)
                        std::unique_ptr<LogMoveKeep> keep;
                        auto own = header.find("own");
                        if (own != header.end() && own->is_object() && !setUpSince
                            && own->value("job", JobId(0)) == lastJob) {
                            const int64_t first = own->value("from", int64_t(0));
                            const int64_t last = own->value("to", int64_t(0));
                            std::set<std::string> names;
                            for (auto& n : own->value("objects", json::array())) {
                                if (n.is_string()) {
                                    names.insert(n.get<std::string>());
                                }
                            }
                            Document* d = doc;
                            keep = std::make_unique<LogMoveKeep>(
                                *doc, [d, first, last, names](long cid, const std::string& prop,
                                                             int64_t txn) {
                                    if (txn < first || txn > last) {
                                        return false;
                                    }
                                    auto obj = d->getObjectByID(cid);
                                    const char* name = obj ? obj->getNameInDocument() : nullptr;
                                    return name && names.count(name) > 0
                                        && WorkerRecompute::sent(name, prop);
                                });
                        }
                        moved = moveAlong(from, fromSeq, toSeq);
                        if (keep) {
                            kept = keep->kept;
                        }
                    }
                    catch (Base::Exception& e) {
                        FC_WARN("moving along the log: " << e.what());
                        moved = false;
                    }
                    if (moved) {
                        held = num;
                    }
                    else {
                        FC_LOG("the rows from version " << from << " to " << num
                                                        << " not folded: opened whole");
                    }
                }
                if (!moved) {
                    closeDoc();
                    if (!store.empty()) {
                        // The version read from the pool's process's store
                        // (sec 3), written out here as the unpacked project a
                        // restore reads, and opened as a version of that file:
                        // its strings, its blobs, the reader's, which a move
                        // goes on reading. The blobs are found there by hash,
                        // not written
                        if (!reader || readerStore != store) {
                            std::string failed;
                            reader = FileHistory::openReader(
                                store, header.value("blobs", std::string()), &failed);
                            if (!reader) {
                                THROWM(Base::RuntimeError, failed);
                            }
                            readerStore = store;
                        }
                        else {
                            reader->logCore()->refreshReader();
                        }
                        useLease(reader->hasher());
                        TransactionLogCore& core = *reader->logCore();
                        folder = file;
                        Document::materialiseVersion(core, num, file, true);
                        auto& app = GetApplication();
                        // Named as the document it is a copy of, alone here
                        const std::string name = app.getUniqueDocumentName(
                            header.value("name", std::string("Copy")).c_str(), true);
                        doc = app.newDocument(name.c_str(), name.c_str(), false, false, false);
                        if (!doc) {
                            THROWM(Base::RuntimeError, "cannot make the copy's document");
                        }
                        // Nothing is undone here
                        doc->setUndoMode(0);
                        doc->_restoreAsVersion(reader, file, std::string());
                        useLease(doc->getStringHasher());
                        held = num;
                    }
                    else {
                        doc = GetApplication().openDocument(file.c_str(), false);
                        if (doc) {
                            useLease(doc->getStringHasher());
                            doc->setUndoMode(0);
                        }
                    }
                }
                if (!doc) {
                    why = "cannot open " + file;
                }
                else {
                    if (auto label = header.find("label");
                        label != header.end() && label->is_string()) {
                        doc->Label.setValue(label->get<std::string>());
                    }
                    // Names spelled as the pool's table spells them
                    if (auto indexed = header.find("indexed");
                        indexed != header.end() && indexed->is_boolean()) {
                        if (StringHasherRef hasher = doc->getStringHasher()) {
                            hasher->setIndexedNames(indexed->get<bool>());
                        }
                        if (reader && reader->hasher()) {
                            reader->hasher()->setIndexedNames(indexed->get<bool>());
                        }
                    }
                    if (header.value("contentids", false)) {
                        const auto registry =
                            StringHasher::registryFromText(header.value("registry", std::string()));
                        for (StringHasherRef hasher :
                             {doc->getStringHasher(),
                              reader ? reader->hasher() : StringHasherRef()}) {
                            if (hasher) {
                                hasher->takeRegistry(registry);
                                hasher->setContentIds(true);
                            }
                        }
                    }
                    why = unfitFor(*doc, header.value("proxies", json::array()));
                }
            }
            catch (Base::Exception& e) {
                why = std::string("cannot open the document: ") + e.what();
            }
            if (why.empty()) {
                json ready {{"type", "ready"},
                            {"resident", residentBytes()},
                            {"seconds", now() - began}};
                if (later) {
                    ready["moved"] = moved;
                }
                if (kept) {
                    ready["kept"] = kept;
                }
                relay->flush();
                send(socket, ready);
            }
            else {
                send(socket, json {{"type", "unfit"}, {"error", why}});
                break;
            }
            setUpSince = true;
            continue;
        }
        if (type != "job") {
            continue;
        }
        std::string refused;
        try {
            adoptSecurity(header);
        }
        catch (Base::Exception& e) {
            refused = e.what();
        }
        Request request;
        request.kind = header.value("kind", std::string());
        auto inputs = header.find("inputs");
        request.inputs = inputs != header.end() ? inputs->dump() : std::string("{}");
        request.out = header.value("out", std::string());
        request.document = doc;
        // Said as it changes, flushed as it is: a worker that dies next has
        // said it (sec 34)
        const JobId jobId = header.value("job", JobId(0));
        std::string lastAt;
        WorkerCrash::setAt(std::string());
        request.at = [&](const std::string& where) {
            if (where == lastAt) {
                return;
            }
            lastAt = where;
            WorkerCrash::setAt(where);
            send(socket, json {{"type", "at"}, {"job", jobId}, {"what", where}});
        };
        const double began = now();
        Reply reply;
        auto handler = kinds().find(request.kind);
        auto& security = ExpressionSecurity::Runtime::instance();
        const std::size_t refusals = security.refusals();
        try {
            if (!refused.empty()) {
                reply.unfit = true;
                reply.error = refused;
            }
            else if (handler != kinds().end()) {
                reply = handler->second(request);
            }
            else {
                reply = runPython(request, header.value("module", std::string()));
            }
        }
        catch (Base::Exception& e) {
            reply.ok = false;
            reply.error = e.what();
        }
        catch (std::exception& e) {
            reply.ok = false;
            reply.error = e.what();
        }
        WorkerCrash::setAt(std::string());
        // A permission refused here is asked for, and the work done, by the
        // pool's process: it records the refusal and puts the question to
        // the user, where an answer the worker gave would be an error put
        // in with nobody asked (docs/WorkerProcess.md sec 23)
        if (security.refusals() != refusals && !reply.unfit) {
            reply.unfit = true;
            reply.error = "an expression permission was refused";
        }
        json output = json::parse(reply.output, nullptr, false);
        if (output.is_discarded()) {
            output = json::object();
        }
        json answer {{"type", "result"},
                     {"job", header.value("job", JobId(0))},
                     {"ok", reply.ok && !reply.unfit},
                     {"unfit", reply.unfit},
                     {"error", reply.error},
                     {"output", output},
                     {"seconds", now() - began},
                     {"resident", residentBytes()}};
        relay->flush();  // what other threads said, before the result
        lastJob = header.value("job", JobId(0));
        setUpSince = false;
        if (!send(socket, answer)) {
            break;
        }
    }
    closeDoc();
    if (lease) {
        lease->more = nullptr;  // what it reaches goes now
    }
    socket.disconnectFromServer();
    return 0;
}

// --------------------------------------------------------------------------
// Python: FreeCAD.workerPool(doc)

namespace
{

const char* levelName(Base::LogStyle level)
{
    switch (level) {
        case Base::LogStyle::Warning:
            return "Warning";
        case Base::LogStyle::Message:
            return "Message";
        case Base::LogStyle::Error:
            return "Error";
        case Base::LogStyle::Log:
            return "Log";
        case Base::LogStyle::Critical:
            return "Critical";
        case Base::LogStyle::Notification:
            return "Notification";
    }
    return "";
}

const char* statusName(WorkerPool::Result::Status status)
{
    switch (status) {
        case WorkerPool::Result::Status::Done:
            return "Done";
        case WorkerPool::Result::Status::Failed:
            return "Failed";
        case WorkerPool::Result::Status::Unfit:
            return "Unfit";
        case WorkerPool::Result::Status::Cancelled:
            return "Cancelled";
    }
    return "Failed";
}

Py::Object fromJson(const std::string& text)
{
    Py::Module mod(PyImport_ImportModule("json"), true);
    Py::Callable loads(mod.getAttr("loads"));
    Py::Tuple args(1);
    args.setItem(0, Py::String(text));
    return loads.apply(args);
}

std::string toJson(const Py::Object& obj)
{
    Py::Module mod(PyImport_ImportModule("json"), true);
    Py::Callable dumps(mod.getAttr("dumps"));
    Py::Tuple args(1);
    args.setItem(0, obj);
    return Py::String(dumps.apply(args)).as_std_string("utf-8");
}

class WorkerPoolPy: public Py::PythonExtension<WorkerPoolPy>
{
public:
    static void init_type()
    {
        behaviors().name("WorkerPool");
        behaviors().doc("The worker processes of a document (docs/WorkerProcess.md)");
        behaviors().supportRepr();
        behaviors().supportGetattr();
        add_varargs_method("start", &WorkerPoolPy::start,
                           "start() -> bool: take the document's copy and start a worker");
        add_keyword_method("submit", &WorkerPoolPy::submit,
                           "submit(kind, inputs={}, module='', tag='', reads=None, pinned=False) ->\n"
                           "job id, 0 if not started; reads: the objects (or names) its result is\n"
                           "made from; pinned: made on the copy standing now, whatever changes");
        add_varargs_method("claim", &WorkerPoolPy::claim,
                           "claim(id) -> 'Yours', 'Done', 'Failed' or 'Unknown'");
        add_varargs_method("state", &WorkerPoolPy::state,
                           "state(id) -> 'Queued', 'Running' or 'Unknown'");
        add_varargs_method("result", &WorkerPoolPy::result,
                           "result(id) -> dict or None: what came of a job, once");
        add_varargs_method("cancel", &WorkerPoolPy::cancel, "cancel(tag) -> count");
        add_varargs_method("stop", &WorkerPoolPy::stop, "stop(reason='stopped')");
        add_varargs_method("poll", &WorkerPoolPy::poll, "poll(ms=0)");
        add_varargs_method("wait", &WorkerPoolPy::wait,
                           "wait(seconds=-1) -> bool: no job queued or running");
        add_varargs_method("info", &WorkerPoolPy::info, "info() -> dict");
        add_varargs_method("recompute", &WorkerPoolPy::recompute,
                           "recompute(objs=None) -> bool: hand the recompute of objs, or of what\n"
                           "is touched, and of what depends on them, to a worker; its result\n"
                           "is put in when it comes (docs/WorkerProcess.md sec 19)");
        add_varargs_method("recomputing", &WorkerPoolPy::recomputing,
                           "recomputing() -> bool: a recompute is with the workers, or has\n"
                           "come and is not put in yet");
        add_varargs_method("waitRecompute", &WorkerPoolPy::waitRecompute,
                           "waitRecompute(seconds=-1) -> bool: put in what comes until none\n"
                           "is running");
        add_varargs_method("cancelRecompute", &WorkerPoolPy::cancelRecompute,
                           "cancelRecompute(reason='cancelled')");
        add_varargs_method("recomputeInfo", &WorkerPoolPy::recomputeInfo,
                           "recomputeInfo() -> dict");
        add_varargs_method("expectedRecompute", &WorkerPoolPy::expectedRecompute,
                           "expectedRecompute([objects]) -> (seconds, unknown): what the "
                           "recompute of what is touched -- or of `objects` and what depends "
                           "on them -- would take here, by what its objects took when last "
                           "made; the first call starts noting it");
    }

    explicit WorkerPoolPy(const std::string& name)
        : name(name)
    {}

    Py::Object repr() override
    {
        return Py::String("<WorkerPool of " + name + ">");
    }

    WorkerPool& pool()
    {
        auto doc = GetApplication().getDocument(name.c_str());
        if (!doc) {
            throw Py::RuntimeError("the document is gone");
        }
        return *WorkerPool::of(*doc);
    }

    Py::Object start(const Py::Tuple&)
    {
        return Py::Boolean(pool().start());
    }

    Py::Object submit(const Py::Tuple& args, const Py::Dict& kwds)
    {
        static const std::array<const char*, 7> keywords {"kind", "inputs", "module", "tag",
                                                          "reads", "pinned", nullptr};
        const char* kind = nullptr;
        PyObject* inputs = nullptr;
        const char* module = "";
        const char* tag = "";
        PyObject* reads = nullptr;
        PyObject* pinned = Py_False;
        if (!Base::Wrapped_ParseTupleAndKeywords(args.ptr(), kwds.ptr(), "s|OssOO!", keywords,
                                                 &kind, &inputs, &module, &tag, &reads,
                                                 &PyBool_Type, &pinned)) {
            throw Py::Exception();
        }
        WorkerPool& p = pool();
        WorkerPool::Job job;
        job.kind = kind;
        job.inputs = inputs ? toJson(Py::Object(inputs)) : std::string("{}");
        job.module = module;
        job.tag = tag;
        job.pinned = PyObject_IsTrue(pinned) != 0;
        if (reads && reads != Py_None) {
            // Names, or the objects themselves
            Py::Sequence seq(reads);
            for (Py::Sequence::size_type i = 0; i < seq.size(); ++i) {
                Py::Object item(seq[i]);
                if (PyObject_TypeCheck(item.ptr(), &DocumentObjectPy::Type)) {
                    auto obj = static_cast<DocumentObjectPy*>(item.ptr())->getDocumentObjectPtr();
                    if (obj && obj->getNameInDocument()) {
                        job.reads.emplace_back(obj->getNameInDocument());
                    }
                }
                else {
                    job.reads.push_back(Py::String(item).as_std_string("utf-8"));
                }
            }
        }
        WorkerPool* raw = &p;
        job.done = [raw](WorkerPool::Result& r) {
            WorkerPool::Result kept = r;
            r.file.clear();  // kept for Python, gone with the pool's directory
            keptResults()[raw][r.id] = std::move(kept);
        };
        return Py::Long(p.submit(std::move(job)));
    }

    Py::Object claim(const Py::Tuple& args)
    {
        long id = 0;
        if (!PyArg_ParseTuple(args.ptr(), "l", &id)) {
            throw Py::Exception();
        }
        switch (pool().claim(id)) {
            case WorkerPool::Claim::Yours:
                return Py::String("Yours");
            case WorkerPool::Claim::Done:
                return Py::String("Done");
            case WorkerPool::Claim::Failed:
                return Py::String("Failed");
            default:
                return Py::String("Unknown");
        }
    }

    Py::Object state(const Py::Tuple& args)
    {
        long id = 0;
        if (!PyArg_ParseTuple(args.ptr(), "l", &id)) {
            throw Py::Exception();
        }
        switch (pool().state(id)) {
            case WorkerPool::State::Queued:
                return Py::String("Queued");
            case WorkerPool::State::Running:
                return Py::String("Running");
            default:
                return Py::String("Unknown");
        }
    }

    Py::Object result(const Py::Tuple& args)
    {
        long id = 0;
        if (!PyArg_ParseTuple(args.ptr(), "l", &id)) {
            throw Py::Exception();
        }
        auto& kept = keptResults()[&pool()];
        auto it = kept.find(id);
        if (it == kept.end()) {
            return Py::None();
        }
        auto& r = it->second;
        Py::Dict dict;
        dict.setItem("Id", Py::Long(r.id));
        dict.setItem("Kind", Py::String(r.kind));
        dict.setItem("Tag", Py::String(r.tag));
        dict.setItem("Status", Py::String(statusName(r.status)));
        dict.setItem("Error", Py::String(r.error));
        dict.setItem("Died", Py::Boolean(r.died));
        Py::List diedAt;
        for (auto& where : r.diedAt) {
            diedAt.append(Py::String(where));
        }
        dict.setItem("DiedAt", diedAt);
        dict.setItem("How", Py::String(r.how));
        dict.setItem("Stack", Py::String(r.stack));
        dict.setItem("CrashLog", Py::String(r.crashLog));
        Py::List messages;
        for (auto& m : r.messages) {
            Py::Dict item;
            item.setItem("Level", Py::String(levelName(m.level)));
            item.setItem("Notifier", Py::String(m.notifier));
            item.setItem("Text", Py::String(m.text));
            item.setItem("Worker", Py::Long(m.worker));
            messages.append(item);
        }
        dict.setItem("Messages", messages);
        dict.setItem("Output", fromJson(r.output));
        dict.setItem("File", Py::String(r.file));
        dict.setItem("Seconds", Py::Float(r.seconds));
        dict.setItem("Resident", Py::Long(static_cast<unsigned long>(r.resident)));
        kept.erase(it);
        return dict;
    }

    Py::Object cancel(const Py::Tuple& args)
    {
        const char* tag = "";
        if (!PyArg_ParseTuple(args.ptr(), "s", &tag)) {
            throw Py::Exception();
        }
        std::string which(tag);
        return Py::Long(static_cast<unsigned long>(
            pool().cancel([&which](const std::string& t) { return t == which; })));
    }

    Py::Object stop(const Py::Tuple& args)
    {
        const char* why = "stopped";
        if (!PyArg_ParseTuple(args.ptr(), "|s", &why)) {
            throw Py::Exception();
        }
        pool().stop(why);
        return Py::None();
    }

    Py::Object poll(const Py::Tuple& args)
    {
        int ms = 0;
        if (!PyArg_ParseTuple(args.ptr(), "|i", &ms)) {
            throw Py::Exception();
        }
        pool().poll(ms);
        return Py::None();
    }

    Py::Object wait(const Py::Tuple& args)
    {
        double seconds = -1;
        if (!PyArg_ParseTuple(args.ptr(), "|d", &seconds)) {
            throw Py::Exception();
        }
        // The GIL is kept: a job's client may be Python
        return Py::Boolean(pool().wait(seconds));
    }

    Py::Object info(const Py::Tuple&)
    {
        WorkerPool& p = pool();
        WorkerPool::Info i = p.info();
        Py::Dict dict;
        dict.setItem("Started", Py::Boolean(p.started()));
        dict.setItem("Workers", Py::Long(i.workers));
        dict.setItem("Limit", Py::Long(i.limit));
        dict.setItem("Spawned", Py::Long(i.spawned));
        dict.setItem("Busy", Py::Long(i.busy));
        dict.setItem("Queued", Py::Long(static_cast<unsigned long>(i.queued)));
        dict.setItem("Resident", Py::Long(static_cast<unsigned long>(i.resident)));
        dict.setItem("Done", Py::Long(i.done));
        dict.setItem("Failed", Py::Long(i.failed));
        dict.setItem("Reason", Py::String(i.reason));
        dict.setItem("Directory", Py::String(i.directory));
        dict.setItem("Copy", Py::String(i.copy));
        dict.setItem("Version", Py::Long(static_cast<long>(i.version)));
        dict.setItem("Stale", Py::Boolean(i.stale));
        dict.setItem("Moves", Py::Long(i.moves));
        dict.setItem("IdleCopies", Py::Long(i.idleCopies));
        dict.setItem("Reopens", Py::Long(i.reopens));
        dict.setItem("Ended", Py::Long(i.ended));
        dict.setItem("Kept", Py::Long(i.kept));
        dict.setItem("StartSeconds", Py::Float(i.startSeconds));
        dict.setItem("EndAfter", Py::Float(i.endAfter));
        dict.setItem("Leases", Py::Long(i.leases));
        dict.setItem("LeasesAsked", Py::Long(i.leasesAsked));
        dict.setItem("ReadStore", Py::Boolean(i.readStore));
        dict.setItem("Blobs", Py::String(i.blobs));
        Py::List pids;
        for (long pid : i.pids) {
            pids.append(Py::Long(pid));
        }
        dict.setItem("Pids", pids);
        return dict;
    }

    Document& document()
    {
        auto doc = GetApplication().getDocument(name.c_str());
        if (!doc) {
            throw Py::RuntimeError("the document is gone");
        }
        return *doc;
    }

    Py::Object recompute(const Py::Tuple& args)
    {
        PyObject* objs = Py_None;
        if (!PyArg_ParseTuple(args.ptr(), "|O", &objs)) {
            throw Py::Exception();
        }
        Document& doc = document();
        std::vector<DocumentObject*> list;
        if (objs != Py_None) {
            Py::Sequence seq(objs);
            for (Py::Sequence::size_type i = 0; i < seq.size(); ++i) {
                Py::Object item(seq[i]);
                if (!PyObject_TypeCheck(item.ptr(), &DocumentObjectPy::Type)) {
                    throw Py::TypeError("expected document objects");
                }
                list.push_back(static_cast<DocumentObjectPy*>(item.ptr())->getDocumentObjectPtr());
            }
        }
        return Py::Boolean(WorkerRecompute::start(doc, list));
    }

    Py::Object recomputing(const Py::Tuple&)
    {
        return Py::Boolean(WorkerRecompute::running(document()));
    }

    Py::Object waitRecompute(const Py::Tuple& args)
    {
        double seconds = -1;
        if (!PyArg_ParseTuple(args.ptr(), "|d", &seconds)) {
            throw Py::Exception();
        }
        return Py::Boolean(WorkerRecompute::wait(document(), seconds));
    }

    Py::Object cancelRecompute(const Py::Tuple& args)
    {
        const char* why = "cancelled";
        if (!PyArg_ParseTuple(args.ptr(), "|s", &why)) {
            throw Py::Exception();
        }
        WorkerRecompute::cancel(document(), why);
        return Py::None();
    }

    Py::Object expectedRecompute(const Py::Tuple& args)
    {
        WorkerRecompute::trackTimes();
        // Of what is touched, or of the objects given and what depends on them
        std::vector<DocumentObject*> objs;
        if (args.size() > 0) {
            Py::Sequence list(args[0]);
            for (Py_ssize_t k = 0; k < list.size(); ++k) {
                Py::Object item = list[k];
                if (!PyObject_TypeCheck(item.ptr(), &DocumentObjectPy::Type)) {
                    throw Py::TypeError("expectedRecompute takes a list of document objects");
                }
                objs.push_back(static_cast<DocumentObjectPy*>(item.ptr())->getDocumentObjectPtr());
            }
        }
        long unknown = 0;
        const double seconds = WorkerRecompute::expectedSeconds(document(), objs, &unknown);
        return Py::TupleN(Py::Float(seconds), Py::Long(unknown));
    }

    Py::Object recomputeInfo(const Py::Tuple&)
    {
        WorkerRecompute::Info i = WorkerRecompute::info(document());
        Py::Dict dict;
        dict.setItem("Running", Py::Boolean(i.running));
        dict.setItem("Started", Py::Long(i.started));
        dict.setItem("Applied", Py::Long(i.applied));
        dict.setItem("Borrowed", Py::Long(i.borrowed));
        dict.setItem("Cancelled", Py::Long(i.cancelled));
        dict.setItem("Failed", Py::Long(i.failed));
        dict.setItem("Unfit", Py::Long(i.unfit));
        dict.setItem("Objects", Py::Long(i.objects));
        dict.setItem("Supplied", Py::Long(i.supplied));
        dict.setItem("ExecutedHere", Py::Long(i.executedHere));
        dict.setItem("MadeHere", Py::Long(i.madeHere));
        dict.setItem("JoinedHere", Py::Long(i.joinedHere));
        dict.setItem("Unused", Py::Long(i.unused));
        dict.setItem("Inner", Py::Long(i.inner));
        Py::List here;
        for (auto& n : i.executedHereNames) {
            here.append(Py::String(n));
        }
        dict.setItem("ExecutedHereNames", here);
        dict.setItem("ById", Py::Boolean(i.byId));
        dict.setItem("ByContentWhy", Py::String(i.byContentWhy));
        dict.setItem("WorkerSeconds", Py::Float(i.workerSeconds));
        dict.setItem("ApplySeconds", Py::Float(i.applySeconds));
        Py::Dict times;
        times.setItem("Start", Py::Float(i.times.start));
        times.setItem("Queued", Py::Float(i.times.queued));
        times.setItem("Setup", Py::Float(i.times.setup));
        times.setItem("Job", Py::Float(i.times.job));
        times.setItem("Returned", Py::Float(i.times.returned));
        times.setItem("Held", Py::Float(i.times.held));
        times.setItem("Read", Py::Float(i.times.read));
        times.setItem("Apply", Py::Float(i.times.apply));
        times.setItem("Total", Py::Float(i.times.total));
        dict.setItem("Times", times);
        dict.setItem("Bytes", Py::Long(static_cast<unsigned long>(i.bytes)));
        dict.setItem("Reason", Py::String(i.reason));
        Py::List crashed;
        for (auto& n : i.crashed) {
            crashed.append(Py::String(n));
        }
        dict.setItem("Crashed", crashed);
        Py::List risky;
        for (auto& n : i.risky) {
            risky.append(Py::String(n));
        }
        dict.setItem("Risky", risky);
        dict.setItem("HandedOver", Py::Long(i.handedOver));
        dict.setItem("Split", Py::Long(i.split));
        dict.setItem("SplitSaving", Py::Float(i.splitSaving));
        Py::Tuple parts(i.splitObjects.size());
        for (std::size_t k = 0; k < i.splitObjects.size(); ++k) {
            parts.setItem(k, Py::Long(i.splitObjects[k]));
        }
        dict.setItem("SplitObjects", parts);
        dict.setItem("SplitJoins", Py::Long(i.splitJoins));
        dict.setItem("SuppliedSecond", Py::Long(i.suppliedSecond));
        return dict;
    }

private:
    std::string name;
};

}  // namespace

PyObject* WorkerPool::pyObject(Document& doc)
{
    static bool ready = false;
    if (!ready) {
        ready = true;
        WorkerPoolPy::init_type();
    }
    WorkerPool::of(doc);
    return new WorkerPoolPy(doc.getName());
}
