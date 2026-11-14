// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <App/Animation.h>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/WriteScope.h>
#include <Base/Console.h>
#include <Base/Exception.h>

#include "AnimationTimeline.h"
#include "Renderer/MotionClipBlob.h"
#include "Renderer/SceneServer.h"
#include "SceneServeClip.h"

FC_LOG_LEVEL_INIT("SceneServe", true, true)

using namespace Gui;

namespace
{
/// The most frames a bake steps through: each is a traversal of the
/// served graph, and the viewer interpolates between them
constexpr int MaxSamples = 240;
constexpr int HeartbeatMs = 1000;
constexpr int RebakeMs = 500;
/// The most the blobs of one clip hold (a sequence's draws at every
/// frame), unless the preference says otherwise: they stay in the
/// server's store and in each viewer for as long as the clip lives
constexpr int DefaultBlobMB = 64;

size_t blobLimit()
{
    auto hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Animation");
    const long mb = hGrp->GetInt("StreamBlobMB", DefaultBlobMB);
    return static_cast<size_t>(std::max(0L, mb)) * 1024 * 1024;
}

}  // namespace

SceneServeClip::SceneServeClip(App::Document* doc, Host host, QObject* parent)
    : QObject(parent)
    , doc(doc)
    , host(std::move(host))
{
    connect(AnimationPlayer::instance(), &AnimationPlayer::stateChanged, this,
            &SceneServeClip::onPlayerState);
    heartbeat.setInterval(HeartbeatMs);
    connect(&heartbeat, &QTimer::timeout, this, [this]() {
        if (playing()) {
            announce();
        }
        else {
            heartbeat.stop();
        }
    });
    // A clip made stale while playing is baked again once the edits stop
    rebake.setSingleShot(true);
    rebake.setInterval(RebakeMs);
    connect(&rebake, &QTimer::timeout, this, [this]() {
        if (!valid && playing()) {
            onPlayerState();
        }
    });
}

SceneServeClip::~SceneServeClip()
{
    if (blobBytes) {
        Render::SceneStreamServer::instance().holdBlobs({}, host.group);
    }
}

App::Animation* SceneServeClip::animation() const
{
    App::Animation* anim = AnimationPlayer::instance()->animation();
    return anim && anim->isAttachedToDocument() && anim->getDocument() == doc ? anim : nullptr;
}

bool SceneServeClip::playing() const
{
    return animation() && AnimationPlayer::instance()->isPlaying();
}

bool SceneServeClip::movePick(uint32_t clipId, double time, uint64_t key, float origin[3],
                              float dir[3]) const
{
    if (!valid || !clipId || clipId != theClip.id || theClip.empty())
        return false;
    App::Animation* anim = animation();
    if (!anim || !anim->Active.getValue())
        return false;
    auto it = std::find(theClip.keys.begin(), theClip.keys.end(), key);
    if (it == theClip.keys.end())
        return false;
    const size_t track = size_t(it - theClip.keys.begin());
    // Where the host has it now, from where the viewer drew it
    double x[16];
    if (!theClip.moveBetween(track, time, anim->Time.getValue(), x))
        return false;
    const double o[3] = {origin[0], origin[1], origin[2]};
    const double d[3] = {dir[0], dir[1], dir[2]};
    for (int r = 0; r < 3; ++r) {
        origin[r] = float(x[r] * o[0] + x[4 + r] * o[1] + x[8 + r] * o[2] + x[12 + r]);
        dir[r] = float(x[r] * d[0] + x[4 + r] * d[1] + x[8 + r] * d[2]);
    }
    return true;
}

bool SceneServeClip::holding() const
{
    return valid && theClip.exact && !theClip.empty() && !told.empty() && playing()
        && !(host.unclipped && host.unclipped());
}

bool SceneServeClip::changed(const App::Property& prop)
{
    auto obj = freecad_cast<App::DocumentObject*>(prop.getContainer());
    const bool ofAnimation = obj && obj->isDerivedFrom<App::Animation>();
    if (isBaking) {
        // The sweep's own writes: what they do is read from the feed
        return true;
    }
    const bool animated = (App::WriteScope::causes() & App::WriteCause::Animation) != 0;
    if (ofAnimation) {
        auto anim = static_cast<App::Animation*>(obj);
        if (&prop == &anim->Tracks || &prop == &anim->FrameRate || &prop == &anim->Exclude
            || &prop == &anim->Recompute) {
            invalidate();
        }
        else if (anim == animation() && &prop != &anim->Time && announced) {
            // Speed, Loop, the range, Active: the clock reads them
            if (&prop == &anim->Start || &prop == &anim->End) {
                invalidate();
            }
            announce();
        }
        // Time itself draws nothing: what it moves says so by itself
        return holding();
    }
    if (!animated) {
        // An edit: the poses the clip holds are of a document that was
        invalidate();
        return false;
    }
    // Whatever the Animation writes at a Time it wrote in the sweep, and
    // the sweep found all of it in the tracks
    if (holding()) {
        ++heldCount;
        return true;
    }
    return false;
}

void SceneServeClip::invalidate()
{
    if (isBaking || !valid) {
        return;
    }
    valid = false;
    theClip = Render::MotionClip();
    clipJson = theClip.toJson(host.quotedGroup);
    told.clear();
    blobBytes = 0;
    Render::SceneStreamServer::instance().holdBlobs({}, host.group);
    if (announced) {
        announce();
    }
    if (playing()) {
        rebake.start();
    }
}

