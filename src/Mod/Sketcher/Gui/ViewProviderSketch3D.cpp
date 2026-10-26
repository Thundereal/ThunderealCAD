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

#include <QLabel>
#include <QVBoxLayout>

#include <Geom_Curve.hxx>
#include <Precision.hxx>

#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMarkerSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoMaterialBinding.h>
#include <Inventor/nodes/SoSeparator.h>

#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/Document.h>
#include <Gui/Inventor/MarkerBitmaps.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>
#include <Gui/ViewParams.h>
#include <Gui/ViewerContext.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Sketcher/App/GeometryFacade.h>
#include <Mod/Sketcher/App/Sketch3DObject.h>
#include <Mod/Sketcher/App/SketcherParams.h>

#include "ViewProviderSketch3D.h"
#include "Workbench.h"

FC_LOG_LEVEL_INIT("Sketch3D", true, true)

using namespace SketcherGui;

PROPERTY_SOURCE(SketcherGui::ViewProviderSketch3D, PartGui::ViewProviderPart)

namespace
{

/// The edit's task panel: the solver's state, and the way out.
class TaskDlgSketch3D: public Gui::TaskView::TaskDialog
{
public:
    explicit TaskDlgSketch3D(ViewProviderSketch3D* vp)
        : vp(vp)
    {
        auto box = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("Sketcher_NewSketch"),
                                              QObject::tr("3D sketch"),
                                              true,
                                              nullptr);
        auto widget = new QWidget();
        auto layout = new QVBoxLayout(widget);
        status = new QLabel(widget);
        status->setWordWrap(true);
        layout->addWidget(status);
        box->groupLayout()->addWidget(widget);
        Content.push_back(box);
    }

    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Close;
    }

    bool reject() override
    {
        Gui::Command::doCommand(Gui::Command::Gui, "Gui.resetEdit()");
        return true;
    }

    void setStatus(const QString& text)
    {
        status->setText(text);
    }

    ViewProviderSketch3D* vp;
    QLabel* status;
};

QString indexText(const std::vector<int>& v)
{
    QStringList list;
    for (int i : v) {
        list << QString::number(i + 1);
    }
    return list.join(QStringLiteral(", "));
}

// The 2D sketch's edit colours, from the same preferences (docs/Sketch3D.md, h: the
// display is the 2D sketch's).
SbColor editColor(const char* key, unsigned long def)
{
    SbColor color;
    float transparency;
    auto grp =
        App::GetApplication().GetParameterGroupByPath("User parameter:BaseApp/Preferences/View");
    color.setPackedValue(uint32_t(grp->GetUnsigned(key, def)), transparency);
    return color;
}

// A curve as a polyline, in the sketch's frame.
void sample(const Part::Geometry* geo, std::vector<SbVec3f>& pts)
{
    auto curve = Handle(Geom_Curve)::DownCast(geo->handle());
    if (curve.IsNull()) {
        return;
    }
    double u0 = curve->FirstParameter(), u1 = curve->LastParameter();
    int n = 1;
    if (!geo->isDerivedFrom<Part::GeomLineSegment>()) {
        if (Precision::IsInfinite(u0) || Precision::IsInfinite(u1)) {
            return;
        }
        n = geo->isDerivedFrom<Part::GeomBSplineCurve>() ? 96 : 64;
    }
    for (int i = 0; i <= n; ++i) {
        gp_Pnt p = curve->Value(u0 + (u1 - u0) * i / n);
        pts.emplace_back(float(p.X()), float(p.Y()), float(p.Z()));
    }
}

}  // namespace

struct ViewProviderSketch3D::EditData
{
    SoSeparator* content {nullptr};  // what the edit draws, in the sketch's frame
    SoMaterial* curveMaterial {nullptr};
    SoDrawStyle* curveStyle {nullptr};
    SoCoordinate3* curveCoords {nullptr};
    SoLineSet* curveLines {nullptr};
    SoMaterial* pointMaterial {nullptr};
    SoDrawStyle* pointStyle {nullptr};
    SoCoordinate3* pointCoords {nullptr};
    SoMarkerSet* points {nullptr};
    Gui::ViewerContext* viewer {nullptr};
    TaskDlgSketch3D* dialog {nullptr};

