// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <QElapsedTimer>

#include <App/Animation.h>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObserver.h>
#include <App/WriteScope.h>
#include <Base/Console.h>
#include <Base/Exception.h>

#include "AnimationTimeline.h"
#include "MainWindow.h"
#include "Renderer/MotionClipBlob.h"
#include "Renderer/SceneServer.h"
#include "SceneServeClip.h"

FC_LOG_LEVEL_INIT("SceneServe", true, true)

using namespace Gui;

namespace
{
/// The most frames a bake steps through, a minute at 30 fps: each is the
/// Animation written at that Time and a traversal of the served graph,
/// and the viewer interpolates between them
constexpr int MaxSamples = 1800;
/// However little room the byte budget leaves for poses, as many frames
/// as this (what every bake stepped through before it followed the rate)
constexpr int FewestSamples = 240;
/// What a bake holds of an object at a frame while it reads them, bytes:
/// the pose and look the baker keeps, the clip's own cells after
constexpr size_t PoseBytes = 128;
/// How long a slice of a bake steps frames before the events are let in
constexpr int SliceMs = 40;
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

/** The frames a bake of \a length seconds at \a rate steps through, for
 * \a objects in the feed and \a limit bytes, and their \a step (s). One
 * a frame of the Animation, at the Times the player plays a kept frame
 * at (docs/Animation.md sec 4), so that a shape rebuilt at a frame shows
 * at that frame in the browser too; spread evenly over the range when
 * that many poses would not fit the budget, or past MaxSamples.
 */
int frameCount(double length, double rate, size_t objects, size_t limit, double& step)
{
    const long want = static_cast<long>(std::floor(length * rate + 1e-6)) + 1;
    const long room = static_cast<long>(limit / (std::max<size_t>(1, objects) * PoseBytes));
    long count = std::min(want, std::max(room, static_cast<long>(FewestSamples)));
    count = std::clamp(count, 2L, static_cast<long>(MaxSamples));
    step = count == want ? 1.0 / rate : length / static_cast<double>(count - 1);
    return static_cast<int>(count);
}

}  // namespace

/// A bake under way: the clip's frames read so far and what they keep
struct SceneServeClip::Bake
{
    Bake()
        : baker([this](const Render::DrawCallList& draws) { return store(draws); })
    {}

    /// The draws of an object at a frame, kept for the viewers to fetch:
    /// one blob for the same bytes, however often they recur, and its
    /// meshes chunks of their own, one for the same mesh however many
    /// blobs name it -- the shape moved through the frames before it
    /// showed, a height keyed back and forth
    int32_t store(const Render::DrawCallList& draws);

    App::DocumentObjectT anim;
    double start = 0;
    double end = 0;
    double rate = 1;
    /// Set by the first frame, from what its feed holds
    double step = 0;
    int count = 0;
    int next = 0;
    /// The Time the player is at: put back when the bake ends
    double keep = 0;
    /// Something changed that the frames read so far did not see
    bool stale = false;
    size_t limit = 0;
    size_t bytes = 0;
    std::map<std::string, int32_t> index;
    std::vector<std::string> keys;
    std::vector<uint32_t> sizes;
    std::vector<std::string> chunks;
    std::vector<uint32_t> chunkSizes;
    std::map<std::string, std::vector<uint8_t>> kept;
    Render::MotionBlobMeshes meshes;
    Render::MotionClipBaker baker;
    QElapsedTimer clock;
};

int32_t SceneServeClip::Bake::store(const Render::DrawCallList& draws)
{
    std::vector<uint8_t> blob = Render::motionBlobOf(draws, &meshes);
    if (blob.empty()) {
        return -1;
    }
    std::string key = Render::sha1Hex(blob.data(), blob.size());
    auto it = index.find(key);
    if (it != index.end()) {
        meshes.drop();
        return it->second;
    }
    // What this blob adds: itself, and the meshes no kept blob named (one
    // met under another cacheId is held already)
    size_t adds = blob.size();
    for (const auto& chunk : meshes.fresh) {
        if (!kept.count(chunk.first)) {
            adds += chunk.second.size();
        }
    }
    if (bytes + adds > limit) {
        meshes.drop();
        return -1;
    }
    bytes += adds;
    for (auto& chunk : meshes.commit()) {
        if (kept.count(chunk.first)) {
            continue;
        }
        chunks.push_back(chunk.first);
        chunkSizes.push_back(static_cast<uint32_t>(chunk.second.size()));
        kept.emplace(chunk.first, std::move(chunk.second));
    }
    const auto number = static_cast<int32_t>(keys.size());
    index[key] = number;
    keys.push_back(key);
    sizes.push_back(static_cast<uint32_t>(blob.size()));
    kept.emplace(std::move(key), std::move(blob));
    return number;
}

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
        if (!valid && !job && playing()) {
            onPlayerState();
        }
    });
    slice.setSingleShot(true);
    slice.setInterval(0);
    connect(&slice, &QTimer::timeout, this, &SceneServeClip::bakeSlice);
}

