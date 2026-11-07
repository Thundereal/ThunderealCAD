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
#include <functional>
#include <numbers>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>

#include "FeatureSheetMetalLoftedFlange.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"
#include "SheetMetal/Refold.h"

using namespace PartDesign;
using Part::TopoShape;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalLoftedFlange, PartDesign::ProfileBased)

const char* SheetMetalLoftedFlange::MaterialSideEnums[] = {"Outside", "Inside", "Middle", nullptr};

SheetMetalLoftedFlange::SheetMetalLoftedFlange()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(Section, (nullptr), "Sheet Metal", App::Prop_None,
                      "The other outline the sheet runs to");
    ADD_PROPERTY_TYPE(MaterialSide,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Where the material is against the sketches: Outside, the sketches on the "
                      "inner skin, the one facing the axis; Inside, on the outer skin; Middle");
    MaterialSide.setEnums(MaterialSideEnums);
    ADD_PROPERTY_TYPE(UseRuleRadius,
                      (true),
                      "Sheet Metal",
                      App::Prop_None,
                      "Lines to lines: bend at the rule's radius; off, at BendRadius");
    ADD_PROPERTY_TYPE(BendRadius,
                      (1.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "The bends' inner radius, when not the rule's");
    BendRadius.setStatus(App::Property::ReadOnly, true);
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
    for (App::Property* prop : std::initializer_list<App::Property*> {&Reversed,
                                                                      &Midplane,
                                                                      &UpToFace,
                                                                      &UpToShape,
                                                                      &AllowMultiFace,
                                                                      &Fit,
                                                                      &FitJoin,
                                                                      &InnerFit,
                                                                      &InnerFitJoin,
                                                                      &Linearize}) {
        prop->setStatus(App::Property::Hidden, true);
    }
}

short SheetMetalLoftedFlange::mustExecute() const
{
    if (Section.isTouched() || MaterialSide.isTouched() || UseRuleRadius.isTouched()
        || BendRadius.isTouched()) {
        return 1;
    }
    return ProfileBased::mustExecute();
}

void SheetMetalLoftedFlange::onChanged(const App::Property* prop)
{
    if (prop == &UseRuleRadius) {
        radiusSwitched(UseRuleRadius, BendRadius);
    }
    ProfileBased::onChanged(prop);
}

namespace
{

constexpr double angular = 1e-7;

/// A sketch's one circle or arc
struct Outline
{
    gp_Circ circle;
    bool closed = false;
    /// an arc's ends, in the order it turns about the circle's axis
    gp_Pnt start, end;
    double sweep = 0;
};

Outline roundOf(const TopoShape& shape, const char* what)
{
    const auto edges = shape.getSubTopoShapes(TopAbs_EDGE);
    if (edges.size() != 1) {
        FC_THROWM(Base::ValueError,
                  "A lofted flange takes one circle or arc, or a chain of lines, in each "
                  "sketch: the " << what
                      << " has " << edges.size() << " edges");
    }
    const TopoDS_Edge& edge = TopoDS::Edge(edges.front().getShape());
    BRepAdaptor_Curve c(edge);
    if (c.GetType() != GeomAbs_Circle) {
        FC_THROWM(Base::ValueError,
                  "A lofted flange takes one circle or arc, or a chain of lines, in each "
                  "sketch: the " << what
                      << " is not round");
    }
    Outline res;
    res.circle = c.Circle();
    const double f = c.FirstParameter(), l = c.LastParameter();
    res.sweep = l - f;
    res.closed = BRep_Tool::IsClosed(edge) || res.sweep >= 2 * std::numbers::pi - angular;
    res.start = c.Value(f);
    res.end = c.Value(l);
    return res;
}

/// The angle of a point about the axis, from x
double angleOf(const gp_Ax3& frame, const gp_Pnt& point)
{
    const gp_Vec v(frame.Location(), point);
    return std::atan2(v.Dot(gp_Vec(frame.YDirection())), v.Dot(gp_Vec(frame.XDirection())));
}

/// a's way, made square to n
gp_Dir across(const gp_Vec& a, const gp_Dir& n)
{
    return gp_Dir(a - gp_Vec(n) * a.Dot(gp_Vec(n)));
}

/// Whether the sketch is straight lines only
bool allLines(const TopoShape& shape)
{
    const auto edges = shape.getSubTopoShapes(TopAbs_EDGE);
    if (edges.empty()) {
        return false;
    }
    for (const auto& e : edges) {
        if (BRepAdaptor_Curve(TopoDS::Edge(e.getShape())).GetType() != GeomAbs_Line) {
            return false;
        }
    }
    return true;
}

/// A sketch's chain of lines: its corners in order, a closed one's from
/// where its first line (Edge1) starts
struct Chain
{
    std::vector<gp_Pnt> points;
    bool closed = false;

