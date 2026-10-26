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

#include <App/DocumentObjectPy.h>
#include <Base/QuantityPy.h>
#include <Base/Tools.h>
#include <Base/VectorPy.h>
#include <Mod/Part/App/GeometryPy.h>

#include "ConstraintPy.h"
#include "Sketch3DObject.h"

// inclusion of the generated files (generated out of Sketch3DObjectPy.xml)
#include "Sketch3DObjectPy.h"
#include "Sketch3DObjectPy.cpp"

using namespace Sketcher;

std::string Sketch3DObjectPy::representation() const
{
    return "<Sketcher::Sketch3DObject>";
}

namespace
{

// A constraint given by index or by name ("Constraint3" for an unnamed one).
int constraintIndex(const Sketch3DObject* sketch, PyObject* key)
{
    if (PyLong_Check(key)) {
        int i = static_cast<int>(PyLong_AsLong(key));
        if (i < 0 || i >= sketch->Constraints.getSize()) {
            throw Py::IndexError("constraint index out of range");
        }
        return i;
    }
    if (PyUnicode_Check(key)) {
        std::string name = PyUnicode_AsUTF8(key);
        const auto& vals = sketch->Constraints.getValues();
        for (std::size_t i = 0; i < vals.size(); ++i) {
            if (PropertyConstraintList::getConstraintName(vals[i]->Name, static_cast<int>(i))
                == name) {
                return static_cast<int>(i);
            }
        }
        throw Py::NameError("no constraint named '" + name + "'");
    }
    throw Py::TypeError("expected a constraint index or name");
}

Py::List indexList(const std::vector<int>& v)
{
    Py::List list;
    for (int i : v) {
        list.append(Py::Long(i));
    }
    return list;
}

}  // namespace

