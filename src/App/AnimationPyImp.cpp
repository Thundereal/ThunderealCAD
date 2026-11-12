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

#include <array>

#include <Base/PyWrapParseTupleAndKeywords.h>

#include "Animation.h"

// inclusion of the generated files (generated out of AnimationPy.xml)
#include "AnimationPy.h"
#include "AnimationPy.cpp"

using namespace App;

namespace
{

/// An easing from a type name, or (type, mode[, amplitude, period, overshoot])
Anim::Easing easingOf(PyObject* obj)
{
    Anim::Easing e;
    if (PyUnicode_Check(obj)) {
        Anim::fromName(PyUnicode_AsUTF8(obj), e.type);
        return e;
    }
    Py::Sequence seq(obj);
    if (seq.size() < 1 || seq.size() > 5) {
        FC_THROWM(Base::TypeError, "easing: a name, or (type, mode[, amplitude, period, overshoot])");
    }
    Anim::fromName(Py::String(seq[0]).as_std_string().c_str(), e.type);
    if (seq.size() > 1) {
        Anim::fromName(Py::String(seq[1]).as_std_string().c_str(), e.mode);
    }
    if (seq.size() > 2) {
        e.amplitude = Py::Float(seq[2]);
    }
    if (seq.size() > 3) {
        e.period = Py::Float(seq[3]);
    }
    if (seq.size() > 4) {
        e.overshoot = Py::Float(seq[4]);
    }
    return e;
}

Py::Object easingTuple(const Anim::Easing& e)
{
    return Py::TupleN(Py::String(Anim::nameOf(e.type)), Py::String(Anim::nameOf(e.mode)),
                      Py::Float(e.amplitude), Py::Float(e.period), Py::Float(e.overshoot));
}

int keyIndex(const AnimationTrack& track, double t)
{
    int i = track.find(t);
    if (i < 0) {
        FC_THROWM(Base::IndexError, "no key at " << t << " s on " << track.target.toString());
    }
    return i;
}

}  // namespace

std::string AnimationPy::representation() const
{
    return {"<Animation object>"};
}

PyObject* AnimationPy::addKey(PyObject* args, PyObject* kwds)
{
    const char* path;
    double t;
    PyObject* value = Py_None;
    const char* interp = nullptr;
    PyObject* easing = Py_None;
    static const std::array<const char*, 6> kwlist {"path", "time", "value", "interp", "easing",
                                                    nullptr};
    if (!Base::Wrapped_ParseTupleAndKeywords(args, kwds, "sd|OzO", kwlist, &path, &t, &value,
                                             &interp, &easing)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        Anim::Interp i {};
        Anim::Easing e;
        bool hasEasing = easing != Py_None;
        if (hasEasing) {
            e = easingOf(easing);
            i = Anim::Interp::Easing;
        }
        if (interp) {
            Anim::fromName(interp, i);
        }
        if (i == Anim::Interp::Python) {
            FC_THROWM(Base::NotImplementedError, "a Python interpolation");
        }
        int track = anim->trackOf(anim->parsePath(path), true);
        int id = anim->setKey(track, t, value, (interp || hasEasing) ? &i : nullptr,
                              hasEasing ? &e : nullptr);
        return Py::new_reference_to(Py::Long(id));
    }
    PY_CATCH
}

PyObject* AnimationPy::removeKey(PyObject* args)
{
    const char* path;
    double t;
    if (!PyArg_ParseTuple(args, "sd", &path, &t)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        auto track = anim->Tracks.getTrack(anim->trackOf(anim->parsePath(path), false));
        anim->Tracks.removeKey(track->id, track->keys[keyIndex(*track, t)].id);
        Py_Return;
    }
    PY_CATCH
}

PyObject* AnimationPy::moveKey(PyObject* args)
{
    const char* path;
    double t;
    double to;
    if (!PyArg_ParseTuple(args, "sdd", &path, &t, &to)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        auto track = anim->Tracks.getTrack(anim->trackOf(anim->parsePath(path), false));
        int i = keyIndex(*track, t);
        anim->Tracks.moveKeys({{track->id, track->keys[i].id}}, to - track->keys[i].key.t);
        Py_Return;
    }
    PY_CATCH
}

