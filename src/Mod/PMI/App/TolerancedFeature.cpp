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

#include <algorithm>
#include <cmath>
#include <set>

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <App/ElementNamingUtils.h>
#include <App/IndexedName.h>
#include <App/MappedElement.h>
#include <Base/Console.h>
#include <Mod/Part/App/BodyBase.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>

#include "Geometry.h"
#include "TolerancedFeature.h"

FC_LOG_LEVEL_INIT("PMI", true, true)

using namespace PMI;

const char* TolerancedFeature::KindEnums[] = {"Integral",
                                              "Axis",
                                              "CentrePlane",
                                              "CentrePoint",
                                              "Apex",
                                              "Group",
                                              "AllAround",
                                              "Between",
                                              "WholePart",
                                              nullptr};

PROPERTY_SOURCE(PMI::TolerancedFeature, PMI::Annotation)

TolerancedFeature::TolerancedFeature()
{
    ADD_PROPERTY_TYPE(References, (nullptr), "PMI", App::Prop_None,
                      "The faces of the part this feature is");
    // Refers, as the set's Part does: claims nothing
    References.setReferOnly(true);
    ADD_PROPERTY_TYPE(Kind, (0L), "PMI", App::Prop_None,
                      "Integral: the faces; Axis, CentrePlane, CentrePoint, Apex: the exact "
                      "element derived from them; Group: several faces as one; AllAround, "
                      "Between: a restricted feature; WholePart: the part, all its faces, "
                      "none named");
    Kind.setEnums(KindEnums);
    // Shape, IsSize and Condition are not outputs: a dimension or a datum on
    // this feature is recomputed only when one of them changes (with
    // OptimizeRecompute a link sees an object's non-output revision only,
    // docs/InputProperties.md sec 2)
    ADD_PROPERTY_TYPE(Shape, (TopoDS_Shape()), "PMI",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Transient),
                      "The faces, or the element derived from them");
    ADD_PROPERTY_TYPE(SavedGeometry, (TopoDS_Shape()), "PMI",
                      App::PropertyType(App::Prop_Output | App::Prop_ReadOnly | App::Prop_Hidden),
                      "The faces as last resolved");
    ADD_PROPERTY_TYPE(SavedNames, (), "PMI",
                      App::PropertyType(App::Prop_Output | App::Prop_ReadOnly | App::Prop_Hidden),
                      "The faces' mapped names as last resolved");
    ADD_PROPERTY_TYPE(IsSize, (false), "PMI", App::Prop_ReadOnly,
                      "Whether it is a feature of size: a cylinder, a sphere, two opposed planes");
}

