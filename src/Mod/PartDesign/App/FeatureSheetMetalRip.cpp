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
#include <limits>
#include <numbers>
#include <set>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <App/Document.h>
#include <Base/Exception.h>
#include <Mod/Part/App/TopoShapeOpCode.h>

#include "FeatureSheetMetalRip.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"

using namespace PartDesign;
using Part::TopoShape;
using SheetMetal::CylBend;
using SheetMetal::Element;
using SheetMetal::Model;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalRip, PartDesign::FeatureAddSub)

SheetMetalRip::SheetMetalRip()
{
    SheetMetalExtension::initExtension(this);
    initAddSubType(Additive);
    ADD_PROPERTY_TYPE(Faces,
                      (nullptr),
                      "Sheet Metal",
                      App::Prop_None,
                      "A face of each bend to cut open, or of each wall to cut down its middle");
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
}

short SheetMetalRip::mustExecute() const
{
    if (Faces.isTouched()) {
        return 1;
    }
    return FeatureAddSub::mustExecute();
}

void SheetMetalRip::onChanged(const App::Property* prop)
{
    // The faces are of the feature this builds on, as a dress-up's Base is
    if (prop == &BaseFeature && BaseFeature.getValue() && Faces.getValue()
        && Faces.getValue() != BaseFeature.getValue()) {
        auto subs = Faces.getSubValues(false);
        auto shadows = Faces.getShadowSubs();
        Faces.setValue(BaseFeature.getValue(), std::move(subs), std::move(shadows));
    }
    FeatureAddSub::onChanged(prop);
}

namespace
{

double along(const gp_Pnt& p, const gp_Dir& d)
{
    return gp_Vec(p.XYZ()).Dot(gp_Vec(d));
}

/// The least and the most of the points' places along d
std::pair<double, double> extent(const std::vector<gp_Pnt>& points, const gp_Dir& d)
{
    std::pair<double, double> res {std::numeric_limits<double>::max(),
                                   -std::numeric_limits<double>::max()};
    for (const gp_Pnt& p : points) {
        res.first = std::min(res.first, along(p, d));
        res.second = std::max(res.second, along(p, d));
    }
    return res;
}

/// A polygon's face swept along v
template<class Hasher>
TopoShape prism(const std::vector<gp_Pnt>& points, const gp_Vec& v, const Hasher& hasher)
{
    BRepBuilderAPI_MakePolygon polygon;
    for (const gp_Pnt& p : points) {
        polygon.Add(p);
    }
    polygon.Close();
    BRepBuilderAPI_MakeFace face(polygon.Wire(), true);
    return TopoShape(0, hasher).makEPrism(TopoShape(face.Face()), v);
}

/// A name of the kind not taken yet in the model
std::string unused(const Model& model, const char* kind)
{
    for (int i = 1;; ++i) {
        const std::string name = kind + std::to_string(i);
        if (model.find(name) < 0) {
            return name;
        }
    }
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalRip::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    const double T = rule->Thickness.getValue();
    const double gap = rule->Gap.getValue();
    try {
        Model model = modelBefore(T);
        if (model.elements.empty()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "A rip builds on a sheet metal feature"));
        }
        TopoShape base = getBaseShape();
        const auto shadows = Faces.getShadowSubs();
        if (shadows.empty()) {
            THROWM(Base::ValueError, "A rip takes a face of each bend or wall to rip");
        }
        const auto hasher = getDocument()->getStringHasher();

        // the points of each element's faces, by the model as it was
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(base.getShape(), TopAbs_FACE, faces);
        std::vector<std::vector<gp_Pnt>> points(model.elements.size());
        for (int i = 1; i <= faces.Extent(); ++i) {
            const int owner = model.owner(TopoDS::Face(faces(i)));
            if (owner < 0) {
                continue;
            }
            for (TopExp_Explorer it(faces(i), TopAbs_VERTEX); it.More(); it.Next()) {
                points[owner].push_back(BRep_Tool::Pnt(TopoDS::Vertex(it.Current())));
            }
        }

