// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 ****************************************************************************/

#ifndef APP_ANIMATION_H
#define APP_ANIMATION_H

/** Key frame animation of any property (docs/Animation.md sections 3 to 5).
 *
 * An App::Animation holds every track and key in one property, Tracks. Its
 * Time evaluates the tracks and writes the targets under a WriteScope of Animation: no
 * undo step, no log row, no modified document. What a target held before
 * the first write is its rest value, the design's, and it is what Stop puts
 * back, what a save writes and what a transaction records as the value
 * before a user's edit.
 */

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "AnimationCurve.h"
#include "DocumentObject.h"
#include "ObjectIdentifier.h"
#include "PropertyLinks.h"
#include "PropertyStandard.h"
#include "PropertyUnits.h"

namespace App
{

class Animation;

namespace Anim
{
/// The names the file and Python use; fromName() throws on an unknown one
AppExport const char* nameOf(Interp value);
AppExport const char* nameOf(EaseType value);
AppExport const char* nameOf(EaseMode value);
AppExport const char* nameOf(HandleType value);
AppExport const char* nameOf(Extrap value);
AppExport const char* nameOf(RotationMode value);
AppExport void fromName(const char* name, Interp& value);
AppExport void fromName(const char* name, EaseType& value);
AppExport void fromName(const char* name, EaseMode& value);
AppExport void fromName(const char* name, HandleType& value);
AppExport void fromName(const char* name, Extrap& value);
AppExport void fromName(const char* name, RotationMode& value);
}  // namespace Anim

/// One key of a track
struct AppExport AnimationKey
{
    int id = 0;          ///< stable within its track
    Anim::Key key;       ///< time, numbers, interpolation, easing, handles
    std::string text;    ///< a stepped key: the property's own saved XML

    bool operator==(const AnimationKey& o) const
    {
        return id == o.id && key == o.key && text == o.text;
    }
};

/// What evaluating a track does (docs/Animation.md 3.4)
enum class AnimationTrackState : std::uint8_t
{
    Ok,          ///< writes
    Unresolved,  ///< the target is gone: skipped and kept
    Mismatch,    ///< the target has another type now: skipped
    Driven,      ///< bound to an expression: written and warned
    Solved,      ///< a placement an Assembly solves: written and warned
};

AppExport const char* toString(AnimationTrackState state);

/** Another Animation placed in this one's time (docs/Animation.md sec 7):
 * a track of kind "Strip", no keys of its own
 */
struct AppExport AnimationStrip
{
    enum Blend : std::uint8_t
    {
        Replace,  ///< its values written over what is below
        Add,      ///< its change from its first key added to what is below
    };
    enum Start : std::uint8_t
    {
        At,     ///< at `offset`
        After,  ///< `offset` after the strip before it ends (the first: after Start)
        With,   ///< `offset` after the strip before it starts
        Click,  ///< as After, and playing stops there for a click: a presentation
    };
    std::string child;  ///< the Animation played, by its name in the document
    double offset = 0;
    double scale = 1;   ///< its time stretched by
    int repeat = 1;
    bool reverse = false;
    Blend blend = Replace;
    Start start = After;

    bool operator==(const AnimationStrip& o) const
    {
        return child == o.child && offset == o.offset && scale == o.scale && repeat == o.repeat
            && reverse == o.reverse && blend == o.blend && start == o.start;
    }
};

AppExport const char* toString(AnimationStrip::Blend blend);
AppExport const char* toString(AnimationStrip::Start start);
AppExport void fromString(const char* name, AnimationStrip::Blend& blend);
AppExport void fromString(const char* name, AnimationStrip::Start& start);

struct AppExport AnimationTrack
{
    int id = 0;                   ///< stable, never reused in the document
    std::string kind = "Property";
    ObjectIdentifier target;      ///< Box.Placement, Box.Placement.Base.z ...
    std::string handlerData;      ///< kinds other than Property
    std::string valueType;        ///< the target's type when first keyed
    std::string unit;             ///< a Quantity sub-path's unit
    std::string label;
    bool mute = false;
    bool lock = false;
    Anim::Extrap before = Anim::Extrap::Constant;
    Anim::Extrap after = Anim::Extrap::Constant;
    Anim::RotationMode rotation = Anim::RotationMode::Shortest;
    Anim::Layout layout;          ///< from valueType and rotation; empty: stepped
    std::vector<AnimationKey> keys;  ///< sorted by time, one per time
    int nextKeyId = 1;
    AnimationStrip strip;            ///< kind "Strip"

