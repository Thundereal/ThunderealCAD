// SPDX-License-Identifier: LGPL-2.1-or-later

// TopoShape::searchSubShape by geometry: what a reference whose name went is
// found again by (PropertyLinkBase::_updateElementReference, a map version's
// regeneration and a missing element alike).

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>

#include <Mod/Part/App/TopoShape.h>

#include "src/App/InitApplication.h"

using Part::TopoShape;

class TopoShapeSearchTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    static std::vector<std::string> search(const TopoShape& shape, const TopoDS_Shape& sub)
    {
        std::vector<std::string> names;
        shape.searchSubShape(sub, &names, Data::SearchOption::CheckGeometry);
        return names;
    }
};

// The two halves of a circle have one curve and both vertices; the halves of
// a cylinder their surface, their vertices and their edges' curves. Each
// finds itself alone: with the other one too, a reference followed by
// geometry took whichever came first (scanner.FCStd's Pocket041, whose
// profile of four half circles became two edges twice, and no face)
TEST_F(TopoShapeSearchTest, halvesAreToldApart)
{
    // Each half from 0 to pi of a circle of its own, the second's turned
    // half a turn: their trimmed curves compare the same as well
    const gp_Dir up(0, 0, 1);
    Handle(Geom_Circle) circle = new Geom_Circle(gp_Ax2(gp_Pnt(), up, gp_Dir(1, 0, 0)), 5.0);
    Handle(Geom_Circle) turned = new Geom_Circle(gp_Ax2(gp_Pnt(), up, gp_Dir(-1, 0, 0)), 5.0);
    TopoDS_Edge first = BRepBuilderAPI_MakeEdge(circle, 0.0, std::numbers::pi).Edge();
    TopoDS_Edge second = BRepBuilderAPI_MakeEdge(turned,
                                                 TopExp::LastVertex(first),
                                                 TopExp::FirstVertex(first),
                                                 0.0,
                                                 std::numbers::pi)
                             .Edge();
    TopoShape ring {BRepBuilderAPI_MakeWire(first, second).Wire()};
    ASSERT_EQ(ring.countSubShapes(TopAbs_EDGE), 2u);
    for (int i = 1; i <= 2; ++i) {
        auto half = BRepBuilderAPI_Copy(ring.getSubShape(TopAbs_EDGE, i)).Shape();
        EXPECT_EQ(search(ring, half), std::vector<std::string> {"Edge" + std::to_string(i)});
    }

    Handle(Geom_CylindricalSurface) surface =
        new Geom_CylindricalSurface(gp_Ax3(gp_Pnt(), up, gp_Dir(1, 0, 0)), 5.0);
    Handle(Geom_CylindricalSurface) turnedSurface =
        new Geom_CylindricalSurface(gp_Ax3(gp_Pnt(), up, gp_Dir(-1, 0, 0)), 5.0);
    TopoDS_Face front =
        BRepBuilderAPI_MakeFace(surface, 0.0, std::numbers::pi, 0.0, 10.0, 1e-7).Face();
    TopoDS_Face back =
        BRepBuilderAPI_MakeFace(turnedSurface, 0.0, std::numbers::pi, 0.0, 10.0, 1e-7).Face();
    TopoDS_Compound both;
    BRep_Builder builder;
    builder.MakeCompound(both);
    builder.Add(both, front);
    builder.Add(both, back);
    TopoShape halves {both};
    EXPECT_EQ(search(halves, BRepBuilderAPI_Copy(front).Shape()),
              std::vector<std::string> {"Face1"});
    EXPECT_EQ(search(halves, BRepBuilderAPI_Copy(back).Shape()),
              std::vector<std::string> {"Face2"});
}

// An arc a later kernel writes as a B-spline is the same edge, and a face
// bounded by it the same face: OCCT 8.0 hands back the circular edge of a
// PartDesign helix's end face so, and the sketch attached to that face was
// not found again (scanner.FCStd's Helix001)
TEST_F(TopoShapeSearchTest, anArcWrittenAsABSplineIsFound)
{
    Handle(Geom_Circle) circle = new Geom_Circle(gp_Ax2(gp_Pnt(), gp_Dir(0, 0, 1)), 5.0);
    Handle(Geom_TrimmedCurve) arc = new Geom_TrimmedCurve(circle, 0.0, std::numbers::pi / 2);
    Handle(Geom_BSplineCurve) spline = GeomConvert::CurveToBSplineCurve(arc);
    const gp_Pnt start = arc->StartPoint();
    const gp_Pnt end = arc->EndPoint();
    auto faceOf = [&](const TopoDS_Edge& edge) {
        BRepBuilderAPI_MakeWire wire(BRepBuilderAPI_MakeEdge(gp_Pnt(), start).Edge(),
                                     edge,
                                     BRepBuilderAPI_MakeEdge(end, gp_Pnt()).Edge());
        return BRepBuilderAPI_MakeFace(wire.Wire()).Face();
    };
    TopoDS_Edge asArc = BRepBuilderAPI_MakeEdge(arc).Edge();
    TopoShape written {faceOf(BRepBuilderAPI_MakeEdge(spline).Edge())};

    std::string splineName;
    for (int i = 1; i <= (int)written.countSubShapes(TopAbs_EDGE); ++i) {
        double first, last;
        auto curve =
            BRep_Tool::Curve(TopoDS::Edge(written.getSubShape(TopAbs_EDGE, i)), first, last);
        if (!curve.IsNull() && curve->IsKind(STANDARD_TYPE(Geom_BSplineCurve)))
            splineName = "Edge" + std::to_string(i);
    }
    ASSERT_FALSE(splineName.empty());
    EXPECT_EQ(search(written, asArc), std::vector<std::string> {splineName});
    EXPECT_EQ(search(written, faceOf(asArc)), std::vector<std::string> {"Face1"});

    // and an arc of another circle between the same two points is not it
    Handle(Geom_Circle) wider =
        new Geom_Circle(gp_Ax2(gp_Pnt(-5, -5, 0), gp_Dir(0, 0, 1)), std::sqrt(125.0));
    GeomAPI_ProjectPointOnCurve onStart(start, wider);
    GeomAPI_ProjectPointOnCurve onEnd(end, wider);
    TopoDS_Edge other = BRepBuilderAPI_MakeEdge(wider,
                                                onStart.LowerDistanceParameter(),
                                                onEnd.LowerDistanceParameter())
                            .Edge();
    EXPECT_EQ(search(written, other), std::vector<std::string> {});
    EXPECT_EQ(search(written, faceOf(other)), std::vector<std::string> {});
}