        // what is picked, each once: walls split first, which leaves every
        // index as it is, then bends taken out by name
        std::set<int> walls;
        std::vector<std::string> bends;
        std::string labels;
        for (const auto& shadow : shadows) {
            const std::string& ref = shadow.first.empty() ? shadow.second : shadow.first;
            const TopoDS_Shape sub = base.getSubShape(ref.c_str(), true);
            if (sub.IsNull() || sub.ShapeType() != TopAbs_FACE) {
                FC_THROWM(Base::ValueError, "No face " << shadow.second);
            }
            const int owner = model.owner(TopoDS::Face(sub));
            if (owner < 0) {
                FC_THROWM(Base::ValueError, "Face " << shadow.second << " is in no wall or bend");
            }
            const Element& element = model.elements[owner];
            labels += (labels.empty() ? "" : ", ") + element.name;
            if (element.kind == Element::Kind::Wall) {
                walls.insert(owner);
            }
            else if (std::find(bends.begin(), bends.end(), element.name) == bends.end()) {
                bends.push_back(element.name);
            }
        }

        std::vector<TopoShape> cuts, legs;
        for (int index : walls) {
            const std::string name = model.elements[index].name;
            // along a bend of the wall's
            int joined = -1;
            for (std::size_t k = 0; k < model.elements.size() && joined < 0; ++k) {
                const Element& bend = model.elements[k];
                if (bend.kind == Element::Kind::Bend
                    && (bend.before == index || bend.after == index)) {
                    joined = static_cast<int>(k);
                }
            }
            if (joined < 0 || points[index].empty()) {
                FC_THROWM(Base::ValueError, name << " has no bend to be ripped along");
            }
            const gp_Ax3 frame = model.elements[index].frame;
            const gp_Dir up = frame.Direction();
            const gp_Dir axis = model.elements[joined].bend.frame.Direction();
            const gp_Dir across = up.Crossed(axis);
            // the middle of the wall across, and its length along
            const auto [lo, hi] = extent(points[index], across);
            const auto [first, last] = extent(points[index], axis);
            const double middle = (lo + hi) / 2;
            if (hi - lo < gap + Precision::Confusion()) {
                FC_THROWM(Base::ValueError, name << " is narrower than the rip's gap");
            }
            // the gap, through the wall and a thickness either side
            const gp_Pnt o = frame.Location();
            const double from = middle - gap / 2 - along(o, across);
            const gp_Pnt corner = o.Translated(gp_Vec(up) * -T + gp_Vec(across) * from
                                               + gp_Vec(axis) * (first - 1 - along(o, axis)));
            cuts.push_back(prism({corner,
                                  corner.Translated(gp_Vec(across) * gap),
                                  corner.Translated(gp_Vec(across) * gap + gp_Vec(up) * 3 * T),
                                  corner.Translated(gp_Vec(up) * 3 * T)},
                                 gp_Vec(axis) * (last - first + 2),
                                 hasher));
            // two walls, each its side of the gap; the bends follow theirs
            const gp_Pnt mid = o.Translated(gp_Vec(across) * (middle - along(o, across)));
            Element other = model.elements[index];
            model.elements[index].limits.emplace_back(
                mid.Translated(gp_Vec(across) * -(gap / 2)), across);
            other.limits.emplace_back(mid.Translated(gp_Vec(across) * (gap / 2)),
                                      across.Reversed());
            const int split = model.addWall(unused(model, "Wall"), other.frame);
            model.elements[split].limits = other.limits;
            for (Element& bend : model.elements) {
                if (bend.kind != Element::Kind::Bend
                    || along(bend.bend.frame.Location(), across) < middle) {
                    continue;
                }
                if (bend.before == index) {
                    bend.before = split;
                }
                if (bend.after == index) {
                    bend.after = split;
                }
            }
        }

