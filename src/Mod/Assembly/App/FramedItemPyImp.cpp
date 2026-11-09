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
#include <Base/PlacementPy.h>
#include <Mod/Part/App/FeatureElement.h>

// inclusion of the generated files (generated out of FramedItem.pyi)
#include "FramedItemPy.h"
#include "FramedItemPy.cpp"

using namespace Assembly;

std::string FramedItemPy::representation() const
{
    // A bushing has no binding of its own
    return std::string("<") + getFramedItemPtr()->getTypeId().getName() + ">";
}

PyObject* FramedItemPy::getEnd(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        Base::Placement frame;
        auto component = getFramedItemPtr()->getEnd(frame);
        Py::Tuple res(2);
        res.setItem(0, component ? Py::asObject(component->getPyObject()) : Py::None());
        res.setItem(1, Py::asObject(new Base::PlacementPy(frame)));
        return Py::new_reference_to(res);
    }
    PY_CATCH;
}

PyObject* FramedItemPy::getEndPlacement(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return new Base::PlacementPy(getFramedItemPtr()->getEndPlacement());
    }
    PY_CATCH;
}

PyObject* FramedItemPy::setEnd(PyObject* args)
{
    PyObject* pyobj = nullptr;
    const char* subname = "";
    const char* vertex = "";
    if (!PyArg_ParseTuple(args,
                          "O!s|s",
                          &(App::DocumentObjectPy::Type),
                          &pyobj,
                          &subname,
                          &vertex)) {
        return nullptr;
    }
    auto component = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    PY_TRY
    {
        auto binding = getFramedItemPtr()->setEnd(component, subname, vertex);
        return binding->getPyObject();
    }
    PY_CATCH;
}

PyObject* FramedItemPy::clearEnd(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        getFramedItemPtr()->clearEnd();
    }
    PY_CATCH;
    Py_Return;
}

PyObject* FramedItemPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int FramedItemPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
