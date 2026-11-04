// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

#include <algorithm>
#include <cmath>
#include <functional>

#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepFill.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Geom_Curve.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalConvert.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Recognise.h"

using namespace PartDesign;
using Part::TopoShape;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalConvert, PartDesign::Feature)

SheetMetalConvert::SheetMetalConvert()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(FixedFace,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A face of the part, a wall's skin: the wall the flat lies on");
    // the part is the body's base, which lies outside it
    FixedFace.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(OpenFaces,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A solid's faces to leave open");
    OpenFaces.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(RipEdges,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "Edges of a solid or a shell to rip: a gap of the rule's across each, "
                      "not a bend");
    RipEdges.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(Side,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Which way a solid or a shell is thickened from its faces: Inside "
                      "keeps its size, Outside makes its faces the inner skins");
    static const char* sides[] = {"Inside", "Outside", nullptr};
    Side.setEnums(sides);
    ADD_PROPERTY_TYPE(Kind,
                      (""),
                      "Sheet Metal",
                      static_cast<App::PropertyType>(App::Prop_Output | App::Prop_ReadOnly),
                      "What the part was taken for");
}

short SheetMetalConvert::mustExecute() const
{
    if (FixedFace.isTouched() || OpenFaces.isTouched() || RipEdges.isTouched()
        || Side.isTouched()) {
        return 1;
    }
    return PartDesign::Feature::mustExecute();
}

namespace
{

/// The face of base that is face: the same, else one lying where it lies
TopoDS_Face faceOf(const TopoShape& base, const TopoDS_Shape& face)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    const gp_Pnt centre = props.CentreOfMass();
    const double area = props.Mass();
    for (TopExp_Explorer it(base.getShape(), TopAbs_FACE); it.More(); it.Next()) {
        if (it.Current().IsPartner(face)) {
            return TopoDS::Face(it.Current());
        }
    }
    for (TopExp_Explorer it(base.getShape(), TopAbs_FACE); it.More(); it.Next()) {
        GProp_GProps p;
        BRepGProp::SurfaceProperties(it.Current(), p);
        if (std::abs(p.Mass() - area) <= 1e-6 * std::max(1.0, area)
            && p.CentreOfMass().Distance(centre) <= 1e-6) {
            return TopoDS::Face(it.Current());
        }
    }
    return {};
}

/// The edge of base that is edge: the same, else one with its ends
TopoDS_Edge edgeOf(const TopoShape& base, const TopoDS_Shape& edge)
{
    for (TopExp_Explorer it(base.getShape(), TopAbs_EDGE); it.More(); it.Next()) {
        if (it.Current().IsPartner(edge)) {
            return TopoDS::Edge(it.Current());
        }
    }
    TopoDS_Vertex a, b;
    TopExp::Vertices(TopoDS::Edge(edge), a, b);
    const gp_Pnt pa = BRep_Tool::Pnt(a), pb = BRep_Tool::Pnt(b);
    for (TopExp_Explorer it(base.getShape(), TopAbs_EDGE); it.More(); it.Next()) {
        TopoDS_Vertex c, d;
        TopExp::Vertices(TopoDS::Edge(it.Current()), c, d);
        const gp_Pnt pc = BRep_Tool::Pnt(c), pd = BRep_Tool::Pnt(d);
        if ((pa.Distance(pc) < 1e-6 && pb.Distance(pd) < 1e-6)
            || (pa.Distance(pd) < 1e-6 && pb.Distance(pc) < 1e-6)) {
            return TopoDS::Edge(it.Current());
        }
    }
    return {};
}

/// A plane face's normal out of the material
gp_Dir outward(const TopoDS_Face& face)
{
    gp_Dir n = BRepAdaptor_Surface(face).Plane().Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) {
        n.Reverse();
    }
    return n;
}

/** A shell thickened into a solid: the shell, its offset along its faces'
 * normals with sharp corners, and a side across each free edge and the one
 * offset from it. OCCT's own thickening of a shell fails on these. Its faces
 * are planes, as a sheet's walls are.
 */
