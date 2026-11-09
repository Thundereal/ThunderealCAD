// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 FreeCAD contributors
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#include <gtest/gtest.h>
#include "src/App/InitApplication.h"
#include <App/Application.h>
#include <App/Document.h>
#include <Base/Interpreter.h>
#include "Mod/Part/App/FaceMakerUnified.h"
#include "Mod/Part/App/TopoShape.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <GC_MakeCircle.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <gp_Pln.hxx>
#include <Precision.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopoDS.hxx>

// NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)

using namespace Part;

class FaceMakerUnifiedTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Base::Interpreter().runString("import Part");
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("test");
        App::GetApplication().newDocument(_docName.c_str(), "testUser");
        _doc = App::GetApplication().getDocument(_docName.c_str());
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_docName.c_str());
    }

    static TopoDS_Wire makeRectWire(double x0, double y0, double x1, double y1)
    {
        gp_Pnt p0(x0, y0, 0), p1(x1, y0, 0), p2(x1, y1, 0), p3(x0, y1, 0);
        BRepBuilderAPI_MakeWire mw;
        mw.Add(BRepBuilderAPI_MakeEdge(p0, p1).Edge());
        mw.Add(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
        mw.Add(BRepBuilderAPI_MakeEdge(p2, p3).Edge());
        mw.Add(BRepBuilderAPI_MakeEdge(p3, p0).Edge());
        return mw.Wire();
    }

    static TopoDS_Wire makeLineWire(double x0, double y0, double x1, double y1)
    {
        return BRepBuilderAPI_MakeWire(
                   BRepBuilderAPI_MakeEdge(gp_Pnt(x0, y0, 0), gp_Pnt(x1, y1, 0)).Edge()
        )
            .Wire();
    }

    static TopoDS_Wire makeFigure8Wire()
    {
        TColgp_Array1OfPnt points(1, 9);
        points.SetValue(1, gp_Pnt(0, 0, 0));
        points.SetValue(2, gp_Pnt(5, 5, 0));
        points.SetValue(3, gp_Pnt(10, 0, 0));
        points.SetValue(4, gp_Pnt(5, -5, 0));
        points.SetValue(5, gp_Pnt(0, 0, 0));
        points.SetValue(6, gp_Pnt(-5, -5, 0));
        points.SetValue(7, gp_Pnt(-10, 0, 0));
        points.SetValue(8, gp_Pnt(-5, 5, 0));
        points.SetValue(9, gp_Pnt(0, 0, 0));
        Handle(TColgp_HArray1OfPnt) hpoints = new TColgp_HArray1OfPnt(points);
        GeomAPI_Interpolate interp(hpoints, Standard_False, Precision::Confusion());
        interp.Perform();
        return BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(interp.Curve()).Edge()).Wire();
    }

    TopoShape makeUnifiedFace(const std::vector<TopoDS_Wire>& wires)
    {
        auto hasher = _doc->getStringHasher();
        long tag = 1;
        std::vector<TopoShape> sources;
        for (const auto& w : wires) {
            TopoShape ts(tag++, hasher);
            ts.setShape(w);
            ts.mapSubElement(ts);
            sources.push_back(std::move(ts));
        }
        TopoShape result(tag, hasher);
        result.makEFace(sources, nullptr, "Part::FaceMakerUnified", nullptr);
        return result;
    }

    void expectAllEdgesNamed(const TopoShape& shape)
    {
        int count = shape.countSubShapes(TopAbs_EDGE);
        ASSERT_GT(count, 0);
        for (int i = 1; i <= count; ++i) {
            auto indexed = Data::IndexedName::fromConst("Edge", i);
            auto mapped = shape.getMappedName(indexed);
            EXPECT_TRUE(mapped) << "Edge" << i << " has no mapped name";
            if (mapped) {
                EXPECT_NE(mapped.toString(), indexed.toString())
                    << "Edge" << i << " has identity mapping (unnamed)";
            }
        }
    }

    void expectAllFacesNamed(const TopoShape& shape)
    {
        int count = shape.countSubShapes(TopAbs_FACE);
        ASSERT_GT(count, 0);
        for (int i = 1; i <= count; ++i) {
            auto indexed = Data::IndexedName::fromConst("Face", i);
            auto mapped = shape.getMappedName(indexed);
            EXPECT_TRUE(mapped) << "Face" << i << " has no mapped name";
            if (mapped) {
                EXPECT_NE(mapped.toString(), indexed.toString())
                    << "Face" << i << " has identity mapping (unnamed)";
            }
        }
    }

    void expectNamingStable(const std::vector<TopoDS_Wire>& wires)
    {
        auto r1 = makeUnifiedFace(wires);
        auto r2 = makeUnifiedFace(wires);
        int edgeCount = r1.countSubShapes(TopAbs_EDGE);
        ASSERT_EQ(edgeCount, r2.countSubShapes(TopAbs_EDGE));
        for (int i = 1; i <= edgeCount; ++i) {
            auto idx = Data::IndexedName::fromConst("Edge", i);
            EXPECT_EQ(r1.getMappedName(idx), r2.getMappedName(idx))
                << "Edge" << i << " name differs across builds";
        }
        int faceCount = r1.countSubShapes(TopAbs_FACE);
        ASSERT_EQ(faceCount, r2.countSubShapes(TopAbs_FACE));
        for (int i = 1; i <= faceCount; ++i) {
            auto idx = Data::IndexedName::fromConst("Face", i);
            EXPECT_EQ(r1.getMappedName(idx), r2.getMappedName(idx))
                << "Face" << i << " name differs across builds";
        }
    }

    std::string _docName;
    App::Document* _doc = nullptr;
};

