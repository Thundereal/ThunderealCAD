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

#include <cmath>
#include <algorithm>
#include <set>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <App/Document.h>
#include <App/GeoFeatureGroupExtension.h>
#include <App/GroupExtension.h>
#include <App/ElementNamingUtils.h>
#include <Mod/Part/App/BodyBase.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>

#include "AnnotationSet.h"
#include "Api.h"
#include "Callout.h"
#include "Dimension.h"
#include "Datum.h"
#include "GeometricTolerance.h"
#include "Geometry.h"
#include "Note.h"
#include "TolerancedFeature.h"

using namespace PMI;

namespace
{
/// The solid bodies of a Body, by type name: PMI does not link PartDesign
std::vector<App::DocumentObject*> solidBodiesOf(Part::BodyBase* body)
{
    std::vector<App::DocumentObject*> res;
    static const Base::Type type = Base::Type::fromName("PartDesign::SolidBody");
    if (!body || type.isBad()) {
        return res;
    }
    for (auto obj : body->Group.getValues()) {
        if (obj && obj->isDerivedFrom(type)) {
            res.push_back(obj);
        }
    }
    return res;
}

/// The same element: a face on the same surface, of the same area and
/// centre; an edge on the same curve, of the same length and centre; a
/// vertex at the same point
bool sameElement(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    if (a.ShapeType() != b.ShapeType()) {
        return false;
    }
    if (a.ShapeType() == TopAbs_VERTEX) {
        return Geom::sameSurface(Geom::ofElement(a), Geom::ofElement(b), 1e-6, 1e-9);
    }
    Primitive pa0 = Geom::ofElement(a), pb0 = Geom::ofElement(b);
    if (pa0.type != pb0.type
        || (pa0.type != Primitive::None && !Geom::sameSurface(pa0, pb0, 1e-6, 1e-9))) {
        return false;
    }
    GProp_GProps pa, pb;
    if (a.ShapeType() == TopAbs_FACE) {
        BRepGProp::SurfaceProperties(a, pa);
        BRepGProp::SurfaceProperties(b, pb);
    }
    else {
        BRepGProp::LinearProperties(a, pa);
        BRepGProp::LinearProperties(b, pb);
    }
    return std::fabs(pa.Mass() - pb.Mass()) < 1e-6 * std::max(1.0, pa.Mass())
        && pa.CentreOfMass().Distance(pb.CentreOfMass()) < 1e-6;
}

const char* elementType(const TopoDS_Shape& s)
{
    switch (s.ShapeType()) {
        case TopAbs_FACE:
            return "Face";
        case TopAbs_EDGE:
            return "Edge";
        case TopAbs_VERTEX:
            return "Vertex";
        default:
            return nullptr;
    }
}
}  // namespace

bool Api::solidReference(App::DocumentObject* obj, const std::string& subname,
                         App::DocumentObject*& solid, std::string& face, std::string& why)
{
    solid = nullptr;
    face.clear();
    if (!obj) {
        why = "nothing picked";
        return false;
    }
    App::DocumentObject* owner = nullptr;
    Part::TopoShape picked;
    try {
        picked = Part::Feature::getTopoShape(obj, subname.c_str(), true, nullptr, &owner);
    }
    catch (const Base::Exception& e) {
        why = e.what();
        return false;
    }
    const char* type = picked.isNull() ? nullptr : elementType(picked.getShape());
    if (!type) {
        why = "pick a face, an edge or a vertex";
        return false;
    }
    // A face of a Body's feature: the solid body holding it
    auto body = owner ? Part::BodyBase::findBodyOf(owner) : nullptr;
    if (!body && obj->isDerivedFrom<Part::BodyBase>()) {
        body = static_cast<Part::BodyBase*>(obj);
    }
    std::vector<App::DocumentObject*> candidates;
    if (body) {
        candidates = solidBodiesOf(body);
        if (candidates.empty()) {
            why = "the Body has no solid body yet: recompute it once it holds an annotation set";
            return false;
        }
    }
    else {
        candidates.push_back(owner ? owner : obj);
    }
    // Compared in the same coordinates: the solid body's shape as the pick
    // was taken, both placed the same way
    for (auto cand : candidates) {
        Part::TopoShape shape = Part::Feature::getTopoShape(cand);
        if (body) {
            // The pick came through the Body's placement
            shape = Part::Feature::getTopoShape(body, (std::string(cand->getNameInDocument()) + ".").c_str());
        }
        auto elements = Geom::elements(shape.getShape(), type);
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (sameElement(elements[i], picked.getShape())) {
                solid = cand;
                face = type + std::to_string(i + 1);
                return true;
            }
        }
    }
    why = std::string("the ") + (type[0] == 'F' ? "face" : type[0] == 'E' ? "edge" : "vertex")
        + " is on no solid of the part";
    return false;
}