TopoShape thickShell(const TopoShape& shell, double offset)
{
    for (TopExp_Explorer it(shell.getShape(), TopAbs_FACE); it.More(); it.Next()) {
        if (BRepAdaptor_Surface(TopoDS::Face(it.Current())).GetType() != GeomAbs_Plane) {
            THROWM(Base::ValueError, "A shell is taken of plane faces only: only planar walls "
                                     "are taken (rolled walls come later)");
        }
    }
    const TopoShape moved = TopoShape().makEOffset(shell, offset, 1e-6, false, false, 0,
                                                   TopoShape::JoinType::Intersection);
    auto freeEdges = [](const TopoDS_Shape& s) {
        TopTools_IndexedDataMapOfShapeListOfShape map;
        TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, map);
        std::vector<std::pair<TopoDS_Edge, TopoDS_Face>> res;
        for (int i = 1; i <= map.Extent(); ++i) {
            if (map(i).Extent() == 1) {
                res.emplace_back(TopoDS::Edge(map.FindKey(i)), TopoDS::Face(map(i).First()));
            }
        }
        return res;
    };
    BRepBuilderAPI_Sewing sewing(1e-6);
    for (TopExp_Explorer it(shell.getShape(), TopAbs_FACE); it.More(); it.Next()) {
        sewing.Add(it.Current());
    }
    for (TopExp_Explorer it(moved.getShape(), TopAbs_FACE); it.More(); it.Next()) {
        sewing.Add(it.Current().Reversed());
    }
    const auto ends = [](const TopoDS_Edge& e) {
        BRepAdaptor_Curve c(e);
        return std::make_pair(c.Value(c.FirstParameter()), c.Value(c.LastParameter()));
    };
    const auto movedFree = freeEdges(moved.getShape());
    for (const auto& [edge, face] : freeEdges(shell.getShape())) {
        // its offset lies on it moved along its face's normal
        const gp_Vec move = gp_Vec(outward(face)) * offset;
        const Handle(Geom_Curve) curve = Handle(Geom_Curve)::DownCast(
            BRepAdaptor_Curve(edge).Curve().Curve()->Translated(move));
        auto on = [&](const gp_Pnt& p) {
            GeomAPI_ProjectPointOnCurve project(p, curve);
            return project.NbPoints() > 0 && project.LowerDistance() <= 1e-6;
        };
        TopoDS_Edge other;
        for (const auto& candidate : movedFree) {
            BRepAdaptor_Curve c(candidate.first);
            const double mid = (c.FirstParameter() + c.LastParameter()) / 2;
            if (on(c.Value(c.FirstParameter())) && on(c.Value(mid))
                && on(c.Value(c.LastParameter()))) {
                other = candidate.first;
                break;
            }
        }
        if (other.IsNull()) {
            THROWM(Base::CADKernelError, "The shell could not be thickened at an open edge");
        }
        auto [p1, p2] = ends(edge);
        auto [q1, q2] = ends(other);
        if (q1.Distance(p1.Translated(move)) > q2.Distance(p1.Translated(move))) {
            std::swap(q1, q2);
            other.Reverse();
        }
        if (BRepAdaptor_Curve(edge).GetType() == GeomAbs_Line) {
            BRepBuilderAPI_MakePolygon side(p1, p2, q2, q1, true);
            sewing.Add(BRepBuilderAPI_MakeFace(side.Wire(), true).Face());
        }
        else {
            sewing.Add(BRepFill::Face(edge, other));
        }
    }
    sewing.Perform();
    TopExp_Explorer sewn(sewing.SewedShape(), TopAbs_SHELL);
    if (!sewn.More()) {
        THROWM(Base::CADKernelError, "The shell could not be thickened");
    }
    BRepBuilderAPI_MakeSolid solid(TopoDS::Shell(sewn.Current()));
    ShapeFix_Solid fix;
    fix.Init(solid.Solid());
    fix.Perform();
    return TopoShape(fix.Solid());
}

