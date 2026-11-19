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
#include <cstdio>
#include <vector>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepTools.hxx>
#include <ElSLib.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Trsf.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>

#include <Base/Console.h>
#include <Base/Placement.h>
#include <Base/Tools.h>
#include <Mod/Part/App/TopoShape.h>

#include "AnnotationSet.h"
#include "Callout.h"
#include "Drawing.h"
#include "Font.h"
#include "TolerancedFeature.h"
#include "View.h"

FC_LOG_LEVEL_INIT("PMI", true, true)

using namespace PMI;

PROPERTY_SOURCE_ABSTRACT(PMI::Callout, PMI::Annotation)

Callout::Callout()
{
    ADD_PROPERTY_TYPE(Plane, (Base::Placement()), "Presentation", App::Prop_None,
                      "The annotation plane: x the reading direction, y up, z toward the reader");
    ADD_PROPERTY_TYPE(TextHeight, (3.5), "Presentation", App::Prop_None,
                      "The capital height of its text");
    ADD_PROPERTY_TYPE(AutoPlace, (true), "Presentation", App::Prop_None,
                      "Place it from its feature; off once it is moved");
    ADD_PROPERTY_TYPE(ShowLeader, (true), "Presentation", App::Prop_None,
                      "A leader to the feature");
    ADD_PROPERTY_TYPE(AttachedTo, (nullptr), "Presentation", App::Prop_None,
                      "Stacked under this callout: its plane, below it, on its leader");
    ADD_PROPERTY_TYPE(Views, (), "Presentation", App::Prop_None,
                      "The PMI views showing it; the first with AnnotationPlane gives its plane");
    ADD_PROPERTY_TYPE(Presentation, (TopoDS_Shape()), "Presentation",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Transient),
                      "What is drawn: strokes as edges, fills as faces");
    ADD_PROPERTY_TYPE(EquivalentText, (""), "Presentation",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Output),
                      "The equivalent unicode string (CAx-IF), for checking");
}

void Callout::onChanged(const App::Property* prop)
{
    // Moved by the user: from now on where it was put
    if (prop == &Plane && !_placing && !isRestoring() && AutoPlace.getValue()) {
        AutoPlace.setValue(false);
    }
    // Only under another callout, and never in a loop
    if (prop == &AttachedTo && AttachedTo.getValue() && !attachedParent() && !isRestoring()) {
        FC_WARN(getFullName() << " cannot be stacked under "
                              << AttachedTo.getValue()->getFullName()
                              << ": not a callout, or a loop");
        AttachedTo.setValue(nullptr);
    }
    Annotation::onChanged(prop);
}

void Callout::onDocumentRestored()
{
    Annotation::onDocumentRestored();
    bool touched = isTouched();
    try {
        updatePresentation();
    }
    catch (...) {
    }
    if (!touched) {
        purgeTouched();
    }
}

std::u32string Callout::number(double value, int decimals)
{
    char buf[64];
    if (decimals >= 0) {
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    }
    else {
        std::snprintf(buf, sizeof(buf), "%.6f", value);
        std::string s(buf);
        auto dot = s.find('.');
        if (dot != std::string::npos) {
            while (!s.empty() && s.back() == '0') {
                s.pop_back();
            }
            if (!s.empty() && s.back() == '.') {
                s.pop_back();
            }
        }
        std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    }
    std::string s(buf);
    if (s == "-0") {
        s = "0";
    }
    return std::u32string(s.begin(), s.end());
}

namespace
{

// A point on a planar face, in its material: the box centre where that is on
// the face, else of a grid over the face the point deepest inside it -- the
// farthest from its edges. The point nearest the centre would do for a plain
// face; for one with a hole in its middle it is on the hole's rim, and the
// leader would point at the hole (docs/GDandT.md sec 6.4).
bool insidePlanarFace(const TopoDS_Face& face, const gp_Pnt& centre, gp_Pnt& target)
{
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Plane) {
        return false;
    }
    const double tol = BRep_Tool::Tolerance(face);
    gp_Pln pln = surf.Plane();
    double u, v;
    ElSLib::Parameters(pln, centre, u, v);
    BRepClass_FaceClassifier cls(face, gp_Pnt2d(u, v), tol);
    if (cls.State() == TopAbs_IN) {
        target = ElSLib::Value(u, v, pln);
        return true;
    }

