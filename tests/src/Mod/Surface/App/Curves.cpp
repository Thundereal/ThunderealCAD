// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BezierCurve.hxx>
#include <Precision.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <Mod/Surface/App/Geom/Curves.h>

using namespace Surface::Geom;

namespace
{

using BCurve = Handle(Geom_BSplineCurve);

// A cubic Bezier as a B-spline, and its Bernstein form anywhere
BCurve cubic(const gp_Pnt (&p)[4])
{
    TColgp_Array1OfPnt poles(1, 4);
    for (int i = 0; i < 4; ++i) {
        poles.SetValue(i + 1, p[i]);
    }
    return GeomConvert::CurveToBSplineCurve(new Geom_BezierCurve(poles));
}

gp_Pnt bernstein(const gp_Pnt (&p)[4], double t)
{
    const double c[4] = {1, 3, 3, 1};
    gp_XYZ x;
    for (int i = 0; i < 4; ++i) {
        x += p[i].XYZ() * (c[i] * std::pow(t, i) * std::pow(1 - t, 3 - i));
    }
    return gp_Pnt(x);
}

TopoDS_Edge arcEdge(const gp_Pnt& a, const gp_Pnt& m, const gp_Pnt& b)
{
    return BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(a, m, b).Value());
}

}  // namespace

// Carried on naturally, a cubic is its own polynomial further on
TEST(Curves, extendNaturallyIsTheSamePolynomial)
{
    const gp_Pnt p[4] = {gp_Pnt(0, 0, 0), gp_Pnt(3, 4, 1), gp_Pnt(7, -2, 2), gp_Pnt(10, 1, 0)};
    for (bool atEnd : {true, false}) {
        BCurve c = cubic(p);
        ASSERT_EQ(extendCurve(c, atEnd, 4.0, Extension::Natural), "");
        double t0 = c->FirstParameter(), t1 = c->LastParameter();
        EXPECT_EQ(atEnd ? t0 : t1, atEnd ? 0.0 : 1.0);
        double worst = 0;
        for (int k = 0; k <= 20; ++k) {
            double t = t0 + (t1 - t0) * k / 20;
            worst = std::max(worst, c->Value(t).Distance(bernstein(p, t)));
        }
        EXPECT_LT(worst, 1e-9);
        gp_Pnt old = atEnd ? p[3] : p[0];
        EXPECT_NEAR(c->Value(atEnd ? t1 : t0).Distance(old), 4.0, 1e-9);
        EXPECT_EQ(c->Degree(), 3);
    }
}

// Straight on: the new part on the tangent line, C1, as long as asked
TEST(Curves, extendLinearlyIsStraightAndTangent)
{
    const gp_Pnt p[4] = {gp_Pnt(0, 0, 0), gp_Pnt(3, 4, 1), gp_Pnt(7, -2, 2), gp_Pnt(10, 1, 0)};
    BCurve original = cubic(p);
    BCurve c = BCurve::DownCast(original->Copy());
    ASSERT_EQ(extendCurve(c, true, 5.0, Extension::Linear), "");
    gp_Pnt end;
    gp_Vec d;
    original->D1(1.0, end, d);
    gp_Lin line(end, gp_Dir(d));
    double off = 0;
    for (int k = 0; k <= 10; ++k) {
        double t = 1.0 + (c->LastParameter() - 1.0) * k / 10;
        off = std::max(off, line.Distance(c->Value(t)));
    }
    EXPECT_LT(off, 1e-9);
    EXPECT_NEAR(c->Value(c->LastParameter()).Distance(end), 5.0, 1e-9);
    gp_Vec before = c->DN(1.0 - 1e-7, 1), after = c->DN(1.0 + 1e-7, 1);
    EXPECT_LT(before.Angle(after), 1e-5);
    // what was there stays
    for (int k = 0; k <= 10; ++k) {
        EXPECT_LT(c->Value(k / 10.0).Distance(original->Value(k / 10.0)), 1e-9);
    }
}

