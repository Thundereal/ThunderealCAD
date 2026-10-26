// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2024 Ondsel <development@ondsel.com>                     *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/


// inclusion of the generated files (generated out of AssemblyObject.xml)
#include "AssemblyObjectPy.h"
#include "AssemblyObjectPy.cpp"

#include "Joint.h"

#include <Base/GeometryPyCXX.h>

using namespace Assembly;

namespace
{
// A pick: (component, subname) or (component, subname, vertex)
bool parsePick(PyObject* pick, App::DocumentObject*& obj, const char*& sub, const char*& vertex)
{
    PyObject* pyobj = nullptr;
    sub = "";
    vertex = nullptr;
    if (!PyArg_ParseTuple(pick, "O!|ss", &(App::DocumentObjectPy::Type), &pyobj, &sub, &vertex)) {
        return false;
    }
    obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    return true;
}
}  // namespace

PyObject* AssemblyObjectPy::makeJoint(PyObject* args)
{
    const char* type = nullptr;
    PyObject* pick1 = nullptr;
    PyObject* pick2 = nullptr;
    if (!PyArg_ParseTuple(args, "sO!O!", &type, &PyTuple_Type, &pick1, &PyTuple_Type, &pick2)) {
        return nullptr;
    }
    App::DocumentObject *obj1 = nullptr, *obj2 = nullptr;
    const char *sub1 = nullptr, *sub2 = nullptr, *vertex1 = nullptr, *vertex2 = nullptr;
    if (!parsePick(pick1, obj1, sub1, vertex1) || !parsePick(pick2, obj2, sub2, vertex2)) {
        return nullptr;
    }
    PY_TRY
    {
        auto joint = Joint::make(getAssemblyObjectPtr(), type, obj1, sub1, vertex1, obj2, sub2, vertex2);
        return joint->getPyObject();
    }
    PY_CATCH;
}

// returns a string which represents the object e.g. when printed in python
std::string AssemblyObjectPy::representation() const
{
    return {"<Assembly object>"};
}

PyObject* AssemblyObjectPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int AssemblyObjectPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}

PyObject* AssemblyObjectPy::solve(PyObject* args) const
{
    PyObject* enableUndoPy;
    bool enableUndo;

    if (!PyArg_ParseTuple(args, "O!", &PyBool_Type, &enableUndoPy)) {
        PyErr_Clear();
        if (!PyArg_ParseTuple(args, "")) {
            return nullptr;
        }
        else {
            enableUndo = false;
        }
    }
    else {
        enableUndo = Base::asBoolean(enableUndoPy);
    }

    int ret = this->getAssemblyObjectPtr()->solve(enableUndo);
    return Py_BuildValue("i", ret);
}

