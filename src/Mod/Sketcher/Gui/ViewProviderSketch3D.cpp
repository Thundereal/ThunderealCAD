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

#include <QLabel>
#include <QVBoxLayout>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Curve.hxx>
#include <TopoDS.hxx>
#include <Precision.hxx>

#include <Inventor/SbLine.h>
#include <Inventor/SbViewVolume.h>
#include <Inventor/SoPath.h>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/SoPickedPoint.h>
#include <Inventor/events/SoMouseButtonEvent.h>
#include <Inventor/details/SoLineDetail.h>
#include <Inventor/details/SoPointDetail.h>
#include <Inventor/events/SoKeyboardEvent.h>
#include <Inventor/engines/SoComposeVec3f.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoFaceSet.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMarkerSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoMaterialBinding.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoScale.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoTransform.h>

#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Mod/Part/App/PartFeature.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/Tools.h>
#include <Gui/Application.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/Document.h>
#include <Gui/Inventor/MarkerBitmaps.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection.h>
#include <Gui/SoFCCSysDragger.h>
#include <Gui/ToolBarManager.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>
#include <Gui/ViewParams.h>
#include <Gui/ViewerContext.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Sketcher/App/GeometryFacade.h>
#include <Mod/Sketcher/App/Sketch3DObject.h>
#include <Mod/Sketcher/App/SketcherParams.h>

#include "ViewProviderSketch3D.h"
#include "ViewProviderSketch3DPy.h"
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

// The toolbar of the 3D sketch's own commands, shown only in its edit.
const QStringList& sketch3DToolbars()
{
    static const QStringList names {QStringLiteral("Sketcher 3D")};
    return names;
}

// The rotation taking the XY plane to the YZ (k = 1) or ZX (k = 2) plane: 120 deg
// steps about (1, 1, 1) cycle x -> y -> z.
Base::Rotation planeTurn(int k)
{
    return Base::Rotation(Base::Vector3d(1, 1, 1), k * 2 * std::numbers::pi / 3);
}

// The gizmo's axis length in its own units (SoFCCSysDragger's arrows).
constexpr float GizmoAxis = 10.f;

// Within this many pixels of an axis, beyond the gizmo, a click locks to it.
constexpr float AxisSnapPixels = 6.f;
constexpr float AxisDeadPixels = 24.f;

}  // namespace

// Where a placed point came from, and so which constraint ties it there.
struct ViewProviderSketch3D::Snap
{
    enum class Kind
    {
        None,
        SketchPoint,  // coincident with (geoId, pos)
        SketchCurve,  // on geoId
        ModelVertex,  // an element of the model: made an external reference first
        ModelEdge,
        ModelPlane,
        Model,  // position only (a curved face)
    };
    Kind kind {Kind::None};
    int geoId {Sketcher::GeoEnum::GeoUndef};
    Sketcher::PointPos pos {Sketcher::PointPos::none};
    App::DocumentObject* obj {nullptr};
    std::string sub;
};

