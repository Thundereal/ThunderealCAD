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

#include "SheetMetalFlatPatternPy.h"
#include "SheetMetalFlatPatternPy.cpp"

using namespace PartDesign;

std::string SheetMetalFlatPatternPy::representation() const
{
    return {"<PartDesign::SheetMetalFlatPattern>"};
}

PyObject* SheetMetalFlatPatternPy::exportDXF(PyObject* args) const
{
    char* path;
    if (!PyArg_ParseTuple(args, "et", "utf-8", &path)) {
        return nullptr;
    }
    const std::string name(path);
    PyMem_Free(path);
    PY_TRY
    {
        getSheetMetalFlatPatternPtr()->exportDXF(name.c_str());
        Py_Return;
    }
    PY_CATCH
}

PyObject* SheetMetalFlatPatternPy::exportSVG(PyObject* args) const
{
    char* path;
    if (!PyArg_ParseTuple(args, "et", "utf-8", &path)) {
        return nullptr;
    }
    const std::string name(path);
    PyMem_Free(path);
    PY_TRY
    {
        getSheetMetalFlatPatternPtr()->exportSVG(name.c_str());
        Py_Return;
    }
    PY_CATCH
}

PyObject* SheetMetalFlatPatternPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int SheetMetalFlatPatternPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
