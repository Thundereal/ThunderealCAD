// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#include <cstring>

#include <Base/PlacementPy.h>
#include <Base/RotationPy.h>
#include <Base/VectorPy.h>

#include "ViewProviderSketch3D.h"

// inclusion of the generated files (generated out of ViewProviderSketch3DPy.xml)
#include "ViewProviderSketch3DPy.h"
#include "ViewProviderSketch3DPy.cpp"

using namespace SketcherGui;

std::string ViewProviderSketch3DPy::representation() const
{
    return "<SketcherGui::ViewProviderSketch3D>";
}

namespace
{
const char* planeName(ViewProviderSketch3D::Plane p)
{
    switch (p) {
        case ViewProviderSketch3D::Plane::YZ:
            return "YZ";
        case ViewProviderSketch3D::Plane::ZX:
            return "ZX";
        default:
            return "XY";
    }
}
}  // namespace

PyObject* ViewProviderSketch3DPy::getManipulator(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    auto vp = getViewProviderSketch3DPtr();
    if (!vp->isEditingSketch()) {
        Py_Return;
    }
    Py::Dict d;
    d.setItem("Origin", Py::asObject(new Base::VectorPy(vp->manipulatorOrigin())));
    d.setItem("Rotation", Py::asObject(new Base::RotationPy(vp->manipulatorRotation())));
    d.setItem("Plane", Py::String(planeName(vp->activePlane())));
    d.setItem("HeightLock", Py::Boolean(vp->heightLocked()));
    d.setItem("HeightBase", Py::asObject(new Base::VectorPy(vp->heightBase())));
    d.setItem("RotationHandles", Py::Boolean(vp->rotationHandlesShown()));
    d.setItem("WorkingPlane", Py::asObject(new Base::PlacementPy(vp->workingPlane())));
    return Py::new_reference_to(d);
}

PyObject* ViewProviderSketch3DPy::setManipulator(PyObject* args)
{
    PyObject *pyOrigin, *pyRot;
    if (!PyArg_ParseTuple(args,
                          "O!O!",
                          &Base::VectorPy::Type,
                          &pyOrigin,
                          &Base::RotationPy::Type,
                          &pyRot)) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->setManipulator(
        *static_cast<Base::VectorPy*>(pyOrigin)->getVectorPtr(),
        *static_cast<Base::RotationPy*>(pyRot)->getRotationPtr());
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::cycleWorkingPlane(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->cycleWorkingPlane();
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::toggleHeightLock(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->toggleHeightLock();
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::resetManipulator(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->resetManipulator();
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::setRotationHandles(PyObject* args)
{
    PyObject* shown;
    if (!PyArg_ParseTuple(args, "O!", &PyBool_Type, &shown)) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->setRotationHandlesShown(PyObject_IsTrue(shown));
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::pointAt(PyObject* args) const
{
    int x, y;
    if (!PyArg_ParseTuple(args, "ii", &x, &y)) {
        return nullptr;
    }
    Base::Vector3d p;
    if (!getViewProviderSketch3DPtr()->pointAt(SbVec2s(short(x), short(y)), p)) {
        Py_Return;
    }
    return new Base::VectorPy(p);
}

PyObject* ViewProviderSketch3DPy::toView(PyObject* args) const
{
    PyObject* pyPoint;
    if (!PyArg_ParseTuple(args, "O!", &Base::VectorPy::Type, &pyPoint)) {
        return nullptr;
    }
    SbVec2s pos;
    if (!getViewProviderSketch3DPtr()->toView(
            *static_cast<Base::VectorPy*>(pyPoint)->getVectorPtr(),
            pos)) {
        Py_Return;
    }
    return Py::new_reference_to(Py::TupleN(Py::Long(pos[0]), Py::Long(pos[1])));
}

PyObject* ViewProviderSketch3DPy::press(PyObject* args)
{
    int x, y;
    if (!PyArg_ParseTuple(args, "ii", &x, &y)) {
        return nullptr;
    }
    return Py::new_reference_to(
        Py::Boolean(getViewProviderSketch3DPtr()->press(SbVec2s(short(x), short(y)))));
}

PyObject* ViewProviderSketch3DPy::move(PyObject* args)
{
    int x, y;
    if (!PyArg_ParseTuple(args, "ii", &x, &y)) {
        return nullptr;
    }
    return Py::new_reference_to(
        Py::Boolean(getViewProviderSketch3DPtr()->move(SbVec2s(short(x), short(y)))));
}

PyObject* ViewProviderSketch3DPy::pressRight(PyObject* args)
{
    int x, y;
    if (!PyArg_ParseTuple(args, "ii", &x, &y)) {
        return nullptr;
    }
    return Py::new_reference_to(
        Py::Boolean(getViewProviderSketch3DPtr()->pressRight(SbVec2s(short(x), short(y)))));
}

namespace
{
using Tool = ViewProviderSketch3D::Tool;
const std::pair<const char*, Tool> toolNames[] = {
    {"None", Tool::None},
    {"Point", Tool::Point},
    {"Line", Tool::Line},
    {"Polyline", Tool::Polyline},
    {"BSplineThrough", Tool::BSplineThrough},
    {"BSplineControl", Tool::BSplineControl},
};
}  // namespace

PyObject* ViewProviderSketch3DPy::startTool(PyObject* args)
{
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) {
        return nullptr;
    }
    for (const auto& [n, tool] : toolNames) {
        if (strcmp(n, name) == 0) {
            getViewProviderSketch3DPtr()->startTool(tool);
            Py_Return;
        }
    }
    PyErr_SetString(PyExc_ValueError, "unknown tool");
    return nullptr;
}

PyObject* ViewProviderSketch3DPy::activeTool(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    Tool tool = getViewProviderSketch3DPtr()->activeTool();
    for (const auto& [n, t] : toolNames) {
        if (t == tool) {
            return Py::new_reference_to(Py::String(n));
        }
    }
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::finishTool(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->finishTool();
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::cancelTool(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->cancelTool();
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::placedCount(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    return Py::new_reference_to(Py::Long(getViewProviderSketch3DPtr()->placedCount()));
}

PyObject* ViewProviderSketch3DPy::setConstructionMode(PyObject* args)
{
    PyObject* on;
    if (!PyArg_ParseTuple(args, "O!", &PyBool_Type, &on)) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->setConstructionMode(PyObject_IsTrue(on));
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::constructionMode(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    return Py::new_reference_to(Py::Boolean(getViewProviderSketch3DPtr()->constructionMode()));
}

PyObject* ViewProviderSketch3DPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int ViewProviderSketch3DPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
