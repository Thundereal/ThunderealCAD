// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2024 Ondsel <development@ondsel.com>                     *
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


// inclusion of the generated files (generated out of AssemblyObject.xml)
#include <array>
#include <cstring>

#include "AssemblyObjectPy.h"
#include "AssemblyObjectPy.cpp"

#include "Joint.h"

#include <Base/GeometryPyCXX.h>
#include <Base/PlacementPy.h>
#include <Base/VectorPy.h>
#include <Base/PyWrapParseTupleAndKeywords.h>
#include <App/Document.h>
#include <App/DocumentObjectPy.h>
#include <Mod/Part/App/OCCError.h>

#include "Interference.h"
#include "MassProperties.h"
#include "Solver/Backend.h"

using namespace Assembly;

namespace
{
// A pick: (component, subname) or (component, subname, vertex)
bool parsePick(PyObject* pick, App::DocumentObject*& obj, const char*& sub, const char*& vertex)
{
    PyObject* pyobj = nullptr;
    sub = "";
    vertex = nullptr;
    if (!PyArg_ParseTuple(pick, "O!|ss", &(App::DocumentObjectPy::Type), &pyobj, &sub, &vertex)) {
        return false;
    }
    obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    return true;
}
}  // namespace

PyObject* AssemblyObjectPy::makeJoint(PyObject* args)
{
    const char* type = nullptr;
    PyObject* pick1 = nullptr;
    PyObject* pick2 = nullptr;
    if (!PyArg_ParseTuple(args, "sO!O!", &type, &PyTuple_Type, &pick1, &PyTuple_Type, &pick2)) {
        return nullptr;
    }
    App::DocumentObject *obj1 = nullptr, *obj2 = nullptr;
    const char *sub1 = nullptr, *sub2 = nullptr, *vertex1 = nullptr, *vertex2 = nullptr;
    if (!parsePick(pick1, obj1, sub1, vertex1) || !parsePick(pick2, obj2, sub2, vertex2)) {
        return nullptr;
    }
    PY_TRY
    {
        auto joint = Joint::make(getAssemblyObjectPtr(), type, obj1, sub1, vertex1, obj2, sub2, vertex2);
        return joint->getPyObject();
    }
    PY_CATCH;
}

// returns a string which represents the object e.g. when printed in python
std::string AssemblyObjectPy::representation() const
{
    return {"<Assembly object>"};
}

PyObject* AssemblyObjectPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int AssemblyObjectPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}

PyObject* AssemblyObjectPy::solve(PyObject* args) const
{
    PyObject* enableUndoPy;
    bool enableUndo;

    if (!PyArg_ParseTuple(args, "O!", &PyBool_Type, &enableUndoPy)) {
        PyErr_Clear();
        if (!PyArg_ParseTuple(args, "")) {
            return nullptr;
        }
        else {
            enableUndo = false;
        }
    }
    else {
        enableUndo = Base::asBoolean(enableUndoPy);
    }

    int ret = this->getAssemblyObjectPtr()->solve(enableUndo);
    return Py_BuildValue("i", ret);
}