    EditData()
    {
        content = new SoSeparator();
        content->ref();
        content->setName("Sketch3DEdit");

        auto curves = new SoSeparator();
        curveMaterial = new SoMaterial();
        auto binding = new SoMaterialBinding();
        binding->value = SoMaterialBinding::PER_PART;
        curveStyle = new SoDrawStyle();
        curveCoords = new SoCoordinate3();
        curveLines = new SoLineSet();
        curves->addChild(binding);
        curves->addChild(curveMaterial);
        curves->addChild(curveStyle);
        curves->addChild(curveCoords);
        curves->addChild(curveLines);
        content->addChild(curves);

        auto pts = new SoSeparator();
        pointMaterial = new SoMaterial();
        auto pbinding = new SoMaterialBinding();
        pbinding->value = SoMaterialBinding::PER_VERTEX;
        pointStyle = new SoDrawStyle();
        pointCoords = new SoCoordinate3();
        points = new SoMarkerSet();
        pts->addChild(pbinding);
        pts->addChild(pointMaterial);
        pts->addChild(pointStyle);
        pts->addChild(pointCoords);
        pts->addChild(points);
        content->addChild(pts);
    }

    ~EditData()
    {
        content->unref();
    }
};

ViewProviderSketch3D::ViewProviderSketch3D()
{
    ADD_PROPERTY_TYPE(EditingWorkbench,
                      ("SketcherWorkbench"),
                      "Visibility automation",
                      App::Prop_None,
                      "The workbench an edit of the 3D sketch switches to; empty for none");
    ADD_PROPERTY_TYPE(HideDependent,
                      (true),
                      "Visibility automation",
                      App::Prop_None,
                      "Hide what depends on the 3D sketch while it is edited");
    ADD_PROPERTY_TYPE(
        TempoVis,
        (Py::None()),
        "Visibility automation",
        App::PropertyType(App::Prop_ReadOnly | App::Prop_Hidden | App::Prop_Transient),
        "The visibility automation's state during an edit");
    sPixmap = "Sketcher_Sketch";
}

ViewProviderSketch3D::~ViewProviderSketch3D() = default;

Sketcher::Sketch3DObject* ViewProviderSketch3D::getSketch3DObject() const
{
    return static_cast<Sketcher::Sketch3DObject*>(pcObject);
}

bool ViewProviderSketch3D::isEditingSketch() const
{
    return edit != nullptr;
}

SoSeparator* ViewProviderSketch3D::getEditRoot() const
{
    return edit ? edit->content : nullptr;
}

bool SketcherGui::isSketch3DInEdit(Gui::Document* doc)
{
    auto vp = doc ? Base::freecad_dynamic_cast<ViewProviderSketch3D>(doc->getInEdit()) : nullptr;
    return vp && vp->isEditingSketch();
}

bool ViewProviderSketch3D::doubleClicked()
{
    Gui::Application::Instance->activeDocument()->setEdit(this);
    return true;
}

void ViewProviderSketch3D::updateData(const App::Property* prop)
{
    PartGui::ViewProviderPart::updateData(prop);
    if (edit) {
        auto sketch = getSketch3DObject();
        if (prop == &sketch->Geometry || prop == &sketch->Constraints
            || prop == &sketch->ExternalGeo) {
            draw();
        }
    }
}

bool ViewProviderSketch3D::setEdit(int ModNum)
{
    if (ModNum != Default) {
        return PartGui::ViewProviderPart::setEdit(ModNum);
    }
    Gui::TaskView::TaskDialog* dlg = Gui::Control().activeDialog();
    if (dlg && !dlg->tryClose()) {
        return false;
    }

    Gui::Selection().clearSelection();
    Gui::Selection().rmvPreselect();

    edit = std::make_unique<EditData>();

    // Visibility automation, as the 2D sketch does it (TempoVis restores it on leave).
    try {
        Gui::Command::addModule(Gui::Command::Gui, "Show");
        QString cmd =
            QStringLiteral(
                "ActiveSketch = App.getDocument('%1').getObject('%2')\n"
                "tv = Show.TempoVis(App.ActiveDocument, tag=ActiveSketch.ViewObject.TypeId)\n"
                "ActiveSketch.ViewObject.TempoVis = tv\n"
                "if ActiveSketch.ViewObject.EditingWorkbench:\n"
                "  tv.activateWorkbench(ActiveSketch.ViewObject.EditingWorkbench)\n"
                "if ActiveSketch.ViewObject.HideDependent:\n"
                "  tv.hide([o for o in ActiveSketch.InList])\n"
                "%3"
                "del(tv)\n")
                .arg(QString::fromUtf8(getDocument()->getDocument()->getName()),
                     QString::fromUtf8(getObject()->getNameInDocument()),
                     Gui::ViewParams::isUsingRenderer()
                         ? QString()
                         : QStringLiteral("tv.hide(ActiveSketch)\n"));
        Gui::Command::runCommand(Gui::Command::Gui, cmd.toUtf8());
    }
    catch (Base::PyException& e) {
        e.ReportException();
    }

    edit->dialog = new TaskDlgSketch3D(this);
    Gui::Control().showDialog(edit->dialog);

    Workbench::enterEditMode();
    draw();
    return true;
}