// No splitting -- baseline: names should exist even without edge modifications.
TEST_F(FaceMakerUnifiedTest, singleRectangleNaming)
{
    auto result = makeUnifiedFace({makeRectWire(0, 0, 10, 10)});
    ASSERT_EQ(result.countSubShapes(TopAbs_FACE), 1);
    expectAllEdgesNamed(result);
    expectAllFacesNamed(result);
}

// Inter-edge splits via mySplitter (diagonal crosses rectangle edges).
TEST_F(FaceMakerUnifiedTest, subdivisionNaming)
{
    auto result = makeUnifiedFace({makeRectWire(0, 0, 10, 10), makeLineWire(0, 0, 10, 10)});
    ASSERT_GE(result.countSubShapes(TopAbs_FACE), 2);
    expectAllEdgesNamed(result);
    expectAllFacesNamed(result);
}

// Partially overlapping rectangles -- group-aware even-odd classification.
TEST_F(FaceMakerUnifiedTest, overlappingRectsNaming)
{
    auto result = makeUnifiedFace({makeRectWire(0, 0, 20, 20), makeRectWire(10, 10, 30, 30)});
    ASSERT_GE(result.countSubShapes(TopAbs_FACE), 1);
    expectAllEdgesNamed(result);
    expectAllFacesNamed(result);
}

// Self-intersecting B-spline -- myPreSplitHistory tracks fragments.
TEST_F(FaceMakerUnifiedTest, selfIntersectingBSplineNaming)
{
    auto result = makeUnifiedFace({makeFigure8Wire()});
    ASSERT_GE(result.countSubShapes(TopAbs_FACE), 2);
    expectAllEdgesNamed(result);
    expectAllFacesNamed(result);
}

// Stability tests -- names must be identical across rebuilds.
TEST_F(FaceMakerUnifiedTest, singleRectangleStable)
{
    expectNamingStable({makeRectWire(0, 0, 10, 10)});
}

TEST_F(FaceMakerUnifiedTest, subdivisionStable)
{
    expectNamingStable({makeRectWire(0, 0, 10, 10), makeLineWire(0, 0, 10, 10)});
}

TEST_F(FaceMakerUnifiedTest, overlappingRectsStable)
{
    expectNamingStable({makeRectWire(0, 0, 20, 20), makeRectWire(10, 10, 30, 30)});
}

TEST_F(FaceMakerUnifiedTest, selfIntersectingBSplineStable)
{
    expectNamingStable({makeFigure8Wire()});
}

// A plain profile's numbering is the caller's: a transfinite mesh takes its
// corners from it, and a script its Edge1.
TEST_F(FaceMakerUnifiedTest, aSimpleWireKeepsItsEdgeOrder)
{
    auto face = makeUnifiedFace({makeRectWire(0, 0, 10, 4)});
    ASSERT_EQ(face.countSubShapes(TopAbs_FACE), 1);
    ASSERT_EQ(face.countSubShapes(TopAbs_EDGE), 4);
    const gp_Pnt middles[] = {{5, 0, 0}, {10, 2, 0}, {5, 4, 0}, {0, 2, 0}};
    for (int i = 1; i <= 4; ++i) {
        BRepAdaptor_Curve curve(TopoDS::Edge(face.getSubShape(TopAbs_EDGE, i)));
        const gp_Pnt middle = curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2);
        EXPECT_LT(middle.Distance(middles[i - 1]), Precision::Confusion()) << "Edge" << i;
    }
}

// Closed, nothing split, and still two faces: it passes through one point twice.
TEST_F(FaceMakerUnifiedTest, aWireTouchingItselfIsStillTwoFaces)
{
    const gp_Pnt points[] = {{0, 0, 0}, {10, 0, 0}, {5, 5, 0}, {10, 10, 0}, {0, 10, 0}, {5, 5, 0}};
    BRepBuilderAPI_MakeWire wire;
    for (int i = 0; i < 6; ++i) {
        wire.Add(BRepBuilderAPI_MakeEdge(points[i], points[(i + 1) % 6]).Edge());
    }
    ASSERT_TRUE(wire.IsDone());
    EXPECT_EQ(makeUnifiedFace({wire.Wire()}).countSubShapes(TopAbs_FACE), 2);
}

// NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers)
