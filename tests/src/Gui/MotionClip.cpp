// SPDX-License-Identifier: LGPL-2.1-or-later

// An Animation's motion as a viewer plays it by itself (docs/Animation.md
// sec 9, "Streaming"): the bake from the feed, the clip on the wire, the
// clock, and the move a viewer makes with them. No scene and no socket --
// the header is what the serving process and the browser's viewer share,
// so what one writes the other reads here.

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "Gui/Renderer/MotionClip.h"
#include "Gui/Renderer/MotionClipBlob.h"

using namespace Render;

// NOLINTBEGIN(readability-magic-numbers)

namespace
{

const double Pi = 3.14159265358979323846;

/// A turn of \a angle about Z, then a move to (x, y, z)
void placement(double angle, double x, double y, double z, double m[16])
{
    Motion::Pose p;
    p.q[2] = std::sin(angle / 2);
    p.q[3] = std::cos(angle / 2);
    p.t[0] = x;
    p.t[1] = y;
    p.t[2] = z;
    Motion::matrixOf(p, m);
}

DrawCall draw(uint64_t key, const double m[16], const std::shared_ptr<const MeshData> &mesh)
{
    DrawCall d;
    d.objectKey = key;
    d.mesh = mesh;
    d.identity = false;
    for (int i = 0; i < 16; ++i) {
        d.model[i] = float(m[i]);
    }
    d.bboxMin[0] = d.bboxMin[1] = d.bboxMin[2] = -1.0F;
    d.bboxMax[0] = d.bboxMax[1] = d.bboxMax[2] = 1.0F;
    return d;
}

std::shared_ptr<const MeshData> mesh(uint64_t id)
{
    auto m = std::make_shared<MeshData>();
    m->cacheId = id;
    return m;
}

/// The feed of a scene at frame \a i of \a count: object 1 stays, object 2
/// (two draws, the second offset inside it) slides along X and turns
DrawCallList feed(int i, int count, const std::shared_ptr<const MeshData> &a,
                  const std::shared_ptr<const MeshData> &b)
{
    DrawCallList draws;
    double still[16], body[16], part[16], local[16];
    placement(0, 5, 5, 0, still);
    draws.push_back(draw(1, still, a));
    const double u = double(i) / (count - 1);
    placement(u * Pi, 10 * u, 0, 0, body);
    draws.push_back(draw(2, body, b));
    placement(0.3, 0, 2, 0, local);
    Motion::mul(body, local, part);
    draws.push_back(draw(2, part, a));
    return draws;
}

void expectNear(const double a[16], const double b[16], double tol)
{
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(a[i], b[i], tol) << "element " << i;
    }
}

}  // namespace

TEST(Motion, aMatrixTimesItsInverseIsTheIdentity)
{
    double m[16], back[16], product[16], one[16];
    placement(0.7, 3, -4, 5, m);
    ASSERT_TRUE(Motion::invertAffine(m, back));
    Motion::mul(m, back, product);
    Motion::identity(one);
    expectNear(product, one, 1e-12);
}

TEST(Motion, aPoseAndItsMatrixAreTheSame)
{
    for (double angle : {0.0, 0.4, 2.0, 3.1, -2.5}) {
        double m[16], again[16];
        placement(angle, 1, 2, 3, m);
        Motion::Pose p;
        ASSERT_TRUE(Motion::poseOf(m, p)) << angle;
        Motion::matrixOf(p, again);
        expectNear(m, again, 1e-9);
    }
}

TEST(Motion, aScaleOrAMirrorIsNoPose)
{
    double m[16];
    Motion::Pose p;
    Motion::identity(m);
    m[0] = 2.0;
    EXPECT_FALSE(Motion::poseOf(m, p));
    Motion::identity(m);
    m[0] = -1.0;
    EXPECT_FALSE(Motion::poseOf(m, p));
}

