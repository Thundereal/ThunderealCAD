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
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_Sequence.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDF_Label.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_View.hxx>
#include <XCAFDoc_ClippingPlaneTool.hxx>
#include <XCAFDoc_ViewTool.hxx>
#include <XCAFView_Object.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <TCollection_ExtendedString.hxx>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>
#include <Base/Placement.h>
#include <Mod/Import/App/ExportOCAF2.h>
#include <App/GeoFeature.h>
#include <Mod/Part/App/BodyBase.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>

#include "AnnotationSet.h"
#include "Datum.h"
#include "Dimension.h"
#include "GeometricTolerance.h"
#include "Note.h"
#include "StepExport.h"
#include "TolerancedFeature.h"
#include "View.h"


using namespace PMI;

namespace
{

Handle(TCollection_HAsciiString) ascii(const std::string& s)
{
    // STEP's strings are ASCII: what is not, in UTF-16 between \X2\ and
    // \X0\ (ISO 10303-21 6.4.3.3), which the writer passes as it is -- a
    // label of a counterbore sign alone is no empty name (SFA, CTC-04)
    std::string out;
    std::u16string wide;
    auto flush = [&]() {
        if (wide.empty()) {
            return;
        }
        out += "\\X2\\";
        char hex[8];
        for (char16_t u : wide) {
            std::snprintf(hex, sizeof(hex), "%04X", unsigned(u));
            out += hex;
        }
        out += "\\X0\\";
        wide.clear();
    };
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 128) {
            flush();
            out += char(c);
            ++i;
            continue;
        }
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        char32_t cp = n == 3 ? c & 0x07 : n == 2 ? c & 0x0F : c & 0x1F;
        if (n == 0 || i + n >= s.size()) {
            // Not UTF-8: dropped
            ++i;
            continue;
        }
        for (int k = 1; k <= n; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        i += n + 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            wide += char16_t(0xD800 + (cp >> 10));
            wide += char16_t(0xDC00 + (cp & 0x3FF));
        }
        else {
            wide += char16_t(cp);
        }
    }
    flush();
    return new TCollection_HAsciiString(out.c_str());
}

/// A graphic's name: its label, as NIST names its graphics ('Text.12'), or
/// the object's own name when the label has no letter or digit of ASCII --
/// SFA reads a name all in \X2\ (a counterbore sign alone) as none, which
/// its checker reports missing (CTC-04, FTC-07)
Handle(TCollection_HAsciiString) nameOf(const App::DocumentObject* obj)
{
    std::string label = obj->Label.getValue();
    for (char c : label) {
        if (Step::isAsciiAlnum(c)) {
            return ascii(label);
        }
    }
    return new TCollection_HAsciiString(obj->getNameInDocument());
}

/// A callout's graphic's name: the file's, while drawn as the file had it
/// (a placeholder's for ours drawn in its place: STC-07's 31 radii are each
/// 'Linear Size.13', the one graphic that presents them all); else its own
Handle(TCollection_HAsciiString) graphicName(const Callout* c)
{
    std::string name = c->ImportedGraphicName.getValue();
    if (c->UseImported.getValue()
        && std::any_of(name.begin(), name.end(), [](char ch) { return Step::isAsciiAlnum(ch); })) {
        return ascii(name);
    }
    return nameOf(c);
}

gp_Trsf trsfOf(const Base::Placement& pla)
{
    gp_Trsf t;
    Part::TopoShape::convertTogpTrsf(pla.toMatrix(), t);
    return t;
}

gp_Ax2 ax2Of(const gp_Trsf& t)
{
    gp_Ax2 ax(gp::Origin(), gp::DZ(), gp::DX());
    return ax.Transformed(t);
}

/// An exported part: its shape as the writer has it, and the way from the
/// annotations' coordinates into that shape's
struct Exported
{
    TDF_Label label;
    TopoDS_Shape shape;
    /// The set's coordinates -- a Body's own, else those the part is
    /// placed in -- to the label's shape's
    gp_Trsf toLocal;
    /// Its faces, edges and vertices by what they share with the
    /// document's: their TShape
    std::map<const TopoDS_TShape*, TopoDS_Shape> elements;
    /// The part's shape as placed, its elements by TShape to their index;
    /// the label's by index. A shape the export baked its placement into
    /// is a copy, sharing no TShape, its elements in the same order.
    std::map<const TopoDS_TShape*, std::pair<TopAbs_ShapeEnum, int>> placedIndex;
    std::map<std::pair<TopAbs_ShapeEnum, int>, TopoDS_Shape> byIndex;
    /// The part's shape as placed, with its element map
    Part::TopoShape placed;
    /// The set's coordinates to the placed shape's
    gp_Trsf setToPlaced;
    /// The placed shape's -- the document's, where a view's camera is -- to
    /// the label's
    gp_Trsf placedToLabel;

    /// The label's element of index \a i of the placed shape's \a type
    TopoDS_Shape at(TopAbs_ShapeEnum type, int i) const
    {
        auto found = byIndex.find(std::make_pair(type, i));
        return found == byIndex.end() ? TopoDS_Shape() : found->second;
    }

    /// The label's own element for \a el, or a null shape
    TopoDS_Shape find(const TopoDS_Shape& el) const
    {
        auto it = elements.find(el.TShape().get());
        if (it != elements.end()) {
            return it->second;
        }
        auto at = placedIndex.find(el.TShape().get());
        if (at != placedIndex.end()) {
            auto found = byIndex.find(at->second);
            if (found != byIndex.end()) {
                return found->second;
            }
        }
        return {};
    }
};

class Writer
{
public:
    explicit Writer(const Import::PostExport& info)
        : _info(info)
    {
        TDF_Label main = info.hDoc->Main();
        _shapes = XCAFDoc_DocumentTool::ShapeTool(main);
        _tool = XCAFDoc_DocumentTool::DimTolTool(main);
    }

