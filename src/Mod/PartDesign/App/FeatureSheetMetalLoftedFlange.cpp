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
#include <BRepGProp.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <ElCLib.hxx>
#include <GProp_GProps.hxx>
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
    ADD_PROPERTY_TYPE(Segments,
                      (24),
                      "Sheet Metal",
                      App::Prop_None,
                      "Round to lines: the chords the round is cut into, all round (an arc its "
                      "share of them)");
    static const App::PropertyIntegerConstraint::Constraints segmentRange = {3, 1000, 1};
    Segments.setConstraints(&segmentRange);
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
        || BendRadius.isTouched() || Segments.isTouched()) {
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

/// A round cut into chords: a circle's segments all round from its X, an
/// arc's share of them from its start the way it turns
Chain chainOf(const Outline& round, int segments)
{
    Chain res;
    res.closed = round.closed;
    const gp_Circ& c = round.circle;
    if (round.closed) {
        for (int i = 0; i < segments; ++i) {
            res.points.push_back(ElCLib::Value(2 * std::numbers::pi * i / segments, c));
        }
        return res;
    }
    const int count =
        std::max(1, static_cast<int>(std::ceil(segments * round.sweep / (2 * std::numbers::pi)
                                               - 1e-9)));
    const double from = ElCLib::Parameter(c, round.start);
    for (int i = 0; i <= count; ++i) {
        res.points.push_back(ElCLib::Value(from + round.sweep * i / count, c));
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

/// A convex polygon in a plane cut by a line in it through on, what lies
/// the way into it from the line kept
void clipTo(std::vector<gp_Pnt>& poly, const gp_Pnt& on, const gp_Vec& into)
{
    const double tol = Precision::Confusion();
    auto d = [&](const gp_Pnt& p) {
        return gp_Vec(on, p).Dot(into);
    };
    std::vector<gp_Pnt> res;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const gp_Pnt &p = poly[i], &q = poly[(i + 1) % poly.size()];
        const double dp = d(p), dq = d(q);
        if (dp >= 0) {
            res.push_back(p);
        }
        if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0)) {
            res.push_back(p.Translated(gp_Vec(p, q) * (dp / (dp - dq))));
        }
    }
    poly.clear();
    for (const gp_Pnt& p : res) {
        if (poly.empty() || p.Distance(poly.back()) > tol) {
            poly.push_back(p);
        }
    }
    if (poly.size() > 1 && poly.front().Distance(poly.back()) <= tol) {
        poly.pop_back();
    }
}

