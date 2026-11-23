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
#include <map>
#include <set>
#include <sstream>

#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp_Face.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Bnd_Box.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <NCollection_Sequence.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <XCAFDoc_ClippingPlaneTool.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_View.hxx>
#include <XCAFDoc_ViewTool.hxx>
#include <XCAFView_Object.hxx>

#include <App/Document.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Base/Console.h>
#include <Base/Placement.h>
#include <Base/Tools.h>
#include <Mod/Import/App/ImportOCAF2.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>

#include "AnnotationSet.h"
#include "Api.h"
#include "Datum.h"
#include "Dimension.h"
#include "GeometricTolerance.h"
#include "Note.h"
#include "Geometry.h"
#include "StepExport.h"
#include "StepImport.h"
#include "TolerancedFeature.h"
#include "View.h"

FC_LOG_LEVEL_INIT("PMI", true, true)

using namespace PMI;

namespace
{

std::string text(const Handle(TCollection_HAsciiString) & s)
{
    return s.IsNull() ? std::string() : std::string(s->ToCString());
}

std::string entry(const TDF_Label& label)
{
    TCollection_AsciiString e;
    TDF_Tool::Entry(label, e);
    return e.ToCString();
}

/// Faces of one object, as an annotation refers to them
struct Faces
{
    Part::Feature* object = nullptr;
    std::vector<std::string> names;
    /// The first element's surface or curve: what decides an axis
    Primitive::Type type = Primitive::None;
    /// And its normal or axis
    gp_Dir dir;
};

class Reader
{
public:
    explicit Reader(const Import::PostImport& info)
        : _info(info)
    {
        TDF_Label main = info.hDoc->Main();
        _shapes = XCAFDoc_DocumentTool::ShapeTool(main);
        _tool = XCAFDoc_DocumentTool::DimTolTool(main);
    }

    Step::Report run()
    {
        NCollection_Sequence<TDF_Label> labels;
        _tool->GetDatumLabels(labels);
        for (auto& l : labels) {
            guard(l, "datum", [&]() { datum(l); });
        }
        targetDatums();
        // OCCT makes a datum label for each time a frame cites it; only the
        // ones whose feature it found carry shapes. A letter none carries
        // is a datum lost.
        for (auto& letter : _cited) {
            if (!_read.count(letter) && !_targets.count(letter)) {
                _report.skipped.push_back("datum " + letter
                                          + ": no shape for its feature, only frames cite it");
            }
        }
        labels.Clear();
        _tool->GetGeomToleranceLabels(labels);
        for (auto& l : labels) {
            guard(l, "tolerance", [&]() { tolerance(l); });
        }
        labels.Clear();
        _tool->GetDimensionLabels(labels);
        for (auto& l : labels) {
            guard(l, "dimension", [&]() { dimension(l); });
        }
        notes();
        views();
        return _report;
    }

private:
    template<typename F>
    void guard(const TDF_Label& l, const char* what, F f)
    {
        try {
            f();
        }
        catch (const Standard_Failure& e) {
            skip(l, what, e.what());
        }
        catch (const Base::Exception& e) {
            skip(l, what, e.what());
        }
    }

    void skip(const TDF_Label& l, const std::string& what, const std::string& why)
    {
        _report.skipped.push_back(what + " " + entry(l) + ": " + why);
    }

    /// The faces of these shape labels, all on one object; false with why
    bool faces(const NCollection_Sequence<TDF_Label>& labels, Faces& out, std::string& why)
    {
        out = Faces();
        if (labels.IsEmpty()) {
            why = "it refers to no shape";
            return false;
        }
        // A file with the geometry also tessellated names each face twice:
        // on the B-rep, and on the tessellated copy. The B-rep is the one.
        std::vector<std::pair<TDF_Label, Part::Feature*>> refs;
        bool exact = false;
        bool solid = false;
        for (auto& l : labels) {
            TDF_Label owner = XCAFDoc_ShapeTool::IsSubShape(l) ? l.Father() : l;
            auto feat = freecad_cast<Part::Feature*>(_info.objectOf(owner));
            if (!feat) {
                why = "no object was made for shape " + entry(owner);
                return false;
            }
            refs.emplace_back(l, feat);
            exact = exact || !tessellated(_shapes->GetShape(l));
            solid = solid || hasSolid(feat);
        }
        // Faces on a solid and on surfaces beside it (supplemental geometry,
        // NIST FTC-10): the solid is the part
        bool several = std::any_of(refs.begin(), refs.end(), [&](const auto& r) {
            return r.second != refs.front().second;
        });
        for (auto& [l, feat] : refs) {
            if (exact && tessellated(_shapes->GetShape(l))) {
                continue;
            }
            if (several && solid && !hasSolid(feat)) {
                continue;
            }
            if (out.object && out.object != feat) {
                why = "it refers to faces of two objects";
                return false;
            }
            out.object = feat;
            auto& base = _bases[feat];
            if (base.isNull()) {
                TopoDS_Shape shape = feat->Shape.getShape().getShape();
                base = Part::TopoShape(shape.Located(TopLoc_Location()));
            }
            TopoDS_Shape sub = _shapes->GetShape(l);
            // Its faces; a file may name edges or points instead (a
            // dimension between two edges, a line profile): those
            bool any = false;
            for (auto type : {TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX}) {
                const char* typeName = type == TopAbs_FACE ? "Face"
                    : type == TopAbs_EDGE                  ? "Edge"
                                                           : "Vertex";
                for (TopExp_Explorer xp(sub, type); xp.More(); xp.Next()) {
                    int idx = base.findShape(xp.Current());
                    if (idx <= 0) {
                        why = std::string("an element of ") + entry(l)
                            + " is not in its object";
                        return false;
                    }
                    std::string name = typeName + std::to_string(idx);
                    if (std::find(out.names.begin(), out.names.end(), name)
                        == out.names.end()) {
                        out.names.push_back(name);
                    }
                    if (!any && out.names.size() == 1) {
                        Primitive first = Geom::ofElement(xp.Current());
                        out.type = first.type;
                        out.dir = first.dir;
                    }
                    any = true;
                }
                if (any) {
                    break;
                }
            }
            if (!any) {
                why = "it refers to " + entry(l) + ", which has no face, edge or vertex";
                return false;
            }
        }
        return true;
    }

    static bool hasSolid(Part::Feature* feat)
    {
        TopExp_Explorer xp(feat->Shape.getShape().getShape(), TopAbs_SOLID);
        return xp.More();
    }

    /// Faces with no surface, only triangles
    static bool tessellated(const TopoDS_Shape& shape)
    {
        TopExp_Explorer xp(shape, TopAbs_FACE);
        for (; xp.More(); xp.Next()) {
            if (!BRep_Tool::Surface(TopoDS::Face(xp.Current())).IsNull()) {
                return false;
            }
        }
        return !shape.IsNull();
    }

    AnnotationSet* set(Part::Feature* obj)
    {
        return Api::getSet(obj, true);
    }

    TolerancedFeature* feature(const Faces& f, bool axis)
    {
        bool axial = f.type == Primitive::Cylinder || f.type == Primitive::Cone
            || f.type == Primitive::Circle;
        return Api::feature(set(partOf(f.object)), f.object, f.names,
                            axis && axial ? "Axis" : nullptr);
    }