    Step::WriteReport run()
    {
        // The sets of the parts exported, each once: a part and a link to
        // it share a shape
        std::set<App::Document*> docs;
        for (auto obj : _info.objects) {
            if (obj && obj->isAttachedToDocument()) {
                docs.insert(obj->getDocument());
            }
        }
        std::set<std::string> done;
        for (auto doc : docs) {
            for (auto o : doc->getObjectsOfType(AnnotationSet::getClassTypeId())) {
                auto set = static_cast<AnnotationSet*>(o);
                auto part = set->getPart();
                if (!part || !std::count(_info.objects.begin(), _info.objects.end(), part)) {
                    continue;
                }
                Exported* ex = exported(part, set);
                if (!ex) {
                    _report.skipped.push_back(std::string(set->Label.getValue())
                                              + ": its part's shape was not exported whole");
                    continue;
                }
                std::string key = std::string(set->getFullName());
                if (!done.insert(key).second) {
                    continue;
                }
                write(set, *ex);
            }
        }
        for (auto& [v, ex] : _views) {
            guard(v, [&, v = v, ex = ex]() { view(v, *ex); });
        }
        return _report;
    }

private:
    /// The part as exported, made once
    Exported* exported(App::DocumentObject* part, const AnnotationSet* set)
    {
        auto it = _parts.find(part);
        if (it != _parts.end()) {
            return it->second.shape.IsNull() ? nullptr : &it->second;
        }
        Exported& ex = _parts[part];
        TDF_Label label = _info.labelOf(part);
        if (label.IsNull()) {
            return nullptr;
        }
        TDF_Label proto = label;
        if (XCAFDoc_ShapeTool::IsReference(label)) {
            XCAFDoc_ShapeTool::GetReferredShape(label, proto);
        }
        TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(proto);
        if (shape.IsNull()) {
            return nullptr;
        }
        Part::TopoShape placed = Part::Feature::getTopoShape(part);
        for (auto type : {TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX}) {
            NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> map;
            TopExp::MapShapes(shape, type, map);
            for (int i = 1; i <= map.Extent(); ++i) {
                ex.elements.emplace(map(i).TShape().get(), map(i));
                ex.byIndex.emplace(std::make_pair(type, i), map(i));
            }
            NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> mine;
            TopExp::MapShapes(placed.getShape(), type, mine);
            for (int i = 1; i <= mine.Extent(); ++i) {
                ex.placedIndex.emplace(mine(i).TShape().get(), std::make_pair(type, i));
            }
        }
        // The same face as placed and as exported: what moves one to the
        // other. A copy the export baked the placement into shares none, and
        // is placed as the part is.
        gp_Trsf placedToLabel;
        for (TopExp_Explorer xp(placed.getShape(), TopAbs_FACE); xp.More(); xp.Next()) {
            auto found = ex.elements.find(xp.Current().TShape().get());
            if (found != ex.elements.end()) {
                placedToLabel = found->second.Location().Transformation()
                    * xp.Current().Location().Transformation().Inverted();
                break;
            }
        }
        // The set's coordinates: a Body's own when it is in one (the part
        // is that Body), else those the part is placed in
        gp_Trsf setToPlaced;
        if (set && Part::BodyBase::findBodyOf(set) == part) {
            if (auto geo = freecad_cast<App::GeoFeature*>(part)) {
                setToPlaced = trsfOf(geo->Placement.getValue());
            }
        }
        ex.toLocal = placedToLabel * setToPlaced;
        ex.setToPlaced = setToPlaced;
        ex.placedToLabel = placedToLabel;
        ex.placed = placed;
        ex.label = proto;
        ex.shape = shape;
        return &ex;
    }

    static TopAbs_ShapeEnum typeOf(const char* name)
    {
        return !std::strcmp(name, "Face") ? TopAbs_FACE
            : !std::strcmp(name, "Edge") ? TopAbs_EDGE
            : !std::strcmp(name, "Vertex") ? TopAbs_VERTEX
                                         : TopAbs_SHAPE;
    }

    /// What tells an element from the others of its kind: its centre and its
    /// size (a face's area, an edge's length)
    static std::pair<gp_Pnt, double> signature(const TopoDS_Shape& s)
    {
        if (s.ShapeType() == TopAbs_VERTEX) {
            return {BRep_Tool::Pnt(TopoDS::Vertex(s)), 0.0};
        }
        GProp_GProps props;
        if (s.ShapeType() == TopAbs_FACE) {
            BRepGProp::SurfaceProperties(s, props);
        }
        else {
            BRepGProp::LinearProperties(s, props);
        }
        return {props.CentreOfMass(), props.Mass()};
    }

    /// The label's element that is \a el, \a sub of \a obj: the same
    /// TShape; else the same element map name in the part's shape -- a
    /// Body's shape and its solids' are copies of their tip's; else the same
    /// place and size
    TopoDS_Shape locate(Exported& ex, App::DocumentObject* obj, const std::string& sub,
                        const TopoDS_Shape& el)
    {
        TopoDS_Shape mine = ex.find(el);
        if (!mine.IsNull()) {
            return mine;
        }
        try {
            Part::TopoShape whole = Part::Feature::getTopoShape(obj);
            Data::MappedElement named = whole.getElementName(sub.c_str());
            if (named.name) {
                Data::IndexedName idx = ex.placed.getIndexedName(named.name);
                if (idx) {
                    mine = ex.at(typeOf(idx.getType()), idx.getIndex());
                    if (!mine.IsNull()) {
                        return mine;
                    }
                }
            }
        }
        catch (const Base::Exception&) {
        }
        TopoDS_Shape moved = el.Moved(TopLoc_Location(ex.setToPlaced));
        auto want = signature(moved);
        double tol = 1e-6 * std::max(1.0, want.first.XYZ().Modulus() + want.second);
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> map;
        TopExp::MapShapes(ex.placed.getShape(), el.ShapeType(), map);
        int found = 0;
        for (int i = 1; i <= map.Extent(); ++i) {
            auto have = signature(map(i));
            if (have.first.Distance(want.first) < tol
                && std::fabs(have.second - want.second) < tol) {
                if (found) {
                    // Two alike: not this way
                    return {};
                }
                found = i;
            }
        }
        return found ? ex.at(el.ShapeType(), found) : TopoDS_Shape();
    }

    /// The labels of the elements \a feature refers to, added to the part's
    /// shape as its sub-shapes; false with \a why if one is not in it
    bool labels(const TolerancedFeature* feature, Exported& ex,
                NCollection_Sequence<TDF_Label>& out, std::string& why)
    {
        out.Clear();
        if (!feature) {
            why = "it has no feature";
            return false;
        }
        // The whole part: its own label, no element of it
        if (feature->getKind() == TolerancedFeature::WholePart) {
            if (ex.label.IsNull()) {
                why = "its part is not exported";
                return false;
            }
            out.Append(ex.label);
            return true;
        }
        for (auto& [obj, subs] : feature->References.getSubListValues()) {
            for (auto& sub : subs) {
                Part::TopoShape el;
                try {
                    el = Part::Feature::getTopoShape(obj, sub.c_str(), true);
                }
                catch (const Base::Exception&) {
                }
                catch (const Standard_Failure&) {
                }
                if (el.isNull()) {
                    why = "its element " + sub + " is missing";
                    return false;
                }
                Exported* on = &ex;
                TopoDS_Shape mine = locate(ex, obj, sub, el.getShape());
                // Else on its own object, exported beside the part: the
                // part's supplemental geometry (NIST CTC-04's thread curves).
                // Not a Body: its set's coordinates are its own, and cached.
                if (mine.IsNull() && !obj->isDerivedFrom<Part::BodyBase>()) {
                    Exported* own = exported(obj, nullptr);
                    if (own && own != &ex) {
                        mine = locate(*own, obj, sub, el.getShape());
                        on = own;
                    }
                }
                if (mine.IsNull()) {
                    why = "its element " + sub + " is not in the shape exported";
                    return false;
                }
                TDF_Label l;
                if (!_shapes->FindSubShape(on->label, mine, l)) {
                    l = _shapes->AddSubShape(on->label, mine);
                }
                if (l.IsNull()) {
                    why = "its element " + sub + " could not be labelled";
                    return false;
                }
                out.Append(l);
            }
        }
        if (out.IsEmpty()) {
            why = "its feature refers to nothing";
            return false;
        }
        return true;
    }