/** Lines to lines: a wall between each line of the profile and the
 * section's in its place -- a facet, or two triangles -- and a bend at
 * every fold. Each wall is its outline on the inner skin through the
 * sheet; each bend its strip in its own flat taken onto its cylinder, its
 * rims there straight from where the walls' rims meet it -- the ends square
 * to the sheet everywhere, so the flat is exact.
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

    /// which of the two is lines where the other is a round cut into
    /// chords: 0 the profile, 1 the section; -1 both lines
    int lines = -1;
    /// a relief at each corner where folds meet on the side the sheet
    /// turns toward, its skin there off the sketch
    bool relief = false;

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
    if (lines < 0 && b.lines() != n) {
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
        for (std::size_t j = 1; lines < 0 && j < n; ++j) {
            if (b.way(j).Dot(a.way(0)) > b.way(best).Dot(a.way(0))) {
                best = j;
            }
        }
        std::rotate(b.points.begin(), b.points.begin() + static_cast<long>(best), b.points.end());
    }
    else {
        outward = areaAbout(a.points, axis) < -tol ? -1 : 1;
        bool back = false;
        if (lines < 0) {
            const gp_Dir way(gp_Vec(b.points.back(), b.points[b.points.size() - 2]));
            back = way.Dot(a.way(0)) > b.way(0).Dot(a.way(0));
        }
        else {
            // the round from its end nearer the lines' start
            back = a.points.front().Distance(b.points.back())
                       + a.points.back().Distance(b.points.front())
                   < a.points.front().Distance(b.points.front())
                         + a.points.back().Distance(b.points.back());
        }
        if (back) {
            std::reverse(b.points.begin(), b.points.end());
        }
    }
    for (std::size_t i = 0; lines < 0 && i < n; ++i) {
        if (a.way(i).Dot(b.way(i)) <= 0) {
            FC_THROWM(Base::ValueError,
                      "Line " << i + 1
                              << " of the lofted flange's profile and the section's in its "
                                 "place run a right angle or more apart");
        }
    }

    // The rulings, each from a corner of the profile to one of the
    // section, where two faces of the sheet meet: lines that run one way
    // span a facet between them, a pair that does not two triangles either
    // side of its shorter diagonal
    struct Ruling
    {
        std::size_t p, s;
    };
    std::vector<Ruling> rulings;
    std::vector<std::size_t> lineOf;
    auto runsWith = [&](std::size_t p, std::size_t s) {
        return a.way(p).IsParallel(b.way(s), angular) && a.way(p).Dot(b.way(s)) > 0;
    };
    if (lines < 0) {
        rulings.push_back({0, 0});
        for (std::size_t i = 0; i < n; ++i) {
            if (!runsWith(i, i)) {
                const bool forward =
                    a.at(i).Distance(b.at(i + 1)) <= a.at(i + 1).Distance(b.at(i));
                rulings.push_back(forward ? Ruling {i, i + 1} : Ruling {i + 1, i});
                lineOf.push_back(i);
            }
            rulings.push_back({i + 1, i + 1});
            lineOf.push_back(i);
        }
    }
    else {
        // Round to lines: from the lines' first corner and the round's
        // nearest to it, each next ruling the shorter of the two -- a step
        // along both where a chord runs a line's way, a facet
        const Chain& l = lines == 0 ? a : b;
        const Chain& r = lines == 0 ? b : a;
        std::size_t near = 0;
        for (std::size_t i = 1; l.closed && i < r.points.size(); ++i) {
            if (r.points[i].Distance(l.points[0]) < r.points[near].Distance(l.points[0])) {
                near = i;
            }
        }
        std::size_t p = lines == 0 ? 0 : near, s = lines == 0 ? near : 0;
        const std::size_t endP = p + a.lines(), endS = s + b.lines();
        rulings.push_back({p, s});
        while (p < endP || s < endS) {
            const std::size_t line = (lines == 0 ? p : s) % l.lines();
            if (p < endP && s < endS && runsWith(p, s)) {
                ++p;
                ++s;
            }
            else if (p == endP) {
                ++s;
            }
            else if (s == endS) {
                ++p;
            }
            else if (a.at(p + 1).Distance(b.at(s)) <= a.at(p).Distance(b.at(s + 1))) {
                ++p;
            }
            else {
                ++s;
            }
            rulings.push_back({p, s});
            lineOf.push_back(line);
        }
        if (a.closed) {
            // the first line's face first, for the seam across it
            const std::size_t m = lineOf.size();
            std::size_t first = 0;
            for (std::size_t j = 0; j < m; ++j) {
                const std::size_t from = lines == 0 ? rulings[j].p : rulings[j].s;
                const std::size_t to = lines == 0 ? rulings[j + 1].p : rulings[j + 1].s;
                if (from % l.lines() == 0 && to != from) {
                    first = j;
                    break;
                }
            }
            rulings.pop_back();
            std::rotate(rulings.begin(), rulings.begin() + static_cast<long>(first), rulings.end());
            std::rotate(lineOf.begin(), lineOf.begin() + static_cast<long>(first), lineOf.end());
            rulings.push_back(rulings.front());
        }
    }

    // each face between two rulings, flat: its sketched corners (a
    // triangle's two on one side the same), its normal out, from its inner
    // skin, which is the sketches' less the material's side
    const double off = side == 1 ? T : side == 2 ? T / 2 : 0;
    struct Face
    {
        gp_Pnt pa, pb, sa, sb, centre;
        gp_Dir normal;
        std::size_t line = 0;
        gp_Pnt inner(const gp_Pnt& p, double off) const
        {
            return p.Translated(gp_Vec(normal) * -off);
        }
        /// the way into the face from a line of it through p along d
        gp_Dir into(const gp_Pnt& p, const gp_Dir& d) const
        {
            return across(gp_Vec(p, centre), d);
        }
    };
    const std::size_t m = rulings.size() - 1;
    std::vector<Face> faces(m);
    for (std::size_t j = 0; j < m; ++j) {
        Face& f = faces[j];
        f.pa = a.at(rulings[j].p);
        f.pb = a.at(rulings[j + 1].p);
        f.sa = b.at(rulings[j].s);
        f.sb = b.at(rulings[j + 1].s);
        f.centre = gp_Pnt((f.pa.XYZ() + f.pb.XYZ() + f.sa.XYZ() + f.sb.XYZ()) / 4);
        f.normal = gp_Dir(gp_Vec(f.pa, f.sb).Crossed(gp_Vec(f.pb, f.sa)) * outward);
        f.line = lineOf[j];
    }

    // The walls, each a face on its inner skin cut by four lines: the
    // sketched lines (a triangle's corner, the line through it the other
    // one's way) and where it starts and ends, each with the way into it.
    // A closed chain's seam: across the middle of the first face where its
    // lines run one way, the first wall its half after the seam and the
    // last its half before; else along the first line's diagonal.
    struct Bound
    {
        Line line;
        gp_Dir into;
        std::string role;
    };
    struct Wall
    {
        std::size_t face = 0;
        Bound bound[4];  // the profile's side, the end, the section's, the start
        std::vector<Bound> reliefs;
        std::vector<gp_Pnt> outline;
        /// the outline along its end and its start, from the profile's side
        gp_Pnt end[2], start[2];
        int index = -1;
    };
    auto wallOf = [&](std::size_t j) {
        const Face& f = faces[j];
        Wall w;
        w.face = j;
        const gp_Dir p = f.pa.Distance(f.pb) > tol ? gp_Dir(gp_Vec(f.pa, f.pb))
                                                   : gp_Dir(gp_Vec(f.sa, f.sb));
        const gp_Dir s = f.sa.Distance(f.sb) > tol ? gp_Dir(gp_Vec(f.sa, f.sb)) : p;
        w.bound[0] = {{f.inner(f.pa, off), p}, f.into(f.pa, p), "Rim1"};
        w.bound[2] = {{f.inner(f.sa, off), s}, f.into(f.sa, s), "Rim2"};
        // open ends where the sketches' do, square to the sheet
        const gp_Dir from(gp_Vec(f.pa, f.sa)), to(gp_Vec(f.pb, f.sb));
        w.bound[3] = {{f.inner(f.pa, off), from}, f.into(f.pa, from), "Start"};
        w.bound[1] = {{f.inner(f.pb, off), to}, f.into(f.pb, to), "End"};
        return w;
    };
    // Where two faces meet, on the skins the sheet turns about: their inner
    // ones where it turns in (toward the side its inner skin faces), their
    // outer ones where it turns out. The point is on the sketched fold's
    // plane square to it; turnsIn says which.
    auto sharpOf = [&](const Face& fa, const Face& fb, bool& turnsIn) {
        const gp_Dir fold(gp_Vec(fb.pa, fb.sa));
        turnsIn = fb.into(fb.pa, fold).Dot(fa.normal) < 0;
        const gp_Vec na(fa.normal), nb(fb.normal);
        const double skin = turnsIn ? -off : T - off;
        return fb.pa.Translated((na + nb) * (skin / (1 + na.Dot(nb))));
    };
    std::vector<Wall> list;
    if (!a.closed) {
        for (std::size_t j = 0; j < m; ++j) {
            list.push_back(wallOf(j));
        }
    }
    else if (m > 1 && lineOf[1] == 0) {
        // along the diagonal of the first line's triangles, where their
        // skins meet as a bend of no radius would, the gap either side of
        // it, each cut square to its own
        for (std::size_t j = 1; j < m; ++j) {
            list.push_back(wallOf(j));
        }
        list.push_back(wallOf(0));
        bool in = true;
        const gp_Pnt sharp = sharpOf(faces[0], faces[1], in);
        for (Wall* w : {&list.front(), &list.back()}) {
            Bound& seam = w->bound[w == &list.front() ? 3 : 1];
            const gp_Vec n(faces[w->face].normal);
            seam.line.p = (in ? sharp : sharp.Translated(n * -T))
                              .Translated(gp_Vec(seam.into) * (gap / 2));
            seam.role = "Seam";
        }
    }
    else {
        const Face& f = faces[0];
        const gp_Pnt mp = f.inner(f.pa.Translated(gp_Vec(f.pa, f.pb) / 2), off);
        const gp_Pnt ms = f.inner(f.sa.Translated(gp_Vec(f.sa, f.sb) / 2), off);
        const gp_Dir g(gp_Vec(mp, ms));
        const gp_Dir w =
            across(f.pa.Distance(f.pb) > tol ? gp_Vec(f.pa, f.pb) : gp_Vec(f.sa, f.sb), g);
        list.push_back(wallOf(0));
        list.front().bound[3] = {{mp.Translated(gp_Vec(w) * (gap / 2)), g}, w, "Seam"};
        for (std::size_t j = 1; j < m; ++j) {
            list.push_back(wallOf(j));
        }
        list.push_back(wallOf(0));
        list.back().bound[1] = {{mp.Translated(gp_Vec(w) * (-gap / 2)), g}, w.Reversed(), "Seam"};
    }

    // a bend between each two walls in turn, where their faces meet
    std::vector<SheetMetal::CylBend> folds;
    std::vector<bool> convex;
    for (std::size_t j = 0; j + 1 < list.size(); ++j) {
        Wall &wa = list[j], &wb = list[j + 1];
        const Face &fa = faces[wa.face], &fb = faces[wb.face];
        const gp_Dir fold(gp_Vec(fb.pa, fb.sa));
        // toward the fold in the one, from it in the other
        const gp_Dir ta = fa.into(fa.pb, fold).Reversed(), tb = fb.into(fb.pa, fold);
        const double turn = std::atan2(gp_Vec(ta).Crossed(gp_Vec(tb)).Magnitude(), ta.Dot(tb));
        if (turn > std::numbers::pi - 1e-6) {
            THROWM(Base::ValueError, "The lofted flange cannot turn back on itself");
        }
        // turning in, its inner skin the faces'; out, their outer one
        bool in = true;
        const gp_Pnt sharp = sharpOf(fa, fb, in);
        const gp_Vec na(fa.normal), nb(fb.normal);
        const gp_Vec ua = in ? -na : na, ub = in ? -nb : nb;
        const gp_Pnt centre = sharp.Translated((ua + ub) * (radius / (1 + ua.Dot(ub))));
        SheetMetal::CylBend bend;
        bend.frame = gp_Ax3(centre, gp_Dir((-ua).Crossed(gp_Vec(ta))), gp_Dir(-ua));
        bend.radius = radius;
        bend.thickness = T;
        bend.angle = turn;
        bend.k = k(turn, radius);
        const gp_Pnt endA = centre.Translated(ua * -radius);
        const gp_Pnt startB = centre.Translated(ub * -radius);
        wa.bound[1] = {{in ? endA : endA.Translated(na * -T), fold}, ta.Reversed(), "End"};
        wb.bound[3] = {{in ? startB : startB.Translated(nb * -T), fold}, tb, "Start"};
        folds.push_back(bend);
        convex.push_back(in);
    }

    // The reliefs. Where folds meet at a corner on the side the sheet turns
    // toward -- of the lines, or of a round where a line's triangle comes
    // to a point on it -- and its skin there is off the sketch, the walls
    // that run to a point there cross their sides short of it, and the
    // sheet runs into itself: each wall about the corner is cut where the
    // last of those comes to its full width, square to the sheet -- one
    // running to the point by a line its base's way, one along a line by a
    // line square to it -- and the bends between follow their walls.
    const std::vector<int> sides = relief ? std::vector<int> {0, 1} : std::vector<int> {};
    for (int c : sides) {
        const gp_Vec away(c == 0 ? axis : axis.Reversed());
        auto ends = [&](const Face& f) {
            return c == 0 ? std::pair {f.pa, f.pb} : std::pair {f.sa, f.sb};
        };
        for (const gp_Pnt& corner : (c == 0 ? a : b).points) {
            auto height = [&](const gp_Pnt& p) {
                return gp_Vec(corner, p).Dot(away);
            };
            double h = 0;
            bool converges = false;
            for (std::size_t j = 0; j < list.size(); ++j) {
                const auto [p, q] = ends(faces[list[j].face]);
                if (p.Distance(corner) > tol || q.Distance(corner) > tol) {
                    continue;
                }
                const gp_Vec n(faces[list[j].face].normal);
                const Line &s = list[j].bound[3].line, &e = list[j].bound[1].line;
                const double den = gp_Vec(s.d).Crossed(gp_Vec(e.d)).Dot(n);
                if (std::abs(den) > angular) {
                    const double t = gp_Vec(s.p, e.p).Crossed(gp_Vec(e.d)).Dot(n) / den;
                    h = std::max(h, height(s.p.Translated(gp_Vec(s.d) * t)));
                }
                for (std::size_t k : {j - 1, j}) {
                    if (k < folds.size()) {
                        converges = converges || (convex[k] ? off : T - off) > tol;
                    }
                }
            }
            if (!converges || h <= tol) {
                continue;
            }
            for (Wall& w : list) {
                const Face& f = faces[w.face];
                const auto [p, q] = ends(f);
                const bool atP = p.Distance(corner) <= tol, atQ = q.Distance(corner) <= tol;
                if (!atP && !atQ) {
                    continue;
                }
                const gp_Vec n(f.normal);
                auto upward = [&](const gp_Dir& d) {
                    const gp_Dir g(n.Crossed(gp_Vec(d)));
                    return g.Dot(gp_Dir(away)) < 0 ? g.Reversed() : g;
                };
                if (atP && atQ) {
                    const gp_Dir base = w.bound[c == 0 ? 2 : 0].line.d;
                    const gp_Dir g = upward(base);
                    const gp_Pnt from = f.inner(corner, off);
                    const gp_Pnt at =
                        from.Translated(gp_Vec(g) * ((h - height(from)) / gp_Vec(g).Dot(away)));
                    w.reliefs.push_back({{at, base}, g, "Relief"});
                    continue;
                }
                const Bound& side = w.bound[atQ ? 1 : 3];
                const double rise = gp_Vec(side.line.d).Dot(away);
                if (side.role == "Seam" || std::abs(rise) < angular) {
                    continue;
                }
                const double t = (h - height(side.line.p)) / rise;
                const gp_Pnt at = side.line.p.Translated(gp_Vec(side.line.d) * t);
                const gp_Dir rim = w.bound[c == 0 ? 0 : 2].line.d;
                w.reliefs.push_back({{at, upward(rim)}, atQ ? rim.Reversed() : rim, "Relief"});
            }
        }
    }

    // each wall's outline: a square about its face cut by its four lines
    double size = 10 * (T + radius + gap);
    for (const gp_Pnt& p : a.points) {
        size += p.Distance(b.points.front());
    }
    for (std::size_t j = 0; j < list.size(); ++j) {
        Wall& wall = list[j];
        const Face& f = faces[wall.face];
        const gp_Pnt c = f.inner(f.centre, off);
        const gp_Vec x = gp_Vec(wall.bound[0].line.d) * size;
        const gp_Vec y = gp_Vec(f.normal).Crossed(gp_Vec(wall.bound[0].line.d)) * size;
        std::vector<gp_Pnt> poly {c.Translated(-x - y), c.Translated(x - y), c.Translated(x + y),
                                  c.Translated(-x + y)};
        for (const Bound& bound : wall.bound) {
            clipTo(poly, bound.line.p, gp_Vec(bound.into));
        }
        for (const Bound& bound : wall.reliefs) {
            clipTo(poly, bound.line.p, gp_Vec(bound.into));
        }
        // its run along its end and its start, from the profile's side
        auto run = [&](const Bound& bound, gp_Pnt res[2]) {
            bool any = false;
            double lo = 0, hi = 0;
            for (const gp_Pnt& p : poly) {
                const gp_Vec v(bound.line.p, p);
                if (std::abs(v.Dot(gp_Vec(bound.into))) > 1e-6) {
                    continue;
                }
                const double t = v.Dot(gp_Vec(bound.line.d));
                if (!any || t < lo) {
                    lo = t;
                    res[0] = p;
                }
                if (!any || t > hi) {
                    hi = t;
                    res[1] = p;
                }
                any = true;
            }
            return any;
        };
        if (poly.size() < 3 || !run(wall.bound[1], wall.end) || !run(wall.bound[3], wall.start)) {
            FC_THROWM(Base::ValueError,
                      "Line " << f.line + 1 << " of the lofted flange is too short for its bends");
        }
        wall.outline = poly;

        const std::string name = "Wall" + std::to_string(walls + 1 + static_cast<int>(j));
        wall.index = model.addWall(name, gp_Ax3(poly.front(), f.normal, wall.bound[0].line.d));
        std::vector<std::pair<TopoDS_Edge, std::string>> edges;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const gp_Pnt &p = poly[i], &q = poly[(i + 1) % poly.size()];
            const gp_Pnt mid = p.Translated(gp_Vec(p, q) / 2);
            std::string role;
            for (const Bound& bound : wall.bound) {
                if (std::abs(gp_Vec(bound.line.p, mid).Dot(gp_Vec(bound.into))) <= 1e-6) {
                    role = bound.role;
                    break;
                }
            }
            for (std::size_t r = 0; role.empty() && r < wall.reliefs.size(); ++r) {
                const Bound& bound = wall.reliefs[r];
                if (std::abs(gp_Vec(bound.line.p, mid).Dot(gp_Vec(bound.into))) <= 1e-6) {
                    role = bound.role + std::to_string(r + 1);
                }
            }
            edges.emplace_back(SheetMetal::lineEdge(p, q), name + role);
        }
        pieces.push_back(
            TopoShape(0, hasher).makEPrism(SheetMetal::namedFace(edges, name),
                                           gp_Vec(f.normal) * T));
        // Each stops at its outline, square to it: a seam's walls lie on
        // one plane, and where folds meet at a corner the next faces lie
        // in its slab, on its side of its own bends
        gp_XYZ middle;
        for (const gp_Pnt& p : poly) {
            middle += p.XYZ();
        }
        const gp_Pnt inside(middle / static_cast<double>(poly.size()));
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const gp_Pnt &p = poly[i], &q = poly[(i + 1) % poly.size()];
            gp_Dir out(gp_Vec(p, q).Crossed(gp_Vec(f.normal)));
            if (gp_Vec(p, inside).Dot(gp_Vec(out)) > 0) {
                out.Reverse();
            }
            model.elements[wall.index].limits.emplace_back(p, out);
        }
    }

    // Each bend's strip in its own flat, on its inner skin: its rims
    // straight between the walls' where they meet it, short of where each
    // wall ends at its other side -- where two folds meet at a corner of an
    // outline, or a fold and the seam, what lies past that is the other's
    for (std::size_t j = 0; j < folds.size(); ++j) {
        const SheetMetal::CylBend& bend = folds[j];
        const Wall &wa = list[j], &wb = list[j + 1];
        const gp_Vec liftA = convex[j] ? gp_Vec() : gp_Vec(faces[wa.face].normal) * T;
        const gp_Vec liftB = convex[j] ? gp_Vec() : gp_Vec(faces[wb.face].normal) * T;
        const gp_Pnt o = bend.frame.Location();
        const gp_Vec x(bend.frame.XDirection()), y(bend.frame.YDirection()),
            z(bend.frame.Direction());
        const double length = bend.angle * bend.neutral();
        auto flat = [&](double along, const gp_Pnt& p) {
            return o.Translated(x * radius + y * along + z * gp_Vec(o, p).Dot(z));
        };
        std::vector<gp_Pnt> strip {flat(0, wa.end[0].Translated(liftA)),
                                   flat(length, wb.start[0].Translated(liftB)),
                                   flat(length, wb.start[1].Translated(liftB)),
                                   flat(0, wa.end[1].Translated(liftA))};
        // The wall before lies flat where it is, the one after unbent. A
        // point s into the strip folded is the chord r sin(s / r) across
        // from its line, not s: so that it stays short of the wall's other
        // side however far round it is, the cut's slope across the strip
        // is taken at the least that can be where the side leans back over
        // the strip (the inner skin, sin(angle) / angle of it), at the most
        // where it leans away (the outer) -- straight in the flat still
        const gp_Trsf unbend = bend.unbend();
        auto clipBy = [&](const Bound& bound, const gp_Vec& lift, const gp_Trsf& move,
                          double line, const gp_Vec& into) {
            const gp_Pnt p = bound.line.p.Translated(lift).Transformed(move);
            const gp_Vec d = gp_Vec(bound.line.d).Transformed(move);
            const gp_Vec w = gp_Vec(bound.into).Transformed(move);
            const double dy = d.Dot(y);
            if (std::abs(dy) < angular) {
                clipTo(strip, p, w);
                return;
            }
            const gp_Pnt at = p.Translated(d * ((line - gp_Vec(o, p).Dot(y)) / dy));
            const double across = w.Dot(into);
            const double reach = across < 0 ? radius * std::sin(bend.angle) / bend.angle
                                            : radius + T;
            clipTo(strip, at, z * w.Dot(z) + into * (across * reach / bend.neutral()));
        };
        clipBy(wa.bound[3], liftA, gp_Trsf(), 0, -y);
        clipBy(wb.bound[1], liftB, unbend, length, y);
        const std::string name = "Bend" + std::to_string(bends + 1 + static_cast<int>(j));
        BRepBuilderAPI_MakePolygon outline;
        for (const gp_Pnt& p : strip) {
            outline.Add(p);
        }
        outline.Close();
        if (strip.size() < 3 || !outline.IsDone()) {
            FC_THROWM(Base::RuntimeError, "The lofted flange's " << name << " could not be made");
        }
        const TopoDS_Face face = BRepBuilderAPI_MakeFace(outline.Wire(), true).Face();
        const TopoDS_Shape solid = SheetMetal::bendSolid(bend, face, radius, radius + T);
        if (solid.IsNull()) {
            FC_THROWM(Base::RuntimeError, "The lofted flange's " << name << " could not be made");
        }
        pieces.emplace_back(solid);
        const int index = model.addBend(name, bend, wa.index, wb.index);

        // Its rims, each edge of the strip but its start and its end,
        // unrolled (rimPoint()), so that it owns its own faces only: where
        // folds meet at a corner, the next bend's lie in its tube sector
        auto along = [&](const gp_Pnt& p) {
            return gp_Vec(o, p).Dot(y);
        };
        auto unrolled = [&](const gp_Pnt& p) {
            return p.Translated(y * (along(p) * (radius / bend.neutral() - 1)));
        };
        gp_XYZ middle;
        for (const gp_Pnt& p : strip) {
            middle += unrolled(p).XYZ();
        }
        const gp_Pnt inside(middle / static_cast<double>(strip.size()));
        for (std::size_t i = 0; i < strip.size(); ++i) {
            const gp_Pnt &p = strip[i], &q = strip[(i + 1) % strip.size()];
            const bool startOrEnd =
                (std::abs(along(p)) <= tol && std::abs(along(q)) <= tol)
                || (std::abs(along(p) - length) <= tol && std::abs(along(q) - length) <= tol);
            if (startOrEnd) {
                continue;
            }
            const gp_Pnt p0 = unrolled(p), p1 = unrolled(q);
            gp_Dir normal(x.Crossed(gp_Vec(p0, p1)));
            if (gp_Vec(p0, inside).Dot(gp_Vec(normal)) > 0) {
                normal.Reverse();
            }
            model.elements[index].rims.emplace_back(p0, normal);
        }
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
            LinesLoft loft;
            loft.lines = linesA && linesB ? -1 : linesA ? 0 : 1;
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
            // a round cut into chords
            const int segments = Segments.getValue();
            const Chain from = linesA ? chainOf(profile, "profile")
                                      : chainOf(roundOf(profile, "profile"), segments);
            const Chain to = linesB ? chainOf(section, "section")
                                    : chainOf(roundOf(section, "section"), segments);
            if (from.closed && loft.gap < Precision::Confusion()) {
                THROWM(Base::ValueError,
                       "A lofted flange all round needs the rule's Gap for its seam");
            }
            // Pieces that overlap make one solid still, but not a sheet: a
            // thick sheet cannot come to a point inside it, where folds of
            // round to lines meet at a corner on the material's side. There
            // it takes a relief, and is built again. However little they
            // overlap, the flat does not close; a sheet that does not comes
            // out within 1e-11 of its pieces' volume.
            const SheetMetal::Model before = model;
            for (bool relief : {false, true}) {
                loft.relief = relief;
                model = before;
                std::vector<TopoShape> pieces;
                loft.build(from, to, gp_Dir(normal), model, pieces, hasher);
                tool = getSolid(TopoShape(0, hasher).makEFuse(pieces));
                double sum = 0;
                for (const TopoShape& piece : pieces) {
                    GProp_GProps props;
                    BRepGProp::VolumeProperties(piece.getShape(), props);
                    sum += props.Mass();
                }
                GProp_GProps props;
                BRepGProp::VolumeProperties(tool.getShape(), props);
                if (sum - props.Mass() <= 1e-9 * sum) {
                    break;
                }
                if (relief) {
                    THROWM(Base::ValueError,
                           "The lofted flange's sheet runs into itself where its folds meet at "
                           "a corner: put the material on the other side, or round the corner");
                }
            }
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