    /// The part an object's PMI is of: its own, or -- supplemental geometry
    /// beside a solid in one App::Part, a thread's cylinder drawn as its
    /// circles (NIST CTC-04) -- that solid's, the one there
    Part::Feature* partOf(Part::Feature* obj)
    {
        if (!obj || hasSolid(obj)) {
            return obj;
        }
        auto group = App::GeoFeatureGroupExtension::getGroupOfObject(obj);
        if (!group) {
            return obj;
        }
        Part::Feature* solid = nullptr;
        auto ext = group->getExtensionByType<App::GeoFeatureGroupExtension>();
        for (auto o : ext->Group.getValues()) {
            auto feat = freecad_cast<Part::Feature*>(o);
            if (feat && feat != obj && hasSolid(feat)) {
                if (solid) {
                    return obj;
                }
                solid = feat;
            }
        }
        return solid ? solid : obj;
    }

    /// The file's plane and text point, in the object's placement
    void place(Callout* c, Part::Feature* obj, bool hasPlane, const gp_Ax2& plane, bool hasText,
               const gp_Pnt& textPoint, const TopoDS_Shape& graphic = TopoDS_Shape())
    {
        if (!graphic.IsNull() && hasPlane) {
            // The file's frame, in its plane: its height is twice the text's
            gp_Trsf toPlane;
            toPlane.SetTransformation(gp_Ax3(plane));
            Bnd_Box box;
            BRepBndLib::Add(graphic.Moved(TopLoc_Location(toPlane)), box);
            if (!box.IsVoid()) {
                double x0, y0, z0, x1, y1, z1;
                box.Get(x0, y0, z0, x1, y1, z1);
                if (y1 - y0 > Precision::Confusion()) {
                    _frameHeight = _frameHeight > 0 ? std::min(_frameHeight, y1 - y0) : y1 - y0;
                }
            }
        }
        if (!hasPlane) {
            return;
        }
        gp_Ax3 ax(plane);
        if (hasText) {
            ax.SetLocation(textPoint);
        }
        auto vec = [](const gp_XYZ& v) { return Base::Vector3d(v.X(), v.Y(), v.Z()); };
        Base::Placement local(vec(ax.Location().XYZ()),
                              Base::Rotation::makeRotationByAxes(vec(ax.XDirection().XYZ()),
                                                                 vec(ax.YDirection().XYZ()),
                                                                 vec(ax.Direction().XYZ())));
        c->AutoPlace.setValue(false);
        c->Plane.setValue(obj->Placement.getValue() * local);
    }

    template<typename T>
    T* add(Part::Feature* obj, const char* type, const char* name)
    {
        auto s = set(partOf(obj));
        auto o = static_cast<T*>(_info.doc->addObject(type, name));
        s->addObject(o);
        _made.push_back(o);
        return o;
    }

    /// Kept once: a streamed import may read the same item twice. The
    /// label read again is the one kept, for the views that show it.
    bool seen(const std::string& key, const TDF_Label& l)
    {
        auto it = _keyed.find(key);
        if (it == _keyed.end()) {
            return false;
        }
        ++_report.duplicates;
        _byLabel[entry(l)] = it->second;
        return true;
    }

    /// The item made of a label, and of the labels read again as it
    void made(const std::string& key, const TDF_Label& l, Callout* c)
    {
        _keyed[key] = c;
        _byLabel[entry(l)] = c;
    }

    static std::string key(const Faces& f)
    {
        std::ostringstream ss;
        ss << f.object->getNameInDocument();
        for (auto& n : f.names) {
            ss << "." << n;
        }
        return ss.str();
    }

    void datum(const TDF_Label& l)
    {
        Handle(XCAFDoc_Datum) attr;
        if (!l.FindAttribute(XCAFDoc_Datum::GetID(), attr)) {
            return;
        }
        auto obj = attr->GetObject();
        if (obj.IsNull()) {
            return skip(l, "datum", "it has no semantics");
        }
        std::string letter = text(obj->GetName());
        if (letter.empty()) {
            return skip(l, "datum", "it has no letter");
        }
        if (obj->IsDatumTarget()) {
            ++_targets[letter];
            target(l, obj, letter);
            return;
        }
        NCollection_Sequence<TDF_Label> first, second;
        XCAFDoc_DimTolTool::GetRefShapeLabel(l, first, second);
        if (first.IsEmpty()) {
            _cited.insert(letter);
            _letterOf[entry(l)] = letter;
            return;
        }
        Faces f;
        std::string why;
        if (!faces(first, f, why)) {
            return skip(l, "datum", why);
        }
        _read.insert(letter);
        // Each frame citing a datum whose feature OCCT found has a label
        // with its shapes too: one datum, many labels, not duplicates
        std::string k = "datum " + letter + " " + key(f);
        auto it = _datums.find(k);
        if (it != _datums.end()) {
            _byLabel[entry(l)] = it->second;
            return;
        }
        auto d = add<Datum>(f.object, "PMI::Datum", "Datum");
        _datums[k] = d;
        _byLabel[entry(l)] = d;
        _byLetter.emplace(letter, d);
        d->Letter.setValue(letter);
        d->Feature.setValue(feature(f, true));
        place(d, f.object, obj->HasPlane(), obj->GetPlane(), obj->HasPointText(),
              obj->GetPointTextAttach());
        graphic(d, f.object, obj);
        ++_report.datums;
    }

