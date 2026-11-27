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

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QThread>

#include <nlohmann/json.hpp>

#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Interpreter.h>
#include <Base/Parameter.h>

#include "Application.h"
#include "ServiceWorkers.h"
#include "WorkerCrash.h"

FC_LOG_LEVEL_INIT("ServiceWorkers", true, true)

using namespace App;
using json = nlohmann::json;

namespace
{

constexpr const char* ServerVariable = "THUNDEREAL_SERVICE_SERVER";
constexpr const char* TokenVariable = "THUNDEREAL_SERVICE_TOKEN";

ParameterGrp::handle params()
{
    return GetApplication().GetParameterGroupByPath("User parameter:BaseApp/Preferences/Workers");
}

double now()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

/// The console program the helpers run: the pool's own (WorkerPool.cpp's
/// workerProgram), Workers/Command if set
std::string program()
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

void ensureCore()
{
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char name[] = "ThunderealCmd";
        static char* argv[] = {name, nullptr};
        new QCoreApplication(argc, argv);  // NOLINT: lives as long as the process
    }
}

std::string randomToken()
{
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    static const char hex[] = "0123456789abcdef";
    std::string token;
    for (int i = 0; i < 32; ++i) {
        token += hex[gen() & 15];
    }
    return token;
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

/// A message: the header's length, the body's, the header (JSON), the body
QByteArray message(const json& header, const std::string& body = {})
{
    const std::string head = header.dump();
    QByteArray out;
    out.reserve(static_cast<int>(8 + head.size() + body.size()));
    put32(out, static_cast<std::uint32_t>(head.size()));
    put32(out, static_cast<std::uint32_t>(body.size()));
    out.append(head.data(), static_cast<int>(head.size()));
    out.append(body.data(), static_cast<int>(body.size()));
    return out;
}

/// 1 a whole message taken off `buffer`, 0 not yet, -1 a broken one
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

bool send(QLocalSocket& socket, const QByteArray& bytes)
{
    if (socket.state() != QLocalSocket::ConnectedState) {
        return false;
    }
    if (socket.write(bytes) != bytes.size()) {
        return false;
    }
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(1000)
            && socket.state() != QLocalSocket::ConnectedState) {
            return false;
        }
    }
    return true;
}

enum class Got
{
    Message,
    Broken,
    Gone,     ///< the other end closed
    Timeout,
};

/// The next message from `socket`, waited for until `deadline` (< 0: none)
Got receive(QLocalSocket& socket, std::string& buffer, json& header, std::string& body,
            double deadline, const std::function<bool()>& alive = {})
{
    for (;;) {
        QByteArray bytes = socket.readAll();
        buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        const int got = take(buffer, header, body);
        if (got > 0) {
            return Got::Message;
        }
        if (got < 0) {
            return Got::Broken;
        }
        if (socket.state() != QLocalSocket::ConnectedState && socket.bytesAvailable() == 0) {
            return Got::Gone;
        }
        if (alive && !alive()) {
            // Gone without its socket said closed yet: what it wrote is read
            socket.waitForReadyRead(50);
            bytes = socket.readAll();
            buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
            return take(buffer, header, body) > 0 ? Got::Message : Got::Gone;
        }
        if (deadline >= 0 && now() > deadline) {
            return Got::Timeout;
        }
        socket.waitForReadyRead(100);
    }
}

// --- the helper's side

std::mutex& handlersLock()
{
    static std::mutex m;
    return m;
}

std::map<std::string, ServiceWorkers::Handler>& handlers()
{
    static std::map<std::string, ServiceWorkers::Handler> h;
    return h;
}

// --- the client's side

struct Call
{
    std::string kind;
    std::string module;
    std::string request;
    double timeout = 0;
    ServiceWorkers::Reply reply;
    bool done = false;
};

class Service
{
public:
    static Service& instance()
    {
        // Never destroyed: a call can be made while the process exits
        static Service* s = new Service;
        return *s;
    }

