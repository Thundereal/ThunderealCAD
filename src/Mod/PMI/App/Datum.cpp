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
#include <cctype>
#include <set>

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Trsf.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>

#include <Mod/Part/App/TopoShape.h>

#include "AnnotationSet.h"
#include "Check.h"
#include "Datum.h"
#include "TolerancedFeature.h"
#include "View.h"

using namespace PMI;

PROPERTY_SOURCE(PMI::Datum, PMI::Callout)

Datum::Datum()
{
    ADD_PROPERTY_TYPE(Letter, ("A"), "PMI", App::Prop_None, "The datum's letter");
    ADD_PROPERTY_TYPE(Feature, (nullptr), "PMI", App::Prop_None, "The datum feature");
    ADD_PROPERTY_TYPE(Targets, (), "PMI", App::Prop_None,
                      "The datum targets establishing it, if any");
    ADD_PROPERTY_TYPE(Shape, (TopoDS_Shape()), "PMI",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Transient),
                      "The simulated datum: a plane, an axis or a point");
}

TolerancedFeature* Datum::calloutFeature() const
{
    return getFeature();
}

TolerancedFeature* Datum::getFeature() const
{
    return freecad_cast<TolerancedFeature*>(Feature.getValue());
}

std::vector<DatumTarget*> Datum::getTargets() const
{
    std::vector<DatumTarget*> out;
    for (auto obj : Targets.getValues()) {
        if (auto t = freecad_cast<DatumTarget*>(obj)) {
            out.push_back(t);
        }
    }
    return out;
}

const App::DocumentObject* Datum::solid() const
{
    if (auto s = AnnotationSet::solidOf(getFeature())) {
        return s;
    }
    for (auto t : getTargets()) {
        auto feat = freecad_cast<TolerancedFeature*>(t->Feature.getValue());
        if (auto s = AnnotationSet::solidOf(feat)) {
            return s;
        }
    }
    return nullptr;
}

void Datum::onChanged(const App::Property* prop)
{
    // The targets draw the letter: they depend on the datum only through it
    if (prop == &Letter && !isRestoring()) {
        for (auto t : getTargets()) {
            t->touch();
        }
    }
    Callout::onChanged(prop);
}

TopoDS_Shape Datum::fromTargets() const
{
    TopoDS_Compound comp;
    BRep_Builder builder;
    builder.MakeCompound(comp);
    bool any = false;
    for (auto t : getTargets()) {
        TopoDS_Shape s = t->Shape.getShape().getShape();
        if (!s.IsNull()) {
            builder.Add(comp, s);
            any = true;
        }
    }
    return any ? TopoDS_Shape(comp) : TopoDS_Shape();
}

bool Datum::isLegalLetter(const std::string& letter, bool asme, std::string& why)
{
    if (letter.empty()) {
        why = "a datum needs a letter";
        return false;
    }
    for (char c : letter) {
        if (c < 'A' || c > 'Z') {
            why = "a datum letter is upper case A to Z: '" + letter + "'";
            return false;
        }
        if (asme && (c == 'I' || c == 'O' || c == 'Q')) {
            why = std::string("ASME Y14.5 does not use the letter ") + c;
            return false;
        }
    }
    return true;
}

TopoDS_Shape Datum::simulated() const
{
    auto feat = getFeature();
    if (!feat) {
        return getTargets().empty() ? TopoDS_Shape() : fromTargets();
    }
    const Primitive& p = feat->primitive();
    TopoDS_Shape shape;
    switch (p.type) {
        case Primitive::Plane:
            // The plane a datum feature simulator touches: the feature's
            // own (or derived) planar shape stands for it
            shape = feat->Shape.getShape().getShape();
            break;
        case Primitive::Cylinder:
        case Primitive::Cone:
        case Primitive::Circle:
        case Primitive::Line: {
            const TopoDS_Shape fs = feat->Shape.getShape().getShape();
            if (feat->getKind() == TolerancedFeature::Axis) {
                shape = fs;
            }
            else {
                // The axis, as long as the feature
                Primitive axis = p.reference();
                shape = Geom::axisEdge(axis.loc, axis.dir, fs);
            }
            break;
        }
        case Primitive::Sphere:
        case Primitive::Point:
            shape = BRepBuilderAPI_MakeVertex(p.loc).Shape();
            break;
        default:
            break;
    }
    return shape;
}

