// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2024 Boyer Pierre-Louis <development@ondsel>            *
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


#include <App/DocumentObjectPy.h>

// inclusion of the generated files (generated out of BomObject.xml)
#include "BomObjectPy.h"
#include "BomObjectPy.cpp"

using namespace Assembly;

// returns a string which represents the object e.g. when printed in python
std::string BomObjectPy::representation() const
{
    return {"<Bill of Materials object>"};
}

PyObject* BomObjectPy::getRows(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        auto bom = getBomObjectPtr();
        Py::List rows;
        for (std::size_t i = 0; i < bom->obj_list.size() && i < bom->obj_index_list.size(); ++i) {
            auto obj = bom->obj_list[i];
            rows.append(Py::TupleN(Py::String(bom->obj_index_list[i]),
                                   obj ? Py::asObject(obj->getPyObject()) : Py::None()));
        }
        return Py::new_reference_to(rows);
    }
    PY_CATCH;
}

PyObject* BomObjectPy::getIndexOf(PyObject* args) const
{
    PyObject* pyObj = nullptr;
    if (!PyArg_ParseTuple(args, "O!", &App::DocumentObjectPy::Type, &pyObj)) {
        return nullptr;
    }
    PY_TRY
    {
        auto obj = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        return Py::new_reference_to(Py::String(getBomObjectPtr()->getIndexOf(obj)));
    }
    PY_CATCH;
}

PyObject* BomObjectPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int BomObjectPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
