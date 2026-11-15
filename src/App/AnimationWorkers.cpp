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

// The worker processes of an Animation (docs/Animation.md sec 4.2). Each is
// a ThunderealCmd running freecad.animationworker on a copy of the
// document: told a frame's number on its standard input, it makes the
// frame (Animation::writeFrame) and answers on its standard output, a line
// starting "FCAW ":
//
//   ready <resident>           the document is open
//   done <frame> <resident>    the frame is in its file
//   fail <frame> <why>         it could not be made
//   unfit <why>                the document cannot be worked on here
//
// "quit" on its input ends it. The frames go to the cache's directory, in
// the cache's own format with a header naming what each value is of, and
// come into the cache as frames on disk (Animation::adoptFrame).

#include "PreCompiled.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>

#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>

#if defined(__APPLE__)
# include <mach/mach.h>
# include <sys/sysctl.h>
#elif defined(_WIN32)
# include <windows.h>
#else
# include <unistd.h>
#endif

#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Parameter.h>

#include <Base/Interpreter.h>

#include "AnimationWorkers.h"
#include "Application.h"
#include "Document.h"
#include "DocumentObject.h"
#include "PropertyPythonObject.h"

FC_LOG_LEVEL_INIT("Animation", true, true)

using namespace App;

namespace
{

ParameterGrp::handle params()
{
    return GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Animation");
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

/// The console program beside this one: ThunderealCmd, or what
/// WorkerCommand of Preferences/Animation names
std::string workerProgram()
{
    std::string set = params()->GetASCII("WorkerCommand", "");
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

}  // namespace

Animation::Workers::Workers(Animation& owner)
    : owner(owner)
{}

Animation::Workers::~Workers()
{
    stop(std::string());
    gone.clear();
    cleanup();
}

bool Animation::Workers::start()
{
    program = workerProgram();
    if (program.empty()) {
        reason = "no console program found to run the workers";
        return false;
    }
    return spawn();
}

bool Animation::Workers::spawn()
{
    auto proc = std::make_unique<Proc>();
    auto process = std::make_unique<QProcess>();
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("THUNDEREAL_ANIMATION_JOB"), QString::fromStdString(job));
    process->setProcessEnvironment(env);
    // What it says on the side, kept with the job for when one fails
    process->setStandardErrorFile(
        QString::fromStdString(dir + "/worker" + std::to_string(spawned) + ".log"));
    QStringList args;
    // Its own copy of the settings: a console program writes them back as
    // it exits, which would race this one's
    args << QStringLiteral("-u") << QString::fromStdString(dir + "/user.cfg")
         << QStringLiteral("-s") << QString::fromStdString(dir + "/system.cfg")
         << QStringLiteral("-c")
         << QStringLiteral("from freecad import animationworker; animationworker.serve()");
    Proc* raw = proc.get();
    QObject::connect(process.get(), &QProcess::readyReadStandardOutput, &context,
                     [this, raw]() { read(*raw); });
    QObject::connect(process.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     &context, [this, raw](int, QProcess::ExitStatus) { finished(*raw); });
    process->start(QString::fromStdString(program), args);
    if (!process->waitForStarted(10000)) {
        reason = "cannot start " + program;
        QObject::disconnect(process.get(), nullptr, &context, nullptr);
        return false;
    }
    proc->process = std::move(process);
    procs.push_back(std::move(proc));
    ++spawned;
    return true;
}

void Animation::Workers::stop(const std::string& why)
{
    if (!why.empty() && (running || !procs.empty())) {
        reason = why;
    }
    running = false;
    for (auto& proc : procs) {
        if (proc->process) {
            QObject::disconnect(proc->process.get(), nullptr, &context, nullptr);
            if (proc->process->state() != QProcess::NotRunning) {
                proc->process->kill();
                proc->process->waitForFinished(2000);
            }
        }
        if (proc->frame >= 0) {
            // Begun, maybe written, not told: not to be taken
            Base::FileInfo(frameFile(proc->frame) + ".part").deleteFile();
            Base::FileInfo(frameFile(proc->frame)).deleteFile();
            proc->frame = -1;
        }
        gone.push_back(std::move(proc));
    }
    procs.clear();
    queue.clear();
    for (auto& [index, file] : arrived) {
        Base::FileInfo(file).deleteFile();
    }
    arrived.clear();
    cleanup();
}

void Animation::Workers::cleanup()
{
    // WorkerKeepJob keeps the job -- the copy, the settings, each worker's
    // log -- to see why one failed
    if (dir.empty() || !procs.empty() || params()->GetBool("WorkerKeepJob", false)) {
        return;
    }
    Base::FileInfo info(dir);
    if (info.exists()) {
        info.deleteDirectoryRecursive();
    }
}

void Animation::Workers::bury()
{
    gone.clear();
}

int Animation::Workers::alive() const
{
    return static_cast<int>(procs.size());
}

std::string Animation::Workers::frameFile(long index) const
{
    return frames + "/" + std::to_string(index) + ".wframe";
}

bool Animation::Workers::owns(long index) const
{
    if (std::find(queue.begin(), queue.end(), index) != queue.end()) {
        return true;
    }
    for (auto& proc : procs) {
        if (proc->frame == index) {
            return true;
        }
    }
    return false;
}

void Animation::Workers::enqueue(const std::vector<long>& indices, long from)
{
    std::vector<long> sorted(indices);
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    // From the frame on show on, then the ones before it
    auto mid = std::lower_bound(sorted.begin(), sorted.end(), from);
    std::rotate(sorted.begin(), mid, sorted.end());
    for (long index : sorted) {
        if (!owner._frames.count(index) && !owns(index)) {
            queue.push_back(index);
        }
    }
    for (auto& proc : procs) {
        dispatch(*proc);
    }
}

StringHasherRef Animation::Workers::table(const std::string& hash)
{
    auto it = loaded.find(hash);
    if (it != loaded.end()) {
        return it->second;
    }
    std::ifstream in(tables + "/" + hash + ".table", std::ios::binary);
    if (!in) {
        return {};
    }
    StringHasherRef hasher(new StringHasher);
    try {
        hasher->restoreTable(in);
    }
    catch (Base::Exception& e) {
        FC_WARN(owner.getFullName() << ": a worker's string table: " << e.what());
        return {};
    }
    loaded[hash] = hasher;
    return hasher;
}

void Animation::Workers::read(Proc& proc)
{
    // Until nothing is left: QProcess tells of new data once, and what it
    // took in while a line was being handled here -- a wait inside it --
    // would sit in its buffer untold, and a worker waiting for its next
    // frame with it
    while (proc.process && proc.process->bytesAvailable() > 0) {
        QByteArray bytes = proc.process->readAllStandardOutput();
        proc.buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        std::size_t end;
        while ((end = proc.buffer.find('\n')) != std::string::npos) {
            std::string text = proc.buffer.substr(0, end);
            proc.buffer.erase(0, end + 1);
            if (!text.empty() && text.back() == '\r') {
                text.pop_back();
            }
            // Anywhere in the line: what else a process prints may leave
            // a line open -- a progress count ends in a carriage return
            std::size_t at = text.rfind("FCAW ");
            if (at != std::string::npos) {
                line(proc, text.substr(at + 5));
            }
        }
    }
}

void Animation::Workers::line(Proc& proc, const std::string& text)
{
    if (!running) {
        return;
    }
    std::istringstream in(text);
    std::string what;
    in >> what;
    if (what == "ready") {
        in >> proc.resident;
        proc.ready = true;
        dispatch(proc);
    }
    else if (what == "done") {
        long index = -1;
        in >> index >> proc.resident;
        resident = std::max(resident, proc.resident);
        if (proc.frame == index) {
            proc.frame = -1;
        }
        ++proc.made;
        failedInRow = 0;
        arrived.emplace_back(index, frameFile(index));
        adoptArrived();
        dispatch(proc);
        grow();
        Animation::signalCacheChanged(owner);
    }
    else if (what == "fail") {
        long index = -1;
        in >> index;
        std::string why;
        std::getline(in, why);
        if (proc.frame == index) {
            proc.frame = -1;
        }
        ++failed;
        FC_LOG(owner.getFullName() << ": a worker did not make frame " << index << ":" << why);
        if (++failedInRow >= 3) {
            stop("the workers fail:" + why);
            return;
        }
        dispatch(proc);
    }
    else if (what == "unfit") {
        std::string why;
        std::getline(in, why);
        while (!why.empty() && why.front() == ' ') {
            why.erase(0, 1);
        }
        stop(why.empty() ? std::string("a worker cannot work on the document") : why);
    }
}

void Animation::Workers::finished(Proc& proc)
{
    // What it said before it went, and what came while a recompute ran
    read(proc);
    adoptArrived();
    if (proc.frame >= 0) {
        // Its frame is made where it is wanted, when it is
        ++failed;
        Base::FileInfo(frameFile(proc.frame) + ".part").deleteFile();
        proc.frame = -1;
    }
    proc.ready = false;
    for (auto it = procs.begin(); it != procs.end(); ++it) {
        if (it->get() == &proc) {
            QObject::disconnect(proc.process.get(), nullptr, &context, nullptr);
            gone.push_back(std::move(*it));  // not deleted inside its own signal
            procs.erase(it);
            break;
        }
    }
    if (procs.empty() && running) {
        running = false;
        if (!queue.empty()) {
            reason = "the workers exited";
        }
        queue.clear();
        cleanup();
    }
    Animation::signalCacheChanged(owner);
}

void Animation::Workers::dispatch(Proc& proc)
{
    if (!running || !proc.ready || proc.frame >= 0 || proc.quitting || !proc.process) {
        return;
    }
    while (!queue.empty()) {
        long index = queue.front();
        queue.pop_front();
        if (owner._frames.count(index)) {
            continue;
        }
        proc.frame = index;
        std::string command = std::to_string(index) + "\n";
        proc.process->write(command.data(), static_cast<qint64>(command.size()));
        // Out now: with an event loop QProcess writes when the loop next
        // runs, and a frame waited for inside an evaluation would wait for
        // a worker that was never told it
        proc.process->waitForBytesWritten(1000);
        return;
    }
    // Nothing more: it goes, and its memory with it
    proc.quitting = true;
    proc.process->write("quit\n", 5);
    proc.process->waitForBytesWritten(1000);
    proc.process->closeWriteChannel();
}

void Animation::Workers::grow()
{
    if (!running || static_cast<int>(procs.size()) >= limit
        || queue.size() <= procs.size()) {
        return;
    }
    // One at a time, and once each has made a frame: then what one holds
    // is known, and the memory left says whether another fits
    std::size_t biggest = 0;
    for (auto& proc : procs) {
        if (!proc->made) {
            return;
        }
        biggest = std::max(biggest, proc->resident);
    }
    if (!biggest) {
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
    if (!spawn()) {
        FC_LOG(owner.getFullName() << ": " << reason);
    }
}

void Animation::Workers::adoptArrived()
{
    auto doc = owner.getDocument();
    if (arrived.empty() || owner._capture || !doc || doc->testStatus(Document::Recomputing)
        || doc->isPerformingTransaction()) {
        return;
    }
    auto list = std::move(arrived);
    arrived.clear();
    for (auto& [index, file] : list) {
        owner.adoptFrame(index, file);
    }
}

void Animation::Workers::poll(int ms)
{
    bury();
    // Without an event loop (a console) output is read only when asked
    std::vector<Proc*> list;
    for (auto& proc : procs) {
        list.push_back(proc.get());
    }
    for (auto proc : list) {
        read(*proc);
        if (running && proc->process && proc->process->state() != QProcess::NotRunning) {
            int slice = ms > 0 ? std::max(1, ms / static_cast<int>(list.size())) : 0;
            proc->process->waitForReadyRead(slice);
            read(*proc);
        }
        if (!running) {
            break;
        }
    }
    adoptArrived();
}

void Animation::Workers::claim(long index)
{
    poll(0);
    if (!running) {
        return;
    }
    auto queued = std::find(queue.begin(), queue.end(), index);
    if (queued != queue.end()) {
        queue.erase(queued);
        return;
    }
    Proc* making = nullptr;
    for (auto& proc : procs) {
        if (proc->frame == index) {
            making = proc.get();
        }
    }
    if (!making) {
        return;
    }
    // Waited for: it is on its way, and nearer done than one begun here.
    // Not for ever: a worker that hangs is let go and the frame made here
    const double mean = owner.meanFrameCost();
    const qint64 most = std::max<qint64>(30000, static_cast<qint64>(mean * 20000));
    QElapsedTimer timer;
    timer.start();
    while (running && making->frame == index && making->process
           && making->process->state() != QProcess::NotRunning) {
        read(*making);
        if (!running || making->frame != index) {
            break;
        }
        if (!making->process->waitForReadyRead(100) && timer.elapsed() > most) {
            FC_WARN(owner.getFullName() << ": a worker took " << timer.elapsed() / 1000
                                        << " s over frame " << index << ", let go");
            making->process->kill();
            making->process->waitForFinished(2000);
            break;
        }
    }
    adoptArrived();
}

// --------------------------------------------------------------------------

int Animation::prepareFrames(const std::vector<double>& times, bool force)
{
    auto doc = getDocument();
    if (!_workers) {
        _workers = std::make_unique<Workers>(*this);
    }
    _workers->bury();
    auto refuse = [this](const std::string& why) {
        if (!_workers->running) {
            _workers->reason = why;
        }
        return _workers->alive();
    };
    if (!doc || !getNameInDocument()) {
        return refuse("the animation is not in a document");
    }
    if (!Active.getValue()) {
        return refuse("the animation is not playing");
    }
    if (!isCaching()) {
        return refuse("the frames are not kept: Cache is off");
    }
    const long setting = params()->GetInt("Workers", -1);
    if (setting == 0) {
        return refuse("workers are off (Workers of Preferences/Animation)");
    }
    if (params()->GetInt("CacheDiskLimitMB", 4096) <= 0
        || params()->GetInt("CacheDiskLimitBytes", -1) == 0) {
        return refuse("the cache has no disk to take the frames");
    }
    auto wanted = [&]() {
        std::vector<long> indices;
        long index = 0;
        for (double t : times.empty() ? frames() : times) {
            if (frameAt(t, index) && !_frames.count(index)) {
                indices.push_back(index);
            }
        }
        return indices;
    };
    std::vector<long> indices = wanted();
    if (indices.empty()) {
        return refuse("every frame is kept");
    }
    long current = 0;
    if (!frameAt(Time.getValue(), current)) {
        current = 0;
    }
    if (_workers->running) {
        _workers->enqueue(indices, current);
        return _workers->alive();
    }
    if (doc->getDependentDocuments(false).size() > 1) {
        return refuse("the document links to others, which a worker would not find");
    }
    const double mean = meanFrameCost();
    const double least = params()->GetFloat("WorkerMinSeconds", 2.0);
    if (!force && mean > 0 && mean * static_cast<double>(indices.size()) < least) {
        return refuse("the frames are made quicker here than a worker starts");
    }
    std::string transient = doc->TransientDir.getValue();
    std::string frameDir = cacheDir();
    if (transient.empty() || frameDir.empty()) {
        return refuse("the document has no directory for the frames");
    }
    // A new job: the document as it is designed, and these settings
    auto workers = std::make_unique<Workers>(*this);
    workers->frames = frameDir;
    workers->dir = transient + "/AnimationWorkers/" + getNameInDocument();
    workers->tables = workers->dir + "/tables";
    workers->job = workers->dir + "/job.txt";
    workers->loaded = std::move(_workers->loaded);
    {
        Base::FileInfo dir(workers->dir);
        if (dir.exists()) {
            dir.deleteDirectoryRecursive();
        }
        if (!Base::FileInfo(workers->tables).createDirectories()) {
            return refuse("cannot make " + workers->tables);
        }
    }
    const std::string copy = workers->dir + "/Document.FCStd";
    std::string failure;
    bool saved = false;
    try {
        // Its rest values, as a save writes the design (docs/Animation.md 3.8)
        saved = doc->saveCopy(copy.c_str(), false);
    }
    catch (Base::Exception& e) {
        failure = e.what();
    }
    if (!saved) {
        Base::FileInfo(workers->dir).deleteDirectoryRecursive();
        return refuse("the document could not be copied for the workers"
                      + (failure.empty() ? std::string() : ": " + failure));
    }
    GetApplication().GetUserParameter().SaveDocument((workers->dir + "/user.cfg").c_str());
    GetApplication().GetSystemParameter().SaveDocument((workers->dir + "/system.cfg").c_str());
    // The objects with a Python class here: one a worker has not got
    // would recompute to nothing there
    std::string proxies;
    {
        Base::PyGILStateLocker lock;
        for (auto obj : doc->getObjects()) {
            auto proxy = freecad_cast<PropertyPythonObject*>(obj->getPropertyByName("Proxy"));
            if (proxy && !proxy->getValue().isNone()) {
                proxies += proxies.empty() ? "" : " ";
                proxies += obj->getNameInDocument();
            }
        }
    }
    {
        std::ofstream out(workers->job, std::ios::binary | std::ios::trunc);
        out << "Document=" << copy << "\n"
            << "Animation=" << getNameInDocument() << "\n"
            << "Frames=" << workers->frames << "\n"
            << "Tables=" << workers->tables << "\n"
            << "Proxies=" << proxies << "\n";
        if (!out) {
            Base::FileInfo(workers->dir).deleteDirectoryRecursive();
            return refuse("cannot write " + workers->job);
        }
    }
    // The save may have let go of frames on disk
    indices = wanted();
    int most = static_cast<int>(setting);
    if (most < 0) {
        most = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 8);
    }
    workers->limit = most;
    workers->failed = _workers->failed;
    _workers->dir.clear();  // the old job's, the same directory as this one
    _workers = std::move(workers);
    _workers->running = true;
    _workers->enqueue(indices, current);
    if (!_workers->start()) {
        _workers->running = false;
        _workers->queue.clear();
        _workers->cleanup();
        return 0;
    }
    signalCacheChanged(*this);
    return _workers->alive();
}

void Animation::stopWorkers(const char* reason)
{
    if (_workers && (_workers->running || _workers->alive())) {
        _workers->stop(reason ? reason : "stopped");
        signalCacheChanged(*this);
    }
}

Animation::WorkerInfo Animation::workerInfo() const
{
    WorkerInfo info;
    if (!_workers) {
        return info;
    }
    auto& w = *_workers;
    info.running = w.running;
    info.workers = w.alive();
    info.limit = w.limit;
    info.queued = w.queue.size();
    const double rate = std::max(1e-9, FrameRate.getValue());
    for (auto& proc : w.procs) {
        if (proc->frame >= 0) {
            info.busy.push_back(Start.getValue() + static_cast<double>(proc->frame) / rate);
        }
    }
    info.resident = w.resident;
    std::sort(info.busy.begin(), info.busy.end());
    info.prepared = _cachePrepared;
    info.failed = w.failed;
    info.reason = w.reason;
    return info;
}

bool Animation::waitWorkers(double seconds)
{
    if (!_workers) {
        return true;
    }
    QElapsedTimer timer;
    timer.start();
    do {
        _workers->poll(seconds == 0 ? 0 : 50);
        if (!_workers->running) {
            break;
        }
    } while (seconds < 0 || timer.elapsed() < static_cast<qint64>(seconds * 1000));
    _workers->bury();
    return !_workers->running;
}