void Datum::onDocumentRestored()
{
    // Shape is not saved: made again, for a reader before the first
    // recompute (its feature is restored before it, its Shape with it)
    bool touched = isTouched();
    try {
        Shape.setValue(simulated());
    }
    catch (...) {
    }
    Callout::onDocumentRestored();
    if (!touched) {
        purgeTouched();
    }
}

App::DocumentObjectExecReturn* Datum::execute()
{
    auto feat = getFeature();
    if (!feat && !getTargets().empty()) {
        // Established by its targets alone: they are drawn, each with its
        // symbol; a datum feature symbol needs a feature to stand on
        Shape.setValue(simulated());
        if (UseImported.getValue() && !ImportedGraphic.getShape().isNull()) {
            updatePresentation();
        }
        else {
            Presentation.setValue(TopoDS_Shape());
        }
        std::string why;
        auto set = getSet();
        if (!isLegalLetter(Letter.getStrValue(), set && set->isASME(), why)) {
            setState(Invalid, why);
        }
        else {
            setState(Ok);
        }
        return App::DocumentObject::StdReturn;
    }
    if (!feat) {
        Shape.setValue(TopoDS_Shape());
        setState(Broken, "no datum feature");
        return App::DocumentObject::StdReturn;
    }
    const Primitive& p = feat->primitive();
    Shape.setValue(simulated());
    updatePresentation();

    std::string why;
    auto set = getSet();
    if (!isLegalLetter(Letter.getStrValue(), set && set->isASME(), why)) {
        setState(Invalid, why);
    }
    else if (feat->getState() == Broken) {
        setState(Broken, "its feature is broken: " + feat->ConditionMessage.getStrValue());
    }
    else if (p.type == Primitive::None) {
        setState(Invalid, "a datum feature must be a plane, a cylinder, a cone or a sphere");
    }
    else {
        setState(Ok);
    }
    return App::DocumentObject::StdReturn;
}

PROPERTY_SOURCE(PMI::DatumReferenceFrame, PMI::Annotation)

DatumReferenceFrame::DatumReferenceFrame()
{
    ADD_PROPERTY_TYPE(Compartments, (), "PMI", App::Prop_None,
                      "Up to three, in order: a datum or a common datum, with modifiers: "
                      "\"A\", \"B(M)\", \"C-D\"");
}

const std::vector<std::string>& DatumReferenceFrame::modifierNames()
{
    // MMR, LMR; free state; basic; projected; translation; point, line, plane
    // (ISO 5459 [PT] [SL] [PL]); any cross section, any longitudinal
    // section; contacting feature; distance variable; diameters
    static const std::vector<std::string> names = {"M",  "L",   "F",  "BSC", "P",  "><", "PT", "SL",
                                                   "PL", "ACS", "ALS", "CF", "DV", "LD", "MD", "PD"};
    return names;
}

namespace
{
std::string trim(const std::string& s)
{
    auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return {};
    }
    auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> res;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            res.push_back(trim(cur));
            cur.clear();
        }
        else {
            cur += c;
        }
    }
    res.push_back(trim(cur));
    return res;
}
}  // namespace

