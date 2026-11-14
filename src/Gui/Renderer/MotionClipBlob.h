// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef RENDER_MOTIONCLIPBLOB_H
#define RENDER_MOTIONCLIPBLOB_H

// The draws of one object at one frame of a motion clip, as the bytes a
// viewer fetches (MotionClip.h, docs/Animation.md sec 9.1): a scene
// snapshot holding only them, in the self-contained layout a bundled
// capture has -- meshes, materials and textures inline -- so a viewer
// reads it with nothing else in hand. Its content key is the SHA-1 of
// the bytes, as every chunk the scene server holds.

#include <cstdint>
#include <vector>

#include "SceneDump.h"

namespace Render
{

/// The blob of \a draws; empty when they would not serialize
inline std::vector<uint8_t> motionBlobOf(const DrawCallList &draws)
{
    SceneSnapshot snap;
    snap.scene = draws;
    std::vector<uint8_t> out;
    if (!saveSceneSnapshot(out, snap))
        out.clear();
    return out;
}

/// The draws a blob holds; false when it is not one
inline bool motionBlobDraws(const void *data, size_t size, DrawCallList &out)
{
    SceneSnapshot snap;
    if (!loadSceneSnapshot(data, size, snap))
        return false;
    if (snap.finalize)
        snap.finalize(snap);
    out = std::move(snap.scene);
    return true;
}

}  // namespace Render

#endif  // RENDER_MOTIONCLIPBLOB_H