struct ViewProviderSketch3D::Placed
{
    Base::Vector3d point;
    Snap snap;
};

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

    // The manipulator: its frame in the sketch's frame, its active plane, the height
    // lock's base point, and the nodes that draw it.
    Base::Vector3d origin;
    Base::Rotation rotation;
    Plane plane {Plane::XY};
    bool heightLock {false};
    Base::Vector3d heightBase;
    bool rotationHandles {true};
    SbVec2s lastPos {0, 0};
    SoSeparator* manipRoot {nullptr};
    SoTransform* manipTransform {nullptr};
    SoScale* manipScale {nullptr};
    SoMaterial* planeMaterials[3] {nullptr, nullptr, nullptr};
    SoSeparator* planeNodes[3] {nullptr, nullptr, nullptr};
    SoCoordinate3* heightCoords {nullptr};
    SoLineSet* heightLine {nullptr};
    Gui::SoFCCSysDragger* dragger {nullptr};
    bool syncing {false};

    // What the drawn parts are: a curve part's geometry id, a point's id and position.
    std::vector<int> curveRefs;
    std::vector<std::pair<int, Sketcher::PointPos>> pointRefs;

    // The active tool, the points it placed and not yet made, and a polyline's chain.
    Tool tool {Tool::None};
    std::vector<Placed> placed;
    int chainFirst {-1};
    int chainLast {-1};
    SoCoordinate3* previewCoords {nullptr};
    SoLineSet* previewLines {nullptr};
    SoCoordinate3* cursorCoords {nullptr};
    SoMarkerSet* cursorMarker {nullptr};
    SoMaterial* cursorMaterial {nullptr};

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

        buildManipulator();
        buildPreview();
    }

    void buildPreview()
    {
        // The tool's rubber band and the point a click would place, not pickable.
        auto preview = new SoSeparator();
        auto pick = new SoPickStyle();
        pick->style = SoPickStyle::UNPICKABLE;
        auto style = new SoDrawStyle();
        style->lineWidth = 1;
        style->linePattern = 0xFF00;
        auto material = new SoMaterial();
        material->diffuseColor = SbColor(0.8f, 0.8f, 0.8f);
        previewCoords = new SoCoordinate3();
        previewLines = new SoLineSet();
        previewLines->numVertices.setNum(0);
        preview->addChild(pick);
        preview->addChild(style);
        preview->addChild(material);
        preview->addChild(previewCoords);
        preview->addChild(previewLines);

        auto cursor = new SoSeparator();
        cursorMaterial = new SoMaterial();
        cursorCoords = new SoCoordinate3();
        cursorMarker = new SoMarkerSet();
        cursorMarker->markerIndex = Gui::Inventor::MarkerBitmaps::getMarkerIndex("CIRCLE_LINE", 9);
        cursorMarker->numPoints = 0;
        cursor->addChild(cursorMaterial);
        cursor->addChild(cursorCoords);
        cursor->addChild(cursorMarker);
        preview->addChild(cursor);
        content->addChild(preview);
    }

    void buildManipulator()
    {
        // Axes and the three plane handles in the gizmo's units, scaled with it so
        // they keep their size on screen; the gizmo itself shows only its rotation
        // rings, which tilt the frame (section 7).
        manipRoot = new SoSeparator();
        manipRoot->setName("Sketch3DManipulator");
        dragger = new Gui::SoFCCSysDragger();
        dragger->setName("Sketch3DManipulatorDragger");
        // About 70 pixels across: its axes are 10 units and its rings of radius 8,
        // and the size is the fraction of the view half a unit takes.
        dragger->draggerSize.setValue(0.015f);
        dragger->hideTranslationX();
        dragger->hideTranslationY();
        dragger->hideTranslationZ();
        dragger->hidePlanarTranslationXY();
        dragger->hidePlanarTranslationYZ();
        dragger->hidePlanarTranslationZX();
        manipRoot->addChild(dragger);

        auto frame = new SoSeparator();
        manipTransform = new SoTransform();
        manipScale = new SoScale();
        auto compose = new SoComposeVec3f();
        compose->x.connectFrom(&dragger->autoScaleResult);
        compose->y.connectFrom(&dragger->autoScaleResult);
        compose->z.connectFrom(&dragger->autoScaleResult);
        manipScale->scaleFactor.connectFrom(&compose->vector);
        frame->addChild(manipTransform);
        frame->addChild(manipScale);

        auto axes = new SoSeparator();
        auto axisPick = new SoPickStyle();
        axisPick->style = SoPickStyle::UNPICKABLE;
        auto axisBinding = new SoMaterialBinding();
        axisBinding->value = SoMaterialBinding::PER_PART;
        auto axisMaterial = new SoMaterial();
        const SbColor axisColors[3] = {SbColor(1, 0, 0), SbColor(0, 0.8f, 0), SbColor(0, 0, 1)};
        axisMaterial->diffuseColor.setValues(0, 3, axisColors);
        auto axisStyle = new SoDrawStyle();
        axisStyle->lineWidth = 2;
        auto axisCoords = new SoCoordinate3();
        const SbVec3f axisPts[6] = {SbVec3f(0, 0, 0),
                                    SbVec3f(GizmoAxis, 0, 0),
                                    SbVec3f(0, 0, 0),
                                    SbVec3f(0, GizmoAxis, 0),
                                    SbVec3f(0, 0, 0),
                                    SbVec3f(0, 0, GizmoAxis)};
        axisCoords->point.setValues(0, 6, axisPts);
        auto axisLines = new SoLineSet();
        const int32_t axisCounts[3] = {2, 2, 2};
        axisLines->numVertices.setValues(0, 3, axisCounts);
        axes->addChild(axisPick);
        axes->addChild(axisBinding);
        axes->addChild(axisMaterial);
        axes->addChild(axisStyle);
        axes->addChild(axisCoords);
        axes->addChild(axisLines);
        frame->addChild(axes);

        // Plane handles: a square in each plane, off the origin as Fusion's are.
        const char* names[3] = {"Sketch3DPlaneXY", "Sketch3DPlaneYZ", "Sketch3DPlaneZX"};
        for (int k = 0; k < 3; ++k) {
            auto node = new SoSeparator();
            node->setName(names[k]);
            planeMaterials[k] = new SoMaterial();
            auto coords = new SoCoordinate3();
            SbVec3f sq[4];
            const float a = 0.25f * GizmoAxis, b = 0.55f * GizmoAxis;
            const float corners[4][2] = {{a, a}, {b, a}, {b, b}, {a, b}};
            for (int c = 0; c < 4; ++c) {
                float u = corners[c][0], v = corners[c][1];
                sq[c] = k == 0 ? SbVec3f(u, v, 0) : (k == 1 ? SbVec3f(0, u, v) : SbVec3f(v, 0, u));
            }
            coords->point.setValues(0, 4, sq);
            auto face = new SoFaceSet();
            face->numVertices.setValue(4);
            node->addChild(planeMaterials[k]);
            node->addChild(coords);
            node->addChild(face);
            planeNodes[k] = node;
            frame->addChild(node);
        }
        manipRoot->addChild(frame);

        // The height lock's line: the plane's normal through the base point, in the
        // sketch's frame, unscaled.
        auto height = new SoSeparator();
        auto heightPick = new SoPickStyle();
        heightPick->style = SoPickStyle::UNPICKABLE;
        auto heightStyle = new SoDrawStyle();
        heightStyle->linePattern = 0xF0F0;
        heightStyle->lineWidth = 1;
        heightCoords = new SoCoordinate3();
        heightLine = new SoLineSet();
        heightLine->numVertices.setNum(0);
        height->addChild(heightPick);
        height->addChild(heightStyle);
        height->addChild(heightCoords);
        height->addChild(heightLine);
        manipRoot->addChild(height);

        content->addChild(manipRoot);
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

PyObject* ViewProviderSketch3D::getPyObject()
{
    if (!pyViewObject) {
        pyViewObject = new ViewProviderSketch3DPy(this);
    }
    pyViewObject->IncRef();
    return pyViewObject;
}

Base::Matrix4D ViewProviderSketch3D::editingTransform() const
{
    // The edit's own path, Links included (memory: editing placement, not own).
    auto doc = Gui::Application::Instance->editDocument();
    if (!doc || doc->getInEdit() != this) {
        return getSketch3DObject()->globalPlacement().toMatrix();
    }
    return doc->getEditingTransform();
}

Base::Vector3d ViewProviderSketch3D::heightBase() const
{
    return edit ? edit->heightBase : Base::Vector3d();
}

Base::Vector3d ViewProviderSketch3D::manipulatorOrigin() const
{
    return edit ? edit->origin : Base::Vector3d();
}

Base::Rotation ViewProviderSketch3D::manipulatorRotation() const
{
    return edit ? edit->rotation : Base::Rotation();
}

ViewProviderSketch3D::Plane ViewProviderSketch3D::activePlane() const
{
    return edit ? edit->plane : Plane::XY;
}

bool ViewProviderSketch3D::heightLocked() const
{
    return edit && edit->heightLock;
}

bool ViewProviderSketch3D::rotationHandlesShown() const
{
    return edit && edit->rotationHandles;
}

Base::Placement ViewProviderSketch3D::workingPlane() const
{
    if (!edit) {
        return {};
    }
    return {edit->origin, edit->rotation * planeTurn(static_cast<int>(edit->plane))};
}

void ViewProviderSketch3D::setManipulator(const Base::Vector3d& origin,
                                          const Base::Rotation& rotation)
{
    if (!edit) {
        return;
    }
    edit->origin = origin;
    edit->rotation = rotation;
    updateManipulator();
}

void ViewProviderSketch3D::cycleWorkingPlane()
{
    if (!edit) {
        return;
    }
    edit->plane = static_cast<Plane>((static_cast<int>(edit->plane) + 1) % 3);
    updateManipulator();
}

void ViewProviderSketch3D::resetManipulator()
{
    if (!edit) {
        return;
    }
    edit->rotation = Base::Rotation();
    edit->plane = Plane::XY;
    edit->heightLock = false;
    updateManipulator();
}

void ViewProviderSketch3D::toggleHeightLock()
{
    if (!edit) {
        return;
    }
    if (!edit->heightLock) {
        // The in-plane position under the pointer is what the lock keeps.
        Base::Vector3d base;
        if (!pointAt(edit->lastPos, base)) {
            base = edit->origin;
        }
        edit->heightBase = base;
    }
    edit->heightLock = !edit->heightLock;
    updateManipulator();
}

void ViewProviderSketch3D::setRotationHandlesShown(bool shown)
{
    if (!edit) {
        return;
    }
    edit->rotationHandles = shown;
    updateManipulator();
}

void ViewProviderSketch3D::updateManipulator()
{
    if (!edit) {
        return;
    }
    Base::StateLocker lock(edit->syncing, true);
    const auto& o = edit->origin;
    double q0, q1, q2, q3;
    edit->rotation.getValue(q0, q1, q2, q3);
    const SbVec3f t(static_cast<float>(o.x), static_cast<float>(o.y), static_cast<float>(o.z));
    const SbRotation r(static_cast<float>(q0),
                       static_cast<float>(q1),
                       static_cast<float>(q2),
                       static_cast<float>(q3));
    edit->manipTransform->translation = t;
    edit->manipTransform->rotation = r;
    edit->dragger->translation = t;
    edit->dragger->rotation = r;
    if (edit->rotationHandles) {
        edit->dragger->showRotationX();
        edit->dragger->showRotationY();
        edit->dragger->showRotationZ();
    }
    else {
        edit->dragger->hideRotationX();
        edit->dragger->hideRotationY();
        edit->dragger->hideRotationZ();
    }
    for (int k = 0; k < 3; ++k) {
        bool active = k == static_cast<int>(edit->plane);
        edit->planeMaterials[k]->diffuseColor =
            active ? SbColor(1.f, 0.75f, 0.f) : SbColor(0.6f, 0.6f, 0.6f);
        edit->planeMaterials[k]->transparency = active ? 0.3f : 0.7f;
    }
    if (edit->heightLock) {
        Base::Vector3d n(0, 0, 1);
        workingPlane().getRotation().multVec(n, n);
        Base::Vector3d a = edit->heightBase - n * 1000., b = edit->heightBase + n * 1000.;
        const SbVec3f pts[2] = {SbVec3f(float(a.x), float(a.y), float(a.z)),
                                SbVec3f(float(b.x), float(b.y), float(b.z))};
        edit->heightCoords->point.setValues(0, 2, pts);
        edit->heightLine->numVertices.setValue(2);
    }
    else {
        edit->heightLine->numVertices.setNum(0);
    }
}

void ViewProviderSketch3D::draggerChanged(void* data, SoDragger* /*dragger*/)
{
    // The rotation rings tilt the frame; the origin stays where it is.
    auto vp = static_cast<ViewProviderSketch3D*>(data);
    if (!vp->edit || vp->edit->syncing) {
        return;
    }
    SbRotation r = vp->edit->dragger->rotation.getValue();
    float q0, q1, q2, q3;
    r.getValue(q0, q1, q2, q3);
    vp->edit->rotation = Base::Rotation(q0, q1, q2, q3);
    vp->updateManipulator();
}

bool ViewProviderSketch3D::toView(const Base::Vector3d& point, SbVec2s& pos) const
{
    if (!edit || !edit->viewer) {
        return false;
    }
    Base::Vector3d w;
    editingTransform().multVec(point, w);
    pos = edit->viewer->getPointOnViewport(SbVec3f(float(w.x), float(w.y), float(w.z)));
    return true;
}

bool ViewProviderSketch3D::pointAt(const SbVec2s& pos, Base::Vector3d& point) const
{
    if (!edit || !edit->viewer) {
        return false;
    }
    const Base::Matrix4D mat = editingTransform();
    Base::Matrix4D inv(mat);
    inv.inverseGauss();
    auto toLocal = [&](const SbVec3f& w, Base::Vector3d& out) {
        if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2])) {
            return false;
        }
        inv.multVec(Base::Vector3d(w[0], w[1], w[2]), out);
        return true;
    };
    // The point of a line closest to the pick ray, in doubles and exactly on the line
    // (the viewer's getPointOnLine goes through the focal plane in floats and lands
    // up to a tenth of a millimetre off it).
    auto onLine = [&](const Base::Vector3d& base, const Base::Vector3d& dir, Base::Vector3d& out) {
        SoCamera* camera = edit->viewer->getCamera();
        if (!camera) {
            return false;
        }
        SbLine ray;
        camera->getViewVolume().projectPointToLine(edit->viewer->getNormalizedPosition(pos), ray);
        Base::Vector3d p0, u(ray.getDirection()[0], ray.getDirection()[1], ray.getDirection()[2]);
        inv.multVec(
            Base::Vector3d(ray.getPosition()[0], ray.getPosition()[1], ray.getPosition()[2]),
            p0);
        Base::Vector3d p1;
        inv.multVec(Base::Vector3d(ray.getPosition()[0], ray.getPosition()[1], ray.getPosition()[2])
                        + u,
                    p1);
        u = p1 - p0;  // the ray in the sketch's frame
        const Base::Vector3d& v = dir;
        Base::Vector3d w = p0 - base;
        double a = u * u, b = u * v, c = v * v, d = u * w, e = v * w;
        double denom = a * c - b * b;
        if (std::fabs(denom) < 1e-12 * a * c) {
            return false;  // the line runs along the ray
        }
        out = base + v * ((a * e - b * d) / denom);
        return true;
    };

    const Base::Placement plane = workingPlane();
    if (edit->heightLock) {
        Base::Vector3d n(0, 0, 1);
        plane.getRotation().multVec(n, n);
        return onLine(edit->heightBase, n, point);
    }

    // Along an axis of the manipulator, beyond the gizmo: that axis. The axis is
    // projected over ten gizmo lengths, so its direction on screen is not a few
    // rounded pixels.
    SbVec2s o;
    if (toView(edit->origin, o)) {
        float dpr = float(edit->viewer->devicePixelRatio());
        double reach = 10. * GizmoAxis * std::max(1e-6f, edit->dragger->autoScaleResult.getValue());
        int best = -1;
        float bestDist = AxisSnapPixels * dpr;
        for (int k = 0; k < 3; ++k) {
            Base::Vector3d axis(k == 0, k == 1, k == 2);
            edit->rotation.multVec(axis, axis);
            SbVec2s e;
            if (!toView(edit->origin + axis * reach, e)) {
                continue;
            }
            SbVec2f d(float(e[0] - o[0]), float(e[1] - o[1]));
            float len = d.length();
            if (len < 1.f) {
                continue;  // seen end on
            }
            d /= len;
            SbVec2f r(float(pos[0] - o[0]), float(pos[1] - o[1]));
            float along = r.dot(d);
            float across = std::fabs(r[0] * d[1] - r[1] * d[0]);
            if (std::fabs(along) > AxisDeadPixels * dpr && across < bestDist) {
                best = k;
                bestDist = across;
            }
        }
        if (best >= 0) {
            Base::Vector3d axis(best == 0, best == 1, best == 2);
            edit->rotation.multVec(axis, axis);
            if (onLine(edit->origin, axis, point)) {
                return true;
            }
        }
    }

    Base::Placement world = Base::Placement(mat) * plane;
    return toLocal(edit->viewer->getPointOnXYPlaneOfPlacement(pos, world), point);
}

