// SPDX-License-Identifier: LGPL-2.1-or-later

#include "gtest/gtest.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom_Surface.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>

#include "Mod/Part/App/OCCTFork.h"

// What a thick solid says of the faces of its input that stay in it
// (docs/TopoNamingEnhance.md sec 9.11, the OCCT fork's Thickness.md sec 69).
// A wall that shares an edge with the removed face is made again where that
// edge is cut, on its own surface, and the kernel said nothing of it: it was
// neither the input's face nor the image of one, and TopoShape::makESHAPE
// had to name it from its neighbours. The fork's kernel reports it modified.
namespace
{

// A plate 60 x 30 x 10 with two square holes 2.5 from its front: nearer the
// outline than twice the thickness taken below
TopoDS_Shape plateWithHolesNearItsOutline()
{
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 60.0, 30.0, 10.0).Shape();
    for (double x : {10.0, 40.0}) {
        TopoDS_Shape hole = BRepPrimAPI_MakeBox(gp_Pnt(x, 2.5, -1), 10.0, 10.0, 12.0).Shape();
        plate = BRepAlgoAPI_Cut(plate, hole).Shape();
    }
    return plate;
}

TopoDS_Face topOf(const TopoDS_Shape& shape)
{
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
        Bnd_Box box;
        BRepBndLib::Add(faces(i), box);
        if (box.CornerMin().Z() > 9.9) {
            return TopoDS::Face(faces(i));
        }
    }
    return {};
}

}  // namespace

TEST(ThickSolidHistory, aWallMadeAgainIsModified)
{
    if (Part::setOCCTOffsetFixLevel(0) < 0) {
        GTEST_SKIP() << "needs the OCCT fork (TKOffset)";
    }
    // Arrange
    const TopoDS_Shape plate = plateWithHolesNearItsOutline();
    const TopoDS_Face top = topOf(plate);
    ASSERT_FALSE(top.IsNull());
    TopTools_ListOfShape removed;
    removed.Append(top);

    // Act: the top removed, 4 inward, the Arc join, Intersection on
    BRepOffsetAPI_MakeThickSolid maker;
    maker.MakeThickSolidByJoin(plate, removed, -4.0, 1e-7, BRepOffset_Skin, true, false, GeomAbs_Arc);
    ASSERT_TRUE(maker.IsDone());
    const TopoDS_Shape result = maker.Shape();
    ASSERT_TRUE(BRepCheck_Analyzer(result).IsValid());

    // Assert: every face of the result is the input's own, or one the input
    // answers for
    TopTools_IndexedMapOfShape inputFaces, resultFaces, told;
    TopExp::MapShapes(plate, TopAbs_FACE, inputFaces);
    TopExp::MapShapes(result, TopAbs_FACE, resultFaces);
    int remade = 0;
    for (int i = 1; i <= inputFaces.Extent(); ++i) {
        const TopoDS_Shape& face = inputFaces(i);
        for (TopTools_ListIteratorOfListOfShape it(maker.Generated(face)); it.More(); it.Next()) {
            told.Add(it.Value());
        }
        const bool kept = resultFaces.Contains(face);
        int modified = 0;
        for (TopTools_ListIteratorOfListOfShape it(maker.Modified(face)); it.More(); it.Next()) {
            told.Add(it.Value());
            if (!resultFaces.Contains(it.Value())) {
                continue;
            }
            ++modified;
            if (!face.IsSame(top)) {
                // a wall made again lies on the wall's own surface
                TopLoc_Location was, is;
                EXPECT_EQ(
                    BRep_Tool::Surface(TopoDS::Face(face), was).get(),
                    BRep_Tool::Surface(TopoDS::Face(it.Value()), is).get()
                ) << "Face" << i;
                EXPECT_FALSE(kept) << "Face" << i << " is in the result and modified too";
                ++remade;
            }
        }
        EXPECT_TRUE(kept || modified > 0) << "Face" << i << " is gone and nothing is said of it";
        EXPECT_EQ(maker.IsDeleted(face), false) << "Face" << i;
    }
    EXPECT_GT(remade, 0) << "the case no longer makes a wall again: it tests nothing";
    // the rounds of the Arc join are made from the input's edges and vertices
    for (TopAbs_ShapeEnum type : {TopAbs_EDGE, TopAbs_VERTEX}) {
        TopTools_IndexedMapOfShape inputs;
        TopExp::MapShapes(plate, type, inputs);
        for (int i = 1; i <= inputs.Extent(); ++i) {
            for (TopTools_ListIteratorOfListOfShape it(maker.Generated(inputs(i))); it.More();
                 it.Next()) {
                told.Add(it.Value());
            }
        }
    }
    for (int i = 1; i <= resultFaces.Extent(); ++i) {
        EXPECT_TRUE(inputFaces.Contains(resultFaces(i)) || told.Contains(resultFaces(i)))
            << "no face of the input answers for Face" << i << " of the result";
    }
}
