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

#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>

#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoShapeHints.h>

#include <Gui/ActiveObjectList.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/Inventor/So3DAnnotation.h>
#include <Gui/MDIView.h>
#include <Mod/PMI/App/AnnotationSet.h>
#include <Mod/PMI/App/Callout.h>
#include <Mod/PMI/App/Datum.h>
#include <Mod/PMI/App/Dimension.h>
#include <Mod/PMI/App/GeometricTolerance.h>
#include <Mod/PMI/App/Note.h>
#include <Mod/PMI/App/TolerancedFeature.h>

#include <QTimer>

#include <Gui/Control.h>

#include "TaskGeometricTolerance.h"
#include "ViewProviderCallout.h"

using namespace PMIGui;

// ---- ShapeLines ----

ShapeLines::ShapeLines()
{
    _root = new SoSeparator;
    _root->ref();
    auto light = new SoLightModel;
    light->model = SoLightModel::BASE_COLOR;
    _root->addChild(light);
    _material = new SoMaterial;
    _root->addChild(_material);
    _style = new SoDrawStyle;
    _style->lineWidth = 1.5f;
    _root->addChild(_style);
    auto hints = new SoShapeHints;
    hints->vertexOrdering = SoShapeHints::UNKNOWN_ORDERING;
    _root->addChild(hints);
    _lineCoords = new SoCoordinate3;
    _root->addChild(_lineCoords);
    _lines = new SoIndexedLineSet;
    _root->addChild(_lines);
    _faceCoords = new SoCoordinate3;
    _root->addChild(_faceCoords);
    _faces = new SoIndexedFaceSet;
    _root->addChild(_faces);
    setColor(0.1f, 0.25f, 0.6f);
}

ShapeLines::~ShapeLines()
{
    _root->unref();
}

void ShapeLines::setColor(float r, float g, float b)
{
    _material->diffuseColor.setValue(r, g, b);
    _material->emissiveColor.setValue(r, g, b);
}

void ShapeLines::setLineWidth(float w)
{
    _style->lineWidth = w;
}

void ShapeLines::setDashed(bool dashed)
{
    _style->linePattern = dashed ? 0xf0f0 : 0xffff;
}

void ShapeLines::set(const TopoDS_Shape& shape)
{
    std::vector<SbVec3f> lp;
    std::vector<int32_t> li;
    std::vector<SbVec3f> fp;
    std::vector<int32_t> fi;
    if (!shape.IsNull()) {
        // Strokes: the edges no face holds, each a segment (they are made
        // so, arcs included)
        for (TopExp_Explorer xp(shape, TopAbs_EDGE, TopAbs_FACE); xp.More(); xp.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(xp.Current());
            TopoDS_Vertex v1, v2;
            TopExp::Vertices(edge, v1, v2);
            if (v1.IsNull() || v2.IsNull()) {
                continue;
            }
            gp_Pnt a = BRep_Tool::Pnt(v1), b = BRep_Tool::Pnt(v2);
            int32_t n = int32_t(lp.size());
            lp.emplace_back(float(a.X()), float(a.Y()), float(a.Z()));
            lp.emplace_back(float(b.X()), float(b.Y()), float(b.Z()));
            li.push_back(n);
            li.push_back(n + 1);
            li.push_back(-1);
        }
        // Fills: planar polygons, meshed exactly; a glyph's faces are shared
        // between its copies and meshed once
        BRepMesh_IncrementalMesh(shape, 0.01, false, 0.5, false);
        for (TopExp_Explorer xp(shape, TopAbs_FACE); xp.More(); xp.Next()) {
            const TopoDS_Face& face = TopoDS::Face(xp.Current());
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) {
                continue;
            }
            gp_Trsf trsf = loc.Transformation();
            int32_t base = int32_t(fp.size());
            for (int i = 1; i <= tri->NbNodes(); ++i) {
                gp_Pnt p = tri->Node(i).Transformed(trsf);
                fp.emplace_back(float(p.X()), float(p.Y()), float(p.Z()));
            }
            for (int i = 1; i <= tri->NbTriangles(); ++i) {
                int a, b, c;
                tri->Triangle(i).Get(a, b, c);
                fi.push_back(base + a - 1);
                fi.push_back(base + b - 1);
                fi.push_back(base + c - 1);
                fi.push_back(-1);
            }
        }
    }
    _lineCoords->point.setValues(0, int(lp.size()), lp.data());
    _lineCoords->point.setNum(int(lp.size()));
    _lines->coordIndex.setValues(0, int(li.size()), li.data());
    _lines->coordIndex.setNum(int(li.size()));
    _faceCoords->point.setValues(0, int(fp.size()), fp.data());
    _faceCoords->point.setNum(int(fp.size()));
    _faces->coordIndex.setValues(0, int(fi.size()), fi.data());
    _faces->coordIndex.setNum(int(fi.size()));
}

