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

#include <cmath>
#include <numbers>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>

#include <App/Document.h>
#include <Base/Exception.h>

#include "FeatureSheetMetalLoftedFlange.h"
#include "FeatureSheetMetalRule.h"
#include "SheetMetal/Pieces.h"

using namespace PartDesign;
using Part::TopoShape;

PROPERTY_SOURCE_WITH_EXTENSIONS(PartDesign::SheetMetalLoftedFlange, PartDesign::ProfileBased)

const char* SheetMetalLoftedFlange::MaterialSideEnums[] = {"Outside", "Inside", "Middle", nullptr};

SheetMetalLoftedFlange::SheetMetalLoftedFlange()
{
    SheetMetalExtension::initExtension(this);
    ADD_PROPERTY_TYPE(Section, (nullptr), "Sheet Metal", App::Prop_None,
                      "The other outline the sheet runs to");
    ADD_PROPERTY_TYPE(MaterialSide,
                      (0L),
                      "Sheet Metal",
                      App::Prop_None,
                      "Where the material is against the sketches: Outside, the sketches on the "
                      "inner skin, the one facing the axis; Inside, on the outer skin; Middle");
    MaterialSide.setEnums(MaterialSideEnums);
    // A refined result would merge faces of two elements
    Refine.setValue(false);
    Refine.setStatus(App::Property::Hidden, true);
    for (App::Property* prop : std::initializer_list<App::Property*> {&Reversed,
                                                                      &Midplane,
                                                                      &UpToFace,
                                                                      &UpToShape,
                                                                      &AllowMultiFace,
                                                                      &Fit,
                                                                      &FitJoin,
                                                                      &InnerFit,
                                                                      &InnerFitJoin,
                                                                      &Linearize}) {
        prop->setStatus(App::Property::Hidden, true);
    }
}

short SheetMetalLoftedFlange::mustExecute() const
{
    if (Section.isTouched() || MaterialSide.isTouched()) {
        return 1;
    }
    return ProfileBased::mustExecute();
}

namespace
{

constexpr double angular = 1e-7;

/// A sketch's one circle or arc
struct Outline
{
    gp_Circ circle;
    bool closed = false;
    /// an arc's ends, in the order it turns about the circle's axis
    gp_Pnt start, end;
    double sweep = 0;
};

Outline roundOf(const TopoShape& shape, const char* what)
{
    const auto edges = shape.getSubTopoShapes(TopAbs_EDGE);
    if (edges.size() != 1) {
        FC_THROWM(Base::ValueError,
                  "A lofted flange takes one circle or arc in each sketch for now: the " << what
                      << " has " << edges.size() << " edges");
    }
    const TopoDS_Edge& edge = TopoDS::Edge(edges.front().getShape());
    BRepAdaptor_Curve c(edge);
    if (c.GetType() != GeomAbs_Circle) {
        FC_THROWM(Base::ValueError,
                  "A lofted flange takes one circle or arc in each sketch for now: the " << what
                      << " is not round");
    }
    Outline res;
    res.circle = c.Circle();
    const double f = c.FirstParameter(), l = c.LastParameter();
    res.sweep = l - f;
    res.closed = BRep_Tool::IsClosed(edge) || res.sweep >= 2 * std::numbers::pi - angular;
    res.start = c.Value(f);
    res.end = c.Value(l);
    return res;
}

/// The angle of a point about the axis, from x
double angleOf(const gp_Ax3& frame, const gp_Pnt& point)
{
    const gp_Vec v(frame.Location(), point);
    return std::atan2(v.Dot(gp_Vec(frame.YDirection())), v.Dot(gp_Vec(frame.XDirection())));
}

/// a's way, made square to n
gp_Dir across(const gp_Vec& a, const gp_Dir& n)
{
    return gp_Dir(a - gp_Vec(n) * a.Dot(gp_Vec(n)));
}

}  // namespace