        for (const std::string& name : bends) {
            const int index = model.find(name);
            const CylBend b = model.elements[index].bend;
            const int before = model.elements[index].before;
            const int after = model.elements[index].after;
            const std::vector<gp_Pnt>& at = points[index];
            if (b.angle > std::numbers::pi - 1e-3) {
                FC_THROWM(Base::ValueError, name << " turns back on itself: it is not ripped");
            }
            if (at.empty()) {
                FC_THROWM(Base::ValueError, name << " has no faces");
            }
            const gp_Pnt o = b.frame.Location();
            const gp_Dir z = b.frame.Direction();
            const gp_Dir x = b.frame.XDirection();
            const gp_Dir y = b.frame.YDirection();
            const gp_Dir x2 = x.Rotated(gp_Ax1(o, z), b.angle);
            const gp_Dir y2 = z.Crossed(x2);
            // the plane halving the corner, through the axis; the wall before
            // the bend against its normal
            gp_Dir m = z.Crossed(gp_Dir(gp_Vec(x) + gp_Vec(x2)));
            if (m.Dot(y) < 0) {
                m.Reverse();
            }
            auto [z0, z1] = extent(at, z);
            z0 -= along(o, z);
            z1 -= along(o, z);
            const double R = b.radius;
            const gp_Pnt base0 = o.Translated(gp_Vec(z) * z0);
            const gp_Vec length = gp_Vec(z) * (z1 - z0);
            // each wall on from where the bend was, to its side of the gap
            auto leg = [&](const gp_Dir& out, const gp_Dir& way, double side) {
                auto to = [&](double r) {
                    return (side * gap / 2 - r * m.Dot(out)) / m.Dot(way);
                };
                return prism({base0.Translated(gp_Vec(out) * R),
                              base0.Translated(gp_Vec(out) * R + gp_Vec(way) * to(R)),
                              base0.Translated(gp_Vec(out) * (R + T) + gp_Vec(way) * to(R + T)),
                              base0.Translated(gp_Vec(out) * (R + T))},
                             length,
                             hasher);
            };
            legs.push_back(leg(x, y, -1));
            legs.push_back(leg(x2, y2, 1));
            // the bend's own material
            const gp_Ax2 ax(base0, z, x);
            TopoDS_Shape outer = BRepPrimAPI_MakeCylinder(ax, R + T, z1 - z0, b.angle).Shape();
            TopoDS_Shape inner = BRepPrimAPI_MakeCylinder(ax, R, z1 - z0, b.angle).Shape();
            cuts.push_back(TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                            {TopoShape(outer), TopoShape(inner)}));

            model.elements[before].limits.emplace_back(o.Translated(gp_Vec(m) * -(gap / 2)), m);
            model.elements[after].limits.emplace_back(o.Translated(gp_Vec(m) * (gap / 2)),
                                                      m.Reversed());
            model.remove(index);
        }

        const int off = model.cutOff();
        if (off >= 0) {
            FC_THROWM(Base::ValueError,
                      "Ripping " << labels << " cuts " << model.elements[off].name
                                 << " off the sheet: a rip opens a loop of bends");
        }

        for (const TopoShape& c : cuts) {
            SheetMetal::storePCurves(c);
        }
        base = TopoShape(0, hasher).makEBoolean(Part::OpCodes::Cut,
                                                {base, TopoShape().makECompound(cuts)});
        base = getSolid(base);
        TopoShape result = base;
        if (!legs.empty()) {
            TopoShape tool = legs.size() == 1 ? legs.front() : TopoShape(0, hasher).makEFuse(legs);
            tool.fixSolidOrientation();
            SheetMetal::storePCurves(tool);
            AddSubShape.setValue(tool);
            if (isRecomputePaused()) {
                return App::DocumentObject::StdReturn;
            }
            result = makeBoolean(base, tool, true);
        }
        else {
            AddSubShape.setValue(TopoShape().makECompound(cuts));
            if (isRecomputePaused()) {
                return App::DocumentObject::StdReturn;
            }
        }
        Shape.setValue(result);
        SheetModel.setValue(model);
    }
    catch (const Base::Exception& e) {
        return new App::DocumentObjectExecReturn(e.what());
    }
    catch (const Standard_Failure& e) {
        return new App::DocumentObjectExecReturn(e.GetMessageString());
    }
    return App::DocumentObject::StdReturn;
}
