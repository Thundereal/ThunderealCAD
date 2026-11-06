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
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <numbers>
#include <span>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Base/Tools.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalForm.h"
#include "FeatureSheetMetalRule.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::Element;
using SheetMetal::Form;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalForm, PartDesign::Feature)

const char* SheetMetalForm::KindEnums[] =
    {"Tool", "Dimple", "Louver", "Bead", "Lance", "DrawnCutout", "FlangedHole", nullptr};

namespace
{

const App::PropertyQuantityConstraint::Constraints flankRange = {1.0, 90.0, 1.0};

/// A face's outer normal
gp_Dir outerNormal(const TopoDS_Face& face)
{
    gp_Dir normal = BRepAdaptor_Surface(face).Plane().Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    return normal;
}

/// d projected onto the plane of normal n, or none if it is along it
bool across(const gp_Vec& d, const gp_Dir& n, gp_Dir& res)
{
    const gp_Vec v = d - gp_Vec(n) * d.Dot(gp_Vec(n));
    if (v.Magnitude() < Precision::Confusion()) {
        return false;
    }
    res = gp_Dir(v);
    return true;
}

/// What a form adds and what it cuts, and what it takes of the part
struct Pressed
{
    TopoDS_Shape outside;
    std::vector<TopoDS_Shape> cuts;
    Form form;
};

/// The planar faces of s lying on the plane through frame's origin, normal
/// to its Z
std::vector<TopoDS_Face> onBase(const TopoDS_Shape& s, const gp_Ax3& frame)
{
    std::vector<TopoDS_Face> res;
    for (TopExp_Explorer it(s, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() != GeomAbs_Plane) {
            continue;
        }
        const gp_Pln plane = surface.Plane();
        if (!plane.Axis().Direction().IsParallel(frame.Direction(), Precision::Angular())
            || std::abs(gp_Vec(frame.Location(), plane.Location()).Dot(gp_Vec(frame.Direction())))
                > Precision::Confusion()) {
            continue;
        }
        res.push_back(face);
    }
    return res;
}

/// A user's tool: its shape set with its base face on the frame's XY plane,
/// rising along Z, taken as the form's outside and hollowed by T, the hollow
/// opened down through the wall
Pressed pressTool(const TopoDS_Shape& tool,
                  const TopoDS_Face& base,
                  const gp_Dir& toolX,
                  const gp_Ax3& frame,
                  double T)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(base, props);
    const gp_Dir into = outerNormal(base).Reversed();
    gp_Dir x;
    if (!across(gp_Vec(toolX), into, x)) {
        x = BRepAdaptor_Surface(base).Plane().Position().XDirection();
    }
    gp_Trsf move;
    move.SetDisplacement(gp_Ax3(props.CentreOfMass(), into, x), frame);
    const TopLoc_Location loc(move);
    const TopoDS_Shape outside = tool.Moved(loc);
    TopTools_ListOfShape open;
    open.Append(base.Moved(loc));
    BRepOffsetAPI_MakeThickSolid thick;
    thick.MakeThickSolidByJoin(outside, open, -T, 1e-6);
    thick.Build();
    if (!thick.IsDone()) {
        THROWM(Base::CADKernelError, "The tool could not be hollowed to the sheet's thickness");
    }
    BRepAlgoAPI_Cut hollow(outside, thick.Shape());
    hollow.Build();
    if (!hollow.IsDone()) {
        THROWM(Base::CADKernelError, "The tool's hollow could not be made");
    }
    Pressed res;
    res.outside = outside;
    res.cuts.push_back(hollow.Shape());
    // the hollow opened through the wall
    for (const TopoDS_Face& face : onBase(hollow.Shape(), frame)) {
        res.cuts.push_back(
            BRepPrimAPI_MakePrism(face, gp_Vec(frame.Direction()) * (-2 * T)).Shape());
    }
    // what it takes of the part: its box in the frame, down through the wall
    gp_Trsf toFrame;
    toFrame.SetTransformation(frame);
    Bnd_Box box;
    BRepBndLib::AddOptimal(BRepBuilderAPI_Transform(outside, toFrame, true).Shape(), box,
                           false, false);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    res.form.halfLength = std::max(std::abs(x0), std::abs(x1));
    res.form.halfWidth = std::max(std::abs(y0), std::abs(y1));
    res.form.zMin = -T;
    res.form.zMax = z1;
    return res;
}

/// A dimple: two cones the thickness apart, the outside's from where it
/// leaves the face, the hollow's opened through the wall; its bends rounded
/// afterwards (roundDimple()). A bead is a dimple drawn out along the
/// frame's X, straight for 2 s between its round ends: its sides planes,
/// its ends half cones. A drawn cutout is either open at its top, its floor
/// cut away, only its rim's bend rounded; a flanged hole is a round one at
/// 90 deg, a collar.
struct Dimple
{
    double r0;      // where the outside leaves the face
    double h;       // its top above the face
    double phi;     // the slant, rad
    double rho;     // the bends' inside radius
    double T;
    double s = 0;   // a bead's straight part, half of it
    bool open = false;
    const char* what = "dimple";