namespace
{
std::string mappedNameOf(const App::ShadowSub& shadow)
{
    std::string name = Data::newElementName(shadow.first.c_str());
    const char* mapped = Data::isMappedElement(name.c_str());
    return mapped ? std::string(mapped) : std::string();
}

std::string stripMissing(const std::string& sub)
{
    const std::string& prefix = Data::missingPrefix();
    if (sub.compare(0, prefix.size(), prefix) == 0) {
        return sub.substr(prefix.size());
    }
    return sub;
}

/// The type of element a reference names: "Face", "Edge" or "Vertex"
std::string typeOf(const std::string& sub)
{
    Data::IndexedName name(sub.c_str());
    return name ? std::string(name.getType()) : std::string();
}

/// The face, edge or vertex \a sub names on \a obj
Part::TopoShape elementOf(App::DocumentObject* obj, const std::string& sub)
{
    if (!obj || sub.empty() || Data::hasMissingElement(sub.c_str())) {
        return Part::TopoShape();
    }
    try {
        auto shape = Part::Feature::getTopoShape(obj, sub.c_str(), true);
        if (!shape.isNull()) {
            auto t = shape.getShape().ShapeType();
            if (t == TopAbs_FACE || t == TopAbs_EDGE || t == TopAbs_VERTEX) {
                return shape;
            }
        }
    }
    catch (const Base::Exception&) {
    }
    catch (const Standard_Failure&) {
    }
    return Part::TopoShape();
}
/// A solid rolled back -- its tip not its last feature -- is being edited at
/// an earlier step: its faces are not the part's, and nothing is resolved
/// against them (docs/GDandT.md sec 7)
bool rolledBack(const App::DocumentObject* obj)
{
    if (!obj || obj->isDerivedFrom<Part::BodyBase>()) {
        return false;
    }
    auto tip = freecad_cast<App::PropertyLink*>(obj->getPropertyByName("Tip"));
    auto group = freecad_cast<App::PropertyLinkList*>(obj->getPropertyByName("Group"));
    if (!tip || !group || !tip->getValue() || group->getValues().empty()) {
        return false;
    }
    return tip->getValue() != group->getValues().back();
}

/// Whether \a faces are a feature of size (ASME Y14.5 1.3.32), or a pattern
/// of them; \a main the face that stands for it
bool ofSize(const std::vector<Primitive>& faces, std::size_t& main)
{
    main = 0;
    if (faces.empty()) {
        return false;
    }
    const Primitive& first = faces.front();
    if (first.type == Primitive::Cylinder || first.type == Primitive::Sphere) {
        // One surface, or a pattern of them: holes of one size on several
        // axes ("4X"), each a feature of size
        if (std::all_of(faces.begin(), faces.end(), [&](const Primitive& p) {
                return p.type == first.type && std::fabs(p.radius - first.radius) < 1e-6;
            })) {
            return true;
        }
    }
    // A width, a pattern of widths, a slot
    double width = 0;
    if (Geom::width(faces, width)) {
        return true;
    }
    // Holes with bores and sinks, standing for them the smallest
    std::size_t hole = 0;
    if (Geom::holes(faces, hole)) {
        main = hole;
        return true;
    }
    return false;
}
/// What each reference was last resolved to, by its index, in one shape: a
/// reference resolved to nothing is an empty compound in its place, so the
/// ones after it keep theirs (a compound of the shapes alone shifted them)
TopoDS_Shape savedAll(const std::vector<TopoDS_Shape>& shapes)
{
    bool any = false;
    for (auto& s : shapes) {
        any = any || !s.IsNull();
    }
    if (!any) {
        return TopoDS_Shape();
    }
    if (shapes.size() == 1) {
        return shapes.front();
    }
    BRep_Builder builder;
    TopoDS_Compound comp;
    builder.MakeCompound(comp);
    for (auto& s : shapes) {
        if (s.IsNull()) {
            TopoDS_Compound none;
            builder.MakeCompound(none);
            builder.Add(comp, none);
        }
        else {
            builder.Add(comp, s);
        }
    }
    return comp;
}

/// savedAll() read back: one per reference, null for one resolved to nothing
std::vector<TopoDS_Shape> savedEach(const TopoDS_Shape& shape)
{
    std::vector<TopoDS_Shape> res;
    for (auto& s : Geom::members(shape)) {
        res.push_back(s.ShapeType() == TopAbs_COMPOUND && !TopoDS_Iterator(s).More()
                          ? TopoDS_Shape()
                          : s);
    }
    return res;
}

/// The ones that are something
std::vector<TopoDS_Shape> present(std::vector<TopoDS_Shape> shapes)
{
    shapes.erase(std::remove_if(shapes.begin(),
                                shapes.end(),
                                [](const TopoDS_Shape& s) { return s.IsNull(); }),
                 shapes.end());
    return shapes;
}
}  // namespace