    std::vector<gp_Pnt> rim;
    for (TopExp_Explorer xp(face, TopAbs_EDGE); xp.More(); xp.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(xp.Current()));
        const int n = 32;
        for (int i = 0; i <= n; ++i) {
            rim.push_back(c.Value(c.FirstParameter()
                                  + (c.LastParameter() - c.FirstParameter()) * i / n));
        }
    }
    if (rim.empty()) {
        return false;
    }
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const int n = 24;
    BRepTopAdaptor_FClass2d in(face, tol);
    double best = -1;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            gp_Pnt2d uv(u0 + (u1 - u0) * (i + 0.5) / n, v0 + (v1 - v0) * (j + 0.5) / n);
            if (in.Perform(uv) != TopAbs_IN) {
                continue;
            }
            gp_Pnt p = ElSLib::Value(uv.X(), uv.Y(), pln);
            double d = 1e100;
            for (const auto& r : rim) {
                d = std::min(d, p.SquareDistance(r));
            }
            // Ties go to the point nearer the centre, so a symmetric face
            // gets the same side every time
            bool nearer = p.SquareDistance(centre) < target.SquareDistance(centre);
            if (d > best * (1 + 1e-6) || (d > best * (1 - 1e-6) && nearer)) {
                best = d;
                target = p;
            }
        }
    }
    return best > 0;
}

}  // namespace

bool Callout::targetPoint(const TolerancedFeature* feature, gp_Pnt& target)
{
    if (!feature) {
        return false;
    }
    TopoDS_Shape shape = feature->Shape.getShape().getShape();
    if (shape.IsNull()) {
        return false;
    }
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) {
        return false;
    }
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    gp_Pnt centre((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
    if (shape.ShapeType() == TopAbs_FACE && insidePlanarFace(TopoDS::Face(shape), centre, target)) {
        return true;
    }
    if (shape.ShapeType() == TopAbs_COMPOUND) {
        TopExp_Explorer xp(shape, TopAbs_FACE);
        if (xp.More()) {
            TopoDS_Shape first = xp.Current();
            xp.Next();
            if (!xp.More() && insidePlanarFace(TopoDS::Face(first), centre, target)) {
                return true;
            }
        }
    }
    BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(centre).Vertex(), shape);
    if (dist.IsDone() && dist.NbSolution() > 0) {
        target = dist.PointOnShape2(1);
    }
    else {
        target = centre;
    }
    return true;
}

