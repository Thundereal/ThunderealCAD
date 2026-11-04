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
#include <limits>
#include <map>
#include <numeric>
#include <set>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <ElCLib.hxx>
#include <Geom2d_Curve.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <Base/Exception.h>

#include "Recognise.h"

namespace PartDesign::SheetMetal
{

namespace
{

/// Lengths are compared within this
constexpr double lengthTol = 1e-5;

/// A face's normal out of the material, of a plane
gp_Dir outward(const TopoDS_Face& face, const gp_Pln& plane)
{
    gp_Dir n = plane.Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) {
        n.Reverse();
    }
    return n;
}

/// The extent of a face's vertices along a direction
std::pair<double, double> extent(const TopoDS_Shape& shape, const gp_Dir& dir)
{
    double lo = std::numeric_limits<double>::max();
    double hi = -std::numeric_limits<double>::max();
    for (TopExp_Explorer it(shape, TopAbs_VERTEX); it.More(); it.Next()) {
        const double d =
            gp_Vec(BRep_Tool::Pnt(TopoDS::Vertex(it.Current())).XYZ()).Dot(gp_Vec(dir));
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    return {lo, hi};
}

struct Plane
{
    int index;  // in the solid's face map, from 1
    TopoDS_Face face;
    gp_Pln plane;
    gp_Dir out;
};

struct Cylinder
{
    int index;
    TopoDS_Face face;
    gp_Cylinder cylinder;
};

/// The union-find of the walls' skins
struct Groups
{
    std::vector<int> parent;
    explicit Groups(int n)
        : parent(n)
    {
        std::iota(parent.begin(), parent.end(), 0);
    }
    int find(int i)
    {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }
    void join(int a, int b)
    {
        parent[find(a)] = find(b);
    }
};

/// A point of a face, the one nearest the middle of its parameters on a grid
/// over them; false for none found
bool pointIn(const TopoDS_Face& face, gp_Pnt& point)
{
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    BRepTopAdaptor_FClass2d inside(face, Precision::PConfusion());
    BRepAdaptor_Surface s(face);
    const int n = 16;
    double best = -1;
    for (int i = 1; i < n; ++i) {
        for (int j = 1; j < n; ++j) {
            const gp_Pnt2d uv(u0 + (u1 - u0) * i / n, v0 + (v1 - v0) * j / n);
            const double off = std::abs(i - n / 2.0) + std::abs(j - n / 2.0);
            if ((best < 0 || off < best) && inside.Perform(uv) == TopAbs_IN) {
                best = off;
                point = s.Value(uv.X(), uv.Y());
            }
        }
    }
    return best >= 0;
}

/// From a point of a plane face into the material, the face it leaves
/// through and how far; a null face for none
std::pair<TopoDS_Face, double> across(const TopoDS_Shape& solid, const Plane& from)
{
    gp_Pnt p;
    if (!pointIn(from.face, p)) {
        return {};
    }
    IntCurvesFace_ShapeIntersector hits;
    hits.Load(solid, Precision::Confusion());
    hits.Perform(gp_Lin(p, from.out.Reversed()), lengthTol, Precision::Infinite());
    std::pair<TopoDS_Face, double> res {TopoDS_Face(), -1};
    for (int i = 1; hits.IsDone() && i <= hits.NbPnt(); ++i) {
        const double w = hits.WParameter(i);
        if (!hits.Face(i).IsSame(from.face) && (res.second < 0 || w < res.second)) {
            res = {hits.Face(i), w};
        }
    }
    return res;
}

/// The distance into the material from a face to a plane face facing it,
/// the two overlapping; negative for none
double facing(const Plane& from, const Plane& to)
{
    if (!from.out.IsOpposite(to.out, Precision::Angular())) {
        return -1;
    }
    const double d = gp_Vec(to.plane.Location(), from.plane.Location()).Dot(gp_Vec(from.out));
    if (d <= lengthTol) {
        return -1;
    }
    // overlapping as seen along the normal: no nearer than the planes' gap
    BRepExtrema_DistShapeShape dist(from.face, to.face);
    if (!dist.IsDone() || dist.Value() > d + lengthTol) {
        return -1;
    }
    return d;
}

}  // namespace

double facingThickness(const TopoDS_Shape& solid, const TopoDS_Face& fixed)
{
    BRepAdaptor_Surface s(fixed);
    if (s.GetType() != GeomAbs_Plane) {
        return -1;
    }
    const Plane from {0, fixed, s.Plane(), outward(fixed, s.Plane())};
    double res = -1;
    for (TopExp_Explorer it(solid, TopAbs_FACE); it.More(); it.Next()) {
        const TopoDS_Face& face = TopoDS::Face(it.Current());
        BRepAdaptor_Surface t(face);
        if (face.IsSame(fixed) || t.GetType() != GeomAbs_Plane) {
            continue;
        }
        const double d = facing(from, {0, face, t.Plane(), outward(face, t.Plane())});
        if (d > 0 && (res < 0 || d < res)) {
            res = d;
        }
    }
    return res;
}

Recognised recognise(const TopoDS_Shape& solid,
                     const TopoDS_Face& fixed,
                     const std::function<double(double, double)>& kOf)
{
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(solid, TopAbs_FACE, faces);
    const int fixedIndex = faces.FindIndex(fixed);
    if (fixedIndex <= 0) {
        THROWM(Base::ValueError, "The fixed face is not a face of the part");
    }
    if (BRepAdaptor_Surface(fixed).GetType() != GeomAbs_Plane) {
        THROWM(Base::ValueError,
               "The fixed face is curved: it is a wall's skin, and only planar walls are "
               "taken (rolled walls come later)");
    }

    std::vector<Plane> planes;
    std::vector<Cylinder> cylinders;
    std::map<int, int> planeOf;  // face index -> index in planes
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face& face = TopoDS::Face(faces(i));
        BRepAdaptor_Surface s(face);
        if (s.GetType() == GeomAbs_Plane) {
            planeOf[i] = static_cast<int>(planes.size());
            planes.push_back({i, face, s.Plane(), outward(face, s.Plane())});
        }
        else if (s.GetType() == GeomAbs_Cylinder) {
            cylinders.push_back({i, face, s.Cylinder()});
        }
    }