    bool stepped() const
    {
        return layout.groups.empty();
    }
    /// The count of components after the property: 0 for a whole track
    int depth() const;
    /// How Python and the timeline name it: the target's path, or
    /// "Camera:<data>" for a handler's track
    std::string pathString() const;
    /// Index of the key at `t`, or -1
    int find(double t) const;
    /// Index of the key with `id`, or -1
    int findId(int id) const;
    /// The curve of the numbered keys, ready to evaluate
    void makeCurve(Anim::Curve& curve) const;

    bool operator==(const AnimationTrack& o) const;
};

/// A key of a track, as the edits name them
struct AnimationKeyRef
{
    int track = 0;
    int key = 0;
};

/** Every track and key of an Animation, as one value.
 *
 * A PropertyXLinkContainer: each target object is a dependency in the Hidden
 * scope, which makes no edge in the dependency graph, so an expression that
 * reads the Animation's Time from an object it drives is no cycle, while the
 * document still knows of the reference (cross-document targets, deletion).
 *
 * Each edit is atomic: one aboutToSetValue(), one hasSetValue(), so one
 * undo step for a command.
 */
class AppExport PropertyAnimationTracks: public PropertyXLinkContainer
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    PropertyAnimationTracks();
    ~PropertyAnimationTracks() override;

    const std::vector<AnimationTrack>& getValues() const
    {
        return _tracks;
    }
    int getNextId() const
    {
        return _nextId;
    }
    /// For ADD_PROPERTY: no tracks
    void setValue()
    {}
    void setValues(std::vector<AnimationTrack>&& tracks, int nextId);

    const AnimationTrack* getTrack(int id) const;
    /// The track on exactly this path, or null
    const AnimationTrack* findTrack(const ObjectIdentifier& path) const;

    /// A new track on `track.target`, its keys and options as given; the id
    /// is assigned and returned. A track on the same path is an error.
    int addTrack(AnimationTrack&& track);
    void removeTrack(int id);
    /// Move a track to `index` in the timeline's order
    void moveTrack(int id, int index);
    /// Insert `key` into `track`; a key at the same time is replaced and
    /// keeps its id. Returns the key's id.
    int setKey(int track, AnimationKey&& key);
    void removeKey(int track, int key);
    /// Move keys in time; one landing on another key's time replaces it
    void moveKeys(const std::vector<AnimationKeyRef>& keys, double dt);
    void setInterp(const std::vector<AnimationKeyRef>& keys, Anim::Interp interp,
                   const Anim::Easing* easing = nullptr);
    void setHandles(AnimationKeyRef key, const Anim::Handle& left, const Anim::Handle& right);
    /// Label, mute, lock, extrapolation, rotation mode and a strip's options of `options`
    void setTrackOptions(int track, const AnimationTrack& options);

    PyObject* getPyObject() override;
    void setPyObject(PyObject*) override;

    /// Every reference is Hidden: given only when `all` asks for those
    void getLinksTo(std::vector<ObjectIdentifier>& identifiers, DocumentObject* obj,
                    const char* subname = nullptr, bool all = false) const override;
    /// Nothing to adjust: no reference makes an edge, so none makes a cycle
    bool adjustLink(const std::set<DocumentObject*>&) override
    {
        return false;
    }
    /// Tracks stay on the object a link replaces in a group (not yet re-pointed)
    Property* CopyOnLinkReplace(const DocumentObject*, DocumentObject*,
                                DocumentObject*) const override
    {
        return nullptr;
    }

    void Save(Base::Writer& writer) const override;
    void Restore(Base::XMLReader& reader) override;
    void afterRestore() override;

    Property* Copy() const override;
    void Paste(const Property& from) override;
    bool isSame(const Property& other) const override;
    unsigned int getMemSize() const override;

    /// The layout of numbers a track of `valueType` keys, or false: stepped
    static bool layoutOf(const std::string& valueType, Anim::RotationMode rotation,
                         Anim::Layout& layout);

protected:
    void onBreakLink(App::DocumentObject* obj) override;
    void hasSetValue() override;

private:
    AnimationTrack* track(int id);
    void updateDeps();

    std::vector<AnimationTrack> _tracks;
    int _nextId = 1;
};

/** App's side of a track kind other than Property (docs/Animation.md 3.6),
 * registered by its kind name. The Gui registers "Camera".
 */
class AppExport AnimationTrackHandler
{
public:
    virtual ~AnimationTrackHandler() = default;
    /// The layout of a track's numbers, from its handler data
    virtual bool layout(const AnimationTrack& track, Anim::Layout& layout) const = 0;
    /// The value now, to key, for `anim`'s track
    virtual bool read(const Animation& anim, const AnimationTrack& track,
                      std::vector<double>& nums) const = 0;
    /// Show `nums`; the first write after activation keeps what it replaces
    virtual bool write(Animation& anim, const AnimationTrack& track,
                       const std::vector<double>& nums) = 0;
    /// The timeline's label
    virtual std::string describe(const AnimationTrack& track) const = 0;
    /// The handler data for a new track; `hint` is what the caller named
    virtual std::string newTrackData(const std::string& hint) const
    {
        return hint;
    }
    /// Put back what the first write replaced: the Animation stops
    virtual void deactivate(Animation& anim, const AnimationTrack& track)
    {
        (void)anim;
        (void)track;
    }