PyObject* AnimationPy::setInterp(PyObject* args, PyObject* kwds)
{
    const char* path;
    double t;
    const char* interp;
    PyObject* easing = Py_None;
    static const std::array<const char*, 5> kwlist {"path", "time", "interp", "easing", nullptr};
    if (!Base::Wrapped_ParseTupleAndKeywords(args, kwds, "sds|O", kwlist, &path, &t, &interp,
                                             &easing)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        auto track = anim->Tracks.getTrack(anim->trackOf(anim->parsePath(path), false));
        Anim::Interp i;
        Anim::fromName(interp, i);
        if (i == Anim::Interp::Python) {
            FC_THROWM(Base::NotImplementedError, "a Python interpolation");
        }
        Anim::Easing e;
        if (easing != Py_None) {
            e = easingOf(easing);
        }
        anim->Tracks.setInterp({{track->id, track->keys[keyIndex(*track, t)].id}}, i,
                               easing != Py_None ? &e : nullptr);
        Py_Return;
    }
    PY_CATCH
}

PyObject* AnimationPy::addTrack(PyObject* args)
{
    const char* path;
    if (!PyArg_ParseTuple(args, "s", &path)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        return Py::new_reference_to(Py::Long(anim->trackOf(anim->parsePath(path), true)));
    }
    PY_CATCH
}

PyObject* AnimationPy::removeTrack(PyObject* args)
{
    const char* path;
    if (!PyArg_ParseTuple(args, "s", &path)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        anim->Tracks.removeTrack(anim->trackOf(anim->parsePath(path), false));
        Py_Return;
    }
    PY_CATCH
}

PyObject* AnimationPy::track(PyObject* args)
{
    const char* path;
    if (!PyArg_ParseTuple(args, "s", &path)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        auto t = anim->Tracks.getTrack(anim->trackOf(anim->parsePath(path), false));
        Py::Dict d;
        d.setItem("Id", Py::Long(t->id));
        d.setItem("Kind", Py::String(t->kind));
        d.setItem("Path", Py::String(t->target.toString()));
        d.setItem("ValueType", Py::String(t->valueType));
        d.setItem("Unit", Py::String(t->unit));
        d.setItem("Label", Py::String(t->label));
        d.setItem("Mute", Py::Boolean(t->mute));
        d.setItem("Lock", Py::Boolean(t->lock));
        d.setItem("Stepped", Py::Boolean(t->stepped()));
        d.setItem("Extrapolation", Py::TupleN(Py::String(Anim::nameOf(t->before)),
                                              Py::String(Anim::nameOf(t->after))));
        d.setItem("Rotation", Py::String(Anim::nameOf(t->rotation)));
        d.setItem("State", Py::String(App::toString(anim->state(t->id))));
        Py::List keys;
        for (auto& k : t->keys) {
            Py::Dict kd;
            kd.setItem("Id", Py::Long(k.id));
            kd.setItem("Time", Py::Float(k.key.t));
            kd.setItem("Interp", Py::String(Anim::nameOf(k.key.interp)));
            if (k.key.interp == Anim::Interp::Easing) {
                kd.setItem("Easing", easingTuple(k.key.easing));
            }
            kd.setItem("Handles", Py::TupleN(Py::String(Anim::nameOf(k.key.left.type)),
                                             Py::String(Anim::nameOf(k.key.right.type))));
            if (t->stepped()) {
                kd.setItem("Text", Py::String(k.text));
            }
            else {
                Py::Tuple nums(k.key.v.size());
                for (std::size_t i = 0; i < k.key.v.size(); ++i) {
                    nums.setItem(i, Py::Float(k.key.v[i]));
                }
                kd.setItem("Numbers", nums);
            }
            keys.append(kd);
        }
        d.setItem("Keys", keys);
        return Py::new_reference_to(d);
    }
    PY_CATCH
}

