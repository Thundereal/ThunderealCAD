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
#include <cmath>
#include <numbers>
#include <optional>
#include <set>

#include <Geom_Circle.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Plane.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <gp_Ax2.hxx>
#include <gp_Mat.hxx>
#include <gp_Quaternion.hxx>

#include <Mod/Part/App/Geometry.h>

#include "Sketch3DSolver.h"

using namespace Sketcher;

namespace
{

// A curve that is not solved for, evaluated by OCCT: an external edge.
class OcctCurve3: public GCS::FixedCurve3
{
public:
    explicit OcctCurve3(Handle(Geom_Curve) c)
        : curve(std::move(c))
    {}
    void value(double u, double p[3], double d1[3]) const override
    {
        gp_Pnt pnt;
        gp_Vec vec;
        curve->D1(u, pnt, vec);
        p[0] = pnt.X();
        p[1] = pnt.Y();
        p[2] = pnt.Z();
        d1[0] = vec.X();
        d1[1] = vec.Y();
        d1[2] = vec.Z();
    }

private:
    Handle(Geom_Curve) curve;
};

Base::Vector3d vec(const GCS::Point3& p)
{
    return Base::Vector3d(*p.x, *p.y, *p.z);
}

}  // namespace

Sketch3DSolver::Sketch3DSolver() = default;
Sketch3DSolver::~Sketch3DSolver() = default;

double* Sketch3DSolver::param(double value, bool unknown)
{
    params.push_back(value);
    double* p = &params.back();
    if (unknown) {
        unknowns.push_back(p);
    }
    return p;
}

GCS::Point3 Sketch3DSolver::point(const Base::Vector3d& v, bool unknown)
{
    return {param(v.x, unknown), param(v.y, unknown), param(v.z, unknown)};
}

void Sketch3DSolver::addEntity(const Part::Geometry* geo, bool external, bool blocked)
{
    bool unknown = !external && !blocked;
    Entity e;
    e.external = external;
    e.source = geo;

    if (auto pt = freecad_cast<const Part::GeomPoint*>(geo)) {
        e.kind = Kind::Point;
        e.start = e.end = e.mid = point(pt->getPoint(), unknown);
    }
    else if (auto line = freecad_cast<const Part::GeomLineSegment*>(geo)) {
        e.kind = Kind::Line;
        e.start = point(line->getStartPoint(), unknown);
        e.end = point(line->getEndPoint(), unknown);
    }
    else if (auto circle = freecad_cast<const Part::GeomCircle*>(geo)) {
        e.kind = Kind::Circle;
        e.mid = point(circle->getCenter(), unknown);
        e.normal = point(circle->getAxisDirection(), unknown);
        e.rad = param(circle->getRadius(), unknown);
        e.circle = {e.mid, e.normal, e.rad};
        if (unknown) {
            GCS::addConstraintCircle3Rules(sys, e.circle);
        }
    }
    else if (auto arc = freecad_cast<const Part::GeomArcOfCircle*>(geo)) {
        // Counterclockwise about its circle's axis from start to end.
        e.kind = Kind::Arc;
        e.mid = point(arc->getCenter(), unknown);
        e.normal = point(arc->getAxisDirection(), unknown);
        e.start = point(arc->getStartPoint(), unknown);
        e.end = point(arc->getEndPoint(), unknown);
        e.arc = {e.mid, e.normal, e.start, e.end};
        if (unknown) {
            GCS::addConstraintArc3Rules(sys, e.arc);
        }
    }
    else if (auto bsp = freecad_cast<const Part::GeomBSplineCurve*>(geo);
             bsp && !bsp->isPeriodic()) {
        // Poles are solved for; weights, degree and knots are kept (docs/Sketch3D.md 4.4).
        e.kind = Kind::BSpline;
        auto poles = bsp->getPoles();
        auto weights = bsp->getWeights();
        for (std::size_t i = 0; i < poles.size(); ++i) {
            e.bsp.poles.push_back(point(poles[i], unknown));
            e.bsp.weights.push_back(param(weights[i], false));
        }
        e.bsp.degree = bsp->getDegree();
        auto knots = bsp->getKnots();
        auto mults = bsp->getMultiplicities();
        for (std::size_t i = 0; i < knots.size(); ++i) {
            for (int m = 0; m < mults[i]; ++m) {
                e.bsp.flatknots.push_back(knots[i]);
            }
        }
        e.distinctKnots = knots;
        e.start = e.bsp.poles.front();
        e.end = e.bsp.poles.back();
    }
    else if (auto plane = freecad_cast<const Part::GeomPlane*>(geo)) {
        // Fixed in v1 (docs/Sketch3D.md section 6).
        e.kind = Kind::Plane;
        auto pln = Handle(Geom_Plane)::DownCast(plane->handle());
        const gp_Ax3& pos = pln->Position();
        gp_Mat m(pos.XDirection().XYZ(), pos.YDirection().XYZ(), pos.Direction().XYZ());
        gp_Quaternion q(m);
        const gp_Pnt& o = pos.Location();
        e.mid = point(Base::Vector3d(o.X(), o.Y(), o.Z()), false);
        e.plane.origin = e.mid;
        e.plane.qw = param(q.W(), false);
        e.plane.qx = param(q.X(), false);
        e.plane.qy = param(q.Y(), false);
        e.plane.qz = param(q.Z(), false);
    }
    else if (auto curve = freecad_cast<const Part::GeomCurve*>(geo); curve && external) {
        e.kind = Kind::Curve;
        e.fixedCurve = std::make_shared<OcctCurve3>(Handle(Geom_Curve)::DownCast(curve->handle()));
        if (auto bounded = freecad_cast<const Part::GeomBoundedCurve*>(geo)) {
            e.start = point(bounded->getStartPoint(), false);
            e.end = point(bounded->getEndPoint(), false);
        }
    }
    else {
        // Not solvable here (a periodic spline, an ellipse of the sketch's own): kept as
        // it is, every reference to it malformed.
        e.kind = Kind::Curve;
    }

    if (external && e.kind != Kind::Curve && e.kind != Kind::Plane && !e.fixedCurve) {
        if (auto curve = freecad_cast<const Part::GeomCurve*>(geo)) {
            e.fixedCurve =
                std::make_shared<OcctCurve3>(Handle(Geom_Curve)::DownCast(curve->handle()));
        }
    }
    entities.push_back(std::move(e));
}