    /// how far a point of the frame's XY is from the straight part's line
    double from(const gp_Pnt& q) const
    {
        const double x = std::max(std::abs(q.X()) - s, 0.0);
        return std::hypot(x, q.Y());
    }

    double outside(double z) const
    {
        return r0 - z / std::tan(phi);
    }
    double inside(double z) const
    {
        return outside(z) - T / std::sin(phi);
    }
};

TopoDS_Shape cone(const gp_Ax3& frame, double z0, double r0, double z1, double r1)
{
    const gp_Ax2 at(frame.Location().Translated(gp_Vec(frame.Direction()) * z0),
                    frame.Direction(),
                    frame.XDirection());
    if (std::abs(r0 - r1) < Precision::Confusion()) {
        return BRepPrimAPI_MakeCylinder(at, r0, z1 - z0).Shape();
    }
    return BRepPrimAPI_MakeCone(at, r0, r1, z1 - z0).Shape();
}

/// The stadium in the frame's XY plane at height z, r from the straight
/// part's line: straight for 2 s along X, its ends half circles
TopoDS_Wire stadium(double s, double r, double z)
{
    const gp_Dir up(0, 0, 1);
    BRepBuilderAPI_MakeWire wire;
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(-s, -r, z), gp_Pnt(s, -r, z)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(
                 GC_MakeArcOfCircle(gp_Circ(gp_Ax2(gp_Pnt(s, 0, z), up), r),
                                    -std::numbers::pi / 2, std::numbers::pi / 2, true)
                     .Value())
                 .Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(s, r, z), gp_Pnt(-s, r, z)).Edge());
    wire.Add(BRepBuilderAPI_MakeEdge(
                 GC_MakeArcOfCircle(gp_Circ(gp_Ax2(gp_Pnt(-s, 0, z), up), r),
                                    std::numbers::pi / 2, 3 * std::numbers::pi / 2, true)
                     .Value())
                 .Edge());
    return wire.Wire();
}

/// The solid ruled between the stadia at z0 and z1, r0 and r1 from the
/// straight part's line, placed in the frame: planes and half cones
TopoDS_Shape drawn(const gp_Ax3& frame, double s, double z0, double r0, double z1, double r1)
{
    BRepOffsetAPI_ThruSections loft(true, true);
    loft.AddWire(stadium(s, r0, z0));
    loft.AddWire(stadium(s, r1, z1));
    loft.Build();
    if (!loft.IsDone()) {
        THROWM(Base::CADKernelError, "The bead could not be made");
    }
    gp_Trsf place;
    place.SetDisplacement(gp_Ax3(gp::XOY()), frame);
    return BRepBuilderAPI_Transform(loft.Shape(), place, true).Shape();
}

Pressed pressDimple(const Dimple& d, const gp_Ax3& frame)
{
    const char* what = d.what;
    if (d.r0 <= 0 || d.h <= 0) {
        FC_THROWM(Base::ValueError, "A " << what << "'s diameter and depth are above 0");
    }
    if (d.outside(d.h) <= Precision::Confusion()) {
        FC_THROWM(Base::ValueError,
                  "A " << what << " " << 2 * d.r0 << " mm across cannot rise " << d.h
                       << " mm at its slant: it closes to a point first");
    }
    // its hollow's top: under its own, or, open, past it, its floor cut away
    double top = d.h - d.T;
    if (d.open) {
        if (d.inside(d.h) <= Precision::Confusion()) {
            FC_THROWM(Base::ValueError,
                      "A " << what << " that narrow has no room at its top for the thickness");
        }
        // as far past the top as its hollow stays open, the thickness at most
        top = d.h + std::min(d.T, d.inside(d.h) * std::tan(d.phi) / 2);
    }
    else if (d.inside(d.h - d.T) <= Precision::Confusion()) {
        FC_THROWM(Base::ValueError,
                  "A " << what << " that narrow has no room inside for the thickness");
    }
    // Its outside from inside the wall, which holds all of it below the
    // face, as the louver's: from the other skin, its foot split that skin,
    // and a ring of it beside the hollow, thin at a steep slant, was neither
    // the wall's nor the form's
    const double foot = -d.T / 2;
    Pressed res;
    if (d.s > 0) {
        res.outside = drawn(frame, d.s, foot, d.outside(foot), d.h, d.outside(d.h));
        res.cuts.push_back(drawn(frame, d.s, -2 * d.T, d.inside(-2 * d.T), top, d.inside(top)));
    }
    else {
        res.outside = cone(frame, foot, d.outside(foot), d.h, d.outside(d.h));
        res.cuts.push_back(cone(frame, -2 * d.T, d.inside(-2 * d.T), top, d.inside(top)));
    }
    // as far as its rim's bend reaches on either skin: the two are
    // concentric, so both end rho tan(phi / 2) past where it leaves the face;
    // a circle for a dimple, a box for a bead
    const double reach = d.r0 + d.rho * std::tan(d.phi / 2) + 10 * Precision::Confusion();
    res.form.halfLength = d.s + reach;
    res.form.halfWidth = d.s > 0 ? reach : 0;
    res.form.zMin = -d.T;
    res.form.zMax = d.h;
    return res;
}