    /// The callout's graphic, for STEP: in the part's coordinates, every
    /// curve a polyline and every fill a mesh. OCCT's writer takes a curve
    /// that is not a line for its B-spline poles, and ignores a sub-shape's
    /// location; a face is written from its triangulation.
    static TopoDS_Shape graphic(const Callout* c, const gp_Trsf& toLocal)
    {
        TopoDS_Shape in = c->Presentation.getShape().getShape();
        if (in.IsNull()) {
            return {};
        }
        TopoDS_Shape s = BRepBuilderAPI_Transform(in, toLocal, true).Shape();
        Bnd_Box box;
        BRepBndLib::Add(s, box);
        double size = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
        double deflection = std::max(1e-4, size * 1e-3);

        BRep_Builder b;
        TopoDS_Compound out;
        b.MakeCompound(out);
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> edges;
        TopExp::MapShapes(s, TopAbs_EDGE, edges);
        for (int i = 1; i <= edges.Extent(); ++i) {
            const TopoDS_Edge& e = TopoDS::Edge(edges(i));
            if (BRep_Tool::Degenerated(e)) {
                continue;
            }
            try {
                BRepAdaptor_Curve curve(e);
                std::vector<gp_Pnt> pts;
                if (curve.GetType() == GeomAbs_Line) {
                    pts = {curve.Value(curve.FirstParameter()),
                           curve.Value(curve.LastParameter())};
                }
                else {
                    GCPnts_TangentialDeflection d(curve, 0.1, deflection);
                    for (int k = 1; k <= d.NbPoints(); ++k) {
                        pts.push_back(d.Value(k));
                    }
                }
                for (std::size_t k = 1; k < pts.size(); ++k) {
                    if (pts[k - 1].Distance(pts[k]) > Precision::Confusion()) {
                        b.Add(out, BRepBuilderAPI_MakeEdge(pts[k - 1], pts[k]).Edge());
                    }
                }
            }
            catch (const Standard_Failure&) {
            }
        }
        BRepMesh_IncrementalMesh(s, deflection, false, 0.5, false);
        for (TopExp_Explorer xp(s, TopAbs_FACE); xp.More(); xp.Next()) {
            TopLoc_Location loc;
            auto tri = BRep_Tool::Triangulation(TopoDS::Face(xp.Current()), loc);
            if (tri.IsNull() || tri->NbTriangles() == 0) {
                continue;
            }
            // A face of its triangles alone: no edges to be written again
            TopoDS_Face f;
            b.MakeFace(f, tri);
            b.Add(out, f.Located(loc));
        }
        return out;
    }

    /// Its plane and text point, and its graphic
    template<typename T>
    static void present(const Callout* c, const Exported& ex, const Handle(T) & obj)
    {
        gp_Ax2 plane = ax2Of(ex.toLocal * trsfOf(c->Plane.getValue()));
        obj->SetPlane(plane);
        obj->SetPointTextAttach(plane.Location());
        TopoDS_Shape g = graphic(c, ex.toLocal);
        if (!g.IsNull()) {
            obj->SetPresentation(g, graphicName(c));
        }
        else if (!c->ImportedPlaceholder.getValues().empty()) {
            // Not drawn, another's graphic presenting it: its placeholder
            // still has the name the file gave it
            obj->SetPresentation(TopoDS_Shape(), graphicName(c));
        }

        // The file's graphic, written again: the kind the file named it
        if (!g.IsNull() && c->UseImported.getValue() && *c->ImportedGraphicType.getValue()) {
            obj->SetPresentationType(ascii(c->ImportedGraphicType.getValue()));
        }
    }

    void write(AnnotationSet* set, Exported& ex)
    {
        std::vector<Datum*> datums;
        std::vector<DatumTarget*> targets;
        std::vector<GeometricTolerance*> tolerances;
        std::vector<Dimension*> dims;
        std::vector<Note*> notes;
        std::vector<View*> views;
        for (auto o : set->Group.getValues()) {
            if (auto d = freecad_cast<Datum*>(o)) {
                datums.push_back(d);
            }
            else if (auto t = freecad_cast<DatumTarget*>(o)) {
                targets.push_back(t);
            }
            else if (auto gt = freecad_cast<GeometricTolerance*>(o)) {
                tolerances.push_back(gt);
            }
            else if (auto dim = freecad_cast<Dimension*>(o)) {
                dims.push_back(dim);
            }
            else if (auto n = freecad_cast<Note*>(o)) {
                notes.push_back(n);
            }
            else if (auto v = freecad_cast<View*>(o)) {
                views.push_back(v);
            }
        }
        // The datums first: OCCT's writer writes the first label of a letter
        // with its feature, and takes the others, a frame's, as references
        for (auto d : datums) {
            guard(d, [&]() { datum(d, ex); });
        }
        for (auto t : targets) {
            guard(t, [&]() { target(t, ex); });
        }
        for (auto gt : tolerances) {
            guard(gt, [&]() { tolerance(gt, ex); });
        }
        // Composite frames: each lower segment related to its upper, both
        // written (AP242's geometric_tolerance_relationship 'composite')
        for (auto gt : tolerances) {
            auto upper = gt->compositeUpper();
            auto lo = _labels.find(gt);
            auto hi = upper ? _labels.find(upper) : _labels.end();
            if (lo != _labels.end() && hi != _labels.end()) {
                _tool->SetCompositeUpper(lo->second, hi->second);
            }
        }
        for (auto dim : dims) {
            guard(dim, [&]() { dimension(dim, ex); });
        }
        for (auto n : notes) {
            guard(n, [&]() { note(n, ex); });
        }
        // Its placeholder, as the file had it: written again
        for (auto o : set->Group.getValues()) {
            auto c = freecad_cast<Callout*>(o);
            auto mine = c ? _labels.find(c) : _labels.end();
            if (mine == _labels.end()) {
                continue;
            }
            const auto& holder = c->ImportedPlaceholder.getValues();
            if (!holder.empty() && !holder.front().empty()) {
                XCAFDoc_DimTolTool::SetPlaceholder(
                    mine->second, holder.front().c_str(),
                    holder.size() > 1 ? ascii(holder[1])->String() : TCollection_AsciiString(),
                    holder.size() > 2 && holder[2] == "shared");
            }
        }
        // Drawn in another's graphic: linked to it as what it presents too
        // (NIST's 32 radii to one '32X R', a hole's depth to its callout)
        for (auto o : set->Group.getValues()) {
            auto c = freecad_cast<Callout*>(o);
            if (!c || !c->drawnByAnother()) {
                continue;
            }
            // A composite frame's lower segment is linked to no graphic, as
            // NIST's is not: being composite says whose it is drawn in
            auto gt = freecad_cast<GeometricTolerance*>(c);
            if (gt && gt->compositeUpper() == c->ImportedWith.getValue()) {
                continue;
            }
            auto mine = _labels.find(c);
            auto host = _labels.find(c->ImportedWith.getValue());
            if (mine != _labels.end() && host != _labels.end()) {
                _tool->SetPresentedWith(mine->second, host->second);
            }
        }
        // Last, once every set is written: a view lists what is written of
        // what it shows, which may be in another set
        for (auto v : views) {
            _views.emplace_back(v, &ex);
        }
    }