int ViewProviderSketch3D::planeHandleAt(const SbVec2s& pos) const
{
    // In screen space, inside the projected square: the rotation rings can lie in
    // front of a handle along the pick ray, and the handle is what is aimed at.
    if (!edit || !edit->viewer) {
        return -1;
    }
    const float scale = edit->dragger->autoScaleResult.getValue();
    const float a = 0.25f * GizmoAxis * scale, b = 0.55f * GizmoAxis * scale;
    const float corners[4][2] = {{a, a}, {b, a}, {b, b}, {a, b}};
    for (int k = 0; k < 3; ++k) {
        SbVec2f quad[4];
        bool ok = true;
        for (int c = 0; c < 4 && ok; ++c) {
            float u = corners[c][0], v = corners[c][1];
            Base::Vector3d q = k == 0
                ? Base::Vector3d(u, v, 0)
                : (k == 1 ? Base::Vector3d(0, u, v) : Base::Vector3d(v, 0, u));
            edit->rotation.multVec(q, q);
            SbVec2s sv;
            ok = toView(edit->origin + q, sv);
            quad[c] = SbVec2f(sv[0], sv[1]);
        }
        if (!ok) {
            continue;
        }
        // inside when on the same side of all four edges
        int sign = 0;
        bool inside = true;
        for (int c = 0; c < 4 && inside; ++c) {
            SbVec2f e = quad[(c + 1) % 4] - quad[c];
            SbVec2f r = SbVec2f(pos[0], pos[1]) - quad[c];
            float cross = e[0] * r[1] - e[1] * r[0];
            int sc = cross > 0 ? 1 : (cross < 0 ? -1 : 0);
            if (sc != 0) {
                if (sign == 0) {
                    sign = sc;
                }
                else if (sc != sign) {
                    inside = false;
                }
            }
        }
        if (inside && sign != 0) {
            return k;
        }
    }
    return -1;
}