    ServiceWorkers::Reply call(const std::string& kind, const std::string& module,
                               const std::string& request, double timeout)
    {
        auto c = std::make_shared<Call>();
        c->kind = kind;
        c->module = module;
        c->request = request;
        c->timeout = timeout;
        const double began = now();
        std::unique_lock<std::mutex> lock(m);
        if (!prepared) {
            prepare(lock);
        }
        queue.push_back(c);
        int idle = 0;
        for (auto& o : owners) {
            idle += o->busy ? 0 : 1;
        }
        const int limit = helperLimit();
        if (static_cast<int>(queue.size()) > idle && static_cast<int>(owners.size()) < limit) {
            startOwner();
        }
        cv.notify_all();
        done.wait(lock, [&] { return c->done; });
        c->reply.seconds = now() - began;
        ++calls;
        seconds += c->reply.seconds;
        handler += c->reply.handler;
        died += c->reply.died ? 1 : 0;
        return c->reply;
    }

    void stop()
    {
        std::vector<std::unique_ptr<Owner>> ending;
        {
            std::lock_guard<std::mutex> lock(m);
            stopping = true;
            ending.swap(owners);
            cv.notify_all();
        }
        for (auto& o : ending) {
            if (o->thread) {
                o->thread->wait(10000);
                delete o->thread;
            }
        }
        std::lock_guard<std::mutex> lock(m);
        stopping = false;
        // The logs are kept where a helper died: the error names them
        if (prepared && died == 0 && !root.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::u8path(root), ec);
            prepared = false;
        }
        // Calls made while stopping are told so (owners were taken)
        for (auto& c : queue) {
            c->reply.error = "the helpers were stopped";
            c->done = true;
        }
        queue.clear();
        done.notify_all();
    }

    /** How many helpers at most: ServiceWorkers of Preferences/Workers, or
     * with 0, its default, one for each thread the machine runs at once, 2
     * at least. Two were the most for a load's pre-mesh, which made a
     * document's meshes 3.3 times slower than on this process's threads
     * (docs/WorkerProcess.md sec 71): the threads it posts to each block
     * in call() for one of the two
     */
    static int helperLimit()
    {
        const long set = params()->GetInt("ServiceWorkers", 0);
        if (set > 0) {
            return static_cast<int>(set);
        }
        return std::max(2, QThread::idealThreadCount());
    }

    /// The helpers kept however long they are idle: ServiceWorkersKept (2);
    /// one past them ends its process once idle for ServiceIdleSeconds (30)
    static int helpersKept()
    {
        return static_cast<int>(std::max(0L, params()->GetInt("ServiceWorkersKept", 2)));
    }

    ServiceWorkers::Info info()
    {
        std::lock_guard<std::mutex> lock(m);
        ServiceWorkers::Info i;
        i.limit = helperLimit();
        i.idleEnded = idleEnded;
        for (auto& o : owners) {
            if (o->pid > 0) {
                ++i.helpers;
                i.pids.push_back(o->pid);
            }
        }
        i.spawned = spawned;
        i.calls = calls;
        i.died = died;
        i.seconds = seconds;
        i.handler = handler;
        i.sent = sent;
        i.received = received;
        i.directory = root;
        return i;
    }