void ViewProviderSketch3D::unsetEdit(int ModNum)
{
    if (ModNum != Default) {
        PartGui::ViewProviderPart::unsetEdit(ModNum);
        return;
    }
    Workbench::leaveEditMode();

    if (edit) {
        int index = pcRoot->findChild(edit->content);
        if (index >= 0) {
            pcRoot->removeChild(index);
        }
        edit.reset();
        if (getSketch3DObject()->isTouched()) {
            App::AutoTransaction trans("3D sketch recompute");
            try {
                Gui::Command::updateActive();
            }
            catch (...) {
            }
        }
    }
    Gui::Selection().clearSelection();
    Gui::Control().closeDialog();

    try {
        QString cmd = QStringLiteral("ActiveSketch = App.getDocument('%1').getObject('%2')\n"
                                     "tv = ActiveSketch.ViewObject.TempoVis\n"
                                     "if tv:\n"
                                     "  tv.restore()\n"
                                     "ActiveSketch.ViewObject.TempoVis = None\n"
                                     "del(tv)\n")
                          .arg(QString::fromUtf8(getDocument()->getDocument()->getName()),
                               QString::fromUtf8(getObject()->getNameInDocument()));
        Gui::Command::runCommand(Gui::Command::Gui, cmd.toUtf8());
    }
    catch (Base::PyException& e) {
        e.ReportException();
    }
}

void ViewProviderSketch3D::setEditViewer(Gui::ViewerContext* viewer, int ModNum)
{
    if (ModNum != Default || !edit) {
        PartGui::ViewProviderPart::setEditViewer(viewer, ModNum);
        return;
    }
    // As the 2D sketch: in mode 3 the edit draws through the node handed to the
    // editing root and the sketch stays hidden per view; otherwise the children move
    // under the editing root, this content with them.
    int index = pcRoot->findChild(edit->content);
    if (index >= 0) {
        pcRoot->removeChild(index);
    }
    if (viewer->hideEditedObject()) {
        viewer->setupEditingRoot(edit->content);
    }
    else {
        pcRoot->addChild(edit->content);
        viewer->setupEditingRoot();
    }
    viewer->setEditing(true);
    edit->viewer = viewer;
    draw();
    PartGui::ViewProviderPart::setEditViewer(viewer, ModNum);
    viewer->setFocusToView();
}

void ViewProviderSketch3D::unsetEditViewer(Gui::ViewerContext* viewer)
{
    if (edit) {
        viewer->setEditing(false);
        edit->viewer = nullptr;
    }
    PartGui::ViewProviderPart::unsetEditViewer(viewer);
}