    // The thickness: from the fixed face to the nearest skin facing it
    const Plane& fixedPlane = planes[planeOf.at(fixedIndex)];
    double thickness = -1;
    for (const Plane& other : planes) {
        const double d = facing(fixedPlane, other);
        if (d > 0 && (thickness < 0 || d < thickness)) {
            thickness = d;
        }
    }
    if (thickness <= 0) {
        THROWM(Base::ValueError,
               "The fixed face has no skin opposite it in the material: the part is not thin");
    }

    // The skins: plane faces with one facing them a thickness away. A plane
    // face facing another at any other distance, lying parallel to a wall,
    // is where the thickness varies.
    const int n = static_cast<int>(planes.size());
    std::vector<int> partner(n, -1);
    std::vector<double> nearest(n, -1);
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            const double d = a == b ? -1 : facing(planes[a], planes[b]);
            if (d <= 0) {
                continue;
            }
            if (nearest[a] < 0 || d < nearest[a]) {
                nearest[a] = d;
            }
            if (std::abs(d - thickness) <= lengthTol) {
                partner[a] = b;
            }
        }
    }
    if (partner[planeOf.at(fixedIndex)] < 0) {
        THROWM(Base::ValueError, "The fixed face is no skin of a wall");
    }

    // The walls: the skins joined with their partners, and with the skins
    // of the same plane they share an edge with
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(solid, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    Groups groups(n);
    for (int a = 0; a < n; ++a) {
        if (partner[a] >= 0) {
            groups.join(a, partner[a]);
        }
    }
    for (int e = 1; e <= edgeFaces.Extent(); ++e) {
        std::vector<int> skins;
        for (const TopoDS_Shape& f : edgeFaces(e)) {
            const auto it = planeOf.find(faces.FindIndex(f));
            if (it != planeOf.end() && partner[it->second] >= 0) {
                skins.push_back(it->second);
            }
        }
        for (std::size_t i = 1; i < skins.size(); ++i) {
            const Plane &p = planes[skins[0]], &q = planes[skins[i]];
            if (p.out.IsEqual(q.out, Precision::Angular())
                && std::abs(p.plane.Distance(q.plane.Location())) <= lengthTol) {
                groups.join(skins[0], skins[i]);
            }
        }
    }

    // A plane face not a skin facing a skin across the material: the
    // thickness varies. Facing a side of a wall -- a leg's end from its edge
    // over a corner relief -- or a skin across empty space is no matter.
    for (int a = 0; a < n; ++a) {
        if (partner[a] >= 0 || nearest[a] < 0) {
            continue;
        }
        const auto [face, d] = across(solid, planes[a]);
        const auto it = face.IsNull() ? planeOf.end() : planeOf.find(faces.FindIndex(face));
        if (it != planeOf.end() && partner[it->second] >= 0
            && planes[it->second].out.IsOpposite(planes[a].out, Precision::Angular())
            && std::abs(d - thickness) > lengthTol) {
            FC_THROWM(Base::ValueError,
                      "Face" << planes[a].index << " is " << d << " mm thick, not the part's "
                             << thickness << " mm: the thickness varies");
        }
    }

    Recognised res;
    Model& model = res.model;
    model.thickness = thickness;

    // The walls by group, the fixed face's first, its first skin the fixed
    // face; the others from one of their skins
    std::map<int, int> wallOf;  // group root -> wall index
    std::vector<int> order;
    const int fixedGroup = groups.find(planeOf.at(fixedIndex));
    order.push_back(fixedGroup);
    for (int a = 0; a < n; ++a) {
        const int g = partner[a] >= 0 ? groups.find(a) : -1;
        if (g >= 0 && std::find(order.begin(), order.end(), g) == order.end()) {
            order.push_back(g);
        }
    }
    for (int g : order) {
        int skin = g == fixedGroup ? planeOf.at(fixedIndex) : -1;
        for (int a = 0; a < n && skin < 0; ++a) {
            if (partner[a] >= 0 && groups.find(a) == g) {
                skin = a;
            }
        }
        const Plane& p = planes[skin];
        const gp_Dir in = p.out.Reversed();
        const gp_Ax3 frame(p.plane.Location(), in, p.plane.Position().XDirection());
        wallOf[g] = model.addWall("Wall" + std::to_string(wallOf.size() + 1), frame);
    }
    auto wallOfFace = [&](const TopoDS_Shape& face) {
        const auto it = planeOf.find(faces.FindIndex(face));
        if (it == planeOf.end() || partner[it->second] < 0) {
            return -1;
        }
        return wallOf.at(groups.find(it->second));
    };

    // The bends: coaxial cylinders a thickness apart, the inner its radius,
    // between the walls whose skins meet it
    struct Found
    {
        CylBend bend;
        int before;
        int after;
        double zmin;
        double zmax;
    };
    std::vector<Found> found;
    std::set<int> taken;
    for (const Cylinder& inner : cylinders) {
        for (const Cylinder& outer : cylinders) {
            const gp_Ax1 &ai = inner.cylinder.Axis(), &ao = outer.cylinder.Axis();
            if (&inner == &outer || !ai.IsParallel(ao, Precision::Angular())
                || gp_Lin(ai).Distance(ao.Location()) > lengthTol
                || std::abs(outer.cylinder.Radius() - inner.cylinder.Radius() - thickness)
                    > lengthTol) {
                continue;
            }
            // The walls meeting the inner skin along its two straight edges
            double u0, u1, v0, v1;
            BRepTools::UVBounds(inner.face, u0, u1, v0, v1);
            BRepAdaptor_Surface s(inner.face);
            gp_Pnt p0;
            gp_Vec du, dv;
            s.D1(u0, (v0 + v1) / 2, p0, du, dv);
            const gp_Pnt onAxis = gp_Lin(ai).Distance(p0) > 0
                ? ElCLib::Value(ElCLib::Parameter(gp_Lin(ai), p0), gp_Lin(ai))
                : ai.Location();
            const gp_Dir x(gp_Vec(onAxis, p0));
            const gp_Dir z(gp_Vec(x).Crossed(du));
            int walls[2] = {-1, -1};
            for (TopExp_Explorer e(inner.face, TopAbs_EDGE); e.More(); e.Next()) {
                const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
                double f, l;
                Handle(Geom2d_Curve) c2d = BRep_Tool::CurveOnSurface(edge, inner.face, f, l);
                if (c2d.IsNull()) {
                    continue;
                }
                const double u = c2d->Value((f + l) / 2).X();
                const int side = std::abs(u - u0) < 1e-7 ? 0 : (std::abs(u - u1) < 1e-7 ? 1 : -1);
                if (side < 0) {
                    continue;
                }
                const int index = edgeFaces.FindIndex(edge);
                for (const TopoDS_Shape& face : edgeFaces(index)) {
                    const int wall = wallOfFace(face);
                    if (wall >= 0) {
                        walls[side] = wall;
                    }
                }
            }
            if (walls[0] < 0 || walls[1] < 0) {
                continue;
            }
            Found bend;
            bend.bend.frame = gp_Ax3(onAxis, z, x);
            bend.bend.radius = inner.cylinder.Radius();
            bend.bend.thickness = thickness;
            bend.bend.angle = u1 - u0;
            bend.bend.k = kOf(bend.bend.angle, bend.bend.radius);
            bend.before = walls[0];
            bend.after = walls[1];
            const auto ri = extent(inner.face, z), ro = extent(outer.face, z);
            bend.zmin = std::min(ri.first, ro.first);
            bend.zmax = std::max(ri.second, ro.second);
            found.push_back(bend);
            taken.insert(inner.index);
            taken.insert(outer.index);
        }
    }
    // in order out from the fixed face's wall, so each is named as it is met
    std::vector<bool> placed(model.elements.size(), false);
    placed[wallOf.at(fixedGroup)] = true;
    std::vector<bool> added(found.size(), false);
    for (bool more = true; more;) {
        more = false;
        for (std::size_t i = 0; i < found.size(); ++i) {
            const Found& f = found[i];
            if (added[i] || !(placed[f.before] || placed[f.after])) {
                continue;
            }
            int bends = 0;
            for (const Element& e : model.elements) {
                bends += e.kind == Element::Kind::Bend;
            }
            model.addBend("Bend" + std::to_string(bends + 1), f.bend, f.before, f.after);
            placed.resize(model.elements.size(), false);
            placed[f.before] = placed[f.after] = true;
            added[i] = more = true;
            // where it meets its walls: a face across the sheet through both
            // lies in one element once split there
            const CylBend& b = f.bend;
            const double r0 = b.radius * 0.5, r1 = b.radius + b.thickness * 1.5;
            const double m = b.thickness;
            for (double a : {0.0, b.angle}) {
                const gp_Dir radial =
                    b.frame.XDirection().Rotated(gp_Ax1(b.frame.Location(), b.frame.Direction()), a);
                auto at = [&](double r, double zz) {
                    return b.frame.Location().Translated(gp_Vec(radial) * r
                                                         + gp_Vec(b.frame.Direction()) * zz);
                };
                const double zl = f.zmin - gp_Vec(b.frame.Location().XYZ()).Dot(
                                               gp_Vec(b.frame.Direction()))
                    - m;
                const double zh = f.zmax - gp_Vec(b.frame.Location().XYZ()).Dot(
                                               gp_Vec(b.frame.Direction()))
                    + m;
                BRepBuilderAPI_MakePolygon poly(at(r0, zl), at(r0, zh), at(r1, zh), at(r1, zl),
                                                Standard_True);
                BRepBuilderAPI_MakeFace face(poly.Wire(), Standard_True);
                if (face.IsDone()) {
                    res.cutters.push_back(face.Face());
                }
            }
        }
    }

    // A curved face that is no bend's must lie within a wall's thickness:
    // a hole's side, a rounded corner of its outline
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face& face = TopoDS::Face(faces(i));
        if (planeOf.count(i) || taken.count(i)) {
            continue;
        }
        bool inWall = false;
        for (const Element& e : model.elements) {
            if (e.kind != Element::Kind::Wall) {
                continue;
            }
            const auto range = extent(face, e.frame.Direction());
            const double base = gp_Vec(e.frame.Location().XYZ()).Dot(gp_Vec(e.frame.Direction()));
            if (range.second - range.first <= thickness + lengthTol
                && range.first >= base - lengthTol
                && range.second <= base + thickness + lengthTol) {
                inWall = true;
                break;
            }
        }
        if (!inWall) {
            FC_THROWM(Base::ValueError,
                      "Face" << i
                             << " is curved and in no wall or bend: only planar walls are "
                                "taken (rolled walls come later)");
        }
    }
    return res;
}

}  // namespace PartDesign::SheetMetal