PyObject* AssemblyObjectPy::generateSimulation(PyObject* args) const
{
    PyObject* pyobj;

    if (!PyArg_ParseTuple(args, "O", &pyobj)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    int ret = this->getAssemblyObjectPtr()->generateSimulation(obj);
    return Py_BuildValue("i", ret);
}

PyObject* AssemblyObjectPy::ensureIdentityPlacements(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->ensureIdentityPlacements();
    Py_Return;
}

PyObject* AssemblyObjectPy::refreshFrozenShape(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        this->getAssemblyObjectPtr()->refreshFrozenShape();
        Py_Return;
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::updateForFrame(PyObject* args) const
{
    unsigned long index {};

    if (!PyArg_ParseTuple(args, "k", &index)) {
        throw Py::RuntimeError("updateForFrame requires an integer index");
    }
    PY_TRY
    {
        this->getAssemblyObjectPtr()->updateForFrame(index);
    }
    PY_CATCH;

    Py_Return;
}

PyObject* AssemblyObjectPy::numberOfFrames(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    size_t ret = this->getAssemblyObjectPtr()->numberOfFrames();
    return Py_BuildValue("k", ret);
}

PyObject* AssemblyObjectPy::updateSolveStatus(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    this->getAssemblyObjectPtr()->updateSolveStatus();
    Py_Return;
}

PyObject* AssemblyObjectPy::undoSolve(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->undoSolve();
    Py_Return;
}

PyObject* AssemblyObjectPy::clearUndo(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->clearUndo();
    Py_Return;
}

PyObject* AssemblyObjectPy::getGroup(PyObject* args)
{
    const char* role = nullptr;
    PyObject* create = Py_False;
    if (!PyArg_ParseTuple(args, "s|O!", &role, &PyBool_Type, &create)) {
        return nullptr;
    }
    PY_TRY
    {
        auto group = getAssemblyObjectPtr()->getAssemblyGroup(role, PyObject_IsTrue(create) != 0);
        if (!group) {
            Py_Return;
        }
        return group->getPyObject();
    }
    PY_CATCH;
}

PyObject* AssemblyObjectPy::isPartConnected(PyObject* args) const
{
    PyObject* pyobj;

    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyobj)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isPartConnected(obj);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::isPartGrounded(PyObject* args) const
{
    PyObject* pyobj;

    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyobj)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isPartGrounded(obj);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::isJointConnectingPartToGround(PyObject* args) const
{
    PyObject* pyobj;
    char* pname;

    if (!PyArg_ParseTuple(args, "O!s", &(App::DocumentObjectPy::Type), &pyobj, &pname)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isJointConnectingPartToGround(obj, pname);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::exportAsASMT(PyObject* args) const
{
    char* utf8Name;
    if (!PyArg_ParseTuple(args, "et", "utf-8", &utf8Name)) {
        return nullptr;
    }

    std::string fileName = utf8Name;
    PyMem_Free(utf8Name);

    if (fileName.empty()) {
        PyErr_SetString(PyExc_ValueError, "Passed string is empty");
        return nullptr;
    }

    this->getAssemblyObjectPtr()->exportAsASMT(fileName);

    Py_Return;
}

PyObject* AssemblyObjectPy::preDrag(PyObject* args) const
{
    PyObject* pyParts;
    double mass = 10.0;
    if (!PyArg_ParseTuple(args, "O|d", &pyParts, &mass)) {
        return nullptr;
    }

    PY_TRY
    {
        std::vector<App::DocumentObject*> parts;
        std::vector<std::pair<App::DocumentObject*, double>> masses;
        Py::Sequence seq(pyParts);
        for (Py::Sequence::iterator it = seq.begin(); it != seq.end(); ++it) {
            PyObject* item = (*it).ptr();
            if (!PyObject_TypeCheck(item, &App::DocumentObjectPy::Type)) {
                throw Py::TypeError("preDrag: expected a sequence of document objects");
            }
            auto* obj = static_cast<App::DocumentObjectPy*>(item)->getDocumentObjectPtr();
            parts.push_back(obj);
            masses.emplace_back(obj, mass);
        }

        auto* assembly = getAssemblyObjectPtr();
        assembly->setObjMasses(masses);
        assembly->preDrag(parts);
    }
    PY_CATCH;

    Py_Return;
}

PyObject* AssemblyObjectPy::doDragStep(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    bool ok = false;
    PY_TRY
    {
        ok = getAssemblyObjectPtr()->doDragStep();
    }
    PY_CATCH;

    return Py::new_reference_to(Py::Boolean(ok));
}

PyObject* AssemblyObjectPy::postDrag(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    PY_TRY
    {
        auto* assembly = getAssemblyObjectPtr();
        assembly->postDrag();
        assembly->setObjMasses({});
    }
    PY_CATCH;

    Py_Return;
}

PyObject* AssemblyObjectPy::getDragStatus(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    auto* assembly = getAssemblyObjectPtr();
    Py::Dict ret;
    ret.setItem("Steps", Py::Long(assembly->getDragStepCount()));
    ret.setItem("Failures", Py::Long(assembly->getDragStepFailures()));
    ret.setItem("LastStepFailed", Py::Boolean(assembly->getLastDragStepFailed()));
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::getSolveStatus(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    auto* assembly = getAssemblyObjectPtr();
    auto toList = [](const std::vector<std::string>& names) {
        Py::List list;
        for (const auto& name : names) {
            list.append(Py::String(name));
        }
        return list;
    };

    Py::Dict ret;
    ret.setItem("DoF", Py::Long(assembly->getLastDoF()));
    ret.setItem("SolverStatus", Py::Long(assembly->getLastSolverStatus()));
    ret.setItem("Conflicting", toList(assembly->getLastConflicting()));
    ret.setItem("Redundant", toList(assembly->getLastRedundant()));
    ret.setItem("PartiallyRedundant", toList(assembly->getLastPartiallyRedundant()));
    ret.setItem("Malformed", toList(assembly->getLastMalformed()));
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::getSolverResiduals(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    Py::Dict ret;
    for (const auto& [name, error] : getAssemblyObjectPtr()->getSolverResiduals()) {
        ret.setItem(name, Py::Float(error));
    }
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::getSolverMetric(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    Py::Dict ret;
    for (const auto& [name, centre, inertia, body] : getAssemblyObjectPtr()->getSolverMetric()) {
        ret.setItem(name, Py::TupleN(Py::Vector(centre), Py::Float(inertia), Py::Long(body)));
    }
    return Py::new_reference_to(ret);
}

Py::List AssemblyObjectPy::getJoints() const
{
    Py::List ret;
    std::vector<App::DocumentObject*> list = getAssemblyObjectPtr()->getJoints();

    for (auto It : list) {
        ret.append(Py::Object(It->getPyObject(), true));
    }

    return ret;
}

PyObject* AssemblyObjectPy::getDownstreamParts(PyObject* args) const
{
    PyObject* pyPart;
    PyObject* pyJoint;

    // Parse the two arguments: a part object and a joint object
    if (!PyArg_ParseTuple(
            args,
            "O!O!",
            &(App::DocumentObjectPy::Type),
            &pyPart,
            &(App::DocumentObjectPy::Type),
            &pyJoint
        )) {
        return nullptr;
    }

    auto* part = static_cast<App::DocumentObjectPy*>(pyPart)->getDocumentObjectPtr();
    auto* joint = static_cast<App::DocumentObjectPy*>(pyJoint)->getDocumentObjectPtr();

    // Call the C++ method
    std::vector<Assembly::ObjRef> downstreamParts
        = this->getAssemblyObjectPtr()->getDownstreamParts(part, joint);

    // Convert the result into a Python list of DocumentObjects
    Py::List ret;
    for (const auto& objRef : downstreamParts) {
        if (objRef.obj) {
            ret.append(Py::Object(objRef.obj->getPyObject(), true));
        }
    }

    return Py::new_reference_to(ret);
}