App::DocumentObjectExecReturn* TolerancedFeature::execute()
{
    for (auto obj : References.getValues()) {
        if (rolledBack(obj)) {
            FC_LOG(getFullName() << ": " << obj->getFullName()
                                 << " is rolled back; kept as it was");
            return App::DocumentObject::StdReturn;
        }
    }

    const std::vector<App::DocumentObject*> objs = References.getValues();
    if (getKind() == WholePart) {
        // The part as it is: no face is named, so none is lost or remapped
        std::string why;
        std::vector<TopoDS_Shape> faces = wholePart();
        derive(faces, why);
        SavedGeometry.setValue(TopoDS_Shape());
        if (!SavedNames.getValues().empty()) {
            SavedNames.setValues(std::vector<std::string>());
        }
        if (objs.empty() || !objs.front()) {
            setState(Broken, "no part referred to");
        }
        else if (faces.empty()) {
            setState(Invalid, "the part has no face");
        }
        else {
            setState(Ok);
        }
        return App::DocumentObject::StdReturn;
    }
    const std::vector<std::string> subs = References.getSubValues(false);
    const std::vector<App::ShadowSub> shadows = References.getShadowSubs();
    const std::vector<std::string> savedNames = SavedNames.getValues();
    const std::vector<TopoDS_Shape> savedFaces = savedEach(SavedGeometry.getShape().getShape());

    std::vector<App::DocumentObject*> newObjs;
    std::vector<std::string> newSubs;
    std::vector<TopoDS_Shape> faces;
    std::vector<std::string> names;
    std::vector<std::string> remapped;
    std::vector<std::string> broken;
    bool changed = false;

    for (std::size_t i = 0; i < objs.size(); ++i) {
        App::DocumentObject* obj = objs[i];
        const std::string sub = i < subs.size() ? subs[i] : std::string();
        std::string mapped = i < shadows.size() ? mappedNameOf(shadows[i]) : std::string();
        if (mapped.empty() && i < savedNames.size()) {
            mapped = savedNames[i];
        }
        const TopoDS_Shape saved = i < savedFaces.size() ? savedFaces[i] : TopoDS_Shape();
        const std::string label = stripMissing(sub);
        // The kind of element it is: a face is found again as a face, an
        // edge as an edge
        std::string type = typeOf(label);
        if (type.empty() && !saved.IsNull()) {
            type = saved.ShapeType() == TopAbs_EDGE ? "Edge"
                : saved.ShapeType() == TopAbs_VERTEX ? "Vertex" : "Face";
        }
        if (type.empty()) {
            type = "Face";
        }

        auto keep = [&](const std::string& s, const Part::TopoShape& face) {
            newObjs.push_back(obj);
            newSubs.push_back(s);
            faces.push_back(face.getShape());
            std::string name;
            try {
                auto whole = Part::Feature::getTopoShape(obj);
                name = whole.getMappedName(Data::IndexedName(s.c_str())).toString();
            }
            catch (...) {
            }
            names.push_back(name);
        };

        // 1. By name: the element map has kept it
        Part::TopoShape face = elementOf(obj, sub);
        if (!face.isNull()) {
            keep(sub, face);
            // The link resolves a missing name by geometry itself
            // (searchElementCache): a name that changed to one its history
            // does not lead to is a face found by geometry, to look at
            const std::string& now = names.back();
            std::string before = i < savedNames.size() ? savedNames[i] : std::string();
            if (!before.empty() && !now.empty() && now != before) {
                bool related = false;
                for (auto& rel : Part::Feature::getRelatedElements(obj, before.c_str())) {
                    if (rel.index.toString() == sub) {
                        related = true;
                        break;
                    }
                }
                if (!related) {
                    remapped.push_back(label + " (by geometry)");
                }
            }
            continue;
        }

        // 2. By history: the elements made from the one referred to
        std::vector<std::string> found;
        if (obj && !mapped.empty()) {
            for (auto& rel : Part::Feature::getRelatedElements(obj, mapped.c_str())) {
                std::string s = rel.index.toString();
                if (typeOf(s) == type) {
                    found.push_back(s);
                }
            }
        }
        const char* how = "history";

        // 3. By geometry: an element of the part on the same surface,
        // curve or point
        if (found.empty() && obj && !saved.IsNull()) {
            Primitive want = Geom::ofElement(saved);
            if (want.type != Primitive::None) {
                auto whole = Part::Feature::getTopoShape(obj);
                auto all = Geom::elements(whole.getShape(), type.c_str());
                for (std::size_t j = 0; j < all.size(); ++j) {
                    if (Geom::sameSurface(want, Geom::ofElement(all[j]))) {
                        found.push_back(type + std::to_string(j + 1));
                    }
                }
            }
            how = "geometry";
        }

        std::vector<std::string> took;
        for (auto& s : found) {
            Part::TopoShape f = elementOf(obj, s);
            if (!f.isNull()) {
                keep(s, f);
                took.push_back(s);
            }
        }
        if (!took.empty()) {
            changed = true;
            std::string msg = label + " -> ";
            for (std::size_t k = 0; k < took.size(); ++k) {
                msg += (k ? "+" : "") + took[k];
            }
            remapped.push_back(msg + " (by " + how + ")");
            continue;
        }

        // 4. Gone: kept, as it was, until reattached
        newObjs.push_back(obj);
        newSubs.push_back(sub);
        faces.push_back(saved);
        names.push_back(mapped);
        broken.push_back(label);
    }

    if (changed) {
        // Into the link, so the next recompute finds them by name; the
        // broken ones keep what they had
        References.setValues(newObjs, newSubs);
    }

    std::string why;
    bool derived = derive(present(faces), why);
    // What it was last resolved to, kept for a broken reference
    SavedGeometry.setValue(savedAll(faces));
    if (SavedNames.getValues() != names) {
        SavedNames.setValues(names);
    }

    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (auto& x : v) {
            s += (s.empty() ? "" : ", ") + x;
        }
        return s;
    };
    if (objs.empty()) {
        setState(Broken, "no faces, edges or points referred to");
    }
    else if (!broken.empty()) {
        setState(Broken, "gone: " + join(broken));
    }
    else if (!derived) {
        setState(Invalid, why);
    }
    else if (!remapped.empty()) {
        setState(Remapped, join(remapped));
    }
    else if (getState() != Remapped) {
        // A remapped feature stays so until the user says it is right
        setState(Ok);
    }
    return App::DocumentObject::StdReturn;
}