TEST(MotionClipBaker, whatMovesRigidlyHasATrackAndWhatStaysHasNone)
{
    auto a = mesh(11), b = mesh(12);
    MotionClipBaker baker;
    const int count = 9;
    for (int i = 0; i < count; ++i) {
        baker.frame(feed(i, count, a, b));
    }
    MotionClip clip = baker.finish(1, 2.0, 0.25);
    EXPECT_TRUE(clip.exact);
    ASSERT_EQ(clip.keys.size(), 1U);
    EXPECT_EQ(clip.keys[0], 2U);
    EXPECT_EQ(clip.frames, 9U);
    ASSERT_EQ(clip.data.size(), 9U * 7U);

    // The track is the move from the first frame: at each frame it takes
    // the first frame's draws to that frame's
    for (int i = 0; i < count; ++i) {
        DrawCallList first = feed(0, count, a, b);
        DrawCallList now = feed(i, count, a, b);
        double x[16];
        ASSERT_TRUE(clip.moveBetween(0, 2.0, 2.0 + i * 0.25, x));
        for (size_t k = 1; k < first.size(); ++k) {
            Motion::moveDraw(first[k], x);
            for (int e = 0; e < 16; ++e) {
                EXPECT_NEAR(first[k].model[e], now[k].model[e], 1e-4) << i << " " << k << " " << e;
            }
        }
    }
}

TEST(MotionClipBaker, aViewerMovesFromThePublishItHoldsNotFromTheFirstFrame)
{
    auto a = mesh(11), b = mesh(12);
    MotionClipBaker baker;
    const int count = 9;
    for (int i = 0; i < count; ++i) {
        baker.frame(feed(i, count, a, b));
    }
    MotionClip clip = baker.finish(1, 0.0, 1.0);
    // Holding the publish of frame 3, shown at frame 6
    DrawCallList held = feed(3, count, a, b);
    DrawCallList want = feed(6, count, a, b);
    double x[16];
    ASSERT_TRUE(clip.moveBetween(0, 3.0, 6.0, x));
    Motion::moveDraw(held[1], x);
    for (int e = 0; e < 16; ++e) {
        EXPECT_NEAR(held[1].model[e], want[1].model[e], 1e-4) << e;
    }
    // And at the Time the publish was taken nothing moves
    double none[16], one[16];
    ASSERT_TRUE(clip.moveBetween(0, 3.0, 3.0, none));
    Motion::identity(one);
    expectNear(none, one, 1e-9);
}

TEST(MotionClipBaker, theBoxOfAMovedDrawMovesWithIt)
{
    double m[16], x[16];
    Motion::identity(m);
    DrawCall d = draw(1, m, mesh(1));
    placement(Pi / 2, 10, 0, 0, x);
    Motion::moveDraw(d, x);
    EXPECT_NEAR(d.bboxMin[0], 9.0, 1e-5);
    EXPECT_NEAR(d.bboxMax[0], 11.0, 1e-5);
    EXPECT_NEAR(d.bboxMin[1], -1.0, 1e-5);
    EXPECT_NEAR(d.bboxMax[2], 1.0, 1e-5);
}

TEST(MotionClipBaker, aRebuiltShapeHasNoTrackAndTheClipIsNotExact)
{
    auto a = mesh(11), b = mesh(12), rebuilt = mesh(13);
    MotionClipBaker baker;
    baker.frame(feed(0, 3, a, b));
    baker.frame(feed(1, 3, a, rebuilt));
    baker.frame(feed(2, 3, a, b));
    MotionClip clip = baker.finish(1, 0.0, 1.0);
    EXPECT_FALSE(clip.exact);
    EXPECT_TRUE(clip.keys.empty());
}

TEST(MotionClipBaker, aChangedColourIsNoMove)
{
    auto a = mesh(11), b = mesh(12);
    MotionClipBaker baker;
    baker.frame(feed(0, 3, a, b));
    DrawCallList recoloured = feed(1, 3, a, b);
    recoloured[0].material.diffuse = 0xFF0000FF;
    baker.frame(recoloured);
    baker.frame(feed(2, 3, a, b));
    MotionClip clip = baker.finish(1, 0.0, 1.0);
    EXPECT_FALSE(clip.exact);
    // What moved is played all the same
    ASSERT_EQ(clip.keys.size(), 1U);
    EXPECT_EQ(clip.keys[0], 2U);
}

