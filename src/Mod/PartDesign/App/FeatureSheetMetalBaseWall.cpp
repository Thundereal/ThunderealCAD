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
#include <numbers>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

#include <App/Document.h>
#include <Base/Exception.h>

#include "FeatureSheetMetalBaseWall.h"
#include "FeatureSheetMetalRule.h"

using namespace PartDesign;
using Part::TopoShape;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalBaseWall, PartDesign::ProfileBased)

const char* SheetMetalBaseWall::MaterialSideEnums[] = {"Outside", "Inside", "Middle", nullptr};

SheetMetalBaseWall::SheetMetalBaseWall()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(MaterialSide,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Where the material is against the sketch: Outside, the sketch on the first "
                      "bend's inner skin (a plate: on the side of the sketch's normal); Inside, on "
                      "its outer skin (a plate: against the normal); Middle");
    MaterialSide.setEnums(MaterialSideEnums);
    ADD_PROPERTY_TYPE(Length,
                      (20.0),
                      "Sheet Metal",
                      App::Prop_None,
                      "How far a profile wall goes along the sketch's normal");
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
    for (App::Property* prop : std::initializer_list<App::Property*> {&UpToFace,
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

namespace
{

using SheetMetal::CylBend;
using SheetMetal::Element;
using SheetMetal::Model;

/// A planar face of the given edges, in order, each edge and the face named
TopoShape namedFace(const std::vector<std::pair<TopoDS_Edge, std::string>>& edges,
                    const std::string& name)
{
    BRepBuilderAPI_MakeWire wire;
    for (const auto& edge : edges) {
        wire.Add(edge.first);
    }
    if (!wire.IsDone()) {
        FC_THROWM(Base::CADKernelError, "Sheet metal: no outline for " << name);
    }
    BRepBuilderAPI_MakeFace face(wire.Wire(), true);
    if (!face.IsDone()) {
        FC_THROWM(Base::CADKernelError, "Sheet metal: no face for " << name);
    }
    TopoShape res(face.Face());
    res.setElementName(Data::IndexedName::fromConst("Face", 1), Data::MappedName(name));
    // the wire builder may hand back copies of the edges: find each by place
    for (int i = 1; i <= res.countSubShapes(TopAbs_EDGE); ++i) {
        const TopoDS_Edge e = TopoDS::Edge(res.getSubShape(TopAbs_EDGE, i));
        BRepAdaptor_Curve c(e);
        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        for (const auto& edge : edges) {
            BRepAdaptor_Curve d(edge.first);
            if (d.Value((d.FirstParameter() + d.LastParameter()) / 2).Distance(mid)
                < Precision::Confusion()) {
                res.setElementName(Data::IndexedName::fromConst("Edge", i),
                                   Data::MappedName(edge.second));
                break;
            }
        }
    }
    return res;
}

TopoDS_Edge line(const gp_Pnt& a, const gp_Pnt& b)
{
    return BRepBuilderAPI_MakeEdge(a, b).Edge();
}

/// An arc about the frame's Z, from its X through angle, reversed if asked
TopoDS_Edge arc(const gp_Ax3& frame, double radius, double angle, bool reversed)
{
    const gp_Ax2 ax(frame.Location(), frame.Direction(), frame.XDirection());
    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(gp_Circ(ax, radius), 0, angle).Edge();
    if (reversed) {
        e.Reverse();
    }
    return e;
}

/// The lines of an open wire as points in order, lines in a row along one
/// direction made one
std::vector<gp_Pnt> pointsOf(const TopoDS_Wire& wire)
{
    std::vector<gp_Pnt> points;
    for (BRepTools_WireExplorer it(wire); it.More(); it.Next()) {
        const TopoDS_Edge& edge = it.Current();
        if (BRepAdaptor_Curve(edge).GetType() != GeomAbs_Line) {
            THROWM(Base::ValueError, "A profile wall is made of lines only");
        }
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(edge, v1, v2, true);
        if (points.empty()) {
            points.push_back(BRep_Tool::Pnt(v1));
        }
        points.push_back(BRep_Tool::Pnt(v2));
    }
    // drop a point between two lines going on in one direction
    for (std::size_t i = 1; i + 1 < points.size();) {
        const gp_Vec a(points[i - 1], points[i]), b(points[i], points[i + 1]);
        if (a.Magnitude() > Precision::Confusion() && b.Magnitude() > Precision::Confusion()
            && a.IsParallel(b, Precision::Angular()) && a.Dot(b) > 0) {
            points.erase(points.begin() + static_cast<long>(i));
        }
        else {
            ++i;
        }
    }
    return points;
}

struct Builder
{
    double thickness = 0;
    double radius = 0;
    const SheetMetalRule* rule = nullptr;
    Model model;
    int walls = 0;
    int bends = 0;
    std::vector<TopoShape> faces;

    std::string nextWall()
    {
        return "Wall" + std::to_string(++walls);
    }
    std::string nextBend()
    {
        return "Bend" + std::to_string(++bends);
    }

    /// K for the model's fold map: what gives the rule's allowance
    double kFor(double angle) const
    {
        const double ba = SheetMetal::bendAllowance(rule->getBend(angle, radius),
                                                    rule->getAllowance());
        return (ba / angle - radius) / thickness;
    }

    /// The pieces of a profile wall on the sketch's plane, normal n;
    /// side 0 outside, 1 inside, 2 middle
    void profile(const std::vector<gp_Pnt>& p, const gp_Dir& n, int side)
    {
        const std::size_t count = p.size() - 1;
        std::vector<gp_Dir> d, l;
        for (std::size_t i = 0; i < count; ++i) {
            const gp_Vec v(p[i], p[i + 1]);
            if (v.Magnitude() < Precision::Confusion()) {
                THROWM(Base::ValueError, "A profile wall has a line of no length");
            }
            d.emplace_back(v);
            l.push_back(n.Crossed(d.back()));
        }
        // the turn at each corner, signed about the normal
        std::vector<double> turn(count, 0);
        for (std::size_t j = 1; j < count; ++j) {
            turn[j] = std::atan2(d[j - 1].Crossed(d[j]).Dot(n), d[j - 1].Dot(d[j]));
            if (std::abs(turn[j]) > std::numbers::pi - 1e-6) {
                THROWM(Base::ValueError, "A profile wall cannot turn back on itself");
            }
        }
        // the material from a to b along each line's left
        const bool left = count < 2 || turn[1] > 0;
        double a = -thickness / 2, b = thickness / 2;
        if (side == 0) {
            a = left ? -thickness : 0;
        }
        else if (side == 1) {
            a = left ? 0 : -thickness;
        }
        if (side != 2) {
            b = a + thickness;
        }
        // where each line's straight part starts and ends, and the bends' axes
        std::vector<gp_Pnt> start(p.begin(), p.end() - 1), end(p.begin() + 1, p.end());
        std::vector<gp_Pnt> center(count);
        for (std::size_t j = 1; j < count; ++j) {
            const double offset = turn[j] > 0 ? b + radius : a - radius;
            const gp_Vec miter = (gp_Vec(l[j - 1]) + gp_Vec(l[j]))
                * (offset / (1 + d[j - 1].Dot(d[j])));
            center[j] = p[j].Translated(miter);
            end[j - 1] = p[j].Translated(gp_Vec(d[j - 1]) * miter.Dot(gp_Vec(d[j - 1])));
            start[j] = p[j].Translated(gp_Vec(d[j]) * miter.Dot(gp_Vec(d[j])));
        }
        int previous = -1;
        for (std::size_t i = 0; i < count; ++i) {
            if (gp_Vec(start[i], end[i]).Dot(gp_Vec(d[i])) < Precision::Confusion()) {
                FC_THROWM(Base::ValueError,
                          "Line " << i + 1 << " of the profile is too short for its bends");
            }
            const gp_Vec la = gp_Vec(l[i]) * a, lb = gp_Vec(l[i]) * b;
            const std::string name = nextWall();
            const int wall = model.addWall(name, gp_Ax3(start[i].Translated(la), l[i], d[i]));
            faces.push_back(namedFace(
                {{line(start[i].Translated(la), end[i].Translated(la)), name + "Skin1"},
                 {line(end[i].Translated(la), end[i].Translated(lb)), name + "End"},
                 {line(end[i].Translated(lb), start[i].Translated(lb)), name + "Skin2"},
                 {line(start[i].Translated(lb), start[i].Translated(la)), name + "Start"}},
                name));
            if (i > 0) {
                bend(center[i], l[i - 1], n, turn[i], previous, wall);
            }
            previous = wall;
        }
    }

    /// A bend about the axis through c along n, from the wall before (its
    /// left l) through the turn
    void bend(const gp_Pnt& c, const gp_Dir& l, const gp_Dir& n, double turn, int before, int after)
    {
        const bool left = turn > 0;
        CylBend bend;
        bend.frame = gp_Ax3(c, left ? n : n.Reversed(), left ? l.Reversed() : l);
        bend.radius = radius;
        bend.thickness = thickness;
        bend.angle = std::abs(turn);
        bend.k = kFor(bend.angle);
        const std::string name = nextBend();
        model.addBend(name, bend, before, after);
        const gp_Dir x = bend.frame.XDirection();
        const gp_Dir x2 = x.Rotated(gp_Ax1(c, bend.frame.Direction()), bend.angle);
        const double outer = radius + thickness;
        faces.push_back(namedFace(
            {{line(c.Translated(gp_Vec(x) * outer), c.Translated(gp_Vec(x) * radius)),
              name + "Start"},
             {arc(bend.frame, radius, bend.angle, false), name + "Inner"},
             {line(c.Translated(gp_Vec(x2) * radius), c.Translated(gp_Vec(x2) * outer)),
              name + "End"},
             {arc(bend.frame, outer, bend.angle, true), name + "Outer"}},
            name));
    }
};

}  // namespace

App::DocumentObjectExecReturn* SheetMetalBaseWall::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    Builder build;
    build.rule = rule;
    build.thickness = rule->Thickness.getValue();
    build.radius = rule->BendRadius.getValue();
    if (build.thickness < Precision::Confusion()) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no thickness"));
    }
    try {
        build.model = modelBefore(build.thickness);
        build.model.thickness = build.thickness;
        for (const Element& e : build.model.elements) {
            ++(e.kind == Element::Kind::Wall ? build.walls : build.bends);
        }

        TopoShape base = getBaseShape(/*silent*/ true);
        auto invObjLoc = positionByPrevious();
        base.move(invObjLoc);
        const Base::Vector3d sketchNormal = getProfileNormal();
        gp_Vec n(sketchNormal.x, sketchNormal.y, sketchNormal.z);
        n.Transform(invObjLoc.Transformation());
        const gp_Dir normal(n);
        const int side = MaterialSide.getValue();

        TopoShape profile = getProfileShape();
        profile.move(invObjLoc);
        bool closed = profile.hasSubShape(TopAbs_WIRE);
        for (const auto& wire : profile.getSubTopoShapes(TopAbs_WIRE)) {
            closed = closed && BRep_Tool::IsClosed(wire.getShape());
        }

        const auto hasher = getDocument()->getStringHasher();
        TopoShape tool;
        if (closed) {
            // a plate: the sketch's face through the thickness
            TopoShape face = getVerifiedFace();
            face.move(invObjLoc);
            gp_Vec along = gp_Vec(normal) * build.thickness;
            gp_Pnt origin = BRep_Tool::Pnt(TopoDS::Vertex(face.getSubShape(TopAbs_VERTEX, 1)));
            gp_Dir into = normal;
            if (side == 1) {
                along.Reverse();
                into.Reverse();
            }
            else if (side == 2) {
                gp_Trsf half;
                half.SetTranslation(gp_Vec(normal) * (-build.thickness / 2));
                face = face.makETransform(half);
                origin.Transform(half);
            }
            build.model.addWall(build.nextWall(), gp_Ax3(origin, into));
            tool = TopoShape(0, hasher).makEPrism(face, along);
        }
        else {
            auto wires = profile.getSubTopoShapes(TopAbs_WIRE);
            if (wires.empty()) {
                wires = profile.makEWires().getSubTopoShapes(TopAbs_WIRE);
            }
            if (wires.size() != 1) {
                return new App::DocumentObjectExecReturn(QT_TRANSLATE_NOOP(
                    "Exception", "A profile wall takes one open line chain, or closed profiles"));
            }
            const auto points = pointsOf(TopoDS::Wire(wires.front().getShape()));
            build.profile(points, normal, side);

            const double length = Length.getValue();
            if (length < Precision::Confusion()) {
                return new App::DocumentObjectExecReturn(
                    QT_TRANSLATE_NOOP("Exception", "Length too small"));
            }
            gp_Vec along = gp_Vec(normal) * (Reversed.getValue() ? -length : length);
            gp_Trsf back;
            if (Midplane.getValue()) {
                back.SetTranslation(along * -0.5);
            }
            std::vector<TopoShape> pieces;
            for (auto& face : build.faces) {
                if (Midplane.getValue()) {
                    face = face.makETransform(back);
                }
                pieces.push_back(TopoShape(0, hasher).makEPrism(face, along));
            }
            if (Midplane.getValue()) {
                build.model.transform(back);
            }
            if (pieces.size() == 1) {
                tool = pieces.front();
            }
            else {
                tool = TopoShape(0, hasher).makEFuse(pieces);
                tool = getSolid(tool);
            }
        }
        if (tool.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "The sheet came out empty"));
        }

        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        Shape.setValue(makeBoolean(base, tool));
        SheetModel.setValue(build.model);
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}
