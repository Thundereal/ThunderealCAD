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
#include <map>
#include <set>
#include <sstream>

#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <gp_Ax3.hxx>
#include <gp_Trsf.hxx>
#include <NCollection_Sequence.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <App/Document.h>
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
#include "Geometry.h"
#include "StepImport.h"
#include "TolerancedFeature.h"

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
    /// The first face's surface: what decides an axis
    Primitive::Type type = Primitive::None;
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
        // OCCT makes a datum label for each time a frame cites it; only the
        // ones whose feature it found carry shapes. A letter none carries
        // is a datum lost.
        for (auto& [letter, n] : _targets) {
            _report.skipped.push_back("datum " + letter + ": " + std::to_string(n)
                                      + " datum targets, not read yet");
        }
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
            skip(l, what, e.GetMessageString());
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
            bool any = false;
            for (TopExp_Explorer xp(sub, TopAbs_FACE); xp.More(); xp.Next()) {
                int idx = base.findShape(xp.Current());
                if (idx <= 0) {
                    why = "a face of " + entry(l) + " is not in its object";
                    return false;
                }
                std::string name = "Face" + std::to_string(idx);
                if (std::find(out.names.begin(), out.names.end(), name) == out.names.end()) {
                    out.names.push_back(name);
                }
                if (!any && out.names.size() == 1) {
                    out.type = Geom::ofFace(xp.Current()).type;
                }
                any = true;
            }
            if (!any) {
                why = "it refers to " + entry(l) + ", which has no face";
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
        bool axial = f.type == Primitive::Cylinder || f.type == Primitive::Cone;
        return Api::feature(set(f.object), f.object, f.names, axis && axial ? "Axis" : nullptr);
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
        auto s = set(obj);
        auto o = static_cast<T*>(_info.doc->addObject(type, name));
        s->addObject(o);
        _made.push_back(o);
        return o;
    }

    /// Kept once: a streamed import may read the same item twice
    bool seen(const std::string& key)
    {
        if (!_keys.insert(key).second) {
            ++_report.duplicates;
            return true;
        }
        return false;
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
            return;
        }
        NCollection_Sequence<TDF_Label> first, second;
        XCAFDoc_DimTolTool::GetRefShapeLabel(l, first, second);
        if (first.IsEmpty()) {
            _cited.insert(letter);
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
        if (!_datums.insert("datum " + letter + " " + key(f)).second) {
            return;
        }
        auto d = add<Datum>(f.object, "PMI::Datum", "Datum");
        d->Letter.setValue(letter);
        d->Feature.setValue(feature(f, true));
        place(d, f.object, obj->HasPlane(), obj->GetPlane(), obj->HasPointText(),
              obj->GetPointTextAttach());
        ++_report.datums;
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
        if (seen(k.str())) {
            return;
        }
        // A position, a coaxiality, or a cylindrical zone of a cylinder is
        // of its axis
        bool axis = zone == 1 || ch == GT::Position || ch == GT::Coaxiality
            || ch == GT::Concentricity;
        auto gt = add<GT>(f.object, "PMI::GeometricTolerance", "Tolerance");
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
        if (!comps.empty()) {
            gt->DatumSystem.setValue(Api::frame(set(f.object), comps));
        }
        place(gt, f.object, obj->HasPlane(), obj->GetPlane(), obj->HasPointText(),
              obj->GetPointTextAttach(), obj->GetPresentation());
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
            ++_report.graphicOnly;
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
        if (location && f2.object != f1.object) {
            return skip(l, "dimension", "it measures between two objects");
        }
        std::ostringstream k;
        k << "dimension " << type << " " << obj->GetValue() << " " << key(f1);
        if (location) {
            k << " " << key(f2);
        }
        if (seen(k.str())) {
            return;
        }
        auto dim = add<Dimension>(f1.object, "PMI::Dimension", "Dimension");
        dim->Type.setValue(type);
        std::vector<App::DocumentObject*> feats {feature(f1, location)};
        if (location) {
            feats.push_back(feature(f2, true));
        }
        dim->Features.setValues(feats);
        double value = obj->GetValue();
        if (obj->IsDimWithPlusMinusTolerance()) {
            dim->Upper.setValue(obj->GetUpperTolValue());
            // OCCT keeps the lower deviation as a magnitude: its writer negates it
            dim->Lower.setValue(-obj->GetLowerTolValue());
        }
        else if (obj->IsDimWithRange()) {
            dim->Upper.setValue(obj->GetUpperBound() - value);
            dim->Lower.setValue(obj->GetLowerBound() - value);
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
        _values.emplace_back(dim, value);
        ++_report.dimensions;
    }

public:
    /// The dimensions with the value the file gave: once recomputed, the
    /// model's measure is compared with it
    std::vector<std::pair<Dimension*, double>> _values;
    /// The smallest frame the file draws: one row, no leader
    double _frameHeight = 0;
    std::vector<Callout*> _made;

private:
    const Import::PostImport& _info;
    Handle(XCAFDoc_ShapeTool) _shapes;
    Handle(XCAFDoc_DimTolTool) _tool;
    std::map<Part::Feature*, Part::TopoShape> _bases;
    std::set<std::string> _keys;
    std::set<std::string> _cited, _read, _datums;
    std::map<std::string, int> _targets;
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
                        << report.dimensions << " dimensions"
                        << (report.duplicates ? ", " + std::to_string(report.duplicates)
                                                    + " read twice and kept once"
                                              : std::string())
                        << (report.graphicOnly ? ", " + std::to_string(report.graphicOnly)
                                                     + " dimensions only a graphic, not read yet"
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
