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

#ifndef _PreComp_
#include <algorithm>
#include <cmath>

#include <QAction>
#include <QMenu>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepLProp_CLProps.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSwitch.h>
#endif

#include <Gui/ActionFunction.h>
#include <Gui/ViewProviderDocumentObject.h>
#include <Mod/Part/App/PartFeature.h>

#include "ViewProviderCurvatureComb.h"


using namespace PartGui;

EXTENSION_PROPERTY_SOURCE(PartGui::ViewProviderCurvatureCombExtension, Gui::ViewProviderExtension)

namespace
{
const App::PropertyFloatConstraint::Constraints ScaleRange = {0.01, 100.0, 0.1};
const App::PropertyIntegerConstraint::Constraints DensityRange = {2, 1000, 4};
const App::PropertyIntegerConstraint::Constraints IsoRange = {0, 100, 1};

/// The teeth's tip: away from the centre of curvature by the curvature
gp_Pnt tip(const gp_Pnt& p, const gp_Vec& towardCentre, double k)
{
    return p.Translated(towardCentre * (-k));
}
}  // namespace

ViewProviderCurvatureCombExtension::ViewProviderCurvatureCombExtension()
{
    initExtensionType(ViewProviderCurvatureCombExtension::getExtensionClassTypeId());
    EXTENSION_ADD_PROPERTY_TYPE(CurvatureComb,
                                (false),
                                "Curvature comb",
                                App::Prop_None,
                                "Show the curvature comb of the edges and of isocurves of the "
                                "faces: a tooth as long as the curvature, away from its centre");
    EXTENSION_ADD_PROPERTY_TYPE(CombScale,
                                (1.0),
                                "Curvature comb",
                                App::Prop_None,
                                "How long the teeth are: at 1, the longest is a fifth of the "
                                "object's size");
    CombScale.setConstraints(&ScaleRange);
    EXTENSION_ADD_PROPERTY_TYPE(CombDensity,
                                (32),
                                "Curvature comb",
                                App::Prop_None,
                                "How many teeth along each curve");
    CombDensity.setConstraints(&DensityRange);
    EXTENSION_ADD_PROPERTY_TYPE(CombIsocurves,
                                (0),
                                "Curvature comb",
                                App::Prop_None,
                                "How many isocurves of each face, each way, get a comb too; 0, "
                                "the edges alone");
    CombIsocurves.setConstraints(&IsoRange);
}

void ViewProviderCurvatureCombExtension::extensionSetupContextMenu(QMenu* menu,
                                                                   QObject*,
                                                                   const char*)
{
    auto func = new Gui::ActionFunction(menu);
    QAction* act = menu->addAction(QObject::tr("Show curvature comb"));
    act->setCheckable(true);
    act->setChecked(CurvatureComb.getValue());
    func->toggle(act, [this](bool on) {
        CurvatureComb.setValue(on);
    });
}

void ViewProviderCurvatureCombExtension::extensionUpdateData(const App::Property* prop)
{
    Gui::ViewProviderExtension::extensionUpdateData(prop);
    if (CurvatureComb.getValue() && prop->is<Part::PropertyPartShape>()
        && strcmp(prop->getName(), "Shape") == 0) {
        rebuild();
    }
}

void ViewProviderCurvatureCombExtension::extensionOnChanged(const App::Property* prop)
{
    if (prop == &CurvatureComb || prop == &CombScale || prop == &CombDensity
        || prop == &CombIsocurves) {
        rebuild();
    }
    else {
        Gui::ViewProviderExtension::extensionOnChanged(prop);
    }
}

void ViewProviderCurvatureCombExtension::combsOf(const TopoDS_Shape& shape,
                                                 std::vector<Comb>& combs) const
{
    const int n = CombDensity.getValue();
    for (TopExp_Explorer x(shape, TopAbs_EDGE); x.More(); x.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(x.Current());
        if (BRep_Tool::Degenerated(edge)) {
            continue;
        }
        BRepAdaptor_Curve c(edge);
        Comb comb;
        for (int k = 0; k <= n; ++k) {
            double t = c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * k / n;
            BRepLProp_CLProps props(c, t, 2, Precision::Confusion());
            gp_Pnt p = props.Value();
            double kappa = props.IsTangentDefined() ? props.Curvature() : 0;
            gp_Vec toward;
            if (kappa > 1e-12) {
                gp_Pnt centre;
                props.CentreOfCurvature(centre);
                toward = gp_Vec(p, centre).Normalized();
            }
            comb.points.push_back(p);
            comb.teeth.push_back(kappa > 1e-12 ? tip(p, toward, kappa) : p);
        }
        combs.push_back(std::move(comb));
    }

    const int m = CombIsocurves.getValue();
    if (m <= 0) {
        return;
    }
    for (TopExp_Explorer x(shape, TopAbs_FACE); x.More(); x.Next()) {
        const TopoDS_Face& face = TopoDS::Face(x.Current());
        BRepAdaptor_Surface s(face);
        BRepTopAdaptor_FClass2d inside(face, Precision::Confusion());
        double u1, u2, v1, v2;
        BRepTools::UVBounds(face, u1, u2, v1, v2);
        const double sign = face.Orientation() == TopAbs_REVERSED ? -1 : 1;
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 1; i <= m; ++i) {
                double f = double(i) / (m + 1);
                Comb comb;
                auto flush = [&] {
                    if (comb.points.size() > 1) {
                        combs.push_back(comb);
                    }
                    comb = Comb();
                };
                for (int k = 0; k <= n; ++k) {
                    double g = double(k) / n;
                    double u = pass == 0 ? u1 + (u2 - u1) * f : u1 + (u2 - u1) * g;
                    double v = pass == 0 ? v1 + (v2 - v1) * g : v1 + (v2 - v1) * f;
                    if (inside.Perform(gp_Pnt2d(u, v)) == TopAbs_OUT) {
                        flush();
                        continue;
                    }
                    BRepLProp_SLProps props(s, u, v, 2, Precision::Confusion());
                    if (!props.IsNormalDefined() || !props.IsCurvatureDefined()) {
                        flush();
                        continue;
                    }
                    // the normal curvature along the iso, by Euler's formula
                    gp_Dir along(pass == 0 ? props.D1V() : props.D1U());
                    double kappa;
                    if (props.IsUmbilic()) {
                        kappa = props.MaxCurvature();
                    }
                    else {
                        gp_Dir dmax, dmin;
                        props.CurvatureDirections(dmax, dmin);
                        double cs = along.Dot(dmax), sn = along.Dot(dmin);
                        kappa = props.MaxCurvature() * cs * cs + props.MinCurvature() * sn * sn;
                    }
                    // a positive normal curvature bends toward the normal
                    gp_Vec normal(props.Normal());
                    comb.points.push_back(props.Value());
                    comb.teeth.push_back(tip(props.Value(), normal * sign, kappa * sign));
                }
                flush();
            }
        }
    }
}