PyObject* AnimationPy::setTrack(PyObject* args, PyObject* kwds)
{
    const char* path;
    const char* label = nullptr;
    PyObject* mute = Py_None;
    PyObject* lock = Py_None;
    PyObject* extrap = Py_None;
    const char* rotation = nullptr;
    static const std::array<const char*, 7> kwlist {"path", "label", "mute", "lock",
                                                    "extrapolation", "rotation", nullptr};
    if (!Base::Wrapped_ParseTupleAndKeywords(args, kwds, "s|zOOOz", kwlist, &path, &label, &mute,
                                             &lock, &extrap, &rotation)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        int id = anim->trackOf(anim->parsePath(path), false);
        if (rotation) {
            Anim::RotationMode mode;
            Anim::fromName(rotation, mode);
            anim->setRotationMode(id, mode);
        }
        AnimationTrack o = *anim->Tracks.getTrack(id);
        if (label) {
            o.label = label;
        }
        if (mute != Py_None) {
            o.mute = PyObject_IsTrue(mute) != 0;
        }
        if (lock != Py_None) {
            o.lock = PyObject_IsTrue(lock) != 0;
        }
        if (extrap != Py_None) {
            if (PyUnicode_Check(extrap)) {
                Anim::fromName(PyUnicode_AsUTF8(extrap), o.before);
                o.after = o.before;
            }
            else {
                Py::Sequence seq(extrap);
                if (seq.size() != 2) {
                    FC_THROWM(Base::TypeError, "extrapolation: a name, or (before, after)");
                }
                Anim::fromName(Py::String(seq[0]).as_std_string().c_str(), o.before);
                Anim::fromName(Py::String(seq[1]).as_std_string().c_str(), o.after);
            }
        }
        if (!(o == *anim->Tracks.getTrack(id))) {
            anim->Tracks.setTrackOptions(id, o);
        }
        Py_Return;
    }
    PY_CATCH
}

PyObject* AnimationPy::keys(PyObject* args)
{
    const char* path;
    if (!PyArg_ParseTuple(args, "s", &path)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        auto t = anim->Tracks.getTrack(anim->trackOf(anim->parsePath(path), false));
        Py::List list;
        for (auto& k : t->keys) {
            list.append(Py::TupleN(Py::Float(k.key.t), anim->value(t->id, k.key.t),
                                   Py::String(Anim::nameOf(k.key.interp))));
        }
        return Py::new_reference_to(list);
    }
    PY_CATCH
}

PyObject* AnimationPy::value(PyObject* args)
{
    const char* path;
    double t;
    if (!PyArg_ParseTuple(args, "sd", &path, &t)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        int id = anim->trackOf(anim->parsePath(path), false);
        return Py::new_reference_to(anim->value(id, t));
    }
    PY_CATCH
}

PyObject* AnimationPy::state(PyObject* args)
{
    const char* path;
    if (!PyArg_ParseTuple(args, "s", &path)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        int id = anim->trackOf(anim->parsePath(path), false);
        return Py::new_reference_to(Py::String(App::toString(anim->state(id))));
    }
    PY_CATCH
}

PyObject* AnimationPy::keyNow(PyObject* args)
{
    const char* path;
    if (!PyArg_ParseTuple(args, "s", &path)) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        return Py::new_reference_to(Py::Long(anim->keyNow(anim->parsePath(path))));
    }
    PY_CATCH
}

PyObject* AnimationPy::keyState(PyObject* args)
{
    const char* path;
    if (!PyArg_ParseTuple(args, "s", &path)) {
        return nullptr;
    }
    PY_TRY
    {
        static const char* names[] = {"None", "Animated", "Keyed", "Changed"};
        auto anim = getAnimationPtr();
        auto state = anim->keyState(anim->parsePath(path));
        return Py::new_reference_to(Py::String(names[static_cast<int>(state)]));
    }
    PY_CATCH
}

PyObject* AnimationPy::nextKeyTime(PyObject* args)
{
    PyObject* forward = Py_True;
    if (!PyArg_ParseTuple(args, "|O", &forward)) {
        return nullptr;
    }
    PY_TRY
    {
        return Py::new_reference_to(
            Py::Float(getAnimationPtr()->nextKeyTime(PyObject_IsTrue(forward) != 0)));
    }
    PY_CATCH
}

PyObject* AnimationPy::activate(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        getAnimationPtr()->activate();
        Py_Return;
    }
    PY_CATCH
}

PyObject* AnimationPy::stop(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        auto anim = getAnimationPtr();
        if (anim->Active.getValue()) {
            anim->Active.setValue(false);  // puts the rest values back
        }
        else {
            anim->deactivate(true);
        }
        Py_Return;
    }
    PY_CATCH
}

PyObject* AnimationPy::evaluate(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        getAnimationPtr()->evaluate();
        Py_Return;
    }
    PY_CATCH
}

PyObject* AnimationPy::frames(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        Py::List list;
        for (double t : getAnimationPtr()->frames()) {
            list.append(Py::Float(t));
        }
        return Py::new_reference_to(list);
    }
    PY_CATCH
}

PyObject* AnimationPy::getCustomAttributes(const char*) const
{
    return nullptr;
}

int AnimationPy::setCustomAttributes(const char*, PyObject*)
{
    return 0;
}