Sketch3DSolver::Entity* Sketch3DSolver::entity(int geoId)
{
    std::size_t i;
    if (geoId >= 0) {
        i = static_cast<std::size_t>(geoId);
        if (i >= internalCount) {
            return nullptr;
        }
    }
    else if (geoId == GeoEnum::GeoUndef) {
        return nullptr;
    }
    else {
        i = internalCount + static_cast<std::size_t>(-geoId - 1);
    }
    return i < entities.size() ? &entities[i] : nullptr;
}

GCS::Point3* Sketch3DSolver::entityPoint(int geoId, PointPos pos)
{
    Entity* e = entity(geoId);
    if (!e) {
        return nullptr;
    }
    switch (e->kind) {
        case Kind::Point:
            return pos == PointPos::end ? nullptr : &e->start;
        case Kind::Line:
        case Kind::BSpline:
        case Kind::Curve:
            if (pos == PointPos::start) {
                return e->start.x ? &e->start : nullptr;
            }
            if (pos == PointPos::end) {
                return e->end.x ? &e->end : nullptr;
            }
            return nullptr;
        case Kind::Circle:
        case Kind::Plane:
            return pos == PointPos::mid ? &e->mid : nullptr;
        case Kind::Arc:
            if (pos == PointPos::start) {
                return &e->start;
            }
            if (pos == PointPos::end) {
                return &e->end;
            }
            return pos == PointPos::mid ? &e->mid : nullptr;
    }
    return nullptr;
}