/// The line edges of shape lying on the line through p along d
std::vector<TopoShape> edgesOn(const TopoShape& shape, const gp_Pnt& p, const gp_Dir& d)
{
    std::vector<TopoShape> res;
    const gp_Lin line(p, d);
    for (int i = 1; i <= shape.countSubShapes(TopAbs_EDGE); ++i) {
        const TopoShape edge = shape.getSubTopoShape(TopAbs_EDGE, i);
        BRepAdaptor_Curve c(TopoDS::Edge(edge.getShape()));
        if (c.GetType() != GeomAbs_Line
            || !c.Line().Direction().IsParallel(d, Precision::Angular())) {
            continue;
        }
        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        if (line.Distance(mid) < 1e-6) {
            res.push_back(edge);
        }
    }
    return res;
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalConvert::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    try {
        TopoShape base = getBaseShape(false, false, false);
        // a shell, open; a body's base feature makes it a solid, open as it was
        const bool shell = base.countSubShapes(TopAbs_SOLID) <= 1
            && base.countSubShapes(TopAbs_SHELL) == 1
            && !BRep_Tool::IsClosed(base.getSubShape(TopAbs_SHELL, 1));
        if (shell) {
            base = base.getSubTopoShape(TopAbs_SHELL, 1);
        }
        const auto& subs = FixedFace.getSubValues();
        if (!FixedFace.getValue() || subs.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "Convert needs a fixed face, a wall's skin"));
        }
        const TopoShape picked =
            Part::Feature::getTopoShape(FixedFace.getValue(), subs.front().c_str(), true);
        if (picked.isNull() || picked.getShape().ShapeType() != TopAbs_FACE) {
            THROWM(Base::ValueError, "The fixed face is not a face");
        }
        const TopoDS_Face fixed = faceOf(base, picked.getShape());
        if (fixed.IsNull()) {
            THROWM(Base::ValueError, "The fixed face is not a face of the part converted");
        }

        const double T = rule->Thickness.getValue();
        const auto hasher = getDocument()->getStringHasher();
        auto pickedOf = [&](App::PropertyLinkSub& link, bool faces) {
            std::vector<TopoDS_Shape> res;
            for (const auto& sub : link.getSubValues()) {
                const TopoShape s =
                    Part::Feature::getTopoShape(link.getValue(), sub.c_str(), true);
                const TopoDS_Shape found =
                    s.isNull() ? TopoDS_Shape()
                               : (faces ? TopoDS_Shape(faceOf(base, s.getShape()))
                                        : TopoDS_Shape(edgeOf(base, s.getShape())));
                if (found.IsNull()) {
                    FC_THROWM(Base::ValueError,
                              link.getName() << ": " << sub << " is not of the part converted");
                }
                res.push_back(found);
            }
            return res;
        };
        const std::vector<TopoDS_Shape> open = pickedOf(OpenFaces, true);
        if (shell && !open.empty()) {
            THROWM(Base::ValueError, "A shell is open where it has no face: Open Faces are a "
                                     "solid's");
        }
        const std::vector<TopoDS_Shape> rips = pickedOf(RipEdges, false);

        // A thin part, the rule's thickness apart, as it is; else a solid,
        // thickened
        TopoShape part = base;
        TopoDS_Face fixedFace = fixed;
        const bool thin = open.empty() && rips.empty() && base.countSubShapes(TopAbs_SOLID) == 1
            && std::abs(SheetMetal::facingThickness(base.getShape(), fixed) - T) <= 1e-6;
        if (!thin) {
            part = thickened(base, fixed, open, rips, *rule);
            // the fixed face's plane holds a skin of the thickened part
            const gp_Pln plane = BRepAdaptor_Surface(fixed).Plane();
            const gp_Dir out = BRepAdaptor_Surface(fixed).Plane().Axis().Direction();
            double area = 0;
            fixedFace.Nullify();
            for (TopExp_Explorer it(part.getShape(), TopAbs_FACE); it.More(); it.Next()) {
                const TopoDS_Face& face = TopoDS::Face(it.Current());
                BRepAdaptor_Surface s(face);
                // Outside, the face is an inner skin, facing the other way
                if (s.GetType() != GeomAbs_Plane
                    || !outward(face).IsParallel(out, Precision::Angular())
                    || plane.Distance(s.Plane().Location()) > 1e-6) {
                    continue;
                }
                GProp_GProps props;
                BRepGProp::SurfaceProperties(face, props);
                if (props.Mass() > area) {
                    area = props.Mass();
                    fixedFace = face;
                }
            }
            if (fixedFace.IsNull()) {
                THROWM(Base::ValueError, "The fixed face is no skin of the thickened part");
            }
        }

        SheetMetal::Recognised found = SheetMetal::recognise(
            part.getShape(), fixedFace, [rule](double angle, double radius) {
                return rule->modelK(angle, radius);
            });
        if (std::abs(found.model.thickness - T) > 1e-6) {
            FC_THROWM(Base::ValueError,
                      "The part is " << found.model.thickness << " mm thick, the rule "
                                     << T << " mm: set the rule's thickness to the part's");
        }

        // split where each bend meets its walls, every face in one element
        TopoShape result = part;
        if (!found.cutters.empty()) {
            BRepAlgoAPI_Splitter split;
            TopTools_ListOfShape args, tools;
            args.Append(part.getShape());
            for (const TopoDS_Face& cutter : found.cutters) {
                tools.Append(cutter);
            }
            split.SetArguments(args);
            split.SetTools(tools);
            split.Build();
            if (!split.IsDone()) {
                THROWM(Base::CADKernelError, "The part could not be split where its bends meet "
                                             "its walls");
            }
            result = TopoShape(0, hasher).makEShape(split, {part});
            // The cutters cross the sheet whole and part it into pieces: fused
            // again, the faces between them go, the faces they split stay split
            const auto pieces = result.getSubTopoShapes(TopAbs_SOLID);
            if (pieces.size() > 1) {
                result = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Fuse, pieces);
            }
            result = getSolid(result);
        }
        // every face one element's
        int index = 0;
        for (TopExp_Explorer it(result.getShape(), TopAbs_FACE); it.More(); it.Next()) {
            ++index;
            if (found.model.owner(TopoDS::Face(it.Current())) < 0) {
                FC_THROWM(Base::ValueError, "Face" << index << " is in no wall or bend");
            }
        }
        Shape.setValue(result);
        SheetModel.setValue(found.model);
        const char* kind = thin ? "Thin part" : (shell ? "Shell" : "Solid");
        if (Kind.getStrValue() != kind) {
            Kind.setValue(kind);
        }
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}