    std::size_t lines() const
    {
        return closed ? points.size() : points.size() - 1;
    }
    const gp_Pnt& at(std::size_t i) const
    {
        return points[i % points.size()];
    }
    /// line i's way
    gp_Dir way(std::size_t i) const
    {
        return gp_Dir(gp_Vec(at(i), at(i + 1)));
    }
};

Chain chainOf(const TopoShape& shape, const char* what)
{
    auto wires = shape.getSubTopoShapes(TopAbs_WIRE);
    if (wires.empty()) {
        wires = shape.makEWires().getSubTopoShapes(TopAbs_WIRE);
    }
    if (wires.size() != 1) {
        FC_THROWM(Base::ValueError,
                  "A lofted flange takes one chain of lines in each sketch: the "
                      << what << " has " << wires.size());
    }
    const TopoDS_Wire wire = TopoDS::Wire(wires.front().getShape());
    const TopoDS_Shape first = shape.getSubShape(TopAbs_EDGE, 1);
    Chain res;
    res.closed = BRep_Tool::IsClosed(wire);
    std::size_t start = 0;
    TopoDS_Edge last;
    for (BRepTools_WireExplorer it(wire); it.More(); it.Next()) {
        if (it.Current().IsSame(first)) {
            start = res.points.size();
        }
        last = it.Current();
        res.points.push_back(BRep_Tool::Pnt(TopExp::FirstVertex(last, true)));
    }
    if (res.closed) {
        std::rotate(res.points.begin(),
                    res.points.begin() + static_cast<long>(start),
                    res.points.end());
    }
    else {
        res.points.push_back(BRep_Tool::Pnt(TopExp::LastVertex(last, true)));
    }
    // no corner that goes on straight
    auto& p = res.points;
    for (std::size_t i = res.closed ? 0 : 1; p.size() > 2 && i < p.size() - (res.closed ? 0 : 1);) {
        const gp_Vec a(p[(i + p.size() - 1) % p.size()], p[i]), b(p[i], p[(i + 1) % p.size()]);
        if (a.IsParallel(b, angular) && a.Dot(b) > 0) {
            p.erase(p.begin() + static_cast<long>(i));
        }
        else {
            ++i;
        }
    }
    if (res.lines() < (res.closed ? 3 : 1)) {
        FC_THROWM(Base::ValueError, "The lofted flange's " << what << " has too few lines");
    }
    return res;
}

/// The area a chain closes, about the axis: above 0 where it goes round
/// counterclockwise
double areaAbout(const std::vector<gp_Pnt>& p, const gp_Dir& axis)
{
    gp_Vec sum;
    for (std::size_t i = 0; i < p.size(); ++i) {
        sum += gp_Vec(p[i].XYZ()).Crossed(gp_Vec(p[(i + 1) % p.size()].XYZ()));
    }
    return sum.Dot(gp_Vec(axis)) / 2;
}

struct Line
{
    gp_Pnt p;
    gp_Dir d;
};

/// Where two lines in one plane cross
gp_Pnt crossing(const Line& a, const Line& b)
{
    const gp_Vec n = gp_Vec(a.d).Crossed(gp_Vec(b.d));
    const double s = gp_Vec(a.p, b.p).Crossed(gp_Vec(b.d)).Dot(n) / n.SquareMagnitude();
    return a.p.Translated(gp_Vec(a.d) * s);
}

/** Lines to lines: a flat facet, a wall, between each line of the profile
 * and the section's in its place, and a bend where two facets meet. Each
 * facet is its outline on the inner skin through the sheet; each bend its
 * strip in its own flat taken onto its cylinder, its rims there straight
 * from where the facets' rims meet it -- the ends square to the sheet
 * everywhere, so the flat is exact.
 */
struct LinesLoft
{
    double thickness = 0;
    double radius = 0;
    double gap = 0;
    int side = 0;
    std::function<double(double, double)> k;
    int walls = 0;
    int bends = 0;