bool Sketch3DSolver::tangentAt(int geoId, PointPos pos, GCS::Dir3& dir)
{
    Entity* e = entity(geoId);
    if (!e) {
        return false;
    }
    switch (e->kind) {
        case Kind::Line:
            dir = GCS::Dir3::diff(e->start, e->end);
            return true;
        case Kind::Arc:
            if (pos == PointPos::start) {
                dir = GCS::Dir3::crossDiff(e->normal, e->mid, e->start);
                return true;
            }
            if (pos == PointPos::end) {
                dir = GCS::Dir3::crossDiff(e->normal, e->mid, e->end);
                return true;
            }
            return false;
        case Kind::BSpline: {
            // Clamped: the end tangent is along the end pole pair.
            auto& p = e->bsp.poles;
            if (p.size() < 2) {
                return false;
            }
            if (pos == PointPos::start) {
                dir = GCS::Dir3::diff(p[0], p[1]);
                return true;
            }
            if (pos == PointPos::end) {
                dir = GCS::Dir3::diff(p[p.size() - 2], p[p.size() - 1]);
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

double Sketch3DSolver::closestParameter(const Entity& e, const GCS::Point3& p) const
{
    if (e.kind == Kind::Line) {
        Base::Vector3d a = vec(e.start), b = vec(e.end), d = b - a;
        double l2 = d.Sqr();
        return l2 == 0. ? 0. : (vec(p) - a) * d / l2;
    }
    double u = 0.;
    if (auto curve = freecad_cast<const Part::GeomCurve*>(e.source)) {
        curve->closestParameter(vec(p), u);
    }
    return u;
}

bool Sketch3DSolver::addConstraint(const Constraint& c, int tag)
{
    const std::size_t index = static_cast<std::size_t>(tag - 1);
    const bool driving = c.isDriving;

    // A dimension's value: a constant when driving, a driven parameter otherwise.
    auto value = [&](double scale = 1.) {
        double* v = param(c.getValue() / scale, false);
        if (!driving) {
            driven.push_back(v);
        }
        valueParams[index] = v;
        valueScale[index] = scale;
        return v;
    };
    auto pt = [&](int geoId, PointPos pos) {
        return entityPoint(geoId, pos);
    };
    auto kindOf = [&](int geoId) {
        Entity* e = entity(geoId);
        return e ? std::optional<Kind>(e->kind) : std::nullopt;
    };
    auto is = [&](int geoId, Kind k) {
        auto kind = kindOf(geoId);
        return kind && *kind == k;
    };

    switch (c.Type) {
        case Coincident: {
            auto p = pt(c.First, c.FirstPos), q = pt(c.Second, c.SecondPos);
            if (!p || !q) {
                return false;
            }
            GCS::addConstraintP3Coincident(sys, *p, *q, tag, driving);
            return true;
        }
        case PointOnObject: {
            auto p = pt(c.First, c.FirstPos);
            Entity* target = entity(c.Second);
            if (!p || !target) {
                return false;
            }
            switch (target->kind) {
                case Kind::Point:
                    GCS::addConstraintP3Coincident(sys, *p, target->start, tag, driving);
                    return true;
                case Kind::Line:
                    GCS::addConstraintP3OnLine(sys,
                                               *p,
                                               target->start,
                                               target->end,
                                               param(closestParameter(*target, *p), true),
                                               tag,
                                               driving);
                    return true;
                case Kind::Circle:
                    GCS::addConstraintP3OnCircle(sys, *p, target->circle, tag, driving);
                    return true;
                case Kind::Arc:
                    GCS::addConstraintP3OnArc(sys, *p, target->arc, tag, driving);
                    return true;
                case Kind::BSpline:
                    GCS::addConstraintP3OnBSpline(sys,
                                                  *p,
                                                  target->bsp,
                                                  param(closestParameter(*target, *p), true),
                                                  tag,
                                                  driving);
                    return true;
                case Kind::Plane:
                    GCS::addConstraintP3OnPlane(sys, *p, target->plane, tag, driving);
                    return true;
                case Kind::Curve:
                    if (!target->fixedCurve) {
                        return false;
                    }
                    GCS::addConstraintP3OnFixedCurve(sys,
                                                     *p,
                                                     target->fixedCurve,
                                                     param(closestParameter(*target, *p), true),
                                                     tag,
                                                     driving);
                    return true;
            }
            return false;
        }
        case AlongX:
        case AlongY:
        case AlongZ: {
            int axis = static_cast<int>(c.Type) - static_cast<int>(AlongX);
            if (c.Second == GeoEnum::GeoUndef) {
                if (!is(c.First, Kind::Line)) {
                    return false;
                }
                Entity* e = entity(c.First);
                GCS::addConstraintP3AlongAxis(sys, e->start, e->end, axis, tag, driving);
                return true;
            }
            auto p = pt(c.First, c.FirstPos), q = pt(c.Second, c.SecondPos);
            if (!p || !q) {
                return false;
            }
            GCS::addConstraintP3AlongAxis(sys, *p, *q, axis, tag, driving);
            return true;
        }
        case Parallel:
        case Perpendicular: {
            int line = c.First, other = c.Second;
            if (is(line, Kind::Plane)) {
                std::swap(line, other);
            }
            if (!is(line, Kind::Line)) {
                return false;
            }
            Entity* l = entity(line);
            Entity* o = entity(other);
            if (!o) {
                return false;
            }
            auto d = GCS::Dir3::diff(l->start, l->end);
            if (o->kind == Kind::Line) {
                auto d2 = GCS::Dir3::diff(o->start, o->end);
                if (c.Type == Parallel) {
                    GCS::addConstraintParallel3(sys,
                                                d,
                                                d2,
                                                param(GCS::initialParallelRatio(d, d2), true),
                                                tag,
                                                driving);
                }
                else {
                    GCS::addConstraintPerpendicular3(sys, d, d2, tag, driving);
                }
                return true;
            }
            if (o->kind == Kind::Plane) {
                if (c.Type == Parallel) {
                    GCS::addConstraintLineParallelPlane3(sys,
                                                         l->start,
                                                         l->end,
                                                         o->plane,
                                                         tag,
                                                         driving);
                }
                else {
                    // Normal to the plane: parallel to its normal.
                    auto n = GCS::Dir3::planeNormal(o->plane);
                    GCS::addConstraintParallel3(sys,
                                                d,
                                                n,
                                                param(GCS::initialParallelRatio(d, n), true),
                                                tag,
                                                driving);
                }
                return true;
            }
            return false;
        }
        case Tangent: {
            if (c.FirstPos == PointPos::none && c.SecondPos == PointPos::none) {
                // Two lines: collinear, as in 2D.
                if (!is(c.First, Kind::Line) || !is(c.Second, Kind::Line)) {
                    return false;
                }
                Entity* a = entity(c.First);
                Entity* b = entity(c.Second);
                GCS::addConstraintCollinear3(sys,
                                             a->start,
                                             a->end,
                                             b->start,
                                             b->end,
                                             param(closestParameter(*a, b->start), true),
                                             param(closestParameter(*a, b->end), true),
                                             tag,
                                             driving);
                return true;
            }
            // End to end: joined, and the tangents there parallel.
            auto p = pt(c.First, c.FirstPos), q = pt(c.Second, c.SecondPos);
            GCS::Dir3 t1, t2;
            if (!p || !q || !tangentAt(c.First, c.FirstPos, t1)
                || !tangentAt(c.Second, c.SecondPos, t2)) {
                return false;
            }
            GCS::addConstraintP3Coincident(sys, *p, *q, tag, driving);
            GCS::addConstraintTangent3(sys,
                                       t1,
                                       t2,
                                       param(GCS::initialParallelRatio(t1, t2), true),
                                       tag,
                                       driving);
            return true;
        }
        case Equal: {
            Entity* a = entity(c.First);
            Entity* b = entity(c.Second);
            if (!a || !b) {
                return false;
            }
            if (a->kind == Kind::Line && b->kind == Kind::Line) {
                GCS::addConstraintEqualLength3(sys,
                                               a->start,
                                               a->end,
                                               b->start,
                                               b->end,
                                               tag,
                                               driving);
                return true;
            }
            if (a->kind == Kind::Arc && b->kind == Kind::Circle) {
                std::swap(a, b);
            }
            if (a->kind == Kind::Circle && b->kind == Kind::Circle) {
                sys.addConstraintEqual(a->rad, b->rad, tag, driving);
                return true;
            }
            if (a->kind == Kind::Circle && b->kind == Kind::Arc) {
                GCS::addConstraintP3Distance(sys, b->start, b->mid, a->rad, tag, driving);
                return true;
            }
            if (a->kind == Kind::Arc && b->kind == Kind::Arc) {
                GCS::addConstraintEqualLength3(sys,
                                               a->mid,
                                               a->start,
                                               b->mid,
                                               b->start,
                                               tag,
                                               driving);
                return true;
            }
            return false;
        }
        case Symmetric: {
            // About a point: the midpoint. About a line or a plane is v2.
            auto p = pt(c.First, c.FirstPos), q = pt(c.Second, c.SecondPos);
            auto m = pt(c.Third, c.ThirdPos);
            if (!p || !q || !m || c.ThirdPos == PointPos::none) {
                return false;
            }
            GCS::addConstraintP3Midpoint(sys, *m, *p, *q, tag, driving);
            return true;
        }
        case Distance: {
            if (c.Second == GeoEnum::GeoUndef) {
                if (!is(c.First, Kind::Line)) {
                    return false;
                }
                Entity* e = entity(c.First);
                GCS::addConstraintP3Distance(sys, e->start, e->end, value(), tag, driving);
                return true;
            }
            auto p = pt(c.First, c.FirstPos);
            if (!p) {
                return false;
            }
            if (c.SecondPos != PointPos::none) {
                auto q = pt(c.Second, c.SecondPos);
                if (!q) {
                    return false;
                }
                GCS::addConstraintP3Distance(sys, *p, *q, value(), tag, driving);
                return true;
            }
            Entity* o = entity(c.Second);
            if (!o) {
                return false;
            }
            if (o->kind == Kind::Line) {
                GCS::addConstraintP3LineDistance(sys, *p, o->start, o->end, value(), tag, driving);
                return true;
            }
            if (o->kind == Kind::Plane) {
                GCS::addConstraintP3PlaneDistance(sys, *p, o->plane, value(), tag, driving);
                return true;
            }
            return false;
        }
        case DistanceX:
        case DistanceY:
        case DistanceZ: {
            int axis = c.Type == DistanceX ? 0 : (c.Type == DistanceY ? 1 : 2);
            if (c.Second == GeoEnum::GeoUndef) {
                if (c.FirstPos == PointPos::none) {
                    if (!is(c.First, Kind::Line)) {
                        return false;
                    }
                    Entity* e = entity(c.First);
                    GCS::addConstraintP3AxisDistance(sys,
                                                     e->start,
                                                     e->end,
                                                     axis,
                                                     value(),
                                                     tag,
                                                     driving);
                    return true;
                }
                // A point's coordinate.
                auto p = pt(c.First, c.FirstPos);
                if (!p) {
                    return false;
                }
                GCS::addConstraintP3AxisDistance(sys, origin, *p, axis, value(), tag, driving);
                return true;
            }
            auto p = pt(c.First, c.FirstPos), q = pt(c.Second, c.SecondPos);
            if (!p || !q) {
                return false;
            }
            GCS::addConstraintP3AxisDistance(sys, *p, *q, axis, value(), tag, driving);
            return true;
        }
        case Angle: {
            if (!is(c.First, Kind::Line) || !is(c.Second, Kind::Line)) {
                return false;
            }
            Entity* a = entity(c.First);
            Entity* b = entity(c.Second);
            GCS::addConstraintAngle3(sys,
                                     GCS::Dir3::diff(a->start, a->end),
                                     GCS::Dir3::diff(b->start, b->end),
                                     value(),
                                     tag,
                                     driving);
            return true;
        }
        case Radius:
        case Diameter: {
            double scale = c.Type == Diameter ? 2. : 1.;
            Entity* e = entity(c.First);
            if (!e) {
                return false;
            }
            if (e->kind == Kind::Circle) {
                GCS::addConstraintCircle3Radius(sys, e->circle, value(scale), tag, driving);
                return true;
            }
            if (e->kind == Kind::Arc) {
                GCS::addConstraintArc3Radius(sys, e->arc, value(scale), tag, driving);
                return true;
            }
            return false;
        }
        case InternalAlignment: {
            auto p = pt(c.First, PointPos::start);
            Entity* s = entity(c.Second);
            int k = c.InternalAlignmentIndex;
            if (!p || !s || s->kind != Kind::BSpline || k < 0) {
                return false;
            }
            if (c.AlignmentType == BSplineControlPoint) {
                if (static_cast<std::size_t>(k) >= s->bsp.poles.size()) {
                    return false;
                }
                GCS::addConstraintP3Coincident(sys, *p, s->bsp.poles[k], tag, driving);
                return true;
            }
            if (c.AlignmentType == BSplineKnotPoint) {
                if (static_cast<std::size_t>(k) >= s->distinctKnots.size()) {
                    return false;
                }
                GCS::addConstraintP3OnBSpline(sys,
                                              *p,
                                              s->bsp,
                                              param(s->distinctKnots[k], false),
                                              tag,
                                              driving);
                return true;
            }
            return false;
        }
        default:
            // Horizontal / Vertical need a plane (v2), and the rest are 2D only.
            return false;
    }
}

int Sketch3DSolver::setUp(const std::vector<const Part::Geometry*>& geometry,
                          const std::vector<const Part::Geometry*>& external,
                          const std::vector<Constraint*>& constraints)
{
    sys.clear();
    params.clear();
    unknowns.clear();
    driven.clear();
    entities.clear();
    conflicting.clear();
    redundant.clear();
    partiallyRedundant.clear();
    malformed.clear();
    valueParams.assign(constraints.size(), nullptr);
    valueScale.assign(constraints.size(), 1.);
    dofs = -1;

    origin = point(Base::Vector3d(0, 0, 0), false);

    std::set<int> blocked;
    for (auto* c : constraints) {
        if (c->Type == Block && c->isActive && c->First >= 0) {
            blocked.insert(c->First);
        }
    }
    internalCount = geometry.size();
    entities.reserve(geometry.size() + external.size());
    for (std::size_t i = 0; i < geometry.size(); ++i) {
        addEntity(geometry[i], false, blocked.count(static_cast<int>(i)) > 0);
    }
    for (auto* geo : external) {
        addEntity(geo, true, true);
    }

    for (std::size_t i = 0; i < constraints.size(); ++i) {
        const Constraint* c = constraints[i];
        if (!c->isActive || c->Type == Block) {
            continue;
        }
        if (!addConstraint(*c, static_cast<int>(i + 1))) {
            malformed.push_back(static_cast<int>(i));
        }
    }

    if (unknowns.empty()) {
        dofs = 0;
        return dofs;
    }
    sys.declareUnknowns(unknowns);
    sys.declareDrivenParams(driven);
    sys.initSolution();
    dofs = sys.dofsNumber();

    auto toIndices = [](const GCS::VEC_I& tags, std::vector<int>& out) {
        out.clear();
        for (int t : tags) {
            if (t > 0) {
                out.push_back(t - 1);
            }
        }
    };
    GCS::VEC_I tags;
    sys.getConflicting(tags);
    toIndices(tags, conflicting);
    sys.getRedundant(tags);
    toIndices(tags, redundant);
    sys.getPartiallyRedundant(tags);
    toIndices(tags, partiallyRedundant);
    return dofs;
}

int Sketch3DSolver::solve()
{
    if (unknowns.empty()) {
        return 0;
    }
    for (auto alg : {GCS::DogLeg, GCS::LevenbergMarquardt, GCS::BFGS}) {
        if (sys.solve(alg) == GCS::SolveStatus::Success) {
            sys.applySolution();
            sys.evaluateDrivenConstraints();
            return 0;
        }
        sys.undoSolution();
    }
    return -1;
}

int Sketch3DSolver::movePoint(int geoId, PointPos pos, const Base::Vector3d& to, bool relative)
{
    Entity* e = entity(geoId);
    if (!e || e->external || unknowns.empty()) {
        return -1;
    }
    // The points that follow the drag: the one asked for, or all a curve's own points.
    std::vector<GCS::Point3*> pts;
    if (pos != PointPos::none) {
        if (auto p = entityPoint(geoId, pos)) {
            pts.push_back(p);
        }
    }
    else {
        for (auto* p : {&e->start, &e->end, &e->mid}) {
            if (p->x && std::find(pts.begin(), pts.end(), p) == pts.end()) {
                pts.push_back(p);
            }
        }
        for (auto& pole : e->bsp.poles) {
            pts.push_back(&pole);
        }
    }
    if (pts.empty()) {
        return -1;
    }
    Base::Vector3d delta = relative ? to : to - vec(*pts.front());
    for (auto* p : pts) {
        GCS::Point3 target = point(vec(*p) + delta, false);
        GCS::addConstraintP3Coincident(sys, target, *p, GCS::DefaultTemporaryConstraint);
    }
    sys.initSolution();
    int res = -1;
    if (sys.solve(GCS::DogLeg) == GCS::SolveStatus::Success) {
        sys.applySolution();
        sys.evaluateDrivenConstraints();
        res = 0;
    }
    else {
        sys.undoSolution();
    }
    sys.clearByTag(GCS::DefaultTemporaryConstraint);
    sys.initSolution();
    return res;
}

bool Sketch3DSolver::getDrivenValue(int constraintIndex, double& value) const
{
    if (constraintIndex < 0 || static_cast<std::size_t>(constraintIndex) >= valueParams.size()
        || !valueParams[constraintIndex]) {
        return false;
    }
    value = *valueParams[constraintIndex] * valueScale[constraintIndex];
    return true;
}

std::vector<std::unique_ptr<Part::Geometry>> Sketch3DSolver::extractGeometry() const
{
    std::vector<std::unique_ptr<Part::Geometry>> res;
    res.reserve(internalCount);
    for (std::size_t i = 0; i < internalCount; ++i) {
        const Entity& e = entities[i];
        std::unique_ptr<Part::Geometry> geo(e.source->clone());
        switch (e.kind) {
            case Kind::Point:
                static_cast<Part::GeomPoint*>(geo.get())->setPoint(vec(e.start));
                break;
            case Kind::Line:
                static_cast<Part::GeomLineSegment*>(geo.get())->setPoints(vec(e.start), vec(e.end));
                break;
            case Kind::Circle: {
                auto circle = static_cast<Part::GeomCircle*>(geo.get());
                auto old = Handle(Geom_Circle)::DownCast(circle->handle());
                Base::Vector3d c = vec(e.mid), n = vec(e.normal);
                gp_Dir dir(n.x, n.y, n.z);
                gp_Dir x = old->Position().XDirection();
                gp_Ax2 ax = std::fabs(dir.Dot(x)) < 0.99 ? gp_Ax2(gp_Pnt(c.x, c.y, c.z), dir, x)
                                                         : gp_Ax2(gp_Pnt(c.x, c.y, c.z), dir);
                circle->setHandle(new Geom_Circle(ax, std::fabs(*e.rad)));
                break;
            }
            case Kind::Arc: {
                // The circle's X axis through the start, so the arc runs from 0.
                auto arc = static_cast<Part::GeomArcOfCircle*>(geo.get());
                Base::Vector3d c = vec(e.mid), n = vec(e.normal), s = vec(e.start) - c,
                               t = vec(e.end) - c;
                double r = s.Length();
                if (r <= 0. || n.Length() <= 0.) {
                    break;
                }
                n.Normalize();
                Base::Vector3d x = s / r, y = n % x;
                double angle = std::atan2(t * y, t * x);
                if (angle <= 0.) {
                    angle += 2 * std::numbers::pi;
                }
                gp_Ax2 ax(gp_Pnt(c.x, c.y, c.z), gp_Dir(n.x, n.y, n.z), gp_Dir(x.x, x.y, x.z));
                arc->setHandle(new Geom_TrimmedCurve(new Geom_Circle(ax, r), 0., angle));
                break;
            }
            case Kind::BSpline: {
                auto bsp = static_cast<Part::GeomBSplineCurve*>(geo.get());
                std::vector<Base::Vector3d> poles;
                std::vector<double> weights;
                for (std::size_t k = 0; k < e.bsp.poles.size(); ++k) {
                    poles.push_back(vec(e.bsp.poles[k]));
                    weights.push_back(*e.bsp.weights[k]);
                }
                bsp->setPoles(poles, weights);
                break;
            }
            case Kind::Plane:
            case Kind::Curve:
                break;
        }
        res.push_back(std::move(geo));
    }
    return res;
}
