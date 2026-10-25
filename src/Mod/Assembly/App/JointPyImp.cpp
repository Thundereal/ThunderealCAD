// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

#include <Base/PlacementPy.h>
#include <Mod/Part/App/FeatureElement.h>

#include "AssemblyObject.h"

// inclusion of the generated files (generated out of Joint.pyi)
#include "JointPy.h"
#include "JointPy.cpp"

using namespace Assembly;

namespace
{
bool checkSide(int side)
{
    if (side == 1 || side == 2) {
        return true;
    }
    PyErr_SetString(PyExc_ValueError, "side must be 1 or 2");
    return false;
}
}  // namespace

std::string JointPy::representation() const
{
    return {"<Assembly::Joint>"};
}

PyObject* JointPy::getComponent(PyObject* args) const
{
    int side = 0;
    if (!PyArg_ParseTuple(args, "i", &side) || !checkSide(side)) {
        return nullptr;
    }
    auto obj = getJointPtr()->getComponent(side);
    if (!obj) {
        Py_Return;
    }
    return obj->getPyObject();
}

PyObject* JointPy::getFrame(PyObject* args) const
{
    int side = 0;
    if (!PyArg_ParseTuple(args, "i", &side) || !checkSide(side)) {
        return nullptr;
    }
    PY_TRY
    {
        return new Base::PlacementPy(getJointPtr()->getSide(side).frame);
    }
    PY_CATCH;
}

PyObject* JointPy::setSide(PyObject* args)
{
    int side = 0;
    PyObject* pyobj = nullptr;
    const char* subname = "";
    const char* vertex = "";
    PyObject* local = Py_False;
    if (!PyArg_ParseTuple(args, "iO!s|sO!", &side, &(App::DocumentObjectPy::Type), &pyobj, &subname, &vertex,
                          &PyBool_Type, &local)
        || !checkSide(side)) {
        return nullptr;
    }
    auto component = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    PY_TRY
    {
        auto link = getJointPtr()->setSide(side, component, subname, vertex,
                                           PyObject_IsTrue(local) != 0);
        return link->getPyObject();
    }
    PY_CATCH;
}

PyObject* JointPy::clearSide(PyObject* args)
{
    int side = 0;
    if (!PyArg_ParseTuple(args, "i", &side) || !checkSide(side)) {
        return nullptr;
    }
    PY_TRY
    {
        getJointPtr()->clearSide(side);
    }
    PY_CATCH;
    Py_Return;
}

PyObject* JointPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int JointPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