int ViewProviderSketch3D::pointNear(const SbVec2s& pos) const
{
    // Sketch points come before curves: at a line's end the pick hits the line as
    // readily as its end point, and a click there means the point.
    if (!edit || !edit->viewer) {
        return -1;
    }
    const float radius = edit->viewer->getPickRadius() * float(edit->viewer->devicePixelRatio());
    int best = -1;
    float bestDist = radius * radius;
    for (int i = 0; i < edit->pointCoords->point.getNum() && i < int(edit->pointRefs.size()); ++i) {
        const SbVec3f& p = edit->pointCoords->point[i];
        SbVec2s v;
        if (!toView(Base::Vector3d(p[0], p[1], p[2]), v)) {
            continue;
        }
        float dx = float(v[0] - pos[0]), dy = float(v[1] - pos[1]);
        float d = dx * dx + dy * dy;
        if (d <= bestDist) {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

bool ViewProviderSketch3D::press(const SbVec2s& pos)
{
    if (!edit || !edit->viewer) {
        return false;
    }
    edit->lastPos = pos;
    int handle = planeHandleAt(pos);
    if (handle >= 0) {
        edit->plane = static_cast<Plane>(handle);
        updateManipulator();
        return true;
    }
    std::unique_ptr<SoPickedPoint> pp(edit->viewer->getPointOnRay(pos, this));
    if (pp && pp->getPath()->containsNode(edit->dragger)) {
        return false;  // the rotation rings: the gizmo drags
    }
    if (edit->tool != Tool::None) {
        Placed placed;
        if (place(pos, placed)) {
            addPlaced(placed);
        }
        return true;
    }
    // A sketch point: the manipulator goes there (section 7, decision k).
    int near = pointNear(pos);
    if (near >= 0) {
        const SbVec3f& p = edit->pointCoords->point[near];
        edit->origin = Base::Vector3d(p[0], p[1], p[2]);
        updateManipulator();
        return true;
    }
    return false;
}

bool ViewProviderSketch3D::move(const SbVec2s& pos)
{
    if (edit) {
        edit->lastPos = pos;
        if (edit->tool != Tool::None) {
            updatePreview();
        }
    }
    // Not taken: the view goes on preselecting the model, which the snap reads.
    return false;
}

bool ViewProviderSketch3D::pressRight(const SbVec2s& pos)
{
    if (!edit || edit->tool == Tool::None) {
        return false;
    }
    edit->lastPos = pos;
    finishTool();
    return true;
}

bool ViewProviderSketch3D::keyPressed(bool pressed, int key)
{
    if (edit && edit->tool != Tool::None && key == SoKeyboardEvent::ESCAPE) {
        // Escape keeps what is placed (memory: Escape means leave, not undo).
        if (pressed) {
            finishTool();
        }
        return true;
    }
    return PartGui::ViewProviderPart::keyPressed(pressed, key);
}

bool ViewProviderSketch3D::mouseMove(const SbVec2s& cursorPos, Gui::ViewerContext* /*viewer*/)
{
    return move(cursorPos);
}

bool ViewProviderSketch3D::mouseButtonPressed(int button,
                                              bool pressed,
                                              const SbVec2s& cursorPos,
                                              const Gui::ViewerContext* /*viewer*/
)
{
    if (button == SoMouseButtonEvent::BUTTON1 && pressed) {
        return press(cursorPos);
    }
    if (button == SoMouseButtonEvent::BUTTON2 && edit && edit->tool != Tool::None) {
        // the release too, or the view's context menu comes up after the finish
        return pressed ? pressRight(cursorPos) : true;
    }
    return false;
}

// ---------------------------------------------------------------------------------
// Drawing tools

namespace
{

std::string pyVec(const Base::Vector3d& v)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "App.Vector(%.17g, %.17g, %.17g)", v.x, v.y, v.z);
    return buf;
}

std::string pyVecs(const std::vector<Base::Vector3d>& pts)
{
    std::string s = "[";
    for (std::size_t i = 0; i < pts.size(); ++i) {
        s += (i ? ", " : "") + pyVec(pts[i]);
    }
    return s + "]";
}

// The frame axis a direction runs along within two degrees, or -1.
int alongAxis(const Base::Vector3d& d)
{
    double l = d.Length();
    if (l <= 0.) {
        return -1;
    }
    for (int k = 0; k < 3; ++k) {
        double c = std::fabs((k == 0 ? d.x : k == 1 ? d.y : d.z) / l);
        if (c > std::cos(2. * std::numbers::pi / 180.)) {
            return k;
        }
    }
    return -1;
}

}  // namespace

ViewProviderSketch3D::Tool ViewProviderSketch3D::activeTool() const
{
    return edit ? edit->tool : Tool::None;
}

int ViewProviderSketch3D::placedCount() const
{
    return edit ? int(edit->placed.size()) : 0;
}

bool ViewProviderSketch3D::constructionMode() const
{
    return construction;
}

void ViewProviderSketch3D::setConstructionMode(bool c)
{
    construction = c;
}

void ViewProviderSketch3D::startTool(Tool tool)
{
    if (!edit) {
        return;
    }
    cancelTool();
    edit->tool = tool;
    edit->placed.clear();
    edit->chainFirst = edit->chainLast = -1;
    updatePreview();
    showToolStatus();
}

void ViewProviderSketch3D::cancelTool()
{
    if (!edit) {
        return;
    }
    edit->tool = Tool::None;
    edit->placed.clear();
    edit->chainFirst = edit->chainLast = -1;
    updatePreview();
    showToolStatus();
}

void ViewProviderSketch3D::finishTool()
{
    if (!edit || edit->tool == Tool::None) {
        return;
    }
    bool spline = edit->tool == Tool::BSplineThrough || edit->tool == Tool::BSplineControl;
    bool pending =
        spline ? edit->placed.size() >= 2 : edit->chainLast >= 0 || !edit->placed.empty();
    if (spline && edit->placed.size() >= 2) {
        makeSpline();
    }
    if (pending) {
        // A chain or a spline made: the tool stays for the next one.
        edit->placed.clear();
        edit->chainFirst = edit->chainLast = -1;
        updatePreview();
        showToolStatus();
        return;
    }
    cancelTool();
}

void ViewProviderSketch3D::showToolStatus()
{
    QString text;
    switch (edit ? edit->tool : Tool::None) {
        case Tool::Point:
            text = QObject::tr("3D point: click to place; Escape ends");
            break;
        case Tool::Line:
            text = edit->placed.empty() ? QObject::tr("3D line: click the start; Escape ends")
                                        : QObject::tr("3D line: click the end; Escape ends");
            break;
        case Tool::Polyline:
            text =
                QObject::tr("3D polyline: click the next point; Escape or a right click finishes");
            break;
        case Tool::BSplineThrough:
            text = QObject::tr(
                "3D spline through points: click the points; Escape or a right click makes it");
            break;
        case Tool::BSplineControl:
            text = QObject::tr(
                "3D spline by control points: click the poles; Escape or a right click makes it");
            break;
        case Tool::None:
            break;
    }
    Gui::getMainWindow()->showMessage(text);
}

bool ViewProviderSketch3D::place(const SbVec2s& pos, Placed& placed) const
{
    if (!edit || !edit->viewer) {
        return false;
    }
    Base::Matrix4D inv = editingTransform();
    inv.inverseGauss();
    auto sketch = getSketch3DObject();

    // 1. The sketch's own points and curves (and its references), as drawn; points
    // first.
    int near = pointNear(pos);
    if (near >= 0) {
        const SbVec3f& p = edit->pointCoords->point[near];
        placed.point = Base::Vector3d(p[0], p[1], p[2]);
        placed.snap.kind = Snap::Kind::SketchPoint;
        placed.snap.geoId = edit->pointRefs[near].first;
        placed.snap.pos = edit->pointRefs[near].second;
        return true;
    }
    std::unique_ptr<SoPickedPoint> pp(edit->viewer->getPointOnRay(pos, this));
    if (pp && !pp->getPath()->containsNode(edit->manipRoot)) {
        SoNode* tail = pp->getPath()->getTail();
        if (tail == edit->points) {
            auto detail = dynamic_cast<const SoPointDetail*>(pp->getDetail());
            int i = detail ? detail->getCoordinateIndex() : -1;
            if (i >= 0 && i < int(edit->pointRefs.size())) {
                const SbVec3f& p = edit->pointCoords->point[i];
                placed.point = Base::Vector3d(p[0], p[1], p[2]);
                placed.snap.kind = Snap::Kind::SketchPoint;
                placed.snap.geoId = edit->pointRefs[i].first;
                placed.snap.pos = edit->pointRefs[i].second;
                return true;
            }
        }
        else if (tail == edit->curveLines) {
            auto detail = dynamic_cast<const SoLineDetail*>(pp->getDetail());
            int part = detail ? detail->getPartIndex() : -1;
            if (part >= 0 && part < int(edit->curveRefs.size())) {
                int geoId = edit->curveRefs[part];
                auto curve =
                    Base::freecad_dynamic_cast<Part::GeomCurve>(sketch->getGeometry(geoId));
                SbVec3f w = pp->getPoint();
                Base::Vector3d at;
                inv.multVec(Base::Vector3d(w[0], w[1], w[2]), at);
                double u;
                if (curve && curve->closestParameter(at, u)) {
                    placed.point = curve->pointAtParameter(u);
                    placed.snap.kind = Snap::Kind::SketchCurve;
                    placed.snap.geoId = geoId;
                    return true;
                }
            }
        }
    }

    // 2. The model element under the pointer: what the view preselected.
    const auto& pre = Gui::Selection().getPreselection();
    App::Document* doc = sketch->getDocument();
    if (pre.pObjectName && pre.pObjectName[0] && pre.pDocName
        && doc->getName() == std::string(pre.pDocName)) {
        App::DocumentObject* top = doc->getObject(pre.pObjectName);
        std::string sub = pre.pSubName ? pre.pSubName : "";
        if (top && top != sketch) {
            // Taken from the sketch's own container when the path goes through it, so
            // the reference is in the frame the sketch's Placement is in.
            App::DocumentObject* obj = top;
            const char* element = nullptr;
            if (auto leaf = top->resolve(sub.c_str(), nullptr, nullptr, &element)) {
                auto group = App::GeoFeatureGroupExtension::getGroupOfObject(sketch);
                if (leaf != top && group
                    && App::GeoFeatureGroupExtension::getGroupOfObject(leaf) == group) {
                    obj = leaf;
                    sub = element ? element : "";
                }
            }
            Base::Vector3d at;
            inv.multVec(Base::Vector3d(pre.x, pre.y, pre.z), at);
            Part::TopoShape shape;
            try {
                shape = Part::Feature::getTopoShape(obj, sub.c_str(), true);
            }
            catch (...) {
            }
            if (!shape.isNull()) {
                // The element in the sketch's frame, as rebuildExternalGeometry takes it.
                Base::Matrix4D toSketch = sketch->Placement.getValue().inverse().toMatrix();
                shape.transformShape(toSketch, false, true);
                const TopoDS_Shape& s = shape.getShape();
                placed.snap.obj = obj;
                placed.snap.sub = sub;
                const Base::Vector3d& local = at;  // the preselected point, in the sketch's frame
                if (s.ShapeType() == TopAbs_VERTEX) {
                    gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(s));
                    placed.point = Base::Vector3d(p.X(), p.Y(), p.Z());
                    placed.snap.kind = Snap::Kind::ModelVertex;
                    return true;
                }
                if (s.ShapeType() == TopAbs_EDGE) {
                    BRepExtrema_DistShapeShape dist(
                        s,
                        BRepBuilderAPI_MakeVertex(gp_Pnt(local.x, local.y, local.z)).Vertex());
                    if (dist.IsDone() && dist.NbSolution() > 0) {
                        gp_Pnt p = dist.PointOnShape1(1);
                        placed.point = Base::Vector3d(p.X(), p.Y(), p.Z());
                        placed.snap.kind = Snap::Kind::ModelEdge;
                        return true;
                    }
                }
                if (s.ShapeType() == TopAbs_FACE) {
                    BRepAdaptor_Surface surf(TopoDS::Face(s));
                    placed.point = local;
                    placed.snap.kind = surf.GetType() == GeomAbs_Plane ? Snap::Kind::ModelPlane
                                                                       : Snap::Kind::Model;
                    return true;
                }
            }
        }
    }

    // 3. The manipulator's plane, axis or height lock.
    placed.snap = Snap();
    return pointAt(pos, placed.point);
}