// ---- ViewProviderCallout ----

PROPERTY_SOURCE(PMIGui::ViewProviderCallout, Gui::ViewProviderDocumentObject)

namespace
{
App::PropertyFloatConstraint::Constraints lineWidthRange = {0.5, 10.0, 0.5};
}

ViewProviderCallout::ViewProviderCallout()
{
    ADD_PROPERTY_TYPE(Color, (Base::Color(0.1f, 0.25f, 0.6f)), "PMI", App::Prop_None,
                      "Its colour while all is well");
    ADD_PROPERTY_TYPE(LineWidth, (1.5), "PMI", App::Prop_None, "The width of its strokes");
    LineWidth.setConstraints(&lineWidthRange);
}

ViewProviderCallout::~ViewProviderCallout() = default;

void ViewProviderCallout::attach(App::DocumentObject* obj)
{
    ViewProviderDocumentObject::attach(obj);
    // On top of the model, with depth among its own lines and fills. The
    // priority is what the render cache orders by; at 0 it is drawn as the
    // model is, and hidden by it (seen: a datum on the bottom face, from
    // above). 1, as the on-view labels are.
    auto top = new Gui::So3DAnnotation;
    top->priority = 1;
    top->addChild(_drawing.root());
    addDisplayMaskMode(top, "Callout");
    refresh();
}

std::vector<std::string> ViewProviderCallout::getDisplayModes() const
{
    return {"Callout"};
}

void ViewProviderCallout::setDisplayMode(const char* mode)
{
    setDisplayMaskMode("Callout");
    ViewProviderDocumentObject::setDisplayMode(mode);
}

void ViewProviderCallout::refresh()
{
    auto callout = freecad_cast<PMI::Callout*>(getObject());
    if (!callout) {
        return;
    }
    switch (callout->getState()) {
        case PMI::Annotation::Remapped:
            _drawing.setColor(0.85f, 0.55f, 0.0f);
            break;
        case PMI::Annotation::Broken:
        case PMI::Annotation::Invalid:
            _drawing.setColor(0.85f, 0.1f, 0.1f);
            break;
        default: {
            const Base::Color& c = Color.getValue();
            _drawing.setColor(c.r, c.g, c.b);
            break;
        }
    }
    _drawing.setLineWidth(float(LineWidth.getValue()));
    _drawing.set(callout->Presentation.getShape().getShape());
}

void ViewProviderCallout::updateData(const App::Property* prop)
{
    auto callout = freecad_cast<PMI::Callout*>(getObject());
    if (callout && (prop == &callout->Presentation || prop == &callout->Condition)) {
        refresh();
    }
    ViewProviderDocumentObject::updateData(prop);
}

void ViewProviderCallout::onChanged(const App::Property* prop)
{
    if (prop == &Color || prop == &LineWidth) {
        refresh();
    }
    ViewProviderDocumentObject::onChanged(prop);
}

QIcon ViewProviderCallout::getIcon() const
{
    const char* name = "PMI_Note";
    auto obj = getObject();
    if (obj && obj->isDerivedFrom<PMI::GeometricTolerance>()) {
        name = "PMI_GeometricTolerance";
    }
    else if (obj && obj->isDerivedFrom<PMI::Datum>()) {
        name = "PMI_Datum";
    }
    else if (obj && obj->isDerivedFrom<PMI::Dimension>()) {
        name = "PMI_Dimension";
    }
    return mergeOverlayIcons(Gui::BitmapFactory().pixmap(name));
}

bool ViewProviderCallout::setEdit(int ModNum)
{
    auto gt = freecad_cast<PMI::GeometricTolerance*>(getObject());
    if (ModNum != ViewProvider::Default || !gt) {
        return ViewProviderDocumentObject::setEdit(ModNum);
    }
    if (auto dlg = Gui::Control().activeDialog()) {
        if (qobject_cast<TaskDlgGeometricTolerance*>(dlg)) {
            Gui::Control().showDialog(dlg);
            return true;
        }
        return false;
    }
    Gui::Control().showDialog(new TaskDlgGeometricTolerance(gt));
    return true;
}

