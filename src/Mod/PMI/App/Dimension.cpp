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
#include <cmath>
#include <regex>
#include <vector>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <Base/Tools.h>
#include <Base/Unit.h>
#include <Mod/Part/App/PartFeature.h>

#include "Dimension.h"
#include "TolerancedFeature.h"
#include "View.h"

using namespace PMI;

const char* Dimension::TypeEnums[] = {"Diameter",
                                      "Radius",
                                      "SphericalDiameter",
                                      "SphericalRadius",
                                      "Width",
                                      "AngularSize",
                                      "Distance",
                                      "Angle",
                                      "CurveLength",
                                      "Depth",
                                      nullptr};

PROPERTY_SOURCE(PMI::Dimension, PMI::Callout)

Dimension::Dimension()
{
    ADD_PROPERTY_TYPE(Type, (0L), "PMI", App::Prop_None,
                      "A size (Diameter ... AngularSize, CurveLength, Depth) of one feature, or a "
                      "location (Distance, Angle) between two");
    Type.setEnums(TypeEnums);
    ADD_PROPERTY_TYPE(Features, (nullptr), "PMI", App::Prop_None,
                      "The toleranced features: one for a size, two for a location");
    ADD_PROPERTY_TYPE(Nominal, (0.0), "PMI", App::Prop_ReadOnly,
                      "Measured from the features on every recompute; as given (read from a "
                      "file) where the model cannot be measured");
    ADD_PROPERTY_TYPE(Upper, (0.0), "PMI", App::Prop_None, "The upper deviation");
    ADD_PROPERTY_TYPE(Lower, (0.0), "PMI", App::Prop_None, "The lower deviation");
    ADD_PROPERTY_TYPE(Fit, (""), "PMI", App::Prop_None,
                      "An ISO 286 fit instead of the deviations, e.g. H7");
    ADD_PROPERTY_TYPE(Basic, (false), "PMI", App::Prop_None,
                      "A basic (theoretically exact) dimension: no tolerance");
    ADD_PROPERTY_TYPE(Modifiers, (), "PMI", App::Prop_None, "ISO 14405 size modifiers");
    ADD_PROPERTY_TYPE(DecimalPlaces, (-1), "PMI", App::Prop_None,
                      "Decimal places shown; -1 for the set's");
    ADD_PROPERTY_TYPE(Limits, (false), "PMI", App::Prop_None,
                      "The tolerance shown as its limits, 34.8-35.2, not as deviations");
    ADD_PROPERTY_TYPE(Reference, (false), "PMI", App::Prop_None,
                      "A reference dimension, for information: in parentheses, no tolerance");
    setUnits();
}

const std::vector<std::string>& Dimension::modifierNames()
{
    static const std::vector<std::string> names = {
        "E",  "LP", "LS", "GG", "GX", "GN", "GC", "CC", "CA",  "CV",  "SX",       "SN",
        "SA", "SM", "SD", "SR", "SQ", "ACS", "SCS", "CT", "F", "Between", "UnitBasis"};
    return names;
}

bool Dimension::isFit(const std::string& fit)
{
    static const std::regex re("^([A-HJKMNPR-Z]|JS|Z[ABC]|[a-hjkmnpr-z]|js|z[abc]|CD|EF|FG|cd|ef|fg)"
                               "(01|0|[1-9]|1[0-8])$");
    return std::regex_match(fit, re);
}

void Dimension::setUnits()
{
    Base::Unit unit = isAngular() ? Base::Unit::Angle : Base::Unit::Length;
    for (App::PropertyQuantity* p : {&Nominal, &Upper, &Lower}) {
        if (p->getUnit() != unit) {
            p->setUnit(unit);
        }
    }
}

void Dimension::onChanged(const App::Property* prop)
{
    if (prop == &Type) {
        setUnits();
    }
    Callout::onChanged(prop);
}

TolerancedFeature* Dimension::calloutFeature() const
{
    for (auto obj : Features.getValues()) {
        if (auto f = freecad_cast<TolerancedFeature*>(obj)) {
            return f;
        }
    }
    return nullptr;
}