PyObject* AssemblyObjectPy::generateSimulation(PyObject* args) const
{
    PyObject* pyobj;

    if (!PyArg_ParseTuple(args, "O", &pyobj)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    int ret = this->getAssemblyObjectPtr()->generateSimulation(obj);
    return Py_BuildValue("i", ret);
}

PyObject* AssemblyObjectPy::ensureIdentityPlacements(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->ensureIdentityPlacements();
    Py_Return;
}

PyObject* AssemblyObjectPy::refreshFrozenShape(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        this->getAssemblyObjectPtr()->refreshFrozenShape();
        Py_Return;
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::updateForFrame(PyObject* args) const
{
    unsigned long index {};

    if (!PyArg_ParseTuple(args, "k", &index)) {
        throw Py::RuntimeError("updateForFrame requires an integer index");
    }
    PY_TRY
    {
        this->getAssemblyObjectPtr()->updateForFrame(index);
    }
    PY_CATCH;

    Py_Return;
}

PyObject* AssemblyObjectPy::updateForTime(PyObject* args) const
{
    double time {};
    if (!PyArg_ParseTuple(args, "d", &time)) {
        return nullptr;
    }
    PY_TRY
    {
        this->getAssemblyObjectPtr()->updateForTime(time);
    }
    PY_CATCH;

    Py_Return;
}

PyObject* AssemblyObjectPy::startSimulation(PyObject* args)
{
    PyObject* pyObj {};
    if (!PyArg_ParseTuple(args, "O!", &App::DocumentObjectPy::Type, &pyObj)) {
        return nullptr;
    }
    PY_TRY
    {
        auto* sim = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        return Py::new_reference_to(Py::Long(getAssemblyObjectPtr()->startSimulation(sim)));
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::getSimulationProgress(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    const auto progress = getAssemblyObjectPtr()->simulationProgress();
    Py::Dict ret;
    ret.setItem("Running", Py::Boolean(progress.running));
    ret.setItem("Done", Py::Boolean(progress.done));
    ret.setItem("Time", Py::Float(progress.time));
    ret.setItem("Frames", Py::Long(static_cast<long>(progress.frames)));
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::cancelSimulation(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    getAssemblyObjectPtr()->cancelSimulation();
    Py_Return;
}

PyObject* AssemblyObjectPy::finishSimulation(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        return Py::new_reference_to(Py::Long(getAssemblyObjectPtr()->finishSimulation()));
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::numberOfFrames(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    size_t ret = this->getAssemblyObjectPtr()->numberOfFrames();
    return Py_BuildValue("k", ret);
}

PyObject* AssemblyObjectPy::getSimulationResults(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        Py::List times;
        Py::List channels;
        const Solver::Trajectory* trajectory = getAssemblyObjectPtr()->lastTrajectory();
        if (trajectory) {
            for (double time : trajectory->times) {
                times.append(Py::Float(time));
            }
            std::size_t width = 0;
            for (const auto& channel : trajectory->channels) {
                width += channel.width;
            }
            const std::size_t frames = width ? trajectory->values.size() / width : 0;
            App::Document* doc = getAssemblyObjectPtr()->getDocument();
            std::size_t offset = 0;
            for (const auto& channel : trajectory->channels) {
                // The model's source names are full names, "Doc#Object"
                const std::size_t hash = channel.source.rfind('#');
                const std::string name =
                    hash == std::string::npos ? channel.source : channel.source.substr(hash + 1);
                App::DocumentObject* obj = doc ? doc->getObject(name.c_str()) : nullptr;
                Py::List values;
                for (std::size_t f = 0; f < frames; ++f) {
                    const double* v = &trajectory->values[f * width + offset];
                    if (channel.width == 3) {
                        values.append(Py::Vector(Base::Vector3d(v[0], v[1], v[2])));
                    }
                    else if (channel.width == 1) {
                        values.append(Py::Float(v[0]));
                    }
                    else {
                        // A wider one's numbers as they are, a tuple
                        Py::Tuple row(channel.width);
                        for (int k = 0; k < channel.width; ++k) {
                            row.setItem(k, Py::Float(v[k]));
                        }
                        values.append(row);
                    }
                }
                Py::Dict entry;
                entry.setItem("Object", obj ? Py::asObject(obj->getPyObject()) : Py::None());
                entry.setItem("Name", Py::String(name));
                entry.setItem("Quantity", Py::String(channel.quantity));
                entry.setItem("Unique", Py::Boolean(channel.unique));
                // The part a value is on, where its source has several (a
                // contact set's); None, the source's own
                Py::Object on = Py::None();
                if (!channel.on.empty()) {
                    const std::size_t at = channel.on.rfind('#');
                    const std::string part =
                        at == std::string::npos ? channel.on : channel.on.substr(at + 1);
                    App::DocumentObject* partObj = doc ? doc->getObject(part.c_str()) : nullptr;
                    if (partObj) {
                        on = Py::asObject(partObj->getPyObject());
                    }
                }
                entry.setItem("On", on);
                entry.setItem("Values", values);
                channels.append(entry);
                offset += channel.width;
            }
        }
        Py::List events;
        if (trajectory) {
            App::Document* doc = getAssemblyObjectPtr()->getDocument();
            auto nameOf = [](const std::string& source) {
                const std::size_t hash = source.rfind('#');
                return hash == std::string::npos ? source : source.substr(hash + 1);
            };
            auto objectOf = [&](const std::string& name) {
                App::DocumentObject* obj =
                    doc && !name.empty() ? doc->getObject(name.c_str()) : nullptr;
                return obj ? Py::asObject(obj->getPyObject()) : Py::None();
            };
            for (const auto& event : trajectory->events) {
                const std::string name = nameOf(event.source);
                Py::Dict entry;
                entry.setItem("Time", Py::Float(event.time));
                entry.setItem("Object", objectOf(name));
                entry.setItem("Name", Py::String(name));
                entry.setItem("Value", Py::Float(event.value));
                entry.setItem("Stopped", Py::Boolean(event.stopped));
                const char* kind = "Sensor";
                if (event.kind != Solver::Event::Kind::Sensor) {
                    kind = event.kind == Solver::Event::Kind::ContactBegin ? "ContactBegin"
                                                                           : "ContactEnd";
                    // The two components, None for the ground
                    Py::Tuple bodies(2);
                    bodies.setItem(0, objectOf(nameOf(event.bodyA)));
                    bodies.setItem(1, objectOf(nameOf(event.bodyB)));
                    entry.setItem("Bodies", bodies);
                }
                entry.setItem("Kind", Py::String(kind));
                events.append(entry);
            }
        }
        Py::Dict ret;
        ret.setItem("Times", times);
        ret.setItem("Channels", channels);
        ret.setItem("Events", events);
        App::DocumentObject* sim = getAssemblyObjectPtr()->lastSimulation();
        ret.setItem("Simulation", sim ? Py::asObject(sim->getPyObject()) : Py::None());
        return Py::new_reference_to(ret);
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::isSimulationStale(PyObject* args)
{
    PyObject* pyObj {};
    if (!PyArg_ParseTuple(args, "O!", &App::DocumentObjectPy::Type, &pyObj)) {
        return nullptr;
    }
    PY_TRY
    {
        auto* sim = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        return Py::new_reference_to(Py::Boolean(getAssemblyObjectPtr()->isSimulationStale(sim)));
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::loadSimulationResults(PyObject* args)
{
    PyObject* pyObj {};
    if (!PyArg_ParseTuple(args, "O!", &App::DocumentObjectPy::Type, &pyObj)) {
        return nullptr;
    }
    PY_TRY
    {
        auto* sim = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        return Py::new_reference_to(
            Py::Boolean(getAssemblyObjectPtr()->loadSimulationResults(sim)));
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::getSimulationPlacements(PyObject* args, PyObject* kwds) const
{
    PyObject* pyObj {};
    static const std::array<const char*, 2> keywords {"obj", nullptr};
    if (!Base::Wrapped_ParseTupleAndKeywords(args,
                                             kwds,
                                             "O!",
                                             keywords,
                                             &App::DocumentObjectPy::Type,
                                             &pyObj)) {
        return nullptr;
    }
    PY_TRY
    {
        auto* obj = static_cast<App::DocumentObjectPy*>(pyObj)->getDocumentObjectPtr();
        const AssemblyObject* assembly = getAssemblyObjectPtr();
        Py::List placements;
        Base::Placement placement;
        for (std::size_t i = 0; i < assembly->numberOfFrames(); ++i) {
            if (!assembly->placementAtFrame(obj, i, placement)) {
                return Py::new_reference_to(Py::List());
            }
            placements.append(Py::asObject(new Base::PlacementPy(new Base::Placement(placement))));
        }
        return Py::new_reference_to(placements);
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::updateSolveStatus(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    this->getAssemblyObjectPtr()->updateSolveStatus();
    Py_Return;
}

PyObject* AssemblyObjectPy::undoSolve(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->undoSolve();
    Py_Return;
}

PyObject* AssemblyObjectPy::clearUndo(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->clearUndo();
    Py_Return;
}

PyObject* AssemblyObjectPy::getGroup(PyObject* args)
{
    const char* role = nullptr;
    PyObject* create = Py_False;
    if (!PyArg_ParseTuple(args, "s|O!", &role, &PyBool_Type, &create)) {
        return nullptr;
    }
    PY_TRY
    {
        auto group = getAssemblyObjectPtr()->getAssemblyGroup(role, PyObject_IsTrue(create) != 0);
        if (!group) {
            Py_Return;
        }
        return group->getPyObject();
    }
    PY_CATCH;
}

PyObject* AssemblyObjectPy::isPartConnected(PyObject* args) const
{
    PyObject* pyobj;

    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyobj)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isPartConnected(obj);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::isPartGrounded(PyObject* args) const
{
    PyObject* pyobj;

    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyobj)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isPartGrounded(obj);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::isJointConnectingPartToGround(PyObject* args) const
{
    PyObject* pyobj;
    char* pname;

    if (!PyArg_ParseTuple(args, "O!s", &(App::DocumentObjectPy::Type), &pyobj, &pname)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isJointConnectingPartToGround(obj, pname);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::exportAsASMT(PyObject* args) const
{
    char* utf8Name;
    if (!PyArg_ParseTuple(args, "et", "utf-8", &utf8Name)) {
        return nullptr;
    }

    std::string fileName = utf8Name;
    PyMem_Free(utf8Name);

    if (fileName.empty()) {
        PyErr_SetString(PyExc_ValueError, "Passed string is empty");
        return nullptr;
    }

    this->getAssemblyObjectPtr()->exportAsASMT(fileName);

    Py_Return;
}

PyObject* AssemblyObjectPy::preDrag(PyObject* args) const
{
    PyObject* pyParts;
    double mass = 10.0;
    if (!PyArg_ParseTuple(args, "O|d", &pyParts, &mass)) {
        return nullptr;
    }

    PY_TRY
    {
        std::vector<App::DocumentObject*> parts;
        std::vector<std::pair<App::DocumentObject*, double>> masses;
        Py::Sequence seq(pyParts);
        for (Py::Sequence::iterator it = seq.begin(); it != seq.end(); ++it) {
            PyObject* item = (*it).ptr();
            if (!PyObject_TypeCheck(item, &App::DocumentObjectPy::Type)) {
                throw Py::TypeError("preDrag: expected a sequence of document objects");
            }
            auto* obj = static_cast<App::DocumentObjectPy*>(item)->getDocumentObjectPtr();
            parts.push_back(obj);
            masses.emplace_back(obj, mass);
        }

        auto* assembly = getAssemblyObjectPtr();
        assembly->setObjMasses(masses);
        assembly->preDrag(parts);
    }
    PY_CATCH;

    Py_Return;
}

PyObject* AssemblyObjectPy::doDragStep(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    bool ok = false;
    PY_TRY
    {
        ok = getAssemblyObjectPtr()->doDragStep();
    }
    PY_CATCH;

    return Py::new_reference_to(Py::Boolean(ok));
}

PyObject* AssemblyObjectPy::postDrag(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    PY_TRY
    {
        auto* assembly = getAssemblyObjectPtr();
        assembly->postDrag();
        assembly->setObjMasses({});
    }
    PY_CATCH;

    Py_Return;
}

PyObject* AssemblyObjectPy::getDragStatus(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    auto* assembly = getAssemblyObjectPtr();
    Py::Dict ret;
    ret.setItem("Steps", Py::Long(assembly->getDragStepCount()));
    ret.setItem("Failures", Py::Long(assembly->getDragStepFailures()));
    ret.setItem("LastStepFailed", Py::Boolean(assembly->getLastDragStepFailed()));
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::getDragCollisions(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    Py::List ret;
    for (const auto& hit : getAssemblyObjectPtr()->getDragCollisions()) {
        ret.append(Py::TupleN(Py::asObject(hit.first->getPyObject()),
                              Py::asObject(hit.second->getPyObject())));
    }
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::getFlexibleFrequencies(PyObject* args)
{
    PyObject* sim = nullptr;
    PyObject* flexible = nullptr;
    if (!PyArg_ParseTuple(args,
                          "O!O!",
                          &App::DocumentObjectPy::Type,
                          &sim,
                          &App::DocumentObjectPy::Type,
                          &flexible)) {
        return nullptr;
    }
    PY_TRY
    {
        std::string error;
        const auto hz = getAssemblyObjectPtr()->getFlexibleFrequencies(
            static_cast<App::DocumentObjectPy*>(sim)->getDocumentObjectPtr(),
            static_cast<App::DocumentObjectPy*>(flexible)->getDocumentObjectPtr(),
            &error);
        if (hz.empty()) {
            PyErr_SetString(Base::PyExc_FC_GeneralError,
                            error.empty() ? "no modes found" : error.c_str());
            return nullptr;
        }
        Py::List list;
        for (double f : hz) {
            list.append(Py::Float(f));
        }
        return Py::new_reference_to(list);
    }
    PY_CATCH_OCC
}

PyObject* AssemblyObjectPy::getBodyInertias(PyObject* args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        auto* assembly = getAssemblyObjectPtr();
        const auto bodies = assembly->getBodyInertias();
        Py::List list;
        for (const auto& [inertia, placement, objects] : bodies) {
            Py::Dict entry;
            Py::List members;
            for (auto* obj : objects) {
                members.append(Py::asObject(obj->getPyObject()));
            }
            entry.setItem("Objects", members);
            entry.setItem("Placement", Py::asObject(new Base::PlacementPy(placement)));
            entry.setItem("Mass", Py::Float(inertia.mass));
            entry.setItem("CenterOfMass", Py::Vector(inertia.com));
            Py::Tuple values(6);
            for (int i = 0; i < 6; ++i) {
                values.setItem(i, Py::Float(inertia.inertia[i]));
            }
            entry.setItem("Inertia", values);
            const char* origin = "Shape";
            if (inertia.origin == Solver::Inertia::Origin::Override) {
                origin = "Override";
            }
            else if (inertia.origin == Solver::Inertia::Origin::AssumedDensity) {
                origin = "AssumedDensity";
            }
            entry.setItem("Origin", Py::String(origin));
            list.append(entry);
        }
        return Py::new_reference_to(list);
    }
    PY_CATCH_OCC
}

PyObject* AssemblyObjectPy::getMassProperties(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    PY_TRY
    {
        auto asDict = [](const MassProperties& props) {
            Py::Dict entry;
            entry.setItem("Object",
                          props.obj ? Py::asObject(props.obj->getPyObject()) : Py::None());
            const char* origin = "None";
            switch (props.origin) {
                case MassProperties::Origin::Shape:
                    origin = "Shape";
                    break;
                case MassProperties::Origin::Assumed:
                    origin = "Assumed";
                    break;
                case MassProperties::Origin::Override:
                    origin = "Override";
                    break;
                case MassProperties::Origin::None:
                    break;
            }
            entry.setItem("Origin", Py::String(origin));
            entry.setItem("Material", Py::String(props.material));
            entry.setItem("Density", Py::Float(props.density));
            entry.setItem("Volume", Py::Float(props.volume));
            entry.setItem("Mass", Py::Float(props.mass));
            entry.setItem("CenterOfMass", Py::Vector(props.com));
            Py::Tuple inertia(6);
            for (int i = 0; i < 6; ++i) {
                inertia.setItem(i, Py::Float(props.inertia[i]));
            }
            entry.setItem("Inertia", inertia);
            // For contact by Hertz's law (ContactSet StiffnessFrom
            // Materials): Young's modulus in kPa
            if (props.obj) {
                const Elasticity elastic = elasticityOf(props.obj);
                entry.setItem("YoungsModulus", Py::Float(elastic.youngsModulus));
                entry.setItem("PoissonRatio", Py::Float(elastic.poissonRatio));
                entry.setItem("ElasticMaterial", Py::String(elastic.material));
            }
            return entry;
        };
        const auto components = massPropertiesOf(getAssemblyObjectPtr());
        Py::List list;
        for (const auto& props : components) {
            list.append(asDict(props));
        }
        Py::Dict ret;
        ret.setItem("Components", list);
        ret.setItem("Total", asDict(totalOf(components)));
        return Py::new_reference_to(ret);
    }
    PY_CATCH_OCC
}

PyObject* AssemblyObjectPy::getCollisionMarkers(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    auto object = [](const App::DocumentObjectT& ref) -> Py::Object {
        auto* obj = ref.getObject();
        return obj ? Py::asObject(obj->getPyObject()) : Py::None();
    };
    Py::List ret;
    for (const auto& marker : getAssemblyObjectPtr()->getCollisionMarkers()) {
        Py::Dict entry;
        entry.setItem("First", object(marker.first));
        entry.setItem("Second", object(marker.second));
        entry.setItem("Centre", Py::Vector(marker.centre));
        entry.setItem("Direction", Py::Vector(marker.direction));
        entry.setItem("Contact", Py::Boolean(marker.kind == InterferenceResult::Kind::Contact));
        const char* kind = "Interference";
        if (marker.kind == InterferenceResult::Kind::Contact) {
            kind = "Contact";
        }
        else if (marker.kind == InterferenceResult::Kind::Clearance) {
            kind = "Clearance";
        }
        entry.setItem("Kind", Py::String(kind));
        entry.setItem("Volume", Py::Float(marker.volume));
        entry.setItem("Area", Py::Float(marker.area));
        entry.setItem("Distance", Py::Float(marker.distance));
        entry.setItem("Clearance", Py::Float(marker.clearance));
        entry.setItem("FromDrag", Py::Boolean(marker.watch != 0));
        for (const auto& [key, names] :
             {std::pair {"Faces1", &marker.faces1}, std::pair {"Faces2", &marker.faces2}}) {
            Py::List list;
            for (const auto& name : *names) {
                list.append(Py::String(name));
            }
            entry.setItem(key, list);
        }
        ret.append(entry);
    }
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::addCollisionMarker(PyObject* args)
{
    PyObject* dict {};
    if (!PyArg_ParseTuple(args, "O!", &PyDict_Type, &dict)) {
        return nullptr;
    }
    PY_TRY
    {
        Py::Dict entry(dict);
        auto object = [&](const char* key) -> App::DocumentObject* {
            Py::Object value = entry.getItem(key);
            if (!PyObject_TypeCheck(value.ptr(), &App::DocumentObjectPy::Type)) {
                throw Py::TypeError(std::string(key) + " is not a document object");
            }
            return static_cast<App::DocumentObjectPy*>(value.ptr())->getDocumentObjectPtr();
        };
        auto vector = [&](const char* key) {
            Py::Object value = entry.getItem(key);
            if (!PyObject_TypeCheck(value.ptr(), &Base::VectorPy::Type)) {
                throw Py::TypeError(std::string(key) + " is not a vector");
            }
            return *static_cast<Base::VectorPy*>(value.ptr())->getVectorPtr();
        };
        auto number = [&](const char* key) {
            return entry.hasKey(key) ? static_cast<double>(Py::Float(entry.getItem(key))) : 0.0;
        };
        auto names = [&](const char* key) {
            std::vector<std::string> list;
            if (entry.hasKey(key)) {
                Py::Sequence seq(entry.getItem(key));
                for (Py::Sequence::iterator it = seq.begin(); it != seq.end(); ++it) {
                    list.push_back(Py::String(*it).as_std_string("utf-8"));
                }
            }
            return list;
        };
        InterferenceResult result;
        result.first = object("Object1");
        result.second = object("Object2");
        result.touched = true;
        result.touchCentre = vector("Centre");
        result.touchNormal = vector("Direction");
        const std::string kind =
            entry.hasKey("Kind") ? Py::String(entry.getItem("Kind")).as_std_string("utf-8") : "";
        if (kind == "Contact") {
            result.kind = InterferenceResult::Kind::Contact;
        }
        else if (kind == "Clearance") {
            result.kind = InterferenceResult::Kind::Clearance;
        }
        result.volume = number("Volume");
        result.touchArea = number("Area");
        result.distance = number("Distance");
        result.clearance = number("Clearance");
        result.faceNames1 = names("Faces1");
        result.faceNames2 = names("Faces2");
        getAssemblyObjectPtr()->addCollisionMarker(result);
        Py_Return;
    }
    PY_CATCH
}

PyObject* AssemblyObjectPy::clearCollisionMarkers(PyObject* args)
{
    PyObject* checksOnly = Py_False;
    if (!PyArg_ParseTuple(args, "|O!", &PyBool_Type, &checksOnly)) {
        return nullptr;
    }
    getAssemblyObjectPtr()->clearCollisionMarkers(checksOnly == Py_True);
    Py_Return;
}

PyObject* AssemblyObjectPy::getSolveStatus(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    auto* assembly = getAssemblyObjectPtr();
    auto toList = [](const std::vector<std::string>& names) {
        Py::List list;
        for (const auto& name : names) {
            list.append(Py::String(name));
        }
        return list;
    };

    Py::Dict ret;
    ret.setItem("DoF", Py::Long(assembly->getLastDoF()));
    ret.setItem("SolverStatus", Py::Long(assembly->getLastSolverStatus()));
    ret.setItem("Conflicting", toList(assembly->getLastConflicting()));
    ret.setItem("Redundant", toList(assembly->getLastRedundant()));
    ret.setItem("PartiallyRedundant", toList(assembly->getLastPartiallyRedundant()));
    ret.setItem("Malformed", toList(assembly->getLastMalformed()));
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::getSolverResiduals(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    Py::Dict ret;
    for (const auto& [name, error] : getAssemblyObjectPtr()->getSolverResiduals()) {
        ret.setItem(name, Py::Float(error));
    }
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::findInterferences(PyObject* args, PyObject* kwds) const
{
    const char* mode = "Exact";
    double tolerance = 0.0;
    int contacts = 0;
    PyObject* objects = Py_None;
    double deflection = 0.0;
    PyObject* frames = Py_None;
    int rules = 1;
    static const std::array<const char*, 8> keywords {"mode",
                                                      "tolerance",
                                                      "contacts",
                                                      "objects",
                                                      "deflection",
                                                      "frames",
                                                      "rules",
                                                      nullptr};
    if (!Base::Wrapped_ParseTupleAndKeywords(args,
                                             kwds,
                                             "|sdpOdOp",
                                             keywords,
                                             &mode,
                                             &tolerance,
                                             &contacts,
                                             &objects,
                                             &deflection,
                                             &frames,
                                             &rules)) {
        return nullptr;
    }

    PY_TRY
    {
        InterferenceOptions options;
        if (std::strcmp(mode, "Fast") == 0) {
            options.mode = InterferenceOptions::Mode::Fast;
        }
        else if (std::strcmp(mode, "Exact") != 0) {
            PyErr_SetString(PyExc_ValueError, "mode is \"Exact\" or \"Fast\"");
            return nullptr;
        }
        options.tolerance = tolerance;
        options.contacts = contacts != 0;
        options.deflection = deflection;
        options.rules = rules != 0;

        std::vector<App::DocumentObject*> only;
        if (objects != Py_None) {
            Py::Sequence seq(objects);
            for (Py::Sequence::iterator it = seq.begin(); it != seq.end(); ++it) {
                PyObject* item = (*it).ptr();
                if (!PyObject_TypeCheck(item, &App::DocumentObjectPy::Type)) {
                    PyErr_SetString(PyExc_TypeError, "objects is a list of document objects");
                    return nullptr;
                }
                only.push_back(static_cast<App::DocumentObjectPy*>(item)->getDocumentObjectPtr());
            }
        }

        if (frames != Py_None) {
            const std::size_t count = getAssemblyObjectPtr()->numberOfFrames();
            if (count == 0) {
                PyErr_SetString(PyExc_ValueError, "frames: no simulation has been run");
                return nullptr;
            }
            Py::Sequence seq(frames);
            for (Py::Sequence::iterator it = seq.begin(); it != seq.end(); ++it) {
                if (!PyLong_Check((*it).ptr())) {
                    PyErr_SetString(PyExc_TypeError, "frames is a list of frame numbers");
                    return nullptr;
                }
                const long frame = Py::Long(*it).as_long();
                if (frame < 0 || static_cast<std::size_t>(frame) >= count) {
                    PyErr_Format(PyExc_ValueError,
                                 "frame %ld: the last simulation has %zu frames",
                                 frame,
                                 count);
                    return nullptr;
                }
                options.frames.push_back(static_cast<std::size_t>(frame));
            }
        }

        Py::List ret;
        for (auto& result : Assembly::findInterferences(getAssemblyObjectPtr(), options, only)) {
            Py::Dict entry;
            entry.setItem("Object1", Py::asObject(result.first->getPyObject()));
            entry.setItem("Object2", Py::asObject(result.second->getPyObject()));
            const char* kind = "Interference";
            if (result.kind == InterferenceResult::Kind::Contact) {
                kind = "Contact";
            }
            else if (result.kind == InterferenceResult::Kind::Clearance) {
                kind = "Clearance";
            }
            entry.setItem("Kind", Py::String(kind));
            entry.setItem("Clearance", Py::Float(result.clearance));
            entry.setItem("Area", Py::Float(result.touchArea));
            for (const auto& [key, names] : {std::pair {"Faces1", &result.faceNames1},
                                             std::pair {"Faces2", &result.faceNames2}}) {
                Py::List list;
                for (const auto& name : *names) {
                    list.append(Py::String(name));
                }
                entry.setItem(key, list);
            }
            entry.setItem("Volume", Py::Float(result.volume));
            entry.setItem("Distance", Py::Float(result.distance));
            entry.setItem("Tolerance", Py::Float(result.tolerance));
            if (result.overlap.isNull()) {
                entry.setItem("Overlap", Py::None());
            }
            else {
                entry.setItem("Overlap", Py::asObject(result.overlap.getPyObject()));
            }
            if (result.frame >= 0) {
                entry.setItem("Frame", Py::Long(result.frame));
                entry.setItem("Time", Py::Float(result.time));
            }
            if (result.touched) {
                entry.setItem("Centre", Py::Vector(result.touchCentre));
                entry.setItem("Direction", Py::Vector(result.touchNormal));
            }
            else {
                entry.setItem("Centre", Py::None());
                entry.setItem("Direction", Py::None());
            }
            ret.append(entry);
        }
        return Py::new_reference_to(ret);
    }
    PY_CATCH_OCC
}

PyObject* AssemblyObjectPy::getSolverMetric(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    Py::Dict ret;
    for (const auto& [name, centre, inertia, body, mass] : getAssemblyObjectPtr()->getSolverMetric()) {
        ret.setItem(
            name,
            Py::TupleN(Py::Vector(centre), Py::Float(inertia), Py::Long(body), Py::Float(mass))
        );
    }
    return Py::new_reference_to(ret);
}

Py::List AssemblyObjectPy::getJoints() const
{
    Py::List ret;
    std::vector<App::DocumentObject*> list = getAssemblyObjectPtr()->getJoints();

    for (auto It : list) {
        ret.append(Py::Object(It->getPyObject(), true));
    }

    return ret;
}

PyObject* AssemblyObjectPy::getDownstreamParts(PyObject* args) const
{
    PyObject* pyPart;
    PyObject* pyJoint;

    // Parse the two arguments: a part object and a joint object
    if (!PyArg_ParseTuple(
            args,
            "O!O!",
            &(App::DocumentObjectPy::Type),
            &pyPart,
            &(App::DocumentObjectPy::Type),
            &pyJoint
        )) {
        return nullptr;
    }

    auto* part = static_cast<App::DocumentObjectPy*>(pyPart)->getDocumentObjectPtr();
    auto* joint = static_cast<App::DocumentObjectPy*>(pyJoint)->getDocumentObjectPtr();

    // Call the C++ method
    std::vector<Assembly::ObjRef> downstreamParts
        = this->getAssemblyObjectPtr()->getDownstreamParts(part, joint);

    // Convert the result into a Python list of DocumentObjects
    Py::List ret;
    for (const auto& objRef : downstreamParts) {
        if (objRef.obj) {
            ret.append(Py::Object(objRef.obj->getPyObject(), true));
        }
    }

    return Py::new_reference_to(ret);
}