    template<typename F>
    void guard(const App::DocumentObject* obj, F f)
    {
        try {
            f();
        }
        catch (const Base::Exception& e) {
            skip(obj, e.what());
        }
        catch (const Standard_Failure& e) {
            skip(obj, e.GetMessageString());
        }
    }

    void skip(const App::DocumentObject* obj, const std::string& why)
    {
        _report.skipped.push_back(std::string(obj->Label.getValue()) + ": " + why);
    }

    void partly(const App::DocumentObject* obj, const std::string& what)
    {
        _report.partly.push_back(std::string(obj->Label.getValue()) + ": " + what);
    }

    void datum(Datum* d, Exported& ex)
    {
        auto feature = freecad_cast<TolerancedFeature*>(d->Feature.getValue());
        if (!feature && !d->Targets.getValues().empty()) {
            // Established by its targets alone: they carry its letter
            return;
        }
        NCollection_Sequence<TDF_Label> shapes;
        std::string why;
        if (!labels(feature, ex, shapes, why)) {
            return skip(d, why);
        }
        TDF_Label l = _tool->AddDatum();
        _labels[d] = l;
        Handle(XCAFDoc_Datum) attr;
        l.FindAttribute(XCAFDoc_Datum::GetID(), attr);
        Handle(XCAFDimTolObjects_DatumObject) obj = new XCAFDimTolObjects_DatumObject();
        obj->SetName(ascii(d->Letter.getValue()));
        present(d, ex, obj);
        attr->SetObject(obj);
        _tool->SetDatum(shapes, l);
        ++_report.datums;
    }

    void target(DatumTarget* t, Exported& ex)
    {
        XCAFDimTolObjects_DatumTargetType type;
        switch (t->TargetType.getValue()) {
            case DatumTarget::Point: type = XCAFDimTolObjects_DatumTargetType_Point; break;
            case DatumTarget::Line: type = XCAFDimTolObjects_DatumTargetType_Line; break;
            case DatumTarget::Rectangle: type = XCAFDimTolObjects_DatumTargetType_Rectangle; break;
            case DatumTarget::Circle: type = XCAFDimTolObjects_DatumTargetType_Circle; break;
            case DatumTarget::CircularCurve:
                type = XCAFDimTolObjects_DatumTargetType_CircularCurve;
                break;
            default: type = XCAFDimTolObjects_DatumTargetType_Area; break;
        }
        // Its letter: the datum it is of, else its label's ("A1")
        std::string letter;
        for (auto o : t->getInList()) {
            if (auto d = freecad_cast<Datum*>(o)) {
                auto ts = d->Targets.getValues();
                if (std::find(ts.begin(), ts.end(), t) != ts.end()) {
                    letter = d->Letter.getValue();
                    break;
                }
            }
        }
        if (letter.empty()) {
            std::string label = t->Label.getValue();
            for (char c : label) {
                if (c < 'A' || c > 'Z') {
                    break;
                }
                letter += c;
            }
        }
        if (letter.empty()) {
            return skip(t, "it is of no datum");
        }
        NCollection_Sequence<TDF_Label> shapes;
        std::string why;
        auto feature = freecad_cast<TolerancedFeature*>(t->Feature.getValue());
        if (type == XCAFDimTolObjects_DatumTargetType_Area || feature) {
            if (!labels(feature, ex, shapes, why)) {
                return skip(t, why);
            }
        }
        TDF_Label l = _tool->AddDatum();
        _labels[t] = l;
        Handle(XCAFDoc_Datum) attr;
        l.FindAttribute(XCAFDoc_Datum::GetID(), attr);
        Handle(XCAFDimTolObjects_DatumObject) obj = new XCAFDimTolObjects_DatumObject();
        obj->SetName(ascii(letter));
        obj->IsDatumTarget(true);
        obj->SetDatumTargetType(type);
        obj->SetDatumTargetNumber(int(t->Number.getValue()));
        if (type != XCAFDimTolObjects_DatumTargetType_Area) {
            obj->SetDatumTargetAxis(ax2Of(ex.toLocal * trsfOf(t->Target.getValue())));
            obj->SetDatumTargetLength(t->Length.getValue());
            obj->SetDatumTargetWidth(t->Width.getValue());
        }
        else if (!shapes.IsEmpty()) {
            // The area itself, its face as written: without it OCCT writes
            // the target as a datum alone, and it is lost (NIST FTC-10's)
            obj->SetDatumTarget(_shapes->GetShape(shapes.First()));
        }
        present(t, ex, obj);
        attr->SetObject(obj);
        if (!shapes.IsEmpty()) {
            _tool->SetDatum(shapes, l);
        }
        ++_report.targets;
    }