void ViewProviderSketch3D::constrainTo(const Snap& snap, int geoId, int pos)
{
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    auto run = [&](const std::string& code) {
        Gui::Command::doCommand(Gui::Command::Doc, "%s", code.c_str());
    };
    // The external reference a model snap needs, reused when the sketch has it.
    auto external = [&]() {
        const auto& objs = sketch->ExternalGeometry.getValues();
        const auto& subs = sketch->ExternalGeometry.getSubValues();
        for (std::size_t i = 0; i < objs.size(); ++i) {
            if (objs[i] == snap.obj && i < subs.size() && subs[i] == snap.sub) {
                return Sketcher::Sketch3DObject::RefExt - int(i);
            }
        }
        int before = sketch->ExternalGeo.getSize();
        run(obj + ".addExternal(" + Gui::Command::getObjectCmd(snap.obj) + ", '" + snap.sub + "')");
        return sketch->ExternalGeo.getSize() > before
            ? Sketcher::Sketch3DObject::RefExt - (sketch->ExternalGeo.getSize() - 1)
            : int(Sketcher::GeoEnum::GeoUndef);
    };
    char buf[256];
    switch (snap.kind) {
        case Snap::Kind::SketchPoint:
            snprintf(buf,
                     sizeof(buf),
                     "%s.addConstraint(Sketcher.Constraint('Coincident', %d, %d, %d, %d))",
                     obj.c_str(),
                     geoId,
                     pos,
                     snap.geoId,
                     int(snap.pos));
            run(buf);
            break;
        case Snap::Kind::SketchCurve:
            snprintf(buf,
                     sizeof(buf),
                     "%s.addConstraint(Sketcher.Constraint('PointOnObject', %d, %d, %d))",
                     obj.c_str(),
                     geoId,
                     pos,
                     snap.geoId);
            run(buf);
            break;
        case Snap::Kind::ModelVertex:
        case Snap::Kind::ModelEdge:
        case Snap::Kind::ModelPlane: {
            int ext = external();
            if (ext == Sketcher::GeoEnum::GeoUndef) {
                break;
            }
            if (snap.kind == Snap::Kind::ModelVertex) {
                snprintf(buf,
                         sizeof(buf),
                         "%s.addConstraint(Sketcher.Constraint('Coincident', %d, %d, %d, 1))",
                         obj.c_str(),
                         geoId,
                         pos,
                         ext);
            }
            else {
                snprintf(buf,
                         sizeof(buf),
                         "%s.addConstraint(Sketcher.Constraint('PointOnObject', %d, %d, %d))",
                         obj.c_str(),
                         geoId,
                         pos,
                         ext);
            }
            run(buf);
            break;
        }
        case Snap::Kind::Model:
        case Snap::Kind::None:
            break;
    }
}

