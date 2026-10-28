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

// inclusion of the generated files (generated out of AppliedLoad.pyi)
#include "AppliedLoadPy.h"
#include "AppliedLoadPy.cpp"

using namespace Assembly;

std::string AppliedLoadPy::representation() const
{
    return {"<Assembly::AppliedLoad>"};
}

PyObject* AppliedLoadPy::getDirectionNow(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return new Base::VectorPy(getAppliedLoadPtr()->getDirectionNow());
    }
    PY_CATCH;
}

PyObject* AppliedLoadPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int AppliedLoadPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