    static bool characteristic(int c, XCAFDimTolObjects_GeomToleranceType& out)
    {
        using GT = GeometricTolerance;
        switch (c) {
            case GT::Straightness: out = XCAFDimTolObjects_GeomToleranceType_Straightness; break;
            case GT::Flatness: out = XCAFDimTolObjects_GeomToleranceType_Flatness; break;
            case GT::Circularity:
                out = XCAFDimTolObjects_GeomToleranceType_CircularityOrRoundness;
                break;
            case GT::Cylindricity: out = XCAFDimTolObjects_GeomToleranceType_Cylindricity; break;
            case GT::ProfileOfLine: out = XCAFDimTolObjects_GeomToleranceType_ProfileOfLine; break;
            case GT::ProfileOfSurface:
                out = XCAFDimTolObjects_GeomToleranceType_ProfileOfSurface;
                break;
            case GT::Angularity: out = XCAFDimTolObjects_GeomToleranceType_Angularity; break;
            case GT::Perpendicularity:
                out = XCAFDimTolObjects_GeomToleranceType_Perpendicularity;
                break;
            case GT::Parallelism: out = XCAFDimTolObjects_GeomToleranceType_Parallelism; break;
            case GT::Position: out = XCAFDimTolObjects_GeomToleranceType_Position; break;
            case GT::Concentricity: out = XCAFDimTolObjects_GeomToleranceType_Concentricity; break;
            case GT::Coaxiality: out = XCAFDimTolObjects_GeomToleranceType_Coaxiality; break;
            case GT::Symmetry: out = XCAFDimTolObjects_GeomToleranceType_Symmetry; break;
            case GT::CircularRunout:
                out = XCAFDimTolObjects_GeomToleranceType_CircularRunout;
                break;
            case GT::TotalRunout: out = XCAFDimTolObjects_GeomToleranceType_TotalRunout; break;
            default: return false;
        }
        return true;
    }