/// The dimple's four bends rounded, each a circle about its axis, a bead's
/// a stadium about its straight part's line -- the rim and the top, outside
/// and in -- added to fillet; how many
int roundDimple(BRepFilletAPI_MakeFillet& fillet,
                const TopoDS_Shape& shape,
                const Dimple& d,
                const gp_Ax3& frame)
{
    if (d.rho <= 0) {
        return 0;
    }
    const struct
    {
        double z, r, radius;
    } bends[] = {
        {0, d.outside(0), d.rho},                 // the rim, on the face
        {-d.T, d.inside(-d.T), d.rho + d.T},      // the rim, beneath
        {d.h, d.outside(d.h), d.rho + d.T},       // the top, outside
        {d.h - d.T, d.inside(d.h - d.T), d.rho},  // the top, inside
    };
    gp_Trsf toFrame;
    toFrame.SetTransformation(frame);
    int count = 0;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
        if (c.GetType() != GeomAbs_Circle && c.GetType() != GeomAbs_Line) {
            continue;
        }
        // its ends and its middle, in the frame
        const double u0 = c.FirstParameter(), u1 = c.LastParameter();
        const gp_Pnt at[] = {c.Value(u0).Transformed(toFrame),
                             c.Value((u0 + u1) / 2).Transformed(toFrame),
                             c.Value(u1).Transformed(toFrame)};
        // open, its top is cut, not bent: the rim's bends alone
        for (const auto& b : d.open ? std::span(bends).first(2) : std::span(bends)) {
            bool on = true;
            for (const gp_Pnt& q : at) {
                on = on && std::abs(q.Z() - b.z) < 1e-6 && std::abs(d.from(q) - b.r) < 1e-6;
            }
            if (on) {
                fillet.Add(b.radius, TopoDS::Edge(edges(i)));
                ++count;
            }
        }
    }
    return count;
}

/// The half-space behind the plane through p, out its outward normal, as a
/// box size across
TopoDS_Shape behind(const gp_Pnt& p, const gp_Dir& out, double size)
{
    const gp_Ax2 plane(p, out);
    const gp_Pnt corner = p.Translated(gp_Vec(plane.XDirection()) * (-size / 2)
                                       + gp_Vec(plane.YDirection()) * (-size / 2)
                                       + gp_Vec(out) * -size);
    return BRepPrimAPI_MakeBox(gp_Ax2(corner, out, plane.XDirection()), size, size, size).Shape();
}

/// A louver in its frame: x along it, y from its hinge, at -w / 2, to its
/// opening, at w / 2, z up. Its outside lies under three planes, the ramp
/// through the hinge's line on the face up to the lip h above it, and the
/// ends sloping down to the face at phi, x = +-l / 2 there; the hollow under
/// the same planes the thickness in from them, opened down through the wall
/// and out at the front, where the sheet was cut: a slit the rule's gap wide
/// in front of the lip, on past where its ends leave the face, so the hood
/// stands free of the sheet before it all along. Its bends rounded
/// afterwards (roundLouver()). A lance is a louver whose ends are cut, not
/// drawn: the ramp alone, square at x = +-l / 2, a slit the gap wide beside
/// each side, back past where its hinge's bend leaves the face, so the tab
/// is free on three sides.
struct Louver
{
    double l, w, h, phi, rho, T, gap;
    bool lance = false;

    /// how far past x = +-l / 2 the slit runs: past where the ends' bends
    /// leave the face, and the gap more; a lance's, the gap
    double reach() const
    {
        return (lance ? 0 : rho * std::tan(phi / 2)) + gap;
    }
    /// how far behind the hinge its bend leaves the face, on either skin
    double behindHinge() const
    {
        return rho * std::tan(theta() / 2);
    }

    double theta() const
    {
        return std::atan2(h, w);
    }
    /// the planes' points and outward normals, in the frame, in by in: the
    /// ramp's, the +x end's and the -x end's
    std::array<std::pair<gp_Pnt, gp_Dir>, 3> planes(double in) const
    {
        const double t = theta();
        std::array<std::pair<gp_Pnt, gp_Dir>, 3> res {
            std::pair {gp_Pnt(0, -w / 2, 0), gp_Dir(0, -std::sin(t), std::cos(t))},
            std::pair {gp_Pnt(l / 2, 0, 0), gp_Dir(std::sin(phi), 0, std::cos(phi))},
            std::pair {gp_Pnt(-l / 2, 0, 0), gp_Dir(-std::sin(phi), 0, std::cos(phi))},
        };
        for (auto& [p, n] : res) {
            p.Translate(gp_Vec(n) * -in);
        }
        return res;
    }
};