bool DatumReferenceFrame::parse(std::vector<Compartment>& result, std::string& why) const
{
    result.clear();
    const auto& values = Compartments.getValues();
    if (values.empty()) {
        why = "a datum reference frame needs a primary datum";
        return false;
    }
    if (values.size() > 3) {
        why = "a datum reference frame has at most three compartments";
        return false;
    }
    std::set<std::string> seen;
    for (auto& raw : values) {
        std::string text = trim(raw);
        Compartment comp;
        std::string body = text;
        auto open = text.find('(');
        if (open != std::string::npos) {
            if (text.back() != ')') {
                why = "unclosed modifiers in '" + raw + "'";
                return false;
            }
            body = trim(text.substr(0, open));
            for (auto& m : split(text.substr(open + 1, text.size() - open - 2), ',')) {
                const auto& names = modifierNames();
                if (std::find(names.begin(), names.end(), m) == names.end()) {
                    why = "unknown datum modifier '" + m + "' in '" + raw + "'";
                    return false;
                }
                comp.modifiers.push_back(m);
            }
        }
        for (auto& d : split(body, '-')) {
            std::string dummy;
            if (!Datum::isLegalLetter(d, false, dummy)) {
                why = "'" + raw + "' is not a datum or a common datum (A, A-B)";
                return false;
            }
            if (!seen.insert(d).second) {
                why = "datum " + d + " is in the frame twice";
                return false;
            }
            comp.datums.push_back(d);
        }
        result.push_back(std::move(comp));
    }
    return true;
}

App::DocumentObjectExecReturn* DatumReferenceFrame::execute()
{
    std::vector<Compartment> comps;
    std::string why;
    if (!parse(comps, why)) {
        setState(Invalid, why);
    }
    else {
        setState(Ok);
    }
    return App::DocumentObject::StdReturn;
}

// ---- DatumTarget ----

PROPERTY_SOURCE(PMI::DatumTarget, PMI::Callout)

const char* DatumTarget::TargetTypeEnums[] = {"Point",  "Line",          "Rectangle", "Circle",
                                              "Area",   "CircularCurve", nullptr};

DatumTarget::DatumTarget()
{
    ADD_PROPERTY_TYPE(Number, (1), "PMI", App::Prop_None, "The target's number: the 1 of A1");
    ADD_PROPERTY_TYPE(TargetType, (long(Point)), "PMI", App::Prop_None,
                      "A point, a line, a rectangle, a circle, an area (its faces), or a "
                      "circular curve (a circle that is no area)");
    TargetType.setEnums(TargetTypeEnums);
    ADD_PROPERTY_TYPE(Target, (Base::Placement()), "PMI", App::Prop_None,
                      "Where it is: the origin its centre, z out of the part, x along a line "
                      "or a rectangle's length");
    ADD_PROPERTY_TYPE(Length, (0.0), "PMI", App::Prop_None,
                      "A line's length, a rectangle's length, a circle's diameter");
    ADD_PROPERTY_TYPE(Width, (0.0), "PMI", App::Prop_None, "A rectangle's width");
    ADD_PROPERTY_TYPE(Movable, (false), "PMI", App::Prop_None,
                      "A movable target: its simulator moves with the feature (ASME)");
    ADD_PROPERTY_TYPE(Feature, (nullptr), "PMI", App::Prop_None,
                      "The faces it lies on; an area target is these faces");
    ADD_PROPERTY_TYPE(Shape, (TopoDS_Shape()), "PMI",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Transient),
                      "The target: a vertex, an edge or a face");
}

TolerancedFeature* DatumTarget::calloutFeature() const
{
    return freecad_cast<TolerancedFeature*>(Feature.getValue());
}

Datum* DatumTarget::getDatum() const
{
    for (auto obj : getInList()) {
        auto d = freecad_cast<Datum*>(obj);
        if (!d) {
            continue;
        }
        const auto& ts = d->Targets.getValues();
        if (std::find(ts.begin(), ts.end(), this) != ts.end()) {
            return d;
        }
    }
    return nullptr;
}

std::string DatumTarget::label() const
{
    auto d = getDatum();
    return (d ? d->Letter.getStrValue() : std::string()) + std::to_string(Number.getValue());
}

bool DatumTarget::hasSize() const
{
    auto t = getTargetType();
    return t == Line || t == Rectangle || t == Circle || t == CircularCurve;
}

bool DatumTarget::calloutTarget(gp_Pnt& target) const
{
    if (getTargetType() == Area) {
        return Callout::calloutTarget(target);
    }
    Base::Vector3d p = Target.getValue().getPosition();
    target = gp_Pnt(p.x, p.y, p.z);
    return true;
}

