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
// The pool says:   setup {document, proxies}   open the copy
//                  job {job, kind, inputs, module, out}
//                  quit
// A worker says:   hello {token, pid}
//                  ready {resident} | unfit {error}   to the setup
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
#include <thread>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QThread>
#include <QTimer>

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
#include "FileBlobManager.h"
#include "FileHistory.h"
#include "PropertyPythonObject.h"
#include "StringHasher.h"
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
        double idleSince = 0;   ///< when it last had nothing, 0 while busy
        bool ready = false;     ///< the copy is open
        bool quitting = false;  ///< told there is no more
        bool over = false;      ///< finished() ran
        long made = 0;
        std::size_t resident = 0;
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
    };
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
    std::unique_ptr<QLocalServer> server;
    std::string token;
    std::string dir;   ///< the pool's: the copy, the settings, logs, results
    std::string root;  ///< the pool's directory: the copies, and the blobs put out
    std::string blobs;  ///< root/blobs: what the store keeps in files, by content
    std::string copy;  ///< the document's copy, empty when none stands
    std::string store;  ///< the store the workers read the version from, if they do
    std::weak_ptr<TransactionLogCore> leased;  ///< the log holding `version` for them
    int64_t version = 0;  ///< the log's version the copy is, 0 for a saved copy
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

    double meanCost(const std::string& kind) const
    {
        auto it = costs.find(kind);
        return it == costs.end() || !it->second.second
            ? 0.0
            : it->second.first / static_cast<double>(it->second.second);
    }

    bool listen();
    bool takeCopy();
    bool exportBlobs(TransactionLogCore& core, int64_t num);
    void dropCopy();
    bool spawn();
    void accept();
    void readStranger(Stranger& s);
    void read(Proc& proc);
    void handle(Proc& proc, const json& header);
    void finished(Proc& proc);
    void dispatch(Proc& proc);
    void grow();
    void deliver(JobId id, Result& result);
    void checkIdle();
    void kill(Proc& proc);
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

bool WorkerPool::Private::takeCopy()
{
    if (!copy.empty()) {
        return true;
    }
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
        // blobs put out for the workers, by content
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
    static int copies = 0;
    dir = root + "/copy-" + std::to_string(++copies);
    Base::FileInfo info(dir);
    if (info.exists()) {
        info.deleteDirectoryRecursive();
    }
    if (!Base::FileInfo(dir + "/out").createDirectories()) {
        reason = "cannot make " + dir;
        return false;
    }
    std::string file;
    std::string failure;
    version = 0;
    store.clear();
    {
        Base::FlagToggler<bool> flag(copying);
        // A version of the log (docs/WorkerProcess.md sec 3): the design as
        // a save serialises it, with no save -- nothing put back, nothing
        // recomputed, no archive -- written out as an unpacked project with
        // the file's strings, which a version does not carry
        if (params()->GetBool("FromLog", true)) {
            try {
                version = doc._snapshotToLog("workers");
                if (version > 0) {
                    auto& core = TransactionLogCore::of(doc.getFileHistory());
                    core.flush();
                    if (params()->GetBool("ReadStore", true) && exportBlobs(core, version)) {
                        // The workers read the version from this process's
                        // store themselves (sec 3): only the blobs it keeps
                        // in files of its own are put out, once each
                        store = doc.getFileHistory().directory() + "/history/log.db";
                        // Held while the copy stands: a worker started
                        // later reads it too
                        core.holdVersion(version, true);
                        leased = doc.getFileHistory().logCore();
                    }
                }
                if (version > 0 && store.empty()) {
                    auto& core = TransactionLogCore::of(doc.getFileHistory());
                    const std::string where = dir + "/version";
                    Document::materialiseVersion(core, version, where, false);
                    StringHasherRef hasher = doc.getStringHasher();
                    Base::ofstream out(Base::FileInfo(where + "/" + Document::stringTableName()),
                                       std::ios::out | std::ios::binary);
                    const std::string table = hasher ? hasher->saveTable() : std::string();
                    out.write(table.data(), static_cast<std::streamsize>(table.size()));
                    if (!out) {
                        THROWM(Base::FileException, "cannot write the string table");
                    }
                    file = where;
                }
            }
            catch (Base::Exception& e) {
                FC_LOG(doc.getName() << ": no version of the log for the workers: " << e.what());
                version = 0;
                store.clear();
            }
        }
        if (file.empty() && store.empty()) {
            // A transaction is open, or the log is off: a copy saved as a
            // save writes it, which puts an Animation's rest values back
            // and its pose again
            file = dir + "/Document.FCStd";
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
    if (file.empty() && store.empty()) {
        Base::FileInfo(dir).deleteDirectoryRecursive();
        reason = "the document could not be copied for the workers"
            + (failure.empty() ? std::string() : ": " + failure);
        return false;
    }
    // Its own copy of the settings: a console program writes them back as
    // it exits, which would race this one's
    GetApplication().GetUserParameter().SaveDocument((dir + "/user.cfg").c_str());
    GetApplication().GetSystemParameter().SaveDocument((dir + "/system.cfg").c_str());
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
    // A change of the design makes the copy stale. An Animation's own
    // properties are its business: it says when its frames change
    changed = doc.signalChangedObject.connect([this](const DocumentObject& obj,
                                                     const Property& prop) {
        if (copying || copy.empty() || doc.testStatus(Document::Recomputing)
            || (WriteScope::causes() & WriteCause::Animation)
            || obj.isDerivedFrom<Animation>()
            || !(doc.writeEffects(prop) & WriteEffect::Dirty)) {
            return;
        }
        pool.stop("the design changed");
    });
    return true;
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
    store.clear();
    changed.disconnect();
    copy.clear();
    proxies.clear();
    if (!dir.empty() && !params()->GetBool("KeepJob", false)) {
        Base::FileInfo info(dir);
        if (info.exists()) {
            info.deleteDirectoryRecursive();
        }
    }
    dir.clear();
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
        QString::fromStdString(dir + "/worker" + std::to_string(spawned) + ".log"));
    process->setStandardInputFile(QProcess::nullDevice());
    QStringList args;
    args << QStringLiteral("-u") << QString::fromStdString(dir + "/user.cfg")
         << QStringLiteral("-s") << QString::fromStdString(dir + "/system.cfg")
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
            // Gone without being told: what it was doing is not coming
            kill(*proc);
        }
    });
    json setup {{"type", "setup"}, {"proxies", proxies}};
    if (!store.empty()) {
        // A version read from this process's store, into a folder of its own
        setup["store"] = store;
        setup["version"] = version;
        setup["blobs"] = blobs;
        setup["document"] = dir + "/worker" + std::to_string(proc->number);
    }
    else {
        setup["document"] = copy;
    }
    send(*socket, setup);
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
        dispatch(proc);
        changedNotice();
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
        proc.resident = header.value("resident", std::size_t(0));
        resident = std::max(resident, proc.resident);
        ++proc.made;
        const std::string out = dir + "/out/" + std::to_string(id);
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
        result.resident = proc.resident;
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
    if (proc.over) {
        return;
    }
    if (proc.process && proc.process->state() != QProcess::NotRunning) {
        QObject::disconnect(proc.process.get(), nullptr, &context, nullptr);
        proc.process->kill();
        proc.process->waitForFinished(2000);
    }
    finished(proc);
}