    /// A datum target (sec 4.3): one per letter and number, the other
    /// labels of it the frames citing it
    void target(const TDF_Label& l, const Handle(XCAFDimTolObjects_DatumObject) & obj,
                const std::string& letter)
    {
        std::string k = "target " + letter + std::to_string(obj->GetDatumTargetNumber());
        if (auto it = _keyed.find(k); it != _keyed.end()) {
            _byLabel[entry(l)] = it->second;
            return;
        }
        int type = -1;
        switch (obj->GetDatumTargetType()) {
            case XCAFDimTolObjects_DatumTargetType_Point: type = DatumTarget::Point; break;
            case XCAFDimTolObjects_DatumTargetType_Line: type = DatumTarget::Line; break;
            case XCAFDimTolObjects_DatumTargetType_Rectangle: type = DatumTarget::Rectangle; break;
            case XCAFDimTolObjects_DatumTargetType_Circle: type = DatumTarget::Circle; break;
            case XCAFDimTolObjects_DatumTargetType_Area: type = DatumTarget::Area; break;
        }
        if (type < 0) {
            return skip(l, "datum target " + k.substr(7), "its type is not read");
        }
        NCollection_Sequence<TDF_Label> first, second;
        XCAFDoc_DimTolTool::GetRefShapeLabel(l, first, second);
        Faces f;
        std::string why;
        bool onFaces = false;
        Part::Feature* owner = nullptr;
        if (type == DatumTarget::Area) {
            if (!faces(first, f, why)) {
                return skip(l, "datum target " + k.substr(7), why);
            }
            onFaces = true;
            owner = f.object;
        }
        else if (!obj->HasDatumTargetParams()) {
            return skip(l, "datum target " + k.substr(7), "it has no position");
        }
        else {
            // The object of the shape the file gives it (a vertex, an edge
            // or a face), else the part: a rectangle has none
            owner = first.IsEmpty() ? nullptr : objectOf(first.First());
            if (!owner) {
                owner = solidObject();
            }
            if (!owner) {
                return skip(l, "datum target " + k.substr(7), "no object for it");
            }
        }
        // The target's frame: the file's axis is often only a point (CTC-02
        // gives every point (0, 0, 1)), and a line's runs along its normal
        // (FTC-06). The part says: z the face's outward normal, a line along
        // the edge it is when it is one.
        gp_Ax2 ax = obj->GetDatumTargetAxis();
        gp_Pnt at = ax.Location();
        gp_Dir along = ax.XDirection();
        double edgeLength = 0;
        if (type == DatumTarget::Line && !first.IsEmpty()) {
            TopoDS_Shape sh = _shapes->GetShape(first.First());
            if (sh.ShapeType() == TopAbs_EDGE) {
                TopoDS_Vertex v0, v1;
                TopExp::Vertices(TopoDS::Edge(sh), v0, v1);
                if (!v0.IsNull() && !v1.IsNull()) {
                    gp_Pnt p0 = BRep_Tool::Pnt(v0), p1 = BRep_Tool::Pnt(v1);
                    if (p0.Distance(p1) > Precision::Confusion()) {
                        at = gp_Pnt((p0.XYZ() + p1.XYZ()) / 2);
                        along = gp_Dir(gp_Vec(p0, p1));
                        edgeLength = p0.Distance(p1);
                    }
                }
            }
        }
        gp_Dir normal = ax.Direction();
        if (type != DatumTarget::Area) {
            // On the part's face: the shape the file gives is often loose
            // geometry of its own (CTC-02's points in an empty compound,
            // FTC-06's lines too), else its own face (CTC-02's K1, a disk)
            std::vector<Part::Feature*> tries;
            if (hasSolid(owner)) {
                tries.push_back(owner);
            }
            if (auto solid = solidObject(); solid && solid != owner) {
                tries.push_back(solid);
            }
            tries.push_back(owner);
            for (auto o : tries) {
                if (faceAt(o, gp_Ax2(at, ax.Direction()), f, normal)) {
                    owner = o;
                    onFaces = true;
                    break;
                }
            }
            if (!onFaces) {
                owner = tries.front();
            }
        }
        gp_Vec x = gp_Vec(along) - gp_Vec(normal) * gp_Vec(along).Dot(gp_Vec(normal));
        if (x.Magnitude() < 1e-9) {
            // Any direction across the normal
            gp_Dir any = std::fabs(normal.X()) < 0.9 ? gp_Dir(1, 0, 0) : gp_Dir(0, 1, 0);
            x = gp_Vec(any) - gp_Vec(normal) * gp_Vec(any).Dot(gp_Vec(normal));
        }
        ax = gp_Ax2(at, normal, gp_Dir(x));
        auto t = add<DatumTarget>(owner, "PMI::DatumTarget", "DatumTarget");
        _keyed[k] = t;
        _byLabel[entry(l)] = t;
        _targetsOf[letter].push_back(t);
        t->Label.setValue(k.substr(7));
        t->Number.setValue(obj->GetDatumTargetNumber());
        t->TargetType.setValue(long(type));
        if (obj->HasDatumTargetParams()) {
            auto vec = [](const gp_XYZ& p) { return Base::Vector3d(p.X(), p.Y(), p.Z()); };
            Base::Placement local(vec(ax.Location().XYZ()),
                                  Base::Rotation::makeRotationByAxes(vec(ax.XDirection().XYZ()),
                                                                     vec(ax.YDirection().XYZ()),
                                                                     vec(ax.Direction().XYZ()),
                                                                     "ZXY"));
            t->Target.setValue(owner->Placement.getValue() * local);
            t->Length.setValue(edgeLength > 0 ? edgeLength : obj->GetDatumTargetLength());
            t->Width.setValue(obj->GetDatumTargetWidth());
        }
        if (onFaces) {
            t->Feature.setValue(feature(f, false));
        }
        place(t, owner, obj->HasPlane(), obj->GetPlane(), obj->HasPointText(),
              obj->GetPointTextAttach());
        graphic(t, owner, obj);
        ++_report.targets;
    }

    /// The object made for a shape label or its owner
    Part::Feature* objectOf(const TDF_Label& l)
    {
        TDF_Label owner = XCAFDoc_ShapeTool::IsSubShape(l) ? l.Father() : l;
        return freecad_cast<Part::Feature*>(_info.objectOf(owner));
    }

    /// The first object made with a solid: what a target with no shape is on
    Part::Feature* solidObject()
    {
        auto all = solidObjects(true);
        return all.empty() ? nullptr : all.front();
    }

    /// The objects made with a solid, from the file's free shapes down
    /// (only the first with \a first)
    std::vector<Part::Feature*> solidObjects(bool first = false)
    {
        std::vector<Part::Feature*> res;
        NCollection_Sequence<TDF_Label> free;
        _shapes->GetFreeShapes(free);
        std::vector<TDF_Label> todo;
        for (NCollection_Sequence<TDF_Label>::Iterator it(free); it.More(); it.Next()) {
            todo.push_back(it.Value());
        }
        while (!todo.empty()) {
            TDF_Label l = todo.back();
            todo.pop_back();
            if (auto feat = objectOf(l); feat && hasSolid(feat)) {
                if (std::find(res.begin(), res.end(), feat) == res.end()) {
                    res.push_back(feat);
                }
                if (first) {
                    return res;
                }
            }
            NCollection_Sequence<TDF_Label> comps;
            if (XCAFDoc_ShapeTool::IsAssembly(l) && XCAFDoc_ShapeTool::GetComponents(l, comps)) {
                for (auto& c : comps) {
                    TDF_Label ref;
                    todo.push_back(XCAFDoc_ShapeTool::GetReferredShape(c, ref) ? ref : c);
                }
            }
        }
        return res;
    }

    /// The face of \a obj a target at \a ax lies on: within reach of its
    /// centre, facing as it does
    bool faceAt(Part::Feature* obj, const gp_Ax2& ax, Faces& out, gp_Dir& normal)
    {
        auto& base = _bases[obj];
        if (base.isNull()) {
            base = Part::TopoShape(obj->Shape.getShape().getShape().Located(TopLoc_Location()));
        }
        TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(ax.Location()).Vertex();
        int best = 0;
        double bestDist = 0;
        bool bestFacing = false;
        gp_Vec bestNormal;
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> faces;
        TopExp::MapShapes(base.getShape(), TopAbs_FACE, faces);
        for (int i = 1; i <= faces.Extent(); ++i) {
            const TopoDS_Face& face = TopoDS::Face(faces(i));
            BRepExtrema_DistShapeShape dist(v, face);
            if (!dist.IsDone() || dist.NbSolution() == 0) {
                continue;
            }
            double d = dist.Value();
            // The normal where the point is: on the face's boundary too
            gp_Vec n;
            {
                double u, w;
                BRepGProp_Face prop(face);
                if (dist.SupportTypeShape2(1) == BRepExtrema_IsInFace) {
                    dist.ParOnFaceS2(1, u, w);
                }
                else {
                    GeomAPI_ProjectPointOnSurf proj(dist.PointOnShape2(1),
                                                    BRep_Tool::Surface(face));
                    if (proj.NbPoints() == 0) {
                        continue;
                    }
                    proj.LowerDistanceParameters(u, w);
                }
                gp_Pnt p;
                prop.Normal(u, w, p, n);
            }
            bool facing = n.Magnitude() > 1e-12 && gp_Dir(n).Dot(ax.Direction()) > 0.9;
            if (!best || d < bestDist - 1e-9 || (d < bestDist + 1e-9 && facing && !bestFacing)) {
                best = i;
                bestDist = d;
                bestFacing = facing;
                bestNormal = n;
            }
        }
        if (!best || bestDist > TargetReach || bestNormal.Magnitude() < 1e-12) {
            return false;
        }
        normal = gp_Dir(bestNormal);
        out = Faces();
        out.object = obj;
        out.names.push_back("Face" + std::to_string(base.findShape(faces(best))));
        Primitive first = Geom::ofFace(faces(best));
        out.type = first.type;
        out.dir = first.dir;
        return true;
    }