bool Callout::defaultPlane(const TolerancedFeature* feature, double h, gp_Ax3& plane,
                           gp_Pnt& target)
{
    if (!targetPoint(feature, target)) {
        return false;
    }
    const Primitive& p = feature->primitive();
    const gp_Dir axes[] = {gp::DX(), -gp::DX(), gp::DY(), -gp::DY(), gp::DZ(), -gp::DZ()};
    gp_Dir z = gp::DZ();
    if (p.type == Primitive::Plane) {
        z = p.dir;
    }
    else if (p.type == Primitive::Line || feature->getKind() == TolerancedFeature::Axis) {
        // An axis is looked at along itself, from its end toward the
        // reader, and the leader goes to that end -- not to its middle,
        // inside the part
        double best = -1;
        for (auto& a : axes) {
            double d = std::fabs(gp_Vec(a).Dot(gp_Vec(p.dir)));
            if (d > best + 1e-9) {
                best = d;
                z = a;
            }
        }
        z = gp_Vec(z).Dot(gp_Vec(p.dir)) < 0 ? -gp_Vec(z) : gp_Vec(z);
        // Prefer looking down or from the front: a view from below or
        // behind only when the axis says so
        if (z.Z() < -0.5 || z.Y() > 0.5 || z.X() < -0.5) {
            z.Reverse();
        }
        double far = -1e100;
        const TopoDS_Shape shape = feature->Shape.getShape().getShape();
        for (TopExp_Explorer xp(shape, TopAbs_VERTEX); xp.More(); xp.Next()) {
            gp_Pnt v = BRep_Tool::Pnt(TopoDS::Vertex(xp.Current()));
            double d = gp_Vec(v.XYZ()).Dot(gp_Vec(z));
            if (d > far) {
                far = d;
                target = v;
            }
        }
    }
    else {
        // The direction the feature faces at the target, then the standard
        // view looking most squarely along it
        gp_Vec n(0, 0, 1);
        switch (p.type) {
            case Primitive::Cylinder:
            case Primitive::Cone: {
                gp_Vec v(p.loc, target);
                n = v - gp_Vec(p.dir) * v.Dot(gp_Vec(p.dir));
                break;
            }
            case Primitive::Sphere:
                n = gp_Vec(p.loc, target);
                break;
            default:
                break;
        }
        if (n.Magnitude() < 1e-9) {
            n = gp_Vec(0, 0, 1);
        }
        double best = -2;
        for (auto& a : axes) {
            double d = n.Dot(gp_Vec(a)) / n.Magnitude();
            if (d > best) {
                best = d;
                z = a;
            }
        }
    }
    // Up: the world's up where the plane can show it, else its y
    gp_Vec up(0, 0, 1);
    up = up - gp_Vec(z) * up.Dot(gp_Vec(z));
    if (up.Magnitude() < 1e-6) {
        up = gp_Vec(0, 1, 0);
        up = up - gp_Vec(z) * up.Dot(gp_Vec(z));
    }
    gp_Dir y(up);
    gp_Dir x = y.Crossed(z);
    // Off the face a little, and up and to the right of the target
    gp_Pnt origin = target.Translated(gp_Vec(z) * (0.05 * h) + gp_Vec(x) * (3 * h)
                                      + gp_Vec(y) * (2 * h));
    plane = gp_Ax3(origin, z, x);
    return true;
}

Callout* Callout::attachedParent() const
{
    auto parent = freecad_cast<Callout*>(AttachedTo.getValue());
    // A loop is no stack: each would wait on the other
    int depth = 0;
    for (auto c = parent; c; c = freecad_cast<Callout*>(c->AttachedTo.getValue())) {
        if (c == this || ++depth > 64) {
            return nullptr;
        }
    }
    return parent;
}

bool Callout::inView(const View* view) const
{
    for (auto obj : Views.getValues()) {
        if (obj == view) {
            return true;
        }
    }
    return false;
}

View* Callout::planeView() const
{
    for (auto obj : Views.getValues()) {
        auto view = freecad_cast<View*>(obj);
        if (view && view->AnnotationPlane.getValue()) {
            return view;
        }
    }
    return nullptr;
}

bool Callout::autoPlane(double h, gp_Ax3& plane, gp_Pnt& target) const
{
    if (!defaultPlane(calloutFeature(), h, plane, target)) {
        return false;
    }
    if (View* view = planeView()) {
        // In the view's plane through the target, up and to the right of it
        gp_Ax3 f = view->frame();
        gp_Vec x(f.XDirection()), y(f.YDirection()), z(f.Direction());
        plane = gp_Ax3(target.Translated(z * (0.05 * h) + x * (3 * h) + y * (2 * h)),
                       f.Direction(), f.XDirection());
    }
    return true;
}

int Callout::autoSlot(const gp_Ax3& mine, double h) const
{
    auto set = getSet();
    if (!set) {
        return 0;
    }
    int slot = 0;
    for (auto obj : set->Group.getValues()) {
        if (obj == this) {
            break;
        }
        auto other = freecad_cast<Callout*>(obj);
        if (!other || !other->AutoPlace.getValue() || !other->calloutFeature()
            || other->attachedParent()) {
            continue;
        }
        gp_Ax3 ax;
        gp_Pnt t;
        double oh = other->TextHeight.getValue() > 0 ? other->TextHeight.getValue() : h;
        if (!other->autoPlane(oh, ax, t)) {
            continue;
        }
        if (!ax.Direction().IsEqual(mine.Direction(), 1e-6)) {
            continue;
        }
        if (ax.Location().Distance(mine.Location()) < 12.0 * h) {
            ++slot;
        }
    }
    return slot;
}