// Two arcs meeting tangent, given out of order and one the other way round,
// join into one curve on both; a third leaving at a corner starts another
TEST(Curves, joinSmoothRunsBreakingAtCorners)
{
    // a quarter circle and the next quarter, radius 5 about the origin
    TopoDS_Edge a = arcEdge(gp_Pnt(5, 0, 0), gp_Pnt(5 / std::sqrt(2.0), 5 / std::sqrt(2.0), 0),
                            gp_Pnt(0, 5, 0));
    TopoDS_Edge b = arcEdge(gp_Pnt(-5, 0, 0), gp_Pnt(-5 / std::sqrt(2.0), 5 / std::sqrt(2.0), 0),
                            gp_Pnt(0, 5, 0));  // the other way round
    // the arcs reach (-5, 0, 0) heading down -Y: this leaves at right angles
    TopoDS_Edge corner = BRepBuilderAPI_MakeEdge(gp_Pnt(-5, 0, 0), gp_Pnt(-11, 0, 0));
    std::string error;
    auto smooth = joinCurves({b, a}, 1e-4, 5 * M_PI / 180, 1e-6, error);
    ASSERT_EQ(smooth.size(), 1u) << error;
    for (const TopoDS_Edge& e : {a, b}) {
        BRepAdaptor_Curve c(e);
        for (int k = 0; k <= 8; ++k) {
            double t = c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * k / 8;
            GeomAPI_ProjectPointOnCurve pr(c.Value(t), smooth[0]);
            ASSERT_GT(pr.NbPoints(), 0);
            EXPECT_LT(pr.LowerDistance(), 1e-4);
        }
    }
    auto two = joinCurves({a, corner, b}, 1e-4, 5 * M_PI / 180, 1e-6, error);
    EXPECT_EQ(two.size(), 2u) << error;
}

// A closed smooth loop joins into one closed curve
TEST(Curves, joinAClosedLoop)
{
    const double r = 5, s = r / std::sqrt(2.0);
    std::vector<TopoDS_Edge> quarters = {
        arcEdge(gp_Pnt(r, 0, 0), gp_Pnt(s, s, 0), gp_Pnt(0, r, 0)),
        arcEdge(gp_Pnt(0, r, 0), gp_Pnt(-s, s, 0), gp_Pnt(-r, 0, 0)),
        arcEdge(gp_Pnt(-r, 0, 0), gp_Pnt(-s, -s, 0), gp_Pnt(0, -r, 0)),
        arcEdge(gp_Pnt(0, -r, 0), gp_Pnt(s, -s, 0), gp_Pnt(r, 0, 0)),
    };
    std::string error;
    auto loop = joinCurves(quarters, 1e-4, 5 * M_PI / 180, 1e-6, error);
    ASSERT_EQ(loop.size(), 1u) << error;
    EXPECT_TRUE(loop[0]->IsClosed());
    for (int k = 0; k < 16; ++k) {
        double t = loop[0]->FirstParameter()
            + (loop[0]->LastParameter() - loop[0]->FirstParameter()) * k / 16;
        EXPECT_NEAR(gp_Pnt(0, 0, 0).Distance(loop[0]->Value(t)), r, 1e-3);
    }
}

// Isocurves of a face with a hole: clipped to it, on its surface
TEST(Curves, isocurvesClippedToTheFace)
{
    BRepBuilderAPI_MakePolygon outer(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0), gp_Pnt(10, 10, 0),
                                     gp_Pnt(0, 10, 0), true);
    gp_Circ c(gp_Ax2(gp_Pnt(5, 5, 0), gp_Dir(0, 0, 1)), 2);
    TopoDS_Wire hole = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(c));
    BRepBuilderAPI_MakeFace mk(outer.Wire(), true);
    mk.Add(TopoDS::Wire(hole.Reversed()));
    TopoDS_Face face = mk.Face();
    auto isos = isocurves(face, 3, 0);
    // the middle one (x = 5) is cut in two by the hole; the others whole
    EXPECT_EQ(isos.size(), 4u);
    double total = 0;
    for (const auto& e : isos) {
        BRepAdaptor_Curve a(e);
        total += a.LastParameter() - a.FirstParameter();
        double f, l;
        EXPECT_FALSE(BRep_Tool::CurveOnSurface(e, face, f, l).IsNull());
    }
    EXPECT_NEAR(total, 10 + 10 + 6, 1e-6);
}
