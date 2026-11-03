// SPDX-License-Identifier: LGPL-2.1-or-later

// A hull's surfaces (ContactShape::surfaces): each point's faces of the
// solid, their normals and principal curvatures, against the solids' own.

#include <gtest/gtest.h>

#include <cmath>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <Mod/Assembly/App/ContactShapes.h>

using Assembly::contactShapeOf;
using Kind = Assembly::Solver::ContactShape::Kind;

namespace
{

constexpr double radius = 5.0;

// The face entry of a point nearest a normal, as the backend takes it
const Assembly::Solver::ContactShape::Surface*
facing(const std::vector<Assembly::Solver::ContactShape::Surface>& faces,
       const Base::Vector3d& normal)
{
    const Assembly::Solver::ContactShape::Surface* best = nullptr;
    for (const auto& face : faces) {
        if (!best || face.normal * normal > best->normal * normal) {
            best = &face;
        }
    }
    return best;
}

}  // namespace

// A cylinder of radius 5 and 20 long along z, its rims chamfered 0.5: a
// hull, each point on its side carrying the side's curvature, 1/5 round
// and none along, and the outward radial normal
TEST(ContactShapes, chamferedRollerCurvature)
{
    BRepPrimAPI_MakeCylinder cylinder(radius, 20.0);
    BRepFilletAPI_MakeChamfer chamfer(cylinder.Shape());
    for (TopExp_Explorer it(cylinder.Shape(), TopAbs_EDGE); it.More(); it.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(it.Current());
        if (BRepAdaptor_Curve(edge).GetType() == GeomAbs_Circle) {
            chamfer.Add(0.5, edge);
        }
    }
    const auto shape = contactShapeOf(chamfer.Shape());
    ASSERT_TRUE(shape);
    ASSERT_EQ(shape->kind, Kind::ConvexHull);
    ASSERT_EQ(shape->surfaces.size(), shape->points.size());
    int side = 0;
    for (std::size_t i = 0; i < shape->points.size(); ++i) {
        const Base::Vector3d& p = shape->points[i];
        ASSERT_FALSE(shape->surfaces[i].empty()) << i;
        const double round = std::hypot(p.x, p.y);
        if (p.z < 0.5 - 1e-9 || p.z > 19.5 + 1e-9 || std::abs(round - radius) > 1e-6) {
            continue;
        }
        ++side;
        const Base::Vector3d radial(p.x / round, p.y / round, 0.0);
        const auto* face = facing(shape->surfaces[i], radial);
        ASSERT_TRUE(face);
        EXPECT_NEAR(face->normal * radial, 1.0, 1e-9) << i;
        EXPECT_NEAR(face->k1, 1.0 / radius, 1e-9) << i;
        EXPECT_NEAR(face->k2, 0.0, 1e-9) << i;
        EXPECT_NEAR(std::abs(face->way2.z), 1.0, 1e-9) << i;
    }
    EXPECT_GT(side, 40);
}

// A block with a bore of radius 10 along y: not convex, a mesh; its bore
// a hollow round (ContactShape::rounds), its axis along y and its reach
// the block's 20, and the bore's points curved -1/10 round it
TEST(ContactShapes, boreIsAHollowRound)
{
    const TopoDS_Shape block =
        BRepAlgoAPI_Cut(
            BRepPrimAPI_MakeBox(gp_Pnt(-20, -10, 0), 40, 20, 40).Shape(),
            BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, -15, 20), gp_Dir(0, 1, 0)), 10.0, 30.0)
                .Shape())
            .Shape();
    const auto shape = contactShapeOf(block);
    ASSERT_TRUE(shape);
    ASSERT_EQ(shape->kind, Kind::Mesh);
    int bores = 0;
    int bore = -1;
    for (std::size_t k = 0; k < shape->rounds.size(); ++k) {
        const auto& round = shape->rounds[k];
        if (!round.concave) {
            continue;
        }
        ++bores;
        bore = static_cast<int>(k);
        EXPECT_FALSE(round.sphere);
        EXPECT_NEAR(round.radius, 10.0, 1e-9);
        EXPECT_NEAR(std::abs(round.axis.y), 1.0, 1e-12);
        EXPECT_NEAR(round.to - round.from, 20.0, 1e-9);
        // Its axis through (0, y, 20)
        EXPECT_NEAR(round.centre.x, 0.0, 1e-9);
        EXPECT_NEAR(round.centre.z, 20.0, 1e-9);
    }
    ASSERT_EQ(bores, 1);
    int on = 0;
    for (const auto& faces : shape->surfaces) {
        for (const auto& face : faces) {
            if (face.round == bore) {
                ++on;
                EXPECT_NEAR(face.k2, -0.1, 1e-9);
                EXPECT_NEAR(face.k1, 0.0, 1e-9);
            }
        }
    }
    EXPECT_GT(on, 40);
}

// A ball of radius 5 with flats cut 0.5 deep at both y sides: a hull, its
// sphere's points 1/5 round both ways, its flats' none
TEST(ContactShapes, flattedBallCurvature)
{
    TopoDS_Shape ball = BRepPrimAPI_MakeSphere(radius).Shape();
    for (const double side : {1.0, -1.0}) {
        const double y = side > 0 ? radius - 0.5 : -radius + 0.5 - 20.0;
        ball = BRepAlgoAPI_Cut(ball, BRepPrimAPI_MakeBox(gp_Pnt(-10, y, -10), 20, 20, 20).Shape())
                   .Shape();
    }
    const auto shape = contactShapeOf(ball);
    ASSERT_TRUE(shape);
    ASSERT_EQ(shape->kind, Kind::ConvexHull);
    ASSERT_EQ(shape->surfaces.size(), shape->points.size());
    int round = 0;
    int flat = 0;
    for (std::size_t i = 0; i < shape->points.size(); ++i) {
        const Base::Vector3d& p = shape->points[i];
        ASSERT_FALSE(shape->surfaces[i].empty()) << i;
        // The flats' rims, sqrt(5^2 - 4.5^2) = 2.18 out, are on both faces
        if (std::abs(std::abs(p.y) - (radius - 0.5)) < 1e-9 && std::hypot(p.x, p.z) < 2.2) {
            ++flat;
            const auto* face = facing(shape->surfaces[i], Base::Vector3d(0, p.y > 0 ? 1 : -1, 0));
            ASSERT_TRUE(face);
            EXPECT_NEAR(face->k1, 0.0, 1e-9) << i;
            EXPECT_NEAR(face->k2, 0.0, 1e-9) << i;
            continue;
        }
        if (std::abs(p.Length() - radius) > 1e-6 || std::abs(p.y) > radius - 0.6) {
            continue;
        }
        ++round;
        const Base::Vector3d out = p / p.Length();
        const auto* face = facing(shape->surfaces[i], out);
        ASSERT_TRUE(face);
        EXPECT_NEAR(face->normal * out, 1.0, 1e-9) << i;
        EXPECT_NEAR(face->k1, 1.0 / radius, 1e-9) << i;
        EXPECT_NEAR(face->k2, 1.0 / radius, 1e-9) << i;
    }
    EXPECT_GT(round, 100);
    EXPECT_GT(flat, 0);
}