void WorkerPool::Private::finished(Proc& proc)
{
    if (proc.over) {
        return;
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
    for (auto it = procs.begin(); it != procs.end(); ++it) {
        if (it->get() == &proc) {
            gone.push_back(std::move(*it));  // not deleted inside its own signal
            procs.erase(it);
            break;
        }
    }
    const bool quiet = proc.quitting;
    if (lost) {
        Base::FileInfo(dir + "/out/" + std::to_string(lost)).deleteFile();
        auto it = jobs.find(lost);
        if (it != jobs.end()) {
            it->second.running = false;
            if (it->second.dropped) {
                jobs.erase(it);
            }
            else if (++it->second.tries < 2) {
                // Once more, first in line: the death may not be the job's
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
                deliver(lost, result);
            }
        }
    }
    if (!quiet) {
        FC_LOG(doc.getName() << ": worker " << proc.number << " ended"
                             << (lost ? " in the middle of a job" : ""));
        if (++failedInRow >= 3) {
            pool.stop("the workers exited");
            return;
        }
    }
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
    while (!queue.empty()) {
        JobId id = queue.front();
        queue.pop_front();
        auto it = jobs.find(id);
        if (it == jobs.end() || it->second.running) {
            continue;
        }
        json inputs = json::parse(it->second.job.inputs, nullptr, false);
        if (inputs.is_discarded()) {
            Result result;
            result.id = id;
            result.kind = it->second.job.kind;
            result.tag = it->second.job.tag;
            result.error = "its inputs are not JSON";
            ++failed;
            deliver(id, result);
            continue;
        }
        json header {{"type", "job"},
                     {"job", id},
                     {"kind", it->second.job.kind},
                     {"inputs", inputs},
                     {"module", it->second.job.module},
                     {"out", dir + "/out/" + std::to_string(id)}};
        it->second.running = true;
        proc.job = id;
        proc.began = now();
        proc.idleSince = 0;
        if (!send(*proc.socket, header)) {
            kill(proc);
        }
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
    // is more queued than workers
    const bool consuming = now() - lastClaim < 2.0;
    if (consuming ? !starved : queue.size() <= procs.size()) {
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
    const JobId id = d->next++;
    Private::Entry entry;
    entry.job = std::move(job);
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

std::size_t WorkerPool::cancel(const std::function<bool(const std::string&)>& which)
{
    Private::Depth guard(*d, true);
    std::size_t count = 0;
    for (auto it = d->jobs.begin(); it != d->jobs.end();) {
        if (it->second.dropped || !which(it->second.job.tag)) {
            ++it;
            continue;
        }
        ++count;
        if (it->second.running) {
            it->second.dropped = true;
            it->second.job.done = nullptr;
            ++it;
        }
        else {
            const JobId id = it->first;
            it = d->jobs.erase(it);
            d->queue.erase(std::remove(d->queue.begin(), d->queue.end(), id), d->queue.end());
        }
    }
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
    info.directory = d->dir;
    info.version = d->version;
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
    std::string buffer;
    Document* doc = nullptr;
    // The pool's process's store, read for the version (and later to move
    // along its log, sec 4)
    std::shared_ptr<FileHistory> reader;
    for (;;) {
        json header;
        std::string body;
        int got = 0;
        while ((got = take(buffer, header, body)) == 0) {
            if (socket.bytesAvailable() == 0) {
                if (socket.state() != QLocalSocket::ConnectedState) {
                    got = -2;
                    break;
                }
                if (!socket.waitForReadyRead(1000)) {
                    continue;
                }
            }
            QByteArray bytes = socket.readAll();
            buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        }
        if (got < 0) {
            break;  // the pool has gone, or said what is not a message
        }
        const std::string type = header.value("type", "");
        if (type == "quit") {
            break;
        }
        if (type == "setup") {
            std::string why;
            try {
                const std::string file = header.value("document", std::string());
                const std::string store = header.value("store", std::string());
                if (!store.empty()) {
                    // The version read from the pool's process's store
                    // (docs/WorkerProcess.md sec 3), written out here as the
                    // unpacked project a folder restore reads, with the
                    // store's strings
                    const int64_t num = header.value("version", int64_t(0));
                    std::string failed;
                    reader = FileHistory::openReader(store, header.value("blobs", std::string()),
                                                     &failed);
                    if (!reader) {
                        THROWM(Base::RuntimeError, failed);
                    }
                    TransactionLogCore& core = *reader->logCore();
                    Document::materialiseVersion(core, num, file, false);
                    const std::string table =
                        reader->hasher() ? reader->hasher()->saveTable() : std::string();
                    Base::ofstream out(Base::FileInfo(file + "/" + Document::stringTableName()),
                                       std::ios::out | std::ios::binary);
                    out.write(table.data(), static_cast<std::streamsize>(table.size()));
                    if (!out) {
                        THROWM(Base::FileException, "cannot write the string table");
                    }
                }
                doc = GetApplication().openDocument(file.c_str(), false);
                if (!doc) {
                    why = "cannot open " + file;
                }
                else {
                    why = unfitFor(*doc, header.value("proxies", json::array()));
                    // Nothing is undone here
                    doc->setUndoMode(0);
                }
            }
            catch (Base::Exception& e) {
                why = std::string("cannot open the document: ") + e.what();
            }
            if (why.empty()) {
                send(socket, json {{"type", "ready"}, {"resident", residentBytes()}});
            }
            else {
                send(socket, json {{"type", "unfit"}, {"error", why}});
                break;
            }
            continue;
        }
        if (type != "job") {
            continue;
        }
        Request request;
        request.kind = header.value("kind", std::string());
        auto inputs = header.find("inputs");
        request.inputs = inputs != header.end() ? inputs->dump() : std::string("{}");
        request.out = header.value("out", std::string());
        request.document = doc;
        const double began = now();
        Reply reply;
        auto handler = kinds().find(request.kind);
        try {
            if (handler != kinds().end()) {
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
        if (!send(socket, answer)) {
            break;
        }
    }
    if (doc) {
        GetApplication().closeDocument(doc->getName());
    }
    socket.disconnectFromServer();
    return 0;
}

// --------------------------------------------------------------------------
// Python: FreeCAD.workerPool(doc)

namespace
{

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
                           "submit(kind, inputs={}, module='', tag='') -> job id, 0 if not started");
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
        static const std::array<const char*, 5> keywords {"kind", "inputs", "module", "tag",
                                                          nullptr};
        const char* kind = nullptr;
        PyObject* inputs = nullptr;
        const char* module = "";
        const char* tag = "";
        if (!Base::Wrapped_ParseTupleAndKeywords(args.ptr(), kwds.ptr(), "s|Oss", keywords, &kind,
                                                 &inputs, &module, &tag)) {
            throw Py::Exception();
        }
        WorkerPool& p = pool();
        WorkerPool::Job job;
        job.kind = kind;
        job.inputs = inputs ? toJson(Py::Object(inputs)) : std::string("{}");
        job.module = module;
        job.tag = tag;
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
        dict.setItem("Version", Py::Long(static_cast<long>(i.version)));
        dict.setItem("ReadStore", Py::Boolean(i.readStore));
        dict.setItem("Blobs", Py::String(i.blobs));
        Py::List pids;
        for (long pid : i.pids) {
            pids.append(Py::Long(pid));
        }
        dict.setItem("Pids", pids);
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
