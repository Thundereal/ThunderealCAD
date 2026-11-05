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

#include <Mod/Part/App/TopoShapeEdgePy.h>

#include "SheetMetalExtensionPy.h"
#include "SheetMetalExtensionPy.cpp"
#include "SheetMetalFlat.h"

using namespace PartDesign;

std::string SheetMetalExtensionPy::representation() const
{
    return {"<PartDesign::SheetMetalExtension>"};
}

PyObject* SheetMetalExtensionPy::sheetElementOf(PyObject* args) const
{
    const char* subname;
    if (!PyArg_ParseTuple(args, "s", &subname)) {
        return nullptr;
    }
    PY_TRY
    {
        const std::string name = getSheetMetalExtensionPtr()->elementOf(subname);
        if (name.empty()) {
            Py_Return;
        }
        return Py::new_reference_to(Py::String(name));
    }
    PY_CATCH
}

PyObject* SheetMetalExtensionPy::bendKFactor(PyObject* args) const
{
    const char* bend;
    if (!PyArg_ParseTuple(args, "s", &bend)) {
        return nullptr;
    }
    PY_TRY
    {
        const auto k = getSheetMetalExtensionPtr()->bendKFactor(bend);
        if (!k) {
            Py_Return;
        }
        return Py::new_reference_to(Py::Float(*k));
    }
    PY_CATCH
}

PyObject* SheetMetalExtensionPy::setBendKFactor(PyObject* args)
{
    const char* bend;
    PyObject* value = Py_None;
    if (!PyArg_ParseTuple(args, "s|O", &bend, &value)) {
        return nullptr;
    }
    PY_TRY
    {
        std::optional<double> k;
        if (value != Py_None) {
            k = PyFloat_AsDouble(value);
            if (PyErr_Occurred()) {
                return nullptr;
            }
        }
        getSheetMetalExtensionPtr()->setBendKFactor(bend, k);
        Py_Return;
    }
    PY_CATCH
}

PyObject* SheetMetalExtensionPy::crossBreakLines(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        Py::List res;
        const auto edges =
            PartDesign::crossBreakLines(getSheetMetalExtensionPtr()->getExtendedObject());
        for (const TopoDS_Edge& e : edges) {
            res.append(Py::asObject(new Part::TopoShapeEdgePy(new Part::TopoShape(e))));
        }
        return Py::new_reference_to(res);
    }
    PY_CATCH
}

PyObject* SheetMetalExtensionPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int SheetMetalExtensionPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