    static void registerHandler(const char* kind, std::shared_ptr<AnimationTrackHandler> handler);
    static AnimationTrackHandler* get(const std::string& kind);
};

class AppExport Animation: public DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::Animation);

public:
    PropertyAnimationTracks Tracks;
    PropertyTime Time;
    PropertyTime Start;
    PropertyTime End;
    PropertyFloat FrameRate;
    PropertyFloat Speed;
    PropertyEnumeration Loop;
    PropertyEnumeration Recompute;
    PropertyStringList Exclude;
    PropertyBool Active;
    PropertyBool Recording;
    PropertyBool Cache;

    enum RecomputeMode
    {
        RecomputeTouched = 0,
        RecomputeNever = 1,
        RecomputeFull = 2,
    };

    Animation();
    ~Animation() override;

    short mustExecute() const override
    {
        return 0;
    }
    DocumentObjectExecReturn* execute() override;
    PyObject* getPyObject() override;
    const char* getViewProviderName() const override
    {
        return "Gui::ViewProviderAnimation";
    }

    /// Make this the document's active Animation and evaluate at Time
    void activate();
    /// Put the rest values back; recompute what that touches unless
    /// `recompute` is false. The Animation stays the document's chosen one.
    void deactivate(bool recompute = true);
    bool isActive() const
    {
        return Active.getValue();
    }
    /// Write every track's value at Time (only when active)
    void evaluate();

    /// The value of `track` at `t`, as Python, without writing
    Py::Object value(int track, double t) const;
    /// The state of `track` now
    AnimationTrackState state(int track) const;
    /// The end of the range: End, or the last key's or strip's end when End is 0
    double endTime() const;

    /// Where a strip lies in this Animation's time
    struct StripSpan
    {
        int track = 0;
        Animation* child = nullptr;
        double start = 0;       ///< here, seconds
        double end = 0;         ///< here: start + length x scale x repeat
        double childStart = 0;  ///< the child's own Start
        double length = 0;      ///< the child's own range
    };
    /// The strips in order, placed; a strip whose Animation is gone is left out
    std::vector<StripSpan> stripSpans() const;
    /// Where playing stops for a click: the starts of the On click strips
    std::vector<double> clickPoints() const;
    /// Make `child` a strip of this one; returns its track
    int addStrip(Animation* child, const AnimationStrip& options);
    /// Each frame's time from Start to endTime() at FrameRate
    std::vector<double> frames() const;
    /// The frame `t` is, when it is one: Start + frame / FrameRate
    bool frameAt(double t, long& frame) const;

    /** The frame cache (docs/Animation.md sec 4): with Cache on, what the
     * recompute of a frame wrote is kept -- shapes with the meshes they
     * were shown with, placements, whatever an expression set -- and the
     * next time Time is that frame it is put back instead of recomputed.
     * Any change of the design drops every frame. What memory holds is
     * bounded (CacheLimitMB of Preferences/Animation, 512): over it the
     * frames shown longest ago go to files in the document's transient
     * directory, themselves bounded (CacheDiskLimitMB, 4096; 0 for none).
     */
    struct CacheInfo
    {
        std::vector<double> times;   ///< the frames kept, in order
        std::vector<double> onDisk;  ///< those of them that are on disk only
        std::size_t bytes = 0;       ///< what memory holds of them, estimated
        std::size_t diskBytes = 0;   ///< what their files hold
        long stored = 0;             ///< frames recomputed and kept, ever
        long replayed = 0;           ///< frames put back instead, ever
        long spilled = 0;            ///< frames moved out of memory onto disk, ever
        long loaded = 0;             ///< frames read back from disk, ever
    };
    CacheInfo cacheInfo() const;
    void clearCache();
    /// Whether frames are kept: Cache, and a Recompute that recomputes
    bool isCaching() const;
    /// Whether an active Animation of `doc`, or of any document when
    /// null, keeps its frames: the shapes shown are then meshed exact
    static bool cachingIn(const Document* doc = nullptr);

    /// Parse `path` relative to this Animation
    ObjectIdentifier parsePath(const std::string& path) const;
    /// The track on `path`, made if missing and `create`; throws when the
    /// target cannot be keyed
    int trackOf(const ObjectIdentifier& path, bool create);
    /** The track of a handler's `kind` (docs/Animation.md 3.6) whose data
     * is the handler's for `hint` -- for "Camera", a 3D view -- made if
     * missing and `create`. Throws when no handler of the kind is
     * registered, as in a process with no GUI.
     */
    int handlerTrackOf(const std::string& kind, const std::string& hint, bool create);
    /** Key `track` at `t` with `value`, or with the target's current value
     * when `value` is null or None. A key already at `t` is replaced and
     * keeps its id. Returns the key's id.
     */
    int setKey(int track, double t, PyObject* value = nullptr,
               const Anim::Interp* interp = nullptr, const Anim::Easing* easing = nullptr);
    /// Key rotations of `track` in `mode`, its keys converted (3.13)
    void setRotationMode(int track, Anim::RotationMode mode);

    /** The value a transaction records as `prop`'s value before a user's
     * edit: the rest value while an Animation holds one for it, else null.
     * Called by TransactionObject::setProperty(), once per property and
     * transaction; the write that follows is the design's, so the next
     * evaluation takes the property's value then as its rest value.
     */
    static const Property* restValueOf(const Property& prop);

    /// What the property editor marks on a row (docs/Animation.md sec 6)
    enum class KeyState : std::uint8_t
    {
        None,      ///< no track on the path
        Animated,  ///< a track, no key at Time
        Keyed,     ///< a key at Time
        Changed,   ///< the value shown is not the track's at Time: changed, not keyed
    };
    KeyState keyState(const ObjectIdentifier& path) const;

    /** Key the property at `path` at Time with its value now: the track on
     * the path, made if missing, and each part track of the same property
     * whose piece is not the curve's at Time (docs/Animation.md sec 6). The
     * key the Insert key command and Record make. Returns the key's id.
     */
    int keyNow(const ObjectIdentifier& path);
    /// Whether Record skips `prop` of `obj`, or of its view provider when
    /// `view` (Output, Transient, Exclude ...)
    bool recordSkips(const DocumentObject& obj, const Property& prop, bool view = false) const;

    /** Record's hooks for a property of `owner`'s view provider, which the
     * document's change signals do not carry: the view provider calls them
     * before and after a change of its own property.
     */
    static void recordViewBefore(const DocumentObject& owner, const Property& prop);
    static void recordViewAfter(const DocumentObject& owner, const Property& prop);
    /// The time of the key before or after Time on any track, or Time
    double nextKeyTime(bool forward) const;