SceneServeClip::~SceneServeClip()
{
    if (job) {
        // The Time is left as the sweep wrote it -- the document may be
        // going -- and the player plays on from where it was
        Render::ExactDetail::release();
        AnimationPlayer::instance()->release(job->keep);
    }
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
    if (!valid || inEdit || !clipId || clipId != theClip.id || theClip.empty())
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
    return valid && !inEdit && theClip.exact && !theClip.empty() && !told.empty() && playing()
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
        else if (job && &prop == &anim->Time && anim == job->anim.getObject()) {
            // Moved while the bake holds the player: it plays on from there
            job->keep = anim->Time.getValue();
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
    if (isBaking) {
        return;
    }
    if (job) {
        // Ended by its next slice, out of whatever signal says so
        job->stale = true;
        return;
    }
    if (!valid) {
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
    auto player = AnimationPlayer::instance();
    if (job && anim && player->isPlaying() && job->anim.getObject() == anim) {
        // Played again while it bakes: the clock waits for it again. A
        // pause, a stop or another Animation ends it at its next slice.
        player->hold();
    }
    if (anim && player->isPlaying()) {
        std::vector<Render::SceneClientInfo> clients;
        Render::SceneStreamServer::instance().clients(clients);
        const bool watched = std::any_of(clients.begin(), clients.end(), [this](const auto& c) {
            return c.doc == host.group;
        });
        if (!valid && !job && watched) {
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
    theClip = Render::MotionClip();
    valid = false;
    heldCount = 0;
    blobBytes = 0;
    Render::SceneStreamServer::instance().holdBlobs({}, host.group);
    clipJson = theClip.toJson(host.quotedGroup);
    told.clear();
    const double start = anim->Start.getValue();
    const double end = anim->endTime();
    if (!(end - start > 1e-9) || !host.capture) {
        return;
    }
    job = std::make_unique<Bake>();
    job->anim = App::DocumentObjectT(anim);
    job->start = start;
    job->end = end;
    job->rate = std::max(1.0, anim->FrameRate.getValue());
    job->keep = anim->Time.getValue();
    job->limit = blobLimit();
    job->clock.start();
    // What a frame is drawn with at the viewer is what the sweep read:
    // the exact mesh, not the coarse one a capture budget or coarse-first
    // tessellation stands in with for a while
    Render::ExactDetail::hold();
    // The player waits for the clip, so that the viewers start with the
    // application and not some seconds behind it
    AnimationPlayer::instance()->hold();
    slice.start();
}

void SceneServeClip::bakeSlice()
{
    if (!job) {
        return;
    }
    Bake& b = *job;
    auto anim = freecad_cast<App::Animation*>(b.anim.getObject());
    if (!anim || anim != animation() || !AnimationPlayer::instance()->isPlaying() || b.stale) {
        endBake(anim, false);
        return;
    }
    QElapsedTimer spent;
    spent.start();
    bool ok = true;
    isBaking = true;
    try {
        do {
            // The first frame sets the count; the last at the range's end
            // when spread over it, a frame of the rate before it otherwise
            double t = b.start + b.next * b.step;
            if (b.next == 0) {
                t = b.start;
            }
            else if (b.next + 1 == b.count) {
                t = std::min(b.end, b.start + (b.count - 1) * b.step);
            }
            anim->Time.setValue(t);
            const Render::DrawCallList* draws = host.capture();
            if (!draws) {
                ok = false;
                break;
            }
            if (b.next == 0) {
                std::vector<uint64_t> objects;
                for (const auto& draw : *draws) {
                    if (draw.objectKey) {
                        objects.push_back(draw.objectKey);
                    }
                }
                std::sort(objects.begin(), objects.end());
                objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
                b.count = frameCount(b.end - b.start, b.rate, objects.size(), b.limit, b.step);
            }
            b.baker.frame(*draws);
            ++b.next;
        } while (b.next < b.count && spent.elapsed() < SliceMs);
    }
    catch (Base::Exception& e) {
        e.ReportException();
        ok = false;
    }
    isBaking = false;
    if (!ok || b.next >= b.count) {
        endBake(anim, ok);
        return;
    }
    if (auto mw = getMainWindow()) {
        mw->showMessage(tr("Preparing the animation for the browser: frame %1 of %2")
                            .arg(b.next)
                            .arg(b.count),
                        2000);
    }
    slice.start();
}

void SceneServeClip::endBake(App::Animation* anim, bool done)
{
    std::unique_ptr<Bake> b = std::move(job);
    slice.stop();
    Render::ExactDetail::release();
    // Where the player is, unless a stop put the rest values back
    if (anim && anim->isAttachedToDocument() && anim->isActive()) {
        isBaking = true;
        try {
            if (std::fabs(anim->Time.getValue() - b->keep) > App::Anim::TimeEpsilon) {
                anim->Time.setValue(b->keep);
            }
        }
        catch (Base::Exception& e) {
            e.ReportException();
        }
        isBaking = false;
    }
    AnimationPlayer::instance()->release(b->keep);
    if (done) {
        theClip = b->baker.finish(++serial, b->start, b->step);
        theClip.blobs = std::move(b->keys);
        theClip.blobSizes = std::move(b->sizes);
        theClip.chunks = std::move(b->chunks);
        theClip.chunkSizes = std::move(b->chunkSizes);
        blobBytes = b->bytes;
        // The poses, sequences and looks as one body served by its key,
        // not base64 on the control channel (MotionClip::bodyBytes)
        if (!theClip.empty()) {
            std::vector<uint8_t> bytes = theClip.bodyBytes();
            theClip.body = Render::sha1Hex(bytes.data(), bytes.size());
            theClip.bodySize = static_cast<uint32_t>(bytes.size());
            b->kept[theClip.body] = std::move(bytes);
        }
        valid = true;
        FC_LOG("animation clip " << theClip.id << " of " << doc->getName() << ": "
                                 << theClip.keys.size() << " tracks, "
                                 << theClip.seqKeys.size() << " sequences, "
                                 << theClip.blobs.size() << " blobs and "
                                 << theClip.chunks.size() << " meshes (" << blobBytes
                                 << " bytes), a body of " << theClip.bodySize << " bytes, "
                                 << theClip.frames << " frames in "
                                 << b->clock.elapsed() << " ms"
                                 << (theClip.exact ? ", exact" : ""));
        // The chunks the clip names, which no publish does
        Render::SceneStreamServer::instance().holdBlobs(std::move(b->kept), host.group);
    }
    else {
        FC_LOG("animation clip of " << doc->getName() << " not baked: "
                                    << (b->stale ? "changed" : "stopped") << " at frame "
                                    << b->next << " of " << b->count);
    }
    clipJson = theClip.toJson(host.quotedGroup);
    told.clear();
    if (auto mw = getMainWindow()) {
        mw->showMessage(done ? tr("The animation is ready for the browser") : QString(), 2000);
    }
    announce();
    // The pose the player is at, held back while the sweep moved it
    if (host.publish) {
        host.publish();
    }
    if (b->stale && playing()) {
        rebake.start();
    }
}

Render::MotionClock SceneServeClip::clock() const
{
    Render::MotionClock c;
    c.clip = valid && !inEdit ? theClip.id : 0;
    if (App::Animation* anim = animation()) {
        auto player = AnimationPlayer::instance();
        c.active = anim->isActive();
        c.playing = player->isPlaying();
        // While a bake holds the player its Time is the sweep's
        c.time = job ? job->keep : anim->Time.getValue();
        c.raw = job ? job->keep : player->rawTime();
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
    // In an edit session the clients are told none (setEditing)
    const uint32_t shown = inEdit ? 0U : theClip.id;
    for (const auto& info : clients) {
        if (info.doc != host.group) {
            continue;
        }
        auto it = told.find(info.id);
        // The clip before the clock that names it
        if (it == told.end() || it->second != shown) {
            if (inEdit) {
                server.sendControl(info.id, Render::MotionClip().toJson(host.quotedGroup));
            }
            else {
                if (clipJson.empty()) {
                    clipJson = theClip.toJson(host.quotedGroup);
                }
                server.sendControl(info.id, clipJson);
            }
        }
        server.sendControl(info.id, time);
        here[info.id] = shown;
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

void SceneServeClip::setEditing(bool on)
{
    if (inEdit == on) {
        return;
    }
    inEdit = on;
    FC_LOG("animation clip of " << doc->getName() << (on ? " set aside: in edit" : " told again"));
    if (announced) {
        announce();
    }
    // The pose the host is at, which the clients now draw as it comes --
    // or, the edit over, the one they move by the clip from
    if (host.publish) {
        host.publish();
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
