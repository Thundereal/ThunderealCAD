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
#include <Mod/Part/App/Geometry.h>
#include <Mod/Part/App/TopoShapePy.h>

#include <TopoDS.hxx>

#include "AssemblyUtils.h"
#include "ElementGeometry.h"
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
        add_varargs_method(
            "getElementGeometry",
            &Module::getElementGeometry,
            "getElementGeometry(shape) -> Part geometry or None\n\n"
            "The analytic geometry of an edge (Part.Line, Part.Circle, Part.Ellipse) or a\n"
            "face (Part.Plane, Part.Cylinder, Part.Cone, Part.Sphere, Part.Toroid), in the\n"
            "shape's coordinates: its own when it is analytic, the recognised one when it\n"
            "is a B-spline or another form within the tolerance of one; None otherwise.\n"
            "The one classification the Assembly workbench uses for joints."
        );
        add_varargs_method(
            "analyzeElement",
            &Module::analyzeElement,
            "analyzeElement(shape) -> dict\n\n"
            "For an edge or a face: Type (the GeomAbs name, e.g. 'Circle', 'BSplineSurface'),\n"
            "Recognized (converted from another form), Gap (its deviation) and Tolerance."
        );
        add_varargs_method(
            "clearElementGeometryCache",
            &Module::clearElementGeometryCache,
            "clearElementGeometryCache() -> None\n\n"
            "Drops the cached recognitions and zeroes getElementGeometryConversions()."
        );
        add_varargs_method(
            "getElementGeometryConversions",
            &Module::getElementGeometryConversions,
            "getElementGeometryConversions() -> int\n\n"
            "How many recognitions ran (not served from the cache) since the last clear."
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

    static TopoDS_Shape shapeArg(const Py::Tuple& args)
    {
        PyObject* pyShape = nullptr;
        if (!PyArg_ParseTuple(args.ptr(), "O!", &Part::TopoShapePy::Type, &pyShape)) {
            throw Py::Exception();
        }
        TopoDS_Shape shape
            = static_cast<Part::TopoShapePy*>(pyShape)->getTopoShapePtr()->getShape();
        if (shape.IsNull()
            || (shape.ShapeType() != TopAbs_EDGE && shape.ShapeType() != TopAbs_FACE)) {
            throw Py::TypeError("an edge or a face is expected");
        }
        return shape;
    }

    Py::Object getElementGeometry(const Py::Tuple& args)
    {
        TopoDS_Shape shape = shapeArg(args);
        if (shape.ShapeType() == TopAbs_EDGE) {
            auto res = analyzeEdge(TopoDS::Edge(shape));
            if (res.curve.IsNull()) {
                return Py::None();
            }
            return Py::asObject(Part::makeFromCurve(res.curve)->getPyObject());
        }
        auto res = analyzeFace(TopoDS::Face(shape));
        if (res.surface.IsNull()) {
            return Py::None();
        }
        return Py::asObject(Part::makeFromSurface(res.surface)->getPyObject());
    }

    static const char* curveTypeName(GeomAbs_CurveType type)
    {
        static const char* const names[] = {
            "Line", "Circle", "Ellipse", "Hyperbola", "Parabola",
            "BezierCurve", "BSplineCurve", "OffsetCurve", "OtherCurve",
        };
        auto i = static_cast<std::size_t>(type);
        return i < std::size(names) ? names[i] : "OtherCurve";
    }

    static const char* surfaceTypeName(GeomAbs_SurfaceType type)
    {
        static const char* const names[] = {
            "Plane", "Cylinder", "Cone", "Sphere", "Torus", "BezierSurface", "BSplineSurface",
            "SurfaceOfRevolution", "SurfaceOfExtrusion", "OffsetSurface", "OtherSurface",
        };
        auto i = static_cast<std::size_t>(type);
        return i < std::size(names) ? names[i] : "OtherSurface";
    }

    Py::Object analyzeElement(const Py::Tuple& args)
    {
        TopoDS_Shape shape = shapeArg(args);
        Py::Dict res;
        if (shape.ShapeType() == TopAbs_EDGE) {
            auto a = analyzeEdge(TopoDS::Edge(shape));
            res.setItem("Type", Py::String(curveTypeName(a.type)));
            res.setItem("Recognized", Py::Boolean(a.recognized));
            res.setItem("Gap", Py::Float(a.gap));
        }
        else {
            auto a = analyzeFace(TopoDS::Face(shape));
            res.setItem("Type", Py::String(surfaceTypeName(a.type)));
            res.setItem("Recognized", Py::Boolean(a.recognized));
            res.setItem("Gap", Py::Float(a.gap));
        }
        res.setItem("Tolerance", Py::Float(recognitionTolerance(shape)));
        return res;
    }

    Py::Object clearElementGeometryCache(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        Assembly::clearElementGeometryCache();
        return Py::None();
    }

    Py::Object getElementGeometryConversions(const Py::Tuple& args)
    {
        if (!PyArg_ParseTuple(args.ptr(), "")) {
            throw Py::Exception();
        }
        return Py::Long(Assembly::elementGeometryConversions());
    }
};

PyObject* initModule()
{
    return Base::Interpreter().addModule(new Module);
}

}  // namespace Assembly