Pressed pressLouver(const Louver& d, const gp_Ax3& frame)
{
    const char* what = d.lance ? "lance" : "louver";
    if (d.l <= 0 || d.w <= 0 || d.h <= 0) {
        FC_THROWM(Base::ValueError, "A " << what << "'s length, width and depth are above 0");
    }
    if (d.gap <= Precision::Confusion()) {
        FC_THROWM(Base::ValueError,
                  "A " << what << "'s slit is the rule's gap wide: a gap above 0");
    }
    const double sp = std::sin(d.phi), cp = std::cos(d.phi);
    // under the lip, inside: the ramp lowered by the thickness, and the ends
    const double lip = d.h - d.T / std::cos(d.theta());
    if (d.lance) {
        if (lip <= Precision::Confusion()) {
            THROWM(Base::ValueError, "A lance that shallow does not clear the sheet");
        }
    }
    else {
        if (d.l / 2 * sp - d.h * cp <= Precision::Confusion()) {
            FC_THROWM(Base::ValueError,
                      "A louver " << d.l << " mm long cannot rise " << d.h
                                  << " mm at its ends' slant: they meet first");
        }
        if (lip <= Precision::Confusion()
            || d.l / 2 * sp - lip * cp - d.T <= Precision::Confusion()) {
            THROWM(Base::ValueError, "A louver that small has no room inside for the thickness");
        }
    }
    const double size = 4 * (d.l + d.w + d.h + d.T) + 1;
    const double m = d.l + d.w + d.h + d.T;
    // a lance's ends square, the box's: only the ramp's plane
    const double ends = d.lance ? 0 : m;
    auto under = [&](double in, const TopoDS_Shape& box) {
        TopoDS_Shape res = box;
        const auto planes = d.planes(in);
        for (std::size_t k = 0; k < (d.lance ? 1 : planes.size()); ++k) {
            const auto& [p, n] = planes[k];
            BRepAlgoAPI_Common common(res, behind(p, n, size));
            common.Build();
            if (!common.IsDone()) {
                THROWM(Base::CADKernelError, "The louver could not be made");
            }
            res = common.Shape();
        }
        return res;
    };
    // Its outside from inside the wall, which holds all of it below the
    // face: from the other skin, its foot there would split that skin, a
    // sliver of it left beside the hollow neither the wall's nor the form's.
    // The hollow from below the wall, out behind the hinge to where the
    // lowered ramp leaves the wall, and up to the front, where the sheet was
    // cut.
    const TopoDS_Shape outside =
        under(0,
              BRepPrimAPI_MakeBox(gp_Pnt(-d.l / 2 - ends, -d.w / 2, -d.T / 2),
                                  gp_Pnt(d.l / 2 + ends, d.w / 2, d.h + m))
                  .Shape());
    const TopoDS_Shape hollow = under(
        d.T,
        BRepPrimAPI_MakeBox(gp_Pnt(-d.l / 2 - (d.lance ? d.gap : m), -d.w / 2 - m, -2 * d.T),
                            gp_Pnt(d.l / 2 + (d.lance ? d.gap : m), d.w / 2, d.h + m))
            .Shape());
    gp_Trsf place;
    place.SetDisplacement(gp_Ax3(gp::XOY()), frame);
    Pressed res;
    res.outside = BRepBuilderAPI_Transform(outside, place, true).Shape();
    res.cuts.push_back(BRepBuilderAPI_Transform(hollow, place, true).Shape());
    const TopoDS_Shape slit = BRepPrimAPI_MakeBox(gp_Pnt(-d.l / 2 - d.reach(), d.w / 2, -2 * d.T),
                                                  gp_Pnt(d.l / 2 + d.reach(), d.w / 2 + d.gap, d.T))
                                  .Shape();
    res.cuts.push_back(BRepBuilderAPI_Transform(slit, place, true).Shape());
    double back = d.behindHinge();
    if (d.lance) {
        // a lance's sides cut free, through the wall, from behind its
        // hinge's bend to the slit before it
        back += d.gap;
        for (double side : {-1.0, 1.0}) {
            const double x0 = side * d.l / 2, x1 = side * (d.l / 2 + d.gap);
            const TopoDS_Shape cut =
                BRepPrimAPI_MakeBox(gp_Pnt(std::min(x0, x1), -d.w / 2 - back, -2 * d.T),
                                    gp_Pnt(std::max(x0, x1), d.w / 2 + d.gap, d.T))
                    .Shape();
            res.cuts.push_back(BRepBuilderAPI_Transform(cut, place, true).Shape());
        }
    }
    // as far as its bends reach on either skin, each rho tan(half its angle)
    // past where it leaves the face, and the slits
    res.form.halfLength = d.l / 2 + d.reach() + 10 * Precision::Confusion();
    res.form.halfWidth = d.w / 2 + std::max(back, d.gap) + 10 * Precision::Confusion();
    res.form.zMin = -d.T;
    res.form.zMax = d.h;
    return res;
}