void SceneServeClip::onPlayerState()
{
    App::Animation* anim = animation();
    if (anim && AnimationPlayer::instance()->isPlaying()) {
        std::vector<Render::SceneClientInfo> clients;
        Render::SceneStreamServer::instance().clients(clients);
        const bool watched = std::any_of(clients.begin(), clients.end(), [this](const auto& c) {
            return c.doc == host.group;
        });
        if (!valid && watched) {
            bake(anim);
        }
        heartbeat.start();
    }
    else {
        heartbeat.stop();
        if (!anim && !announced) {
            return;
        }
    }
    announce();
    // The pose this state is in, named to the clients when it goes out
    if (host.publish) {
        host.publish();
    }
}

void SceneServeClip::bake(App::Animation* anim)
{
    const double start = anim->Start.getValue();
    const double end = anim->endTime();
    const double keep = anim->Time.getValue();
    theClip = Render::MotionClip();
    valid = false;
    heldCount = 0;
    blobBytes = 0;
    std::map<std::string, std::vector<uint8_t>> kept;
    if (end - start > 1e-9 && host.capture) {
        const double rate = std::max(1.0, anim->FrameRate.getValue());
        const int count = std::clamp(static_cast<int>(std::lround((end - start) * rate)) + 1, 2,
                                     MaxSamples);
        const double step = (end - start) / (count - 1);
        // The draws of an object at a frame, kept for the viewers to
        // fetch: one blob for the same bytes, however often they recur
        std::map<std::string, int32_t> index;
        std::vector<std::string> keys;
        std::vector<uint32_t> sizes;
        const size_t limit = blobLimit();
        auto store = [&](const Render::DrawCallList& draws) -> int32_t {
            std::vector<uint8_t> blob = Render::motionBlobOf(draws);
            if (blob.empty()) {
                return -1;
            }
            std::string key = Render::sha1Hex(blob.data(), blob.size());
            auto it = index.find(key);
            if (it != index.end()) {
                return it->second;
            }
            if (blobBytes + blob.size() > limit) {
                return -1;
            }
            blobBytes += blob.size();
            const auto number = static_cast<int32_t>(keys.size());
            index[key] = number;
            keys.push_back(key);
            sizes.push_back(static_cast<uint32_t>(blob.size()));
            kept.emplace(std::move(key), std::move(blob));
            return number;
        };
        Render::MotionClipBaker baker(store);
        bool ok = true;
        isBaking = true;
        // What a frame is drawn with at the viewer is what the sweep
        // read: the exact mesh, not the coarse one a capture budget or
        // coarse-first tessellation stands in with for a while
        Render::ExactDetail::hold();
        try {
            for (int i = 0; i < count && ok; ++i) {
                anim->Time.setValue(i + 1 == count ? end : start + i * step);
                const Render::DrawCallList* draws = host.capture();
                if (draws) {
                    baker.frame(*draws);
                }
                else {
                    ok = false;
                }
            }
        }
        catch (Base::Exception& e) {
            e.ReportException();
            ok = false;
        }
        Render::ExactDetail::release();
        try {
            anim->Time.setValue(keep);
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
        isBaking = false;
        if (ok) {
            theClip = baker.finish(++serial, start, step);
            theClip.blobs = std::move(keys);
            theClip.blobSizes = std::move(sizes);
            valid = true;
            FC_LOG("animation clip " << theClip.id << " of " << doc->getName() << ": "
                                     << theClip.keys.size() << " tracks, "
                                     << theClip.seqKeys.size() << " sequences, "
                                     << theClip.blobs.size() << " blobs (" << blobBytes
                                     << " bytes), " << theClip.frames << " frames"
                                     << (theClip.exact ? ", exact" : ""));
        }
        else {
            kept.clear();
            blobBytes = 0;
        }
    }
    // The chunks the clip names, which no publish does
    Render::SceneStreamServer::instance().holdBlobs(std::move(kept), host.group);
    clipJson = theClip.toJson(host.quotedGroup);
    told.clear();
}

Render::MotionClock SceneServeClip::clock() const
{
    Render::MotionClock c;
    c.clip = valid ? theClip.id : 0;
    if (App::Animation* anim = animation()) {
        auto player = AnimationPlayer::instance();
        c.active = anim->isActive();
        c.playing = player->isPlaying();
        c.time = anim->Time.getValue();
        c.raw = player->rawTime();
        c.speed = anim->Speed.getValue();
        c.start = anim->Start.getValue();
        c.end = anim->endTime();
        c.loop = static_cast<int>(anim->Loop.getValue());
    }
    c.version = baseVersion;
    c.baseActive = baseActive;
    c.baseTime = baseTime;
    return c;
}

void SceneServeClip::announce()
{
    auto& server = Render::SceneStreamServer::instance();
    std::vector<Render::SceneClientInfo> clients;
    server.clients(clients);
    const std::string time = clock().toJson(host.quotedGroup);
    std::map<uint64_t, uint32_t> here;
    for (const auto& info : clients) {
        if (info.doc != host.group) {
            continue;
        }
        auto it = told.find(info.id);
        // The clip before the clock that names it
        if (it == told.end() || it->second != theClip.id) {
            if (clipJson.empty()) {
                clipJson = theClip.toJson(host.quotedGroup);
            }
            server.sendControl(info.id, clipJson);
        }
        server.sendControl(info.id, time);
        here[info.id] = theClip.id;
    }
    told = std::move(here);
    announced = true;
}

void SceneServeClip::published(uint64_t version)
{
    App::Animation* anim = animation();
    baseVersion = version;
    baseActive = anim && anim->isActive();
    baseTime = anim ? anim->Time.getValue() : 0.0;
    if (announced) {
        announce();
    }
}

void SceneServeClip::syncClients()
{
    if (announced) {
        announce();
    }
    // The first client to come while it plays: there was nobody to bake for
    if (!valid && playing()) {
        rebake.start();
    }
}

#include "moc_SceneServeClip.cpp"