bool Dimension::measure(double& value, std::string& why) const
{
    std::vector<TolerancedFeature*> feats;
    for (auto obj : Features.getValues()) {
        if (auto f = freecad_cast<TolerancedFeature*>(obj)) {
            feats.push_back(f);
        }
    }
    std::size_t need = isSize() ? 1 : 2;
    if (feats.size() != need) {
        why = isSize() ? "a size dimension needs one toleranced feature"
                       : "a location dimension needs two toleranced features";
        return false;
    }
    const Primitive& p = feats[0]->primitive();
    const auto& faces = feats[0]->facePrimitives();
    // A size is of the faces, whatever the feature derives from them: a
    // hole's diameter is measured on its cylinder, also when the feature
    // is its axis (shared with a position tolerance)
    // The first face of the kind the size is of: a blind hole's feature may
    // hold its drill point's cone too
    const Primitive* found = faces.empty() ? &p : &faces.front();
    Primitive::Type want = Primitive::None;
    switch (getType()) {
        case Diameter:
        case Radius:
            want = Primitive::Cylinder;
            break;
        case SphericalDiameter:
        case SphericalRadius:
            want = Primitive::Sphere;
            break;
        default:
            break;
    }
    auto first = [&](Primitive::Type type) {
        for (auto& f : faces) {
            if (f.type == type) {
                found = &f;
                return true;
            }
        }
        return false;
    };
    if (want != Primitive::None && !first(want) && getType() == Radius) {
        first(Primitive::Torus);
    }
    // A hole's or a boss's edge stands for its size as its cylinder does
    if (want == Primitive::Cylinder && found->type != Primitive::Cylinder
        && found->type != Primitive::Torus) {
        first(Primitive::Circle);
    }
    const Primitive& surface = *found;
    switch (getType()) {
        case Radius:
            // A fillet's radius: the tube of its torus
            if (surface.type == Primitive::Torus) {
                value = surface.radius;
                return true;
            }
            [[fallthrough]];
        case Diameter:
            if (surface.type != Primitive::Cylinder && surface.type != Primitive::Circle) {
                why = getType() == Radius
                    ? "a radius needs a cylindrical or toroidal feature, or a circular edge"
                    : "a diameter needs a cylindrical feature or a circular edge";
                return false;
            }
            value = getType() == Diameter ? 2 * surface.radius : surface.radius;
            return true;
        case SphericalDiameter:
        case SphericalRadius:
            if (surface.type != Primitive::Sphere) {
                why = "a spherical size needs a spherical feature";
                return false;
            }
            value = getType() == SphericalDiameter ? 2 * surface.radius : surface.radius;
            return true;
        case Width:
            if (faces.size() == 2 && faces[0].type == Primitive::Plane
                && faces[1].type == Primitive::Plane) {
                if (!Geom::distance(faces[0], faces[1], value, why)) {
                    return false;
                }
                // Two faces of one plane (a counterbore's floor, halved) are
                // no width
                if (value > 1e-6) {
                    return true;
                }
            }
            // A pattern of widths ("2X"), a slot
            if (Geom::width(faces, value)) {
                return true;
            }
            why = "a width needs a feature of two parallel planar faces, a pattern of them or "
                  "a slot";
            return false;
        case AngularSize:
            if (surface.type == Primitive::Cone) {
                value = 2 * surface.angle * 180.0 / M_PI;
                return true;
            }
            if (faces.size() == 2 && Geom::angle(faces[0], faces[1], value, why)) {
                value *= 180.0 / M_PI;
                return true;
            }
            why = "an angular size needs a cone or two planar faces";
            return false;
        case Depth: {
            // On its floor alone: of the bores the floor closes
            bool bored = std::any_of(faces.begin(), faces.end(), [](const Primitive& f) {
                return f.type == Primitive::Cylinder;
            });
            if (bored) {
                return Geom::depth(faces, value, why);
            }
            std::vector<Primitive> all = faces;
            const auto& bores = feats[0]->borePrimitives();
            all.insert(all.end(), bores.begin(), bores.end());
            return Geom::depth(all, value, why);
        }
        case Distance:
            return Geom::distance(p, feats[1]->primitive(), value, why);
        case Angle:
            if (Geom::angle(p, feats[1]->primitive(), value, why)) {
                value *= 180.0 / M_PI;
                return true;
            }
            return false;
        case CurveLength: {
            // The edges' length; along faces (a file's curve on them) it is
            // not measured: the value stays as given
            TopoDS_Shape shape = feats[0]->Shape.getValue();
            if (shape.IsNull() || TopExp_Explorer(shape, TopAbs_FACE).More()
                || !TopExp_Explorer(shape, TopAbs_EDGE).More()) {
                value = Nominal.getValue();
                return true;
            }
            value = 0;
            for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next()) {
                value += GCPnts_AbscissaPoint::Length(BRepAdaptor_Curve(TopoDS::Edge(e.Current())));
            }
            return true;
        }
    }
    return false;
}

