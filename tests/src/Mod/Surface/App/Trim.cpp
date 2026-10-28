// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>

#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Precision.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>

#include <Mod/Surface/App/Geom/Trim.h>

using namespace Surface::Geom;

namespace
{

// z = 2 sin(x / 4) cos(y / 5) over x in [0, 20], y in [0, 12]
TopoDS_Face wave()
{
    TColgp_Array2OfPnt pts(1, 9, 1, 7);
    for (int i = 0; i < 9; ++i) {
        for (int j = 0; j < 7; ++j) {
            double x = 20.0 * i / 8, y = 12.0 * j / 6;
            pts.SetValue(i + 1, j + 1, gp_Pnt(x, y, 2 * std::sin(x / 4) * std::cos(y / 5)));
        }
    }
    GeomAPI_PointsToBSplineSurface fit(pts, 3, 3, GeomAbs_C2, 1e-9);
    return BRepBuilderAPI_MakeFace(fit.Surface(), Precision::Confusion());
}

TopoDS_Shape line(const gp_Pnt& a, const gp_Pnt& b)
{
    return BRepBuilderAPI_MakeEdge(a, b).Shape();
}

double distance(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    BRepExtrema_DistShapeShape d(a, b);
    return d.Value();
}

// The faces `face` is split into by `edges`
std::vector<TopoDS_Face> split(const TopoDS_Face& face, const std::vector<TopoDS_Shape>& edges)
{
    BRepAlgoAPI_Splitter splitter;
    TopTools_ListOfShape args, tools;
    args.Append(face);
    for (const auto& e : edges) {
        tools.Append(e);
    }
    splitter.SetArguments(args);
    splitter.SetTools(tools);
    splitter.Build();
    std::vector<TopoDS_Face> out;
    for (TopExp_Explorer x(splitter.Shape(), TopAbs_FACE); x.More(); x.Next()) {
        out.push_back(TopoDS::Face(x.Current()));
    }
    return out;
}

double area(const TopoDS_Shape& s)
{
    GProp_GProps p;
    BRepGProp::SurfaceProperties(s, p);
    return p.Mass();
}

int edgeCount(const std::vector<TopoDS_Shape>& shapes)
{
    int n = 0;
    for (const auto& s : shapes) {
        for (TopExp_Explorer x(s, TopAbs_EDGE); x.More(); x.Next()) {
            ++n;
        }
    }
    return n;
}

}  // namespace

// A curve above the face, laid onto it along its normal, splits it in two
TEST(Trim, projectedAlongTheNormalSplits)
{
    TopoDS_Face face = wave();
    auto edges = projectOnto(face, {line(gp_Pnt(10, -2, 5), gp_Pnt(10, 14, 5))}, nullptr, 1e-6);
    ASSERT_GE(edgeCount(edges), 1);
    for (const auto& e : edges) {
        EXPECT_LT(distance(e, face), 1e-6);
    }
    auto pieces = split(face, edges);
    ASSERT_EQ(pieces.size(), 2u);
    // within what the area of a trimmed face is integrated to
    EXPECT_NEAR(area(pieces[0]) + area(pieces[1]), area(face), 1e-5 * area(face));
}

// The same along a direction given
TEST(Trim, projectedAlongADirectionSplits)
{
    TopoDS_Face face = wave();
    gp_Dir down(0, 0, -1);
    auto edges = projectOnto(face, {line(gp_Pnt(-2, 6, 5), gp_Pnt(22, 6, 5))}, &down, 1e-6);
    ASSERT_GE(edgeCount(edges), 1);
    auto pieces = split(face, edges);
    ASSERT_EQ(pieces.size(), 2u);
    // along -Z the cut is at y = 6 exactly
    for (const auto& e : edges) {
        for (TopExp_Explorer x(e, TopAbs_VERTEX); x.More(); x.Next()) {
            EXPECT_NEAR(BRep_Tool::Pnt(TopoDS::Vertex(x.Current())).Y(), 6, 1e-6);
        }
    }
}

// An edge on the face already is kept as it is; one beside it is left out
TEST(Trim, onTheFaceKeptBesideItLeftOut)
{
    TopoDS_Face face = wave();
    Handle(Geom_Surface) s = BRep_Tool::Surface(face);
    double u1, u2, v1, v2;
    s->Bounds(u1, u2, v1, v2);
    TopoDS_Edge iso = BRepBuilderAPI_MakeEdge(s->UIso((u1 + u2) / 2), v1, v2);
    auto kept = projectOnto(face, {iso}, nullptr, 1e-6);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_TRUE(kept.front().IsSame(iso));
    gp_Dir down(0, 0, -1);
    auto none = projectOnto(face, {line(gp_Pnt(30, 0, 5), gp_Pnt(30, 12, 5))}, &down, 1e-6);
    EXPECT_EQ(edgeCount(none), 0);
}

// The piece nearest a point; a point as near to two is a tie
TEST(Trim, nearestPieceAndTies)
{
    TopoDS_Face face = wave();
    gp_Dir down(0, 0, -1);
    auto pieces = split(face, projectOnto(face, {line(gp_Pnt(10, -2, 5), gp_Pnt(10, 14, 5))}, &down, 1e-6));
    ASSERT_EQ(pieces.size(), 2u);
    int left = distance(pieces[0], BRepBuilderAPI_MakeVertex(gp_Pnt(2, 6, 0))) < 1 ? 0 : 1;
    EXPECT_EQ(nearestPiece(pieces, gp_Pnt(2, 6, 3), 1e-6), left);
    EXPECT_EQ(nearestPiece(pieces, gp_Pnt(18, 6, -3), 1e-6), 1 - left);
    int tie = -1;
    // on the cut: both pieces are 0 from it
    int first = nearestPiece(pieces, gp_Pnt(10, 6, 2 * std::sin(2.5) * std::cos(1.2)), 1e-6, &tie);
    EXPECT_GE(first, 0);
    EXPECT_EQ(tie, 1 - first);
    EXPECT_EQ(nearestPiece({}, gp_Pnt(0, 0, 0), 1e-6), -1);
}