PyObject* Sketch3DObjectPy::solve(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return Py::new_reference_to(Py::Long(getSketch3DObjectPtr()->solve()));
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::addGeometry(PyObject* args)
{
    PyObject* pcObj;
    PyObject* construction = Py_False;
    if (!PyArg_ParseTuple(args, "O|O!", &pcObj, &PyBool_Type, &construction)) {
        return nullptr;
    }
    PY_TRY
    {
        bool cons = PyObject_IsTrue(construction);
        auto add = [&](PyObject* o) {
            if (!PyObject_TypeCheck(o, &(Part::GeometryPy::Type))) {
                throw Py::TypeError("expected a Part geometry");
            }
            auto geo = static_cast<Part::GeometryPy*>(o)->getGeometryPtr();
            return getSketch3DObjectPtr()->addGeometry(geo, cons);
        };
        if (PySequence_Check(pcObj)) {
            Py::Sequence seq(pcObj);
            Py::Tuple res(seq.size());
            for (int i = 0; i < seq.size(); ++i) {
                res.setItem(i, Py::Long(add(seq[i].ptr())));
            }
            return Py::new_reference_to(res);
        }
        return Py::new_reference_to(Py::Long(add(pcObj)));
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::delGeometry(PyObject* args)
{
    int index;
    if (!PyArg_ParseTuple(args, "i", &index)) {
        return nullptr;
    }
    PY_TRY
    {
        getSketch3DObjectPtr()->delGeometry(index);
        Py_Return;
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::addConstraint(PyObject* args)
{
    PyObject* pcObj;
    if (!PyArg_ParseTuple(args, "O", &pcObj)) {
        return nullptr;
    }
    PY_TRY
    {
        auto add = [&](PyObject* o) {
            if (!PyObject_TypeCheck(o, &(ConstraintPy::Type))) {
                throw Py::TypeError("expected a Sketcher.Constraint");
            }
            return getSketch3DObjectPtr()->addConstraint(
                static_cast<ConstraintPy*>(o)->getConstraintPtr());
        };
        if (PySequence_Check(pcObj)) {
            Py::Sequence seq(pcObj);
            Py::Tuple res(seq.size());
            for (int i = 0; i < seq.size(); ++i) {
                res.setItem(i, Py::Long(add(seq[i].ptr())));
            }
            return Py::new_reference_to(res);
        }
        return Py::new_reference_to(Py::Long(add(pcObj)));
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::delConstraint(PyObject* args)
{
    PyObject* key;
    if (!PyArg_ParseTuple(args, "O", &key)) {
        return nullptr;
    }
    PY_TRY
    {
        auto sketch = getSketch3DObjectPtr();
        sketch->delConstraint(constraintIndex(sketch, key));
        Py_Return;
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::setDatum(PyObject* args)
{
    PyObject* key;
    PyObject* value;
    if (!PyArg_ParseTuple(args, "OO", &key, &value)) {
        return nullptr;
    }
    PY_TRY
    {
        auto sketch = getSketch3DObjectPtr();
        int index = constraintIndex(sketch, key);
        double datum;
        if (PyObject_TypeCheck(value, &(Base::QuantityPy::Type))) {
            Base::Quantity q = *static_cast<Base::QuantityPy*>(value)->getQuantityPtr();
            datum = q.getUnit() == Base::Unit::Angle ? Base::toRadians<double>(q.getValue())
                                                     : q.getValue();
        }
        else if (PyNumber_Check(value)) {
            datum = PyFloat_AsDouble(value);
        }
        else {
            throw Py::TypeError("expected a number or a Quantity");
        }
        sketch->setDatum(index, datum);
        Py_Return;
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::getDatum(PyObject* args) const
{
    PyObject* key;
    if (!PyArg_ParseTuple(args, "O", &key)) {
        return nullptr;
    }
    PY_TRY
    {
        auto sketch = getSketch3DObjectPtr();
        int index = constraintIndex(sketch, key);
        double value = sketch->getDatum(index);
        Base::Quantity datum;
        if (sketch->Constraints[index]->Type == Angle) {
            datum.setValue(Base::toDegrees<double>(value));
            datum.setUnit(Base::Unit::Angle);
        }
        else {
            datum.setValue(value);
            datum.setUnit(Base::Unit::Length);
        }
        return new Base::QuantityPy(new Base::Quantity(datum));
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::setDriving(PyObject* args)
{
    PyObject* key;
    PyObject* driving;
    if (!PyArg_ParseTuple(args, "OO!", &key, &PyBool_Type, &driving)) {
        return nullptr;
    }
    PY_TRY
    {
        auto sketch = getSketch3DObjectPtr();
        sketch->setDriving(constraintIndex(sketch, key), PyObject_IsTrue(driving));
        Py_Return;
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::setConstruction(PyObject* args)
{
    int index;
    PyObject* construction;
    if (!PyArg_ParseTuple(args, "iO!", &index, &PyBool_Type, &construction)) {
        return nullptr;
    }
    PY_TRY
    {
        getSketch3DObjectPtr()->setConstruction(index, PyObject_IsTrue(construction));
        Py_Return;
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::addExternal(PyObject* args)
{
    PyObject* pyObj;
    const char* sub = "";
    if (!PyArg_ParseTuple(args, "O!|s", &(App::DocumentObjectPy::Type), &pyObj, &sub)) {
        return nullptr;
    }
    PY_TRY
    {
        auto obj = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        return Py::new_reference_to(Py::Long(getSketch3DObjectPtr()->addExternal(obj, sub)));
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::delExternal(PyObject* args)
{
    int geoId;
    if (!PyArg_ParseTuple(args, "i", &geoId)) {
        return nullptr;
    }
    PY_TRY
    {
        getSketch3DObjectPtr()->delExternal(geoId);
        Py_Return;
    }
    PY_CATCH
}

PyObject* Sketch3DObjectPy::movePoint(PyObject* args)
{
    int geoId, pos;
    PyObject* pcVect;
    PyObject* relative = Py_False;
    if (!PyArg_ParseTuple(args,
                          "iiO!|O!",
                          &geoId,
                          &pos,
                          &(Base::VectorPy::Type),
                          &pcVect,
                          &PyBool_Type,
                          &relative)) {
        return nullptr;
    }
    PY_TRY
    {
        Base::Vector3d v = *static_cast<Base::VectorPy*>(pcVect)->getVectorPtr();
        int res = getSketch3DObjectPtr()->movePoint(geoId,
                                                    static_cast<PointPos>(pos),
                                                    v,
                                                    PyObject_IsTrue(relative));
        return Py::new_reference_to(Py::Long(res));
    }
    PY_CATCH
}

Py::Long Sketch3DObjectPy::getDoF() const
{
    return Py::Long(getSketch3DObjectPtr()->getDoF());
}

Py::List Sketch3DObjectPy::getConflicting() const
{
    return indexList(getSketch3DObjectPtr()->getConflicting());
}

Py::List Sketch3DObjectPy::getRedundant() const
{
    return indexList(getSketch3DObjectPtr()->getRedundant());
}

Py::List Sketch3DObjectPy::getPartiallyRedundant() const
{
    return indexList(getSketch3DObjectPtr()->getPartiallyRedundant());
}

Py::List Sketch3DObjectPy::getMalformed() const
{
    return indexList(getSketch3DObjectPtr()->getMalformed());
}

PyObject* Sketch3DObjectPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int Sketch3DObjectPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