TEST(MotionClipBaker, anObjectThatGoesOrComesWithNowhereToKeepItsDrawsIsNotExact)
{
    auto a = mesh(11), b = mesh(12);
    {
        MotionClipBaker baker;
        baker.frame(feed(0, 3, a, b));
        DrawCallList less = feed(1, 3, a, b);
        less.erase(less.begin());
        baker.frame(less);
        baker.frame(feed(2, 3, a, b));
        MotionClip clip = baker.finish(1, 0.0, 1.0);
        EXPECT_FALSE(clip.exact);
        // The one that went has a track all the same, hidden at frame 1
        ASSERT_EQ(clip.keys.size(), 2U);
        EXPECT_EQ(clip.keys[0], 1U);
        EXPECT_EQ(clip.keys[1], 2U);
        ASSERT_EQ(clip.shown.size(), 6U);
        EXPECT_EQ(clip.shown, (std::vector<uint8_t> {1, 0, 1, 1, 1, 1}));
        EXPECT_EQ(clip.refBlob[0], -1);
    }
    {
        MotionClipBaker baker;
        DrawCallList less = feed(0, 3, a, b);
        less.erase(less.begin());
        baker.frame(less);
        baker.frame(feed(1, 3, a, b));
        baker.frame(feed(2, 3, a, b));
        MotionClip clip = baker.finish(1, 0.0, 1.0);
        EXPECT_FALSE(clip.exact);
        ASSERT_EQ(clip.keys.size(), 2U);
        // Measured from the frame it came at
        EXPECT_EQ(clip.refFrame[0], 1);
    }
}

namespace
{

/// A store that keeps what it is given, up to \a room blobs
struct Kept
{
    std::vector<DrawCallList> blobs;
    size_t room = 1000;
    MotionClipBaker::Store store()
    {
        return [this](const DrawCallList &draws) -> int32_t {
            if (blobs.size() >= room) {
                return -1;
            }
            blobs.push_back(draws);
            return int32_t(blobs.size() - 1);
        };
    }
};

}  // namespace

TEST(MotionClipBaker, aHiddenObjectHasItsDrawsKeptAndTheClipIsExact)
{
    auto a = mesh(11), b = mesh(12);
    Kept kept;
    MotionClipBaker baker(kept.store());
    DrawCallList less = feed(0, 4, a, b);
    less.erase(less.begin() + 1, less.end());
    // Object 2 hidden at frames 0 and 2
    baker.frame(less);
    baker.frame(feed(1, 4, a, b));
    less = feed(2, 4, a, b);
    less.erase(less.begin() + 1, less.end());
    baker.frame(less);
    baker.frame(feed(3, 4, a, b));
    MotionClip clip = baker.finish(1, 0.0, 1.0);
    EXPECT_TRUE(clip.exact);
    ASSERT_EQ(clip.keys.size(), 1U);
    EXPECT_EQ(clip.keys[0], 2U);
    EXPECT_EQ(clip.shown, (std::vector<uint8_t> {0, 1, 0, 1}));
    EXPECT_FALSE(clip.shownAt(0, 0.2));
    EXPECT_TRUE(clip.shownAt(0, 0.8));
    EXPECT_EQ(clip.refFrame[0], 1);
    ASSERT_GE(clip.refBlob[0], 0);
    // The kept draws are frame 1's, and the track takes them to frame 3's
    const DrawCallList &ref = kept.blobs[size_t(clip.refBlob[0])];
    ASSERT_EQ(ref.size(), 2U);
    DrawCallList want = feed(3, 4, a, b);
    double x[16];
    ASSERT_TRUE(clip.moveBetween(0, clip.timeOf(1), 3.0, x));
    for (size_t k = 0; k < 2; ++k) {
        DrawCall moved = ref[k];
        Motion::moveDraw(moved, x);
        for (int e = 0; e < 16; ++e) {
            EXPECT_NEAR(moved.model[e], want[k + 1].model[e], 1e-4) << k << " " << e;
        }
    }
    EXPECT_TRUE(clip.seqKeys.empty());
}