void ViewProviderSketch3D::draw()
{
    if (!edit) {
        return;
    }
    auto sketch = getSketch3DObject();
    const bool fully = sketch->getDoF() == 0;
    float scale = edit->viewer ? float(edit->viewer->devicePixelRatio()) : 1.f;

    std::vector<SbVec3f> coords;
    std::vector<int32_t> counts;
    std::vector<SbColor> colors;
    std::vector<SbVec3f> pts;
    std::vector<SbColor> ptColors;

    auto addCurve = [&](const Part::Geometry* geo, const SbColor& color) {
        std::size_t before = coords.size();
        sample(geo, coords);
        if (coords.size() > before) {
            counts.push_back(int32_t(coords.size() - before));
            colors.push_back(color);
        }
    };
    auto addPoint = [&](const Base::Vector3d& v, const SbColor& color) {
        pts.emplace_back(float(v.x), float(v.y), float(v.z));
        ptColors.push_back(color);
    };

    using Params = Sketcher::SketcherParams;
    const SbColor edgeColor = editColor("EditedEdgeColor", Params::defaultEditedEdgeColor());
    const SbColor constructionColor =
        editColor("ConstructionColor", Params::defaultConstructionColor());
    const SbColor externalColor = editColor("ExternalColor", Params::defaultExternalColor());
    const SbColor fullyColor =
        editColor("FullyConstrainedColor", Params::defaultFullyConstrainedColor());
    const SbColor fullyElementColor =
        editColor("FullyConstraintElementColor", Params::defaultFullyConstraintElementColor());
    const SbColor fullyConstructionColor =
        editColor("FullyConstraintConstructionElementColor",
                  Params::defaultFullyConstraintConstructionElementColor());
    const SbColor vertexColor = editColor("SketchVertexColor", Params::defaultSketchVertexColor());

    for (auto geo : sketch->Geometry.getValues()) {
        if (geo->isDerivedFrom<Part::GeomPlane>()) {
            continue;  // a workplane: the manipulator shows it (section 6)
        }
        bool construction = Sketcher::GeometryFacade::getConstruction(geo);
        const SbColor& curveColor = construction
            ? (fully ? fullyConstructionColor : constructionColor)
            : (fully ? fullyColor : edgeColor);
        const SbColor& pointColor = fully ? fullyElementColor : vertexColor;
        if (auto pt = Base::freecad_dynamic_cast<Part::GeomPoint>(geo)) {
            addPoint(pt->getPoint(), pointColor);
            continue;
        }
        addCurve(geo, curveColor);
        if (auto bounded = Base::freecad_dynamic_cast<Part::GeomBoundedCurve>(geo)) {
            addPoint(bounded->getStartPoint(), pointColor);
            addPoint(bounded->getEndPoint(), pointColor);
        }
        if (auto conic = Base::freecad_dynamic_cast<Part::GeomConic>(geo)) {
            addPoint(conic->getCenter(), pointColor);
        }
        else if (auto arc = Base::freecad_dynamic_cast<Part::GeomArcOfConic>(geo)) {
            addPoint(arc->getCenter(), pointColor);
        }
    }
    for (auto geo : sketch->ExternalGeo.getValues()) {
        if (auto pt = Base::freecad_dynamic_cast<Part::GeomPoint>(geo)) {
            addPoint(pt->getPoint(), externalColor);
        }
        else if (!geo->isDerivedFrom<Part::GeomPlane>()) {
            addCurve(geo, externalColor);
        }
    }

    edit->curveCoords->point.setValues(0, int(coords.size()), coords.data());
    edit->curveCoords->point.setNum(int(coords.size()));
    edit->curveLines->numVertices.setValues(0, int(counts.size()), counts.data());
    edit->curveLines->numVertices.setNum(int(counts.size()));
    edit->curveMaterial->diffuseColor.setValues(0, int(colors.size()), colors.data());
    edit->curveMaterial->diffuseColor.setNum(int(colors.size()));
    edit->curveStyle->lineWidth = 2.f * scale;

    edit->pointCoords->point.setValues(0, int(pts.size()), pts.data());
    edit->pointCoords->point.setNum(int(pts.size()));
    edit->pointMaterial->diffuseColor.setValues(0, int(ptColors.size()), ptColors.data());
    edit->pointMaterial->diffuseColor.setNum(int(ptColors.size()));
    edit->pointStyle->pointSize = 8.f * scale;
    edit->points->markerIndex = Gui::Inventor::MarkerBitmaps::getMarkerIndex("CIRCLE_FILLED", 7);

    if (edit->dialog) {
        QString text;
        int dof = sketch->getDoF();
        if (!sketch->getMalformed().empty()) {
            text = QObject::tr("Constraints a 3D sketch cannot solve: %1")
                       .arg(indexText(sketch->getMalformed()));
        }
        else if (!sketch->getConflicting().empty()) {
            text =
                QObject::tr("Conflicting constraints: %1").arg(indexText(sketch->getConflicting()));
        }
        else if (dof == 0) {
            text = QObject::tr("Fully constrained");
        }
        else {
            text = QObject::tr("%1 degrees of freedom").arg(dof);
        }
        if (!sketch->getRedundant().empty()) {
            text += QStringLiteral("\n")
                + QObject::tr("Redundant constraints: %1").arg(indexText(sketch->getRedundant()));
        }
        edit->dialog->setStatus(text);
    }
}