App::DocumentObjectExecReturn* SheetMetalLoftedFlange::execute()
{
    auto rule = getRule();
    if (!rule) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no rule"));
    }
    const double thickness = rule->Thickness.getValue();
    if (thickness < Precision::Confusion()) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The sheet has no thickness"));
    }
    if (!Section.getValue()) {
        return new App::DocumentObjectExecReturn(
            QT_TRANSLATE_NOOP("Exception", "The lofted flange has no section to run to"));
    }
    try {
        SheetMetal::Model model = modelBefore(thickness);
        model.thickness = thickness;
        int bends = 0;
        for (const auto& e : model.elements) {
            bends += e.kind != SheetMetal::Element::Kind::Wall;
        }

        TopoShape base = getBaseShape(/*silent*/ true);
        auto invObjLoc = positionByPrevious();
        base.move(invObjLoc);
        TopoShape profile = getProfileShape();
        profile.move(invObjLoc);
        TopoShape section = Part::Feature::getTopoShape(Section.getValue());
        if (section.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "The lofted flange's section has no shape"));
        }
        section.move(invObjLoc);

        Outline a = roundOf(profile, "profile");
        Outline b = roundOf(section, "section");
        const double tol = Precision::Confusion();
        if (!a.circle.Axis().IsParallel(b.circle.Axis(), angular)) {
            THROWM(Base::ValueError, "The lofted flange's two outlines are not on parallel planes");
        }
        if (gp_Lin(a.circle.Axis()).Distance(b.circle.Location()) > tol) {
            THROWM(Base::ValueError, "The lofted flange's two outlines are not about one axis");
        }
        if (a.closed != b.closed) {
            THROWM(Base::ValueError,
                   "The lofted flange's two outlines are one a circle, the other an arc");
        }
        // from the narrow end to the wide; the same size, the profile to the section
        if (b.circle.Radius() < a.circle.Radius() - tol) {
            std::swap(a, b);
        }
        const gp_Vec rise(a.circle.Location(), b.circle.Location());
        const double height = rise.Magnitude();
        if (height < tol) {
            THROWM(Base::ValueError, "The lofted flange's two outlines are on one plane");
        }
        const gp_Dir axis(rise);
        const double rn = a.circle.Radius(), rw = b.circle.Radius();
        const double slant = std::hypot(rw - rn, height);
        const double semiAngle = std::atan2(rw - rn, height);

        // where the sheet starts about the axis, and how far it turns
        gp_Dir start;
        double sweep;
        if (a.closed) {
            const double gap = rule->Gap.getValue();
            if (gap < tol) {
                THROWM(Base::ValueError,
                       "A lofted flange all round needs the rule's Gap for its seam");
            }
            // the seam the gap wide on the narrow circle, about its X
            const double turn = gap / rn;
            start = across(gp_Vec(a.circle.XAxis().Direction()), axis)
                        .Rotated(gp_Ax1(a.circle.Location(), axis), turn / 2);
            sweep = 2 * std::numbers::pi - turn;
        }
        else {
            // each arc from where it starts turning about the axis
            for (Outline* r : {&a, &b}) {
                if (r->circle.Axis().Direction().Dot(axis) < 0) {
                    std::swap(r->start, r->end);
                }
            }
            start = across(gp_Vec(a.circle.Location(), a.start), axis);
            const gp_Ax3 frame(a.circle.Location(), axis, start);
            if (std::abs(a.sweep - b.sweep) > angular
                || std::abs(angleOf(frame, b.start.Translated(-rise))) > angular) {
                THROWM(Base::ValueError,
                       "The lofted flange's two arcs do not turn through the same angle");
            }
            sweep = a.sweep;
        }

        // the inner skin the material's side less of the sketches' line
        const int side = MaterialSide.getValue();
        const double off = side == 1 ? thickness : side == 2 ? thickness / 2 : 0;
        SheetMetal::ConeBend cone;
        cone.frame = gp_Ax3(a.circle.Location().Translated(gp_Vec(axis) * (off * std::sin(semiAngle))),
                            axis,
                            start);
        cone.radius = rn - off * std::cos(semiAngle);
        cone.semiAngle = semiAngle;
        cone.thickness = thickness;
        cone.angle = sweep;
        if (cone.radius < tol) {
            THROWM(Base::ValueError,
                   "The lofted flange's narrow end is too small for the sheet's thickness");
        }
        // K at the skin's curve across the cone's line, halfway along
        const double curve =
            (cone.radius + slant / 2 * std::sin(semiAngle)) / std::cos(semiAngle);
        cone.k = rule->modelK(sweep, curve);

        const std::string name = "Bend" + std::to_string(bends + 1);
        const auto hasher = getDocument()->getStringHasher();
        TopoShape tool = TopoShape(0, hasher).makERevolve(
            SheetMetal::coneSection(cone, slant, name),
            gp_Ax1(cone.frame.Location(), axis),
            sweep);
        if (tool.isNull()) {
            return new App::DocumentObjectExecReturn(
                QT_TRANSLATE_NOOP("Exception", "The sheet came out empty"));
        }
        model.addCone(name, cone);

        // oriented, so the join need not check its result
        tool.fixSolidOrientation();
        SheetMetal::storePCurves(tool);
        AddSubShape.setValue(tool);
        if (isRecomputePaused()) {
            return App::DocumentObject::StdReturn;
        }
        Shape.setValue(makeBoolean(base, tool, true));
        applyBendOverrides(model);
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