void Callout::updatePresentation()
{
    double h = TextHeight.getValue();
    if (h <= 0.0) {
        h = 3.5;
    }
    TolerancedFeature* feat = calloutFeature();
    gp_Pnt target;
    bool hasTarget = targetPoint(feat, target);

    Callout* parent = attachedParent();
    if (parent) {
        // Under the parent's drawing, left edges in line, in its plane
        double ph = parent->TextHeight.getValue() > 0 ? parent->TextHeight.getValue() : h;
        Drawing above;
        parent->layout(above, ph);
        Base::Placement pla = parent->Plane.getValue();
        {
            Base::StateLocker guard(_placing);
            if (!pla.isSame(Plane.getValue(), 1e-9)) {
                Plane.setValue(pla);
            }
        }
        Drawing mine;
        layout(mine, h);
        if (!above.isEmpty() && !mine.isEmpty()) {
            pla = pla
                * Base::Placement(Base::Vector3d(above.xmin - mine.xmin,
                                                 above.ymin - 0.5 * h - mine.ymax, 0),
                                  Base::Rotation());
            Base::StateLocker guard(_placing);
            if (!pla.isSame(Plane.getValue(), 1e-9)) {
                Plane.setValue(pla);
            }
        }
    }
    else if (AutoPlace.getValue() && hasTarget) {
        gp_Ax3 ax;
        if (autoPlane(h, ax, target)) {
            // Callouts landing together on parallel planes are stacked, each
            // a slot up, in the set's order -- deterministic, whatever order
            // they recompute in (docs/GDandT.md sec 6.4)
            int slot = autoSlot(ax, h);
            if (slot > 0) {
                ax.Translate(gp_Vec(ax.YDirection()) * (slot * 3.0 * h));
            }
            gp_Trsf trsf;
            trsf.SetTransformation(ax);
            trsf.Invert();
            Base::Matrix4D mat;
            Part::TopoShape::convertToMatrix(trsf, mat);
            Base::Placement pla(mat);
            if (!pla.isSame(Plane.getValue(), 1e-9)) {
                Base::StateLocker guard(_placing);
                Plane.setValue(pla);
            }
        }
    }

    Drawing d;
    layout(d, h);

    // The leader, in the plane: the target as the plane sees it (a target
    // off the plane is drawn where it projects, CAx-IF sec 8.3)
    Base::Matrix4D toPlane = Plane.getValue().toMatrix();
    toPlane.inverseOrthogonal();
    if (!parent && ShowLeader.getValue() && hasTarget && !d.isEmpty()
        && leaderStyle() != NoLeader) {
        Base::Vector3d t = toPlane * Base::Vector3d(target.X(), target.Y(), target.Z());
        double my = (d.ymin + d.ymax) / 2;
        double mx = (d.xmin + d.xmax) / 2;
        double sx, sy;
        if (leaderStyle() == TriangleLeader) {
            // From the datum box's middle, below or above
            sx = mx;
            sy = t.y < my ? d.ymin : d.ymax;
        }
        else {
            sx = std::fabs(t.x - d.xmin) < std::fabs(t.x - d.xmax) ? d.xmin : d.xmax;
            sy = my;
        }
        double dx = t.x - sx, dy = t.y - sy;
        double len = std::hypot(dx, dy);
        if (len > 1e-9) {
            if (leaderStyle() == TriangleLeader) {
                double s = h;
                double ux = dx / len, uy = dy / len;
                double bx = t.x - ux * s, by = t.y - uy * s;
                double w = s * 0.6;
                d.line(sx, sy, bx, by);
                d.fill({{t.x - uy * w, t.y + ux * w}, {t.x + uy * w, t.y - ux * w}, {bx, by}});
            }
            else {
                d.line(sx, sy, t.x, t.y);
                d.arrow(t.x, t.y, dx, dy, h);
            }
        }
    }

    TopoDS_Shape shape = d.shape();
    gp_Trsf trsf;
    Part::TopoShape::convertTogpTrsf(Plane.getValue().toMatrix(), trsf);
    Presentation.setValue(shape.Moved(TopLoc_Location(trsf)));
    std::string text = Font::toUtf8(equivalentText());
    if (EquivalentText.getStrValue() != text) {
        EquivalentText.setValue(text);
    }
}
