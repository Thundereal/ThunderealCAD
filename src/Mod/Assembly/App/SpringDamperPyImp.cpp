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

#include <App/DocumentObjectPy.h>
#include <Base/VectorPy.h>
#include <Mod/Part/App/FeatureElement.h>

// inclusion of the generated files (generated out of SpringDamper.pyi)
#include "SpringDamperPy.h"
#include "SpringDamperPy.cpp"

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

std::string SpringDamperPy::representation() const
{
    return {"<Assembly::SpringDamper>"};
}

PyObject* SpringDamperPy::getEnd(PyObject* args) const
{
    int side = 0;
    if (!PyArg_ParseTuple(args, "i", &side) || !checkSide(side)) {
        return nullptr;
    }
    PY_TRY
    {
        Base::Vector3d point;
        auto component = getSpringDamperPtr()->getEnd(side, point);
        Py::Tuple res(2);
        res.setItem(0, component ? Py::asObject(component->getPyObject()) : Py::None());
        res.setItem(1, Py::asObject(new Base::VectorPy(point)));
        return Py::new_reference_to(res);
    }
    PY_CATCH;
}

PyObject* SpringDamperPy::getEndPoint(PyObject* args) const
{
    int side = 0;
    if (!PyArg_ParseTuple(args, "i", &side) || !checkSide(side)) {
        return nullptr;
    }
    PY_TRY
    {
        return new Base::VectorPy(getSpringDamperPtr()->getEndPoint(side));
    }
    PY_CATCH;
}

PyObject* SpringDamperPy::setEnd(PyObject* args)
{
    int side = 0;
    PyObject* pyobj = nullptr;
    const char* subname = "";
    const char* vertex = "";
    if (!PyArg_ParseTuple(args,
                          "iO!s|s",
                          &side,
                          &(App::DocumentObjectPy::Type),
                          &pyobj,
                          &subname,
                          &vertex)
        || !checkSide(side)) {
        return nullptr;
    }
    auto component = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    PY_TRY
    {
        auto binding = getSpringDamperPtr()->setEnd(side, component, subname, vertex);
        return binding->getPyObject();
    }
    PY_CATCH;
}

PyObject* SpringDamperPy::clearEnd(PyObject* args)
{
    int side = 0;
    if (!PyArg_ParseTuple(args, "i", &side) || !checkSide(side)) {
        return nullptr;
    }
    PY_TRY
    {
        getSpringDamperPtr()->clearEnd(side);
    }
    PY_CATCH;
    Py_Return;
}

PyObject* SpringDamperPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int SpringDamperPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