void ViewProviderSketch3D::addPlaced(const Placed& placed)
{
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    const char* cons = construction ? "True" : "False";

    auto run = [&](const std::string& code) {
        Gui::Command::doCommand(Gui::Command::Doc, "%s", code.c_str());
    };
    auto tie = [&](const Snap& snap, int geoId, int pos) {
        constrainTo(snap, geoId, pos);
    };
    auto fixed = [](const Snap& snap) {
        return snap.kind == Snap::Kind::SketchPoint || snap.kind == Snap::Kind::ModelVertex;
    };

    switch (edit->tool) {
        case Tool::Point: {
            Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Add 3D sketch point"));
            int id = sketch->Geometry.getSize();
            run(obj + ".addGeometry(Part.Point(" + pyVec(placed.point) + "), " + cons + ")");
            tie(placed.snap, id, 1);
            Gui::Command::commitCommand();
            break;
        }
        case Tool::Line:
        case Tool::Polyline: {
            if (edit->placed.empty()) {
                edit->placed.push_back(placed);
                break;
            }
            const Placed& prev = edit->placed.back();
            if ((placed.point - prev.point).Length() < Precision::Confusion()) {
                break;  // the same point again
            }
            Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Add 3D sketch line"));
            int id = sketch->Geometry.getSize();
            run(obj + ".addGeometry(Part.LineSegment(" + pyVec(prev.point) + ", "
                + pyVec(placed.point) + "), " + cons + ")");
            if (edit->chainLast >= 0) {
                char buf[192];
                snprintf(buf,
                         sizeof(buf),
                         "%s.addConstraint(Sketcher.Constraint('Coincident', %d, 2, %d, 1))",
                         obj.c_str(),
                         edit->chainLast,
                         id);
                run(buf);
            }
            else {
                tie(prev.snap, id, 1);
            }
            bool closes = edit->tool == Tool::Polyline && edit->chainFirst >= 0
                && placed.snap.kind == Snap::Kind::SketchPoint
                && placed.snap.geoId == edit->chainFirst
                && placed.snap.pos == Sketcher::PointPos::start;
            tie(placed.snap, id, 2);
            // Drawn along a frame axis, and not held at both ends already: Along it.
            int axis = alongAxis(placed.point - prev.point);
            bool heldStart = edit->chainLast >= 0 || fixed(prev.snap);
            if (axis >= 0 && !(heldStart && fixed(placed.snap))) {
                static const char* along[3] = {"AlongX", "AlongY", "AlongZ"};
                run(obj + ".addConstraint(Sketcher.Constraint('" + along[axis] + "', "
                    + std::to_string(id) + "))");
            }
            Gui::Command::commitCommand();
            if (edit->tool == Tool::Line || closes) {
                edit->placed.clear();
                edit->chainFirst = edit->chainLast = -1;
            }
            else {
                if (edit->chainFirst < 0) {
                    edit->chainFirst = id;
                }
                edit->chainLast = id;
                edit->placed.clear();
                Placed end = placed;
                end.snap = Snap();  // the next segment ties to this one instead
                edit->placed.push_back(end);
            }
            break;
        }
        case Tool::BSplineThrough:
        case Tool::BSplineControl:
            if (edit->placed.empty()
                || (placed.point - edit->placed.back().point).Length() >= Precision::Confusion()) {
                edit->placed.push_back(placed);
            }
            break;
        case Tool::None:
            return;
    }
    // The manipulator follows the last point (section 7, decision k).
    edit->origin = placed.point;
    if (edit->heightLock) {
        edit->heightLock = false;
    }
    updateManipulator();
    updatePreview();
    showToolStatus();
}

