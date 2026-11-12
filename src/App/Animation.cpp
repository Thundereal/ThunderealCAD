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

#include "PreCompiled.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <set>
#include <unordered_map>

#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/PlacementPy.h>
#include <Base/QuantityPy.h>
#include <Base/Reader.h>
#include <Base/Rotation.h>
#include <Base/RotationPy.h>
#include <Base/Tools.h>
#include <Base/VectorPy.h>
#include <Base/Writer.h>

#include "Animation.h"
#include "AnimationPy.h"
#include "Application.h"
#include "AutoTransaction.h"
#include "WriteScope.h"
#include "Document.h"
#include "PropertyContainerPy.h"
#include "PropertyGeo.h"

FC_LOG_LEVEL_INIT("Animation", true, true)

using namespace App;
using namespace App::Anim;

// --------------------------------------------------------------------------
// Names, numbers

namespace
{

const char* InterpNames[] = {"Constant", "Linear", "Bezier", "Easing", "Python"};
const char* EaseTypeNames[] = {"Quad", "Cubic", "Quart", "Quint", "Sine",
                               "Expo", "Circ",  "Elastic", "Back", "Bounce"};
const char* EaseModeNames[] = {"In", "Out", "InOut", "OutIn"};
const char* HandleNames[] = {"AutoClamped", "Auto", "Vector", "Aligned", "Free"};
const char* ExtrapNames[] = {"Constant", "Linear", "Cycle", "CycleOffset", "PingPong"};
const char* RotationNames[] = {"Shortest", "Turns", "Euler"};
const char* StateNames[] = {"Ok", "Unresolved", "Mismatch", "Driven", "Solved"};
const char* BlendNames[] = {"Replace", "Add"};
const char* StartNames[] = {"At", "After", "With", "Click"};
const char* LoopModes[] = {"Once", "Loop", "PingPong", nullptr};
const char* RecomputeModes[] = {"Touched", "Never", "Full", nullptr};

template<class E, std::size_t N>
E enumOf(const char* (&names)[N], const char* name, const char* what)
{
    for (std::size_t i = 0; i < N; ++i) {
        if (std::strcmp(names[i], name) == 0) {
            return static_cast<E>(i);
        }
    }
    FC_THROWM(Base::ValueError, "unknown " << what << " '" << name << "'");
}

template<class E, std::size_t N>
const char* nameOf(const char* (&names)[N], E value)
{
    auto i = static_cast<std::size_t>(value);
    return i < N ? names[i] : "?";
}

/// The shortest text that reads back as `v`
std::string number(double v)
{
    char buf[40];
    for (int p = 15; p <= 17; ++p) {
        std::snprintf(buf, sizeof(buf), "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) {
            break;
        }
    }
    return buf;
}

std::string numbers(const std::vector<double>& v)
{
    std::string s;
    for (double d : v) {
        if (!s.empty()) {
            s += ' ';
        }
        s += number(d);
    }
    return s;
}

std::vector<double> parseNumbers(const char* s)
{
    std::vector<double> res;
    char* end = nullptr;
    for (;;) {
        double d = std::strtod(s, &end);
        if (end == s) {
            break;
        }
        res.push_back(d);
        s = end;
    }
    return res;
}

/// "S3 Q4": the groups of a layout, a letter for the kind and a size
std::string layoutText(const Layout& layout)
{
    std::string s;
    for (auto& g : layout.groups) {
        if (!s.empty()) {
            s += ' ';
        }
        s += "SDQC"[static_cast<int>(g.kind)];
        s += std::to_string(g.size);
    }
    return s;
}

void parseLayout(const char* s, Layout& layout)
{
    layout.clear();
    std::istringstream in(s);
    std::string word;
    while (in >> word) {
        const char* kinds = "SDQC";
        const char* k = std::strchr(kinds, word[0]);
        int size = std::atoi(word.c_str() + 1);
        if (!k || size <= 0) {
            FC_THROWM(Base::ValueError, "invalid animation layout '" << s << "'");
        }
        layout.groups.push_back({static_cast<Kind>(k - kinds), size});
    }
}

std::string saveText(const Property& prop)
{
    Base::StringWriter writer;
    prop.Save(writer);
    return writer.getString();
}

void restoreText(Property& prop, const std::string& text)
{
    std::istringstream in("<Value>" + text + "</Value>");
    Base::XMLReader reader("<animation>", in);
    reader.readElement("Value");
    prop.Restore(reader);
}

std::unique_ptr<Property> makeProperty(const std::string& type, const std::string& unit)
{
    Base::Type t = Base::Type::fromName(type.c_str());
    if (t.isBad() || !t.isDerivedFrom(Property::getClassTypeId())) {
        return {};
    }
    std::unique_ptr<Property> prop(static_cast<Property*>(t.createInstance()));
    if (auto q = freecad_dynamic_cast<PropertyQuantity>(prop.get())) {
        if (!unit.empty()) {
            q->setUnit(Base::Unit(unit));
        }
    }
    return prop;
}

/// Who holds a rest value for a property, for restValueOf()
std::unordered_map<const Property*, Animation*>& restOwners()
{
    static std::unordered_map<const Property*, Animation*> owners;
    return owners;
}

/// The Animations recording: a view provider's every change asks, so the
/// answer when none is must cost nothing
std::set<Animation*>& recorders()
{
    static std::set<Animation*> set;
    return set;
}

std::map<std::string, std::shared_ptr<AnimationTrackHandler>>& handlers()
{
    static std::map<std::string, std::shared_ptr<AnimationTrackHandler>> map;
    return map;
}

}  // namespace

const char* App::toString(AnimationTrackState state)
{
    return nameOf(StateNames, state);
}

const char* App::toString(AnimationStrip::Blend blend)
{
    return nameOf(BlendNames, blend);
}

const char* App::toString(AnimationStrip::Start start)
{
    return nameOf(StartNames, start);
}

void App::fromString(const char* name, AnimationStrip::Blend& blend)
{
    blend = enumOf<AnimationStrip::Blend>(BlendNames, name, "strip blend");
}

void App::fromString(const char* name, AnimationStrip::Start& start)
{
    start = enumOf<AnimationStrip::Start>(StartNames, name, "strip start");
}

#define ANIM_NAMES(_type, _names, _what)                                                       \
    const char* App::Anim::nameOf(_type value)                                                 \
    {                                                                                          \
        return ::nameOf(_names, value);                                                        \
    }                                                                                          \
    void App::Anim::fromName(const char* name, _type& value)                                   \
    {                                                                                          \
        value = enumOf<_type>(_names, name, _what);                                            \
    }

ANIM_NAMES(Interp, InterpNames, "interpolation")
ANIM_NAMES(EaseType, EaseTypeNames, "easing")
ANIM_NAMES(EaseMode, EaseModeNames, "easing mode")
ANIM_NAMES(HandleType, HandleNames, "handle type")
ANIM_NAMES(Extrap, ExtrapNames, "extrapolation")
ANIM_NAMES(RotationMode, RotationNames, "rotation mode")

// --------------------------------------------------------------------------
// AnimationTrack

int AnimationTrack::depth() const
{
    return std::max(0, target.numSubComponents() - 1);
}

std::string AnimationTrack::pathString() const
{
    if (kind == "Property") {
        return target.toString();
    }
    if (kind == "Strip") {
        return kind + ":" + strip.child;
    }
    return handlerData.empty() ? kind : kind + ":" + handlerData;
}

int AnimationTrack::find(double t) const
{
    auto it = std::lower_bound(keys.begin(), keys.end(), t - TimeEpsilon,
                               [](const AnimationKey& k, double v) { return k.key.t < v; });
    if (it != keys.end() && std::fabs(it->key.t - t) <= TimeEpsilon) {
        return static_cast<int>(it - keys.begin());
    }
    return -1;
}

