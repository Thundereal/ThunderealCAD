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
#include <Mod/Assembly/App/Solver/LocalSurface.h>

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

// A flexible part's surface as its solid has it (D4b way 1 step 3): one
// tetrahedron of a rod R 5 along z, a face of it on the rod's round -- its
// corners and its edges' nodes there, 0.8 rad round and 4 long -- its
// fourth corner on the axis. That face alone is on the solid's surface; the
// six-node face stands 3.9 um in from the round, and through its nine more
// points, 0.11 um
TEST(ContactShapes, aFlexiblePartsTrueSurface)
{
    namespace LS = Assembly::Solver::LocalSurface;
    const TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(5.0, 40.0).Shape();
    auto round = [](double angle, double z) {
        return Base::Vector3d(5.0 * std::cos(angle), 5.0 * std::sin(angle), z);
    };
    const Base::Vector3d inner(0.0, 0.0, 1.0);
    std::vector<Base::Vector3d> nodes {round(0.1, 1.0), round(0.9, 1.0), round(0.1, 5.0), inner};
    // The edges (0 1), (1 2), (0 2) on the round; (0 3), (1 3), (2 3) straight
    nodes.push_back(round(0.5, 1.0));
    nodes.push_back(round(0.5, 3.0));
    nodes.push_back(round(0.1, 3.0));
    for (int k = 0; k < 3; ++k) {
        nodes.push_back((nodes[static_cast<std::size_t>(k)] + inner) / 2.0);
    }
    const std::vector<std::array<Assembly::Solver::Id, 10>> tets {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}};
    const auto faces = Assembly::trueFacesOf(rod, nodes, tets);
    ASSERT_EQ(faces.size(), 1u);
    const auto& face = faces.front();
    EXPECT_EQ(face.corners, (std::array<Assembly::Solver::Id, 3> {0, 1, 2}));
    EXPECT_GE(face.face, 0);
    // The face in its corners' order, its edges' nodes (0 1), (1 2), (2 0)
    LS::Face x;
    const std::array<std::size_t, 6> order {0, 1, 2, 4, 5, 6};
    for (std::size_t k = 0; k < 6; ++k) {
        const auto& v = nodes[order[k]];
        x[k] = {v.x, v.y, v.z};
    }
    LS::Offsets offsets;
    for (std::size_t p = 0; p < offsets.size(); ++p) {
        offsets[p] = {face.offsets[p].x, face.offsets[p].y, face.offsets[p].z};
    }
    double own = 0.0;
    double through = 0.0;
    for (int i = 0; i <= 20; ++i) {
        for (int j = 0; i + j <= 20; ++j) {
            const std::array<double, 3> L {(20 - i - j) / 20.0, i / 20.0, j / 20.0};
            const LS::Vec a = LS::pointAt(x, nullptr, L);
            const LS::Vec b = LS::pointAt(x, &offsets, L);
            own = std::max(own, std::abs(std::hypot(a[0], a[1]) - 5.0));
            through = std::max(through, std::abs(std::hypot(b[0], b[1]) - 5.0));
        }
    }
    EXPECT_GT(own, 3.5e-3);
    EXPECT_LT(through, 1.5e-4);
    // A box's faces are planes: none
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    std::vector<Base::Vector3d> flat {Base::Vector3d(0, 0, 0),
                                      Base::Vector3d(4, 0, 0),
                                      Base::Vector3d(0, 4, 0),
                                      Base::Vector3d(1, 1, 1)};
    flat.push_back((flat[0] + flat[1]) / 2.0);
    flat.push_back((flat[1] + flat[2]) / 2.0);
    flat.push_back((flat[0] + flat[2]) / 2.0);
    for (int k = 0; k < 3; ++k) {
        flat.push_back((flat[static_cast<std::size_t>(k)] + flat[3]) / 2.0);
    }
    EXPECT_TRUE(Assembly::trueFacesOf(box, flat, tets).empty());
}