void ViewProviderSketch3D::makeSpline()
{
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    std::vector<Base::Vector3d> pts;
    for (const auto& p : edit->placed) {
        pts.push_back(p.point);
    }
    bool through = edit->tool == Tool::BSplineThrough;
    int n = int(pts.size());
    int id = sketch->Geometry.getSize();
    std::string code = "_bs = Part.BSplineCurve()\n";
    if (through) {
        // OCCT puts the knots at the points' chord-length parameters, so each point
        // is the curve at its knot, as the knot alignment says.
        code += "_bs.interpolate(" + pyVecs(pts) + ")\n";
    }
    else {
        code += "_bs.buildFromPoles(" + pyVecs(pts) + ", False, "
            + std::to_string(std::min(3, n - 1)) + ")\n";
    }
    code += obj + ".addGeometry(_bs, " + (construction ? "True" : "False") + ")\ndel _bs";
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Add 3D sketch spline"));
    Gui::Command::doCommand(Gui::Command::Doc, "%s", code.c_str());
    // The points it goes through (at its knots) or its poles, as sketch points held
    // to it, so they snap, take constraints and drag the spline.
    const char* alignment = through ? "BSplineKnotPoint" : "BSplineControlPoint";
    for (int k = 0; k < n; ++k) {
        int pid = sketch->Geometry.getSize();
        Gui::Command::doCommand(Gui::Command::Doc,
                                "%s.addGeometry(Part.Point(%s), True)\n"
                                "%s.addConstraint(Sketcher.Constraint('InternalAlignment:Sketcher::"
                                "%s', %d, 1, %d, %d))",
                                obj.c_str(),
                                pyVec(pts[k]).c_str(),
                                obj.c_str(),
                                alignment,
                                pid,
                                id,
                                k);
        // the same tie the line tool makes, on the point
        constrainTo(edit->placed[k].snap, pid, 1);
    }
    Gui::Command::commitCommand();
}

