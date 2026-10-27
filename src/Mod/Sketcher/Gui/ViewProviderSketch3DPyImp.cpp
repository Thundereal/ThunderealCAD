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

PyObject* ViewProviderSketch3DPy::dragTo(PyObject* args)
{
    int x, y;
    if (!PyArg_ParseTuple(args, "ii", &x, &y)) {
        return nullptr;
    }
    return Py::new_reference_to(
        Py::Boolean(getViewProviderSketch3DPtr()->dragTo(SbVec2s(short(x), short(y)))));
}

PyObject* ViewProviderSketch3DPy::release(PyObject* args)
{
    int x, y;
    if (!PyArg_ParseTuple(args, "ii", &x, &y)) {
        return nullptr;
    }
    return Py::new_reference_to(
        Py::Boolean(getViewProviderSketch3DPtr()->release(SbVec2s(short(x), short(y)))));
}

PyObject* ViewProviderSketch3DPy::keepPlane(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    return Py::new_reference_to(Py::Long(getViewProviderSketch3DPtr()->keepPlane()));
}

PyObject* ViewProviderSketch3DPy::activateKeptPlane(PyObject* args)
{
    int geoId;
    if (!PyArg_ParseTuple(args, "i", &geoId)) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->activateKeptPlane(geoId);
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::activeKeptPlane(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    return Py::new_reference_to(Py::Long(getViewProviderSketch3DPtr()->activeKeptPlane()));
}

PyObject* ViewProviderSketch3DPy::press(PyObject* args)
{
    int x, y;
    PyObject* ctrl = Py_False;
    if (!PyArg_ParseTuple(args, "ii|O!", &x, &y, &PyBool_Type, &ctrl)) {
        return nullptr;
    }
    return Py::new_reference_to(Py::Boolean(
        getViewProviderSketch3DPtr()->press(SbVec2s(short(x), short(y)), PyObject_IsTrue(ctrl))));
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
    {"Circle", Tool::Circle},
    {"Circle3Point", Tool::Circle3Point},
    {"Arc", Tool::Arc},
    {"Arc3Point", Tool::Arc3Point},
    {"Fillet", Tool::Fillet},
    {"SplineOnFace", Tool::SplineOnFace},
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

PyObject* ViewProviderSketch3DPy::select(PyObject* args)
{
    int geoId, pos = 0;
    PyObject* toggle = Py_True;
    if (!PyArg_ParseTuple(args, "i|iO!", &geoId, &pos, &PyBool_Type, &toggle)) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->select({geoId, static_cast<Sketcher::PointPos>(pos)},
                                         PyObject_IsTrue(toggle));
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::selectConstraint(PyObject* args)
{
    int index;
    PyObject* toggle = Py_True;
    if (!PyArg_ParseTuple(args, "i|O!", &index, &PyBool_Type, &toggle)) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->selectConstraint(index, PyObject_IsTrue(toggle));
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::clearSelection(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->clearSelection();
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::getSelection(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    auto vp = getViewProviderSketch3DPtr();
    Py::List elements, constraints;
    for (const auto& e : vp->selectedElements()) {
        elements.append(Py::TupleN(Py::Long(e.geoId), Py::Long(int(e.pos))));
    }
    for (int i : vp->selectedConstraints()) {
        constraints.append(Py::Long(i));
    }
    return Py::new_reference_to(Py::TupleN(elements, constraints));
}

PyObject* ViewProviderSketch3DPy::constrain(PyObject* args)
{
    const char* name;
    if (!PyArg_ParseTuple(args, "s", &name)) {
        return nullptr;
    }
    Sketcher::ConstraintType type = Sketcher::None;
    for (int t = 1; t < Sketcher::NumConstraintTypes; ++t) {
        if (Sketcher::Constraint::typeToString(static_cast<Sketcher::ConstraintType>(t)) == name) {
            type = static_cast<Sketcher::ConstraintType>(t);
        }
    }
    QString why;
    if (type == Sketcher::None || !getViewProviderSketch3DPtr()->constrainSelection(type, &why)) {
        PyErr_SetString(PyExc_ValueError,
                        type == Sketcher::None ? "unknown constraint type"
                                               : why.toUtf8().constData());
        return nullptr;
    }
    return Py::new_reference_to(Py::True());
}

PyObject* ViewProviderSketch3DPy::deleteSelection(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->deleteSelection();
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::editDatum(PyObject* args)
{
    int index;
    if (!PyArg_ParseTuple(args, "i", &index)) {
        return nullptr;
    }
    return Py::new_reference_to(Py::Boolean(getViewProviderSketch3DPtr()->editDatum(index)));
}

PyObject* ViewProviderSketch3DPy::isEditingDatum(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    return Py::new_reference_to(Py::Boolean(getViewProviderSketch3DPtr()->isEditingDatum()));
}

PyObject* ViewProviderSketch3DPy::finishDatumEdit(PyObject* args)
{
    PyObject* accept = Py_True;
    if (!PyArg_ParseTuple(args, "|O!", &PyBool_Type, &accept)) {
        return nullptr;
    }
    getViewProviderSketch3DPtr()->finishDatumEdit(PyObject_IsTrue(accept));
    Py_Return;
}

PyObject* ViewProviderSketch3DPy::getLabels(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    Py::Dict d;
    for (const auto& [index, text] : getViewProviderSketch3DPtr()->labelTexts()) {
        d.setItem(Py::Long(index), Py::String(text));
    }
    return Py::new_reference_to(d);
}

PyObject* ViewProviderSketch3DPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int ViewProviderSketch3DPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