TopoShape SheetMetalConvert::thickened(const TopoShape& base,
                                       const TopoDS_Face& fixed,
                                       const std::vector<TopoDS_Shape>& open,
                                       const std::vector<TopoDS_Shape>& rips,
                                       const SheetMetalRule& rule)
{
    (void)fixed;
    const bool shell = base.getShape().ShapeType() == TopAbs_SHELL;
    if (!shell && base.countSubShapes(TopAbs_SOLID) != 1) {
        THROWM(Base::ValueError, "Convert takes a thin part, one solid or one shell");
    }
    const double T = rule.Thickness.getValue();
    const double R = rule.BendRadius.getValue();
    const double gap = rule.Gap.getValue();
    const bool inside = Side.getValue() == 0;
    const auto hasher = getDocument()->getStringHasher();
    auto isOpen = [&](const TopoDS_Shape& face) {
        return std::any_of(open.begin(), open.end(), [&](const TopoDS_Shape& f) {
            return f.IsSame(face);
        });
    };
    auto isRip = [&](const TopoDS_Shape& edge) {
        return std::any_of(rips.begin(), rips.end(), [&](const TopoDS_Shape& e) {
            return e.IsSame(edge);
        });
    };

    // The edges between two faces kept: bends but where ripped. Bends that
    // join the faces round in a loop do not lie flat.
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(base.getShape(), TopAbs_FACE, faces);
    std::vector<int> group(faces.Extent() + 1);
    for (int i = 0; i <= faces.Extent(); ++i) {
        group[i] = i;
    }
    std::function<int(int)> root = [&](int i) {
        return group[i] == i ? i : group[i] = root(group[i]);
    };
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(base.getShape(), TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    struct Edge
    {
        TopoDS_Edge edge;
        gp_Pnt p;
        gp_Dir d;
        gp_Dir na, nb;
        double length;
    };
    std::vector<Edge> bends, ripped;
    for (int i = 1; i <= edgeFaces.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(edgeFaces.FindKey(i));
        std::vector<TopoDS_Face> two;
        for (const TopoDS_Shape& f : edgeFaces(i)) {
            if (!isOpen(f) && std::none_of(two.begin(), two.end(), [&](const TopoDS_Face& g) {
                    return g.IsSame(f);
                })) {
                two.push_back(TopoDS::Face(f));
            }
        }
        if (two.size() != 2) {
            continue;
        }
        const int index = edgeFaces.FindIndex(edge);
        BRepAdaptor_Curve c(edge);
        if (c.GetType() != GeomAbs_Line
            || BRepAdaptor_Surface(two[0]).GetType() != GeomAbs_Plane
            || BRepAdaptor_Surface(two[1]).GetType() != GeomAbs_Plane) {
            FC_THROWM(Base::ValueError,
                      "Edge" << index
                             << " is curved or between curved faces: only planar walls are "
                                "taken (rolled walls come later)");
        }
        Edge e {edge,
                c.Value(c.FirstParameter()),
                c.Line().Direction(),
                outward(two[0]),
                outward(two[1]),
                c.LastParameter() - c.FirstParameter()};
        if (e.na.IsParallel(e.nb, Precision::Angular())) {
            continue;
        }
        if (isRip(edge)) {
            ripped.push_back(e);
            continue;
        }
        const int a = root(faces.FindIndex(two[0])), b = root(faces.FindIndex(two[1]));
        if (a == b) {
            FC_THROWM(Base::ValueError,
                      "The bends close a loop round the part at Edge"
                          << index << ", which does not lie flat: rip an edge of the loop");
        }
        group[a] = b;
        bends.push_back(e);
    }

    // thickened from its faces, the open ones taken away
    std::vector<TopoShape> openFaces;
    for (const TopoDS_Shape& f : open) {
        openFaces.emplace_back(f);
    }
    TopoShape res(0, hasher);
    if (shell) {
        // a shell thickened against its faces' normals, or with them
        res = getSolid(thickShell(base, inside ? -T : T));
    }
    else {
        TopoShape from = base;
        if (!inside) {
            // Outside: the part grown by T but at its open faces, then thickened
            // inward as Inside is -- thickened outward, OCCT cannot round the
            // outer edges it makes
            TopoShape skin(0, hasher);
            skin.makEThickSolid(base, openFaces, T, 1e-6, false, false, 0,
                                TopoShape::JoinType::Intersection);
            from = TopoShape(0, hasher)
                       .makEBoolean(Part::OpCodes::Fuse, {skin, base})
                       .makERefine();
            from = getSolid(from);
            // the open faces again, on the grown part: in each one's plane, facing
            // as it does
            openFaces.clear();
            for (const TopoDS_Shape& f : open) {
                const gp_Pln plane = BRepAdaptor_Surface(TopoDS::Face(f)).Plane();
                const gp_Dir out = outward(TopoDS::Face(f));
                for (TopExp_Explorer it(from.getShape(), TopAbs_FACE); it.More(); it.Next()) {
                    const TopoDS_Face& face = TopoDS::Face(it.Current());
                    BRepAdaptor_Surface s(face);
                    if (s.GetType() == GeomAbs_Plane
                        && outward(face).IsEqual(out, Precision::Angular())
                        && plane.Distance(s.Plane().Location()) <= 1e-6
                        && std::none_of(openFaces.begin(),
                                        openFaces.end(),
                                        [&](const TopoShape& g) {
                                            return g.getShape().IsSame(face);
                                        })) {
                        openFaces.emplace_back(face);
                    }
                }
            }
        }
        res.makEThickSolid(from, openFaces, -T, 1e-6, false, false, 0,
                           TopoShape::JoinType::Intersection);
    }

    // Each rip a gap across the corner, on the plane halving it. Where bends
    // meet at a rip's end, a corner relief as large as their zones: there the
    // bends end, square, and each rounding ends on a plane square to it.
    std::vector<TopoShape> cuts;
    TopTools_IndexedMapOfShape corners;
    TopExp::MapShapes(res.getShape(), TopAbs_VERTEX, corners);
    auto setback = [&](const Edge& e) {
        return (R + T) * std::tan(std::acos(std::max(-1.0, std::min(1.0, e.na.Dot(e.nb)))) / 2);
    };
    for (const Edge& e : ripped) {
        const gp_Vec m = gp_Vec(e.na) - gp_Vec(e.nb);
        if (m.Magnitude() < Precision::Confusion()) {
            continue;
        }
        const double margin = R + 2 * T + 1;
        // the gap's two ends along the rip, from its first vertex
        double ends[2] = {-margin, e.length + margin};
        for (int end = 0; end < 2; ++end) {
            const gp_Pnt v = e.p.Translated(gp_Vec(e.d) * (end ? e.length : 0));
            const gp_Vec along = gp_Vec(e.d) * (end ? -1 : 1);  // into the rip
            // the bends from this end, each running away from it
            std::vector<std::pair<const Edge*, gp_Vec>> met;
            for (const Edge& b : bends) {
                const gp_Pnt far = b.p.Translated(gp_Vec(b.d) * b.length);
                if (b.p.Distance(v) <= Precision::Confusion()) {
                    met.emplace_back(&b, gp_Vec(b.d));
                }
                else if (far.Distance(v) <= Precision::Confusion()) {
                    met.emplace_back(&b, -gp_Vec(b.d));
                }
            }
            if (met.size() != 2) {
                continue;
            }
            // the sheet's outer corner there: the vertex itself inside, the
            // nearest one outside
            gp_Pnt corner = v;
            double best = -1;
            for (int i = 1; i <= corners.Extent(); ++i) {
                const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(corners(i)));
                if (best < 0 || q.Distance(v) < best) {
                    best = q.Distance(v);
                    corner = q;
                }
            }
            // past the zones by half a gap: square on them, a relief's sides
            // would lie in the planes where the other bend meets its walls
            const double past = std::max(gap, T / 10) / 2;
            const double s1 = setback(*met[0].first) + past;
            const double s2 = setback(*met[1].first) + past;
            const double sMax = std::max(s1, s2);
            const gp_Vec u1 = met[0].second, u2 = met[1].second;
            const double over = T;
            const gp_Pnt p0 = corner.Translated(-(u1 + u2 + along) * over);
            const gp_Vec a = u1 * (s2 + over), b = u2 * (s1 + over);
            BRepBuilderAPI_MakePolygon square(p0,
                                              p0.Translated(a),
                                              p0.Translated(a + b),
                                              p0.Translated(b),
                                              true);
            cuts.emplace_back(
                BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(square.Wire()).Face(),
                                      along * (sMax + over))
                    .Shape());
            // the gap from within the relief, clear of the skins
            const double at = gp_Vec(e.p, corner).Dot(gp_Vec(e.d))
                + (end ? -1 : 1) * (sMax - past - R / 2);
            ends[end] = at;
        }
        const gp_Dir normal(m);
        const gp_Dir w = e.d.Crossed(normal);
        const double half = 2 * (R + T) + T;
        const gp_Pnt origin = e.p.Translated(gp_Vec(e.d) * ends[0] + gp_Vec(w) * half
                                             - gp_Vec(normal) * (gap / 2));
        cuts.emplace_back(
            BRepPrimAPI_MakeBox(gp_Ax2(origin, normal, e.d), ends[1] - ends[0], 2 * half, gap)
                .Solid());
    }
    if (!cuts.empty()) {
        res = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                               {res, TopoShape().makECompound(cuts)});
        res = getSolid(res);
    }

    // each bend's two sharp edges rounded: the outer one by R + T, the
    // inner one by R, a bend of the sheet's thickness
    std::vector<TopoShape> outer, inner;
    for (const Edge& e : bends) {
        const gp_Vec shift = (gp_Vec(e.na) + gp_Vec(e.nb)) * (T / (1 + e.na.Dot(e.nb)));
        const gp_Pnt mid = e.p.Translated(gp_Vec(e.d) * (e.length / 2));
        const gp_Pnt onOuter = inside ? mid : mid.Translated(shift);
        const gp_Pnt onInner = inside ? mid.Translated(-shift) : mid;
        for (const TopoShape& edge : edgesOn(res, onOuter, e.d)) {
            outer.push_back(edge);
        }
        for (const TopoShape& edge : edgesOn(res, onInner, e.d)) {
            inner.push_back(edge);
        }
    }
    if (!outer.empty()) {
        res = TopoShape(0, hasher).makEFillet(res, outer, R + T, R + T);
    }
    // found again: the first rounding made the shape anew
    inner.clear();
    for (const Edge& e : bends) {
        const gp_Vec shift = (gp_Vec(e.na) + gp_Vec(e.nb)) * (T / (1 + e.na.Dot(e.nb)));
        const gp_Pnt mid = e.p.Translated(gp_Vec(e.d) * (e.length / 2));
        const gp_Pnt onInner = inside ? mid.Translated(-shift) : mid;
        for (const TopoShape& edge : edgesOn(res, onInner, e.d)) {
            inner.push_back(edge);
        }
    }
    if (!inner.empty()) {
        res = TopoShape(0, hasher).makEFillet(res, inner, R, R);
    }

    return getSolid(res);
}