/// The louver's bends rounded, added to fillet; how many. In two steps:
/// first the ridges between the ramp and the ends, concentric, Radius
/// inside and Radius and the thickness outside; then where the ramp and the
/// ends leave the face, on it and beneath, all of Radius. OCCT blends no
/// corner beneath where the hinge's and an end's bends, Radius and the
/// thickness, meet the inner ridge, Radius, so those bends are a little
/// thicker than the sheet through them (T (1 / cos(a / 2) - 1) more, a the
/// bend's angle: 8% at 45 deg).
int roundLouver(BRepFilletAPI_MakeFillet& fillet,
                const TopoDS_Shape& shape,
                const Louver& d,
                const gp_Ax3& frame,
                double halfLength,
                double halfWidth,
                int step)
{
    if (d.rho <= 0) {
        return 0;
    }
    gp_Trsf toFrame;
    toFrame.SetTransformation(frame);
    const auto outer = d.planes(0), inner = d.planes(d.T);
    const double tol = 1e-6;
    auto on = [&](const gp_Pnt& q, const std::pair<gp_Pnt, gp_Dir>& plane) {
        return std::abs(gp_Vec(plane.first, q).Dot(gp_Vec(plane.second))) < tol;
    };
    int count = 0;
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
        BRepAdaptor_Curve c(edge);
        if (c.GetType() != GeomAbs_Line) {
            continue;
        }
        const gp_Pnt a = c.Value(c.FirstParameter()).Transformed(toFrame);
        const gp_Pnt b = c.Value(c.LastParameter()).Transformed(toFrame);
        bool inside = true;
        for (const gp_Pnt& q : {a, b}) {
            inside = inside && std::abs(q.X()) <= halfLength && std::abs(q.Y()) <= halfWidth
                && q.Z() >= -d.T - tol && q.Z() <= d.h + tol;
        }
        // the lip's edges and the cut's, at the front, are not bends
        if (!inside || (std::abs(a.Y() - d.w / 2) < tol && std::abs(b.Y() - d.w / 2) < tol)) {
            continue;
        }
        auto both = [&](auto test) {
            return test(a) && test(b);
        };
        // the whole edge on the one plane
        auto lies = [&](const std::pair<gp_Pnt, gp_Dir>& plane) {
            return on(a, plane) && on(b, plane);
        };
        // on one end's plane, the +x end's or the -x end's
        auto onEnd = [&](const std::array<std::pair<gp_Pnt, gp_Dir>, 3>& planes) {
            return lies(planes[1]) || lies(planes[2]);
        };
        const bool face = both([](const gp_Pnt& q) {
            return std::abs(q.Z()) < 1e-6;
        });
        const bool beneath = both([&](const gp_Pnt& q) {
            return std::abs(q.Z() + d.T) < 1e-6;
        });
        double radius = 0;
        if (d.lance) {
            // its hinge alone, concentric: its ends on the side slits' faces
            if (step == 0 && face && lies(outer[0])) {
                radius = d.rho;
            }
            else if (step == 0 && beneath && lies(inner[0])) {
                radius = d.rho + d.T;
            }
        }
        else if (step == 0) {
            if (lies(outer[0]) && onEnd(outer)) {
                radius = d.rho + d.T;
            }
            else if (lies(inner[0]) && onEnd(inner)) {
                radius = d.rho;
            }
        }
        else if ((face && (lies(outer[0]) || onEnd(outer)))
                 || (beneath && (lies(inner[0]) || onEnd(inner)))) {
            radius = d.rho;
        }
        if (radius > 0) {
            fillet.Add(radius, edge);
            ++count;
        }
    }
    return count;
}

/// Points around what a form takes of the part, at the middle of the wall
std::vector<gp_Pnt> around(const Form& form, double T)
{
    std::vector<std::pair<double, double>> xy;
    if (form.halfWidth > 0) {
        for (double x : {-1.0, 0.0, 1.0}) {
            for (double y : {-1.0, 0.0, 1.0}) {
                xy.emplace_back(x * form.halfLength, y * form.halfWidth);
            }
        }
    }
    else {
        const int n = 16;
        for (int k = 0; k < n; ++k) {
            const double a = 2 * std::numbers::pi * k / n;
            xy.emplace_back(form.halfLength * std::cos(a), form.halfLength * std::sin(a));
        }
    }
    std::vector<gp_Pnt> res;
    const gp_Ax3& f = form.frame;
    for (const auto& [x, y] : xy) {
        res.push_back(f.Location().Translated(gp_Vec(f.XDirection()) * x
                                              + gp_Vec(f.YDirection()) * y
                                              + gp_Vec(f.Direction()) * (-T / 2)));
    }
    return res;
}

}  // namespace