    /// The datums of the letters only targets give: one each, with no
    /// feature; and each datum's targets, in number order
    void targetDatums()
    {
        for (auto& [letter, ts] : _targetsOf) {
            Datum* d = nullptr;
            auto range = _byLetter.equal_range(letter);
            for (auto it = range.first; it != range.second && !d; ++it) {
                d = freecad_cast<Datum*>(it->second);
            }
            if (!d) {
                AnnotationSet* s = ts.front()->getSet();
                if (!s) {
                    continue;
                }
                d = static_cast<Datum*>(_info.doc->addObject("PMI::Datum", "Datum"));
                s->addObject(d);
                _made.push_back(d);
                _byLetter.emplace(letter, d);
                d->Letter.setValue(letter);
                _read.insert(letter);
                ++_report.datums;
            }
            std::sort(ts.begin(), ts.end(), [](DatumTarget* a, DatumTarget* b) {
                return a->Number.getValue() < b->Number.getValue();
            });
            std::vector<App::DocumentObject*> all(ts.begin(), ts.end());
            d->Targets.setValues(all);
        }
    }

    static int characteristic(XCAFDimTolObjects_GeomToleranceType t)
    {
        using GT = GeometricTolerance;
        switch (t) {
            case XCAFDimTolObjects_GeomToleranceType_Angularity: return GT::Angularity;
            case XCAFDimTolObjects_GeomToleranceType_CircularRunout: return GT::CircularRunout;
            case XCAFDimTolObjects_GeomToleranceType_CircularityOrRoundness: return GT::Circularity;
            case XCAFDimTolObjects_GeomToleranceType_Coaxiality: return GT::Coaxiality;
            case XCAFDimTolObjects_GeomToleranceType_Concentricity: return GT::Concentricity;
            case XCAFDimTolObjects_GeomToleranceType_Cylindricity: return GT::Cylindricity;
            case XCAFDimTolObjects_GeomToleranceType_Flatness: return GT::Flatness;
            case XCAFDimTolObjects_GeomToleranceType_Parallelism: return GT::Parallelism;
            case XCAFDimTolObjects_GeomToleranceType_Perpendicularity: return GT::Perpendicularity;
            case XCAFDimTolObjects_GeomToleranceType_Position: return GT::Position;
            case XCAFDimTolObjects_GeomToleranceType_ProfileOfLine: return GT::ProfileOfLine;
            case XCAFDimTolObjects_GeomToleranceType_ProfileOfSurface: return GT::ProfileOfSurface;
            case XCAFDimTolObjects_GeomToleranceType_Straightness: return GT::Straightness;
            case XCAFDimTolObjects_GeomToleranceType_Symmetry: return GT::Symmetry;
            case XCAFDimTolObjects_GeomToleranceType_TotalRunout: return GT::TotalRunout;
            default: return -1;
        }
    }

    static const char* modifier(XCAFDimTolObjects_GeomToleranceModif m)
    {
        switch (m) {
            case XCAFDimTolObjects_GeomToleranceModif_Any_Cross_Section: return "ACS";
            case XCAFDimTolObjects_GeomToleranceModif_Common_Zone: return "CZ";
            case XCAFDimTolObjects_GeomToleranceModif_Free_State: return "F";
            case XCAFDimTolObjects_GeomToleranceModif_Line_Element: return "LE";
            case XCAFDimTolObjects_GeomToleranceModif_Major_Diameter: return "MD";
            case XCAFDimTolObjects_GeomToleranceModif_Minor_Diameter: return "LD";
            case XCAFDimTolObjects_GeomToleranceModif_Not_Convex: return "NC";
            case XCAFDimTolObjects_GeomToleranceModif_Pitch_Diameter: return "PD";
            case XCAFDimTolObjects_GeomToleranceModif_Separate_Requirement: return "SZ";
            case XCAFDimTolObjects_GeomToleranceModif_Statistical_Tolerance: return "ST";
            case XCAFDimTolObjects_GeomToleranceModif_Tangent_Plane: return "T";
            case XCAFDimTolObjects_GeomToleranceModif_All_Around: return "AllAround";
            case XCAFDimTolObjects_GeomToleranceModif_All_Over: return "AllOver";
            default: return nullptr;
        }
    }

    static const char* modifier(XCAFDimTolObjects_DatumSingleModif m)
    {
        switch (m) {
            case XCAFDimTolObjects_DatumSingleModif_MaximumMaterialRequirement: return "M";
            case XCAFDimTolObjects_DatumSingleModif_LeastMaterialRequirement: return "L";
            case XCAFDimTolObjects_DatumSingleModif_FreeState: return "F";
            case XCAFDimTolObjects_DatumSingleModif_Basic: return "BSC";
            case XCAFDimTolObjects_DatumSingleModif_Translation: return "><";
            case XCAFDimTolObjects_DatumSingleModif_Point: return "PT";
            case XCAFDimTolObjects_DatumSingleModif_Line: return "SL";
            case XCAFDimTolObjects_DatumSingleModif_Plane: return "PL";
            case XCAFDimTolObjects_DatumSingleModif_AnyCrossSection: return "ACS";
            case XCAFDimTolObjects_DatumSingleModif_Any_LongitudinalSection: return "ALS";
            case XCAFDimTolObjects_DatumSingleModif_ContactingFeature: return "CF";
            case XCAFDimTolObjects_DatumSingleModif_DistanceVariable: return "DV";
            case XCAFDimTolObjects_DatumSingleModif_MajorDiameter: return "MD";
            case XCAFDimTolObjects_DatumSingleModif_MinorDiameter: return "LD";
            case XCAFDimTolObjects_DatumSingleModif_PitchDiameter: return "PD";
            default: return nullptr;
        }
    }

    /// The tolerance's datum compartments: by precedence, a common datum
    /// joined (A-B), each with its modifiers
    std::vector<std::string> compartments(const TDF_Label& l)
    {
        NCollection_Sequence<TDF_Label> datums;
        XCAFDoc_DimTolTool::GetDatumWithObjectOfTolerLabels(l, datums);
        std::map<int, std::pair<std::vector<std::string>, std::vector<std::string>>> byPosition;
        for (auto& dl : datums) {
            Handle(XCAFDoc_Datum) attr;
            if (!dl.FindAttribute(XCAFDoc_Datum::GetID(), attr)) {
                continue;
            }
            auto obj = attr->GetObject();
            if (obj.IsNull()) {
                continue;
            }
            auto& c = byPosition[obj->GetPosition()];
            std::string letter = text(obj->GetName());
            if (std::find(c.first.begin(), c.first.end(), letter) == c.first.end()) {
                c.first.push_back(letter);
            }
            for (auto m : obj->GetModifiers()) {
                const char* name = modifier(m);
                if (name && std::find(c.second.begin(), c.second.end(), name) == c.second.end()) {
                    c.second.emplace_back(name);
                }
            }
        }
        std::vector<std::string> out;
        for (auto& [pos, c] : byPosition) {
            std::string s;
            for (auto& d : c.first) {
                s += (s.empty() ? "" : "-") + d;
            }
            if (!c.second.empty()) {
                s += "(";
                for (std::size_t i = 0; i < c.second.size(); ++i) {
                    s += (i ? "," : "") + c.second[i];
                }
                s += ")";
            }
            out.push_back(s);
        }
        return out;
    }