TEST(MotionClipBaker, aRebuiltShapeIsASequenceOfItsDraws)
{
    auto a = mesh(11), b = mesh(12), rebuilt = mesh(13);
    Kept kept;
    MotionClipBaker baker(kept.store());
    const int count = 4;
    // Rigid for frames 0 to 2, rebuilt at 3
    baker.frame(feed(0, count, a, b));
    baker.frame(feed(1, count, a, b));
    baker.frame(feed(2, count, a, b));
    baker.frame(feed(3, count, a, rebuilt));
    MotionClip clip = baker.finish(1, 0.0, 1.0);
    EXPECT_TRUE(clip.exact);
    EXPECT_TRUE(clip.keys.empty());
    ASSERT_EQ(clip.seqKeys.size(), 1U);
    EXPECT_EQ(clip.seqKeys[0], 2U);
    ASSERT_EQ(clip.seq.size(), 4U);
    // Every frame's draws, the ones before the rebuild made from the
    // moves they were
    for (int i = 0; i < count; ++i) {
        const int32_t blob = clip.blobAt(0, double(i));
        ASSERT_GE(blob, 0) << i;
        const DrawCallList &got = kept.blobs[size_t(blob)];
        DrawCallList want = feed(i, count, a, i == 3 ? rebuilt : b);
        ASSERT_EQ(got.size(), 2U) << i;
        for (size_t k = 0; k < 2; ++k) {
            EXPECT_EQ(got[k].objectKey, 2U);
            EXPECT_EQ(got[k].mesh, want[k + 1].mesh) << i << " " << k;
            for (int e = 0; e < 16; ++e) {
                EXPECT_NEAR(got[k].model[e], want[k + 1].model[e], 1e-4) << i << " " << k;
            }
        }
    }
    EXPECT_EQ(clip.frameAt(1.4), 1U);
    EXPECT_EQ(clip.frameAt(1.6), 2U);
    EXPECT_EQ(clip.frameAt(-5.0), 0U);
    EXPECT_EQ(clip.frameAt(9.0), 3U);
}

TEST(MotionClipBaker, aSequenceHiddenAtAFrameHasNoDrawsThere)
{
    auto a = mesh(11), b = mesh(12), rebuilt = mesh(13);
    Kept kept;
    MotionClipBaker baker(kept.store());
    baker.frame(feed(0, 3, a, b));
    DrawCallList less = feed(1, 3, a, rebuilt);
    less.erase(less.begin() + 1, less.end());
    baker.frame(less);
    baker.frame(feed(2, 3, a, rebuilt));
    MotionClip clip = baker.finish(1, 0.0, 1.0);
    EXPECT_TRUE(clip.exact);
    ASSERT_EQ(clip.seqKeys.size(), 1U);
    EXPECT_GE(clip.seq[0], 0);
    EXPECT_EQ(clip.seq[1], -1);
    EXPECT_GE(clip.seq[2], 0);
}

TEST(MotionClipBaker, aSequenceWithNoRoomLeftIsLeftOutAndTheClipIsNotExact)
{
    auto a = mesh(11), b = mesh(12), rebuilt = mesh(13);
    Kept kept;
    kept.room = 2;
    MotionClipBaker baker(kept.store());
    baker.frame(feed(0, 4, a, b));
    baker.frame(feed(1, 4, a, rebuilt));
    baker.frame(feed(2, 4, a, b));
    baker.frame(feed(3, 4, a, rebuilt));
    MotionClip clip = baker.finish(1, 0.0, 1.0);
    EXPECT_FALSE(clip.exact);
    EXPECT_TRUE(clip.seqKeys.empty());
}

TEST(MotionClipBaker, drawsOfOneObjectMovingApartAreNoRigidMove)
{
    auto a = mesh(11), b = mesh(12);
    MotionClipBaker baker;
    baker.frame(feed(0, 3, a, b));
    DrawCallList apart = feed(1, 3, a, b);
    apart[2].model[13] += 0.5F;
    baker.frame(apart);
    MotionClip clip = baker.finish(1, 0.0, 1.0);
    EXPECT_FALSE(clip.exact);
    EXPECT_TRUE(clip.keys.empty());
}