std::vector<TopoDS_Shape> TolerancedFeature::wholePart() const
{
    std::vector<TopoDS_Shape> faces;
    const auto& objs = References.getValues();
    if (objs.empty() || !objs.front()) {
        return faces;
    }
    try {
        TopoDS_Shape whole = Part::Feature::getTopoShape(objs.front()).getShape();
        if (!whole.IsNull()) {
            for (TopExp_Explorer it(whole, TopAbs_FACE); it.More(); it.Next()) {
                faces.push_back(it.Current());
            }
        }
    }
    catch (const Base::Exception&) {
    }
    catch (const Standard_Failure&) {
    }
    return faces;
}

const std::vector<Primitive>& TolerancedFeature::borePrimitives() const
{
    if (_boresFound) {
        return _bores;
    }
    _boresFound = true;
    _bores.clear();
    // From one copy of the part: each getTopoShape() is a copy of its own,
    // its faces none of another's
    const auto& objs = References.getValues();
    const auto& subs = References.getSubValues();
    if (objs.empty() || !objs.front()) {
        return _bores;
    }
    try {
        auto whole = Part::Feature::getTopoShape(objs.front());
        std::vector<TopoDS_Shape> floors;
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            if (objs[i] != objs.front() || Data::hasMissingElement(subs[i].c_str())) {
                continue;
            }
            TopoDS_Shape face = whole.getSubShape(subs[i].c_str(), true);
            if (!face.IsNull()) {
                floors.push_back(face);
            }
        }
        _bores = Geom::bores(whole.getShape(), floors);
    }
    catch (const Base::Exception&) {
    }
    catch (const Standard_Failure&) {
    }
    return _bores;
}

