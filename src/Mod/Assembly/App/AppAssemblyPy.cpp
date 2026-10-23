// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
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


#include <Base/Interpreter.h>
#include <Base/Tools.h>

#include <App/DocumentObjectPy.h>

#include "AssemblyUtils.h"
#include "Solver/Registry.h"


namespace Assembly
{
class Module: public Py::ExtensionModule<Module>
{
public:
    Module()
        : Py::ExtensionModule<Module>("AssemblyApp")
    {
        add_varargs_method(
            "getAvailableSolvers",
            &Module::getAvailableSolvers,
            "getAvailableSolvers() -> list of str\n\n"
            "The names of the registered solver backends, in registration order: the\n"
            "values the SolverBackend property and preference take."
        );
        add_varargs_method(
            "getDistanceType",
            &Module::getDistanceType,
            "getDistanceType(joint) -> str\n\n"
            "The sub-type of a Distance joint, as its references make it ('PlaneCone'):\n"
            "the name of the C++ DistanceType."
        );
        add_varargs_method(
            "getRefusedDistanceTypes",
            &Module::getRefusedDistanceTypes,
            "getRefusedDistanceTypes() -> list of str\n\n"
            "The Distance sub-types the solver has no mapping for: a new joint of one is\n"
            "refused, one already in a document is solved as a planar distance."
        );
        add_varargs_method(
            "isDistanceJointRefused",
            &Module::isDistanceJointRefused,
            "isDistanceJointRefused(joint) -> bool\n\n"
            "True when the joint's sub-type is refused: one of getRefusedDistanceTypes(),\n"
            "except 'Other' with a whole object (a datum, an LCS) on a side."
        );
        initialize("This module is the Assembly module.");  // register with Python
    }

private:
    Py::Object getAvailableSolvers(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        Py::List names;
        for (const auto& name : Solver::Registry::instance().names()) {
            names.append(Py::String(name));
        }
        return names;
    }

    static App::DocumentObject* jointArg(const Py::Tuple& args)
    {
        PyObject* pyJoint = nullptr;
        if (!PyArg_ParseTuple(args.ptr(), "O!", &App::DocumentObjectPy::Type, &pyJoint)) {
            throw Py::Exception();
        }
        return static_cast<App::DocumentObjectPy*>(pyJoint)->getDocumentObjectPtr();
    }

    Py::Object getDistanceType(const Py::Tuple& args)
    {
        return Py::String(Assembly::distanceTypeName(Assembly::getDistanceType(jointArg(args))));
    }

    Py::Object getRefusedDistanceTypes(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        Py::List names;
        for (auto type : refusedDistanceTypes()) {
            names.append(Py::String(distanceTypeName(type)));
        }
        return names;
    }

    Py::Object isDistanceJointRefused(const Py::Tuple& args)
    {
        auto* joint = jointArg(args);
        return Py::Boolean(Assembly::isDistanceJointRefused(joint, Assembly::getDistanceType(joint)));
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace Assembly