    void tolerance(const TDF_Label& l)
    {
        Handle(XCAFDoc_GeomTolerance) attr;
        if (!l.FindAttribute(XCAFDoc_GeomTolerance::GetID(), attr)) {
            return;
        }
        auto obj = attr->GetObject();
        if (obj.IsNull()) {
            return skip(l, "tolerance", "it has no semantics");
        }
        int ch = characteristic(obj->GetType());
        if (ch < 0) {
            return skip(l, "tolerance", "it has no characteristic");
        }
        NCollection_Sequence<TDF_Label> first, second;
        XCAFDoc_DimTolTool::GetRefShapeLabel(l, first, second);
        Faces f;
        std::string why;
        if (!faces(first, f, why)) {
            return skip(l, "tolerance", why);
        }
        using GT = GeometricTolerance;
        int zone = obj->GetTypeOfValue() == XCAFDimTolObjects_GeomToleranceTypeValue_Diameter ? 1
            : obj->GetTypeOfValue() == XCAFDimTolObjects_GeomToleranceTypeValue_SphericalDiameter
            ? 2
            : 0;
        auto comps = compartments(l);
        std::ostringstream k;
        k << "tolerance " << ch << " " << zone << " " << obj->GetValue() << " " << key(f);
        for (auto& c : comps) {
            k << "|" << c;
        }
        if (seen(k.str(), l)) {
            return;
        }
        // A position, a coaxiality, or a cylindrical zone of a cylinder is
        // of its axis
        bool axis = zone == 1 || ch == GT::Position || ch == GT::Coaxiality
            || ch == GT::Concentricity;
        auto gt = add<GT>(f.object, "PMI::GeometricTolerance", "Tolerance");
        made(k.str(), l, gt);
        gt->Characteristic.setValue(ch);
        gt->Feature.setValue(feature(f, axis));
        gt->ZoneShape.setValue(zone);
        gt->Value.setValue(obj->GetValue());
        switch (obj->GetMaterialRequirementModifier()) {
            case XCAFDimTolObjects_GeomToleranceMatReqModif_M:
                gt->MaterialCondition.setValue(1L);
                break;
            case XCAFDimTolObjects_GeomToleranceMatReqModif_L:
                gt->MaterialCondition.setValue(2L);
                break;
            default:
                break;
        }
        std::vector<std::string> mods;
        for (auto m : obj->GetModifiers()) {
            if (m == XCAFDimTolObjects_GeomToleranceModif_Maximum_Material_Requirement) {
                gt->MaterialCondition.setValue(1L);
            }
            else if (m == XCAFDimTolObjects_GeomToleranceModif_Least_Material_Requirement) {
                gt->MaterialCondition.setValue(2L);
            }
            else if (const char* name = modifier(m)) {
                mods.emplace_back(name);
            }
        }
        if (obj->GetZoneModifier() == XCAFDimTolObjects_GeomToleranceZoneModif_Projected) {
            mods.emplace_back("P");
            gt->ProjectedLength.setValue(obj->GetValueOfZoneModifier());
        }
        gt->Modifiers.setValues(mods);
        if (obj->GetMaxValueModifier() > 0) {
            gt->MaxValue.setValue(obj->GetMaxValueModifier());
        }
        switch (obj->GetUnitBasis()) {
            case XCAFDimTolObjects_GeomToleranceUnitBasis_Length:
                gt->UnitBasis.setValue(long(GT::PerLength));
                break;
            case XCAFDimTolObjects_GeomToleranceUnitBasis_Square:
                gt->UnitBasis.setValue(long(GT::PerSquare));
                break;
            case XCAFDimTolObjects_GeomToleranceUnitBasis_Rectangle:
                gt->UnitBasis.setValue(long(GT::PerRectangle));
                gt->UnitSize2.setValue(obj->GetSecondUnitSize());
                break;
            case XCAFDimTolObjects_GeomToleranceUnitBasis_Circle:
                gt->UnitBasis.setValue(long(GT::PerCircle));
                break;
            default:
                break;
        }
        if (obj->GetUnitBasis() != XCAFDimTolObjects_GeomToleranceUnitBasis_None) {
            gt->UnitSize.setValue(obj->GetUnitSize());
        }
        if (!comps.empty()) {
            gt->DatumSystem.setValue(Api::frame(set(f.object), comps));
        }
        place(gt, f.object, obj->HasPlane(), obj->GetPlane(), obj->HasPointText(),
              obj->GetPointTextAttach(), obj->GetPresentation());
        graphic(gt, f.object, obj);
        ++_report.tolerances;
    }

    /// Our type, or -1
    static int dimensionType(XCAFDimTolObjects_DimensionType t)
    {
        using D = Dimension;
        switch (t) {
            case XCAFDimTolObjects_DimensionType_Size_Diameter: return D::Diameter;
            case XCAFDimTolObjects_DimensionType_Size_Radius: return D::Radius;
            case XCAFDimTolObjects_DimensionType_Size_SphericalDiameter:
                return D::SphericalDiameter;
            case XCAFDimTolObjects_DimensionType_Size_SphericalRadius: return D::SphericalRadius;
            case XCAFDimTolObjects_DimensionType_Size_Thickness: return D::Width;
            case XCAFDimTolObjects_DimensionType_Size_Angular: return D::AngularSize;
            case XCAFDimTolObjects_DimensionType_Size_CurveLength: return D::CurveLength;
            case XCAFDimTolObjects_DimensionType_Location_Angular: return D::Angle;
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance:
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromCenterToOuter:
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromCenterToInner:
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromOuterToCenter:
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromOuterToOuter:
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromOuterToInner:
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromInnerToCenter:
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromInnerToOuter:
            case XCAFDimTolObjects_DimensionType_Location_LinearDistance_FromInnerToInner:
                return D::Distance;
            default: return -1;
        }
    }

