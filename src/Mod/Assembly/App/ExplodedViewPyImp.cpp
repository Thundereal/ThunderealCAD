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

#include <Mod/Part/App/PartPyCXX.h>

#include "AssemblyObject.h"
#include "ExplodedViewPy.h"
#include "ExplodedViewPy.cpp"

using namespace Assembly;

std::string ExplodedViewPy::representation() const
{
    return {"<Exploded View>"};
}

PyObject* ExplodedViewPy::getAssembly(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        auto assembly = getExplodedViewPtr()->getAssembly();
        if (!assembly) {
            Py_Return;
        }
        return assembly->getPyObject();
    }
    PY_CATCH;
}

PyObject* ExplodedViewPy::getSteps(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        Py::List res;
        for (auto step : getExplodedViewPtr()->getSteps()) {
            res.append(Py::asObject(step->getPyObject()));
        }
        return Py::new_reference_to(res);
    }
    PY_CATCH;
}

PyObject* ExplodedViewPy::getCenterAndSize(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        Base::Vector3d center;
        double size = 0;
        getExplodedViewPtr()->getCenterAndSize(center, size);
        return Py::new_reference_to(
            Py::TupleN(Py::asObject(new Base::VectorPy(center)), Py::Float(size)));
    }
    PY_CATCH;
}

PyObject* ExplodedViewPy::explodedAt(PyObject* args) const
{
    PyObject* pyProgress = Py_None;
    PyObject* paths = Py_False;
    if (!PyArg_ParseTuple(args, "|OO!", &pyProgress, &PyBool_Type, &paths)) {
        return nullptr;
    }
    PY_TRY
    {
        std::optional<double> progress;
        if (pyProgress != Py_None) {
            double value = PyFloat_AsDouble(pyProgress);
            if (value == -1.0 && PyErr_Occurred()) {
                return nullptr;
            }
            progress = value;
        }
        auto pose = getExplodedViewPtr()->explodedAt(progress);
        Py::List moves;
        for (const auto& move : pose.moves) {
            moves.append(Py::TupleN(Py::asObject(move.object->getPyObject()),
                                    Py::asObject(new Base::PlacementPy(move.placement))));
        }
        Py::List trails;
        for (const auto& trail : pose.trails) {
            if (PyObject_IsTrue(paths)) {
                Py::List points;
                for (const auto& p : trail.path) {
                    points.append(Py::asObject(new Base::VectorPy(p)));
                }
                trails.append(Py::TupleN(Py::Long(trail.step),
                                         Py::asObject(new Base::VectorPy(trail.from)),
                                         Py::asObject(new Base::VectorPy(trail.to)),
                                         points));
            }
            else {
                trails.append(Py::TupleN(Py::Long(trail.step),
                                         Py::asObject(new Base::VectorPy(trail.from)),
                                         Py::asObject(new Base::VectorPy(trail.to))));
            }
        }
        return Py::new_reference_to(Py::TupleN(moves, trails));
    }
    PY_CATCH;
}

PyObject* ExplodedViewPy::getExplodedShape(PyObject* args) const
{
    PyObject* pyProgress = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &pyProgress)) {
        return nullptr;
    }
    PY_TRY
    {
        std::optional<double> progress;
        if (pyProgress != Py_None) {
            double value = PyFloat_AsDouble(pyProgress);
            if (value == -1.0 && PyErr_Occurred()) {
                return nullptr;
            }
            progress = value;
        }
        return Py::new_reference_to(
            Part::shape2pyshape(getExplodedViewPtr()->getExplodedShape(progress)));
    }
    PY_CATCH;
}

PyObject* ExplodedViewPy::pose(PyObject* args)
{
    PyObject* pyProgress = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &pyProgress)) {
        return nullptr;
    }
    PY_TRY
    {
        std::optional<double> progress;
        if (pyProgress != Py_None) {
            double value = PyFloat_AsDouble(pyProgress);
            if (value == -1.0 && PyErr_Occurred()) {
                return nullptr;
            }
            progress = value;
        }
        getExplodedViewPtr()->pose(progress);
        Py_Return;
    }
    PY_CATCH;
}

PyObject* ExplodedViewPy::unpose(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        getExplodedViewPtr()->unpose();
        Py_Return;
    }
    PY_CATCH;
}

PyObject* ExplodedViewPy::posedAt(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    auto at = getExplodedViewPtr()->posedAt();
    if (!at) {
        Py_Return;
    }
    return PyFloat_FromDouble(*at);
}

PyObject* ExplodedViewPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int ExplodedViewPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
