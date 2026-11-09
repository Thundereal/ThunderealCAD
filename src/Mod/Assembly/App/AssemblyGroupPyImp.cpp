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

#include "AssemblyGroup.h"

// inclusion of the generated files (generated out of AssemblyGroup.pyi)
#include "AssemblyGroupPy.h"
#include "AssemblyGroupPy.cpp"

using namespace Assembly;

std::string AssemblyGroupPy::representation() const
{
    return {"<Assembly Group>"};
}

PyObject* AssemblyGroupPy::newObject(PyObject* args)
{
    const char* type = nullptr;
    const char* name = "";
    if (!PyArg_ParseTuple(args, "s|s", &type, &name)) {
        return nullptr;
    }
    PY_TRY
    {
        auto obj = getAssemblyGroupPtr()->newObject(type, name);
        if (!obj) {
            PyErr_Format(PyExc_TypeError, "Cannot make an object of type '%s'", type);
            return nullptr;
        }
        return obj->getPyObject();
    }
    PY_CATCH;
}

PyObject* AssemblyGroupPy::addObject(PyObject* args)
{
    PyObject* pyobj = nullptr;
    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyobj)) {
        return nullptr;
    }
    PY_TRY
    {
        getAssemblyGroupPtr()->addObject(
            static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr()
        );
    }
    PY_CATCH;
    Py_Return;
}

PyObject* AssemblyGroupPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int AssemblyGroupPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