TEST(MotionClip, whatIsWrittenIsRead)
{
    auto a = mesh(11), b = mesh(12);
    MotionClipBaker baker;
    for (int i = 0; i < 5; ++i) {
        baker.frame(feed(i, 5, a, b));
    }
    MotionClip clip = baker.finish(7, 1.5, 0.1);
    const std::string json = clip.toJson("\"Doc\"");
    EXPECT_NE(json.find("\"cmd\":\"clip\",\"doc\":\"Doc\""), std::string::npos);
    MotionClip read;
    ASSERT_TRUE(read.fromJson(json.c_str()));
    EXPECT_EQ(read.id, 7U);
    EXPECT_DOUBLE_EQ(read.start, 1.5);
    EXPECT_DOUBLE_EQ(read.step, 0.1);
    EXPECT_EQ(read.frames, 5U);
    EXPECT_EQ(read.keys, clip.keys);
    EXPECT_EQ(read.data, clip.data);
    EXPECT_FALSE(read.empty());
}

TEST(MotionClip, hiddenFramesAndSequencesAreWrittenAndRead)
{
    auto a = mesh(11), b = mesh(12), rebuilt = mesh(13);
    Kept kept;
    MotionClipBaker baker(kept.store());
    DrawCallList first = feed(0, 3, a, b);
    first.erase(first.begin());
    baker.frame(first);
    baker.frame(feed(1, 3, a, b));
    DrawCallList last = feed(2, 3, a, rebuilt);
    baker.frame(last);
    MotionClip clip = baker.finish(3, 0.0, 0.5);
    for (size_t i = 0; i < kept.blobs.size(); ++i) {
        clip.blobs.push_back(std::string(40, char('a' + i)));
        clip.blobSizes.push_back(uint32_t(100 + i));
    }
    ASSERT_FALSE(clip.shown.empty());
    ASSERT_FALSE(clip.seqKeys.empty());
    MotionClip read;
    ASSERT_TRUE(read.fromJson(clip.toJson("\"Doc\"").c_str()));
    EXPECT_EQ(read.keys, clip.keys);
    EXPECT_EQ(read.data, clip.data);
    EXPECT_EQ(read.shown, clip.shown);
    EXPECT_EQ(read.refFrame, clip.refFrame);
    EXPECT_EQ(read.refBlob, clip.refBlob);
    EXPECT_EQ(read.seqKeys, clip.seqKeys);
    EXPECT_EQ(read.seq, clip.seq);
    EXPECT_EQ(read.blobs, clip.blobs);
    EXPECT_EQ(read.blobSizes, clip.blobSizes);
    // A blob number past the table is no clip
    std::string json = clip.toJson("\"Doc\"");
    json = json.substr(0, json.find("\"blobs\":[")) + "\"blobs\":[],\"sizes\":[]}";
    EXPECT_FALSE(read.fromJson(json.c_str()));
}

TEST(MotionClipBlob, theDrawsOfABlobAreTheDrawsItWasMadeOf)
{
    auto owned = std::make_shared<MeshData>();
    static const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    static const float normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    static const int32_t indices[3] = {0, 1, 2};
    owned->cacheId = 77;
    owned->numVertices = 3;
    owned->positions = positions;
    owned->normals = normals;
    owned->triangleIndices = indices;
    owned->numTriangleIndices = 3;
    double m[16];
    placement(0.5, 1, 2, 3, m);
    DrawCallList draws {draw(5, m, owned)};
    draws[0].indexCount = 3;
    draws[0].material.diffuse = 0x336699FF;
    const std::vector<uint8_t> bytes = motionBlobOf(draws);
    ASSERT_FALSE(bytes.empty());
    DrawCallList read;
    ASSERT_TRUE(motionBlobDraws(bytes.data(), bytes.size(), read));
    ASSERT_EQ(read.size(), 1U);
    EXPECT_EQ(read[0].objectKey, 5U);
    EXPECT_EQ(read[0].material.diffuse, 0x336699FFU);
    for (int e = 0; e < 16; ++e) {
        EXPECT_NEAR(read[0].model[e], m[e], 1e-6) << e;
    }
    ASSERT_TRUE(read[0].mesh);
    ASSERT_EQ(read[0].mesh->numVertices, 3);
    EXPECT_FLOAT_EQ(read[0].mesh->positions[3], 1.0F);
    // The same draws, the same bytes: what the content key relies on
    EXPECT_EQ(motionBlobOf(draws), bytes);
    EXPECT_FALSE(motionBlobDraws(bytes.data(), bytes.size() / 2, read));
}