bool DatumTarget::autoPlane(double h, gp_Ax3& plane, gp_Pnt& target) const
{
    if (getTargetType() == Area) {
        return Callout::autoPlane(h, plane, target);
    }
    if (!calloutTarget(target)) {
        return false;
    }
    if (View* view = planeView()) {
        // In the view's plane through the target, up and to the right of it
        gp_Ax3 f = view->frame();
        gp_Vec x(f.XDirection()), y(f.YDirection()), z(f.Direction());
        plane = gp_Ax3(target.Translated(z * (0.05 * h) + x * (3 * h) + y * (2 * h)),
                       f.Direction(), f.XDirection());
        return true;
    }
    // In the plane of the part's surface there, up and to the right
    const Base::Placement& pla = Target.getValue();
    Base::Vector3d z = pla.getRotation().multVec(Base::Vector3d(0, 0, 1));
    Base::Vector3d x = pla.getRotation().multVec(Base::Vector3d(1, 0, 0));
    gp_Dir dz(z.x, z.y, z.z), dx(x.x, x.y, x.z);
    gp_Vec vy = gp_Vec(dz).Crossed(gp_Vec(dx));
    double off = hasSize() ? 0.5 * Length.getValue() : 0.0;
    gp_Pnt o = target.Translated(gp_Vec(dz) * (0.05 * h) + gp_Vec(dx) * (off + 3 * h)
                                 + vy * (off + 3 * h));
    plane = gp_Ax3(o, dz, dx);
    return true;
}

TopoDS_Shape DatumTarget::makeShape() const
{
    const Base::Placement& pla = Target.getValue();
    gp_Trsf trsf;
    Part::TopoShape::convertTogpTrsf(pla.toMatrix(), trsf);
    double len = Length.getValue();
    double wid = Width.getValue();
    TopoDS_Shape s;
    switch (getTargetType()) {
        case Point:
            s = BRepBuilderAPI_MakeVertex(gp_Pnt()).Shape();
            break;
        case Line:
            if (len > 0) {
                s = BRepBuilderAPI_MakeEdge(gp_Pnt(-len / 2, 0, 0), gp_Pnt(len / 2, 0, 0)).Shape();
            }
            break;
        case Rectangle:
            if (len > 0 && wid > 0) {
                BRepBuilderAPI_MakePolygon poly(gp_Pnt(-len / 2, -wid / 2, 0),
                                                gp_Pnt(len / 2, -wid / 2, 0),
                                                gp_Pnt(len / 2, wid / 2, 0),
                                                gp_Pnt(-len / 2, wid / 2, 0), true);
                s = BRepBuilderAPI_MakeFace(poly.Wire(), true).Shape();
            }
            break;
        case Circle:
            if (len > 0) {
                gp_Circ c(gp_Ax2(), len / 2);
                TopoDS_Edge e = BRepBuilderAPI_MakeEdge(c).Edge();
                s = BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakeWire(e).Wire(), true).Shape();
            }
            break;
        case CircularCurve:
            // The circle itself, no face within it
            if (len > 0) {
                s = BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(), len / 2)).Edge();
            }
            break;
        case Area:
            if (auto feat = calloutFeature()) {
                return feat->Shape.getShape().getShape();
            }
            return {};
    }
    return s.IsNull() ? s : s.Moved(TopLoc_Location(trsf));
}

void DatumTarget::onDocumentRestored()
{
    // Shape is not saved: made again, as Datum's
    bool touched = isTouched();
    try {
        Shape.setValue(makeShape());
    }
    catch (...) {
    }
    Callout::onDocumentRestored();
    if (!touched) {
        purgeTouched();
    }
}

App::DocumentObjectExecReturn* DatumTarget::execute()
{
    Shape.setValue(makeShape());
    updatePresentation();
    for (auto& issue : Check::object(this)) {
        if (issue.severity == Check::Error) {
            setState(Invalid, issue.message);
            return App::DocumentObject::StdReturn;
        }
    }
    auto feat = calloutFeature();
    if (feat && feat->getState() == Broken) {
        setState(Broken, "its feature is broken: " + feat->ConditionMessage.getStrValue());
    }
    else {
        setState(Ok);
    }
    return App::DocumentObject::StdReturn;
}
