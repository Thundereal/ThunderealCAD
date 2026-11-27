/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
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

#include <BOPAlgo_ArgumentAnalyzer.hxx>
#include <BOPAlgo_CheckResult.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_ListOfStatus.hxx>
#include <BRepCheck_Result.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS_Iterator.hxx>

#include "GeometryCheck.h"

using namespace Part;

namespace
{

/// The panel's walk (TaskCheckGeometryResults::recursiveCheck and
/// checkSub), its entries made findings in the order it made them
struct Walk
{
    const BRepCheck_Analyzer& analyzer;
    const TopoDS_Shape& root;
    std::vector<GeometryCheck::Finding>& found;
    TopTools_MapOfShape checked;

    int indexOf(const TopoDS_Shape& shape) const
    {
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(root, shape.ShapeType(), map);
        return map.FindIndex(shape);
    }

    int add(int parent, const TopoDS_Shape& shape, int status)
    {
        GeometryCheck::Finding f;
        f.parent = parent;
        f.type = static_cast<int>(shape.ShapeType());
        f.index = indexOf(shape);
        f.source = GeometryCheck::Source::Brep;
        f.status = status;
        found.push_back(f);
        return static_cast<int>(found.size()) - 1;
    }

    void sub(const TopoDS_Shape& shape, TopAbs_ShapeEnum type, int parent)
    {
        for (TopExp_Explorer exp(shape, type); exp.More(); exp.Next()) {
            const Handle(BRepCheck_Result)& res = analyzer.Result(exp.Current());
            const TopoDS_Shape& current = exp.Current();
            for (res->InitContextIterator(); res->MoreShapeInContext();
                 res->NextShapeInContext()) {
                if (!res->ContextualShape().IsSame(shape)) {
                    continue;
                }
                for (BRepCheck_ListIteratorOfListOfStatus it(res->StatusOnShape()); it.More();
                     it.Next()) {
                    if (it.Value() == BRepCheck_NoError) {
                        break;
                    }
                    checked.Add(current);
                    add(parent, current, static_cast<int>(it.Value()));
                }
            }
        }
    }

    void recurse(const TopoDS_Shape& shape, int parent)
    {
        int branch = parent;
        if (!analyzer.Result(shape).IsNull() && !checked.Contains(shape)) {
            BRepCheck_ListIteratorOfListOfStatus it(analyzer.Result(shape)->Status());
            if (it.Value() != BRepCheck_NoError) {
                branch = add(parent, shape, static_cast<int>(it.Value()));
            }
        }
        checked.Add(shape);
        if (shape.ShapeType() == TopAbs_SOLID) {
            sub(shape, TopAbs_SHELL, branch);
        }
        if (shape.ShapeType() == TopAbs_EDGE) {
            sub(shape, TopAbs_VERTEX, branch);
        }
        if (shape.ShapeType() == TopAbs_FACE) {
            sub(shape, TopAbs_WIRE, branch);
            sub(shape, TopAbs_EDGE, branch);
            sub(shape, TopAbs_VERTEX, branch);
        }
        for (TopoDS_Iterator it(shape); it.More(); it.Next()) {
            recurse(it.Value(), branch);
        }
    }
};

}  // namespace

std::vector<GeometryCheck::Finding> GeometryCheck::check(const TopoDS_Shape& shape,
                                                         const Options& options,
                                                         const Message_ProgressRange& progress)
{
    std::vector<Finding> found;
    if (shape.IsNull()) {
        return found;
    }
    BRepCheck_Analyzer analyzer(shape);
    if (!analyzer.IsValid()) {
        Finding self;
        self.type = static_cast<int>(shape.ShapeType());
        found.push_back(self);
        Walk walk {analyzer, shape, found, {}};
        walk.recurse(shape, 0);
        return found;
    }
    if (!options.bop) {
        return found;
    }
    // BOPAlgo_ArgumentAnalyzer can be really slow: only on a shape
    // BRepCheck_Analyzer passes, and on a copy, as BRepAlgoAPI_Check does
    // (src/BOPTest/BOPTest_CheckCommands.cxx). The copy keeps the shape's
    // order, so an index in it is the index in the shape
    TopoDS_Shape copy = BRepBuilderAPI_Copy(shape).Shape();
    BOPAlgo_ArgumentAnalyzer bop;
    bop.SetShape1(copy);
    bop.ArgumentTypeMode() = options.argumentType;
    bop.SelfInterMode() = options.selfInter;
    bop.SmallEdgeMode() = options.smallEdge;
    bop.RebuildFaceMode() = options.rebuildFace;
    bop.ContinuityMode() = options.continuity;
    bop.SetParallelMode(!options.singleThreaded);
    bop.SetRunParallel(!options.singleThreaded);
    bop.TangentMode() = options.tangent;
    bop.MergeVertexMode() = options.mergeVertex;
    bop.MergeEdgeMode() = options.mergeEdge;
    bop.CurveOnSurfaceMode() = options.curveOnSurface;
    bop.Perform(progress);
    if (!bop.HasFaulty()) {
        return found;
    }
    Finding self;
    self.type = static_cast<int>(shape.ShapeType());
    self.source = Source::Bop;
    found.push_back(self);
    for (const BOPAlgo_CheckResult& result : bop.GetCheckResult()) {
        for (const TopoDS_Shape& faulty : result.GetFaultyShapes1()) {
            TopTools_IndexedMapOfShape map;
            TopExp::MapShapes(copy, faulty.ShapeType(), map);
            Finding f;
            f.parent = 0;
            f.type = static_cast<int>(faulty.ShapeType());
            f.index = map.FindIndex(faulty);
            f.source = Source::Bop;
            f.status = static_cast<int>(result.GetCheckStatus());
            found.push_back(f);
        }
    }
    return found;
}