    static bool modifier(const std::string& m, XCAFDimTolObjects_GeomToleranceModif& out)
    {
        static const std::map<std::string, XCAFDimTolObjects_GeomToleranceModif> map = {
            {"ACS", XCAFDimTolObjects_GeomToleranceModif_Any_Cross_Section},
            {"CZ", XCAFDimTolObjects_GeomToleranceModif_Common_Zone},
            {"F", XCAFDimTolObjects_GeomToleranceModif_Free_State},
            {"LE", XCAFDimTolObjects_GeomToleranceModif_Line_Element},
            {"MD", XCAFDimTolObjects_GeomToleranceModif_Major_Diameter},
            {"LD", XCAFDimTolObjects_GeomToleranceModif_Minor_Diameter},
            {"NC", XCAFDimTolObjects_GeomToleranceModif_Not_Convex},
            {"PD", XCAFDimTolObjects_GeomToleranceModif_Pitch_Diameter},
            {"SZ", XCAFDimTolObjects_GeomToleranceModif_Separate_Requirement},
            {"ST", XCAFDimTolObjects_GeomToleranceModif_Statistical_Tolerance},
            {"T", XCAFDimTolObjects_GeomToleranceModif_Tangent_Plane},
            {"AllAround", XCAFDimTolObjects_GeomToleranceModif_All_Around},
            {"AllOver", XCAFDimTolObjects_GeomToleranceModif_All_Over},
        };
        auto it = map.find(m);
        if (it == map.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    static bool modifier(const std::string& m, XCAFDimTolObjects_DatumSingleModif& out)
    {
        static const std::map<std::string, XCAFDimTolObjects_DatumSingleModif> map = {
            {"M", XCAFDimTolObjects_DatumSingleModif_MaximumMaterialRequirement},
            {"L", XCAFDimTolObjects_DatumSingleModif_LeastMaterialRequirement},
            {"F", XCAFDimTolObjects_DatumSingleModif_FreeState},
            {"BSC", XCAFDimTolObjects_DatumSingleModif_Basic},
            {"><", XCAFDimTolObjects_DatumSingleModif_Translation},
            {"PT", XCAFDimTolObjects_DatumSingleModif_Point},
            {"SL", XCAFDimTolObjects_DatumSingleModif_Line},
            {"PL", XCAFDimTolObjects_DatumSingleModif_Plane},
            {"ACS", XCAFDimTolObjects_DatumSingleModif_AnyCrossSection},
            {"ALS", XCAFDimTolObjects_DatumSingleModif_Any_LongitudinalSection},
            {"CF", XCAFDimTolObjects_DatumSingleModif_ContactingFeature},
            {"DV", XCAFDimTolObjects_DatumSingleModif_DistanceVariable},
            {"MD", XCAFDimTolObjects_DatumSingleModif_MajorDiameter},
            {"LD", XCAFDimTolObjects_DatumSingleModif_MinorDiameter},
            {"PD", XCAFDimTolObjects_DatumSingleModif_PitchDiameter},
        };
        auto it = map.find(m);
        if (it == map.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    void tolerance(GeometricTolerance* gt, Exported& ex)
    {
        XCAFDimTolObjects_GeomToleranceType type;
        if (!characteristic(gt->Characteristic.getValue(), type)) {
            return skip(gt, "it has no characteristic");
        }
        auto feature = freecad_cast<TolerancedFeature*>(gt->Feature.getValue());
        NCollection_Sequence<TDF_Label> shapes;
        std::string why;
        if (!labels(feature, ex, shapes, why)) {
            return skip(gt, why);
        }
        // The frame's compartments, each a datum or a common datum
        std::vector<DatumReferenceFrame::Compartment> comps;
        if (auto frame = freecad_cast<DatumReferenceFrame*>(gt->DatumSystem.getValue())) {
            if (!frame->parse(comps, why)) {
                return skip(gt, "its datum system: " + why);
            }
        }

        TDF_Label l = _tool->AddGeomTolerance();
        _labels[gt] = l;
        Handle(XCAFDoc_GeomTolerance) attr;
        l.FindAttribute(XCAFDoc_GeomTolerance::GetID(), attr);
        Handle(XCAFDimTolObjects_GeomToleranceObject) obj =
            new XCAFDimTolObjects_GeomToleranceObject();
        obj->SetType(type);
        switch (gt->ZoneShape.getValue()) {
            case GeometricTolerance::DiameterZone:
                obj->SetTypeOfValue(XCAFDimTolObjects_GeomToleranceTypeValue_Diameter);
                break;
            case GeometricTolerance::SphericalDiameterZone:
                obj->SetTypeOfValue(XCAFDimTolObjects_GeomToleranceTypeValue_SphericalDiameter);
                break;
            default: break;
        }
        obj->SetValue(gt->Value.getValue());
        switch (gt->MaterialCondition.getValue()) {
            case GeometricTolerance::Maximum:
                obj->SetMaterialRequirementModifier(XCAFDimTolObjects_GeomToleranceMatReqModif_M);
                break;
            case GeometricTolerance::Least:
                obj->SetMaterialRequirementModifier(XCAFDimTolObjects_GeomToleranceMatReqModif_L);
                break;
            default: break;
        }
        for (auto& m : gt->Modifiers.getValues()) {
            XCAFDimTolObjects_GeomToleranceModif mod;
            if (m == "P") {
                obj->SetZoneModifier(XCAFDimTolObjects_GeomToleranceZoneModif_Projected);
                obj->SetValueOfZoneModifier(gt->ProjectedLength.getValue());
            }
            else if (m == "U") {
                obj->SetZoneModifier(XCAFDimTolObjects_GeomToleranceZoneModif_NonUniform);
                obj->SetValueOfZoneModifier(gt->UnequalValue.getValue());
                partly(gt, "its unequally disposed zone (U): OCCT writes none");
            }
            else if (modifier(m, mod)) {
                obj->AddModifier(mod);
            }
            else {
                partly(gt, "modifier " + m + " has no place in STEP's tolerance");
            }
        }
        if (gt->MaxValue.getValue() > 0) {
            obj->SetMaxValueModifier(gt->MaxValue.getValue());
        }
        if (gt->UnitBasis.getValue() != GeometricTolerance::WholeFeature) {
            static const XCAFDimTolObjects_GeomToleranceUnitBasis bases[] = {
                XCAFDimTolObjects_GeomToleranceUnitBasis_None,
                XCAFDimTolObjects_GeomToleranceUnitBasis_Length,
                XCAFDimTolObjects_GeomToleranceUnitBasis_Square,
                XCAFDimTolObjects_GeomToleranceUnitBasis_Rectangle,
                XCAFDimTolObjects_GeomToleranceUnitBasis_Circle};
            obj->SetUnitBasis(bases[gt->UnitBasis.getValue()], gt->UnitSize.getValue(),
                              gt->UnitBasis.getValue() == GeometricTolerance::PerRectangle
                                  ? gt->UnitSize2.getValue()
                                  : 0.0);
            if (!obj->GetModifiers().IsEmpty() || gt->getDatumSystem()
                || gt->MaterialCondition.getValue() != GeometricTolerance::NoMaterial
                || gt->MaxValue.getValue() > 0) {
                partly(gt, "a tolerance per unit with modifiers or datums: its unit is not "
                           "written, STEP has no entity for both");
            }
        }
        // On the whole part it is all over, said or not: what the writer
        // writes it on the product's shape by
        if (feature->getKind() == TolerancedFeature::WholePart) {
            bool over = false;
            for (auto m : obj->GetModifiers()) {
                over = over || m == XCAFDimTolObjects_GeomToleranceModif_All_Over;
            }
            if (!over) {
                obj->AddModifier(XCAFDimTolObjects_GeomToleranceModif_All_Over);
            }
        }
        present(gt, ex, obj);
        attr->SetObject(obj);
        _tool->SetGeomTolerance(shapes, l);

        // Each compartment's datums, at its position: a frame's own labels,
        // as OCCT reads them
        for (std::size_t i = 0; i < comps.size(); ++i) {
            for (auto& letter : comps[i].datums) {
                TDF_Label dl = _tool->AddDatum();
                Handle(XCAFDoc_Datum) dattr;
                dl.FindAttribute(XCAFDoc_Datum::GetID(), dattr);
                Handle(XCAFDimTolObjects_DatumObject) dobj = new XCAFDimTolObjects_DatumObject();
                dobj->SetName(ascii(letter));
                dobj->SetPosition(int(i) + 1);
                for (auto& m : comps[i].modifiers) {
                    XCAFDimTolObjects_DatumSingleModif mod;
                    if (modifier(m, mod)) {
                        dobj->AddModifier(mod);
                    }
                    else {
                        partly(gt, "datum modifier " + m + " has no place in STEP's");
                    }
                }
                dattr->SetObject(dobj);
                _tool->SetDatumToGeomTol(dl, l);
            }
        }
        ++_report.tolerances;
    }

    static bool dimensionType(int t, XCAFDimTolObjects_DimensionType& out)
    {
        switch (t) {
            case Dimension::Diameter: out = XCAFDimTolObjects_DimensionType_Size_Diameter; break;
            case Dimension::Radius: out = XCAFDimTolObjects_DimensionType_Size_Radius; break;
            case Dimension::SphericalDiameter:
                out = XCAFDimTolObjects_DimensionType_Size_SphericalDiameter;
                break;
            case Dimension::SphericalRadius:
                out = XCAFDimTolObjects_DimensionType_Size_SphericalRadius;
                break;
            // A depth is a thickness, as NIST's files write a hole's (STC-06, STC-10)
            case Dimension::Width:
            case Dimension::Depth: out = XCAFDimTolObjects_DimensionType_Size_Thickness; break;
            case Dimension::AngularSize: out = XCAFDimTolObjects_DimensionType_Size_Angular; break;
            case Dimension::Distance:
                out = XCAFDimTolObjects_DimensionType_Location_LinearDistance;
                break;
            case Dimension::Angle: out = XCAFDimTolObjects_DimensionType_Location_Angular; break;
            case Dimension::CurveLength:
                out = XCAFDimTolObjects_DimensionType_Size_CurveLength;
                break;
            default: return false;
        }
        return true;
    }

    /// A fit (ISO 286, "H7", "g6"): its class of tolerance -- a hole's in
    /// upper case, a shaft's in lower
    static bool fit(const std::string& f, const Handle(XCAFDimTolObjects_DimensionObject)& obj)
    {
        std::size_t cut = f.find_first_of("0123456789");
        if (cut == 0 || cut == std::string::npos) {
            return false;
        }
        std::string variance = f.substr(0, cut);
        std::string grade = f.substr(cut);
        bool hole = Step::isAsciiUpper(variance[0]);
        for (auto& c : variance) {
            c = Step::asciiUpper(c);
        }
        int v = 0, g = -1;
        for (int i = 1; i < 29; ++i) {
            if (variance == Step::FitVariances[i]) {
                v = i;
            }
        }
        for (int i = 0; i < 20; ++i) {
            if (grade == Step::FitGrades[i]) {
                g = i;
            }
        }
        if (!v || g < 0) {
            return false;
        }
        obj->SetClassOfTolerance(hole, XCAFDimTolObjects_DimensionFormVariance(v),
                                 XCAFDimTolObjects_DimensionGrade(g));
        return true;
    }

    static bool modifier(const std::string& m, XCAFDimTolObjects_DimensionModif& out)
    {
        static const std::map<std::string, XCAFDimTolObjects_DimensionModif> map = {
            {"LP", XCAFDimTolObjects_DimensionModif_TwoPointSize},
            {"LS", XCAFDimTolObjects_DimensionModif_LocalSizeDefinedBySphere},
            {"GG", XCAFDimTolObjects_DimensionModif_LeastSquaresAssociationCriterion},
            {"GX", XCAFDimTolObjects_DimensionModif_MaximumInscribedAssociation},
            {"GN", XCAFDimTolObjects_DimensionModif_MinimumCircumscribedAssociation},
            {"CC", XCAFDimTolObjects_DimensionModif_CircumferenceDiameter},
            {"CA", XCAFDimTolObjects_DimensionModif_AreaDiameter},
            {"CV", XCAFDimTolObjects_DimensionModif_VolumeDiameter},
            {"SX", XCAFDimTolObjects_DimensionModif_MaximumSize},
            {"SN", XCAFDimTolObjects_DimensionModif_MinimumSize},
            {"SA", XCAFDimTolObjects_DimensionModif_AverageSize},
            {"SM", XCAFDimTolObjects_DimensionModif_MedianSize},
            {"SD", XCAFDimTolObjects_DimensionModif_MidRangeSize},
            {"SR", XCAFDimTolObjects_DimensionModif_RangeOfSizes},
            {"SQ", XCAFDimTolObjects_DimensionModif_Square},
            {"ACS", XCAFDimTolObjects_DimensionModif_AnyCrossSection},
            {"SCS", XCAFDimTolObjects_DimensionModif_SpecificFixedCrossSection},
            {"CT", XCAFDimTolObjects_DimensionModif_CommonTolerance},
            {"F", XCAFDimTolObjects_DimensionModif_FreeStateCondition},
            {"Between", XCAFDimTolObjects_DimensionModif_Between},
        };
        auto it = map.find(m);
        if (it == map.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    void dimension(Dimension* dim, Exported& ex)
    {
        XCAFDimTolObjects_DimensionType type;
        if (!dimensionType(dim->Type.getValue(), type)) {
            return skip(dim, "its type is not written");
        }
        auto feats = dim->Features.getValues();
        bool location = !dim->isSize();
        if (feats.empty() || (location && feats.size() < 2)) {
            return skip(dim, "it has not the features it measures");
        }
        NCollection_Sequence<TDF_Label> first, second;
        std::string why;
        if (!labels(freecad_cast<TolerancedFeature*>(feats[0]), ex, first, why)) {
            return skip(dim, why);
        }
        if (location
            && !labels(freecad_cast<TolerancedFeature*>(feats[1]), ex, second, why)) {
            return skip(dim, "its second feature: " + why);
        }
        TDF_Label l = _tool->AddDimension();
        _labels[dim] = l;
        Handle(XCAFDoc_Dimension) attr;
        l.FindAttribute(XCAFDoc_Dimension::GetID(), attr);
        Handle(XCAFDimTolObjects_DimensionObject) obj = new XCAFDimTolObjects_DimensionObject();
        obj->SetType(type);
        double value = dim->Nominal.getValue();
        obj->SetValue(value);
        double upper = dim->Upper.getValue();
        double lower = dim->Lower.getValue();
        if (dim->Basic.getValue()) {
            // Theoretically exact, its tolerance a frame's: a 'dimensional
            // note' that is 'theoretical', as AP242 has a basic dimension
            // (NIST CTC-04's 75 and 10, SFA's [75])
            obj->AddDescription(new TCollection_HAsciiString("theoretical"),
                                new TCollection_HAsciiString("dimensional note"));
        }
        else if (dim->Reference.getValue()) {
            // For information, no tolerance: a 'dimensional note' that is
            // 'auxiliary', as AP242 has a reference dimension
            obj->AddDescription(new TCollection_HAsciiString("auxiliary"),
                                new TCollection_HAsciiString("dimensional note"));
        }
        else if (dim->Limits.getValue() && (upper != 0.0 || lower != 0.0)) {
            // Its limits, a range: lower and upper limit, as it was given
            obj->SetLowerBound(value + lower);
            obj->SetUpperBound(value + upper);
        }
        else if (upper != 0.0 || lower != 0.0) {
            obj->SetUpperTolValue(upper);
            // A magnitude, as OCCT keeps it: its writer negates it
            obj->SetLowerTolValue(-lower);
        }
        if (dim->DecimalPlaces.getValue() > 0) {
            int left = std::max(1, int(std::floor(std::log10(std::max(1.0, std::fabs(value)))))
                                       + 1);
            obj->SetNbOfDecimalPlaces(left, int(dim->DecimalPlaces.getValue()));
        }
        for (auto& m : dim->Modifiers.getValues()) {
            XCAFDimTolObjects_DimensionModif mod;
            if (modifier(m, mod)) {
                obj->AddModifier(mod);
            }
            else {
                partly(dim, "modifier " + m + " has no place in STEP's dimension");
            }
        }
        if (strlen(dim->Fit.getValue()) > 0 && !fit(dim->Fit.getValue(), obj)) {
            partly(dim, std::string("its fit ") + dim->Fit.getValue() + " is no ISO 286 fit");
        }
        present(dim, ex, obj);
        attr->SetObject(obj);
        if (location) {
            _tool->SetDimension(first, second, l);
        }
        else {
            NCollection_Sequence<TDF_Label> none;
            _tool->SetDimension(first, none, l);
        }
        ++_report.dimensions;
    }

    /// A note: its graphic, named by its text (OCCT has no note of its own
    /// in GD&T), on its feature if it has one
    void note(Note* n, Exported& ex)
    {
        NCollection_Sequence<TDF_Label> shapes;
        std::string why;
        auto feature = freecad_cast<TolerancedFeature*>(n->Feature.getValue());
        if (feature && !labels(feature, ex, shapes, why)) {
            return skip(n, why);
        }
        TDF_Label l = _tool->AddDimension();
        _labels[n] = l;
        Handle(XCAFDoc_Dimension) attr;
        l.FindAttribute(XCAFDoc_Dimension::GetID(), attr);
        Handle(XCAFDimTolObjects_DimensionObject) obj = new XCAFDimTolObjects_DimensionObject();
        obj->SetType(shapes.IsEmpty() ? XCAFDimTolObjects_DimensionType_CommonLabel
                                      : XCAFDimTolObjects_DimensionType_DimensionPresentation);
        present(n, ex, obj);
        // Its words, written as a semantic text (AP242's recommended
        // practice): one text, its lines as they are, signs and all, as
        // NIST's files give a note -- a line each left an empty line an
        // empty text, which SFA reports (STC-07's general note)
        std::string words;
        const auto& lines = n->Text.getValues();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            words += (i ? "\n" : "") + lines[i];
        }
        if (!lines.empty()) {
            obj->AddDescription(new TCollection_HAsciiString(words.c_str()),
                                nameOf(n));
        }
        // Its graphic named as the file named it, while drawn as the file has
        // it; else by its words, read back as its label; by the label when
        // they have no letter or digit of ASCII (a counterbore sign)
        std::string text;
        for (auto& line : n->Text.getValues()) {
            text += (text.empty() ? "" : " ") + line;
        }
        bool plain = false;
        for (char c : text) {
            plain = plain || Step::isAsciiAlnum(c);
        }
        TopoDS_Shape g = obj->GetPresentation();
        if (g.IsNull()) {
            attr->SetObject(obj);
            return skip(n, "it has no graphic");
        }
        bool asFile = n->UseImported.getValue() && *n->ImportedGraphicName.getValue();
        obj->SetPresentation(g, asFile || !plain ? graphicName(n) : ascii(text));
        attr->SetObject(obj);
        if (!shapes.IsEmpty()) {
            NCollection_Sequence<TDF_Label> none;
            _tool->SetDimension(shapes, none, l);
        }
        ++_report.notes;
    }

private:
    const Import::PostExport& _info;
    Handle(XCAFDoc_ShapeTool) _shapes;
    Handle(XCAFDoc_DimTolTool) _tool;
    /// A saved view (sec 5.2, P5): its camera, the callouts it shows and the
    /// shapes. The camera is the document's; a STEP camera is an eye, a line
    /// of sight and a window, so the eye is put out of the part, on the line
    /// through the point looked at, and a view fitting all gets the window
    /// the part's extent fills.
    void view(View* v, Exported& ex)
    {
        gp_Trsf cam = ex.placedToLabel * trsfOf(v->Camera.getValue());
        gp_Ax2 frame = ax2Of(cam);
        gp_Pnt at = frame.Location();
        gp_Dir z = frame.Direction();
        gp_Dir y = frame.YDirection();
        gp_Dir x = frame.XDirection();
        Bnd_Box box;
        BRepBndLib::Add(ex.shape, box);
        double nearest = 0, half = 0, diag = 1;
        if (!box.IsVoid()) {
            diag = std::sqrt(box.SquareExtent());
            double x0, y0, z0, x1, y1, z1;
            box.Get(x0, y0, z0, x1, y1, z1);
            for (int i = 0; i < 8; ++i) {
                gp_Vec d(at, gp_Pnt(i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0));
                nearest = std::max(nearest, d.Dot(gp_Vec(z)));
                half = std::max({half, std::abs(d.Dot(gp_Vec(x))), std::abs(d.Dot(gp_Vec(y)))});
            }
        }
        double distance = nearest + diag;
        double height = v->Height.getValue() > Precision::Confusion() ? v->Height.getValue()
                                                                      : 2 * half * 1.05;
        Handle(XCAFView_Object) obj = new XCAFView_Object();
        obj->SetName(ascii(v->Label.getValue()));
        obj->SetType(v->Perspective.getValue() ? XCAFView_ProjectionType_Central
                                               : XCAFView_ProjectionType_Parallel);
        obj->SetProjectionPoint(at.Translated(gp_Vec(z) * distance));
        obj->SetViewDirection(z.Reversed());
        obj->SetUpDirection(y);
        obj->SetZoomFactor(distance);
        obj->SetWindowHorizontalSize(height);
        obj->SetWindowVerticalSize(height);
        // Its clipping plane, the document's as the camera is: z toward
        // the viewer, what lies in front cut away -- as a STEP clipping
        // plane's normal says (NIST FTC-11's section view)
        NCollection_Sequence<TDF_Label> planes;
        if (v->Clip.getValue()) {
            gp_Ax2 cut = ax2Of(ex.placedToLabel * trsfOf(v->ClipPlane.getValue()));
            Handle(XCAFDoc_ClippingPlaneTool) clipping =
                XCAFDoc_DocumentTool::ClippingPlaneTool(_info.hDoc->Main());
            planes.Append(clipping->AddClippingPlane(gp_Pln(gp_Ax3(cut)),
                                                     TCollection_ExtendedString()));
        }
        // The shapes it shows: its objects that were exported, else the part
        NCollection_Sequence<TDF_Label> shapes;
        for (auto o : v->Objects.getValues()) {
            TDF_Label l = _info.labelOf(o);
            if (!l.IsNull()) {
                shapes.Append(l);
            }
        }
        if (shapes.IsEmpty()) {
            shapes.Append(ex.label);
        }
        NCollection_Sequence<TDF_Label> gdts;
        for (auto m : v->members()) {
            auto it = _labels.find(m);
            if (it != _labels.end()) {
                gdts.Append(it->second);
            }
        }
        Handle(XCAFDoc_ViewTool) tool = XCAFDoc_DocumentTool::ViewTool(_info.hDoc->Main());
        TDF_Label l = tool->AddView();
        XCAFDoc_View::Set(l)->SetObject(obj);
        tool->SetView(shapes, gdts, planes, l);
        ++_report.views;
    }

    std::map<App::DocumentObject*, Exported> _parts;
    /// The views of the sets written, each with its set's part
    std::vector<std::pair<View*, Exported*>> _views;
    /// The label each callout was written as: what a view lists
    std::map<const App::DocumentObject*, TDF_Label> _labels;
    Step::WriteReport _report;
};

}  // namespace

Step::WriteReport Step::write(const Import::PostExport& info)
{
    Writer writer(info);
    WriteReport report = writer.run();
    int any = report.datums + report.targets + report.tolerances + report.dimensions
        + report.notes + report.views;
    if (any || !report.skipped.empty()) {
        // The user's, not a log's: no source location in front
        Base::Console().message("PMI written: %d datums, %d datum targets, %d tolerances, "
                                "%d dimensions, %d notes, %d views\n",
                                report.datums, report.targets, report.tolerances,
                                report.dimensions, report.notes, report.views);
    }
    for (auto& s : report.skipped) {
        Base::Console().warning("PMI not written: %s\n", s.c_str());
    }
    for (auto& s : report.partly) {
        Base::Console().warning("PMI: %s\n", s.c_str());
    }
    return report;
}

const char* const Step::FitVariances[29] = {
    "",  "A", "B", "C",  "CD", "D", "E", "EF", "F", "FG", "G",  "H",  "JS", "J", "K",
    "M", "N", "P", "R", "S", "T", "U", "V",  "X", "Y",  "Z", "ZA", "ZB", "ZC"};
const char* const Step::FitGrades[20] = {"01", "0",  "1",  "2",  "3",  "4",  "5",
                                         "6",  "7",  "8",  "9",  "10", "11", "12",
                                         "13", "14", "15", "16", "17", "18"};

void Step::registerExport()
{
    Import::ExportOCAF2::addPostExport("PMI", [](const Import::PostExport& info) {
        Step::write(info);
    });
}
