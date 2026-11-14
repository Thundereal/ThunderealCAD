// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef GUI_SCENESERVECLIP_H
#define GUI_SCENESERVECLIP_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include <QObject>
#include <QTimer>

#include <FCGlobal.h>

#include "Renderer/MotionClip.h"

namespace App
{
class Animation;
class Document;
class Property;
}  // namespace App

namespace Gui
{

/** A served document's Animation as its clients play it by themselves
 * (docs/Animation.md sec 9, "Streaming").
 *
 * When the player starts an Animation of the document, the clip is baked:
 * the Animation is stepped through its range and each object's pose read
 * from the feed the source would publish (Render::MotionClipBaker). The
 * clients are sent the clip and, on every change of the player's state and
 * once a second while it plays, the clock (Render::MotionClock). Each
 * publish is named to them with the Time its pose was taken at, which is
 * what a client moves the draws from.
 *
 * With that a publish for what the Animation writes says nothing new, so
 * the source asks changed() before it schedules one. It is held only for
 * a write the Animation makes while it plays, and only when the clip is
 * exact -- the sweep changed nothing in the feed that a track does not
 * say; every other change publishes as it always did, and a real edit
 * makes the clip stale. GUI thread.
 */
class SceneServeClip: public QObject
{
    Q_OBJECT

public:
    struct Host
    {
        /// The document's group on the wire, and the same as a JSON string
        std::string group;
        std::string quotedGroup;
        /// Traverse the served graph and give the feed; null when it cannot
        std::function<const Render::DrawCallList*()> capture;
        /// Ask the source for a publish
        std::function<void()> publish;
        /// True while a client is fed from every publish and cannot play
        /// the clip -- a Cycles stream, which traces the scene each
        /// publish states: nothing is held then. Null: no such client.
        std::function<bool()> unclipped;
    };

    SceneServeClip(App::Document* doc, Host host, QObject* parent = nullptr);
    ~SceneServeClip() override;

    /** A property of the document or of one of its view providers changed.
     * True when the clients need no publish for it: they play it by the
     * clip, or the clip is being baked.
     */
    bool changed(const App::Property& prop);
    /// Objects came or went: the clip is stale
    void invalidate();
    /// The source published \a version
    void published(uint64_t version);
    /// Clients came or went
    void syncClients();

    bool baking() const
    {
        return isBaking;
    }
    const Render::MotionClip& clip() const
    {
        return theClip;
    }
    /// How many publishes were held since the clip was baked
    unsigned held() const
    {
        return heldCount;
    }
    /// What the clip's blobs hold, bytes
    size_t blobs() const
    {
        return blobBytes;
    }
    /** A click a viewer made on an object it had moved by the clip
     * \a clipId to \a time: the ray (\a origin, \a dir, world) moved by
     * that object's move from \a time to the Time the host is at, so that
     * it meets the object where the host has it as it met it where the
     * viewer drew it. False, the ray untouched, when the clip is not this
     * one, the object no track of it, or no Animation poses the document.
     */
    bool movePick(uint32_t clipId, double time, uint64_t key, float origin[3],
                  float dir[3]) const;

private:
    App::Animation* animation() const;
    bool playing() const;
    bool holding() const;
    void onPlayerState();
    void bake(App::Animation* anim);
    void announce();
    Render::MotionClock clock() const;

    App::Document* doc;
    Host host;
    Render::MotionClip theClip;
    std::string clipJson;
    /// The clip id each client was last sent
    std::map<uint64_t, uint32_t> told;
    uint32_t serial = 0;
    bool valid = false;
    bool isBaking = false;
    bool announced = false;
    unsigned heldCount = 0;
    size_t blobBytes = 0;
    uint64_t baseVersion = 0;
    bool baseActive = false;
    double baseTime = 0;
    QTimer heartbeat;
    QTimer rebake;
};

}  // namespace Gui

#endif  // GUI_SCENESERVECLIP_H
