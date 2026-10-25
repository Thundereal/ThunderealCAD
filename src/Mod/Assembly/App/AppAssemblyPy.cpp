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

#include <cstring>
#include <map>
#include <memory>

#include <App/Document.h>
#include <App/DocumentObjectPy.h>
#include <App/ObjectIdentifier.h>
#include <App/PropertyLinks.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Part/App/TopoShapePy.h>

#include <TopoDS.hxx>

#include "AssemblyUtils.h"
#include "CamJoint.h"
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
            "True when a new Distance joint between the joint's elements is refused:\n"
            "getDistanceRefusal(joint) is not empty."
        );
        add_varargs_method(
            "getDistanceRefusal",
            &Module::getDistanceRefusal,
            "getDistanceRefusal(joint) -> str\n\n"
            "Why a new Distance joint between the joint's elements is refused, naming\n"
            "the element and what it is ('the face Face1 of Loft is not planar, ...');\n"
            "'' when it is not."
        );
        add_varargs_method(
            "getCamRefusal",
            &Module::getCamRefusal,
            "getCamRefusal(joint) -> str\n\n"
            "Why the joint's picks cannot make a Cam joint, naming the element and what\n"
            "it should be ('the face Face1 of Cam is not an edge: ...'); '' when they can.\n"
            "Reference1 is the cam (an edge), Reference2 the follower (a vertex, or a\n"
            "circular edge: a roller); a pick not made yet is not checked."
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
            "replaceObjectLinks",
            &Module::replaceObjectLinks,
            "replaceObjectLinks(old, new) -> int\n\n"
            "Re-points what links to 'old' at 'new' -- link properties and\n"
            "expressions of every other object of the document -- as a link's\n"
            "replacement does; not the groups that list 'old' (its removal takes\n"
            "it out of those). Returns how many properties changed."
        );
        add_varargs_method(
            "renamePropertyReferences",
            &Module::renamePropertyReferences,
            "renamePropertyReferences(obj, {old: new}) -> None\n\n"
            "Re-points the expressions of every object of obj's document that\n"
            "name obj's property 'old' at its property 'new', as for a property\n"
            "renamed. The bindings of obj's own expressions are not moved."
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

    Py::Object getDistanceRefusal(const Py::Tuple& args)
    {
        auto* joint = jointArg(args);
        return Py::String(Assembly::distanceJointRefusal(joint, Assembly::getDistanceType(joint)));
    }

    Py::Object replaceObjectLinks(const Py::Tuple& args)
    {
        PyObject *pyOld = nullptr, *pyNew = nullptr;
        if (!PyArg_ParseTuple(
                args.ptr(), "O!O!", &App::DocumentObjectPy::Type, &pyOld,
                &App::DocumentObjectPy::Type, &pyNew)) {
            throw Py::Exception();
        }
        auto* oldObj = static_cast<App::DocumentObjectPy*>(pyOld)->getDocumentObjectPtr();
        auto* newObj = static_cast<App::DocumentObjectPy*>(pyNew)->getDocumentObjectPtr();
        if (!oldObj->getDocument() || oldObj->getDocument() != newObj->getDocument()) {
            throw Py::ValueError("both objects must be in one document");
        }
        std::vector<std::pair<App::Property*, std::unique_ptr<App::Property>>> changes;
        std::vector<App::Property*> props;
        for (auto* obj : oldObj->getDocument()->getObjects()) {
            if (obj == oldObj || obj == newObj) {
                continue;
            }
            props.clear();
            obj->getPropertyList(props);
            for (auto* prop : props) {
                auto* link = Base::freecad_dynamic_cast<App::PropertyLinkBase>(prop);
                if (!link || prop->getContainer() != obj) {
                    continue;
                }
                // A group drops the old object when it is removed; replacing
                // it there would list the new one twice
                const char* name = prop->getName();
                if (name && (std::strcmp(name, "Group") == 0 || name[0] == '_')) {
                    continue;
                }
                std::unique_ptr<App::Property> copy(link->CopyOnLinkReplace(obj, oldObj, newObj));
                if (copy) {
                    changes.emplace_back(prop, std::move(copy));
                }
            }
        }
        for (auto& change : changes) {
            change.first->Paste(*change.second);
        }
        return Py::Long(static_cast<long>(changes.size()));
    }

    Py::Object renamePropertyReferences(const Py::Tuple& args)
    {
        PyObject *pyObj = nullptr, *pyNames = nullptr;
        if (!PyArg_ParseTuple(
                args.ptr(), "O!O!", &App::DocumentObjectPy::Type, &pyObj, &PyDict_Type, &pyNames)) {
            throw Py::Exception();
        }
        auto* obj = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        if (!obj->getDocument()) {
            throw Py::ValueError("the object is not in a document");
        }
        std::map<App::ObjectIdentifier, App::ObjectIdentifier> paths;
        for (const auto& item : Py::Dict(pyNames)) {
            std::string from = Py::String(item.first).as_std_string();
            std::string to = Py::String(item.second).as_std_string();
            paths.emplace(
                App::ObjectIdentifier(obj, from.c_str()),
                App::ObjectIdentifier(obj, to.c_str())
            );
        }
        obj->getDocument()->renameObjectIdentifiers(paths);
        return Py::None();
    }

    Py::Object getCamRefusal(const Py::Tuple& args)
    {
        return Py::String(Assembly::camJointRefusal(jointArg(args)));
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