TEST(MotionClip, noClipIsWrittenAndReadAsNone)
{
    MotionClip none;
    MotionClip read;
    read.id = 9;
    ASSERT_TRUE(read.fromJson(none.toJson("\"Doc\"").c_str()));
    EXPECT_TRUE(read.empty());
    EXPECT_EQ(read.id, 0U);
    // Data that is not the size the keys and frames say is no clip
    EXPECT_FALSE(read.fromJson(
        "{\"cmd\":\"clip\",\"doc\":\"D\",\"id\":3,\"start\":0,\"step\":1,\"frames\":2,"
        "\"keys\":[\"0000000000000002\"],\"data\":\"AAAA\"}"));
    EXPECT_TRUE(read.empty());
}

TEST(MotionClock, itCountsAsThePlayerDoes)
{
    MotionClock c;
    c.playing = true;
    c.start = 1.0;
    c.end = 3.0;
    c.raw = 2.0;
    c.speed = 2.0;
    c.time = 2.0;
    // Once: to the end and no further
    c.loop = MotionClock::Once;
    EXPECT_DOUBLE_EQ(c.timeAfter(0.25), 2.5);
    EXPECT_DOUBLE_EQ(c.timeAfter(5.0), 3.0);
    // Loop: from the start again
    c.loop = MotionClock::Loop;
    EXPECT_DOUBLE_EQ(c.timeAfter(0.75), 1.5);
    // PingPong: back from the end
    c.loop = MotionClock::PingPong;
    EXPECT_DOUBLE_EQ(c.timeAfter(0.75), 2.5);
    EXPECT_DOUBLE_EQ(c.timeAfter(1.75), 1.5);
    // Paused: the Time it was told
    c.playing = false;
    c.time = 1.25;
    EXPECT_DOUBLE_EQ(c.timeAfter(4.0), 1.25);
}

TEST(MotionClock, whatIsWrittenIsRead)
{
    MotionClock c;
    c.clip = 4;
    c.active = true;
    c.playing = true;
    c.time = 1.25;
    c.raw = 7.25;
    c.speed = 0.5;
    c.start = 1.0;
    c.end = 3.0;
    c.loop = MotionClock::PingPong;
    c.version = 123456789012ULL;
    c.baseActive = true;
    c.baseTime = 1.125;
    MotionClock read;
    const std::string json = c.toJson("\"Doc\"");
    EXPECT_NE(json.find("\"cmd\":\"time\""), std::string::npos);
    read.fromJson(json.c_str());
    EXPECT_EQ(read.clip, 4U);
    EXPECT_TRUE(read.active);
    EXPECT_TRUE(read.playing);
    EXPECT_DOUBLE_EQ(read.time, 1.25);
    EXPECT_DOUBLE_EQ(read.raw, 7.25);
    EXPECT_DOUBLE_EQ(read.speed, 0.5);
    EXPECT_DOUBLE_EQ(read.start, 1.0);
    EXPECT_DOUBLE_EQ(read.end, 3.0);
    EXPECT_EQ(read.loop, int(MotionClock::PingPong));
    EXPECT_EQ(read.version, 123456789012ULL);
    EXPECT_TRUE(read.baseActive);
    EXPECT_DOUBLE_EQ(read.baseTime, 1.125);
}

// NOLINTEND(readability-magic-numbers)