protected:
    void onChanged(const Property* prop) override;
    void onDocumentRestored() override;
    void unsetupObject() override;
    void onSettingDocument() override;

private:
    struct Target;
    struct Rest;
    bool resolve(const AnimationTrack& track, Target& target) const;
    bool readValue(const AnimationTrack& track, const Target& target, PyObject* value,
                   AnimationKey& key) const;
    void writeTrack(const AnimationTrack& track, const Target& target, double t);
    /// A track of an Add strip: its change from its first key added to `from`
    void writeTrackAdd(const AnimationTrack& track, const Target& target, double t,
                       const Property& from);
    Rest& restOf(const Target& target);
    void dropRest(const DocumentObject* obj, const Property* prop);
    void connectDocument();
    void evaluateAt(double t);
    struct Frame;
    struct Capture;
    bool replayFrame(long frame);
    void storeFrame(long frame, Capture& capture, double cost);
    void dropFrame(long frame);
    bool spillFrame(long index, Frame& frame);
    bool loadFrame(Frame& frame);
    std::string cacheDir() const;
    void trimCache();

    std::map<const Property*, std::unique_ptr<Rest>> _rest;
    std::vector<fastsignals::scoped_connection> _conns;
    bool _evaluating = false;
    bool _pending = false;
    bool _saving = false;
    bool _recordingKey = false;
    std::map<long, std::unique_ptr<Frame>> _frames;
    Capture* _capture = nullptr;
    std::size_t _cacheBytes = 0;
    std::size_t _cacheDiskBytes = 0;
    double _cacheDiskSeconds = 0;       ///< spent writing and reading frames
    std::size_t _cacheDiskWork = 0;     ///< the bytes that took
    std::size_t _cacheHeapMark = 0;     ///< the heap when the last frame was kept
    std::size_t _cacheMeasured = 0;     ///< bytes of the frames measured
    std::size_t _cacheMeasuredFrames = 0;
    long _cacheSpilled = 0;
    long _cacheLoaded = 0;
    std::uint64_t _cacheTick = 0;
    long _cacheStored = 0;
    long _cacheReplayed = 0;
    void onBeforeRecord(const DocumentObject& obj, const Property& prop, bool view = false);
    void onRecord(const DocumentObject& obj, const Property& prop, bool view = false);
};

}  // namespace App

#endif  // APP_ANIMATION_H