    void build(Chain a, Chain b, const gp_Dir& normal, SheetMetal::Model& model,
               std::vector<TopoShape>& pieces, const App::StringHasherRef& hasher) const;
};

void LinesLoft::build(Chain a, Chain b, const gp_Dir& normal, SheetMetal::Model& model,
                      std::vector<TopoShape>& pieces, const App::StringHasherRef& hasher) const
{
    const double tol = Precision::Confusion();
    const double T = thickness;
    const gp_Pnt& origin = a.points.front();
    const double height = gp_Vec(origin, b.points.front()).Dot(gp_Vec(normal));
    for (const gp_Pnt& p : b.points) {
        if (std::abs(gp_Vec(origin, p).Dot(gp_Vec(normal)) - height) > tol) {
            THROWM(Base::ValueError, "The lofted flange's two outlines are not on parallel planes");
        }
    }
    if (std::abs(height) < tol) {
        THROWM(Base::ValueError, "The lofted flange's two outlines are on one plane");
    }
    const gp_Dir axis = height > 0 ? normal : normal.Reversed();
    if (a.closed != b.closed) {
        THROWM(Base::ValueError, "The lofted flange's two outlines are one closed, the other open");
    }
    const std::size_t n = a.lines();
    if (b.lines() != n) {
        FC_THROWM(Base::ValueError,
                  "The lofted flange's two outlines have " << n << " and " << b.lines()
                                                           << " lines: they take as many");
    }

    // the material's way out of the chain: a closed one's, turned
    // counterclockwise about the axis, out of it; an open one's away from
    // where it turns, or off its right for a single line
    double outward = 1;
    if (a.closed) {
        for (Chain* c : {&a, &b}) {
            if (areaAbout(c->points, axis) < 0) {
                // the other way round, its first line first still
                std::reverse(c->points.begin(), c->points.end());
                std::rotate(c->points.begin(), c->points.end() - 2, c->points.end());
            }
        }
        // the section's line running the profile's first one's way first
        std::size_t best = 0;
        for (std::size_t j = 1; j < n; ++j) {
            if (b.way(j).Dot(a.way(0)) > b.way(best).Dot(a.way(0))) {
                best = j;
            }
        }
        std::rotate(b.points.begin(), b.points.begin() + static_cast<long>(best), b.points.end());
    }
    else {
        outward = areaAbout(a.points, axis) < -tol ? -1 : 1;
        const gp_Dir back(gp_Vec(b.points.back(), b.points[b.points.size() - 2]));
        if (back.Dot(a.way(0)) > b.way(0).Dot(a.way(0))) {
            std::reverse(b.points.begin(), b.points.end());
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!a.way(i).IsParallel(b.way(i), angular) || a.way(i).Dot(b.way(i)) < 0) {
            FC_THROWM(Base::ValueError,
                      "Line " << i + 1
                              << " of the lofted flange's profile and the section's in its "
                                 "place do not run one way: lofting those is the next step");
        }
    }

    // each facet's way along its lines and its normal out, from its inner
    // skin, which is the sketches' less the material's side
    const double off = side == 1 ? T : side == 2 ? T / 2 : 0;
    std::vector<gp_Dir> way(n), out(n);
    for (std::size_t i = 0; i < n; ++i) {
        way[i] = a.way(i);
        out[i] = gp_Dir(gp_Vec(way[i]).Crossed(gp_Vec(a.at(i), b.at(i))) * outward);
    }
    auto onInner = [&](const gp_Pnt& p, std::size_t i) {
        return p.Translated(gp_Vec(out[i]) * -off);
    };

    // where each facet starts and ends on its inner skin, and the bends
    std::vector<Line> start(n), end(n);
    struct Corner
    {
        SheetMetal::CylBend bend;
        bool convex = true;
        std::size_t at = 0;
    };
    std::vector<Corner> corners;
    for (std::size_t j = 1; j < (a.closed ? n + 1 : n); ++j) {
        const std::size_t c = j % n, ia = (c + n - 1) % n, ib = c;
        const gp_Dir fold(gp_Vec(a.at(c), b.at(c)));
        auto inFacet = [&](const gp_Dir& e) {
            return gp_Dir(gp_Vec(e) - gp_Vec(fold) * e.Dot(fold));
        };
        const gp_Dir ta = inFacet(way[ia]), tb = inFacet(way[ib]);
        const double turn = std::atan2(gp_Vec(ta).Crossed(gp_Vec(tb)).Magnitude(), ta.Dot(tb));
        if (turn > std::numbers::pi - 1e-6) {
            THROWM(Base::ValueError, "The lofted flange cannot turn back on itself");
        }
        Corner corner;
        corner.at = c;
        // turning in, its inner skin the facets'; out, their outer one
        corner.convex = tb.Dot(out[ia]) < 0;
        const gp_Vec na(out[ia]), nb(out[ib]);
        const double skin = corner.convex ? -off : T - off;
        const gp_Vec ua = corner.convex ? -na : na, ub = corner.convex ? -nb : nb;
        const gp_Pnt sharp = a.at(c).Translated((na + nb) * (skin / (1 + na.Dot(nb))));
        const gp_Pnt centre = sharp.Translated((ua + ub) * (radius / (1 + ua.Dot(ub))));
        SheetMetal::CylBend& bend = corner.bend;
        bend.frame = gp_Ax3(centre, gp_Dir((-ua).Crossed(gp_Vec(ta))), gp_Dir(-ua));
        bend.radius = radius;
        bend.thickness = T;
        bend.angle = turn;
        bend.k = k(turn, radius);
        const gp_Pnt endA = centre.Translated(ua * -radius);
        const gp_Pnt startB = centre.Translated(ub * -radius);
        end[ia] = {corner.convex ? endA : endA.Translated(na * -T), fold};
        start[ib] = {corner.convex ? startB : startB.Translated(nb * -T), fold};
        corners.push_back(corner);
    }

    // the walls: a closed chain's first facet in two, either side of the
    // seam, the first wall its second half and the last its first
    struct Wall
    {
        std::size_t facet;
        Line from, to;
        bool seamFrom = false, seamTo = false;
        int index = -1;
        gp_Pnt c[4];
    };
    std::vector<Wall> list;
    if (a.closed) {
        const gp_Pnt mp = onInner(a.at(0).Translated(gp_Vec(a.at(0), a.at(1)) / 2), 0);
        const gp_Pnt ms = onInner(b.at(0).Translated(gp_Vec(b.at(0), b.at(1)) / 2), 0);
        const gp_Dir g(gp_Vec(mp, ms));
        const gp_Dir w = across(gp_Vec(way[0]), g);
        const Line seamFrom {mp.Translated(gp_Vec(w) * (gap / 2)), g};
        const Line seamTo {mp.Translated(gp_Vec(w) * (-gap / 2)), g};
        list.push_back({0, seamFrom, end[0], true, false});
        for (std::size_t i = 1; i < n; ++i) {
            list.push_back({i, start[i], end[i]});
        }
        list.push_back({0, start[0], seamTo, false, true});
    }
    else {
        start[0] = {onInner(a.at(0), 0), gp_Dir(gp_Vec(a.at(0), b.at(0)))};
        end[n - 1] = {onInner(a.at(n), n - 1), gp_Dir(gp_Vec(a.at(n), b.at(n)))};
        for (std::size_t i = 0; i < n; ++i) {
            list.push_back({i, start[i], end[i]});
        }
    }
    for (std::size_t j = 0; j < list.size(); ++j) {
        Wall& wall = list[j];
        const std::size_t i = wall.facet;
        const Line rim1 {onInner(a.at(i), i), way[i]}, rim2 {onInner(b.at(i), i), way[i]};
        wall.c[0] = crossing(rim1, wall.from);
        wall.c[1] = crossing(rim1, wall.to);
        wall.c[2] = crossing(rim2, wall.to);
        wall.c[3] = crossing(rim2, wall.from);
        if (gp_Vec(wall.c[0], wall.c[1]).Dot(gp_Vec(way[i])) < tol
            || gp_Vec(wall.c[3], wall.c[2]).Dot(gp_Vec(way[i])) < tol) {
            FC_THROWM(Base::ValueError,
                      "Line " << i + 1 << " of the lofted flange is too short for its bends");
        }
        const std::string name = "Wall" + std::to_string(walls + 1 + static_cast<int>(j));
        wall.index = model.addWall(name, gp_Ax3(wall.c[0], out[i], way[i]));
        TopoShape face = SheetMetal::namedFace(
            {{SheetMetal::lineEdge(wall.c[0], wall.c[1]), name + "Rim1"},
             {SheetMetal::lineEdge(wall.c[1], wall.c[2]), name + (wall.seamTo ? "Seam" : "End")},
             {SheetMetal::lineEdge(wall.c[2], wall.c[3]), name + "Rim2"},
             {SheetMetal::lineEdge(wall.c[3], wall.c[0]),
              name + (wall.seamFrom ? "Seam" : "Start")}},
            name);
        pieces.push_back(TopoShape(0, hasher).makEPrism(face, gp_Vec(out[i]) * T));
        // the seam's walls lie on one plane: each stops at its side of it
        if (wall.seamFrom) {
            model.elements[wall.index].limits.emplace_back(
                wall.from.p, across(gp_Vec(way[i]), wall.from.d).Reversed());
        }
        if (wall.seamTo) {
            model.elements[wall.index].limits.emplace_back(wall.to.p,
                                                           across(gp_Vec(way[i]), wall.to.d));
        }
    }

    // each bend's strip in its own flat, its rims straight between the
    // facets' where they meet it, on its inner skin
    for (std::size_t j = 0; j < corners.size(); ++j) {
        const Corner& corner = corners[j];
        const SheetMetal::CylBend& bend = corner.bend;
        const std::size_t before = j, after = j + 1;
        const Wall &wa = list[before], &wb = list[after];
        const gp_Vec liftA = corner.convex ? gp_Vec() : gp_Vec(out[wa.facet]) * T;
        const gp_Vec liftB = corner.convex ? gp_Vec() : gp_Vec(out[wb.facet]) * T;
        const gp_Pnt o = bend.frame.Location();
        const gp_Vec x(bend.frame.XDirection()), y(bend.frame.YDirection()),
            z(bend.frame.Direction());
        auto flat = [&](double turn, const gp_Pnt& p) {
            return o.Translated(x * radius + y * (turn * bend.neutral()) + z * gp_Vec(o, p).Dot(z));
        };
        BRepBuilderAPI_MakePolygon strip(flat(0, wa.c[1].Translated(liftA)),
                                         flat(bend.angle, wb.c[0].Translated(liftB)),
                                         flat(bend.angle, wb.c[3].Translated(liftB)),
                                         flat(0, wa.c[2].Translated(liftA)),
                                         true);
        const TopoDS_Face face = BRepBuilderAPI_MakeFace(strip.Wire(), true).Face();
        const TopoDS_Shape solid = SheetMetal::bendSolid(bend, face, radius, radius + T);
        const std::string name = "Bend" + std::to_string(bends + 1 + static_cast<int>(j));
        if (solid.IsNull()) {
            FC_THROWM(Base::RuntimeError, "The lofted flange's " << name << " could not be made");
        }
        pieces.emplace_back(solid);
        model.addBend(name, bend, wa.index, wb.index);
    }
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalLoftedFlange::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    const double thickness = rule->Thickness.getValue();
    if (thickness < Precision::Confusion()) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no thickness"));
    }
    if (!Section.getValue()) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The lofted flange has no section to run to"));
    }
    try {
        SheetMetal::Model model = modelBefore(thickness);
        model.thickness = thickness;
        int walls = 0, bends = 0;
        for (const auto& e : model.elements) {
            ++(e.kind == SheetMetal::Element::Kind::Wall ? walls : bends);
        }

        TopoShape base = getBaseShape(/*silent*/ true);
        auto invObjLoc = positionByPrevious();
        base.move(invObjLoc);
        TopoShape profile = getProfileShape();
        profile.move(invObjLoc);
        TopoShape section = Part::Feature::getTopoShape(Section.getValue());
        if (section.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "The lofted flange's section has no shape"));
        }
        section.move(invObjLoc);

        const auto hasher = getDocument()->getStringHasher();
        const bool linesA = allLines(profile), linesB = allLines(section);
        TopoShape tool;
        if (linesA || linesB) {
            if (!linesA || !linesB) {
                THROWM(Base::ValueError,
                       "The lofted flange's two outlines are one lines, the other round: "
                       "lofting those is the next step");
            }
            LinesLoft loft;
            loft.thickness = thickness;
            loft.radius = bendRadius(*rule, UseRuleRadius, BendRadius);
            if (loft.radius <= 0.0) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "The bend radius is not more than 0"));
            }
            loft.gap = rule->Gap.getValue();
            loft.side = MaterialSide.getValue();
            loft.k = [rule](double angle, double radius) {
                return rule->modelK(angle, radius);
            };
            loft.walls = walls;
            loft.bends = bends;
            const Base::Vector3d sketchNormal = getProfileNormal();
            gp_Vec normal(sketchNormal.x, sketchNormal.y, sketchNormal.z);
            normal.Transform(invObjLoc.Transformation());
            const Chain from = chainOf(profile, "profile"), to = chainOf(section, "section");
            if (from.closed && loft.gap < Precision::Confusion()) {
                THROWM(Base::ValueError,
                       "A lofted flange all round needs the rule's Gap for its seam");
            }
            std::vector<TopoShape> pieces;
            loft.build(from, to, gp_Dir(normal), model, pieces, hasher);
            tool = getSolid(TopoShape(0, hasher).makEFuse(pieces));
        }
        else {
            Outline a = roundOf(profile, "profile");
            Outline b = roundOf(section, "section");
            const double tol = Precision::Confusion();
            if (!a.circle.Axis().IsParallel(b.circle.Axis(), angular)) {
                THROWM(Base::ValueError,
                       "The lofted flange's two outlines are not on parallel planes");
            }
            if (gp_Lin(a.circle.Axis()).Distance(b.circle.Location()) > tol) {
                THROWM(Base::ValueError, "The lofted flange's two outlines are not about one axis");
            }
            if (a.closed != b.closed) {
                THROWM(Base::ValueError,
                       "The lofted flange's two outlines are one a circle, the other an arc");
            }
            // from the narrow end to the wide; the same size, the profile to the section
            if (b.circle.Radius() < a.circle.Radius() - tol) {
                std::swap(a, b);
            }
            const gp_Vec rise(a.circle.Location(), b.circle.Location());
            const double height = rise.Magnitude();
            if (height < tol) {
                THROWM(Base::ValueError, "The lofted flange's two outlines are on one plane");
            }
            const gp_Dir axis(rise);
            const double rn = a.circle.Radius(), rw = b.circle.Radius();
            const double slant = std::hypot(rw - rn, height);
            const double semiAngle = std::atan2(rw - rn, height);

            // where the sheet starts about the axis, and how far it turns
            gp_Dir start;
            double sweep;
            if (a.closed) {
                const double gap = rule->Gap.getValue();
                if (gap < tol) {
                    THROWM(Base::ValueError,
                           "A lofted flange all round needs the rule's Gap for its seam");
                }
                // the seam the gap wide on the narrow circle, about its X
                const double turn = gap / rn;
                start = across(gp_Vec(a.circle.XAxis().Direction()), axis)
                            .Rotated(gp_Ax1(a.circle.Location(), axis), turn / 2);
                sweep = 2 * std::numbers::pi - turn;
            }
            else {
                // each arc from where it starts turning about the axis
                for (Outline* r : {&a, &b}) {
                    if (r->circle.Axis().Direction().Dot(axis) < 0) {
                        std::swap(r->start, r->end);
                    }
                }
                start = across(gp_Vec(a.circle.Location(), a.start), axis);
                const gp_Ax3 frame(a.circle.Location(), axis, start);
                if (std::abs(a.sweep - b.sweep) > angular
                    || std::abs(angleOf(frame, b.start.Translated(-rise))) > angular) {
                    THROWM(Base::ValueError,
                           "The lofted flange's two arcs do not turn through the same angle");
                }
                sweep = a.sweep;
            }

            // the inner skin the material's side less of the sketches' line
            const int side = MaterialSide.getValue();
            const double off = side == 1 ? thickness : side == 2 ? thickness / 2 : 0;
            SheetMetal::ConeBend cone;
            cone.frame = gp_Ax3(
                a.circle.Location().Translated(gp_Vec(axis) * (off * std::sin(semiAngle))),
                axis,
                start);
            cone.radius = rn - off * std::cos(semiAngle);
            cone.semiAngle = semiAngle;
            cone.thickness = thickness;
            cone.angle = sweep;
            if (cone.radius < tol) {
                THROWM(Base::ValueError,
                       "The lofted flange's narrow end is too small for the sheet's thickness");
            }
            // K at the skin's curve across the cone's line, halfway along
            const double curve =
                (cone.radius + slant / 2 * std::sin(semiAngle)) / std::cos(semiAngle);
            cone.k = rule->modelK(sweep, curve);

            const std::string name = "Bend" + std::to_string(bends + 1);
            tool = TopoShape(0, hasher).makERevolve(
                SheetMetal::coneSection(cone, slant, name),
                gp_Ax1(cone.frame.Location(), axis),
                sweep);
            model.addCone(name, cone);
        }
        if (tool.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "The sheet came out empty"));
        }

        // oriented, so the join need not check its result
        tool.fixSolidOrientation();
        SheetMetal::storePCurves(tool);
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        Shape.setValue(makeBoolean(base, tool, true));
        applyBendOverrides(model);
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
