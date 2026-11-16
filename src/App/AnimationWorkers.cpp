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

// An Animation's frames made by worker processes (docs/Animation.md sec
// 4.2): each frame not kept is a job of the kind "animation.frame" in the
// document's WorkerPool (docs/WorkerProcess.md sec 8). A worker makes it
// with Animation::writeFrame() on its copy of the document, into the file
// the pool gives it, the string table its values name beside the other
// workers'; the frame comes into the cache as a frame on disk
// (Animation::adoptFrame).

#include "PreCompiled.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <QElapsedTimer>

#include <nlohmann/json.hpp>

#include <Base/Console.h>
#include <Base/FileInfo.h>
#include <Base/Parameter.h>

#include "AnimationWorkers.h"
#include "Application.h"
#include "Document.h"

FC_LOG_LEVEL_INIT("Animation", true, true)

using namespace App;
using json = nlohmann::json;

namespace
{

constexpr const char* FrameKind = "animation.frame";

ParameterGrp::handle params()
{
    return GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Animation");
}

/// In a worker: make the frame the job names, into the pool's file
WorkerPool::Reply makeFrame(const WorkerPool::Request& request)
{
    WorkerPool::Reply reply;
    json inputs = json::parse(request.inputs, nullptr, false);
    const std::string name = inputs.value("animation", std::string());
    auto obj = request.document ? request.document->getObject(name.c_str()) : nullptr;
    auto anim = freecad_cast<Animation*>(obj);
    if (!anim) {
        reply.ok = false;
        reply.unfit = true;
        reply.error = "no animation " + name + " in the copy";
        return reply;
    }
    // Nothing is kept here: each frame goes to its file and is let go
    if (anim->Cache.getValue()) {
        anim->Cache.setValue(false);
    }
    auto written = anim->writeFrame(inputs.value("index", 0L), request.out,
                                    inputs.value("tables", std::string()));
    reply.output = json {{"bytes", written.bytes}, {"cost", written.cost}, {"table", written.table}}
                       .dump();
    return reply;
}

const bool registered = WorkerPool::registerKind(FrameKind, makeFrame);

bool moveFile(const std::string& from, const std::string& to)
{
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) {
        return true;
    }
    // Another file system
    if (!std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing,
                                    ec)) {
        return false;
    }
    std::filesystem::remove(from, ec);
    return true;
}

}  // namespace

Animation::Workers::Workers(Animation& owner)
    : owner(owner)
{
    (void)registered;
    static long serial = 0;
    tag = std::string("animation:") + (owner.getNameInDocument() ? owner.getNameInDocument() : "")
        + ":" + std::to_string(++serial);
}

Animation::Workers::~Workers()
{
    if (auto p = pool()) {
        p->cancel([this](const std::string& t) { return t == tag; });
    }
    for (auto& [index, file] : arrived) {
        Base::FileInfo(file).deleteFile();
    }
}

WorkerPool* Animation::Workers::pool() const
{
    return doc ? WorkerPool::of(*doc, false) : nullptr;
}

void Animation::Workers::enqueue(const std::vector<long>& indices, long from)
{
    WorkerPool* p = pool();
    if (!p || !p->started()) {
        return;
    }
    std::vector<long> sorted(indices);
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    // From the frame on show on, then the ones before it
    auto mid = std::lower_bound(sorted.begin(), sorted.end(), from);
    std::rotate(sorted.begin(), mid, sorted.end());
    for (long index : sorted) {
        if (owner._frames.count(index) || jobs.count(index)) {
            continue;
        }
        WorkerPool::Job job;
        job.kind = FrameKind;
        job.tag = tag;
        job.inputs = json {{"animation", owner.getNameInDocument()},
                           {"index", index},
                           {"tables", tables}}
                         .dump();
        // Known only once it is submitted: the callback finds it by frame
        auto slot = std::make_shared<WorkerPool::JobId>(0);
        job.done = [this, index, slot](WorkerPool::Result& r) { done(index, *slot, r); };
        WorkerPool::JobId id = p->submit(std::move(job));
        if (!id) {
            break;
        }
        *slot = id;
        jobs[index] = id;
    }
}

void Animation::Workers::done(long index, WorkerPool::JobId id, WorkerPool::Result& r)
{
    auto it = jobs.find(index);
    if (it != jobs.end() && (it->second == id || !id)) {
        jobs.erase(it);
    }
    resident = std::max(resident, r.resident);
    switch (r.status) {
        case WorkerPool::Result::Status::Done: {
            std::string dir = owner.cacheDir();
            std::string file = dir + "/" + std::to_string(index) + ".wframe";
            if (!dir.empty() && !r.file.empty() && moveFile(r.file, file)) {
                r.file.clear();
                arrived.emplace_back(index, file);
                adoptArrived();
            }
            else {
                ++failed;
            }
            break;
        }
        case WorkerPool::Result::Status::Failed:
            ++failed;
            FC_LOG(owner.getFullName() << ": a worker did not make frame " << index << ": "
                                       << r.error);
            break;
        case WorkerPool::Result::Status::Unfit:
            // As unfit for every other frame: they are made here
            cancel(r.error.empty() ? std::string("a worker cannot make the frames") : r.error);
            break;
        case WorkerPool::Result::Status::Cancelled:
            // The pool stopped: its copy is not the design, nor the frames
            // that came and were not taken in yet, whose string tables go
            // with its directory
            cancel(r.error);
            break;
    }
    Animation::signalCacheChanged(owner);
}

