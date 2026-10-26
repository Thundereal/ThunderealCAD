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

#include <algorithm>
#include <sstream>

#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Plane.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <App/Datums.h>
#include <App/Document.h>
#include <App/ObjectIdentifier.h>
#include <Base/Console.h>
#include <Base/Tools.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "GeometryFacade.h"
#include "Sketch3DObject.h"
#include "Sketch3DObjectPy.h"

FC_LOG_LEVEL_INIT("Sketch3D", true, true)

using namespace Sketcher;

PROPERTY_SOURCE_WITH_EXTENSIONS(Sketcher::Sketch3DObject, Part::Feature)

namespace
{

Part::GeomPlane* makePlane(const gp_Dir& normal, const gp_Dir& xdir)
{
    return new Part::GeomPlane(new Geom_Plane(gp_Ax3(gp_Pnt(0, 0, 0), normal, xdir)));
}

Part::GeomLineSegment* makeAxis(const Base::Vector3d& dir)
{
    auto line = new Part::GeomLineSegment();
    line->setPoints(Base::Vector3d(0, 0, 0), dir);
    return line;
}

}  // namespace

Sketch3DObject::Sketch3DObject()
{
    ADD_PROPERTY_TYPE(Geometry,
                      (nullptr),
                      "Sketch3D",
                      App::Prop_None,
                      "The 3D sketch's geometry, in its frame");
    ADD_PROPERTY_TYPE(Constraints,
                      (nullptr),
                      "Sketch3D",
                      App::Prop_None,
                      "The 3D sketch's constraints");
    ADD_PROPERTY_TYPE(ExternalGeometry,
                      (nullptr, nullptr),
                      "Sketch3D",
                      App::Prop_None,
                      "Model vertices, edges, planar faces and datums the sketch refers to");
    ADD_PROPERTY_TYPE(ExternalGeo,
                      (nullptr),
                      "Sketch3D",
                      App::PropertyType(App::Prop_Hidden | App::Prop_ReadOnly),
                      "The external references as geometry in the sketch's frame");
    AttachExtension::initExtension(this);

    frameGeo.emplace_back(makeAxis(Base::Vector3d(1, 0, 0)));
    frameGeo.emplace_back(makeAxis(Base::Vector3d(0, 1, 0)));
    frameGeo.emplace_back(makeAxis(Base::Vector3d(0, 0, 1)));
    frameGeo.emplace_back(makePlane(gp::DZ(), gp::DX()));
    frameGeo.emplace_back(makePlane(gp::DX(), gp::DY()));
    frameGeo.emplace_back(makePlane(gp::DY(), gp::DZ()));

    Constraints.signalConstraintsRenamed.connect(
        [this](const std::map<App::ObjectIdentifier, App::ObjectIdentifier>& renamed) {
            ExpressionEngine.renameExpressions(renamed);
            for (auto doc : App::GetApplication().getDocuments()) {
                doc->renameObjectIdentifiers(renamed);
            }
        });
    Constraints.signalConstraintsRemoved.connect(
        [this](const std::set<App::ObjectIdentifier>& removed) {
            for (const auto& path : removed) {
                ExpressionEngine.setValue(path, std::shared_ptr<App::Expression>());
            }
        });
}

Sketch3DObject::~Sketch3DObject() = default;