private:
    struct Owner
    {
        QThread* thread = nullptr;
        bool busy = false;
        long pid = 0;
        int number = 0;
    };

    /// Under the lock: the directory and the configuration the helpers read
    void prepare(std::unique_lock<std::mutex>&)
    {
        prepared = true;
        ensureCore();
        const std::string temp = Application::getTempPath();
        sweep(temp);
        root = temp + "ThunderealService-" + std::to_string(QCoreApplication::applicationPid());
        Base::FileInfo(root).createDirectories();
        // Their own copies: a helper saving its parameters at its exit
        // must not write over this process's
        GetApplication().GetUserParameter().SaveDocument((root + "/user.cfg").c_str());
        GetApplication().GetSystemParameter().SaveDocument((root + "/system.cfg").c_str());
        if (auto app = QCoreApplication::instance()) {
            QObject::connect(app, &QCoreApplication::aboutToQuit, [] {
                Service::instance().stop();
            });
        }
    }

    /// The directories of processes that are gone, a day after they went
    static void sweep(const std::string& temp)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string prefix = "ThunderealService-";
        for (const auto& entry : fs::directory_iterator(fs::u8path(temp), ec)) {
            const std::string name = entry.path().filename().string();
            if (name.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            const auto age = fs::file_time_type::clock::now() - fs::last_write_time(entry, ec);
            if (ec || age < std::chrono::hours(24)) {
                continue;
            }
            fs::remove_all(entry.path(), ec);
        }
    }

    void startOwner()
    {
        auto o = std::make_unique<Owner>();
        o->number = static_cast<int>(owners.size());
        Owner* raw = o.get();
        o->thread = QThread::create([this, raw] { run(*raw); });
        owners.push_back(std::move(o));
        raw->thread->start();
    }

    struct Helper
    {
        std::unique_ptr<QLocalServer> server;
        std::unique_ptr<QProcess> process;
        std::unique_ptr<QLocalSocket> socket;
        std::string buffer;
        std::string log;
        std::set<std::string> kinds;  ///< imported for
    };

    bool start(Owner& me, Helper& h, std::string& why)
    {
        const std::string exe = program();
        if (exe.empty()) {
            why = "no console program found to run the helpers";
            return false;
        }
        std::string token = randomToken();
        int number = 0;
        {
            std::lock_guard<std::mutex> lock(m);
            number = spawned++;
        }
        h = Helper();
        h.server = std::make_unique<QLocalServer>();
        h.server->setSocketOptions(QLocalServer::UserAccessOption);
        const QString name = QStringLiteral("thundereal-service-%1-%2-%3")
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(number)
                                 .arg(QString::fromStdString(token.substr(0, 8)));
        QLocalServer::removeServer(name);
        if (!h.server->listen(name)) {
            why = "cannot listen for a helper: " + h.server->errorString().toStdString();
            return false;
        }
        h.log = root + "/helper" + std::to_string(number) + ".log";
        h.process = std::make_unique<QProcess>();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QString::fromLatin1(ServerVariable), h.server->fullServerName());
        env.insert(QString::fromLatin1(TokenVariable), QString::fromStdString(token));
        h.process->setProcessEnvironment(env);
        h.process->setProcessChannelMode(QProcess::MergedChannels);
        h.process->setStandardOutputFile(QString::fromStdString(h.log));
        h.process->setStandardInputFile(QProcess::nullDevice());
        QStringList args;
        args << QStringLiteral("-u") << QString::fromStdString(root + "/user.cfg")
             << QStringLiteral("-s") << QString::fromStdString(root + "/system.cfg")
             << QStringLiteral("-c") << QStringLiteral("import FreeCAD; FreeCAD._serveService()");
        h.process->start(QString::fromStdString(exe), args);
        if (!h.process->waitForStarted(10000)) {
            why = "cannot start " + exe;
            return false;
        }
        const double deadline = now() + 60;
        while (!h.server->hasPendingConnections()) {
            if (h.process->state() == QProcess::NotRunning) {
                why = "the helper ended before it connected; its log: " + h.log;
                return false;
            }
            if (now() > deadline) {
                h.process->kill();
                h.process->waitForFinished(2000);
                why = "the helper did not connect in 60 s; its log: " + h.log;
                return false;
            }
            h.server->waitForNewConnection(100);
        }
        h.socket.reset(h.server->nextPendingConnection());
        h.socket->setParent(nullptr);
        json header;
        std::string body;
        if (receive(*h.socket, h.buffer, header, body, now() + 10) != Got::Message
            || header.value("token", "") != token) {
            h.process->kill();
            h.process->waitForFinished(2000);
            why = "the helper did not say who it is";
            return false;
        }
        std::lock_guard<std::mutex> lock(m);
        me.pid = static_cast<long>(h.process->processId());
        return true;
    }

    /// How a helper that is gone ended, and what its log says of a crash
    std::string howEnded(Helper& h)
    {
        std::string how;
        if (h.process) {
            if (h.process->state() != QProcess::NotRunning) {
                h.process->waitForFinished(2000);
            }
            if (h.process->state() != QProcess::NotRunning) {
                how = "did not end";
            }
            else if (h.process->exitStatus() == QProcess::CrashExit) {
                how = "crashed";
            }
            else {
                how = "exited with code " + std::to_string(h.process->exitCode());
            }
        }
        WorkerCrash::Report report = WorkerCrash::read(h.log);
        if (report.found) {
            how += " (" + WorkerCrash::what(report) + ")";
            FC_ERR("a helper crashed; its log: " << h.log << "\n" << WorkerCrash::text(report));
        }
        return how;
    }

    void finish(Owner& me, const std::shared_ptr<Call>& c)
    {
        std::lock_guard<std::mutex> lock(m);
        c->done = true;
        me.busy = false;
        done.notify_all();
    }

    void run(Owner& me)
    {
        Helper h;
        for (;;) {
            std::shared_ptr<Call> c;
            {
                std::unique_lock<std::mutex> lock(m);
                auto wanted = [&] { return stopping || !queue.empty(); };
                if (h.socket && me.number >= helpersKept()) {
                    // One past those kept: its process ends once idle
                    // long enough, and is started again by the next call
                    // that finds the others busy
                    const double idle = std::max(0.0, params()->GetFloat("ServiceIdleSeconds", 30.0));
                    if (!cv.wait_for(lock, std::chrono::duration<double>(idle), wanted)) {
                        lock.unlock();
                        end(h);
                        h = Helper();
                        lock.lock();
                        me.pid = 0;
                        ++idleEnded;
                        continue;
                    }
                }
                else {
                    cv.wait(lock, wanted);
                }
                if (stopping) {
                    break;
                }
                c = queue.front();
                queue.pop_front();
                me.busy = true;
            }
            const double began = now();
            if (!h.socket || h.socket->state() != QLocalSocket::ConnectedState) {
                std::string why;
                if (!start(me, h, why)) {
                    c->reply.error = why;
                    h = Helper();
                    {
                        std::lock_guard<std::mutex> lock(m);
                        me.pid = 0;
                    }
                    finish(me, c);
                    continue;
                }
                c->reply.started = now() - began;
            }
            json head {{"kind", c->kind}, {"module", c->module}};
            const QByteArray bytes = message(head, c->request);
            {
                std::lock_guard<std::mutex> lock(m);
                sent += static_cast<std::size_t>(bytes.size());
            }
            json header;
            std::string body;
            Got got = Got::Gone;
            if (send(*h.socket, bytes)) {
                QProcess* process = h.process.get();
                got = receive(*h.socket, h.buffer, header, body, now() + c->timeout, [process] {
                    return process->state() != QProcess::NotRunning;
                });
            }
            if (got == Got::Message) {
                c->reply.ok = header.value("ok", false);
                c->reply.error = header.value("error", "");
                c->reply.handler = header.value("seconds", 0.0);
                c->reply.body = std::move(body);
                std::lock_guard<std::mutex> lock(m);
                received += c->reply.body.size();
            }
            else {
                if (got == Got::Timeout || got == Got::Broken) {
                    h.process->kill();
                }
                c->reply.died = true;
                c->reply.error = (got == Got::Timeout
                                      ? "the helper ran past " + std::to_string(int(c->timeout))
                                          + " s and was ended"
                                      : "the helper " + howEnded(h))
                    + " making " + c->kind;
                h = Helper();
                std::lock_guard<std::mutex> lock(m);
                me.pid = 0;
            }
            finish(me, c);
        }
        end(h);  // told to stop
    }

    /// `h`'s process ended: it ends when its socket closes
    static void end(Helper& h)
    {
        if (h.socket) {
            h.socket->disconnectFromServer();
        }
        if (h.process && !h.process->waitForFinished(3000)) {
            h.process->kill();
            h.process->waitForFinished(2000);
        }
    }

    std::mutex m;
    std::condition_variable cv;    ///< a call queued, or stop
    std::condition_variable done;  ///< a call answered
    std::deque<std::shared_ptr<Call>> queue;
    std::vector<std::unique_ptr<Owner>> owners;
    bool stopping = false;
    bool prepared = false;
    std::string root;
    int spawned = 0;
    long idleEnded = 0;
    long calls = 0;
    long died = 0;
    double seconds = 0;
    double handler = 0;
    std::size_t sent = 0;
    std::size_t received = 0;
};

}  // namespace