void Animation::Workers::cancel(const std::string& why)
{
    if (!why.empty() && (!jobs.empty() || !arrived.empty())) {
        reason = why;
    }
    if (auto p = pool()) {
        p->cancel([this](const std::string& t) { return t == tag; });
    }
    jobs.clear();
    for (auto& [index, file] : arrived) {
        Base::FileInfo(file).deleteFile();
    }
    arrived.clear();
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

void Animation::Workers::adoptArrived()
{
    auto d = owner.getDocument();
    if (arrived.empty() || owner._capture || !d || d->testStatus(Document::Recomputing)
        || d->isPerformingTransaction()) {
        return;
    }
    auto list = std::move(arrived);
    arrived.clear();
    for (auto& [index, file] : list) {
        owner.adoptFrame(index, file);
    }
}

void Animation::Workers::claim(long index)
{
    WorkerPool* p = pool();
    auto it = jobs.find(index);
    if (!p || it == jobs.end()) {
        adoptArrived();
        return;
    }
    const WorkerPool::JobId id = it->second;
    // Done: the result came through done(), which took the job off
    if (p->claim(id) != WorkerPool::Claim::Done) {
        it = jobs.find(index);
        if (it != jobs.end() && it->second == id) {
            jobs.erase(it);
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
    auto alive = [this]() {
        WorkerPool* p = _workers->pool();
        return p ? p->info().workers : 0;
    };
    auto refuse = [&](const std::string& why) {
        if (!_workers->running()) {
            _workers->reason = why;
        }
        return alive();
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
    std::string why;
    if (!WorkerPool::supported(&why)) {
        return refuse(why);
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
    WorkerPool* pool = WorkerPool::of(*doc);
    if (_workers->running() && _workers->doc == doc && pool->started()) {
        _workers->enqueue(indices, current);
        return alive();
    }
    const double mean = meanFrameCost();
    const double least = params()->GetFloat("WorkerMinSeconds", 2.0);
    if (!force && mean > 0 && mean * static_cast<double>(indices.size()) < least) {
        return refuse("the frames are made quicker here than a worker starts");
    }
    if (cacheDir().empty()) {
        return refuse("the document has no directory for the frames");
    }
    // The document as it is designed, if the pool has no copy standing
    if (!pool->start()) {
        return refuse(pool->reason());
    }
    _workers->doc = doc;
    _workers->tables = pool->info().directory + "/tables";
    if (!Base::FileInfo(_workers->tables).createDirectories()) {
        return refuse("cannot make " + _workers->tables);
    }
    _workers->reason.clear();
    // The copy's save may have let go of frames on disk
    _workers->enqueue(wanted(), current);
    signalCacheChanged(*this);
    return alive();
}

void Animation::stopWorkers(const char* reason)
{
    if (_workers && (_workers->running() || !_workers->arrived.empty())) {
        _workers->cancel(reason ? reason : "stopped");
        signalCacheChanged(*this);
    }
}

void Animation::designChanged()
{
    // The workers' copy is not the design any more, whoever's jobs they run
    if (auto doc = getDocument()) {
        if (auto pool = WorkerPool::of(*doc, false)) {
            if (pool->started()) {
                pool->stop("the design changed");
            }
        }
    }
    stopWorkers("the design changed");
}

Animation::WorkerInfo Animation::workerInfo() const
{
    WorkerInfo info;
    if (!_workers) {
        if (auto doc = getDocument()) {
            info.limit = WorkerPool::of(*doc)->info().limit;
        }
        return info;
    }
    auto& w = *_workers;
    info.running = w.running();
    WorkerPool* pool = w.pool();
    if (!pool && getDocument()) {
        pool = WorkerPool::of(*getDocument());
    }
    const double rate = std::max(1e-9, FrameRate.getValue());
    if (pool) {
        WorkerPool::Info p = pool->info();
        info.workers = p.workers;
        info.limit = p.limit;
        info.resident = std::max(w.resident, p.resident);
        for (auto& [index, id] : w.jobs) {
            switch (pool->state(id)) {
                case WorkerPool::State::Running:
                    info.busy.push_back(Start.getValue() + static_cast<double>(index) / rate);
                    break;
                case WorkerPool::State::Queued:
                    ++info.queued;
                    break;
                default:
                    break;
            }
        }
    }
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
    for (;;) {
        WorkerPool* pool = _workers->pool();
        if (!pool) {
            _workers->jobs.clear();
        }
        if (!_workers->running()) {
            break;
        }
        pool->poll(seconds == 0 ? 0 : 50);
        if (seconds >= 0 && timer.elapsed() >= static_cast<qint64>(seconds * 1000)) {
            break;
        }
    }
    _workers->adoptArrived();
    return !_workers->running();
}