    static const char* modifier(XCAFDimTolObjects_DimensionModif m)
    {
        switch (m) {
            case XCAFDimTolObjects_DimensionModif_TwoPointSize: return "LP";
            case XCAFDimTolObjects_DimensionModif_LocalSizeDefinedBySphere: return "LS";
            case XCAFDimTolObjects_DimensionModif_LeastSquaresAssociationCriterion: return "GG";
            case XCAFDimTolObjects_DimensionModif_MaximumInscribedAssociation: return "GX";
            case XCAFDimTolObjects_DimensionModif_MinimumCircumscribedAssociation: return "GN";
            case XCAFDimTolObjects_DimensionModif_CircumferenceDiameter: return "CC";
            case XCAFDimTolObjects_DimensionModif_AreaDiameter: return "CA";
            case XCAFDimTolObjects_DimensionModif_VolumeDiameter: return "CV";
            case XCAFDimTolObjects_DimensionModif_MaximumSize: return "SX";
            case XCAFDimTolObjects_DimensionModif_MinimumSize: return "SN";
            case XCAFDimTolObjects_DimensionModif_AverageSize: return "SA";
            case XCAFDimTolObjects_DimensionModif_MedianSize: return "SM";
            case XCAFDimTolObjects_DimensionModif_MidRangeSize: return "SD";
            case XCAFDimTolObjects_DimensionModif_RangeOfSizes: return "SR";
            case XCAFDimTolObjects_DimensionModif_Square: return "SQ";
            case XCAFDimTolObjects_DimensionModif_AnyCrossSection: return "ACS";
            case XCAFDimTolObjects_DimensionModif_SpecificFixedCrossSection: return "SCS";
            case XCAFDimTolObjects_DimensionModif_CommonTolerance: return "CT";
            case XCAFDimTolObjects_DimensionModif_FreeStateCondition: return "F";
            case XCAFDimTolObjects_DimensionModif_Between: return "Between";
            default: return nullptr;
        }
    }

    void dimension(const TDF_Label& l)
    {
        Handle(XCAFDoc_Dimension) attr;
        if (!l.FindAttribute(XCAFDoc_Dimension::GetID(), attr)) {
            return;
        }
        auto obj = attr->GetObject();
        if (obj.IsNull()) {
            return skip(l, "dimension", "it has no semantics");
        }
        // No type and no value: the file gives it only as a graphic, as it
        // does a "dimension presentation"
        if (obj->GetType() == XCAFDimTolObjects_DimensionType_DimensionPresentation
            || obj->GetType() == XCAFDimTolObjects_DimensionType_CommonLabel
            || (obj->GetType() == XCAFDimTolObjects_DimensionType_Location_None
                && !obj->HasQualifier() && obj->GetValues().IsNull())) {
            graphicOnly(l, obj);
            return;
        }
        int type = dimensionType(obj->GetType());
        {
            NCollection_Sequence<TDF_Label> a, b;
            XCAFDoc_DimTolTool::GetRefShapeLabel(l, a, b);
            FC_LOG("dimension " << entry(l) << ": type " << int(obj->GetType()) << ", value "
                                << obj->GetValue() << ", shapes " << a.Length() << "/"
                                << b.Length());
        }
        if (type < 0) {
            return skip(l, "dimension", "its type is not read yet ("
                                            + std::to_string(int(obj->GetType())) + ")");
        }
        NCollection_Sequence<TDF_Label> first, second;
        XCAFDoc_DimTolTool::GetRefShapeLabel(l, first, second);
        Faces f1, f2;
        std::string why;
        if (!faces(first, f1, why)) {
            return skip(l, "dimension", why);
        }
        bool location = type == Dimension::Distance || type == Dimension::Angle;
        if (location && !faces(second, f2, why)) {
            return skip(l, "dimension", "its second feature: " + why);
        }
        // Between two objects of one part: a solid and its supplemental
        // geometry (CTC-04's location to a thread's upper plane)
        if (location && partOf(f2.object) != partOf(f1.object)) {
            return skip(l, "dimension", "it measures between two objects");
        }
        std::ostringstream k;
        k << "dimension " << type << " " << obj->GetValue() << " " << key(f1);
        if (location) {
            k << " " << key(f2);
        }
        if (seen(k.str(), l)) {
            return;
        }
        auto dim = add<Dimension>(f1.object, "PMI::Dimension", "Dimension");
        made(k.str(), l, dim);
        dim->Type.setValue(type);
        // Circles a plane is square to are where they lie, not an axis: a
        // thread's upper plane drawn as its rims, 25 from a face (CTC-04)
        auto rims = [](const Faces& c, const Faces& p) {
            return c.type == Primitive::Circle && p.type == Primitive::Plane
                && c.dir.IsParallel(p.dir, 1e-9);
        };
        std::vector<App::DocumentObject*> feats {feature(f1, location && !rims(f1, f2))};
        if (location) {
            feats.push_back(feature(f2, !rims(f2, f1)));
        }
        dim->Features.setValues(feats);
        double value = obj->GetValue();
        // The file's, until the model is measured: kept where it cannot be
        // (a curve length along faces, a width of what is not two planes)
        dim->Nominal.setValue(value);
        if (obj->IsDimWithPlusMinusTolerance()) {
            dim->Upper.setValue(obj->GetUpperTolValue());
            // OCCT keeps the lower deviation as a magnitude: its writer negates it
            dim->Lower.setValue(-obj->GetLowerTolValue());
        }
        else if (obj->IsDimWithRange()) {
            dim->Upper.setValue(obj->GetUpperBound() - value);
            dim->Lower.setValue(obj->GetLowerBound() - value);
            // Given as its limits, shown and written so again
            dim->Limits.setValue(true);
        }
        // A fit, its class of tolerance: "G6", a hole's; "g6", a shaft's (FTC-10)
        bool hole = false;
        XCAFDimTolObjects_DimensionFormVariance variance;
        XCAFDimTolObjects_DimensionGrade grade;
        if (obj->IsDimWithClassOfTolerance() && obj->GetClassOfTolerance(hole, variance, grade)
            && int(variance) > 0 && int(variance) < 29 && int(grade) >= 0 && int(grade) < 20) {
            std::string fit = Step::FitVariances[int(variance)];
            if (!hole) {
                for (auto& c : fit) {
                    c = Step::asciiLower(c);
                }
            }
            dim->Fit.setValue(fit + Step::FitGrades[int(grade)]);
        }
        // A reference dimension: a 'dimensional note' that is 'auxiliary';
        // a basic one, 'theoretical'
        for (int i = 0; i < obj->NbDescriptions(); ++i) {
            auto text = obj->GetDescription(i);
            if (!text.IsNull() && text->String().IsEqual("auxiliary")) {
                dim->Reference.setValue(true);
            }
            if (!text.IsNull() && text->String().IsEqual("theoretical")) {
                dim->Basic.setValue(true);
            }
        }
        int left = 0, right = 0;
        obj->GetNbOfDecimalPlaces(left, right);
        if (right > 0) {
            dim->DecimalPlaces.setValue(right);
        }
        std::vector<std::string> mods;
        for (auto m : obj->GetModifiers()) {
            if (const char* name = modifier(m)) {
                mods.emplace_back(name);
            }
        }
        dim->Modifiers.setValues(mods);
        place(dim, f1.object, obj->HasPlane(), obj->GetPlane(), obj->HasTextPoint(),
              obj->GetPointTextAttach());
        graphic(dim, f1.object, obj);
        _values.emplace_back(dim, value);
        ++_report.dimensions;
    }

    /// The graphic the file drew for \a c, into the set's coordinates:
    /// drawn once the import has done (UseImported)
    /// with the kind the file named it
    template<typename T>
    void graphic(Callout* c, Part::Feature* obj, const Handle(T) & x)
    {
        TopoDS_Shape shape = x->GetPresentation();
        if (shape.IsNull()) {
            return;
        }
        gp_Trsf trsf;
        Part::TopoShape::convertTogpTrsf(obj->Placement.getValue().toMatrix(), trsf);
        _graphics.emplace_back(c, shape.Moved(TopLoc_Location(trsf)));
        _graphicTypes[c] = text(x->GetPresentationType());
        _lastObject = obj;
    }