void ViewProviderSketch3D::updatePreview()
{
    if (!edit) {
        return;
    }
    std::vector<SbVec3f> pts;
    Placed under;
    bool have = edit->tool != Tool::None && edit->viewer && place(edit->lastPos, under);
    for (const auto& p : edit->placed) {
        pts.emplace_back(float(p.point.x), float(p.point.y), float(p.point.z));
    }
    if (have && !pts.empty()) {
        pts.emplace_back(float(under.point.x), float(under.point.y), float(under.point.z));
    }
    edit->previewCoords->point.setValues(0, int(pts.size()), pts.data());
    edit->previewCoords->point.setNum(int(pts.size()));
    if (pts.size() >= 2) {
        edit->previewLines->numVertices.setValue(int32_t(pts.size()));
    }
    else {
        edit->previewLines->numVertices.setNum(0);
    }
    if (have) {
        edit->cursorCoords->point.setValue(float(under.point.x),
                                           float(under.point.y),
                                           float(under.point.z));
        edit->cursorMarker->numPoints = 1;
        edit->cursorMaterial->diffuseColor =
            under.snap.kind == Snap::Kind::None ? SbColor(1, 1, 1) : SbColor(1, 0.85f, 0);
    }
    else {
        edit->cursorMarker->numPoints = 0;
    }
}

ViewProviderSketch3D* SketcherGui::activeSketch3DInEdit()
{
    auto doc = Gui::Application::Instance->activeDocument();
    auto vp = doc ? Base::freecad_dynamic_cast<ViewProviderSketch3D>(doc->getInEdit()) : nullptr;
    return vp && vp->isEditingSketch() ? vp : nullptr;
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

    edit->dragger->rotationIncrement.setValue(std::numbers::pi / 36);
    edit->dragger->addMotionCallback(&ViewProviderSketch3D::draggerChanged, this);
    edit->dragger->addFinishCallback(&ViewProviderSketch3D::draggerChanged, this);

    Workbench::enterEditMode();
    Gui::ToolBarManager::getInstance()->setState(sketch3DToolbars(),
                                                 Gui::ToolBarManager::State::ForceAvailable);
    draw();
    updateManipulator();
    return true;
}

void ViewProviderSketch3D::unsetEdit(int ModNum)
{
    if (ModNum != Default) {
        PartGui::ViewProviderPart::unsetEdit(ModNum);
        return;
    }
    Workbench::leaveEditMode();
    Gui::getMainWindow()->showMessage(QString());
    Gui::ToolBarManager::getInstance()->setState(sketch3DToolbars(),
                                                 Gui::ToolBarManager::State::RestoreDefault);

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
    if (auto camera = viewer->getCamera()) {
        edit->dragger->setUpAutoScale(camera);
    }
    draw();
    updateManipulator();
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

    edit->curveRefs.clear();
    edit->pointRefs.clear();
    auto addCurve = [&](const Part::Geometry* geo, const SbColor& color, int geoId) {
        std::size_t before = coords.size();
        sample(geo, coords);
        if (coords.size() > before) {
            counts.push_back(int32_t(coords.size() - before));
            colors.push_back(color);
            edit->curveRefs.push_back(geoId);
        }
    };
    auto addPoint =
        [&](const Base::Vector3d& v, const SbColor& color, int geoId, Sketcher::PointPos pos) {
            pts.emplace_back(float(v.x), float(v.y), float(v.z));
            ptColors.push_back(color);
            edit->pointRefs.emplace_back(geoId, pos);
        };
    using Sketcher::PointPos;

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

    int geoId = -1;
    for (auto geo : sketch->Geometry.getValues()) {
        ++geoId;
        if (geo->isDerivedFrom<Part::GeomPlane>()) {
            continue;  // a workplane: the manipulator shows it (section 6)
        }
        bool construction = Sketcher::GeometryFacade::getConstruction(geo);
        const SbColor& curveColor = construction
            ? (fully ? fullyConstructionColor : constructionColor)
            : (fully ? fullyColor : edgeColor);
        const SbColor& pointColor = fully ? fullyElementColor : vertexColor;
        if (auto pt = Base::freecad_dynamic_cast<Part::GeomPoint>(geo)) {
            addPoint(pt->getPoint(), pointColor, geoId, PointPos::start);
            continue;
        }
        addCurve(geo, curveColor, geoId);
        if (auto bounded = Base::freecad_dynamic_cast<Part::GeomBoundedCurve>(geo)) {
            addPoint(bounded->getStartPoint(), pointColor, geoId, PointPos::start);
            addPoint(bounded->getEndPoint(), pointColor, geoId, PointPos::end);
        }
        if (auto conic = Base::freecad_dynamic_cast<Part::GeomConic>(geo)) {
            addPoint(conic->getCenter(), pointColor, geoId, PointPos::mid);
        }
        else if (auto arc = Base::freecad_dynamic_cast<Part::GeomArcOfConic>(geo)) {
            addPoint(arc->getCenter(), pointColor, geoId, PointPos::mid);
        }
    }
    int extId = Sketcher::Sketch3DObject::RefExt;
    for (auto geo : sketch->ExternalGeo.getValues()) {
        if (auto pt = Base::freecad_dynamic_cast<Part::GeomPoint>(geo)) {
            addPoint(pt->getPoint(), externalColor, extId, PointPos::start);
        }
        else if (!geo->isDerivedFrom<Part::GeomPlane>()) {
            addCurve(geo, externalColor, extId);
        }
        --extId;
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