SheetMetalForm::SheetMetalForm()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(Kind,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Tool: Tool's shape, set on its face, as the form's outside; Dimple, "
                      "Louver, Bead, Lance, DrawnCutout, FlangedHole: a built-in one");
    Kind.setEnums(KindEnums);
    ADD_PROPERTY_TYPE(Face,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "The wall's skin the forms rise out of");
    ADD_PROPERTY_TYPE(Positions,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A sketch whose points are where the forms are");
    ADD_PROPERTY_TYPE(Angle,
                      (0.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "Each form turned about the face's normal, from the sketch's X");
    ADD_PROPERTY_TYPE(Reversed,
                      (false),
                      "Sheet Metal",
                      App::Prop_None,
                      "Rising into the face, out of the wall's other skin");
    ADD_PROPERTY_TYPE(Tool,
                      (nullptr),
                      "Form",
                      App::Prop_None,
                      "Kind Tool: the tool, and its face that sits on the wall's skin");
    ADD_PROPERTY_TYPE(Diameter,
                      (10.0),
                      "Form",
                      App::Prop_None,
                      "Kind Dimple, Bead, DrawnCutout: across where it leaves the face; "
                      "FlangedHole: the hole, inside its collar");
    ADD_PROPERTY_TYPE(Depth,
                      (2.0),
                      "Form",
                      App::Prop_None,
                      "Kind Dimple, Louver, Bead, Lance, DrawnCutout, FlangedHole: its top "
                      "above the face");
    ADD_PROPERTY_TYPE(Radius,
                      (1.0),
                      "Form",
                      App::Prop_None,
                      "Kind Dimple, Louver, Bead, Lance, DrawnCutout, FlangedHole: the inside "
                      "radius of its bends");
    ADD_PROPERTY_TYPE(FlankAngle,
                      (45.0),
                      "Form",
                      App::Prop_None,
                      "Kind Dimple, Bead, DrawnCutout: the slant of its side from the face, up "
                      "to 90 deg; Louver: its ends'");
    FlankAngle.setConstraints(&flankRange);
    ADD_PROPERTY_TYPE(Length,
                      (20.0),
                      "Form",
                      App::Prop_None,
                      "Kind Louver, Lance: along the form's X; Bead, DrawnCutout: along it end "
                      "to end, where it leaves the face");
    ADD_PROPERTY_TYPE(Width,
                      (6.0),
                      "Form",
                      App::Prop_None,
                      "Kind Louver, Lance: from its hinge to its opening, which faces the "
                      "form's Y");
    onChanged(&Kind);
}

short SheetMetalForm::mustExecute() const
{
    if (Kind.isTouched() || Face.isTouched() || Positions.isTouched() || Angle.isTouched()
        || Reversed.isTouched() || Tool.isTouched() || Diameter.isTouched() || Depth.isTouched()
        || Radius.isTouched() || FlankAngle.isTouched() || Length.isTouched()
        || Width.isTouched()) {
        return 1;
    }
    return PartDesign::Feature::mustExecute();
}

void SheetMetalForm::onChanged(const App::Property* prop)
{
    // what the other kinds take, greyed out
    if (prop == &Kind) {
        const int kind = Kind.getValue();
        Tool.setStatus(App::Property::ReadOnly, kind != 0);
        // by kind: Tool 0, Dimple 1, Louver 2, Bead 3, Lance 4, DrawnCutout 5,
        // FlangedHole 6
        auto in = [kind](std::initializer_list<int> kinds) {
            return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
        };
        Diameter.setStatus(App::Property::ReadOnly, !in({1, 3, 5, 6}));
        for (App::Property* p : std::initializer_list<App::Property*> {&Depth, &Radius}) {
            p->setStatus(App::Property::ReadOnly, kind == 0);
        }
        FlankAngle.setStatus(App::Property::ReadOnly, !in({1, 2, 3, 5}));
        Length.setStatus(App::Property::ReadOnly, !in({2, 3, 4, 5}));
        Width.setStatus(App::Property::ReadOnly, !in({2, 4}));
    }
    // The built-in sizes start from the sheet's thickness, once, when the
    // feature takes its rule (ruled 2026-11-06); any set by then are kept
    if (isRestoring()) {
        sized = true;
    }
    else if (prop == &Rule && !sized) {
        if (auto rule = getRule()) {
            sized = true;
            const double T = rule->Thickness.getValue();
            const std::pair<App::PropertyLength*, double> sizes[] = {
                {&Diameter, 10}, {&Depth, 2}, {&Radius, 1}, {&Length, 20}, {&Width, 6}};
            for (const auto& [p, k] : sizes) {
                if (p->getValue() == k) {
                    p->setValue(k * T);
                }
            }
        }
    }
    // The face is of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Face.getValue()
        && Face.getValue() != BaseFeature.getValue()) {
        auto subs = Face.getSubValues(false);
        auto shadows = Face.getShadowSubs();
        Face.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    PartDesign::Feature::onChanged(prop);
}

App::DocumentObjectExecReturn* SheetMetalForm::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        Model model = modelBefore(rule->Thickness.getValue());
        if (model.elements.empty()) {
            THROWM(Base::ValueError, "A form builds on a sheet metal feature");
        }
        const double T = model.thickness;
        const TopoShape base = getBaseShape();

        // the wall's skin
        const auto shadows = Face.getShadowSubs();
        if (shadows.size() != 1) {
            THROWM(Base::ValueError, "A form takes one face: a wall's skin it rises out of");
        }
        const std::string& ref =
            shadows.front().first.empty() ? shadows.front().second : shadows.front().first;
        const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
        if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE) {
            FC_THROWM(Base::ValueError, "No face " << shadows.front().second);
        }
        const TopoDS_Face face = TopoDS::Face(sub);
        const int wall = model.owner(face);
        if (wall < 0 || model.elements[wall].kind != Element::Kind::Wall
            || BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane) {
            FC_THROWM(Base::ValueError,
                      "Face " << shadows.front().second
                              << " is no wall's skin: a form goes on a wall");
        }
        const gp_Dir normal = outerNormal(face);
        const gp_Pln plane = BRepAdaptor_Surface(face).Plane();
        const gp_Dir rise = Reversed.getValue() ? normal.Reversed() : normal;

        // where: the sketch's points, its X turned by Angle for the forms'
        auto sketch = Base::freecad_dynamic_cast<Part::Feature>(Positions.getValue());
        if (!sketch) {
            THROWM(Base::ValueError, "A form takes a sketch of its points: Positions");
        }
        std::vector<gp_Pnt> points;
        for (TopExp_Explorer it(Part::Feature::getTopoShape(sketch).getShape(),
                                TopAbs_VERTEX,
                                TopAbs_EDGE);
             it.More();
             it.Next()) {
            points.push_back(BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
        }
        if (points.empty()) {
            FC_THROWM(Base::ValueError, sketch->getFullLabel() << " has no point for a form");
        }
        Base::Vector3d sx(1, 0, 0);
        sketch->Placement.getValue().getRotation().multVec(sx, sx);
        gp_Dir xDir;
        if (!across(gp_Vec(sx.x, sx.y, sx.z), rise, xDir)) {
            xDir = plane.Position().XDirection();
        }
        xDir.Rotate(gp_Ax1(gp_Pnt(), rise), Base::toRadians(Angle.getValue()));

        // the tool, Kind Tool
        TopoDS_Shape tool;
        TopoDS_Face toolBase;
        gp_Dir toolX(1, 0, 0);
        const bool isTool = Kind.getValue() == 0;
        if (isTool) {
            auto obj = Tool.getValue();
            const auto subs = Tool.getSubValues(true);
            if (!obj || subs.size() != 1) {
                THROWM(Base::ValueError, "A form of Kind Tool takes a tool and its face");
            }
            const TopoShape toolShape = Part::Feature::getTopoShape(obj);
            TopExp_Explorer solid(toolShape.getShape(), TopAbs_SOLID);
            const TopoDS_Shape baseSub = toolShape.getSubShape(subs.front().c_str(), true);
            if (!solid.More() || baseSub.IsNull() || baseSub.ShapeType() != TopAbs_FACE
                || BRepAdaptor_Surface(TopoDS::Face(baseSub)).GetType() != GeomAbs_Plane) {
                FC_THROWM(Base::ValueError,
                          "The tool " << obj->getFullLabel()
                                      << " takes a solid and a plane face of it");
            }
            tool = solid.Current();
            toolBase = TopoDS::Face(baseSub);
            if (auto feature = Base::freecad_dynamic_cast<App::GeoFeature>(obj)) {
                Base::Vector3d tx(1, 0, 0);
                feature->Placement.getValue().getRotation().multVec(tx, tx);
                toolX = gp_Dir(tx.x, tx.y, tx.z);
            }
        }

        std::vector<TopoShape> adds;
        std::vector<TopoShape> cuts;
        const auto hasher = getDocument()->getStringHasher();
        // each built-in form's bends, rounded once they are all in the part,
        // in steps: the form's bends of each step added to that step's
        using Round = std::function<int(BRepFilletAPI_MakeFillet&, const TopoDS_Shape&, int)>;
        std::vector<Round> rounds;
        const int steps = 2;
        for (const gp_Pnt& p : points) {
            // onto the face, and in it
            const double off = gp_Vec(plane.Location(), p).Dot(gp_Vec(plane.Axis().Direction()));
            const gp_Pnt on = p.Translated(gp_Vec(plane.Axis().Direction()) * -off);
            BRepClass_FaceClassifier inside(face, on, 1e-6);
            if (inside.State() != TopAbs_IN) {
                FC_THROWM(Base::ValueError,
                          "A point of " << sketch->getFullLabel() << " at (" << p.X() << ", "
                                        << p.Y() << ", " << p.Z() << ") is off the face");
            }
            const gp_Pnt origin =
                Reversed.getValue() ? on.Translated(gp_Vec(normal) * -T) : on;
            const gp_Ax3 frame(origin, rise, xDir);
            Pressed pressed;
            if (isTool) {
                pressed = pressTool(tool, toolBase, toolX, frame, T);
            }
            else if (const int kind = Kind.getValue(); kind == 1 || kind == 3 || kind >= 5) {
                Dimple d {Diameter.getValue() / 2,
                          Depth.getValue(),
                          Base::toRadians(FlankAngle.getValue()),
                          Radius.getValue(),
                          T};
                if (kind == 3) {
                    if (Length.getValue() <= Diameter.getValue() + Precision::Confusion()) {
                        THROWM(Base::ValueError,
                               "A bead is longer than it is wide: as long, it is a dimple");
                    }
                    d.s = (Length.getValue() - Diameter.getValue()) / 2;
                    d.what = "bead";
                }
                else if (kind == 5) {
                    if (Length.getValue() < Diameter.getValue() - Precision::Confusion()) {
                        THROWM(Base::ValueError,
                               "A drawn cutout is no shorter than it is wide: Length at least "
                               "Diameter");
                    }
                    d.s = std::max(Length.getValue() - Diameter.getValue(), 0.0) / 2;
                    d.open = true;
                    d.what = "drawn cutout";
                }
                else if (kind == 6) {
                    // a collar, the hole Diameter across inside it
                    if (Depth.getValue() <= Radius.getValue() + Precision::Confusion()) {
                        THROWM(Base::ValueError,
                               "A flanged hole's collar rises past its bend: Depth above Radius");
                    }
                    d.r0 = Diameter.getValue() > 0 ? Diameter.getValue() / 2 + T : 0;
                    d.phi = std::numbers::pi / 2;
                    d.open = true;
                    d.what = "flanged hole";
                }
                pressed = pressDimple(d, frame);
                rounds.emplace_back(
                    [d, frame](BRepFilletAPI_MakeFillet& f, const TopoDS_Shape& s, int step) {
                        return step == 0 ? roundDimple(f, s, d, frame) : 0;
                    });
            }
            else {
                const Louver d {Length.getValue(),
                                Width.getValue(),
                                Depth.getValue(),
                                Base::toRadians(FlankAngle.getValue()),
                                Radius.getValue(),
                                T,
                                rule->Gap.getValue(),
                                Kind.getValue() == 4};
                pressed = pressLouver(d, frame);
                const double hl = pressed.form.halfLength, hw = pressed.form.halfWidth;
                rounds.emplace_back([d, frame, hl, hw](BRepFilletAPI_MakeFillet& f,
                                                       const TopoDS_Shape& s,
                                                       int step) {
                    return roundLouver(f, s, d, frame, hl, hw, step);
                });
            }
            // all of it on the wall: short of its bends and its limits
            Form& form = pressed.form;
            form.frame = frame;
            for (const gp_Pnt& q : around(form, T)) {
                if (!model.contains(wall, q, 1e-6)) {
                    FC_THROWM(Base::ValueError,
                              "A form at (" << p.X() << ", " << p.Y() << ", " << p.Z()
                                            << ") runs off " << model.elements[wall].name);
                }
            }
            form.name = "Form" + std::to_string(model.forms.size() + 1);
            form.wall = wall;
            model.forms.push_back(form);
            adds.emplace_back(pressed.outside);
            for (const TopoDS_Shape& cut : pressed.cuts) {
                cuts.emplace_back(cut);
            }
        }

        std::vector<TopoShape> fuse {base};
        fuse.insert(fuse.end(), adds.begin(), adds.end());
        TopoShape fused(0, hasher);
        fused.makEBoolean(Part::OpCodes::Fuse, fuse);
        std::vector<TopoShape> cut {fused};
        cut.insert(cut.end(), cuts.begin(), cuts.end());
        TopoShape res(0, hasher);
        res.makEBoolean(Part::OpCodes::Cut, cut);
        if (res.countSubShapes(TopAbs_SOLID) != 1) {
            THROWM(Base::ValueError, "The forms left the sheet in pieces");
        }
        res = getSolid(res);
        for (int step = 0; step < steps && !rounds.empty(); ++step) {
            BRepFilletAPI_MakeFillet fillet(res.getShape());
            int count = 0;
            for (const auto& round : rounds) {
                count += round(fillet, res.getShape(), step);
            }
            if (count > 0) {
                try {
                    fillet.Build();
                }
                catch (const Standard_Failure&) {
                }
                if (!fillet.IsDone()) {
                    std::string kind = Kind.getValueAsString();
                    kind[0] = static_cast<char>(std::tolower(kind[0]));
                    FC_THROWM(Base::ValueError,
                              "The " << kind
                                     << "'s bends cannot be rounded that much: a smaller "
                                        "Radius, or a larger form");
                }
                TopoShape rounded(0, hasher);
                rounded.makESHAPE(fillet.Shape(), Part::MapperMaker(fillet), {res},
                                  Part::OpCodes::Fillet);
                res = getSolid(rounded);
            }
        }
        Shape.setValue(res);
        SheetModel.setValue(model);
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}
