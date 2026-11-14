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
#include <Base/VectorPy.h>

#include "ExplodedViewStepPy.h"
#include "ExplodedViewStepPy.cpp"

using namespace Assembly;

std::string ExplodedViewStepPy::representation() const
{
    return {"<Exploded View Step>"};
}

PyObject* ExplodedViewStepPy::explodeFrom(PyObject* args) const
{
    PyObject* pyCenter = nullptr;
    double size = 100.0;
    double fraction = 1.0;
    PyObject* paths = Py_False;
    if (!PyArg_ParseTuple(args, "O!d|dO!", &Base::VectorPy::Type, &pyCenter, &size, &fraction,
                          &PyBool_Type, &paths)) {
        return nullptr;
    }
    PY_TRY
    {
        ExplodeFrame frame;
        frame.center = *static_cast<Base::VectorPy*>(pyCenter)->getVectorPtr();
        frame.size = size;
        std::vector<ExplodedPose::Trail> trails;
        ExplodedViewStep::explode(*getExplodedViewStepPtr(), frame, fraction, 0, trails);
        Py::List moves;
        for (auto obj : frame.order) {
            moves.append(Py::TupleN(Py::asObject(obj->getPyObject()),
                                    Py::asObject(new Base::PlacementPy(frame.placed[obj]))));
        }
        Py::List pyTrails;
        for (const auto& trail : trails) {
            if (PyObject_IsTrue(paths)) {
                Py::List points;
                for (const auto& p : trail.path) {
                    points.append(Py::asObject(new Base::VectorPy(p)));
                }
                pyTrails.append(Py::TupleN(Py::asObject(new Base::VectorPy(trail.from)),
                                           Py::asObject(new Base::VectorPy(trail.to)),
                                           points));
            }
            else {
                pyTrails.append(Py::TupleN(Py::asObject(new Base::VectorPy(trail.from)),
                                           Py::asObject(new Base::VectorPy(trail.to))));
            }
        }
        return Py::new_reference_to(Py::TupleN(moves, pyTrails));
    }
    PY_CATCH;
}

PyObject* ExplodedViewStepPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int ExplodedViewStepPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