int AnimationTrack::findId(int id) const
{
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void AnimationTrack::makeCurve(Curve& curve) const
{
    curve.layout = layout;
    curve.before = before;
    curve.after = after;
    curve.keys.clear();
    curve.keys.reserve(keys.size());
    for (auto& k : keys) {
        curve.keys.push_back(k.key);
    }
    curve.prepare();
}

bool AnimationTrack::operator==(const AnimationTrack& o) const
{
    return id == o.id && kind == o.kind && target == o.target && handlerData == o.handlerData
        && valueType == o.valueType && unit == o.unit && label == o.label && mute == o.mute
        && lock == o.lock && before == o.before && after == o.after && rotation == o.rotation
        && layout == o.layout && keys == o.keys && nextKeyId == o.nextKeyId
        && strip == o.strip;
}

// --------------------------------------------------------------------------
// AnimationTrackHandler

void AnimationTrackHandler::registerHandler(const char* kind,
                                            std::shared_ptr<AnimationTrackHandler> handler)
{
    if (handler) {
        handlers()[kind] = std::move(handler);
    }
    else {
        handlers().erase(kind);
    }
}

AnimationTrackHandler* AnimationTrackHandler::get(const std::string& kind)
{
    auto it = handlers().find(kind);
    return it == handlers().end() ? nullptr : it->second.get();
}

// --------------------------------------------------------------------------
// PropertyAnimationTracks

TYPESYSTEM_SOURCE(App::PropertyAnimationTracks, App::PropertyXLinkContainer)

PropertyAnimationTracks::PropertyAnimationTracks() = default;
PropertyAnimationTracks::~PropertyAnimationTracks() = default;

void PropertyAnimationTracks::setValues(std::vector<AnimationTrack>&& tracks, int nextId)
{
    aboutToSetValue();
    _tracks = std::move(tracks);
    _nextId = nextId;
    for (auto& t : _tracks) {
        _nextId = std::max(_nextId, t.id + 1);
    }
    hasSetValue();
}

const AnimationTrack* PropertyAnimationTracks::getTrack(int id) const
{
    for (auto& t : _tracks) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

AnimationTrack* PropertyAnimationTracks::track(int id)
{
    for (auto& t : _tracks) {
        if (t.id == id) {
            return &t;
        }
    }
    FC_THROWM(Base::IndexError, "no animation track " << id);
}

const AnimationTrack* PropertyAnimationTracks::findTrack(const ObjectIdentifier& path) const
{
    const std::string& s = path.toString();
    for (auto& t : _tracks) {
        if (t.kind == "Property" && t.target.toString() == s) {
            return &t;
        }
    }
    return nullptr;
}

int PropertyAnimationTracks::addTrack(AnimationTrack&& t)
{
    if (t.kind == "Property" && findTrack(t.target)) {
        FC_THROWM(Base::ValueError, "a track on " << t.target.toString() << " exists");
    }
    aboutToSetValue();
    t.id = _nextId++;
    for (auto& k : t.keys) {
        if (k.id <= 0) {
            k.id = t.nextKeyId++;
        }
    }
    _tracks.push_back(std::move(t));
    int id = _tracks.back().id;
    hasSetValue();
    return id;
}

void PropertyAnimationTracks::removeTrack(int id)
{
    for (auto it = _tracks.begin(); it != _tracks.end(); ++it) {
        if (it->id == id) {
            aboutToSetValue();
            _tracks.erase(it);
            hasSetValue();
            return;
        }
    }
}

void PropertyAnimationTracks::moveTrack(int id, int index)
{
    for (std::size_t i = 0; i < _tracks.size(); ++i) {
        if (_tracks[i].id != id) {
            continue;
        }
        index = std::clamp(index, 0, static_cast<int>(_tracks.size()) - 1);
        if (index == static_cast<int>(i)) {
            return;
        }
        aboutToSetValue();
        AnimationTrack t = std::move(_tracks[i]);
        _tracks.erase(_tracks.begin() + static_cast<long>(i));
        _tracks.insert(_tracks.begin() + index, std::move(t));
        hasSetValue();
        return;
    }
}

int PropertyAnimationTracks::setKey(int trackId, AnimationKey&& key)
{
    track(trackId);  // throws when missing, before anything changes
    aboutToSetValue();
    auto& t = *track(trackId);
    int i = t.find(key.key.t);
    if (i >= 0) {
        key.id = t.keys[i].id;
        key.key.t = t.keys[i].key.t;
        t.keys[i] = std::move(key);
    }
    else {
        if (key.id <= 0 || t.findId(key.id) >= 0) {
            key.id = t.nextKeyId;
        }
        t.nextKeyId = std::max(t.nextKeyId, key.id + 1);
        auto it = std::upper_bound(
            t.keys.begin(), t.keys.end(), key.key.t,
            [](double v, const AnimationKey& k) { return v < k.key.t; });
        i = static_cast<int>(it - t.keys.begin());
        t.keys.insert(it, std::move(key));
    }
    int id = t.keys[i].id;
    hasSetValue();
    return id;
}

void PropertyAnimationTracks::removeKey(int trackId, int keyId)
{
    auto& t = *track(trackId);
    int i = t.findId(keyId);
    if (i < 0) {
        return;
    }
    aboutToSetValue();
    t.keys.erase(t.keys.begin() + i);
    hasSetValue();
}

void PropertyAnimationTracks::moveKeys(const std::vector<AnimationKeyRef>& refs, double dt)
{
    if (refs.empty() || dt == 0) {
        return;
    }
    for (auto& r : refs) {
        track(r.track);
    }
    aboutToSetValue();
    std::map<int, std::vector<int>> byTrack;
    for (auto& r : refs) {
        byTrack[r.track].push_back(r.key);
    }
    for (auto& [tid, ids] : byTrack) {
        auto& t = *track(tid);
        std::vector<AnimationKey> moved;
        for (auto it = t.keys.begin(); it != t.keys.end();) {
            if (std::find(ids.begin(), ids.end(), it->id) != ids.end()) {
                moved.push_back(std::move(*it));
                moved.back().key.t += dt;
                it = t.keys.erase(it);
            }
            else {
                ++it;
            }
        }
        // A key landing on another key's time replaces it, as in Blender
        for (auto& m : moved) {
            int i = t.find(m.key.t);
            if (i >= 0) {
                t.keys.erase(t.keys.begin() + i);
            }
            auto it = std::upper_bound(
                t.keys.begin(), t.keys.end(), m.key.t,
                [](double v, const AnimationKey& k) { return v < k.key.t; });
            t.keys.insert(it, std::move(m));
        }
    }
    hasSetValue();
}

void PropertyAnimationTracks::setInterp(const std::vector<AnimationKeyRef>& refs, Interp interp,
                                        const Easing* easing)
{
    if (refs.empty()) {
        return;
    }
    for (auto& r : refs) {
        track(r.track);
    }
    aboutToSetValue();
    for (auto& r : refs) {
        auto& t = *track(r.track);
        int i = t.findId(r.key);
        if (i < 0) {
            continue;
        }
        t.keys[i].key.interp = interp;
        if (easing) {
            t.keys[i].key.easing = *easing;
        }
    }
    hasSetValue();
}

void PropertyAnimationTracks::setHandles(AnimationKeyRef ref, const Handle& left,
                                         const Handle& right)
{
    auto& t0 = *track(ref.track);
    if (t0.findId(ref.key) < 0) {
        FC_THROWM(Base::IndexError, "no key " << ref.key << " in track " << ref.track);
    }
    aboutToSetValue();
    auto& t = *track(ref.track);
    auto& k = t.keys[t.findId(ref.key)].key;
    k.left = left;
    k.right = right;
    hasSetValue();
}

void PropertyAnimationTracks::setTrackOptions(int trackId, const AnimationTrack& o)
{
    track(trackId);
    aboutToSetValue();
    auto& t = *track(trackId);
    t.label = o.label;
    t.mute = o.mute;
    t.lock = o.lock;
    t.before = o.before;
    t.after = o.after;
    if (t.kind == "Strip") {
        auto child = t.strip.child;
        t.strip = o.strip;
        t.strip.child = child;  // a strip's Animation is the strip
    }
    if (t.rotation != o.rotation || t.layout != o.layout) {
        // A new rotation mode comes with the keys' numbers in it
        t.rotation = o.rotation;
        t.layout = o.layout;
        t.keys = o.keys;
    }
    hasSetValue();
}

bool PropertyAnimationTracks::layoutOf(const std::string& valueType, RotationMode rotation,
                                       Layout& layout)
{
    layout.clear();
    layout.rotation = rotation;
    auto prop = makeProperty(valueType, std::string());
    return prop && prop->getAnimLayout(layout) && !layout.groups.empty();
}

void PropertyAnimationTracks::updateDeps()
{
    std::map<DocumentObject*, bool> deps;
    auto owner = freecad_dynamic_cast<DocumentObject>(getContainer());
    for (auto& t : _tracks) {
        if (t.kind == "Strip" && owner && owner->getDocument()) {
            if (auto child = owner->getDocument()->getObject(t.strip.child.c_str())) {
                deps[child] = true;  // Hidden as the targets are
            }
            continue;
        }
        if (t.kind != "Property") {
            continue;
        }
        try {
            for (auto& [obj, props] : t.target.getDep(false)) {
                if (obj) {
                    deps[obj] = true;  // Hidden: no edge in the graph
                }
            }
        }
        catch (Base::Exception& e) {
            FC_LOG("no dependency for " << t.target.toString() << ": " << e.what());
        }
    }
    PropertyXLinkContainer::updateDeps(std::move(deps));
}

void PropertyAnimationTracks::hasSetValue()
{
    auto owner = freecad_dynamic_cast<DocumentObject>(getContainer());
    if (owner && !owner->isRestoring() && !testFlag(LinkRestoring)) {
        updateDeps();
    }
    PropertyXLinkContainer::hasSetValue();
}

void PropertyAnimationTracks::afterRestore()
{
    PropertyXLinkContainer::afterRestore();
    updateDeps();
}

void PropertyAnimationTracks::getLinksTo(std::vector<ObjectIdentifier>& identifiers,
                                         DocumentObject* obj, const char*, bool all) const
{
    if (!all || !obj) {
        return;
    }
    for (auto& t : _tracks) {
        if (t.kind == "Property" && t.target.getDocumentObject() == obj) {
            identifiers.emplace_back(*this);
            return;
        }
    }
}

void PropertyAnimationTracks::onBreakLink(DocumentObject*)
{
    // Its tracks are kept: unresolved until an undo brings the object back
}

void PropertyAnimationTracks::Save(Base::Writer& writer) const
{
    writer.Stream() << writer.ind() << "<Tracks nextId=\"" << _nextId << "\" count=\""
                    << _tracks.size();
    if (!_XLinks.empty()) {
        writer.Stream() << "\" xlink=\"1";
    }
    writer.Stream() << "\">\n";
    writer.incInd();
    if (!_XLinks.empty()) {
        PropertyXLinkContainer::Save(writer);
    }
    for (auto& t : _tracks) {
        auto& s = writer.Stream();
        // An unresolved target has no persistent form: kept as it reads
        std::string target = t.target.toPersistentString();
        if (target.empty()) {
            target = t.target.toString();
        }
        s << writer.ind() << "<Track id=\"" << t.id << "\" kind=\"" << encodeAttribute(t.kind)
          << "\" target=\"" << encodeAttribute(target)
          << "\" valueType=\"" << encodeAttribute(t.valueType) << "\"";
        if (!t.unit.empty()) {
            s << " unit=\"" << encodeAttribute(t.unit) << "\"";
        }
        if (!t.label.empty()) {
            s << " label=\"" << encodeAttribute(t.label) << "\"";
        }
        if (!t.handlerData.empty()) {
            s << " handler=\"" << encodeAttribute(t.handlerData) << "\"";
        }
        if (t.mute) {
            s << " mute=\"1\"";
        }
        if (t.lock) {
            s << " lock=\"1\"";
        }
        if (t.kind == "Strip") {
            auto& st = t.strip;
            s << " child=\"" << encodeAttribute(st.child) << "\" start=\"" << toString(st.start)
              << "\" offset=\"" << number(st.offset) << "\" scale=\"" << number(st.scale)
              << "\" repeat=\"" << st.repeat << "\" reverse=\"" << (st.reverse ? 1 : 0)
              << "\" blend=\"" << toString(st.blend) << "\"";
        }
        s << " before=\"" << nameOf(ExtrapNames, t.before) << "\" after=\""
          << nameOf(ExtrapNames, t.after) << "\" rotation=\"" << nameOf(RotationNames, t.rotation)
          << "\" layout=\"" << layoutText(t.layout) << "\" nextKeyId=\"" << t.nextKeyId
          << "\" count=\"" << t.keys.size() << "\">\n";
        writer.incInd();
        for (auto& k : t.keys) {
            auto& key = k.key;
            s << writer.ind() << "<Key id=\"" << k.id << "\" t=\"" << number(key.t)
              << "\" interp=\"" << nameOf(InterpNames, key.interp) << "\"";
            if (key.interp == Interp::Easing) {
                s << " easing=\"" << nameOf(EaseTypeNames, key.easing.type) << "\" mode=\""
                  << nameOf(EaseModeNames, key.easing.mode) << "\" amplitude=\""
                  << number(key.easing.amplitude) << "\" period=\"" << number(key.easing.period)
                  << "\" overshoot=\"" << number(key.easing.overshoot) << "\"";
            }
            auto handle = [&](const char* side, const Handle& h) {
                if (h.type == HandleType::AutoClamped) {
                    return;
                }
                s << ' ' << side << "=\"" << nameOf(HandleNames, h.type) << "\"";
                if (h.type == HandleType::Aligned || h.type == HandleType::Free) {
                    s << ' ' << side << "dt=\"" << number(h.dt) << "\" " << side << "dv=\""
                      << numbers(h.dv) << "\"";
                }
            };
            handle("left", key.left);
            handle("right", key.right);
            if (t.stepped()) {
                s << " text=\"" << encodeAttribute(k.text) << "\"";
            }
            else {
                s << " v=\"" << numbers(key.v) << "\"";
            }
            s << "/>\n";
        }
        writer.decInd();
        s << writer.ind() << "</Track>\n";
    }
    writer.decInd();
    writer.Stream() << writer.ind() << "</Tracks>\n";
}

void PropertyAnimationTracks::Restore(Base::XMLReader& reader)
{
    auto owner = freecad_dynamic_cast<DocumentObject>(getContainer());
    reader.readElement("Tracks");
    int nextId = static_cast<int>(reader.getAttributeAsInteger("nextId"));
    auto count = reader.getAttributeAsUnsigned("count");
    if (reader.hasAttribute("xlink") && reader.getAttributeAsInteger("xlink")) {
        PropertyXLinkContainer::Restore(reader);
    }
    std::vector<AnimationTrack> tracks;
    tracks.reserve(count);
    for (unsigned long i = 0; i < count; ++i) {
        reader.readElement("Track");
        AnimationTrack t;
        t.id = static_cast<int>(reader.getAttributeAsInteger("id"));
        t.kind = reader.getAttribute("kind");
        const char* target = reader.getAttribute("target");
        if (t.kind == "Property" && owner) {
            try {
                t.target = ObjectIdentifier::parse(owner, target);
            }
            catch (Base::Exception& e) {
                FC_ERR(getFullName() << ": cannot read track target '" << target
                                          << "': " << e.what());
            }
        }
        t.valueType = reader.getAttribute("valueType");
        t.unit = reader.getAttribute("unit", "");
        t.label = reader.getAttribute("label", "");
        t.handlerData = reader.getAttribute("handler", "");
        t.mute = reader.getAttributeAsInteger("mute", "0") != 0;
        t.lock = reader.getAttributeAsInteger("lock", "0") != 0;
        if (t.kind == "Strip") {
            auto& st = t.strip;
            st.child = reader.getAttribute("child", "");
            fromString(reader.getAttribute("start", "After"), st.start);
            st.offset = reader.getAttributeAsFloat("offset", "0");
            st.scale = reader.getAttributeAsFloat("scale", "1");
            st.repeat = static_cast<int>(reader.getAttributeAsInteger("repeat", "1"));
            st.reverse = reader.getAttributeAsInteger("reverse", "0") != 0;
            fromString(reader.getAttribute("blend", "Replace"), st.blend);
        }
        t.before = enumOf<Extrap>(ExtrapNames, reader.getAttribute("before", "Constant"),
                                  "extrapolation");
        t.after = enumOf<Extrap>(ExtrapNames, reader.getAttribute("after", "Constant"),
                                 "extrapolation");
        t.rotation = enumOf<RotationMode>(RotationNames,
                                          reader.getAttribute("rotation", "Shortest"),
                                          "rotation mode");
        parseLayout(reader.getAttribute("layout", ""), t.layout);
        t.layout.rotation = t.rotation;
        t.nextKeyId = static_cast<int>(reader.getAttributeAsInteger("nextKeyId", "1"));
        auto keyCount = reader.getAttributeAsUnsigned("count", "0");
        int size = t.layout.size();
        for (unsigned long j = 0; j < keyCount; ++j) {
            reader.readElement("Key");
            AnimationKey k;
            k.id = static_cast<int>(reader.getAttributeAsInteger("id"));
            k.key.t = reader.getAttributeAsFloat("t");
            k.key.interp = enumOf<Interp>(InterpNames, reader.getAttribute("interp", "Linear"),
                                          "interpolation");
            if (reader.hasAttribute("easing")) {
                k.key.easing.type =
                    enumOf<EaseType>(EaseTypeNames, reader.getAttribute("easing"), "easing");
                k.key.easing.mode = enumOf<EaseMode>(
                    EaseModeNames, reader.getAttribute("mode", "InOut"), "easing mode");
                k.key.easing.amplitude = reader.getAttributeAsFloat("amplitude", "1");
                k.key.easing.period = reader.getAttributeAsFloat("period", "0.3");
                k.key.easing.overshoot = reader.getAttributeAsFloat("overshoot", "1.70158");
            }
            auto handle = [&](const std::string& side, Handle& h) {
                h.type = enumOf<HandleType>(
                    HandleNames, reader.getAttribute(side.c_str(), "AutoClamped"), "handle");
                if (reader.hasAttribute((side + "dt").c_str())) {
                    h.dt = reader.getAttributeAsFloat((side + "dt").c_str());
                    h.dv = parseNumbers(reader.getAttribute((side + "dv").c_str(), ""));
                }
            };
            handle("left", k.key.left);
            handle("right", k.key.right);
            if (t.stepped()) {
                k.text = reader.getAttribute("text", "");
            }
            else {
                k.key.v = parseNumbers(reader.getAttribute("v", ""));
                if (static_cast<int>(k.key.v.size()) != size) {
                    FC_ERR(getFullName() << ": key " << k.id << " of track " << t.id
                                              << " has " << k.key.v.size() << " numbers, not "
                                              << size);
                    k.key.v.resize(size, 0.0);
                }
            }
            t.nextKeyId = std::max(t.nextKeyId, k.id + 1);
            t.keys.push_back(std::move(k));
        }
        std::stable_sort(t.keys.begin(), t.keys.end(),
                         [](const AnimationKey& a, const AnimationKey& b) {
                             return a.key.t < b.key.t;
                         });
        reader.readEndElement("Track");
        tracks.push_back(std::move(t));
    }
    reader.readEndElement("Tracks");
    setValues(std::move(tracks), nextId);
}

Property* PropertyAnimationTracks::Copy() const
{
    auto p = new PropertyAnimationTracks;
    p->_tracks = _tracks;
    p->_nextId = _nextId;
    return p;
}

void PropertyAnimationTracks::Paste(const Property& from)
{
    auto& other = dynamic_cast<const PropertyAnimationTracks&>(from);
    std::vector<AnimationTrack> tracks = other._tracks;
    setValues(std::move(tracks), other._nextId);
}

bool PropertyAnimationTracks::isSame(const Property& other) const
{
    if (this == &other) {
        return true;
    }
    if (other.getTypeId() != getTypeId()) {
        return false;
    }
    auto& o = static_cast<const PropertyAnimationTracks&>(other);
    return _nextId == o._nextId && _tracks == o._tracks;
}

unsigned int PropertyAnimationTracks::getMemSize() const
{
    std::size_t size = sizeof(*this);
    for (auto& t : _tracks) {
        size += sizeof(t) + t.keys.size() * sizeof(AnimationKey);
        for (auto& k : t.keys) {
            size += k.key.v.size() * sizeof(double) + k.text.size();
        }
    }
    return static_cast<unsigned int>(size);
}

PyObject* PropertyAnimationTracks::getPyObject()
{
    Py::List list;
    for (auto& t : _tracks) {
        Py::Dict d;
        d.setItem("Id", Py::Long(t.id));
        d.setItem("Kind", Py::String(t.kind));
        d.setItem("Path", Py::String(t.pathString()));
        d.setItem("ValueType", Py::String(t.valueType));
        d.setItem("Label", Py::String(t.label));
        d.setItem("Mute", Py::Boolean(t.mute));
        d.setItem("Lock", Py::Boolean(t.lock));
        d.setItem("Extrapolation", Py::TupleN(Py::String(nameOf(ExtrapNames, t.before)),
                                              Py::String(nameOf(ExtrapNames, t.after))));
        d.setItem("Rotation", Py::String(nameOf(RotationNames, t.rotation)));
        Py::List keys;
        for (auto& k : t.keys) {
            keys.append(Py::TupleN(Py::Long(k.id), Py::Float(k.key.t),
                                   Py::String(nameOf(InterpNames, k.key.interp))));
        }
        d.setItem("Keys", keys);
        list.append(d);
    }
    return Py::new_reference_to(list);
}

void PropertyAnimationTracks::setPyObject(PyObject*)
{
    FC_THROWM(Base::TypeError, "tracks are edited by the Animation's methods (addKey, ...)");
}

// --------------------------------------------------------------------------
// Animation

struct Animation::Target
{
    AnimationTrackState state = AnimationTrackState::Unresolved;
    DocumentObject* obj = nullptr;          ///< the object the path names
    PropertyContainer* container = nullptr; ///< obj, or its view provider
    Property* prop = nullptr;               ///< whose value the track is (part of)
    Property* pathProp = nullptr;           ///< what the path resolves to
    int depth = 0;                          ///< components after prop: 0 for a whole track
};

struct Animation::Rest
{
    DocumentObject* obj = nullptr;
    std::unique_ptr<Property> rest;     ///< the design's value
    std::unique_ptr<Property> written;  ///< what the last evaluation left; null: the
                                        ///< value now is the design's
    bool touched = false;               ///< whether obj was touched before
};

PROPERTY_SOURCE(App::Animation, App::DocumentObject)

Animation::Animation()
{
    ADD_PROPERTY_TYPE(Tracks, (), "Animation", Prop_NoRecompute, "Every track and key");
    ADD_PROPERTY_TYPE(Time, (0.0), "Animation", Prop_NoRecompute,
                      "The time shown. Setting it evaluates the tracks");
    Time.setStatus(Property::NoModify, true);
    ADD_PROPERTY_TYPE(Start, (0.0), "Animation", Prop_NoRecompute, "Where the play range starts");
    ADD_PROPERTY_TYPE(End, (0.0), "Animation", Prop_NoRecompute,
                      "Where the play range ends; 0 for the last key's time");
    ADD_PROPERTY_TYPE(FrameRate, (30.0), "Animation", Prop_NoRecompute,
                      "Frames per second of playing and of export");
    ADD_PROPERTY_TYPE(Speed, (1.0), "Animation", Prop_NoRecompute, "Playing speed");
    Loop.setEnums(LoopModes);
    ADD_PROPERTY_TYPE(Loop, (0L), "Animation", Prop_NoRecompute, "What playing does at the end");
    Recompute.setEnums(RecomputeModes);
    ADD_PROPERTY_TYPE(Recompute, (0L), "Animation", Prop_NoRecompute,
                      "Touched: recompute what each frame touches.\n"
                      "Never: update the view only.\n"
                      "Full: recompute the whole document each frame");
    ADD_PROPERTY_TYPE(Exclude, (), "Animation", Prop_NoRecompute,
                      "Paths Record never keys; '*' patterns allowed");
    ADD_PROPERTY_TYPE(Active, (false), "Animation", Prop_NoRecompute,
                      "The document's playing Animation: at most one");
    Active.setStatus(Property::NoModify, true);
    ADD_PROPERTY_TYPE(Recording, (false), "Animation",
                      PropertyType(Prop_Transient | Prop_NoPersist | Prop_NoRecompute),
                      "Key every property changed");
    Recording.setStatus(Property::NoModify, true);
}

Animation::~Animation()
{
    // Nothing is written back: the document is going, and its file was
    // saved with the rest values
    auto& owners = restOwners();
    for (auto& v : _rest) {
        owners.erase(v.first);
    }
    recorders().erase(this);
}

DocumentObjectExecReturn* Animation::execute()
{
    return DocumentObject::StdReturn;
}

ObjectIdentifier Animation::parsePath(const std::string& path) const
{
    return ObjectIdentifier::parse(this, path);
}

bool Animation::resolve(const AnimationTrack& track, Target& target) const
{
    target = Target();
    if (track.kind == "Strip") {
        auto doc = getDocument();
        auto child = doc ? doc->getObject(track.strip.child.c_str()) : nullptr;
        if (child && child->isDerivedFrom(Animation::getClassTypeId()) && child != this) {
            target.state = AnimationTrackState::Ok;
            return true;
        }
        return false;
    }
    if (track.kind != "Property") {
        if (AnimationTrackHandler::get(track.kind)) {
            target.state = AnimationTrackState::Ok;
            return true;
        }
        return false;
    }
    auto obj = track.target.getDocumentObject();
    if (!obj || !obj->isAttachedToDocument() || obj->isRemoving()) {
        return false;
    }
    int ptype = 0;
    auto prop = track.target.getProperty(&ptype);
    if (!prop) {
        return false;
    }
    target.obj = obj;
    target.container = obj;
    target.pathProp = prop;
    target.prop = prop;
    auto comps = track.target.getPropertyComponents(1);
    target.depth = static_cast<int>(comps.size());
    // ViewObject is a pseudo property of a path (ObjectIdentifier answers
    // the object's Label in its place); the other pseudo ones, _shape and
    // the like, are no values to animate
    bool viewObject = prop == &obj->ViewObject;
    if (ptype != 0) {
        viewObject = track.target.getPropertyComponent(0).getName() == "ViewObject";
        if (!viewObject) {
            return false;
        }
    }
    if (viewObject) {
        if (comps.empty() || !comps[0].isSimple()) {
            return false;
        }
        // A view provider's property: the property itself, for its codec
        // and its rest value, not the Python object that reaches it
        Base::PyGILStateLocker lock;
        Py::Object vo = obj->ViewObject.getValue();
        if (!PyObject_TypeCheck(vo.ptr(), &PropertyContainerPy::Type)) {
            return false;
        }
        auto container = static_cast<PropertyContainerPy*>(vo.ptr())->getPropertyContainerPtr();
        auto vprop = container ? container->getPropertyByName(comps[0].getName().c_str())
                               : nullptr;
        if (!vprop) {
            return false;
        }
        target.container = container;
        target.prop = vprop;
        target.depth = static_cast<int>(comps.size()) - 1;
        target.pathProp = nullptr;
    }
    target.state = AnimationTrackState::Ok;
    if (target.depth == 0 && !track.valueType.empty()) {
        if (track.valueType != target.prop->getTypeId().getName()) {
            target.state = AnimationTrackState::Mismatch;
            return false;
        }
        if (!track.stepped()) {
            Layout now;
            now.rotation = track.rotation;
            if (!target.prop->getAnimLayout(now) || now != track.layout) {
                target.state = AnimationTrackState::Mismatch;  // a list of another size
                return false;
            }
        }
    }
    if (target.container == obj) {
        const char* name = target.prop->getName();
        for (auto& v : obj->ExpressionEngine.getExpressions()) {
            if (name && v.first.getPropertyName() == name) {
                target.state = AnimationTrackState::Driven;
                break;
            }
        }
    }
    return true;
}

AnimationTrackState Animation::state(int trackId) const
{
    auto track = Tracks.getTrack(trackId);
    if (!track) {
        FC_THROWM(Base::IndexError, "no animation track " << trackId);
    }
    Target target;
    resolve(*track, target);
    return target.state;
}

int Animation::trackOf(const ObjectIdentifier& path, bool create)
{
    if (auto t = Tracks.findTrack(path)) {
        return t->id;
    }
    if (!create) {
        FC_THROWM(Base::ValueError, "no animation track on " << path.toString());
    }
    AnimationTrack track;
    track.target = path;
    Target target;
    if (!resolve(track, target)) {
        FC_THROWM(Base::ValueError, "cannot animate " << path.toString() << ": "
                                                      << path.resolveErrorString());
    }
    if (target.obj == this) {
        FC_THROWM(Base::ValueError, "an Animation does not animate itself");
    }
    if (target.prop->testStatus(Property::Immutable)
        || target.prop->testStatus(Property::ReadOnly)
        || (target.container->getPropertyType(target.prop) & Prop_ReadOnly)) {
        FC_THROWM(Base::ValueError, "cannot animate read-only " << path.toString());
    }
    if (target.depth == 0) {
        track.valueType = target.prop->getTypeId().getName();
        track.layout.rotation = track.rotation;
        if (!target.prop->getAnimLayout(track.layout)) {
            track.layout.clear();  // stepped
        }
    }
    else {
        Base::PyGILStateLocker lock;
        Py::Object value;
        try {
            value = pyObjectFromAny(target.pathProp ? target.pathProp->getPathValue(path)
                                                     : path.getValue(true));
        }
        catch (Py::Exception&) {
            Base::PyException::ThrowException();
        }
        PyObject* v = value.ptr();
        if (PyObject_TypeCheck(v, &Base::QuantityPy::Type)) {
            track.valueType = PropertyQuantity::getClassTypeId().getName();
            track.unit = static_cast<Base::QuantityPy*>(v)->getQuantityPtr()->getUnit().getString();
        }
        else if (PyBool_Check(v)) {
            FC_THROWM(Base::TypeError,
                      "cannot animate " << path.toString() << ": a part that is a bool");
        }
        else if (PyFloat_Check(v)) {
            track.valueType = PropertyFloat::getClassTypeId().getName();
        }
        else if (PyLong_Check(v)) {
            track.valueType = PropertyInteger::getClassTypeId().getName();
        }
        else if (PyObject_TypeCheck(v, &Base::VectorPy::Type)) {
            track.valueType = PropertyVector::getClassTypeId().getName();
        }
        else if (PyObject_TypeCheck(v, &Base::RotationPy::Type)) {
            track.valueType = PropertyRotation::getClassTypeId().getName();
        }
        else if (PyObject_TypeCheck(v, &Base::PlacementPy::Type)) {
            track.valueType = PropertyPlacement::getClassTypeId().getName();
        }
        else {
            FC_THROWM(Base::TypeError, "cannot animate " << path.toString() << ": a part of type "
                                                         << Py_TYPE(v)->tp_name);
        }
        PropertyAnimationTracks::layoutOf(track.valueType, track.rotation, track.layout);
    }
    return Tracks.addTrack(std::move(track));
}

int Animation::handlerTrackOf(const std::string& kind, const std::string& hint, bool create)
{
    auto handler = AnimationTrackHandler::get(kind);
    if (!handler) {
        FC_THROWM(Base::RuntimeError, "no handler of " << kind << " tracks here");
    }
    std::string data = handler->newTrackData(hint);
    for (auto& t : Tracks.getValues()) {
        if (t.kind == kind && t.handlerData == data) {
            return t.id;
        }
    }
    if (!create) {
        FC_THROWM(Base::ValueError, "no " << kind << " track " << data);
    }
    AnimationTrack track;
    track.kind = kind;
    track.handlerData = data;
    track.valueType = kind;
    if (!handler->layout(track, track.layout)) {
        FC_THROWM(Base::ValueError, "no " << kind << " track " << data);
    }
    track.rotation = track.layout.rotation;
    return Tracks.addTrack(std::move(track));
}

bool Animation::readValue(const AnimationTrack& track, const Target& target, PyObject* value,
                          AnimationKey& key) const
{
    if (value == Py_None) {
        value = nullptr;
    }
    Base::PyGILStateLocker lock;
    try {
        if (target.depth == 0) {
            const Property* src = target.prop;
            std::unique_ptr<Property> temp;
            if (value) {
                temp.reset(target.prop->Copy());
                temp->setPyObject(value);
                src = temp.get();
            }
            if (track.stepped()) {
                key.text = saveText(*src);
                return true;
            }
            return src->getAnimValue(track.layout, key.key.v);
        }
        auto temp = makeProperty(track.valueType, track.unit);
        if (!temp) {
            return false;
        }
        if (value) {
            temp->setPyObject(value);
        }
        else {
            Py::Object v = pyObjectFromAny(target.pathProp
                                               ? target.pathProp->getPathValue(track.target)
                                               : track.target.getValue(true));
            temp->setPyObject(v.ptr());
        }
        return temp->getAnimValue(track.layout, key.key.v);
    }
    catch (Py::Exception&) {
        Base::PyException::ThrowException();
    }
    return false;
}

int Animation::setKey(int trackId, double t, PyObject* value, const Interp* interp,
                      const Easing* easing)
{
    auto track = Tracks.getTrack(trackId);
    if (!track) {
        FC_THROWM(Base::IndexError, "no animation track " << trackId);
    }
    if (track->kind != "Property") {
        auto handler = AnimationTrackHandler::get(track->kind);
        if (!handler) {
            FC_THROWM(Base::RuntimeError, "no handler of " << track->kind << " tracks here");
        }
        AnimationKey key;
        key.key.t = t;
        int i = track->find(t);
        if (i >= 0) {
            key.key = track->keys[i].key;
            key.key.v.clear();
        }
        if (value && value != Py_None) {
            Base::PyGILStateLocker lock;
            Py::Sequence seq(value);
            for (Py_ssize_t j = 0; j < seq.size(); ++j) {
                key.key.v.push_back(static_cast<double>(Py::Float(seq[j])));
            }
        }
        else if (!handler->read(*this, *track, key.key.v)) {
            FC_THROWM(Base::RuntimeError, "cannot read " << handler->describe(*track));
        }
        if (static_cast<int>(key.key.v.size()) != track->layout.size()) {
            FC_THROWM(Base::ValueError, handler->describe(*track) << " takes "
                                        << track->layout.size() << " numbers");
        }
        if (interp) {
            key.key.interp = *interp;
        }
        if (easing) {
            key.key.easing = *easing;
        }
        return Tracks.setKey(trackId, std::move(key));
    }
    Target target;
    if (!resolve(*track, target)) {
        FC_THROWM(Base::ValueError, "cannot key " << track->target.toString() << ": "
                                                  << App::toString(target.state));
    }
    AnimationKey key;
    key.key.t = t;
    int i = track->find(t);
    if (i >= 0) {
        // The options stay with a key replaced
        key.key = track->keys[i].key;
        key.key.v.clear();
    }
    if (!readValue(*track, target, value, key)) {
        FC_THROWM(Base::TypeError, "cannot read a value of " << track->target.toString());
    }
    if (interp) {
        key.key.interp = *interp;
    }
    if (easing) {
        key.key.easing = *easing;
    }
    return Tracks.setKey(trackId, std::move(key));
}

void Animation::setRotationMode(int trackId, RotationMode mode)
{
    auto track = Tracks.getTrack(trackId);
    if (!track) {
        FC_THROWM(Base::IndexError, "no animation track " << trackId);
    }
    if (track->rotation == mode) {
        return;
    }
    AnimationTrack t = *track;
    t.rotation = mode;
    Target target;
    std::unique_ptr<Property> temp;
    if (resolve(*track, target) && target.depth == 0) {
        temp.reset(target.prop->Copy());
    }
    else if (track->depth() > 0 || !track->stepped()) {
        temp = makeProperty(track->valueType, track->unit);
    }
    if (!track->stepped()) {
        if (!temp) {
            FC_THROWM(Base::TypeError, "cannot convert the keys of " << track->target.toString());
        }
        t.layout.clear();
        t.layout.rotation = mode;
        temp->getAnimLayout(t.layout);
        for (auto& k : t.keys) {
            std::vector<double> v;
            if (!temp->setAnimValue(track->layout, k.key.v) || !temp->getAnimValue(t.layout, v)) {
                FC_THROWM(Base::TypeError,
                          "cannot convert the keys of " << track->target.toString());
            }
            k.key.v = std::move(v);
            // Handles are per number, and the numbers are others now
            k.key.left.dv.clear();
            k.key.right.dv.clear();
        }
    }
    Tracks.setTrackOptions(trackId, t);
}

Animation::Rest& Animation::restOf(const Target& target)
{
    auto& r = _rest[target.prop];
    if (!r) {
        r = std::make_unique<Rest>();
        r->obj = target.obj;
        r->touched = target.obj->isTouched();
        restOwners()[target.prop] = this;
    }
    // The value now is the design's unless it is what was last written:
    // somebody else wrote it since -- an edit, an undo, a recompute
    if (!r->rest || !r->written || !r->written->isSame(*target.prop)) {
        r->rest.reset(target.prop->Copy());
    }
    return *r;
}

const Property* Animation::restValueOf(const Property& prop)
{
    if (WriteScope::causes() & WriteCause::Animation) {
        return nullptr;
    }
    auto& owners = restOwners();
    auto it = owners.find(&prop);
    if (it == owners.end()) {
        return nullptr;
    }
    auto rit = it->second->_rest.find(&prop);
    if (rit == it->second->_rest.end()) {
        return nullptr;
    }
    auto& r = *rit->second;
    const Property* res = nullptr;
    if (r.rest && r.written && r.written->isSame(prop)) {
        res = r.rest.get();
    }
    // What is written next is the design's: the next evaluation takes it
    // as the rest value
    r.written.reset();
    return res;
}

void Animation::dropRest(const DocumentObject* obj, const Property* prop)
{
    auto& owners = restOwners();
    for (auto it = _rest.begin(); it != _rest.end();) {
        if (it->first == prop || (obj && it->second->obj == obj)) {
            owners.erase(it->first);
            it = _rest.erase(it);
        }
        else {
            ++it;
        }
    }
}

void Animation::writeTrack(const AnimationTrack& track, const Target& target, double t)
{
    if (track.stepped()) {
        // The last key at or before t, else the first
        auto it = std::upper_bound(
            track.keys.begin(), track.keys.end(), t + TimeEpsilon,
            [](double v, const AnimationKey& k) { return v < k.key.t; });
        const auto& key = it == track.keys.begin() ? *it : *(it - 1);
        if (saveText(*target.prop) != key.text) {
            restoreText(*target.prop, key.text);
        }
        return;
    }
    Curve curve;
    track.makeCurve(curve);
    std::vector<double> nums;
    if (!curve.evaluate(t, nums)) {
        return;
    }
    if (target.depth == 0) {
        if (!target.prop->setAnimValue(track.layout, nums)) {
            FC_THROWM(Base::TypeError, "cannot write " << target.prop->getFullName());
        }
        return;
    }
    auto temp = makeProperty(track.valueType, track.unit);
    if (!temp || !temp->setAnimValue(track.layout, nums)) {
        FC_THROWM(Base::TypeError, "cannot write " << track.target.toString());
    }
    Base::PyGILStateLocker lock;
    try {
        Py::Object v(temp->getPyObject(), true);
        // A view provider's piece through the path itself: its owner's
        // path rules are the document object's
        if (target.pathProp) {
            target.pathProp->setPathValue(track.target, pyObjectToAny(v));
        }
        else {
            track.target.setValue(pyObjectToAny(v));
        }
    }
    catch (Py::Exception&) {
        Base::PyException::ThrowException();
    }
}

namespace
{

/// Whether two values of a layout are the same, a quaternion and its
/// negative being one rotation
bool sameNumbers(const Layout& layout, const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    auto close = [](double x, double y) {
        return std::fabs(x - y) <= 1e-7 * (1.0 + std::fabs(x) + std::fabs(y));
    };
    std::size_t i = 0;
    for (auto& g : layout.groups) {
        bool same = true;
        bool negated = g.kind == Kind::Quaternion;
        for (int j = 0; j < g.size && i + j < a.size(); ++j) {
            same = same && close(a[i + j], b[i + j]);
            negated = negated && close(a[i + j], -b[i + j]);
        }
        if (!same && !negated) {
            return false;
        }
        i += static_cast<std::size_t>(g.size);
    }
    return true;
}

/// A shell pattern, '*' and '?'
bool globMatch(const char* p, const char* s)
{
    if (*p == 0) {
        return *s == 0;
    }
    if (*p == '*') {
        return globMatch(p + 1, s) || (*s && globMatch(p, s + 1));
    }
    if (*s && (*p == '?' || *p == *s)) {
        return globMatch(p + 1, s + 1);
    }
    return false;
}

}  // namespace

Animation::KeyState Animation::keyState(const ObjectIdentifier& path) const
{
    auto track = Tracks.findTrack(path);
    if (!track || track->keys.empty()) {
        return KeyState::None;
    }
    double t = Time.getValue();
    KeyState res = track->find(t) >= 0 ? KeyState::Keyed : KeyState::Animated;
    // Changed only means something while the pose is on show
    Target target;
    if (!Active.getValue() || !resolve(*track, target) || !_rest.count(target.prop)) {
        return res;
    }
    // A whole track under part tracks is not the value on show by itself
    std::string prefix = track->target.toString() + ".";
    for (auto& other : Tracks.getValues()) {
        if (!other.keys.empty() && !other.mute
            && other.target.toString().compare(0, prefix.size(), prefix) == 0) {
            return res;
        }
    }
    try {
        AnimationKey now;
        if (!readValue(*track, target, nullptr, now)) {
            return res;
        }
        if (track->stepped()) {
            auto it = std::upper_bound(
                track->keys.begin(), track->keys.end(), t + TimeEpsilon,
                [](double v, const AnimationKey& k) { return v < k.key.t; });
            const auto& key = it == track->keys.begin() ? *it : *(it - 1);
            return key.text == now.text ? res : KeyState::Changed;
        }
        Curve curve;
        track->makeCurve(curve);
        std::vector<double> nums;
        if (curve.evaluate(t, nums) && !sameNumbers(track->layout, nums, now.key.v)) {
            return KeyState::Changed;
        }
    }
    catch (Base::Exception&) {
    }
    return res;
}

int Animation::keyNow(const ObjectIdentifier& path)
{
    // No evaluation between the keys: it would put the pieces back on
    // their curves before they are read. One after, when not Record's.
    bool outer = !_recordingKey;
    struct Restore
    {
        bool& flag;
        bool old;
        ~Restore()
        {
            flag = old;
        }
    } restore {_recordingKey, _recordingKey};
    _recordingKey = true;
    double t = Time.getValue();
    int id = trackOf(path, true);
    int key = setKey(id, t, nullptr);
    // The part tracks of the same property whose piece moved: without
    // their keys, applied after the whole, they would undo the pose just
    // keyed
    std::string prefix = path.toString() + ".";
    std::vector<int> parts;
    for (auto& other : Tracks.getValues()) {
        if (other.id != id && other.target.toString().compare(0, prefix.size(), prefix) == 0) {
            parts.push_back(other.id);
        }
    }
    for (int part : parts) {
        auto track = Tracks.getTrack(part);
        if (!track || track->keys.empty() || track->stepped()) {
            continue;
        }
        Target target;
        if (!resolve(*track, target)) {
            continue;
        }
        AnimationKey now;
        if (!readValue(*track, target, nullptr, now)) {
            continue;
        }
        Curve curve;
        track->makeCurve(curve);
        std::vector<double> nums;
        if (curve.evaluate(t, nums) && !sameNumbers(track->layout, nums, now.key.v)) {
            setKey(part, t, nullptr);
        }
    }
    if (outer && Active.getValue()) {
        _recordingKey = false;
        evaluate();
    }
    return key;
}

bool Animation::recordSkips(const DocumentObject& obj, const Property& prop, bool view) const
{
    if (&obj == this || obj.isDerivedFrom(Animation::getClassTypeId()) || !prop.getName()
        || obj.getDocument() != getDocument()) {
        return true;
    }
    auto type = prop.getType();
    if ((type & (Prop_Output | Prop_Transient | Prop_NoPersist | Prop_ReadOnly | Prop_Hidden))
        || prop.testStatus(Property::Output) || prop.testStatus(Property::Transient)
        || prop.testStatus(Property::PropNoPersist) || prop.testStatus(Property::ReadOnly)
        || prop.testStatus(Property::Hidden) || prop.testStatus(Property::Immutable)) {
        return true;
    }
    if (&prop == &obj.Label || &prop == &obj.Label2 || &prop == &obj.ExpressionEngine
        || &prop == &obj.ViewObject || prop.isDerivedFrom(PropertyLinkBase::getClassTypeId())) {
        return true;
    }
    // A view provider sets many of its own as it goes (a display mode, what
    // is selectable): what has numbers or a colour is a pose, Visibility the
    // one switch that is
    Layout viewLayout;
    if (view && !prop.getAnimLayout(viewLayout)
        && std::strcmp(prop.getName(), "Visibility") != 0) {
        return true;
    }
    Layout layout;
    if (!prop.getAnimLayout(layout) && !prop.isDerivedFrom(PropertyBool::getClassTypeId())
        && !prop.isDerivedFrom(PropertyEnumeration::getClassTypeId())) {
        return true;  // a shape, a string: not what a pose is made of
    }
    std::string path = std::string(obj.getNameInDocument()) + (view ? ".ViewObject." : ".")
        + prop.getName();
    for (auto& pattern : Exclude.getValues()) {
        if (globMatch(pattern.c_str(), path.c_str()) || globMatch(pattern.c_str(), prop.getName())) {
            return true;
        }
    }
    return false;
}

void Animation::onBeforeRecord(const DocumentObject& obj, const Property& prop, bool view)
{
    auto doc = getDocument();
    if (!Recording.getValue() || !Active.getValue() || _recordingKey || _evaluating
        || !doc || !(doc->writeEffects(prop) & WriteEffect::Key)
        || recordSkips(obj, prop, view)) {
        return;
    }
    // The value before the edit is the design's: kept as the rest value,
    // the edit being a pose
    auto& r = _rest[&prop];
    if (!r) {
        r = std::make_unique<Rest>();
        r->obj = const_cast<DocumentObject*>(&obj);
        r->touched = obj.isTouched();
        r->rest.reset(prop.Copy());
        restOwners()[&prop] = this;
    }
    else if (!r->written || !r->written->isSame(prop)) {
        r->rest.reset(prop.Copy());
    }
    r->written.reset(prop.Copy());
}

void Animation::onRecord(const DocumentObject& obj, const Property& prop, bool view)
{
    auto doc = getDocument();
    if (!Recording.getValue() || !Active.getValue() || _recordingKey || _evaluating
        || !doc || !(doc->writeEffects(prop) & WriteEffect::Key)
        || recordSkips(obj, prop, view)) {
        return;
    }
    Base::FlagToggler<bool> flag(_recordingKey);
    try {
        keyNow(parsePath(std::string(obj.getNameInDocument()) + (view ? ".ViewObject." : ".")
                         + prop.getName()));
    }
    catch (Base::Exception& e) {
        FC_ERR(getFullName() << ": cannot record " << prop.getFullName() << ": " << e.what());
        return;
    }
    // What it holds now is the pose keyed, not the design
    auto it = _rest.find(&prop);
    if (it != _rest.end()) {
        it->second->written.reset(prop.Copy());
    }
}

namespace
{
/// The view property a change is being made to: one set in turn while it
/// changes is the view provider's doing, not the user's, and not keyed
const Property* viewChanging = nullptr;
}  // namespace

void Animation::recordViewBefore(const DocumentObject& owner, const Property& prop)
{
    if (recorders().empty() || (WriteScope::causes() & WriteCause::Animation)) {
        return;
    }
    if (viewChanging && viewChanging != &prop) {
        return;
    }
    viewChanging = &prop;
    for (auto anim : std::vector<Animation*>(recorders().begin(), recorders().end())) {
        if (anim->getDocument() == owner.getDocument()) {
            anim->onBeforeRecord(owner, prop, true);
        }
    }
}

void Animation::recordViewAfter(const DocumentObject& owner, const Property& prop)
{
    if (recorders().empty() || (WriteScope::causes() & WriteCause::Animation)
        || viewChanging != &prop) {
        return;
    }
    viewChanging = nullptr;
    for (auto anim : std::vector<Animation*>(recorders().begin(), recorders().end())) {
        if (anim->getDocument() == owner.getDocument()) {
            anim->onRecord(owner, prop, true);
        }
    }
}

double Animation::nextKeyTime(bool forward) const
{
    double t = Time.getValue();
    double best = t;
    bool found = false;
    for (auto& track : Tracks.getValues()) {
        for (auto& k : track.keys) {
            double kt = k.key.t;
            if (forward ? kt > t + TimeEpsilon : kt < t - TimeEpsilon) {
                if (!found || (forward ? kt < best : kt > best)) {
                    best = kt;
                    found = true;
                }
            }
        }
    }
    return best;
}

void Animation::writeTrackAdd(const AnimationTrack& track, const Target& target, double t,
                              const Property& from)
{
    Curve curve;
    track.makeCurve(curve);
    std::vector<double> value;
    std::vector<double> cur;
    if (!curve.evaluate(t, value) || !from.getAnimValue(track.layout, cur)
        || cur.size() != value.size()) {
        writeTrack(track, target, t);
        return;
    }
    const auto& base = track.keys.front().key.v;
    std::size_t i = 0;
    for (auto& g : track.layout.groups) {
        auto n = static_cast<std::size_t>(g.size);
        if (g.kind == Kind::Scalar || g.kind == Kind::Colour) {
            for (std::size_t j = i; j < i + n; ++j) {
                cur[j] += value[j] - base[j];
            }
        }
        else if (g.kind == Kind::Quaternion) {
            // The turn from the first key to now, after what is below
            Base::Rotation q(value[i], value[i + 1], value[i + 2], value[i + 3]);
            Base::Rotation q0(base[i], base[i + 1], base[i + 2], base[i + 3]);
            Base::Rotation c(cur[i], cur[i + 1], cur[i + 2], cur[i + 3]);
            Base::Rotation r = (q * q0.inverse()) * c;
            r.getValue(cur[i], cur[i + 1], cur[i + 2], cur[i + 3]);
        }
        // A direction is not added to: what is below stands
        i += n;
    }
    if (target.depth == 0) {
        target.prop->setAnimValue(track.layout, cur);
    }
}

std::vector<Animation::StripSpan> Animation::stripSpans() const
{
    std::vector<StripSpan> res;
    // A file whose strips play each other: no deeper than this
    static thread_local int depth = 0;
    if (depth > 8) {
        return res;
    }
    struct Deeper
    {
        Deeper()
        {
            ++depth;
        }
        ~Deeper()
        {
            --depth;
        }
    } deeper;
    auto doc = getDocument();
    double prevStart = Start.getValue();
    double prevEnd = Start.getValue();
    for (auto& t : Tracks.getValues()) {
        if (t.kind != "Strip") {
            continue;
        }
        auto child = doc ? freecad_dynamic_cast<Animation>(doc->getObject(t.strip.child.c_str()))
                         : nullptr;
        if (!child || child == this) {
            continue;
        }
        StripSpan span;
        span.track = t.id;
        span.child = child;
        span.childStart = child->Start.getValue();
        span.length = std::max(0.0, child->endTime() - span.childStart);
        switch (t.strip.start) {
        case AnimationStrip::At:
            span.start = t.strip.offset;
            break;
        case AnimationStrip::With:
            span.start = prevStart + t.strip.offset;
            break;
        default:
            span.start = prevEnd + t.strip.offset;
            break;
        }
        span.end = span.start
            + span.length * std::max(1e-9, t.strip.scale) * std::max(1, t.strip.repeat);
        if (span.length > TimeEpsilon) {
            res.push_back(span);
        }
        prevStart = span.start;
        prevEnd = span.end;
    }
    return res;
}

std::vector<double> Animation::clickPoints() const
{
    std::vector<double> res;
    for (auto& span : stripSpans()) {
        auto t = Tracks.getTrack(span.track);
        if (t && t->strip.start == AnimationStrip::Click) {
            res.push_back(span.start);
        }
    }
    return res;
}

int Animation::addStrip(Animation* child, const AnimationStrip& options)
{
    if (!child || child == this || child->getDocument() != getDocument()) {
        FC_THROWM(Base::ValueError, "a strip is another Animation of the same document");
    }
    // A strip of a strip of this one would play this one inside itself
    std::vector<const Animation*> todo {child};
    for (std::size_t i = 0; i < todo.size() && i < 64; ++i) {
        for (auto& span : todo[i]->stripSpans()) {
            if (span.child == this) {
                FC_THROWM(Base::ValueError, child->getFullName() << " plays " << getFullName());
            }
            todo.push_back(span.child);
        }
    }
    AnimationTrack track;
    track.kind = "Strip";
    track.valueType = "Strip";
    track.strip = options;
    track.strip.child = child->getNameInDocument();
    return Tracks.addTrack(std::move(track));
}

void Animation::evaluate()
{
    evaluateAt(Time.getValue());
}

void Animation::evaluateAt(double t)
{
    auto doc = getDocument();
    if (!doc || !Active.getValue()) {
        return;
    }
    if (_evaluating || doc->isPerformingTransaction() || doc->testStatus(Document::Restoring)) {
        _pending = true;
        return;
    }
    _pending = false;
    Base::FlagToggler<bool> flag(_evaluating);
    WriteScope animated(WriteCause::Animation);

    struct Item
    {
        const AnimationTrack* track;
        Target target;
        double time;   ///< in the time of the Animation the track is of
        int layer;     ///< 0 this one's own, then each strip's in turn
        bool add;      ///< of an Add strip
    };
    // Grouped by property, in the timeline's order; in a group by layer --
    // this Animation's own tracks, then each strip's -- and in a layer the
    // whole track first, then the parts, shorter paths first
    // (docs/Animation.md 3.4, 7)
    std::vector<std::vector<Item>> groups;
    std::map<const Property*, std::size_t> index;
    int layers = 0;
    std::function<void(const Animation*, double, int, bool, int)> collect =
        [&](const Animation* owner, double time, int layer, bool add, int depth) {
            for (auto& track : owner->Tracks.getValues()) {
                if (track.mute || track.keys.empty() || track.kind == "Strip") {
                    continue;
                }
                Item item {&track, {}, time, layer, add};
                if (!resolve(track, item.target)) {
                    continue;
                }
                if (track.kind != "Property") {
                    auto handler = AnimationTrackHandler::get(track.kind);
                    Curve curve;
                    track.makeCurve(curve);
                    std::vector<double> nums;
                    if (handler && curve.evaluate(time, nums)) {
                        try {
                            handler->write(*this, track, nums);
                        }
                        catch (Base::Exception& e) {
                            FC_ERR(getFullName() << ": " << handler->describe(track) << ": "
                                                 << e.what());
                        }
                    }
                    continue;
                }
                auto res = index.emplace(item.target.prop, groups.size());
                if (res.second) {
                    groups.emplace_back();
                }
                groups[res.first->second].push_back(std::move(item));
            }
            // Its strips, each at its own time; one of them before its start
            // plays nothing, after its end holds its last pose
            if (depth > 8) {
                return;
            }
            for (auto& span : owner->stripSpans()) {
                auto st = owner->Tracks.getTrack(span.track);
                if (!st || st->mute || span.child == this || time < span.start - TimeEpsilon) {
                    continue;
                }
                double local = (time - span.start) / std::max(1e-9, st->strip.scale);
                double total = span.length * std::max(1, st->strip.repeat);
                double phase;
                if (local >= total) {
                    phase = span.length;
                }
                else {
                    phase = std::fmod(local, span.length);
                }
                if (st->strip.reverse) {
                    phase = span.length - phase;
                }
                collect(span.child, span.childStart + phase, ++layers,
                        st->strip.blend == AnimationStrip::Add, depth + 1);
            }
        };
    collect(this, t, 0, false, 0);
    for (auto& group : groups) {
        std::stable_sort(group.begin(), group.end(), [](const Item& a, const Item& b) {
            return a.layer != b.layer ? a.layer < b.layer : a.target.depth < b.target.depth;
        });
        auto& first = group.front().target;
        Rest& rest = restOf(first);
        // The rest value as it stands before this frame's writes, for an
        // Add strip to add to
        std::unique_ptr<Property> restValue;
        for (auto& item : group) {
            if (item.add) {
                restValue.reset(rest.rest->Copy());
                break;
            }
        }
        bool written = false;
        for (auto& item : group) {
            try {
                if (item.add && item.target.depth == 0 && !item.track->stepped()) {
                    // Onto what is below this frame, or onto the rest value
                    writeTrackAdd(*item.track, item.target, item.time,
                                  written ? *item.target.prop : *restValue);
                }
                else {
                    writeTrack(*item.track, item.target, item.time);
                }
                written = true;
            }
            catch (Base::Exception& e) {
                FC_ERR(getFullName() << ": track " << item.track->target.toString() << ": "
                                     << e.what());
            }
        }
        rest.written.reset(first.prop->Copy());
    }
    // An expression reading this Animation's Time drives what it is in
    for (auto obj : getInList()) {
        if (obj && obj != this && obj->isAttachedToDocument()) {
            obj->touch();
        }
    }
    if (doc->testStatus(Document::Recomputing)) {
        return;
    }
    switch (Recompute.getValue()) {
    case RecomputeTouched:
        doc->recompute();
        break;
    case RecomputeFull:
        for (auto obj : doc->getObjects()) {
            obj->touch();
        }
        doc->recompute();
        break;
    default:
        break;
    }
}

void Animation::activate()
{
    if (!Active.getValue()) {
        Active.setValue(true);  // evaluates
    }
    else {
        evaluate();
    }
}

void Animation::deactivate(bool recompute)
{
    auto doc = getDocument();
    _pending = false;
    // A handler's tracks keep their own rest: a camera put back
    for (auto& track : Tracks.getValues()) {
        if (track.kind != "Property") {
            if (auto handler = AnimationTrackHandler::get(track.kind)) {
                try {
                    handler->deactivate(*this, track);
                }
                catch (Base::Exception& e) {
                    e.ReportException();
                }
            }
        }
    }
    if (_rest.empty()) {
        return;
    }
    std::vector<DocumentObject*> purge;
    {
        WriteScope animated(WriteCause::Animation);
        auto rest = std::move(_rest);
        _rest.clear();
        auto& owners = restOwners();
        for (auto& [prop, r] : rest) {
            owners.erase(prop);
            // Put back only a pose of ours; a value somebody wrote since is
            // the design's
            if (!r->obj || !r->obj->isAttachedToDocument() || !r->rest || !r->written
                || !r->written->isSame(*prop)) {
                continue;
            }
            try {
                const_cast<Property*>(prop)->Paste(*r->rest);
            }
            catch (Base::Exception& e) {
                FC_ERR(getFullName() << ": cannot restore " << prop->getFullName() << ": "
                                     << e.what());
            }
            if (!r->touched) {
                purge.push_back(r->obj);
            }
        }
        if (!recompute || !doc || doc->testStatus(Document::Recomputing)) {
            return;
        }
        if (Recompute.getValue() == RecomputeNever) {
            // Never recomputed while playing, so nothing derived holds the
            // pose: what the writes touched is as it was
            for (auto obj : purge) {
                if (obj->isAttachedToDocument()) {
                    obj->purgeTouched();
                }
            }
        }
        else {
            doc->recompute();
        }
    }
}

double Animation::endTime() const
{
    double end = End.getValue();
    if (end > 0) {
        return end;
    }
    for (auto& t : Tracks.getValues()) {
        if (!t.keys.empty()) {
            end = std::max(end, t.keys.back().key.t);
        }
    }
    for (auto& span : stripSpans()) {
        end = std::max(end, span.end);
    }
    return end;
}

std::vector<double> Animation::frames() const
{
    std::vector<double> res;
    double rate = FrameRate.getValue();
    if (rate <= 0) {
        return res;
    }
    double start = Start.getValue();
    double end = endTime();
    for (long i = 0;; ++i) {
        double t = start + static_cast<double>(i) / rate;
        if (t > end + TimeEpsilon) {
            break;
        }
        res.push_back(t);
    }
    return res;
}

Py::Object Animation::value(int trackId, double t) const
{
    auto track = Tracks.getTrack(trackId);
    if (!track) {
        FC_THROWM(Base::IndexError, "no animation track " << trackId);
    }
    if (track->keys.empty()) {
        return Py::None();
    }
    if (track->kind != "Property") {
        Curve curve;
        track->makeCurve(curve);
        std::vector<double> nums;
        if (!curve.evaluate(t, nums)) {
            return Py::None();
        }
        Py::Tuple res(nums.size());
        for (std::size_t i = 0; i < nums.size(); ++i) {
            res.setItem(i, Py::Float(nums[i]));
        }
        return res;
    }
    Target target;
    bool resolved = resolve(*track, target);
    std::unique_ptr<Property> temp;
    if (track->depth() == 0 || (resolved && target.depth == 0)) {
        if (resolved) {
            temp.reset(target.prop->Copy());
        }
        else {
            temp = makeProperty(track->valueType, track->unit);
        }
    }
    else {
        temp = makeProperty(track->valueType, track->unit);
    }
    if (!temp) {
        return Py::None();
    }
    if (track->stepped()) {
        auto it = std::upper_bound(
            track->keys.begin(), track->keys.end(), t + TimeEpsilon,
            [](double v, const AnimationKey& k) { return v < k.key.t; });
        const auto& key = it == track->keys.begin() ? *it : *(it - 1);
        restoreText(*temp, key.text);
    }
    else {
        Curve curve;
        track->makeCurve(curve);
        std::vector<double> nums;
        if (!curve.evaluate(t, nums) || !temp->setAnimValue(track->layout, nums)) {
            return Py::None();
        }
    }
    return Py::asObject(temp->getPyObject());
}

void Animation::connectDocument()
{
    _conns.clear();
    auto doc = getDocument();
    if (!doc) {
        return;
    }
    _conns.emplace_back(doc->signalStartSave.connect([this](const Document&, const std::string&) {
        // A save writes the design: the rest values, and what they
        // recompute to (docs/Animation.md 3.8)
        if (Active.getValue() && !_rest.empty()) {
            _saving = true;
            deactivate(true);
        }
    }));
    _conns.emplace_back(doc->signalFinishSave.connect([this](const Document&, const std::string&) {
        if (_saving) {
            _saving = false;
            evaluate();
        }
    }));
    auto pending = [this](const Document&) {
        if (_pending) {
            evaluate();
        }
    };
    _conns.emplace_back(doc->signalUndo.connect(pending));
    _conns.emplace_back(doc->signalRedo.connect(pending));
    _conns.emplace_back(doc->signalDeletedObject.connect([this](const DocumentObject& obj) {
        if (&obj == this) {
            return;
        }
        // Kept by the undo as it is now: put the design back in it first
        WriteScope animated(WriteCause::Animation);
        for (auto& [prop, r] : _rest) {
            if (r->obj == &obj && r->rest && r->written && r->written->isSame(*prop)) {
                try {
                    const_cast<Property*>(prop)->Paste(*r->rest);
                }
                catch (Base::Exception& e) {
                    e.ReportException();
                }
            }
        }
        dropRest(&obj, nullptr);
    }));
    _conns.emplace_back(doc->signalBeforeChangeObject.connect(
        [this](const DocumentObject& obj, const Property& prop) { onBeforeRecord(obj, prop); }));
    _conns.emplace_back(doc->signalChangedObject.connect(
        [this](const DocumentObject& obj, const Property& prop) { onRecord(obj, prop); }));
    _conns.emplace_back(
        GetApplication().signalFinishRestoreDocument.connect([this](const Document& d) {
            if (&d == getDocument() && _pending) {
                evaluate();
            }
        }));
    _conns.emplace_back(GetApplication().signalRemoveDynamicProperty.connect(
        [this](const Property& prop) { dropRest(nullptr, &prop); }));
}

void Animation::onSettingDocument()
{
    DocumentObject::onSettingDocument();
    connectDocument();
}

void Animation::onChanged(const Property* prop)
{
    DocumentObject::onChanged(prop);
    auto doc = getDocument();
    if (!doc || isRestoring() || doc->testStatus(Document::Restoring)) {
        return;
    }
    if (prop == &Time) {
        // Setting the time makes this the playing Animation
        if (!Active.getValue()) {
            Active.setValue(true);
        }
        else {
            evaluate();
        }
    }
    else if (prop == &Active) {
        if (Active.getValue()) {
            for (auto obj : doc->getObjectsOfType(Animation::getClassTypeId())) {
                auto other = static_cast<Animation*>(obj);
                if (other != this && other->Active.getValue()) {
                    other->Active.setValue(false);  // puts its rest values back
                }
            }
            evaluate();
        }
        else {
            deactivate(true);
        }
    }
    else if (prop == &Tracks) {
        // A key Record makes is of the value on show already
        if (Active.getValue() && !_recordingKey) {
            evaluate();
        }
    }
    else if (prop == &Recording) {
        if (Recording.getValue()) {
            recorders().insert(this);
        }
        else {
            recorders().erase(this);
        }
        viewChanging = nullptr;
        if (Recording.getValue() && !Active.getValue()) {
            Active.setValue(true);
        }
    }
}

void Animation::onDocumentRestored()
{
    DocumentObject::onDocumentRestored();
    // Evaluated once the document is whole (connectDocument()): a
    // recompute does not run in the middle of a restore
    if (Active.getValue()) {
        _pending = true;
    }
}

void Animation::unsetupObject()
{
    recorders().erase(this);
    // Deleted: the design is put back as it was
    deactivate(true);
    _conns.clear();
    DocumentObject::unsetupObject();
}

PyObject* Animation::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject = Py::Object(new AnimationPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}