bool TolerancedFeature::derive(const std::vector<TopoDS_Shape>& faces, std::string& why)
{
    _boresFound = false;
    _faces.clear();
    for (auto& f : faces) {
        _faces.push_back(Geom::ofElement(f));
    }
    _primitive = Primitive();
    std::size_t main = 0;
    TopoDS_Shape shape = Geom::compound(faces);
    bool size = false;
    bool ok = true;

    if (!_faces.empty()) {
        size = ofSize(_faces, main);
        _primitive = _faces[main];
    }

    switch (getKind()) {
        case Axis: {
            // A counterbored hole's: its hole's
            if (_faces.empty() || (_faces[main].type != Primitive::Cylinder
                                   && _faces[main].type != Primitive::Cone
                                   && _faces[main].type != Primitive::Circle)) {
                why = "an axis needs a cylindrical or conical face, or a circular edge";
                ok = false;
                break;
            }
            _primitive = _faces[main].reference();
            {
                TopoDS_Shape edge =
                    Geom::axisEdge(_primitive.loc, _primitive.dir, Geom::compound(faces));
                if (!edge.IsNull()) {
                    shape = edge;
                }
            }
            break;
        }
        case CentrePlane: {
            if (_faces.size() != 2 || _faces[0].type != Primitive::Plane
                || _faces[1].type != Primitive::Plane
                || !_faces[0].dir.IsParallel(_faces[1].dir, 1e-9)) {
                why = "a centre plane needs two parallel planar faces";
                ok = false;
                break;
            }
            double s = gp_Vec(_faces[0].loc, _faces[1].loc).Dot(gp_Vec(_faces[0].dir));
            gp_Trsf move;
            move.SetTranslation(gp_Vec(_faces[0].dir) * (s / 2));
            shape = BRepBuilderAPI_Transform(faces[0], move, true).Shape();
            _primitive = _faces[0];
            _primitive.loc = _faces[0].loc.Translated(gp_Vec(_faces[0].dir) * (s / 2));
            break;
        }
        case CentrePoint:
        case Apex: {
            Primitive::Type need = getKind() == Apex ? Primitive::Cone : Primitive::Sphere;
            if (_faces.empty()
                || (_faces[0].type != need
                    && !(getKind() == CentrePoint && _faces[0].type == Primitive::Circle))) {
                why = getKind() == Apex
                    ? "an apex needs a conical face"
                    : "a centre point needs a spherical face or a circular edge";
                ok = false;
                break;
            }
            _primitive.type = Primitive::Point;
            _primitive.loc = _faces[0].loc;
            shape = BRepBuilderAPI_MakeVertex(_faces[0].loc).Shape();
            break;
        }
        default:
            break;
    }

    Shape.setValue(shape);
    if (IsSize.getValue() != size) {
        IsSize.setValue(size);
    }
    return ok;
}

void TolerancedFeature::onDocumentRestored()
{
    Annotation::onDocumentRestored();
    // Shape is not saved: made again from what was saved, so the check and
    // the dimensions have their geometry before the first recompute
    std::string why;
    auto saved = getKind() == WholePart ? wholePart()
                                        : present(savedEach(SavedGeometry.getShape().getShape()));
    bool touched = isTouched();
    derive(saved, why);
    if (!touched) {
        purgeTouched();
    }
}

bool TolerancedFeature::refersTo(const App::DocumentObject* part,
                                 const std::vector<std::string>& subs) const
{
    std::set<std::string> want;
    for (auto& s : subs) {
        want.insert(Data::oldElementName(s.c_str()));
    }
    std::set<std::string> have;
    const auto& objs = References.getValues();
    auto mine = References.getSubValues(false);
    if (getKind() == WholePart) {
        return want.empty() && !objs.empty() && objs.front() == part;
    }
    for (std::size_t i = 0; i < objs.size() && i < mine.size(); ++i) {
        if (objs[i] != part) {
            return false;
        }
        have.insert(mine[i]);
    }
    return !want.empty() && want == have;
}
