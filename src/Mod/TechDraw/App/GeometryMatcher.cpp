/***************************************************************************
 *   Copyright (c) 2023 WandererFan <wandererfan@gmail.com>                *
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
// a class to handle changes to dimension reference geometry

// detects changes in reference geometry (2d & 3d) that would invalidate a dimension
// identifies replacement view/model geometry

#include "PreCompiled.h"
#ifndef _PreComp_
#endif

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepLProp_CLProps.hxx>
#include <BRep_Tool.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <TopExp.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>

#include <Base/Console.h>
#include <Base/Tools.h>

#include <Mod/Part/App/TopoShape.h>

#include "GeometryMatcher.h"
#include "DrawUtil.h"

using namespace TechDraw;
using DU = DrawUtil;

namespace {
bool liesOn(const TopoDS_Edge& edge, const gp_Pnt& point)
{
    BRepExtrema_DistShapeShape dist(edge, BRepBuilderAPI_MakeVertex(point));
    return dist.IsDone() && dist.NbSolution() > 0 && dist.Value() < EWTOLERANCE;
}

bool alongLiesOn(const TopoDS_Edge& from, const TopoDS_Edge& onto)
{
    BRepAdaptor_Curve curve(from);
    const double first = curve.FirstParameter();
    const double last = curve.LastParameter();
    for (double at : {0.25, 0.5, 0.75}) {
        if (!liesOn(onto, curve.Value(first + at * (last - first)))) {
            return false;
        }
    }
    return true;
}
}  // namespace

// a set of routines for comparing geometry for equality.

bool GeometryMatcher::compareGeometry(Part::TopoShape shape1,  Part::TopoShape shape2)
{
//    Base::Console().Message("GM::compareGeometry()\n");
    if (shape1.isNull() || shape2.isNull()) {
//        Base::Console().Message("GM::compareGeometry - one or more TopoShapes are null\n");
        return false;
    }
    TopoDS_Shape geom1 = shape1.getShape();
    TopoDS_Shape geom2 = shape2.getShape();
    if (geom1.IsNull() || geom2.IsNull()) {
//        Base::Console().Message("GM::compareGeometry - one or more TopoDS_Shapes are null\n");
        return false;
    }

    if (geom1.ShapeType() == TopAbs_VERTEX) {
        return comparePoints(geom1, geom2);
    }
    if (geom1.ShapeType() == TopAbs_EDGE) {
        return compareEdges(geom1, geom2);
    }
    return false;
}

bool GeometryMatcher::comparePoints(TopoDS_Shape &shape1,  TopoDS_Shape &shape2)
{
//    Base::Console().Message("GM::comparePoints()\n");
    if (shape1.ShapeType() != TopAbs_VERTEX ||
        shape2.ShapeType() != TopAbs_VERTEX) {
        // can not compare these shapes
        return false;
    }
    auto vert1 = TopoDS::Vertex(shape1);
    Base::Vector3d point1 = DU::toVector3d(BRep_Tool::Pnt(vert1));
    auto vert2 = TopoDS::Vertex(shape2);
    Base::Vector3d point2 = DU::toVector3d(BRep_Tool::Pnt(vert2));
    if (point1.IsEqual(point2, EWTOLERANCE)) {
        return true;
    }
    return false;
}

bool GeometryMatcher::compareEdges(TopoDS_Shape &shape1,  TopoDS_Shape &shape2)
{
//    Base::Console().Message("GM::compareEdges()\n");
    if (shape1.ShapeType() != TopAbs_EDGE ||
        shape2.ShapeType() != TopAbs_EDGE) {
        // can not compare these shapes
//        Base::Console().Message("GM::compareEdges - shape is not an edge\n");
        return false;
    }
    TopoDS_Edge edge1 = TopoDS::Edge(shape1);
    TopoDS_Edge edge2 = TopoDS::Edge(shape2);
   if (edge1.IsNull() || edge2.IsNull()) {
//        Base::Console().Message("GM::compareEdges - an input edge is null\n");
        return false;
    }

    BRepAdaptor_Curve adapt1(edge1);
    BRepAdaptor_Curve adapt2(edge2);

    if (adapt1.GetType() == GeomAbs_Line &&
        adapt2.GetType() == GeomAbs_Line) {
        return compareLines(edge1, edge2);
    }

    if (adapt1.GetType() == GeomAbs_Circle &&
        adapt2.GetType() == GeomAbs_Circle) {
        if (adapt1.IsClosed() && adapt2.IsClosed()) {
            return compareCircles(edge1, edge2);
        } else {
            return compareCircleArcs(edge1, edge2);
        }
    }

    if (adapt1.GetType() == GeomAbs_Ellipse &&
        adapt2.GetType() == GeomAbs_Ellipse) {
        if (adapt1.IsClosed() && adapt2.IsClosed()) {
            return compareEllipses(edge1, edge2);
        } else {
            return compareEllipseArcs(edge1, edge2);
        }
    }

    if (adapt1.GetType() == GeomAbs_BSplineCurve &&
        adapt2.GetType() == GeomAbs_BSplineCurve) {
        return compareBSplines(edge1, edge2);
    }

    // if we reach this point, we have dissimilar geometry types
    return compareDifferent(edge1, edge2);
}

bool GeometryMatcher::compareLines(TopoDS_Edge &edge1, TopoDS_Edge &edge2)
{
//    Base::Console().Message("GM::compareLines()\n");
    // how does the edge that was NOT null in compareEdges become null here?
    // should not happen, but does!
    if (edge1.IsNull() || edge2.IsNull()) {
//        Base::Console().Message("GM::compareLine - an input edge is null\n");
        return false;
    }
    auto start1 = DU::toVector3d(BRep_Tool::Pnt(TopExp::FirstVertex(edge1)));
    auto end1 = DU::toVector3d(BRep_Tool::Pnt(TopExp::LastVertex(edge1)));
    auto start2 = DU::toVector3d(BRep_Tool::Pnt(TopExp::FirstVertex(edge2)));
    auto end2 = DU::toVector3d(BRep_Tool::Pnt(TopExp::LastVertex(edge2)));
    if (start1.IsEqual(start2, EWTOLERANCE) &&
        end1.IsEqual(end2, EWTOLERANCE)) {
        //exact match
        return true;
    }
    // the same segment the other way: a projection may run an edge either way,
    // and OCCT 8's runs some of 7.7's backwards (docs/WorkerProcess.md sec 46)
    return start1.IsEqual(end2, EWTOLERANCE) && end1.IsEqual(start2, EWTOLERANCE);
}

bool GeometryMatcher::compareCircles(TopoDS_Edge &edge1, TopoDS_Edge &edge2)
{
//    Base::Console().Message("GM::compareCircles()\n");
    // how does the edge that was NOT null in compareEdges become null here?
    if (edge1.IsNull() || edge2.IsNull()) {
//        Base::Console().Message("GM::compareCircles - an input edge is null\n");
        return false;
    }

    BRepAdaptor_Curve adapt1(edge1);
    BRepAdaptor_Curve adapt2(edge2);
    gp_Circ circle1 = adapt1.Circle();
    gp_Circ circle2 = adapt2.Circle();
    double radius1 = circle1.Radius();
    double radius2 = circle2.Radius();
    auto center1 = DU::toVector3d(circle1.Location());
    auto center2 = DU::toVector3d(circle2.Location());
    if (DU::fpCompare(radius1, radius2, EWTOLERANCE) &&
        center1.IsEqual(center2, EWTOLERANCE)) {
        //exact match
        return true;
        }
    return false;
}

bool GeometryMatcher::compareEllipses(TopoDS_Edge &edge1, TopoDS_Edge &edge2)
{
    // how does the edge that was NOT null in compareEdges become null here?
    if (edge1.IsNull() || edge2.IsNull()) {
//        Base::Console().Message("GM::compareEllipses - an input edge is null\n");
        return false;
    }

    BRepAdaptor_Curve adapt1(edge1);
    BRepAdaptor_Curve adapt2(edge2);
    gp_Elips ellipse1 = adapt1.Ellipse();
    gp_Elips ellipse2 = adapt2.Ellipse();
    double major1 = ellipse1.MajorRadius();
    double minor1 = ellipse1.MinorRadius();
    double major2 = ellipse2.MajorRadius();
    double minor2 = ellipse2.MinorRadius();
    auto center1 = DU::toVector3d(ellipse1.Location());
    auto center2 = DU::toVector3d(ellipse2.Location());
    if (DU::fpCompare(major1, major2, EWTOLERANCE) &&
        DU::fpCompare(minor1, minor2, EWTOLERANCE) &&
        center1.IsEqual(center2, EWTOLERANCE)) {
        // exact match
        return true;
    }
    return false;
 }

// two B-splines are the same edge when their ends are, either way, and points
// along each lie on the other.  Not by recognising each as a line or a circle:
// a projected edge is often a trimmed piece of a longer B-spline, whose poles
// say nothing of the piece (docs/WorkerProcess.md sec 46)
bool GeometryMatcher::compareBSplines(TopoDS_Edge &edge1, TopoDS_Edge &edge2)
{
//    Base::Console().Message("GM::compareBSplines()\n");
    // how does the edge that was NOT null in compareEdges become null here?
    if (edge1.IsNull() || edge2.IsNull()) {
        Base::Console().Message("GM::compareBSplines - an input edge is null\n");
        return false;
    }
    try {
        return compareEndPoints(edge1, edge2, false) && alongLiesOn(edge1, edge2)
            && alongLiesOn(edge2, edge1);
    }
    catch (Standard_Failure&) {
        return false;
    }
}

// this is a weak comparison.  we should also check center & radius?
bool GeometryMatcher::compareCircleArcs(TopoDS_Edge &edge1, TopoDS_Edge &edge2)
{
    return compareEndPoints(edge1, edge2);
}

bool GeometryMatcher::compareEllipseArcs(TopoDS_Edge &edge1, TopoDS_Edge &edge2)
{
    return compareEndPoints(edge1, edge2);
 }

// this is where we would try to match a bspline against a line or a circle.
// not sure how successful this would be.  For now, we just say it doesn't match
// A line or a circle written as a B-spline is the same edge as the line or the
// circle: OCCT 8's projection writes as B-splines some of what 7.7's wrote as
// lines, and a reference saved then must still find it (docs/WorkerProcess.md
// sec 46).  Told by the points, not by recognising the B-spline: a projected
// edge is often a trimmed piece of a longer one, whose poles say nothing of
// the piece.  The same end points, either way, and points along each lying on
// the other -- which tells the two halves of a circle apart.

bool GeometryMatcher::compareDifferent(TopoDS_Edge &edge1, TopoDS_Edge &edge2)
{
//    Base::Console().Message("GM::compareDifferent()\n");
    BRepAdaptor_Curve adapt1(edge1);
    BRepAdaptor_Curve adapt2(edge2);
    const bool spline1 = adapt1.GetType() == GeomAbs_BSplineCurve;
    const bool spline2 = adapt2.GetType() == GeomAbs_BSplineCurve;
    if (spline1 == spline2) {
        // neither is written otherwise (a line is not a circle), or both are,
        // which compareBSplines takes
        return false;
    }
    const GeomAbs_CurveType plain = spline1 ? adapt2.GetType() : adapt1.GetType();
    if (plain != GeomAbs_Line && plain != GeomAbs_Circle && plain != GeomAbs_Ellipse) {
        return false;
    }
    try {
        return compareEndPoints(edge1, edge2, false) && alongLiesOn(edge1, edge2)
            && alongLiesOn(edge2, edge1);
    }
    catch (Standard_Failure&) {
        return false;
    }
}

bool GeometryMatcher::compareEndPoints(TopoDS_Edge &edge1, TopoDS_Edge &edge2, bool middle)
{
    // how does the edge that was NOT null in compareEdges become null here?
    if (edge1.IsNull() || edge2.IsNull()) {
//        Base::Console().Message("GM::compareLine - an input edge is null\n");
        return false;
    }

    BRepAdaptor_Curve adapt1(edge1);
    BRepAdaptor_Curve adapt2(edge2);
    double pFirst1 = adapt1.FirstParameter();
    double pLast1 = adapt1.LastParameter();
    BRepLProp_CLProps props1(adapt1, pFirst1, 0, Precision::Confusion());
    auto begin1 = DU::toVector3d(props1.Value());
    props1.SetParameter(pLast1);
    auto end1 = DU::toVector3d(props1.Value());
    double pFirst2 = adapt2.FirstParameter();
    double pLast2 = adapt2.LastParameter();
    BRepLProp_CLProps props2(adapt2, pFirst2, 0, Precision::Confusion());
    auto begin2 = DU::toVector3d(props2.Value());
    props2.SetParameter(pLast2);
    auto end2 = DU::toVector3d(props2.Value());

    // the end points either way -- a projection may run an edge backwards
    // (docs/WorkerProcess.md sec 46) -- and the middle: the two halves of a
    // circle have each other's ends, the other way round
    const bool forward = begin1.IsEqual(begin2, EWTOLERANCE) && end1.IsEqual(end2, EWTOLERANCE);
    const bool backward = begin1.IsEqual(end2, EWTOLERANCE) && end1.IsEqual(begin2, EWTOLERANCE);
    if (!forward && !backward) {
        return false;
    }
    if (!middle) {
        return true;
    }
    props1.SetParameter((pFirst1 + pLast1) / 2.0);
    props2.SetParameter((pFirst2 + pLast2) / 2.0);
    return DU::toVector3d(props1.Value()).IsEqual(DU::toVector3d(props2.Value()), EWTOLERANCE);
}