App::DocumentObjectExecReturn* Dimension::execute()
{
    setUnits();
    double value = 0.0;
    std::string why;
    if (!measure(value, why)) {
        setState(Invalid, why);
        // Drawn as the file draws it still: what the file shows is shown, and
        // written again (CTC-03's thickness over five faces, its graphic lost
        // with it); drawn from what it says, there is nothing to measure
        if (UseImported.getValue() && !ImportedGraphic.getShape().isNull()) {
            updatePresentation();
        }
        return App::DocumentObject::StdReturn;
    }
    // Measured again: a value that agrees with the one it has (a file's 90 as
    // 90.000000000292) leaves it as it is
    // Two planes are at an angle and at its supplement: the one it has (a
    // file's 120, measured 60) is the one meant
    if ((getType() == Angle || getType() == AngularSize) && Nominal.getValue() > 90.0
        && std::fabs(Nominal.getValue() - (180.0 - value)) < 1e-6 * 180.0) {
        value = 180.0 - value;
    }
    if (std::fabs(Nominal.getValue() - value) > 1e-9 * std::max(1.0, std::fabs(value))) {
        // Given as its limits, the limits stay: its deviations are about the
        // nominal, which a file gives as the middle of them and the model
        // measures elsewhere (10.00-10.05 on a model at 10)
        double shift = Nominal.getValue() - value;
        bool given = Limits.getValue() && Nominal.getValue() != 0.0
            && (Upper.getValue() != 0.0 || Lower.getValue() != 0.0);
        Nominal.setValue(value);
        if (given) {
            Base::StateLocker guard(_placing);
            Upper.setValue(Upper.getValue() + shift);
            Lower.setValue(Lower.getValue() + shift);
        }
    }
    updatePresentation();
    for (auto obj : Features.getValues()) {
        auto f = freecad_cast<TolerancedFeature*>(obj);
        if (f && f->getState() == Broken) {
            setState(Broken, "its feature is broken: " + f->ConditionMessage.getStrValue());
            return App::DocumentObject::StdReturn;
        }
    }
    setState(Ok);
    return App::DocumentObject::StdReturn;
}

bool Dimension::linear(gp_Vec& dir, gp_Pnt& p1, gp_Pnt& p2, TopoDS_Shape& s1,
                       TopoDS_Shape& s2) const
{
    std::vector<TolerancedFeature*> feats;
    for (auto obj : Features.getValues()) {
        if (auto f = freecad_cast<TolerancedFeature*>(obj)) {
            feats.push_back(f);
        }
    }
    const double atol = 1e-9;
    if (getType() == Width) {
        if (feats.size() != 1) {
            return false;
        }
        const auto& faces = feats[0]->facePrimitives();
        std::vector<TopoDS_Shape> shapes;
        for (TopExp_Explorer xp(feats[0]->Shape.getShape().getShape(), TopAbs_FACE); xp.More();
             xp.Next()) {
            shapes.push_back(xp.Current());
        }
        if (faces.size() != 2 || shapes.size() != 2 || faces[0].type != Primitive::Plane
            || faces[1].type != Primitive::Plane || !faces[0].dir.IsParallel(faces[1].dir, atol)) {
            return false;
        }
        dir = gp_Vec(faces[0].dir);
        p1 = faces[0].loc;
        p2 = faces[1].loc;
        s1 = shapes[0];
        s2 = shapes[1];
    }
    else if (getType() == Distance) {
        if (feats.size() != 2) {
            return false;
        }
        Primitive a = feats[0]->primitive().reference();
        Primitive b = feats[1]->primitive().reference();
        // Measured across: a plane's normal; between two parallel axes, or
        // an axis and a point, square to the axis; between points, the line
        // joining them
        auto across = [](const Primitive& line, const gp_Pnt& p) {
            gp_Vec v(line.loc, p);
            return v - gp_Vec(line.dir) * v.Dot(gp_Vec(line.dir));
        };
        bool aLine = a.type == Primitive::Line, bLine = b.type == Primitive::Line;
        if (a.type == Primitive::Plane || b.type == Primitive::Plane) {
            const Primitive& pl = a.type == Primitive::Plane ? a : b;
            const Primitive& other = a.type == Primitive::Plane ? b : a;
            if (other.type == Primitive::Plane && !other.dir.IsParallel(pl.dir, atol)) {
                return false;
            }
            if (other.type == Primitive::Line && !other.dir.IsNormal(pl.dir, atol)) {
                return false;
            }
            dir = gp_Vec(pl.dir);
        }
        else if (aLine && bLine) {
            if (!a.dir.IsParallel(b.dir, atol)) {
                return false;
            }
            dir = across(a, b.loc);
        }
        else if (aLine || bLine) {
            dir = aLine ? across(a, b.loc) : across(b, a.loc);
        }
        else {
            dir = gp_Vec(a.loc, b.loc);
        }
        p1 = a.loc;
        p2 = b.loc;
        s1 = feats[0]->Shape.getShape().getShape();
        s2 = feats[1]->Shape.getShape().getShape();
    }
    else {
        return false;
    }
    if (dir.Magnitude() < 1e-9 || s1.IsNull() || s2.IsNull()) {
        return false;
    }
    dir.Normalize();
    return true;
}