void ViewProviderCallout::unsetEdit(int ModNum)
{
    if (ModNum == ViewProvider::Default) {
        QTimer::singleShot(0, &Gui::Control(), qOverload<>(&Gui::ControlSingleton::closeDialog));
        return;
    }
    ViewProviderDocumentObject::unsetEdit(ModNum);
}

// ---- ViewProviderFeature ----

PROPERTY_SOURCE(PMIGui::ViewProviderFeature, Gui::ViewProviderDocumentObject)

ViewProviderFeature::ViewProviderFeature()
{
    _drawing.setColor(0.2f, 0.5f, 0.25f);
    _drawing.setDashed(true);
}

ViewProviderFeature::~ViewProviderFeature() = default;

void ViewProviderFeature::attach(App::DocumentObject* obj)
{
    ViewProviderDocumentObject::attach(obj);
    addDisplayMaskMode(_drawing.root(), "Construction");
    refresh();
}

std::vector<std::string> ViewProviderFeature::getDisplayModes() const
{
    return {"Construction"};
}

void ViewProviderFeature::setDisplayMode(const char* mode)
{
    setDisplayMaskMode("Construction");
    ViewProviderDocumentObject::setDisplayMode(mode);
}

void ViewProviderFeature::refresh()
{
    auto feat = freecad_cast<PMI::TolerancedFeature*>(getObject());
    if (!feat) {
        return;
    }
    // The faces are the part's, drawn by it; what is derived is drawn here
    bool derived = feat->getKind() == PMI::TolerancedFeature::Axis
        || feat->getKind() == PMI::TolerancedFeature::CentrePlane
        || feat->getKind() == PMI::TolerancedFeature::CentrePoint
        || feat->getKind() == PMI::TolerancedFeature::Apex;
    if (!derived) {
        _drawing.set(TopoDS_Shape());
        return;
    }
    TopoDS_Shape shape = feat->Shape.getShape().getShape();
    // A centre plane drawn as its outline
    if (!shape.IsNull() && shape.ShapeType() == TopAbs_FACE) {
        BRep_Builder builder;
        TopoDS_Compound comp;
        builder.MakeCompound(comp);
        for (TopExp_Explorer xp(shape, TopAbs_EDGE); xp.More(); xp.Next()) {
            builder.Add(comp, xp.Current());
        }
        shape = comp;
    }
    _drawing.set(shape);
}

void ViewProviderFeature::updateData(const App::Property* prop)
{
    auto feat = freecad_cast<PMI::TolerancedFeature*>(getObject());
    if (feat && (prop == &feat->Shape || prop == &feat->Kind)) {
        refresh();
    }
    ViewProviderDocumentObject::updateData(prop);
}

QIcon ViewProviderFeature::getIcon() const
{
    return mergeOverlayIcons(Gui::BitmapFactory().pixmap("PMI_Feature"));
}

// ---- ViewProviderAnnotationSet ----

PROPERTY_SOURCE(PMIGui::ViewProviderAnnotationSet, Gui::ViewProviderDocumentObjectGroup)

ViewProviderAnnotationSet::ViewProviderAnnotationSet() = default;

bool ViewProviderAnnotationSet::doubleClicked()
{
    auto doc = Gui::Application::Instance->activeDocument();
    if (!doc) {
        doc = getDocument();
    }
    auto view = doc ? doc->setActiveView(this) : nullptr;
    if (!view) {
        return false;
    }
    if (view->getActiveObject<App::DocumentObject*>(PMIKEY) == getObject()) {
        Gui::Command::doCommand(Gui::Command::Gui,
                                "Gui.ActiveDocument.ActiveView.setActiveObject('%s', None)",
                                PMIKEY);
        return true;
    }
    Gui::Command::doCommand(Gui::Command::Gui,
                            "Gui.ActiveDocument.ActiveView.setActiveObject('%s', %s)",
                            PMIKEY,
                            getObject()->getFullName(true).c_str());
    return true;
}

QIcon ViewProviderAnnotationSet::getIcon() const
{
    return mergeOverlayIcons(Gui::BitmapFactory().pixmap("PMI_AnnotationSet"));
}