void ViewProviderCurvatureCombExtension::rebuild()
{
    auto vp = getExtendedViewProvider();
    if (!vp) {
        return;
    }
    if (!pcComb) {
        if (!CurvatureComb.getValue()) {
            return;
        }
        pcComb = new SoSwitch();
        pcComb->setName("CurvatureComb");
        auto sep = new SoSeparator();
        auto pick = new SoPickStyle();
        pick->style = SoPickStyle::UNPICKABLE;
        sep->addChild(pick);
        auto light = new SoLightModel();
        light->model = SoLightModel::BASE_COLOR;
        sep->addChild(light);
        auto style = new SoDrawStyle();
        style->lineWidth = 1.0f;
        sep->addChild(style);
        auto teethColor = new SoBaseColor();
        teethColor->rgb.setValue(0.45f, 0.55f, 0.75f);
        sep->addChild(teethColor);
        teethCoords = new SoCoordinate3();
        teethCoords->setName("CombTeeth");
        sep->addChild(teethCoords);
        teethLines = new SoLineSet();
        sep->addChild(teethLines);
        auto envelopeColor = new SoBaseColor();
        envelopeColor->rgb.setValue(0.85f, 0.15f, 0.15f);
        sep->addChild(envelopeColor);
        envelopeCoords = new SoCoordinate3();
        envelopeCoords->setName("CombEnvelope");
        sep->addChild(envelopeCoords);
        envelopeLines = new SoLineSet();
        sep->addChild(envelopeLines);
        pcComb->addChild(sep);
        vp->getRoot()->addChild(pcComb);
    }
    pcComb->whichChild = CurvatureComb.getValue() ? SO_SWITCH_ALL : SO_SWITCH_NONE;
    teethCoords->point.setNum(0);
    teethLines->numVertices.setNum(0);
    envelopeCoords->point.setNum(0);
    envelopeLines->numVertices.setNum(0);
    if (!CurvatureComb.getValue()) {
        return;
    }

    auto feature = dynamic_cast<Part::Feature*>(vp->getObject());
    if (!feature) {
        return;
    }
    // as the view draws it: without the object's placement, which its root
    // applies
    TopoDS_Shape shape = feature->Shape.getShape().getShape();
    if (shape.IsNull()) {
        return;
    }
    shape.Location(TopLoc_Location());
    std::vector<Comb> combs;
    try {
        combsOf(shape, combs);
    }
    catch (Standard_Failure&) {
        return;
    }

    // scaled so that the longest tooth is a fifth of the object's size
    double longest = 0;
    for (const auto& c : combs) {
        for (std::size_t i = 0; i < c.points.size(); ++i) {
            longest = std::max(longest, c.points[i].Distance(c.teeth[i]));
        }
    }
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, false, false);
    double size = box.IsVoid() ? 1 : std::sqrt(box.SquareExtent());
    double scale = longest > 1e-12 ? CombScale.getValue() * 0.2 * size / longest : 0;

    std::vector<SbVec3f> teeth, envelope;
    std::vector<int32_t> envelopeCounts;
    for (const auto& c : combs) {
        for (std::size_t i = 0; i < c.points.size(); ++i) {
            const gp_Pnt& p = c.points[i];
            gp_Pnt q = p.Translated(gp_Vec(p, c.teeth[i]) * scale);
            teeth.emplace_back(float(p.X()), float(p.Y()), float(p.Z()));
            teeth.emplace_back(float(q.X()), float(q.Y()), float(q.Z()));
            envelope.emplace_back(float(q.X()), float(q.Y()), float(q.Z()));
        }
        envelopeCounts.push_back(int32_t(c.points.size()));
    }
    teethCoords->point.setValues(0, int(teeth.size()), teeth.data());
    std::vector<int32_t> twos(teeth.size() / 2, 2);
    teethLines->numVertices.setValues(0, int(twos.size()), twos.data());
    envelopeCoords->point.setValues(0, int(envelope.size()), envelope.data());
    envelopeLines->numVertices.setValues(0,
                                         int(envelopeCounts.size()),
                                         envelopeCounts.data());
}

namespace Gui
{
EXTENSION_PROPERTY_SOURCE_TEMPLATE(PartGui::ViewProviderCurvatureCombExtensionPython,
                                   PartGui::ViewProviderCurvatureCombExtension)

// explicit template instantiation
template class PartGuiExport
    ViewProviderExtensionPythonT<PartGui::ViewProviderCurvatureCombExtension>;
}  // namespace Gui
