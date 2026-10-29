// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GC_MakeSegment.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Precision.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <Mod/Surface/App/Geom/Continuity.h>

using namespace Surface::Geom;

namespace
{

constexpr double Deg = M_PI / 180;

TopoDS_Edge segment(const gp_Pnt& a, const gp_Pnt& b)
{
    return BRepBuilderAPI_MakeEdge(a, b);
}

}  // namespace

// Two faces of a box meet at right angles
TEST(Continuity, aCrease)
{
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    TopTools_IndexedDataMapOfShapeListOfShape map;
    TopExp::MapShapesAndAncestors(box, TopAbs_EDGE, TopAbs_FACE, map);
    const TopoDS_Edge& e = TopoDS::Edge(map.FindKey(1));
    const auto& faces = map.FindFromIndex(1);
    ASSERT_EQ(faces.Extent(), 2);
    auto r = edgeContinuity(e, TopoDS::Face(faces.First()), TopoDS::Face(faces.Last()));
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_LT(r.maxGap, 1e-9);
    EXPECT_NEAR(r.maxAngle, 90 * Deg, 1e-9);
    EXPECT_EQ(r.order(), 0);
}

// A flat face and a quarter cylinder of radius 5 tangent to it: G1, the
// curvature jumping by 1/5
TEST(Continuity, tangentWithACurvatureJump)
{
    TopoDS_Face flat = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), -10, 0, 0, 10);
    // a cylinder along -Y through (0, y, 5), radius 5: it touches z = 0 at
    // x = 0 (u = 0) and rises away from the flat face, to (5, y, 5)
    Handle(Geom_CylindricalSurface) cyl =
        new Geom_CylindricalSurface(gp_Ax3(gp_Pnt(0, 0, 5), gp_Dir(0, -1, 0), gp_Dir(0, 0, -1)), 5);
    TopoDS_Face round = BRepBuilderAPI_MakeFace(cyl, 0, M_PI / 2, -10, 0, Precision::Confusion());
    TopoDS_Edge e = segment(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0));
    auto r = edgeContinuity(e, flat, round);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_LT(r.maxGap, 1e-9);
    EXPECT_LT(r.maxAngle, 1e-6);
    EXPECT_NEAR(r.maxCurvatureGap, 0.2, 1e-6);
    EXPECT_EQ(r.order(), 1);
}

// One smooth surface cut in two: G2 across the cut
TEST(Continuity, oneSurfaceCutInTwo)
{
    TColgp_Array2OfPnt pts(1, 7, 1, 6);
    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 6; ++j) {
            double x = 20.0 * i / 6, y = 10.0 * j / 5;
            pts.SetValue(i + 1, j + 1, gp_Pnt(x, y, 2 * std::sin(x / 4) * std::cos(y / 5)));
        }
    }
    Handle(Geom_BSplineSurface) s = GeomAPI_PointsToBSplineSurface(pts).Surface();
    double u1, u2, v1, v2;
    s->Bounds(u1, u2, v1, v2);
    double um = (u1 + u2) / 2;
    TopoDS_Face a = BRepBuilderAPI_MakeFace(s, u1, um, v1, v2, Precision::Confusion());
    TopoDS_Face b = BRepBuilderAPI_MakeFace(s, um, u2, v1, v2, Precision::Confusion());
    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(s->UIso(um), v1, v2);
    auto r = edgeContinuity(e, a, b);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_LT(r.maxGap, 1e-7);
    EXPECT_LT(r.maxAngle, 1e-6);
    EXPECT_LT(r.maxCurvatureGap, 1e-5);
    EXPECT_EQ(r.order(), 2);
}

// Two flat faces 0.01 apart do not even meet
TEST(Continuity, aGap)
{
    TopoDS_Face a = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), -10, 0, 0, 10);
    TopoDS_Face b = BRepBuilderAPI_MakeFace(
        gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0.01), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))), 0, 10, 0, 10);
    auto r = edgeContinuity(segment(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0)), a, b);
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_NEAR(r.maxGap, 0.01, 1e-9);
    EXPECT_EQ(r.order(), -1);
}
