// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef RENDER_MOTIONCLIPBLOB_H
#define RENDER_MOTIONCLIPBLOB_H

// The draws of one object at one frame of a motion clip, as the bytes a
// viewer fetches (MotionClip.h, docs/Animation.md sec 9.1): a scene
// snapshot holding only them. Its content key is the SHA-1 of the bytes,
// as every chunk the scene server holds.
//
// Two layouts. Self-contained, as a bundled capture is -- meshes,
// materials and textures inline -- read with nothing else in hand. Or
// with its meshes out of band (MotionBlobMeshes): each mesh a chunk of
// its own, in the very encoding a publish gives it, so a mesh that
// recurs -- the same shape at many frames, moved; a height keyed back
// and forth; the publish's own mesh -- is one chunk however many blobs
// name it, fetched once and stored once (MotionBlobRead).

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "SceneDump.h"

namespace Render
{

/// Where a clip's blobs put their meshes: chunks keyed by content.
///
/// A blob is written against what is known, and what it adds stays
/// pending until the writer either commits it (the blob is kept) or
/// drops it (the blob is over the limit): a mesh only a dropped blob
/// named must not be named as held by a later one.
struct MotionBlobMeshes
{
    struct Known
    {
        std::string key;
        uint32_t size = 0;
    };
    /// What the kept blobs named, by the mesh's cacheId
    std::map<uint64_t, Known> committed;
    /// What the blob being written added
    std::map<uint64_t, Known> pending;
    /// The bytes of the chunks it added, by key
    std::map<std::string, std::vector<uint8_t>> fresh;

    /// The chunks the blob just written added, held from now on
    std::map<std::string, std::vector<uint8_t>> commit()
    {
        committed.insert(pending.begin(), pending.end());
        pending.clear();
        std::map<std::string, std::vector<uint8_t>> out = std::move(fresh);
        fresh.clear();
        return out;
    }
    void drop()
    {
        pending.clear();
        fresh.clear();
    }
};

/// The blob of \a draws; empty when they would not serialize. With \a
/// meshes, the meshes are out of band, and the ones not already known
/// are left pending in it.
inline std::vector<uint8_t> motionBlobOf(const DrawCallList &draws,
                                         MotionBlobMeshes *meshes = nullptr)
{
    SceneSnapshot snap;
    snap.scene = draws;
    if (meshes) {
        meshes->drop();
        snap.meshBlobs.reuse = [meshes](uint64_t id, uint32_t &size) -> std::string {
            auto it = meshes->committed.find(id);
            if (it == meshes->committed.end()) {
                it = meshes->pending.find(id);
                if (it == meshes->pending.end())
                    return {};
            }
            size = it->second.size;
            return it->second.key;
        };
        snap.meshBlobs.store = [meshes](uint64_t id, const std::string &key,
                                        std::vector<uint8_t> &&chunk) {
            meshes->pending[id] = {key, uint32_t(chunk.size())};
            meshes->fresh.emplace(key, std::move(chunk));
        };
    }
    std::vector<uint8_t> out;
    if (!saveSceneSnapshot(out, snap)) {
        out.clear();
        if (meshes)
            meshes->drop();
    }
    return out;
}

/// A blob being read: its draws, once every mesh chunk it names is in.
///
/// The draws hold their meshes from the start and a chunk is parsed
/// into them as it lands, so they are whole exactly when nothing is
/// outstanding; a self-contained blob is complete as soon as it loads.
struct MotionBlobRead
{
    SceneSnapshot snap;

    /// False when the bytes are not a blob
    bool load(const void *data, size_t size)
    {
        snap = SceneSnapshot();
        return loadSceneSnapshot(data, size, snap);
    }
    /// The chunks still wanted, a key with its size each, once a key
    std::vector<std::pair<std::string, uint32_t>> wanted() const
    {
        std::vector<std::pair<std::string, uint32_t>> out;
        for (const auto &entry : snap.deferredChunks) {
            if (!entry.fill)
                continue;
            bool seen = false;
            for (const auto &w : out)
                seen = seen || w.first == entry.key;
            if (!seen)
                out.emplace_back(entry.key, entry.size);
        }
        return out;
    }
    bool wants(const std::string &key) const
    {
        for (const auto &entry : snap.deferredChunks)
            if (entry.fill && entry.key == key)
                return true;
        return false;
    }
    /// The bytes of \a key, parsed into every mesh naming it; false when
    /// they are not the mesh
    bool fill(const std::string &key, const void *data, size_t size)
    {
        bool ok = true;
        for (auto &entry : snap.deferredChunks) {
            if (!entry.fill || entry.key != key)
                continue;
            ok = entry.fill(snap, data, size) && ok;
            entry.fill = nullptr;
        }
        return ok;
    }
    bool complete() const
    {
        for (const auto &entry : snap.deferredChunks)
            if (entry.fill)
                return false;
        return true;
    }
    /// The draws, when complete
    bool finish(DrawCallList &out)
    {
        if (!complete())
            return false;
        if (snap.finalize)
            snap.finalize(snap);
        out = std::move(snap.scene);
        return true;
    }
};

/// The draws a self-contained blob holds; false when it is not one
inline bool motionBlobDraws(const void *data, size_t size, DrawCallList &out)
{
    MotionBlobRead read;
    return read.load(data, size) && read.finish(out);
}

}  // namespace Render

#endif  // RENDER_MOTIONCLIPBLOB_H