bool Dimension::autoPlane(double h, gp_Ax3& plane, gp_Pnt& target) const
{
    gp_Vec x;
    gp_Pnt p1, p2;
    TopoDS_Shape s1, s2;
    if (!linear(x, p1, p2, s1, s2)) {
        return Callout::autoPlane(h, plane, target);
    }
    // The standard view most square to what is measured: top, front, right
    // (z toward the reader), with its right and up
    struct Frame
    {
        gp_Vec z, right, up;
    };
    const Frame views[] = {{gp_Vec(0, 0, 1), gp_Vec(1, 0, 0), gp_Vec(0, 1, 0)},
                          {gp_Vec(0, -1, 0), gp_Vec(1, 0, 0), gp_Vec(0, 0, 1)},
                          {gp_Vec(1, 0, 0), gp_Vec(0, 1, 0), gp_Vec(0, 0, 1)}};
    const Frame* view = &views[0];
    double best = 2;
    for (const auto& v : views) {
        double d = std::fabs(v.z.Dot(x));
        if (d < best - 1e-9) {
            best = d;
            view = &v;
        }
    }
    // In a PMI view whose plane holds what is measured: that view
    Frame inView;
    if (PMI::View* pv = planeView()) {
        gp_Ax3 f = pv->frame();
        if (std::fabs(gp_Vec(f.Direction()).Dot(x)) < 1e-6) {
            inView = {gp_Vec(f.Direction()), gp_Vec(f.XDirection()), gp_Vec(f.YDirection())};
            view = &inView;
        }
    }
    // Read from the bottom or the right: along the view's right, or up
    double r = x.Dot(view->right);
    if (r < -1e-9 || (std::fabs(r) <= 1e-9 && x.Dot(view->up) < 0)) {
        x.Reverse();
    }
    gp_Vec z = view->z - x * view->z.Dot(x);
    if (z.Magnitude() < 1e-9) {
        return Callout::autoPlane(h, plane, target);
    }
    z.Normalize();
    gp_Vec y = z.Crossed(x);
    // Beside the part on the view's up side, in front of both sides
    gp_Pnt mid((p1.XYZ() + p2.XYZ()) / 2);
    double ytop = -1e100, ztop = -1e100;
    for (const TopoDS_Shape* s : {&s1, &s2}) {
        for (TopExp_Explorer xp(*s, TopAbs_VERTEX); xp.More(); xp.Next()) {
            gp_Vec v(mid, BRep_Tool::Pnt(TopoDS::Vertex(xp.Current())));
            ytop = std::max(ytop, v.Dot(y));
            ztop = std::max(ztop, v.Dot(z));
        }
    }
    // Beside the whole part, not just the faces measured: a distance
    // between two holes' axes would be inside it, and dimensions landing
    // beside the part together are stacked a slot apart (autoSlot)
    for (auto obj : Features.getValues()) {
        auto f = freecad_cast<TolerancedFeature*>(obj);
        if (!f || f->References.getValues().empty()) {
            continue;
        }
        auto part = freecad_cast<Part::Feature*>(f->References.getValues().front());
        if (!part) {
            continue;
        }
        Bnd_Box box;
        BRepBndLib::Add(part->Shape.getShape().getShape(), box);
        if (box.IsVoid()) {
            continue;
        }
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        for (int i = 0; i < 8; ++i) {
            gp_Vec v(mid, gp_Pnt(i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0));
            ytop = std::max(ytop, v.Dot(y));
            ztop = std::max(ztop, v.Dot(z));
        }
    }
    if (ytop < -1e99) {
        ytop = ztop = 0;
    }
    gp_Pnt origin = mid.Translated(y * (ytop + 2 * h) + z * (ztop + 0.05 * h));
    plane = gp_Ax3(origin, gp_Dir(z), gp_Dir(x));
    target = mid;
    return true;
}