AnnotationSet* Api::getSet(App::DocumentObject* part, bool create)
{
    if (!part || !part->getDocument()) {
        return nullptr;
    }
    // A solid body's set is its Body's
    App::DocumentObject* target = part;
    if (auto body = Part::BodyBase::findBodyOf(part)) {
        if (!part->isDerivedFrom<Part::BodyBase>()) {
            target = body;
        }
    }
    if (auto set = AnnotationSet::find(target)) {
        return set;
    }
    if (!create) {
        return nullptr;
    }
    auto doc = part->getDocument();
    auto set = static_cast<AnnotationSet*>(
        doc->addObject(AnnotationSet::getClassTypeId().getName(), "PMI"));
    if (target->isDerivedFrom<Part::BodyBase>()) {
        if (auto group = target->getExtensionByType<App::GroupExtension>(true)) {
            group->addObject(set);
        }
    }
    else {
        // Beside the shape: in its App::Part, else its plain group, if it has
        // one -- a set outside the Part would link into it out of scope
        auto group = App::GeoFeatureGroupExtension::getGroupOfObject(target);
        if (!group) {
            group = App::GroupExtension::getGroupOfObject(target);
        }
        if (auto ext = group ? group->getExtensionByType<App::GroupExtension>(true) : nullptr) {
            ext->addObject(set);
        }
    }
    set->Part.setValue(target);
    return set;
}

TolerancedFeature* Api::feature(AnnotationSet* set, App::DocumentObject* solid,
                                const std::vector<std::string>& faces, const char* kind)
{
    if (!set || !solid || faces.empty()) {
        return nullptr;
    }
    for (auto feat : set->features()) {
        if (feat->refersTo(solid, faces)
            && (!kind || std::string(feat->Kind.getValueAsString()) == kind)) {
            return feat;
        }
    }
    auto doc = set->getDocument();
    auto feat = static_cast<TolerancedFeature*>(
        doc->addObject(TolerancedFeature::getClassTypeId().getName(), "Feature"));
    set->addObject(feat);
    std::vector<App::DocumentObject*> objs(faces.size(), solid);
    feat->References.setValues(objs, faces);
    if (kind) {
        feat->Kind.setValue(kind);
    }
    return feat;
}

std::string Api::nextDatumLetter(AnnotationSet* set, App::DocumentObject* solid)
{
    std::set<std::string> used;
    if (set) {
        for (auto obj : set->Group.getValues()) {
            if (auto datum = freecad_cast<Datum*>(obj)) {
                if (!solid || datum->solid() == solid) {
                    used.insert(datum->Letter.getStrValue());
                }
            }
        }
    }
    bool asme = set && set->isASME();
    for (char c = 'A'; c <= 'Z'; ++c) {
        if (asme && (c == 'I' || c == 'O' || c == 'Q')) {
            continue;
        }
        std::string s(1, c);
        if (!used.count(s)) {
            return s;
        }
    }
    // Past Z: AA, AB, ... (ASME Y14.5 7.11)
    for (char c = 'A'; c <= 'Z'; ++c) {
        std::string s(2, c);
        if (!used.count(s)) {
            return s;
        }
    }
    return "AA";
}