    /// PMI with only a graphic: kept until all else is read, as it may be
    /// the graphic of what is read later
    void graphicOnly(const TDF_Label& l, const Handle(XCAFDimTolObjects_DimensionObject) & obj)
    {
        _pending.push_back({l, obj, _lastObject});
    }

    struct Pending
    {
        TDF_Label label;
        Handle(XCAFDimTolObjects_DimensionObject) obj;
        Part::Feature* last;
    };

    /// What compares two graphics: their edges and their extent
    struct Extent
    {
        int edges = 0;
        Bnd_Box box;
    };

    static Extent extent(const TopoDS_Shape& shape)
    {
        Extent e;
        NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> edges;
        TopExp::MapShapes(shape, TopAbs_EDGE, edges);
        e.edges = edges.Extent();
        BRepBndLib::Add(shape, e.box);
        return e;
    }

    static bool same(const Extent& a, const Extent& b)
    {
        if (a.edges != b.edges || a.box.IsVoid() || b.box.IsVoid()) {
            return false;
        }
        double tol = 1e-6 * std::max(1.0, std::sqrt(a.box.SquareExtent()));
        return a.box.CornerMin().Distance(b.box.CornerMin()) < tol
            && a.box.CornerMax().Distance(b.box.CornerMax()) < tol;
    }

    /// The graphic-only PMI, each a note that carries it (sec 5.1 step 3),
    /// on its feature if the file names one. One drawn as something read
    /// with its semantics is that one's graphic given again (CTC-01 gives
    /// its datums' this way): kept once, the label shows that one.
    void notes()
    {
        std::vector<Extent> read;
        read.reserve(_graphics.size());
        for (auto& g : _graphics) {
            read.push_back(extent(g.second));
        }
        for (auto& p : _pending) {
            guard(p.label, "graphic", [&]() { note(p, read); });
        }
    }

    void note(const Pending& p, const std::vector<Extent>& read)
    {
        TopoDS_Shape shape = p.obj->GetPresentation();
        NCollection_Sequence<TDF_Label> first, second;
        XCAFDoc_DimTolTool::GetRefShapeLabel(p.label, first, second);
        Faces f;
        std::string why;
        bool onFaces = faces(first, f, why);
        // On its feature; else beside what was read last; else -- a file of
        // graphics alone, on a part whose faces it cannot name (a tessellated
        // one) -- beside the part
        Part::Feature* owner = onFaces ? f.object : p.last ? p.last : solidObject();
        if (shape.IsNull() || !owner) {
            ++_report.graphicOnly;
            return;
        }
        gp_Trsf trsf;
        Part::TopoShape::convertTogpTrsf(owner->Placement.getValue().toMatrix(), trsf);
        Extent e = extent(shape.Moved(TopLoc_Location(trsf)));
        for (std::size_t i = 0; i < read.size(); ++i) {
            if (same(e, read[i])) {
                _byLabel[entry(p.label)] = _graphics[i].first;
                ++_report.twice;
                return;
            }
        }
        auto n = add<Note>(owner, "PMI::Note", "Note");
        _byLabel[entry(p.label)] = n;
        std::string name = text(p.obj->GetPresentationName());
        if (!name.empty()) {
            n->Label.setValue(name);
        }
        // Its words, when the file gives them (a semantic text): a line
        // each, and each of its lines
        std::vector<std::string> lines;
        for (int i = 0; i < p.obj->NbDescriptions(); ++i) {
            std::istringstream words(text(p.obj->GetDescription(i)));
            for (std::string line; std::getline(words, line);) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                lines.push_back(line);
            }
        }
        if (!lines.empty()) {
            n->Text.setValues(lines);
        }
        if (onFaces) {
            n->Feature.setValue(feature(f, false));
        }
        place(n, owner, p.obj->HasPlane(), p.obj->GetPlane(), p.obj->HasTextPoint(),
              p.obj->GetPointTextAttach());
        graphic(n, owner, p.obj);
        ++_report.notes;
    }

    /// The file's saved views (sec 5.1 step 4): a PMI::View each, with
    /// the callouts it shows as its members
    void views()
    {
        TDF_Label main = _info.hDoc->Main();
        TDF_Label viewsLabel = XCAFDoc_DocumentTool::ViewsLabel(main);
        Handle(XCAFDoc_ViewTool) tool;
        if (_made.empty() || !viewsLabel.FindAttribute(XCAFDoc_ViewTool::GetID(), tool)) {
            return;
        }
        NCollection_Sequence<TDF_Label> labels;
        tool->GetViewLabels(labels);
        for (auto& l : labels) {
            guard(l, "view", [&]() { view(tool, l); });
        }
    }