PyObject* Sketch3DObject::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        PythonObject = Py::Object(new Sketch3DObjectPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

std::vector<const Part::Geometry*> Sketch3DObject::getExternalGeometry() const
{
    std::vector<const Part::Geometry*> res;
    for (const auto& g : frameGeo) {
        res.push_back(g.get());
    }
    for (auto g : ExternalGeo.getValues()) {
        res.push_back(g);
    }
    return res;
}

std::vector<const Part::Geometry*> Sketch3DObject::getCompleteGeometry() const
{
    std::vector<const Part::Geometry*> res(Geometry.getValues().begin(),
                                           Geometry.getValues().end());
    auto ext = getExternalGeometry();
    res.insert(res.end(), ext.rbegin(), ext.rend());
    return res;
}

const Part::Geometry* Sketch3DObject::getGeometry(int geoId) const
{
    if (geoId >= 0) {
        return geoId < Geometry.getSize() ? Geometry[geoId] : nullptr;
    }
    auto ext = getExternalGeometry();
    std::size_t i = static_cast<std::size_t>(-geoId - 1);
    return i < ext.size() ? ext[i] : nullptr;
}

void Sketch3DObject::acceptGeometry()
{
    Constraints.acceptGeometry(getCompleteGeometry());
}

long Sketch3DObject::nextGeometryId()
{
    long id = 0;
    for (auto geo : Geometry.getValues()) {
        if (geo->getExtensionPtr(SketchGeometryExtension::getClassTypeId())) {
            id = std::max<long>(id, GeometryFacade::getId(geo));
        }
    }
    return id + 1;
}

int Sketch3DObject::addGeometry(const Part::Geometry* geo, bool construction)
{
    auto copy = geo->clone();
    GeometryFacade::setId(copy, static_cast<int>(nextGeometryId()));
    GeometryFacade::setConstruction(copy, construction || copy->isDerivedFrom<Part::GeomPlane>());
    std::vector<const Part::Geometry*> vals(Geometry.getValues().begin(),
                                            Geometry.getValues().end());
    vals.push_back(copy);
    Geometry.setValues(std::move(vals));
    solve();
    return Geometry.getSize() - 1;
}

void Sketch3DObject::delGeometry(int geoId)
{
    if (geoId < 0 || geoId >= Geometry.getSize()) {
        FC_THROWM(Base::IndexError, "No geometry " << geoId);
    }
    std::vector<const Part::Geometry*> vals(Geometry.getValues().begin(),
                                            Geometry.getValues().end());
    vals.erase(vals.begin() + geoId);

    // Constraints on it go; ids above it move down.
    std::vector<Constraint*> cons;
    for (auto c : Constraints.getValues()) {
        if (c->First == geoId || c->Second == geoId || c->Third == geoId) {
            continue;
        }
        auto copy = c->clone();
        for (int* id : {&copy->First, &copy->Second, &copy->Third}) {
            if (*id > geoId) {
                --*id;
            }
        }
        cons.push_back(copy);
    }
    Geometry.setValues(std::move(vals));
    Constraints.setValues(std::move(cons));
    solve();
}

int Sketch3DObject::addConstraint(const Constraint* constraint)
{
    std::vector<Constraint*> vals(Constraints.getValues());
    vals.push_back(constraint->clone());
    Constraints.setValues(std::move(vals));
    solve();
    return Constraints.getSize() - 1;
}

void Sketch3DObject::delConstraint(int index)
{
    if (index < 0 || index >= Constraints.getSize()) {
        FC_THROWM(Base::IndexError, "No constraint " << index);
    }
    std::vector<Constraint*> vals(Constraints.getValues());
    vals.erase(vals.begin() + index);
    Constraints.setValues(std::move(vals));
    solve();
}

int Sketch3DObject::getConstraintIndex(const std::string& name) const
{
    const auto& vals = Constraints.getValues();
    for (std::size_t i = 0; i < vals.size(); ++i) {
        if (PropertyConstraintList::getConstraintName(vals[i]->Name, static_cast<int>(i)) == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Sketch3DObject::setDatum(int index, double value)
{
    if (index < 0 || index >= Constraints.getSize()) {
        FC_THROWM(Base::IndexError, "No constraint " << index);
    }
    const Constraint* c = Constraints[index];
    if (!c->isDimensional()) {
        FC_THROWM(Base::TypeError, "Constraint " << index << " has no value");
    }
    if (!c->isDriving) {
        FC_THROWM(Base::ValueError,
                  "Constraint " << index << " is a reference; it has no value to set");
    }
    std::unique_ptr<Constraint> copy(c->clone());
    double old = copy->getValue();
    copy->setValue(value);
    Constraints.set1Value(index, copy.get());
    if (solve() != 0) {
        copy->setValue(old);
        Constraints.set1Value(index, copy.get());
        solve();
        FC_THROWM(Base::ValueError, "The sketch does not solve with that value; it is unchanged");
    }
}

double Sketch3DObject::getDatum(int index) const
{
    if (index < 0 || index >= Constraints.getSize()) {
        FC_THROWM(Base::IndexError, "No constraint " << index);
    }
    const Constraint* c = Constraints[index];
    if (!c->isDimensional()) {
        FC_THROWM(Base::TypeError, "Constraint " << index << " has no value");
    }
    return c->getValue();
}

void Sketch3DObject::setDriving(int index, bool driving)
{
    if (index < 0 || index >= Constraints.getSize()) {
        FC_THROWM(Base::IndexError, "No constraint " << index);
    }
    std::unique_ptr<Constraint> copy(Constraints[index]->clone());
    if (!copy->isDimensional()) {
        FC_THROWM(Base::TypeError, "Constraint " << index << " has no value");
    }
    copy->isDriving = driving;
    Constraints.set1Value(index, copy.get());
    solve();
}

void Sketch3DObject::setConstruction(int geoId, bool construction)
{
    if (geoId < 0 || geoId >= Geometry.getSize()) {
        FC_THROWM(Base::IndexError, "No geometry " << geoId);
    }
    std::unique_ptr<Part::Geometry> copy(Geometry[geoId]->clone());
    GeometryFacade::setConstruction(copy.get(), construction);
    Geometry.set1Value(geoId, std::move(copy));
    buildShape();
}

int Sketch3DObject::addExternal(App::DocumentObject* obj, const char* subname)
{
    if (!obj || !obj->isAttachedToDocument() || obj->getDocument() != getDocument()) {
        FC_THROWM(Base::ValueError, "The reference must be an object of the same document");
    }
    auto objs = ExternalGeometry.getValues();
    auto subs = ExternalGeometry.getSubValues();
    objs.push_back(obj);
    subs.emplace_back(subname ? subname : "");
    ExternalGeometry.setValues(objs, subs);
    rebuildExternalGeometry();
    if (ExternalGeo.getSize() < static_cast<int>(subs.size())) {
        objs.pop_back();
        subs.pop_back();
        ExternalGeometry.setValues(objs, subs);
        rebuildExternalGeometry();
        FC_THROWM(Base::ValueError,
                  "Cannot use " << obj->getFullName() << "." << (subname ? subname : "")
                                << " as a reference");
    }
    solve();
    return RefExt - (ExternalGeo.getSize() - 1);
}

void Sketch3DObject::delExternal(int geoId)
{
    int i = RefExt - geoId;
    if (geoId > RefExt || i >= ExternalGeometry.getSize()) {
        FC_THROWM(Base::IndexError, "No external reference " << geoId);
    }
    auto objs = ExternalGeometry.getValues();
    auto subs = ExternalGeometry.getSubValues();
    objs.erase(objs.begin() + i);
    subs.erase(subs.begin() + i);

    std::vector<Constraint*> cons;
    for (auto c : Constraints.getValues()) {
        if (c->First == geoId || c->Second == geoId || c->Third == geoId) {
            continue;
        }
        auto copy = c->clone();
        for (int* id : {&copy->First, &copy->Second, &copy->Third}) {
            if (*id != GeoEnum::GeoUndef && *id < geoId) {
                ++*id;
            }
        }
        cons.push_back(copy);
    }
    ExternalGeometry.setValues(objs, subs);
    rebuildExternalGeometry();
    Constraints.setValues(std::move(cons));
    solve();
}

void Sketch3DObject::rebuildExternalGeometry()
{
    // Taken as they are, in the sketch's frame -- no projection, unlike the 2D sketch.
    Base::Placement inv = Placement.getValue().inverse();
    Base::Matrix4D invMat = inv.toMatrix();
    std::vector<const Part::Geometry*> geos;
    const auto& objs = ExternalGeometry.getValues();
    const auto& subs = ExternalGeometry.getSubValues();
    for (std::size_t i = 0; i < objs.size(); ++i) {
        auto obj = objs[i];
        const std::string& sub = i < subs.size() ? subs[i] : std::string();
        std::unique_ptr<Part::Geometry> geo;
        try {
            if (auto plane = freecad_cast<App::Plane*>(obj)) {
                Base::Placement plm = inv * plane->Placement.getValue();
                Base::Vector3d o = plm.getPosition(), n(0, 0, 1), x(1, 0, 0);
                plm.getRotation().multVec(n, n);
                plm.getRotation().multVec(x, x);
                geo = std::make_unique<Part::GeomPlane>(new Geom_Plane(
                    gp_Ax3(gp_Pnt(o.x, o.y, o.z), gp_Dir(n.x, n.y, n.z), gp_Dir(x.x, x.y, x.z))));
            }
            else if (auto point = freecad_cast<App::Point*>(obj)) {
                Base::Vector3d p;
                inv.multVec(point->getBasePoint(), p);
                geo = std::make_unique<Part::GeomPoint>(p);
            }
            else {
                Part::TopoShape shape = Part::Feature::getTopoShape(obj, sub.c_str(), true);
                if (shape.isNull()) {
                    FC_WARN("Null shape for reference " << obj->getFullName() << "." << sub);
                    continue;
                }
                shape.transformShape(invMat, false, true);
                const TopoDS_Shape& s = shape.getShape();
                if (s.ShapeType() == TopAbs_VERTEX) {
                    gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(s));
                    geo = std::make_unique<Part::GeomPoint>(Base::Vector3d(p.X(), p.Y(), p.Z()));
                }
                else if (s.ShapeType() == TopAbs_EDGE) {
                    geo = Part::Geometry::fromShape(s, true);
                }
                else if (s.ShapeType() == TopAbs_FACE) {
                    BRepAdaptor_Surface surf(TopoDS::Face(s));
                    if (surf.GetType() == GeomAbs_Plane) {
                        geo = std::make_unique<Part::GeomPlane>(surf.Plane());
                    }
                }
            }
        }
        catch (Base::Exception& e) {
            FC_WARN("Reference " << obj->getFullName() << "." << sub << ": " << e.what());
        }
        if (!geo) {
            FC_WARN("Unsupported reference " << obj->getFullName() << "." << sub);
            continue;
        }
        GeometryFacade::setConstruction(geo.get(), true);
        geos.push_back(geo.release());
    }
    ExternalGeo.setValues(std::move(geos));
    acceptGeometry();
}

int Sketch3DObject::solve(bool updateGeometry)
{
    Base::StateLocker lock(solving, true);
    lastDoF = solver.setUp(std::vector<const Part::Geometry*>(Geometry.getValues().begin(),
                                                              Geometry.getValues().end()),
                           getExternalGeometry(),
                           Constraints.getValues());
    lastConflicting = solver.getConflicting();
    lastRedundant = solver.getRedundant();
    lastPartiallyRedundant = solver.getPartiallyRedundant();
    lastMalformed = solver.getMalformed();

    int res = 0;
    if (!lastMalformed.empty()) {
        res = -5;
    }
    else if (!lastConflicting.empty()) {
        res = -3;
    }
    else if (solver.solve() != 0) {
        res = -1;
    }
    else if (!lastRedundant.empty()) {
        res = -4;
    }
    if ((res == 0 || res == -4) && updateGeometry && !applySolution()) {
        res = -1;
    }
    return res;
}

int Sketch3DObject::movePoint(int geoId, PointPos pos, const Base::Vector3d& to, bool relative)
{
    Base::StateLocker lock(solving, true);
    solver.setUp(std::vector<const Part::Geometry*>(Geometry.getValues().begin(),
                                                    Geometry.getValues().end()),
                 getExternalGeometry(),
                 Constraints.getValues());
    if (!solver.getConflicting().empty() || solver.movePoint(geoId, pos, to, relative) != 0) {
        return -1;
    }
    return applySolution() ? 0 : -1;
}

bool Sketch3DObject::applySolution()
{
    // A solution the geometry cannot take (a line collapsed to a point) is no
    // solution: the sketch stays as it was, as the 2D sketch undoes an invalid one.
    std::vector<std::unique_ptr<Part::Geometry>> solved;
    try {
        solved = solver.extractGeometry();
    }
    catch (const Base::Exception& e) {
        FC_WARN(getFullName() << ": invalid solution, not applied: " << e.what());
        return false;
    }
    std::vector<const Part::Geometry*> vals;
    for (auto& g : solved) {
        vals.push_back(g.release());
    }
    Geometry.setValues(std::move(vals));
    updateDrivenValues();
    buildShape();
    return true;
}

void Sketch3DObject::updateDrivenValues()
{
    const auto& vals = Constraints.getValues();
    std::vector<Constraint*> copy;
    bool changed = false;
    for (std::size_t i = 0; i < vals.size(); ++i) {
        double v;
        if (!vals[i]->isDriving && solver.getDrivenValue(static_cast<int>(i), v)
            && vals[i]->getValue() != v) {
            if (copy.empty()) {
                copy = vals;
            }
            copy[i] = vals[i]->clone();
            copy[i]->setValue(v);
            changed = true;
        }
    }
    if (changed) {
        Constraints.setValues(std::move(copy));
    }
}

void Sketch3DObject::buildShape()
{
    // Each edge is named from its geometry id, as the 2D sketch names its edges, so a
    // Pipe that follows Edge3 keeps following it through a solve or a reorder.
    std::vector<Part::TopoShape> edges, vertices;
    for (auto geo : Geometry.getValues()) {
        if (GeometryFacade::getConstruction(geo) || geo->isDerivedFrom<Part::GeomPlane>()) {
            continue;
        }
        std::ostringstream ss;
        ss << 'g' << GeometryFacade::getId(geo);
        std::string name = ss.str();
        Part::TopoShape shape(geo->toShape());
        if (geo->isDerivedFrom<Part::GeomPoint>()) {
            shape.setElementName(Data::IndexedName::fromConst("Vertex", 1),
                                 Data::MappedName::fromRawData(name.c_str()));
            vertices.push_back(shape);
            vertices.back().copyElementMap(shape, Part::OpCodes::Sketch);
            continue;
        }
        shape.setElementName(Data::IndexedName::fromConst("Edge", 1),
                             Data::MappedName::fromRawData(name.c_str()));
        auto curve = freecad_cast<const Part::GeomBoundedCurve*>(geo);
        int nvertex = curve ? shape.countSubShapes(TopAbs_VERTEX) : 0;
        for (int i = 1; i <= nvertex; ++i) {
            gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(shape.getSubShape(TopAbs_VERTEX, i)));
            Base::Vector3d pt(p.X(), p.Y(), p.Z());
            const char* end = nullptr;
            if (Base::DistanceP2(pt, curve->getStartPoint()) < 1e-14) {
                end = "v1";
            }
            else if (Base::DistanceP2(pt, curve->getEndPoint()) < 1e-14) {
                end = "v2";
            }
            if (end) {
                shape.setElementName(Data::IndexedName::fromConst("Vertex", i),
                                     Data::MappedName::fromRawData((name + end).c_str()));
            }
        }
        edges.push_back(shape);
    }
    if (edges.empty() && vertices.empty()) {
        Shape.setValue(Part::TopoShape());
        return;
    }
    Part::TopoShape result(0, getDocument()->getStringHasher());
    if (vertices.empty()) {
        result.makEWires(edges, Part::OpCodes::Sketch);
    }
    else {
        std::vector<Part::TopoShape> parts;
        if (!edges.empty()) {
            auto wires = Part::TopoShape().makEWires(edges, Part::OpCodes::Sketch);
            for (const auto& wire : wires.getSubTopoShapes(TopAbs_WIRE)) {
                parts.push_back(wire);
            }
        }
        parts.insert(parts.end(), vertices.begin(), vertices.end());
        result.makECompound(parts);
    }
    result.Tag = getID();
    Shape.setValue(result);
}

App::DocumentObjectExecReturn* Sketch3DObject::execute()
{
    try {
        auto ret = Part::Feature::execute();  // the attachment
        if (ret != App::DocumentObject::StdReturn) {
            return ret;
        }
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    rebuildExternalGeometry();
    std::ostringstream ss;
    auto list = [&](const char* what, const std::vector<int>& v) {
        ss << what;
        for (int i : v) {
            ss << ' ' << i;
        }
    };
    switch (solve(true)) {
        case 0:
            break;
        case -4:
            list("Redundant constraints:", lastRedundant);
            FC_WARN(getFullName() << ": " << ss.str());
            break;
        case -3:
            list("Conflicting constraints:", lastConflicting);
            return new App::DocumentObjectExecReturn(ss.str(), this);
        case -5:
            list("Constraints a 3D sketch cannot solve:", lastMalformed);
            return new App::DocumentObjectExecReturn(ss.str(), this);
        default:
            return new App::DocumentObjectExecReturn("The 3D sketch does not solve", this);
    }
    return App::DocumentObject::StdReturn;
}

short Sketch3DObject::mustExecute() const
{
    if (Geometry.isTouched() || Constraints.isTouched() || ExternalGeometry.isTouched()) {
        return 1;
    }
    return Part::Feature::mustExecute();
}

void Sketch3DObject::onChanged(const App::Property* prop)
{
    if (prop == &Geometry && !isRestoring() && !solving) {
        acceptGeometry();
    }
    Part::Feature::onChanged(prop);
}

void Sketch3DObject::onDocumentRestored()
{
    Part::Feature::onDocumentRestored();
    acceptGeometry();
    // The status (DoF, conflicts) without moving anything.
    solve(false);
}