App::DocumentObject* Api::frame(AnnotationSet* set, const std::vector<std::string>& compartments)
{
    if (!set || compartments.empty()) {
        return nullptr;
    }
    auto trim = [](std::string v) {
        v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
        return v;
    };
    std::vector<std::string> want;
    for (auto& c : compartments) {
        want.push_back(trim(c));
    }
    for (auto obj : set->Group.getValues()) {
        if (auto drf = freecad_cast<DatumReferenceFrame*>(obj)) {
            std::vector<std::string> have;
            for (auto& c : drf->Compartments.getValues()) {
                have.push_back(trim(c));
            }
            if (have == want) {
                return drf;
            }
        }
    }
    auto drf = static_cast<DatumReferenceFrame*>(set->getDocument()->addObject(
        DatumReferenceFrame::getClassTypeId().getName(), "DatumSystem"));
    set->addObject(drf);
    drf->Compartments.setValues(want);
    return drf;
}

Callout* Api::stackUnder(const Callout* callout)
{
    // Its own lines, or its own point: a linear dimension, a datum target
    auto ownLines = [](const Callout* c) {
        auto dim = freecad_cast<const Dimension*>(c);
        return (dim && !dim->isSize()) || c->isDerivedFrom<DatumTarget>();
    };
    if (!callout || ownLines(callout)) {
        return nullptr;
    }
    auto set = callout->getSet();
    TolerancedFeature* feature = callout->calloutFeature();
    if (!set || !feature) {
        return nullptr;
    }
    Callout* under = nullptr;
    for (auto obj : set->Group.getValues()) {
        auto c = freecad_cast<Callout*>(obj);
        if (!c || c == callout || ownLines(c) || c->calloutFeature() != feature) {
            continue;
        }
        under = c;
    }
    return under;
}

std::vector<TolerancedFeature*> Api::featuresOf(const Callout* callout)
{
    std::vector<TolerancedFeature*> out;
    if (auto dim = freecad_cast<const Dimension*>(callout)) {
        for (auto obj : dim->Features.getValues()) {
            if (auto f = freecad_cast<TolerancedFeature*>(obj)) {
                out.push_back(f);
            }
        }
    }
    else if (callout) {
        if (auto f = callout->calloutFeature()) {
            out.push_back(f);
        }
    }
    return out;
}

std::vector<Api::Applies> Api::query(const App::DocumentObject* solid, const std::string& face)
{
    std::vector<Applies> out;
    auto set = solid ? AnnotationSet::find(solid) : nullptr;
    if (!set) {
        return out;
    }
    const std::string name = Data::oldElementName(face.c_str());
    auto holds = [&](const TolerancedFeature* f) {
        const auto& objs = f->References.getValues();
        auto subs = f->References.getSubValues(false);
        for (std::size_t i = 0; i < objs.size() && i < subs.size(); ++i) {
            if (objs[i] == solid && Data::oldElementName(subs[i].c_str()) == name) {
                return true;
            }
        }
        return false;
    };
    std::set<const Callout*> seen;
    std::set<std::string> letters;
    for (auto obj : set->Group.getValues()) {
        auto c = freecad_cast<Callout*>(obj);
        if (!c) {
            continue;
        }
        for (auto f : featuresOf(c)) {
            if (holds(f)) {
                out.push_back({c, "on"});
                seen.insert(c);
                if (auto datum = freecad_cast<Datum*>(c)) {
                    letters.insert(datum->Letter.getStrValue());
                }
                break;
            }
        }
    }
    if (letters.empty()) {
        return out;
    }
    for (auto obj : set->Group.getValues()) {
        auto gt = freecad_cast<GeometricTolerance*>(obj);
        if (!gt || seen.count(gt) || AnnotationSet::solidOf(gt->getFeature()) != solid) {
            continue;
        }
        auto drf = gt->getDatumSystem();
        std::vector<DatumReferenceFrame::Compartment> comps;
        std::string why;
        if (!drf || !drf->parse(comps, why)) {
            continue;
        }
        std::string cited;
        for (auto& comp : comps) {
            for (auto& d : comp.datums) {
                if (letters.count(d) && cited.find(d) == std::string::npos) {
                    cited += cited.empty() ? d : ", " + d;
                }
            }
        }
        if (!cited.empty()) {
            out.push_back({gt, "datum " + cited});
        }
    }
    return out;
}