    void view(const Handle(XCAFDoc_ViewTool) & tool, const TDF_Label& l)
    {
        Handle(XCAFDoc_View) attr;
        if (!l.FindAttribute(XCAFDoc_View::GetID(), attr) || attr->GetObject().IsNull()) {
            return;
        }
        auto obj = attr->GetObject();
        if (obj->Type() == XCAFView_ProjectionType_NoCamera) {
            return skip(l, "view", "it has no camera");
        }
        // Its callouts: a datum label only a frame cites shows the datum
        // of its letter
        std::vector<Callout*> members;
        NCollection_Sequence<TDF_Label> gdts;
        tool->GetRefGDTLabel(l, gdts);
        for (auto& g : gdts) {
            std::string e = entry(g);
            Callout* c = nullptr;
            auto it = _byLabel.find(e);
            if (it != _byLabel.end()) {
                c = it->second;
            }
            else if (auto lt = _letterOf.find(e); lt != _letterOf.end()) {
                auto dt = _byLetter.find(lt->second);
                c = dt == _byLetter.end() ? nullptr : dt->second;
            }
            if (c && std::find(members.begin(), members.end(), c) == members.end()) {
                members.push_back(c);
            }
        }
        // In the set of what it shows, else the first one made
        Callout* first = members.empty() ? _made.front() : members.front();
        AnnotationSet* s = first->getSet();
        auto part = s ? freecad_cast<App::GeoFeature*>(s->Part.getValue()) : nullptr;
        if (!s || !part) {
            return skip(l, "view", "no annotation set for it");
        }
        auto v = static_cast<View*>(_info.doc->addObject("PMI::View", "View"));
        s->addObject(v);
        std::string name = text(obj->Name());
        if (!name.empty()) {
            v->Label.setValue(name);
        }
        // The camera looks along -z with y up; its origin the point looked
        // at, here the one on the line of sight nearest the part
        gp_Dir look = obj->ViewDirection();
        gp_Dir up = obj->UpDirection();
        gp_Dir z = look.Reversed();
        if (up.IsParallel(z, Precision::Angular())) {
            return skip(l, "view", "its up direction is its direction of sight");
        }
        gp_Dir x = up.Crossed(z);
        gp_Dir y = z.Crossed(x);
        gp_Pnt eye = obj->ProjectionPoint();
        gp_Pnt target = eye;
        if (auto feat = freecad_cast<Part::Feature*>(part)) {
            Bnd_Box box;
            BRepBndLib::Add(feat->Shape.getShape().getShape().Located(TopLoc_Location()), box);
            if (!box.IsVoid()) {
                gp_Pnt c = (box.CornerMin().XYZ() + box.CornerMax().XYZ()) / 2;
                target = eye.Translated(gp_Vec(look) * gp_Vec(eye, c).Dot(gp_Vec(look)));
            }
        }
        auto vec = [](const gp_XYZ& p) { return Base::Vector3d(p.X(), p.Y(), p.Z()); };
        Base::Placement local(vec(target.XYZ()),
                              Base::Rotation::makeRotationByAxes(vec(x.XYZ()), vec(y.XYZ()),
                                                                 vec(z.XYZ()), "ZXY"));
        v->Standard.setValue(long(View::Custom));
        // A saved view shows; its callouts keep the planes the file gives them
        v->AnnotationPlane.setValue(false);
        v->Camera.setValue(part->Placement.getValue() * local);
        v->Perspective.setValue(obj->Type() == XCAFView_ProjectionType_Central);
        if (obj->WindowVerticalSize() > Precision::Confusion()) {
            v->Height.setValue(obj->WindowVerticalSize());
        }
        // Its clipping plane: the normal toward the viewer, what lies in
        // front cut away (FTC-11's section view). One plane is what a view
        // has; a file's others are said to be left out
        NCollection_Sequence<TDF_Label> planes;
        Handle(XCAFDoc_ClippingPlaneTool) clipping =
            XCAFDoc_DocumentTool::ClippingPlaneTool(_info.hDoc->Main());
        if (!clipping.IsNull() && tool->GetRefClippingPlaneLabel(l, planes)
            && !planes.IsEmpty()) {
            gp_Pln pln;
            Handle(TCollection_HAsciiString) planeName;
            bool capping = false;
            if (clipping->GetClippingPlane(planes.First(), pln, planeName, capping)) {
                const gp_Ax3& a = pln.Position();
                gp_Dir cz = a.Direction();
                gp_Dir cx = a.XDirection();
                gp_Dir cy = cz.Crossed(cx);
                Base::Placement cut(vec(a.Location().XYZ()),
                                    Base::Rotation::makeRotationByAxes(vec(cx.XYZ()),
                                                                       vec(cy.XYZ()),
                                                                       vec(cz.XYZ()),
                                                                       "ZXY"));
                v->ClipPlane.setValue(part->Placement.getValue() * cut);
                v->Clip.setValue(true);
            }
            if (planes.Length() > 1) {
                skip(l, "view", "its clipping planes after the first");
            }
        }
        // The shapes it shows (sec 6.3, Objects): the objects made of them
        NCollection_Sequence<TDF_Label> shapes;
        std::vector<App::DocumentObject*> shown;
        if (tool->GetRefShapeLabel(l, shapes)) {
            for (auto& sl : shapes) {
                auto o = objectOf(sl);
                if (o && std::find(shown.begin(), shown.end(), o) == shown.end()) {
                    shown.push_back(o);
                }
            }
        }
        // Only a list naming a solid: one naming construction curves alone
        // (STC-09's) would hide the part itself. A list naming every solid
        // the file has shows all there is: left empty, which says so
        auto solids = solidObjects();
        bool all = !solids.empty()
            && std::all_of(solids.begin(), solids.end(), [&shown](Part::Feature* f) {
                   return std::find(shown.begin(), shown.end(), f) != shown.end();
               });
        if (!all && std::any_of(shown.begin(), shown.end(), [this](App::DocumentObject* o) {
                return hasSolid(static_cast<Part::Feature*>(o));
            })) {
            v->Objects.setValues(shown);
        }
        for (auto c : members) {
            auto views = c->Views.getValues();
            views.push_back(v);
            c->Views.setValues(views);
        }
        ++_report.views;
    }

public:
    /// The dimensions with the value the file gave: once recomputed, the
    /// model's measure is compared with it
    std::vector<std::pair<Dimension*, double>> _values;
    /// The smallest frame the file draws: one row, no leader
    double _frameHeight = 0;
    std::vector<Callout*> _made;
    std::vector<std::pair<Callout*, TopoDS_Shape>> _graphics;
    std::map<Callout*, std::string> _graphicTypes;
    Part::Feature* _lastObject = nullptr;
    std::vector<Pending> _pending;

private:
    const Import::PostImport& _info;
    Handle(XCAFDoc_ShapeTool) _shapes;
    Handle(XCAFDoc_DimTolTool) _tool;
    std::map<Part::Feature*, Part::TopoShape> _bases;
    std::map<std::string, Callout*> _keyed, _datums, _byLabel;
    std::multimap<std::string, Callout*> _byLetter;
    /// A datum label that only a frame cites: its letter
    std::map<std::string, std::string> _letterOf;
    std::set<std::string> _cited, _read;
    std::map<std::string, int> _targets;
    std::map<std::string, std::vector<DatumTarget*>> _targetsOf;
    /// How far from its face a target's centre may be: a file's rounding
    static constexpr double TargetReach = 1e-3;
    Step::Report _report;
};

}  // namespace

Step::Report Step::read(const Import::PostImport& info)
{
    Reader reader(info);
    Report report = reader.run();
    // The file's text size, from its frames (ASME: a frame is twice the
    // text height): a part a metre long is not lettered at 3.5
    if (reader._frameHeight > 0) {
        for (auto c : reader._made) {
            c->TextHeight.setValue(reader._frameHeight / 2);
        }
    }
    // Drawn as the file draws them, until edited; set last, as any edit
    // turns it off
    for (auto& [c, shape] : reader._graphics) {
        c->ImportedGraphic.setValue(shape);
        c->ImportedGraphicType.setValue(reader._graphicTypes[c]);
        c->UseImported.setValue(true);
    }
    if (report.datums + report.tolerances + report.dimensions == 0) {
        for (auto& s : report.skipped) {
            FC_WARN("PMI: " << s);
        }
        return report;
    }
    info.doc->recompute();
    // The nominal is the model's: one that is not what the file says is said
    for (auto& [dim, value] : reader._values) {
        double nominal = dim->Nominal.getValue();
        if (std::abs(nominal - value) > 1e-4 * std::max(1.0, std::abs(value))) {
            report.mismatched.push_back(std::string(dim->getNameInDocument())
                                     + ": the file says " + std::to_string(value)
                                     + ", the model measures " + std::to_string(nominal)
                                     + " (kept)");
        }
    }
    FC_MSG("PMI: read " << report.datums << " datums, " << report.tolerances << " tolerances, "
                        << report.dimensions << " dimensions, " << report.notes
                        << " graphic-only notes, " << report.views << " views, "
                        << report.targets << " datum targets"
                        << (report.duplicates ? ", " + std::to_string(report.duplicates)
                                                    + " read twice and kept once"
                                              : std::string())
                        << (report.twice ? ", " + std::to_string(report.twice)
                                               + " graphics given twice and kept once"
                                         : std::string())
                        << (report.graphicOnly ? ", " + std::to_string(report.graphicOnly)
                                                     + " dimensions with no graphic, not read"
                                               : std::string()));
    for (auto& s : report.skipped) {
        FC_WARN("PMI: " << s);
    }
    for (auto& s : report.mismatched) {
        FC_WARN("PMI: " << s);
    }
    return report;
}

void Step::registerImport()
{
    Import::ImportOCAF2::addPostImport("PMI", [](const Import::PostImport& info) {
        Step::read(info);
    });
}