void ServiceWorkers::registerKind(const std::string& kind, Handler handler)
{
    std::lock_guard<std::mutex> lock(handlersLock());
    handlers()[kind] = std::move(handler);
}

ServiceWorkers::Reply ServiceWorkers::call(const std::string& kind, const std::string& module,
                                           const std::string& request, double timeoutSeconds)
{
    return Service::instance().call(kind, module, request, timeoutSeconds);
}

ServiceWorkers::Info ServiceWorkers::info()
{
    return Service::instance().info();
}

void ServiceWorkers::stop()
{
    Service::instance().stop();
}

int ServiceWorkers::serve()
{
    WorkerCrash::install();
    ensureCore();
    const QByteArray server = qgetenv(ServerVariable);
    const QByteArray token = qgetenv(TokenVariable);
    if (server.isEmpty()) {
        Base::Console().Error("_serveService: not started as a helper\n");
        return 2;
    }
    QLocalSocket socket;
    socket.connectToServer(QString::fromUtf8(server));
    if (!socket.waitForConnected(10000)) {
        Base::Console().Error("_serveService: cannot connect\n");
        return 2;
    }
    json hello {{"type", "hello"},
                {"token", token.toStdString()},
                {"pid", QCoreApplication::applicationPid()}};
    if (!send(socket, message(hello))) {
        return 2;
    }
    std::string buffer;
    std::set<std::string> imported;
    for (;;) {
        json header;
        std::string body;
        const Got got = receive(socket, buffer, header, body, -1);
        if (got != Got::Message) {
            return got == Got::Gone ? 0 : 3;
        }
        const std::string kind = header.value("kind", "");
        const std::string module = header.value("module", "");
        Handler handler;
        {
            std::lock_guard<std::mutex> lock(handlersLock());
            auto it = handlers().find(kind);
            if (it != handlers().end()) {
                handler = it->second;
            }
        }
        if (!handler && !module.empty() && imported.insert(module).second) {
            try {
                Base::Interpreter().runString(("import " + module).c_str());
            }
            catch (Base::Exception& e) {
                e.ReportException();
            }
            std::lock_guard<std::mutex> lock(handlersLock());
            auto it = handlers().find(kind);
            if (it != handlers().end()) {
                handler = it->second;
            }
        }
        json reply {{"ok", false}};
        std::string out;
        const double began = now();
        if (!handler) {
            reply["error"] = "no handler of " + kind;
        }
        else {
            WorkerCrash::setAt(kind);
            std::string error;
            try {
                reply["ok"] = handler(body, out, error);
            }
            catch (Base::Exception& e) {
                error = e.what();
            }
            catch (std::exception& e) {
                error = e.what();
            }
            catch (...) {
                error = "unknown exception";
            }
            WorkerCrash::setAt({});
            if (!error.empty()) {
                reply["error"] = error;
            }
        }
        reply["seconds"] = now() - began;
        if (!send(socket, message(reply, out))) {
            return 0;
        }
    }
}
