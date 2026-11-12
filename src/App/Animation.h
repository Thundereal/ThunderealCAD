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
 * Time evaluates the tracks and writes the targets under AnimatedWrites: no
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

    bool stepped() const
    {
        return layout.groups.empty();
    }
    /// The count of components after the property: 0 for a whole track
    int depth() const;
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
    /// Label, mute, lock, extrapolation and rotation mode of `options`
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
    virtual bool layout(const AnimationTrack& track, Anim::Layout& layout) const = 0;
    virtual bool read(const AnimationTrack& track, std::vector<double>& nums) const = 0;
    virtual bool write(const AnimationTrack& track, const std::vector<double>& nums) = 0;
    virtual std::string describe(const AnimationTrack& track) const = 0;

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
    /// The end of the range: End, or the last key's time when End is 0
    double endTime() const;
    /// Each frame's time from Start to endTime() at FrameRate
    std::vector<double> frames() const;

    /// Parse `path` relative to this Animation
    ObjectIdentifier parsePath(const std::string& path) const;
    /// The track on `path`, made if missing and `create`; throws when the
    /// target cannot be keyed
    int trackOf(const ObjectIdentifier& path, bool create);
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
    /// Whether Record skips `prop` (Output, Transient, Exclude ...)
    bool recordSkips(const DocumentObject& obj, const Property& prop) const;
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
    Rest& restOf(const Target& target);
    void dropRest(const DocumentObject* obj, const Property* prop);
    void connectDocument();
    void evaluateAt(double t);

    std::map<const Property*, std::unique_ptr<Rest>> _rest;
    std::vector<fastsignals::scoped_connection> _conns;
    bool _evaluating = false;
    bool _pending = false;
    bool _saving = false;
    bool _recordingKey = false;
    void onBeforeRecord(const DocumentObject& obj, const Property& prop);
    void onRecord(const DocumentObject& obj, const Property& prop);
};

}  // namespace App

#endif  // APP_ANIMATION_H
