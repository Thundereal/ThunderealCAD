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
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <string_view>

#include <boost/uuid/uuid.hpp>

#include <QApplication>
#include <QDateTime>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
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
#include <Inventor/nodes/SoImage.h>
#include <Inventor/nodes/SoTranslation.h>
#include <Inventor/sensors/SoNodeSensor.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMarkerSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoMaterialBinding.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoScale.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoTransform.h>

#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <App/Expression.h>
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
#include <Gui/DatumValueEditor.h>
#include <Gui/InventorBase.h>
#include <Gui/MainWindow.h>
#include <Gui/SoDatumLabel.h>
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

#include "Utils.h"
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
        auto box = new Gui::TaskView::TaskBox(Gui::BitmapFactory().pixmap("Sketcher_Sketch3D"),
                                              QObject::tr("3D sketch"),
                                              true,
                                              nullptr);
        auto widget = new QWidget();
        auto layout = new QVBoxLayout(widget);
        status = new QLabel(widget);
        status->setObjectName(QStringLiteral("Sketch3DStatus"));
        status->setWordWrap(true);
        status->setTextFormat(Qt::RichText);
        // a constraint number in the solver's message selects that constraint
        QObject::connect(status, &QLabel::linkActivated, status, [vp](const QString& link) {
            if (link.startsWith(QLatin1String("c:"))) {
                vp->clearSelection();
                vp->selectConstraint(link.mid(2).toInt());
            }
        });
        layout->addWidget(status);
        // Every constraint, so one without a label or an icon (a coincidence) can be
        // selected and deleted too; in step with the view's selection.
        list = new QListWidget(widget);
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        list->setObjectName(QStringLiteral("Sketch3DConstraintList"));
        QObject::connect(list, &QListWidget::itemSelectionChanged, list, [this]() {
            if (syncing) {
                return;
            }
            Base::StateLocker lock(syncing, true);
            this->vp->clearSelection();
            for (auto item : list->selectedItems()) {
                this->vp->selectConstraint(item->data(Qt::UserRole).toInt(), false);
            }
        });
        layout->addWidget(list);
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

    struct Row
    {
        int index;
        QIcon icon;
        QString text;
        bool selected;
    };
    void setConstraints(const std::vector<Row>& rows)
    {
        if (syncing) {
            return;
        }
        Base::StateLocker lock(syncing, true);
        list->clear();
        for (const auto& r : rows) {
            auto item = new QListWidgetItem(r.icon, r.text, list);
            item->setData(Qt::UserRole, r.index);
            item->setSelected(r.selected);
        }
    }

    ViewProviderSketch3D* vp;
    QLabel* status;
    QListWidget* list;
    bool syncing {false};
};

// Constraint numbers as links that select them (counted from 1, as the 2D sketch's
// messages count).
QString indexText(const std::vector<int>& v)
{
    QStringList list;
    for (int i : v) {
        list << QStringLiteral("<a href=\"c:%1\">%2</a>").arg(i).arg(i + 1);
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

// Keys the edit takes before a shortcut does, as the 2D sketch's ShortcutListener:
// Delete (Std_Delete's), and while a tool places points a number's first character
// (the digits are the standard views' shortcuts), which opens the typed value's entry.
class Sketch3DDeleteFilter: public QObject
{
public:
    explicit Sketch3DDeleteFilter(ViewProviderSketch3D* vp)
        : vp(vp)
    {}

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (event->type() == QEvent::ShortcutOverride) {
            auto key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Delete && !vp->isEditingDatum()
                && (!vp->selectedElements().empty() || !vp->selectedConstraints().empty())
                && vp->activeTool() == ViewProviderSketch3D::Tool::None) {
                key->accept();
                vp->deleteSelection();
                return true;
            }
            QString text = key->text();
            bool numeric = text.size() == 1
                && (text[0].isDigit() || text[0] == QLatin1Char('-')
                    || text[0] == QLatin1Char('.'));
            if (numeric && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
                && !vp->isEditingDatum() && !vp->isTypingPlacement()
                && vp->activeTool() != ViewProviderSketch3D::Tool::None
                && vp->activeTool() != ViewProviderSketch3D::Tool::Fillet
                && !qobject_cast<QLineEdit*>(QApplication::focusWidget())) {
                key->accept();
                vp->startTypedPlacement(text);
                return true;
            }
        }
        return QObject::eventFilter(obj, event);
    }

private:
    ViewProviderSketch3D* vp;
};

// The edit's colours changed in the preferences: drawn again, at once, as the 2D
// sketch's edit does it.
class Sketch3DColorObserver: public ParameterGrp::ObserverType
{
public:
    explicit Sketch3DColorObserver(ViewProviderSketch3D* vp)
        : vp(vp)
        , group(App::GetApplication().GetParameterGroupByPath(
              "User parameter:BaseApp/Preferences/View"))
    {
        group->Attach(this);
    }
    ~Sketch3DColorObserver() override
    {
        group->Detach(this);
    }
    void OnChange(Base::Subject<const char*>& /*caller*/, const char* reason) override
    {
        if (reason
            && (std::string_view(reason).ends_with("Color")
                || std::string_view(reason).ends_with("ColorLight")
                || std::string_view(reason) == "ConstraintOutlineWidth"
                || isViewBackgroundParameter(reason))
            && vp->isEditingSketch()) {
            vp->draw();
        }
    }

private:
    ViewProviderSketch3D* vp;
    ParameterGrp::handle group;
};

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
        Model,  // a curved face: made a reference, the point held on it
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

// A circle or arc the circular tools make: an arc runs sweep radians (negative:
// clockwise) about normal from centre + radius * xdir; a circle has closed set.
struct ViewProviderSketch3D::Circular
{
    Base::Vector3d centre;
    Base::Vector3d normal;
    Base::Vector3d xdir;
    double radius {0.};
    double sweep {0.};
    bool closed {false};

    Base::Vector3d at(double t) const
    {
        Base::Vector3d y = normal % xdir;
        return centre + (xdir * std::cos(t) + y * std::sin(t)) * radius;
    }
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
    // An arc from its centre: its normal, taken at the start, and the angle the
    // pointer swept from the start since, which says which way it runs.
    Base::Vector3d arcNormal;
    double arcSweep {0.};
    double arcLast {0.};
    SoCoordinate3* previewCoords {nullptr};
    SoLineSet* previewLines {nullptr};
    SoCoordinate3* cursorCoords {nullptr};
    SoMarkerSet* cursorMarker {nullptr};
    SoMaterial* cursorMaterial {nullptr};

    // The edit's selection, the dimension labels by constraint, a double click's
    // first press, and the value edit in progress.
    std::vector<Element> selection;
    std::vector<int> selectedConstraints;
    SoSeparator* labelRoot {nullptr};
    std::map<int, SoSeparator*> labelNodes;
    // The icons of the constraints without a value: each icon's node -> constraint, and
    // where it stands; the camera they were placed for, and the redraw it waits for.
    SoSeparator* iconRoot {nullptr};
    std::map<SoNode*, int> iconNodes;
    std::vector<std::pair<int, Base::Vector3d>> icons;
    std::unique_ptr<SoNodeSensor> cameraSensor;
    std::unique_ptr<QTimer> refaceTimer;
    std::map<int, Gui::SoDatumLabel*> labels;
    std::map<int, Base::Placement> labelPlanes;  // each label's plane, in the sketch's frame
    int lastLabelPress {-1};
    qint64 lastLabelTime {0};
    class Datum3DSession* datum {nullptr};

    // A press on the sketch that a move with the button held turns into a drag.
    bool dragCandidate {false};
    bool dragging {false};
    Element dragElement {Sketcher::GeoEnum::GeoUndef, Sketcher::PointPos::none};
    SbVec2s pressPos {0, 0};
    Base::Vector3d dragLast;
    // The kept plane points are put on (section 6), or -1.
    int keptPlane {-1};
    std::unique_ptr<QObject> deleteFilter;
    /// The status (DoF, conflicts) changes with a solve, with or without a
    /// property changing: drawn again after each
    fastsignals::scoped_connection solverUpdate;
    std::unique_ptr<ParameterGrp::ObserverType> colorObserver;
    // Typed values: the entries open, what a placement typed adds to the segment it
    // makes, and the ring turn the angle's entry sets
    class Typed3DSession* placing {nullptr};
    class Typed3DSession* turning {nullptr};
    double typedLength {-1.};
    bool typedOffsetSet {false};
    Base::Vector3d typedOffset;
    Base::Rotation turnStart;
    int turnAxis {-1};

    /// The pointer at p while an arc's end is picked: the sweep follows it round.
    void trackSweep(const Base::Vector3d& p)
    {
        if (placed.size() != 2) {
            return;
        }
        const Base::Vector3d& c = placed[0].point;
        const Base::Vector3d& n = arcNormal;
        Base::Vector3d x = placed[1].point - c, v = p - c;
        x = x - n * (x * n);
        v = v - n * (v * n);
        if (x.Length() < Precision::Confusion() || v.Length() < Precision::Confusion()) {
            return;
        }
        Base::Vector3d y = n % x;
        double a = std::atan2(v * y, v * x);
        double d = a - arcLast;
        while (d > std::numbers::pi) {
            d -= 2 * std::numbers::pi;
        }
        while (d < -std::numbers::pi) {
            d += 2 * std::numbers::pi;
        }
        arcSweep += d;
        arcLast = a;
    }

    EditData()
    {
        content = new SoSeparator();
        content->ref();
        content->setName("Sketch3DEdit");

        auto curves = new SoSeparator();
        curveMaterial = new SoMaterial();
        // PER_PART on a line set is a colour per segment (draw() repeats each
        // curve's for its segments); the picks read the part index this way
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

        labelRoot = new SoSeparator();
        labelRoot->setName("Sketch3DLabels");
        content->addChild(labelRoot);
        iconRoot = new SoSeparator();
        iconRoot->setName("Sketch3DIcons");
        content->addChild(iconRoot);

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
    sPixmap = "Sketcher_Sketch3D";
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
    auto doc = Gui::Application::Instance->editDocumentOf(getObject());
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
    edit->keptPlane = -1;
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
    edit->keptPlane = -1;
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
    // The pick ray through a pixel in the sketch's frame, in doubles from there on.
    // `eye` is where on it (p0 + u * eye) a perspective camera's eye is: nothing before
    // that is in view. An orthographic camera has no behind.
    double eye = -std::numeric_limits<double>::infinity();
    auto pickRay = [&](const SbVec2s& at, Base::Vector3d& p0, Base::Vector3d& u) {
        SoCamera* camera = edit->viewer->getCamera();
        if (!camera) {
            return false;
        }
        SbLine ray;
        camera->getViewVolume().projectPointToLine(edit->viewer->getNormalizedPosition(at), ray);
        Base::Vector3d start(ray.getPosition()[0], ray.getPosition()[1], ray.getPosition()[2]);
        Base::Vector3d p1;
        inv.multVec(start, p0);
        inv.multVec(start
                        + Base::Vector3d(ray.getDirection()[0],
                                         ray.getDirection()[1],
                                         ray.getDirection()[2]),
                    p1);
        u = p1 - p0;
        eye = -std::numeric_limits<double>::infinity();
        if (camera->getTypeId().isDerivedFrom(SoPerspectiveCamera::getClassTypeId())) {
            const SbVec3f& c = camera->position.getValue();
            Base::Vector3d e;
            inv.multVec(Base::Vector3d(c[0], c[1], c[2]), e);
            eye = ((e - p0) * u) / (u * u);
        }
        return true;
    };
    // The point of a line closest to the pick ray, in doubles and exactly on the line
    // (the viewer's getPointOnLine goes through the focal plane in floats and lands
    // up to a tenth of a millimetre off it). None where that point of the ray is
    // behind the eye: in perspective a line's far side on screen is the part of it
    // behind the eye.
    auto onLine = [&](const Base::Vector3d& base, const Base::Vector3d& dir, Base::Vector3d& out) {
        Base::Vector3d p0, u;
        if (!pickRay(pos, p0, u)) {
            return false;
        }
        const Base::Vector3d& v = dir;
        Base::Vector3d w = p0 - base;
        double a = u * u, b = u * v, c = v * v, d = u * w, e = v * w;
        double denom = a * c - b * b;
        if (std::fabs(denom) < 1e-12 * a * c) {
            return false;  // the line runs along the ray
        }
        if ((b * e - c * d) / denom < eye) {
            return false;
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
    // A circle's rim, an arc's start and end are taken into its plane: the axis along
    // its normal would put them back on the centre, so it does not lock.
    Base::Vector3d normal;
    if ((edit->tool == Tool::Circle || edit->tool == Tool::Arc) && !edit->placed.empty()) {
        normal = Base::Vector3d(0, 0, 1);
        if (edit->tool == Tool::Arc && edit->placed.size() >= 2) {
            normal = edit->arcNormal;
        }
        else {
            plane.getRotation().multVec(normal, normal);
        }
    }
    SbVec2s o;
    Base::Vector3d sight0, sight;
    if (toView(edit->origin, o) && pickRay(o, sight0, sight)) {
        sight.Normalize();
        float dpr = float(edit->viewer->devicePixelRatio());
        double reach = 10. * GizmoAxis * std::max(1e-6f, edit->dragger->autoScaleResult.getValue());
        int best = -1;
        float bestDist = AxisSnapPixels * dpr;
        for (int k = 0; k < 3; ++k) {
            Base::Vector3d axis(k == 0, k == 1, k == 2);
            edit->rotation.multVec(axis, axis);
            if (std::fabs(axis * normal) > 0.999) {
                continue;
            }
            // Within about 15 degrees of the line of sight a pixel along it is a long
            // way: seen end on, it does not lock
            if ((axis % sight).Length() < 0.25) {
                continue;
            }
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

    // On the plane: where the ray meets it, in doubles and exactly on it, as for a
    // line (the viewer's getPointOnXYPlaneOfPlacement works in floats, and a point
    // placed on the XY plane came out at z = 1e-6).
    Base::Vector3d p0, u;
    if (pickRay(pos, p0, u)) {
        Base::Vector3d o = plane.getPosition(), n(0, 0, 1);
        plane.getRotation().multVec(n, n);
        double un = u * n;
        if (std::fabs(un) > 1e-12 * u.Length()) {
            const double t = ((o - p0) * n) / un;
            if (t < eye) {
                return false;  // the plane is met behind the eye: not in view there
            }
            point = p0 + u * t;
            point = point - n * ((point - o) * n);
            return true;
        }
    }
    Base::Placement world = Base::Placement(mat) * plane;  // seen edge on
    try {
        return toLocal(edit->viewer->getPointOnXYPlaneOfPlacement(pos, world), point);
    }
    catch (const Base::Exception&) {
        return false;  // the ray runs in the plane: no point on it
    }
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

bool ViewProviderSketch3D::press(const SbVec2s& pos, bool ctrl)
{
    if (!edit || !edit->viewer) {
        return false;
    }
    edit->lastPos = pos;
    edit->dragCandidate = false;
    // A press anywhere while a value is typed takes what is typed.
    if (edit->datum) {
        finishDatumEdit(true);
        return true;
    }
    int handle = planeHandleAt(pos);
    if (handle >= 0) {
        edit->plane = static_cast<Plane>(handle);
        updateManipulator();
        return true;
    }
    std::unique_ptr<SoPickedPoint> pp(edit->viewer->getPointOnRay(pos, this));
    if (ctrl && takePlaneAt(pos, pp.get())) {
        return true;  // asked for, so before the gizmo
    }
    if (pp && pp->getPath()->containsNode(edit->dragger)) {
        return false;  // the rotation rings: the gizmo drags
    }
    if (edit->tool == Tool::Fillet) {
        int near = pointNear(pos);
        QString why;
        if (near < 0
            || !filletAt(edit->pointRefs[near].first, edit->pointRefs[near].second, &why)) {
            Gui::getMainWindow()->showMessage(
                why.isEmpty() ? QObject::tr("Click a corner where two lines meet") : why,
                5000);
        }
        return true;
    }
    if (edit->tool != Tool::None) {
        Placed placed;
        if (place(pos, placed)) {
            addPlaced(placed);
        }
        return true;
    }
    // A dimension's label: selected; a second press on it soon after, its value
    // edited.
    int label = pp ? labelOnPath(pp->getPath()) : -1;
    if (label >= 0) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        bool twice = label == edit->lastLabelPress
            && now - edit->lastLabelTime < QApplication::doubleClickInterval();
        edit->lastLabelPress = label;
        edit->lastLabelTime = now;
        if (twice) {
            editDatum(label);
        }
        else {
            selectConstraint(label);
        }
        return true;
    }
    // A sketch point: selected, and the manipulator goes there (section 7,
    // decision k).
    int near = pointNear(pos);
    if (near >= 0) {
        const SbVec3f& p = edit->pointCoords->point[near];
        edit->origin = Base::Vector3d(p[0], p[1], p[2]);
        updateManipulator();
        Element e {edit->pointRefs[near].first, edit->pointRefs[near].second};
        select(e);
        if (e.geoId >= 0) {
            edit->dragCandidate = true;
            edit->dragElement = e;
            edit->pressPos = pos;
        }
        return true;
    }
    // A curve: selected.
    if (pp && pp->getPath()->getTail() == edit->curveLines) {
        auto detail = dynamic_cast<const SoLineDetail*>(pp->getDetail());
        int part = detail ? detail->getLineIndex() : -1;  // the polyline: a curve
        if (part >= 0 && part < int(edit->curveRefs.size())) {
            Element e {edit->curveRefs[part], Sketcher::PointPos::none};
            select(e);
            auto sketch = getSketch3DObject();
            auto geo = sketch->getGeometry(e.geoId);
            // a workplane moves when it is not blocked (section 6)
            bool blocked = false;
            for (auto c : sketch->Constraints.getValues()) {
                blocked = blocked || (c->Type == Sketcher::Block && c->First == e.geoId);
            }
            if (e.geoId >= 0 && geo && !(geo->isDerivedFrom<Part::GeomPlane>() && blocked)) {
                // a curve is dragged by where it was taken, on the working plane
                // through that point
                SbVec3f w = pp->getPoint();
                Base::Matrix4D inv = editingTransform();
                inv.inverseGauss();
                inv.multVec(Base::Vector3d(w[0], w[1], w[2]), edit->dragLast);
                edit->origin = edit->dragLast;
                updateManipulator();
                edit->dragCandidate = true;
                edit->dragElement = e;
                edit->pressPos = pos;
            }
            return true;
        }
    }
    // Nothing of the sketch: the selection goes, and the view has the press.
    clearSelection();
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

bool ViewProviderSketch3D::dragTo(const SbVec2s& pos)
{
    if (!edit || !edit->dragCandidate) {
        return false;
    }
    edit->lastPos = pos;
    if (!edit->dragging) {
        // a press that wanders a few pixels is still a click
        int dx = pos[0] - edit->pressPos[0], dy = pos[1] - edit->pressPos[1];
        if (dx * dx + dy * dy < 9) {
            return true;
        }
        edit->dragging = true;
        Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Drag 3D sketch geometry"));
    }
    Base::Vector3d at;
    if (!pointAt(pos, at)) {
        return true;
    }
    auto sketch = getSketch3DObject();
    const Element& e = edit->dragElement;
    if (e.pos != Sketcher::PointPos::none) {
        sketch->movePoint(e.geoId, e.pos, at, false);
    }
    else {
        sketch->movePoint(e.geoId, e.pos, at - edit->dragLast, true);
        edit->dragLast = at;
    }
    return true;
}

bool ViewProviderSketch3D::release(const SbVec2s& /*pos*/)
{
    if (!edit || !edit->dragCandidate) {
        return false;
    }
    bool dragged = edit->dragging;
    edit->dragCandidate = edit->dragging = false;
    if (dragged) {
        Gui::Command::commitCommand();
        // the manipulator goes with the point it was on
        Base::Vector3d p;
        if (edit->dragElement.pos != Sketcher::PointPos::none
            && pointOf(edit->dragElement.geoId, edit->dragElement.pos, p)) {
            edit->origin = p;
            updateManipulator();
        }
    }
    return dragged;
}

namespace
{

/// A quantity's value, its unit dropped
bool quantityOf(const QString& s, double& v)
{
    try {
        v = Base::Quantity::parse(s.trimmed().toStdString()).getValue();
        return true;
    }
    catch (const Base::Exception&) {
        return false;
    }
}

/// Three offsets typed for a placement, "dx, dy, dz": commas, semicolons or
/// spaces between
bool threeOffsets(const QString& text, double v[3])
{
    static const QRegularExpression separators(QStringLiteral("[,;]|\\s+(?=[-+.0-9])"));
    QStringList parts = text.split(separators, Qt::SkipEmptyParts);
    return parts.size() == 3 && quantityOf(parts[0], v[0]) && quantityOf(parts[1], v[1])
        && quantityOf(parts[2], v[2]);
}

/// A distance's kind typed with its value: "x 20", "y 20", "z 20", "d 20"
const QRegularExpression& kindText()
{
    static const QRegularExpression re(QStringLiteral("^\\s*([xyzdXYZD])\\s*([-+.0-9].*)$"));
    return re;
}

}  // namespace

namespace SketcherGui
{

/** An entry for a value typed in the view that is not a constraint's: a placement's
 * length or offsets, the manipulator's angle. Enter gives the text to apply, which
 * says whether it took it; Escape closes it.
 */
class Typed3DSession: public QObject
{
public:
    using Apply = std::function<bool(const QString&, QString*)>;
    Typed3DSession(Gui::ViewerContext* viewer,
                   const Base::Placement& plane,
                   double value,
                   const Base::Unit& unit,
                   Apply apply,
                   std::function<void()> closed)
        : apply(std::move(apply))
        , closed(std::move(closed))
    {
        editor = std::make_unique<Gui::DatumValueEditor>(viewer, plane);
        Gui::DatumValueEditor::Target target;
        target.value = value;
        target.unit = unit;
        editor->installKeyFilter(this);
        editor->edit(target);
    }

    void setText(const QString& text)
    {
        editor->setText(text);
    }

    void setTextReader(Gui::DatumValueEditor::TextReader reader)
    {
        editor->setTextReader(std::move(reader));
    }

    void finish()
    {
        if (ended) {
            return;
        }
        ended = true;
        editor->close();
        closed();
        deleteLater();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (ended || event->type() != QEvent::KeyPress || editor->isCompleting()) {
            return QObject::eventFilter(watched, event);
        }
        switch (static_cast<QKeyEvent*>(event)->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter: {
                QString why;
                if (apply(editor->text(), &why)) {
                    finish();
                }
                else {
                    editor->showError(why);
                }
                return true;
            }
            case Qt::Key_Escape:
                finish();
                return true;
            default:
                return false;
        }
    }

private:
    std::unique_ptr<Gui::DatumValueEditor> editor;
    Apply apply;
    std::function<void()> closed;
    bool ended {false};
};

}  // namespace SketcherGui

bool ViewProviderSketch3D::startTypedPlacement(const QString& text)
{
    if (!edit || !edit->viewer || edit->tool == Tool::None || edit->tool == Tool::Fillet) {
        return false;
    }
    if (!edit->placing) {
        // where the pointer would place, toward which a length runs
        Placed under;
        Base::Vector3d at = edit->origin;
        if (place(edit->lastPos, under)) {
            at = under.point;
        }
        Base::Placement plane(Base::Placement(editingTransform())
                              * Base::Placement(at, workingPlane().getRotation()));
        edit->placing = new Typed3DSession(
            edit->viewer,
            plane,
            (at - edit->origin).Length(),
            Base::Unit::Length,
            [this](const QString& typed, QString* why) {
                return placeTyped(typed, why);
            },
            [this]() {
                if (edit) {
                    edit->placing = nullptr;
                }
            });
        // three offsets are no value, but Enter takes them: said as what they do
        edit->placing->setTextReader([](const QString& typed, QString& says) {
            double v[3];
            if (!threeOffsets(typed, v)) {
                return false;
            }
            says = QObject::tr("A point at %1, %2, %3 from the last")
                       .arg(QString::number(v[0], 'g', 6),
                            QString::number(v[1], 'g', 6),
                            QString::number(v[2], 'g', 6));
            return true;
        });
    }
    edit->placing->setText(text);
    return true;
}

bool ViewProviderSketch3D::isTypingPlacement() const
{
    return edit && edit->placing;
}

bool ViewProviderSketch3D::placeTyped(const QString& text, QString* why)
{
    auto fail = [&](const QString& message) {
        if (why) {
            *why = message;
        }
        return false;
    };
    if (!edit || edit->tool == Tool::None || edit->tool == Tool::Fillet) {
        return fail(QObject::tr("No tool placing points"));
    }
    // one value, a length; else three, the offsets
    double length = 0.;
    Base::Vector3d offset;
    bool isLength = quantityOf(text, length);
    if (!isLength) {
        double v[3];
        if (!threeOffsets(text, v)) {
            return fail(QObject::tr("A length, or three offsets: dx, dy, dz"));
        }
        offset = Base::Vector3d(v[0], v[1], v[2]);
    }
    else if (length <= 0.) {
        return fail(QObject::tr("A length above zero"));
    }
    const Base::Vector3d base = edit->origin;
    Placed placed;
    if (isLength) {
        Placed under;
        Base::Vector3d dir;
        if (place(edit->lastPos, under)) {
            dir = under.point - base;
        }
        if (dir.Length() < Precision::Confusion()) {
            dir = Base::Vector3d(1, 0, 0);
            edit->rotation.multVec(dir, dir);
        }
        placed.point = base + dir.Normalize() * length;
        edit->typedLength = length;
    }
    else {
        Base::Vector3d d;
        edit->rotation.multVec(offset, d);
        placed.point = base + d;
        edit->typedOffset = offset;
        edit->typedOffsetSet = true;
    }
    addPlaced(placed);
    edit->typedLength = -1.;
    edit->typedOffsetSet = false;
    return true;
}

void ViewProviderSketch3D::draggerStarted(void* data, SoDragger* /*dragger*/)
{
    auto vp = static_cast<ViewProviderSketch3D*>(data);
    if (vp->edit) {
        vp->edit->turnStart = vp->edit->rotation;
    }
}

void ViewProviderSketch3D::draggerFinished(void* data, SoDragger* /*dragger*/)
{
    // A ring turned the manipulator: about which of its own axes and how far, the
    // angle offered for typing (the S2 leftover).
    auto vp = static_cast<ViewProviderSketch3D*>(data);
    auto& e = vp->edit;
    if (!e || !e->viewer) {
        return;
    }
    Base::Rotation delta = e->turnStart.inverse() * e->rotation;
    Base::Vector3d axis;
    double angle;
    delta.getValue(axis, angle);
    if (std::fabs(angle) < 1e-9) {
        return;
    }
    int k = 0;
    for (int i = 1; i < 3; ++i) {
        if (std::fabs(axis[i]) > std::fabs(axis[k])) {
            k = i;
        }
    }
    double degrees = Base::toDegrees<double>(axis[k] < 0 ? -angle : angle);
    e->turnAxis = k;
    if (e->turning) {
        e->turning->finish();
    }
    Base::Placement plane(Base::Placement(vp->editingTransform())
                          * Base::Placement(e->origin, e->turnStart));
    e->turning = new Typed3DSession(
        e->viewer,
        plane,
        degrees,
        Base::Unit::Angle,
        [vp](const QString& typed, QString* why) {
            return vp->turnTyped(typed, why);
        },
        [vp]() {
            if (vp->edit) {
                vp->edit->turning = nullptr;
            }
        });
}

void ViewProviderSketch3D::turnByRing(int axis, double degrees)
{
    if (!edit || axis < 0 || axis > 2) {
        return;
    }
    edit->turnStart = edit->rotation;
    Base::Vector3d a(axis == 0, axis == 1, axis == 2);
    edit->rotation = edit->turnStart * Base::Rotation(a, Base::toRadians(degrees));
    updateManipulator();
    draggerFinished(this, edit->dragger);
}

bool ViewProviderSketch3D::turnTyped(const QString& text, QString* why)
{
    if (!edit || edit->turnAxis < 0) {
        return false;
    }
    double degrees;
    try {
        Base::Quantity q = Base::Quantity::parse(text.trimmed().toStdString());
        degrees =
            q.getUnit() == Base::Unit::Angle ? q.getValueAs(Base::Quantity::Degree) : q.getValue();
    }
    catch (const Base::Exception&) {
        if (why) {
            *why = QObject::tr("An angle");
        }
        return false;
    }
    Base::Vector3d a(edit->turnAxis == 0, edit->turnAxis == 1, edit->turnAxis == 2);
    edit->rotation = edit->turnStart * Base::Rotation(a, Base::toRadians(degrees));
    edit->keptPlane = -1;
    updateManipulator();
    return true;
}

bool ViewProviderSketch3D::isTypingTurn() const
{
    return edit && edit->turning;
}

int ViewProviderSketch3D::keepPlane()
{
    if (!edit) {
        return -1;
    }
    auto sketch = getSketch3DObject();
    Base::Placement plane = workingPlane();
    double q0, q1, q2, q3;
    plane.getRotation().getValue(q0, q1, q2, q3);
    const Base::Vector3d& o = plane.getPosition();
    int id = sketch->Geometry.getSize();
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Keep 3D sketch plane"));
    Gui::Command::doCommand(
        Gui::Command::Doc,
        "_p = Part.Plane()\n"
        "_p.transform(App.Placement(App.Vector(%.17g, %.17g, %.17g), "
        "App.Rotation(%.17g, %.17g, %.17g, "
        "%.17g)).toMatrix())\n"
        "%s.addConstraint(Sketcher.Constraint('Block', %s.addGeometry(_p, True)))\n"
        "del _p",
        o.x,
        o.y,
        o.z,
        q0,
        q1,
        q2,
        q3,
        Gui::Command::getObjectCmd(sketch).c_str(),
        Gui::Command::getObjectCmd(sketch).c_str());
    Gui::Command::commitCommand();
    activateKeptPlane(id);
    return id;
}

void ViewProviderSketch3D::activateKeptPlane(int geoId)
{
    if (!edit) {
        return;
    }
    auto geo = geoId >= 0 ? getSketch3DObject()->getGeometry(geoId) : nullptr;
    if (!geo || !geo->isDerivedFrom<Part::GeomPlane>()) {
        edit->keptPlane = -1;
        draw();
        return;
    }
    edit->keptPlane = geoId;
    auto pln = Handle(Geom_Plane)::DownCast(geo->handle());
    const gp_Ax3& ax = pln->Position();
    auto vec = [](const gp_XYZ& v) {
        return Base::Vector3d(v.X(), v.Y(), v.Z());
    };
    edit->origin = vec(ax.Location().XYZ());
    edit->rotation = Base::Rotation::makeRotationByAxes(vec(ax.XDirection().XYZ()),
                                                        vec(ax.YDirection().XYZ()),
                                                        vec(ax.Direction().XYZ()));
    edit->plane = Plane::XY;
    edit->heightLock = false;
    updateManipulator();
    draw();
}

int ViewProviderSketch3D::activeKeptPlane() const
{
    return edit ? edit->keptPlane : -1;
}

bool ViewProviderSketch3D::takePlaneAt(const SbVec2s& pos, SoPickedPoint* pp)
{
    // A kept plane: active again.
    if (pp && pp->getPath()->getTail() == edit->curveLines) {
        auto detail = dynamic_cast<const SoLineDetail*>(pp->getDetail());
        int part = detail ? detail->getLineIndex() : -1;  // the polyline: a curve
        if (part >= 0 && part < int(edit->curveRefs.size())) {
            auto geo = getSketch3DObject()->getGeometry(edit->curveRefs[part]);
            if (geo && geo->isDerivedFrom<Part::GeomPlane>()) {
                activateKeptPlane(edit->curveRefs[part]);
                return true;
            }
        }
    }
    // A planar face or a datum plane of the model: the working plane, through the
    // point pressed (SOLIDWORKS' Ctrl+click [S 8]).
    Placed placed;
    if (!place(pos, placed) || placed.snap.kind != Snap::Kind::ModelPlane) {
        return false;
    }
    auto sketch = getSketch3DObject();
    Part::TopoShape shape;
    try {
        shape = Part::Feature::getTopoShape(placed.snap.obj, placed.snap.sub.c_str(), true);
    }
    catch (...) {
        return false;
    }
    if (shape.isNull() || shape.getShape().ShapeType() != TopAbs_FACE) {
        return false;
    }
    shape.transformShape(sketch->Placement.getValue().inverse().toMatrix(), false, true);
    BRepAdaptor_Surface surf(TopoDS::Face(shape.getShape()));
    gp_Pln pln = surf.Plane();
    const gp_Ax3& ax = pln.Position();
    auto vec = [](const gp_XYZ& v) {
        return Base::Vector3d(v.X(), v.Y(), v.Z());
    };
    edit->keptPlane = -1;
    edit->origin = placed.point;
    edit->rotation = Base::Rotation::makeRotationByAxes(vec(ax.XDirection().XYZ()),
                                                        vec(ax.YDirection().XYZ()),
                                                        vec(ax.Direction().XYZ()));
    edit->plane = Plane::XY;
    edit->heightLock = false;
    updateManipulator();
    draw();
    return true;
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
    // a number typed while placing points: its entry (the S2 leftover)
    if (edit && edit->tool != Tool::None && edit->tool != Tool::Fillet && !edit->placing) {
        QString ch;
        if (key >= SoKeyboardEvent::NUMBER_0 && key <= SoKeyboardEvent::NUMBER_9) {
            ch = QString::number(key - SoKeyboardEvent::NUMBER_0);
        }
        else if (key >= SoKeyboardEvent::PAD_0 && key <= SoKeyboardEvent::PAD_9) {
            ch = QString::number(key - SoKeyboardEvent::PAD_0);
        }
        else if (key == SoKeyboardEvent::MINUS || key == SoKeyboardEvent::PAD_SUBTRACT) {
            ch = QStringLiteral("-");
        }
        else if (key == SoKeyboardEvent::PERIOD || key == SoKeyboardEvent::PAD_PERIOD) {
            ch = QStringLiteral(".");
        }
        if (!ch.isEmpty()) {
            if (pressed) {
                startTypedPlacement(ch);
            }
            return true;
        }
    }
    if (edit && edit->tool != Tool::None && key == SoKeyboardEvent::ESCAPE) {
        // Escape keeps what is placed (memory: Escape means leave, not undo).
        if (pressed) {
            finishTool();
        }
        return true;
    }
    if (edit && edit->tool == Tool::None && key == SoKeyboardEvent::DELETE
        && (!edit->selection.empty() || !edit->selectedConstraints.empty())) {
        if (pressed) {
            deleteSelection();
        }
        return true;
    }
    return PartGui::ViewProviderPart::keyPressed(pressed, key);
}

bool ViewProviderSketch3D::mouseMove(const SbVec2s& cursorPos, Gui::ViewerContext* viewer)
{
    if (edit && edit->dragCandidate && viewer && (viewer->mouseButtons() & Qt::LeftButton)) {
        return dragTo(cursorPos);
    }
    return move(cursorPos);
}

bool ViewProviderSketch3D::mouseButtonPressed(int button,
                                              bool pressed,
                                              const SbVec2s& cursorPos,
                                              const Gui::ViewerContext* viewer)
{
    if (button == SoMouseButtonEvent::BUTTON1 && pressed) {
        bool ctrl = viewer && (viewer->keyboardModifiers() & Qt::ControlModifier);
        return press(cursorPos, ctrl);
    }
    if (button == SoMouseButtonEvent::BUTTON1 && !pressed && edit && edit->dragCandidate) {
        return release(cursorPos);
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
    if (edit->tool == Tool::SplineOnFace && edit->placed.size() >= 2) {
        makeSplineOnFace();
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
        case Tool::Circle:
            text = edit->placed.empty()
                ? QObject::tr("3D circle: click the centre; Escape ends")
                : QObject::tr("3D circle: click a point on the rim, in the working plane");
            break;
        case Tool::Circle3Point:
            text = QObject::tr("3D circle by three points: click three points on it; Escape ends");
            break;
        case Tool::Arc:
            text = edit->placed.empty() ? QObject::tr("3D arc: click the centre; Escape ends")
                : edit->placed.size() == 1
                ? QObject::tr("3D arc: click the start, in the working plane")
                : QObject::tr("3D arc: click the end; it runs the way the pointer turns");
            break;
        case Tool::Arc3Point:
            text = QObject::tr(
                "3D arc by three points: click the start, the end, then a point on the arc");
            break;
        case Tool::Fillet:
            text = QObject::tr("3D fillet: click a corner where two lines meet; Escape ends");
            break;
        case Tool::SplineOnFace:
            text = edit->placed.empty()
                ? QObject::tr("Spline on face: click points on a face of the model; Escape ends")
                : QObject::tr("Spline on face: click more points on the same face; Escape or a "
                              "right click makes it");
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
            int part = detail ? detail->getLineIndex() : -1;  // the polyline: a curve
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

void ViewProviderSketch3D::constrainTo(const Snap& snap, int geoId, int pos, bool onKeptPlane)
{
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    auto run = [&](const std::string& code) {
        Gui::Command::doCommand(Gui::Command::Doc, "%s", code.c_str());
    };
    // The external reference a model snap needs, reused when the sketch has it.
    auto external = [&]() {
        return referenceTo(snap.obj, snap.sub);
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
        case Snap::Kind::ModelPlane:
        case Snap::Kind::Model: {
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
        case Snap::Kind::None:
            // On the kept plane, held there (section 6).
            if (onKeptPlane && edit && edit->keptPlane >= 0) {
                snprintf(buf,
                         sizeof(buf),
                         "%s.addConstraint(Sketcher.Constraint('PointOnObject', %d, %d, %d))",
                         obj.c_str(),
                         geoId,
                         pos,
                         edit->keptPlane);
                run(buf);
            }
            break;
    }
}

int ViewProviderSketch3D::referenceTo(App::DocumentObject* target, const std::string& sub)
{
    auto sketch = getSketch3DObject();
    const auto& objs = sketch->ExternalGeometry.getValues();
    const auto& subs = sketch->ExternalGeometry.getSubValues();
    for (std::size_t i = 0; i < objs.size(); ++i) {
        if (objs[i] == target && i < subs.size() && subs[i] == sub) {
            return Sketcher::Sketch3DObject::RefExt - int(i);
        }
    }
    int before = sketch->ExternalGeo.getSize();
    try {
        Gui::Command::doCommand(Gui::Command::Doc,
                                "%s.addExternal(%s, '%s')",
                                Gui::Command::getObjectCmd(sketch).c_str(),
                                Gui::Command::getObjectCmd(target).c_str(),
                                sub.c_str());
    }
    catch (const Base::Exception& e) {
        e.reportException();
    }
    return sketch->ExternalGeo.getSize() > before
        ? Sketcher::Sketch3DObject::RefExt - (sketch->ExternalGeo.getSize() - 1)
        : int(Sketcher::GeoEnum::GeoUndef);
}

void ViewProviderSketch3D::constrainOn(const Snap& snap, int geoId)
{
    // A rim point the circle or arc goes through: the point it was snapped to lies on
    // it. A model vertex becomes a reference first, as in constrainTo.
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    int point = Sketcher::GeoEnum::GeoUndef;
    int pos = 1;
    if (snap.kind == Snap::Kind::SketchPoint) {
        point = snap.geoId;
        pos = int(snap.pos);
    }
    else if (snap.kind == Snap::Kind::ModelVertex) {
        point = referenceTo(snap.obj, snap.sub);
    }
    if (point == Sketcher::GeoEnum::GeoUndef) {
        return;
    }
    Gui::Command::doCommand(Gui::Command::Doc,
                            "%s.addConstraint(Sketcher.Constraint('PointOnObject', %d, %d, %d))",
                            obj.c_str(),
                            point,
                            pos,
                            geoId);
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
            // what was typed for it, kept: its length, or its offsets while the
            // manipulator's frame is the sketch's
            char typed[192];
            if (edit->typedLength > 0.) {
                snprintf(typed,
                         sizeof(typed),
                         "%s.addConstraint(Sketcher.Constraint('Distance', %d, %.15g))",
                         obj.c_str(),
                         id,
                         edit->typedLength);
                run(typed);
            }
            else if (edit->typedOffsetSet && edit->rotation.isIdentity()) {
                static const char* names[3] = {"DistanceX", "DistanceY", "DistanceZ"};
                const double v[3] = {edit->typedOffset.x, edit->typedOffset.y, edit->typedOffset.z};
                for (int k = 0; k < 3; ++k) {
                    if (std::fabs(v[k]) > Precision::Confusion()) {
                        snprintf(typed,
                                 sizeof(typed),
                                 "%s.addConstraint(Sketcher.Constraint('%s', %d, 1, %d, 2, %.15g))",
                                 obj.c_str(),
                                 names[k],
                                 id,
                                 id,
                                 v[k]);
                        run(typed);
                    }
                }
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
        case Tool::Circle:
        case Tool::Circle3Point:
        case Tool::Arc:
        case Tool::Arc3Point: {
            if (!edit->placed.empty()
                && (placed.point - edit->placed.back().point).Length() < Precision::Confusion()) {
                break;  // the same point again
            }
            if (edit->tool == Tool::Arc) {
                edit->trackSweep(placed.point);
            }
            edit->placed.push_back(placed);
            if (edit->tool == Tool::Arc && edit->placed.size() == 2) {
                // the arc lies in the working plane as it is at its start
                Base::Vector3d n(0, 0, 1);
                workingPlane().getRotation().multVec(n, n);
                edit->arcNormal = n;
                edit->arcSweep = edit->arcLast = 0.;
            }
            std::size_t need = edit->tool == Tool::Circle ? 2 : 3;
            if (edit->placed.size() >= need) {
                if (makeCircular()) {
                    edit->placed.clear();
                }
                else {
                    edit->placed.pop_back();  // click the last point again
                }
            }
            break;
        }
        case Tool::SplineOnFace: {
            // on a face of the model, the same face each time
            bool onFace =
                placed.snap.kind == Snap::Kind::Model || placed.snap.kind == Snap::Kind::ModelPlane;
            const Snap* first = edit->placed.empty() ? nullptr : &edit->placed.front().snap;
            if (!onFace
                || (first && (first->obj != placed.snap.obj || first->sub != placed.snap.sub))) {
                Gui::getMainWindow()->showMessage(
                    first ? QObject::tr("Click on the same face as the first point")
                          : QObject::tr("Click on a face of the model"),
                    5000);
                return;
            }
            if (edit->placed.empty()
                || (placed.point - edit->placed.back().point).Length() >= Precision::Confusion()) {
                edit->placed.push_back(placed);
            }
            break;
        }
        case Tool::Fillet:
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

bool ViewProviderSketch3D::circular(const Base::Vector3d* under, Circular& out) const
{
    if (!edit) {
        return false;
    }
    std::vector<Base::Vector3d> pts;
    for (const auto& p : edit->placed) {
        pts.push_back(p.point);
    }
    if (under) {
        pts.push_back(*under);
    }
    const double eps = Precision::Confusion();
    const double full = 2 * std::numbers::pi;
    switch (edit->tool) {
        case Tool::Circle:
        case Tool::Arc: {
            if (pts.size() < 2) {
                return false;
            }
            Base::Vector3d n(0, 0, 1);
            if (edit->tool == Tool::Arc && edit->placed.size() >= 2) {
                n = edit->arcNormal;
            }
            else {
                workingPlane().getRotation().multVec(n, n);
            }
            const Base::Vector3d& c = pts[0];
            Base::Vector3d r = pts[1] - c;
            r = r - n * (r * n);
            double len = r.Length();
            if (len < eps) {
                return false;
            }
            out.centre = c;
            out.normal = n;
            out.xdir = r / len;
            out.radius = len;
            // a circle, or the arc's circle while its start is picked
            out.closed = edit->tool == Tool::Circle || pts.size() < 3;
            if (out.closed) {
                out.sweep = full;
                return true;
            }
            Base::Vector3d e = pts[2] - c;
            e = e - n * (e * n);
            if (e.Length() < eps) {
                return false;
            }
            Base::Vector3d y = n % out.xdir;
            double a = std::atan2(e * y, e * out.xdir);
            if (edit->arcSweep >= 0.) {
                if (a <= 0.) {
                    a += full;
                }
            }
            else if (a >= 0.) {
                a -= full;
            }
            out.sweep = a;
            return std::fabs(a) > 1e-6 && std::fabs(a) < full - 1e-6;
        }
        case Tool::Circle3Point:
        case Tool::Arc3Point: {
            if (pts.size() < 3) {
                return false;
            }
            // The circle through a, b and m; an arc from a to b through m.
            const Base::Vector3d &a = pts[0], &b = pts[1], &m = pts[2];
            Base::Vector3d ab = b - a, am = m - a, w = ab % am;
            if (w.Length() < 1e-9 * ab.Length() * am.Length() || w.Length() < eps * eps) {
                return false;  // in a line
            }
            out.centre = a + (w % ab * am.Sqr() + am % w * ab.Sqr()) / (2 * w.Sqr());
            out.radius = (a - out.centre).Length();
            out.xdir = (a - out.centre) / out.radius;
            out.closed = edit->tool == Tool::Circle3Point;
            if (out.closed) {
                out.normal = Base::Vector3d(w).Normalize();
                out.sweep = full;
                return true;
            }
            // counterclockwise about am x ab, a reaches m before b
            out.normal = (am % ab).Normalize();
            Base::Vector3d y = out.normal % out.xdir, e = b - out.centre;
            double angle = std::atan2(e * y, e * out.xdir);
            out.sweep = angle <= 0. ? angle + full : angle;
            return true;
        }
        default:
            return false;
    }
}

bool ViewProviderSketch3D::makeCircular()
{
    Circular cir;
    if (!circular(nullptr, cir)) {
        Gui::getMainWindow()->showMessage(
            QObject::tr("No circle through these points: they are in a line"),
            5000);
        return false;
    }
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    const char* cons = construction ? "True" : "False";
    const auto& placed = edit->placed;
    int id = sketch->Geometry.getSize();
    char radius[64];
    snprintf(radius, sizeof(radius), "%.17g", cir.radius);
    std::string geo;
    if (cir.closed) {
        geo = "Part.Circle(" + pyVec(cir.centre) + ", " + pyVec(cir.normal) + ", " + radius + ")";
    }
    else if (edit->tool == Tool::Arc3Point) {
        // through the points as clicked: from the first to the second by the third
        geo = "Part.ArcOfCircle(" + pyVec(placed[0].point) + ", " + pyVec(placed[2].point) + ", "
            + pyVec(placed[1].point) + ")";
    }
    else {
        geo = "Part.ArcOfCircle(" + pyVec(cir.at(0.)) + ", " + pyVec(cir.at(cir.sweep / 2)) + ", "
            + pyVec(cir.at(cir.sweep)) + ")";
    }
    Gui::Command::openCommand(cir.closed ? QT_TRANSLATE_NOOP("Command", "Add 3D sketch circle")
                                         : QT_TRANSLATE_NOOP("Command", "Add 3D sketch arc"));
    Gui::Command::doCommand(Gui::Command::Doc,
                            "%s.addGeometry(%s, %s)",
                            obj.c_str(),
                            geo.c_str(),
                            cons);
    // The snaps: a centre, start and end tie as a line's ends do; a point the curve
    // was drawn through lies on it.
    switch (edit->tool) {
        case Tool::Circle:
            constrainTo(placed[0].snap, id, 3, false);
            constrainOn(placed[1].snap, id);
            break;
        case Tool::Arc:
            constrainTo(placed[0].snap, id, 3, false);
            constrainTo(placed[1].snap, id, 1, false);
            constrainTo(placed[2].snap, id, 2, false);
            break;
        case Tool::Arc3Point:
            constrainTo(placed[0].snap, id, 1, false);
            constrainTo(placed[1].snap, id, 2, false);
            constrainOn(placed[2].snap, id);
            break;
        default:
            for (const auto& p : placed) {
                constrainOn(p.snap, id);
            }
            break;
    }
    // On a kept plane, and held by nothing else: in it, by its centre on the plane and
    // its axis along the plane's normal (section 4.4, arc / circle in a plane).
    bool held = std::any_of(placed.begin(), placed.end(), [](const Placed& p) {
        return p.snap.kind != Snap::Kind::None;
    });
    if (edit->keptPlane >= 0 && !held) {
        Gui::Command::doCommand(
            Gui::Command::Doc,
            "%s.addConstraint(Sketcher.Constraint('PointOnObject', %d, 3, %d))\n"
            "%s.addConstraint(Sketcher.Constraint('Parallel', %d, %d))",
            obj.c_str(),
            id,
            edit->keptPlane,
            obj.c_str(),
            id,
            edit->keptPlane);
    }
    Gui::Command::commitCommand();
    return true;
}

bool ViewProviderSketch3D::makeSplineOnFace()
{
    // Its points in the face's (u, v), a 2D spline through them there: on the face
    // exactly (Part.CurveOnSurface), solved there, following the face (decision f).
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    const Snap& face = edit->placed.front().snap;
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Add 3D sketch spline on face"));
    int ext = referenceTo(face.obj, face.sub);
    auto surfaceGeo = ext != Sketcher::GeoEnum::GeoUndef
        ? Base::freecad_dynamic_cast<Part::GeomSurface>(sketch->getGeometry(ext))
        : nullptr;
    if (!surfaceGeo) {
        Gui::Command::abortCommand();
        Gui::getMainWindow()->showMessage(QObject::tr("That face cannot be referenced"), 5000);
        return false;
    }
    auto surface = Handle(Geom_Surface)::DownCast(surfaceGeo->handle());
    std::vector<gp_Pnt2d> uvs;
    for (const auto& p : edit->placed) {
        double u = 0., v = 0.;
        GeomAPI_ProjectPointOnSurf proj(gp_Pnt(p.point.x, p.point.y, p.point.z), surface);
        if (proj.NbPoints() > 0) {
            proj.LowerDistanceParameters(u, v);
        }
        // across a periodic surface's seam, the way nearest the point before
        if (!uvs.empty()) {
            if (surface->IsUPeriodic()) {
                double period = surface->UPeriod();
                u += period * std::round((uvs.back().X() - u) / period);
            }
            if (surface->IsVPeriodic()) {
                double period = surface->VPeriod();
                v += period * std::round((uvs.back().Y() - v) / period);
            }
        }
        uvs.emplace_back(u, v);
    }
    std::string list = "[";
    char buf[96];
    for (std::size_t i = 0; i < uvs.size(); ++i) {
        snprintf(buf,
                 sizeof(buf),
                 "%sApp.Base.Vector2d(%.17g, %.17g)",
                 i ? ", " : "",
                 uvs[i].X(),
                 uvs[i].Y());
        list += buf;
    }
    list += "]";
    int id = sketch->Geometry.getSize();
    int extIndex = Sketcher::Sketch3DObject::RefExt - ext;
    try {
        Gui::Command::doCommand(
            Gui::Command::Doc,
            "_bs = Part.Geom2d.BSplineCurve2d()\n"
            "_bs.interpolate(%s)\n"
            "%s.addGeometry(Part.CurveOnSurface(_bs, %s.ExternalGeo[%d]), %s)\n"
            "%s.addConstraint(Sketcher.Constraint('PointOnObject', %d, 0, %d))\n"
            "del _bs",
            list.c_str(),
            obj.c_str(),
            obj.c_str(),
            extIndex,
            construction ? "True" : "False",
            obj.c_str(),
            id,
            ext);
        // its points, at its knots, as sketch points held to it
        for (std::size_t k = 0; k < uvs.size(); ++k) {
            gp_Pnt p = surface->Value(uvs[k].X(), uvs[k].Y());
            int pid = sketch->Geometry.getSize();
            Gui::Command::doCommand(
                Gui::Command::Doc,
                "%s.addGeometry(Part.Point(%s), True)\n"
                "%s.addConstraint(Sketcher.Constraint('InternalAlignment:Sketcher::"
                "BSplineKnotPoint', %d, 1, %d, %d))",
                obj.c_str(),
                pyVec(Base::Vector3d(p.X(), p.Y(), p.Z())).c_str(),
                obj.c_str(),
                pid,
                id,
                int(k));
        }
        Gui::Command::commitCommand();
        return true;
    }
    catch (const Base::Exception& e) {
        Gui::Command::abortCommand();
        Gui::getMainWindow()->showMessage(QString::fromUtf8(e.what()), 5000);
        return false;
    }
}

void ViewProviderSketch3D::updatePreview()
{
    if (!edit) {
        return;
    }
    std::vector<SbVec3f> pts;
    Placed under;
    bool have = edit->tool != Tool::None && edit->viewer && place(edit->lastPos, under);
    auto add = [&](const Base::Vector3d& v) {
        pts.emplace_back(float(v.x), float(v.y), float(v.z));
    };
    Circular cir;
    if (have && edit->tool == Tool::Arc) {
        edit->trackSweep(under.point);
    }
    if (have && circular(&under.point, cir)) {
        // from the centre out to the start and round, or round through the points
        if (edit->tool == Tool::Circle || edit->tool == Tool::Arc) {
            add(cir.centre);
        }
        int n = std::max(8, int(std::ceil(std::fabs(cir.sweep) / (2 * std::numbers::pi) * 64)));
        for (int i = 0; i <= n; ++i) {
            add(cir.at(cir.sweep * i / n));
        }
    }
    else {
        for (const auto& p : edit->placed) {
            add(p.point);
        }
        if (have && !pts.empty()) {
            add(under.point);
        }
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

// ---------------------------------------------------------------------------------
// Selection, constraints from it, dimension labels and their values

namespace
{
// A Python string literal for a recorded command.
std::string pyQuote(const std::string& text)
{
    std::string out = "u'";
    for (char ch : text) {
        if (ch == '\\' || ch == '\'') {
            out += '\\';
        }
        if (ch == '\n') {
            out += "\\n";
            continue;
        }
        out += ch;
    }
    return out + "'";
}
}  // namespace

namespace SketcherGui
{

/** A dimension's value edited in place, as the 2D sketch's DatumEditSession does it
 * (docs/SketcherPort.md "One editor for a constraint's value"), on the same editor:
 * Enter applies, Escape too unless DatumEscapeTakesBack, Tab applies and moves to the
 * next dimension, a press elsewhere applies. What is applied is one undo step.
 */
class Datum3DSession: public QObject
{
public:
    Datum3DSession(ViewProviderSketch3D* vp, Gui::ViewerContext* viewer, int index)
        : vp(vp)
        , viewer(viewer)
    {
        // The command that made the dimension stays open until its value is in:
        // the two are one undo step, as in the 2D sketch. Otherwise the entry is a
        // step of its own.
        App::AutoTransaction::setEnable(false);
        if (!App::GetApplication().getActiveTransaction()) {
            Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Edit 3D sketch datum"));
        }
        show(index);
    }

    void show(int index)
    {
        auto sketch = vp->getSketch3DObject();
        const Sketcher::Constraint* c = sketch->Constraints[index];
        Base::Placement plane =
            Base::Placement(vp->editingTransform()) * vp->edit->labelPlanes[index];
        editor = std::make_unique<Gui::DatumValueEditor>(viewer, plane);
        // a distance's kind with its value is no value, but Enter takes it
        editor->setTextReader([](const QString& typed, QString& says) {
            auto match = kindText().match(typed);
            double v = 0;
            if (!match.hasMatch() || !quantityOf(match.captured(2), v)) {
                return false;
            }
            const QChar kind = match.captured(1).at(0).toUpper();
            says = kind == QLatin1Char('D')
                ? QObject::tr("Distance %1").arg(match.captured(2).trimmed())
                : QObject::tr("Distance %1 %2").arg(kind).arg(match.captured(2).trimmed());
            return true;
        });
        editor->setDrivingIcons(
            Gui::BitmapFactory().iconFromTheme("Sketcher_Toggle_Constraint_Driving"),
            Gui::BitmapFactory().iconFromTheme("Sketcher_Toggle_Constraint_Driven"));
        Gui::DatumValueEditor::Target target;
        target.label = vp->edit->labels[index];
        target.value = c->getValue();
        target.unit = Base::Unit::Length;
        if (c->Type == Sketcher::Angle) {
            target.value = Base::toDegrees<double>(target.value);
            target.unit = Base::Unit::Angle;
        }
        target.path = sketch->Constraints.createPath(index);
        if (auto info = sketch->getExpression(target.path); info.expression) {
            target.expression = QString::fromStdString(info.expression->toString());
        }
        target.driving = c->isDriving ? 1 : 0;
        target.nameShown = true;
        target.name = QString::fromStdString(c->Name);
        tag = c->getTag();
        editor->installKeyFilter(this);
        editor->edit(target);
    }

    int current() const
    {
        const auto& all = vp->getSketch3DObject()->Constraints.getValues();
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i]->getTag() == tag) {
                return int(i);
            }
        }
        return -1;
    }

    bool apply()
    {
        auto sketch = vp->getSketch3DObject();
        int index = current();
        if (index < 0) {
            return false;
        }
        if (!editor->isModified()) {
            return true;
        }
        if (int kind = retyped(index); kind != 0) {
            return kind > 0;
        }
        Gui::DatumValueEditor::Entry entry;
        QString why;
        if (!editor->read(entry, &why) && entry.driving != 0) {
            editor->showError(why);
            return false;
        }
        const Sketcher::Constraint* c = sketch->Constraints[index];
        try {
            const std::string obj = Gui::Command::getObjectCmd(sketch);
            bool driving = entry.driving != 0;
            if (driving != c->isDriving) {
                Gui::Command::doCommand(Gui::Command::Doc,
                                        "%s.setDriving(%d, %s)",
                                        obj.c_str(),
                                        index,
                                        driving ? "True" : "False");
            }
            if (driving) {
                std::string path = sketch->Constraints.createPath(index).toString();
                bool bound =
                    bool(sketch->getExpression(sketch->Constraints.createPath(index)).expression);
                if (entry.isExpression) {
                    std::string expr = entry.expression->toString();
                    Gui::Command::doCommand(Gui::Command::Doc,
                                            "%s.setExpression('%s', %s)",
                                            obj.c_str(),
                                            path.c_str(),
                                            pyQuote(expr).c_str());
                }
                else {
                    if (bound) {
                        Gui::Command::doCommand(Gui::Command::Doc,
                                                "%s.setExpression('%s', None)",
                                                obj.c_str(),
                                                path.c_str());
                    }
                    Gui::Command::doCommand(Gui::Command::Doc,
                                            "%s.setDatum(%d, App.Units.Quantity('%.15g %s'))",
                                            obj.c_str(),
                                            index,
                                            entry.value.getValue(),
                                            entry.value.getUnit().getString().c_str());
                }
                if (entry.isExpression) {
                    sketch->getDocument()->recomputeFeature(sketch);
                }
            }
            std::string name = entry.name.toStdString();
            if (name != sketch->Constraints[index]->Name) {
                Gui::Command::doCommand(Gui::Command::Doc,
                                        "%s.renameConstraint(%d, %s)",
                                        obj.c_str(),
                                        index,
                                        pyQuote(name).c_str());
            }
            applied = true;
            show(index);
            return true;
        }
        catch (const Base::Exception& e) {
            editor->showError(QString::fromUtf8(e.what()));
            return false;
        }
    }

    /** A distance's kind picked in its entry: "x 20", "y 20", "z 20" (or "d 20" back to
     * the distance itself) makes it that kind with that value. 0 when the text is no
     * such thing, 1 done, -1 refused (and said why).
     */
    int retyped(int index)
    {
        auto match = kindText().match(editor->text());
        if (!match.hasMatch()) {
            return 0;
        }
        auto sketch = vp->getSketch3DObject();
        const Sketcher::Constraint* c = sketch->Constraints[index];
        bool distance = c->Type == Sketcher::Distance || c->Type == Sketcher::DistanceX
            || c->Type == Sketcher::DistanceY || c->Type == Sketcher::DistanceZ;
        bool line =
            c->Second == Sketcher::GeoEnum::GeoUndef && c->FirstPos == Sketcher::PointPos::none;
        bool points =
            c->FirstPos != Sketcher::PointPos::none && c->SecondPos != Sketcher::PointPos::none;
        Base::Quantity q;
        if (!distance || !(line || points) || !editor->parse(match.captured(2), q)) {
            editor->showError(QObject::tr("x, y, z or d before a value: a line's or two points' "
                                          "distance along that axis, or itself"));
            return -1;
        }
        QChar k = match.captured(1).at(0).toLower();
        const char* type = k == QLatin1Char('x') ? "DistanceX"
            : k == QLatin1Char('y')              ? "DistanceY"
            : k == QLatin1Char('z')              ? "DistanceZ"
                                                 : "Distance";
        // what the geometry measures that way now gives the sign a bare value takes
        Base::Vector3d a, b;
        if (line) {
            vp->pointOf(c->First, Sketcher::PointPos::start, a);
            vp->pointOf(c->First, Sketcher::PointPos::end, b);
        }
        else {
            vp->pointOf(c->First, c->FirstPos, a);
            vp->pointOf(c->Second, c->SecondPos, b);
        }
        Base::Vector3d d = b - a;
        double now = k == QLatin1Char('x') ? d.x
            : k == QLatin1Char('y')        ? d.y
            : k == QLatin1Char('z')        ? d.z
                                           : d.Length();
        double value = q.getValue();
        if (value > 0 && now < 0) {
            value = -value;
        }
        std::string args = line ? std::to_string(c->First)
                                : std::to_string(c->First) + ", " + std::to_string(int(c->FirstPos))
                + ", " + std::to_string(c->Second) + ", " + std::to_string(int(c->SecondPos));
        const std::string obj = Gui::Command::getObjectCmd(sketch);
        try {
            Gui::Command::doCommand(Gui::Command::Doc,
                                    "%s.delConstraint(%d)\n"
                                    "%s.addConstraint(Sketcher.Constraint('%s', %s, %.15g))",
                                    obj.c_str(),
                                    index,
                                    obj.c_str(),
                                    type,
                                    args.c_str(),
                                    value);
        }
        catch (const Base::Exception& e) {
            editor->showError(QString::fromUtf8(e.what()));
            return -1;
        }
        applied = true;
        show(sketch->Constraints.getSize() - 1);
        return 1;
    }

    void move(int step)
    {
        auto sketch = vp->getSketch3DObject();
        int n = sketch->Constraints.getSize();
        int index = current();
        for (int k = 1; k <= n; ++k) {
            int next = ((index + step * k) % n + n) % n;
            if (vp->edit->labels.count(next)) {
                show(next);
                return;
            }
        }
    }

    void finish(bool accept)
    {
        if (ended) {
            return;
        }
        ended = true;
        if (accept) {
            apply();
        }
        if (editor) {
            editor->close();
        }
        if (accept) {
            Gui::Command::commitCommand();
        }
        else {
            Gui::Command::abortCommand();
        }
        vp->edit->datum = nullptr;
        deleteLater();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (ended || event->type() != QEvent::KeyPress) {
            return QObject::eventFilter(watched, event);
        }
        auto key = static_cast<QKeyEvent*>(event);
        if (editor->isCompleting()) {
            return false;
        }
        switch (key->key()) {
            case Qt::Key_Escape:
                if (App::GetApplication()
                        .GetParameterGroupByPath(
                            "User parameter:BaseApp/Preferences/Mod/Sketcher/General")
                        ->GetBool("DatumEscapeTakesBack",
                                  Sketcher::SketcherParams::defaultDatumEscapeTakesBack())) {
                    finish(false);
                    return true;
                }
                [[fallthrough]];
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (apply()) {
                    finish(true);  // applied: the step is committed
                }
                return true;
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
                if (apply()) {
                    move(key->key() == Qt::Key_Tab && !(key->modifiers() & Qt::ShiftModifier) ? 1
                                                                                              : -1);
                }
                return true;
            default:
                return false;
        }
    }

private:
    ViewProviderSketch3D* vp;
    Gui::ViewerContext* viewer;
    std::unique_ptr<Gui::DatumValueEditor> editor;
    boost::uuids::uuid tag {};
    bool applied {false};
    bool ended {false};
};

}  // namespace SketcherGui

const std::vector<ViewProviderSketch3D::Element>& ViewProviderSketch3D::selectedElements() const
{
    static const std::vector<Element> none;
    return edit ? edit->selection : none;
}

const std::vector<int>& ViewProviderSketch3D::selectedConstraints() const
{
    static const std::vector<int> none;
    return edit ? edit->selectedConstraints : none;
}

void ViewProviderSketch3D::select(const Element& element, bool toggle)
{
    if (!edit) {
        return;
    }
    auto it = std::find(edit->selection.begin(), edit->selection.end(), element);
    if (it != edit->selection.end()) {
        if (toggle) {
            edit->selection.erase(it);
        }
    }
    else {
        edit->selection.push_back(element);
    }
    draw();
}

void ViewProviderSketch3D::selectConstraint(int index, bool toggle)
{
    if (!edit) {
        return;
    }
    auto it = std::find(edit->selectedConstraints.begin(), edit->selectedConstraints.end(), index);
    if (it != edit->selectedConstraints.end()) {
        if (toggle) {
            edit->selectedConstraints.erase(it);
        }
    }
    else {
        edit->selectedConstraints.push_back(index);
    }
    draw();
}

void ViewProviderSketch3D::clearSelection()
{
    if (edit && (!edit->selection.empty() || !edit->selectedConstraints.empty())) {
        edit->selection.clear();
        edit->selectedConstraints.clear();
        draw();
    }
}

bool ViewProviderSketch3D::pointOf(int geoId, Sketcher::PointPos pos, Base::Vector3d& point) const
{
    auto geo = getSketch3DObject()->getGeometry(geoId);
    if (!geo) {
        return false;
    }
    using Sketcher::PointPos;
    if (auto pt = Base::freecad_dynamic_cast<Part::GeomPoint>(geo)) {
        point = pt->getPoint();
        return pos == PointPos::start || pos == PointPos::mid;
    }
    if (pos == PointPos::mid) {
        if (auto conic = Base::freecad_dynamic_cast<Part::GeomConic>(geo)) {
            point = conic->getCenter();
            return true;
        }
        if (auto arc = Base::freecad_dynamic_cast<Part::GeomArcOfConic>(geo)) {
            point = arc->getCenter();
            return true;
        }
        return false;
    }
    if (auto bounded = Base::freecad_dynamic_cast<Part::GeomBoundedCurve>(geo)) {
        if (pos == PointPos::start) {
            point = bounded->getStartPoint();
            return true;
        }
        if (pos == PointPos::end) {
            point = bounded->getEndPoint();
            return true;
        }
    }
    return false;
}

namespace
{
// What a curve of the 3D sketch is, for the constraint commands.
enum class CurveKind
{
    Line,
    Circle,
    Arc,
    Plane,
    Other
};
CurveKind kindOf(const Part::Geometry* geo)
{
    if (!geo) {
        return CurveKind::Other;
    }
    if (geo->isDerivedFrom<Part::GeomLineSegment>()) {
        return CurveKind::Line;
    }
    if (geo->isDerivedFrom<Part::GeomCircle>()) {
        return CurveKind::Circle;
    }
    if (geo->isDerivedFrom<Part::GeomArcOfCircle>()) {
        return CurveKind::Arc;
    }
    if (geo->isDerivedFrom<Part::GeomPlane>()) {
        return CurveKind::Plane;
    }
    return CurveKind::Other;
}

Base::Vector3d planeNormal(const Part::Geometry* geo, Base::Vector3d& origin)
{
    auto pln = Handle(Geom_Plane)::DownCast(geo->handle());
    const gp_Ax3& ax = pln->Position();
    origin = Base::Vector3d(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
    return Base::Vector3d(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
}

// What an angle is measured from: a line's start and direction, or a plane's,
// circle's or arc's centre and normal.
bool axisOf(const Part::Geometry* geo, Base::Vector3d& base, Base::Vector3d& dir)
{
    switch (kindOf(geo)) {
        case CurveKind::Line: {
            auto seg = static_cast<const Part::GeomLineSegment*>(geo);
            base = seg->getStartPoint();
            dir = seg->getEndPoint() - base;
            return dir.Length() > 0.;
        }
        case CurveKind::Plane:
            dir = planeNormal(geo, base);
            return true;
        case CurveKind::Circle: {
            auto c = static_cast<const Part::GeomCircle*>(geo);
            base = c->getCenter();
            dir = c->getAxisDirection();
            return true;
        }
        case CurveKind::Arc: {
            auto a = static_cast<const Part::GeomArcOfCircle*>(geo);
            base = a->getCenter();
            dir = a->getAxisDirection();
            return true;
        }
        default:
            return false;
    }
}
}  // namespace

bool ViewProviderSketch3D::constrainSelection(Sketcher::ConstraintType type, QString* why)
{
    using Sketcher::PointPos;
    auto fail = [&](const QString& text) {
        if (why) {
            *why = text;
        }
        return false;
    };
    if (!edit) {
        return fail(QObject::tr("No 3D sketch in edit"));
    }
    auto sketch = getSketch3DObject();
    std::vector<Element> points, curves;
    for (const auto& e : edit->selection) {
        (e.pos == PointPos::none ? curves : points).push_back(e);
    }
    auto kind = [&](const Element& e) {
        return kindOf(sketch->getGeometry(e.geoId));
    };
    auto round = [&](const Element& e) {
        return kind(e) == CurveKind::Circle || kind(e) == CurveKind::Arc;
    };
    auto pt = [&](const Element& e) {
        Base::Vector3d p;
        pointOf(e.geoId, e.pos, p);
        return p;
    };
    auto line = [&](const Element& e, Base::Vector3d& a, Base::Vector3d& b) {
        auto seg = Base::freecad_dynamic_cast<Part::GeomLineSegment>(sketch->getGeometry(e.geoId));
        if (!seg) {
            return false;
        }
        a = seg->getStartPoint();
        b = seg->getEndPoint();
        return true;
    };

    // Each constraint as a recorded command, the dimensions with what the geometry
    // measures now, so nothing moves until the value is changed.
    std::vector<std::string> made;
    std::vector<int> removed;  // Block toggled off
    auto con = [&](const std::string& args) {
        made.push_back("Sketcher.Constraint(" + args + ")");
    };
    auto num = [](double v) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", v);
        return std::string(buf);
    };
    auto id = [](const Element& e) {
        return std::to_string(e.geoId);
    };
    auto pp = [](const Element& e) {
        return std::to_string(e.geoId) + ", " + std::to_string(int(e.pos));
    };
    auto quoted = [](const char* t) {
        return std::string("'") + t + "'";
    };
    bool dimension = false;

    switch (type) {
        case Sketcher::Coincident:
        case Sketcher::PointOnObject:
            if (type == Sketcher::Coincident && points.empty() && curves.size() == 2
                && round(curves[0]) && round(curves[1])) {
                // Two circles or arcs: coaxial, the 3D concentric (section 4.4) --
                // centres together and normals parallel.
                con(quoted("Coincident") + ", " + id(curves[0]) + ", 3, " + id(curves[1]) + ", 3");
                con(quoted("Parallel") + ", " + id(curves[0]) + ", " + id(curves[1]));
            }
            else if (type == Sketcher::Coincident && points.size() >= 2 && curves.empty()) {
                for (std::size_t i = 1; i < points.size(); ++i) {
                    con(quoted("Coincident") + ", " + pp(points[0]) + ", " + pp(points[i]));
                }
            }
            else if (!points.empty() && curves.size() == 1) {
                for (const auto& p : points) {
                    con(quoted("PointOnObject") + ", " + pp(p) + ", " + id(curves[0]));
                }
            }
            else {
                return fail(QObject::tr("Select two or more points, points and one curve or "
                                        "plane, or two circles or arcs"));
            }
            break;
        case Sketcher::AlongX:
        case Sketcher::AlongY:
        case Sketcher::AlongZ: {
            const char* name = type == Sketcher::AlongX
                ? "AlongX"
                : (type == Sketcher::AlongY ? "AlongY" : "AlongZ");
            if (points.size() == 2 && curves.empty()) {
                con(quoted(name) + ", " + pp(points[0]) + ", " + pp(points[1]));
            }
            else if (points.empty() && !curves.empty()) {
                for (const auto& c : curves) {
                    if (kind(c) != CurveKind::Line) {
                        return fail(QObject::tr("Select lines, or two points"));
                    }
                    con(quoted(name) + ", " + id(c));
                }
            }
            else {
                return fail(QObject::tr("Select lines, or two points"));
            }
            break;
        }
        case Sketcher::Parallel:
        case Sketcher::Perpendicular: {
            // A circle or arc stands for its plane: a line parallel to it lies along
            // that plane, perpendicular runs along its axis; two of them compare normals.
            const char* name = type == Sketcher::Parallel ? "Parallel" : "Perpendicular";
            auto fits = [&](const Element& e) {
                return kind(e) == CurveKind::Line || kind(e) == CurveKind::Plane || round(e);
            };
            if (points.empty() && curves.size() == 2 && fits(curves[0]) && fits(curves[1])
                && !(kind(curves[0]) == CurveKind::Plane && kind(curves[1]) == CurveKind::Plane)) {
                con(quoted(name) + ", " + id(curves[0]) + ", " + id(curves[1]));
            }
            else {
                return fail(
                    QObject::tr("Select two of lines, circles, arcs and planes, not two planes"));
            }
            break;
        }
        case Sketcher::Tangent:
            if (points.empty() && curves.size() == 2 && kind(curves[0]) == CurveKind::Line
                && kind(curves[1]) == CurveKind::Line) {
                con(quoted("Tangent") + ", " + id(curves[0]) + ", " + id(curves[1]));
            }
            else if (curves.empty() && points.size() == 2 && points[0].geoId != points[1].geoId) {
                con(quoted("Tangent") + ", " + pp(points[0]) + ", " + pp(points[1]));
            }
            else if (points.size() == 1 && curves.size() == 1 && curves[0].geoId < 0) {
                // a curve's end on a model edge, tangent to it there (G1 to the edge)
                con(quoted("Tangent") + ", " + pp(points[0]) + ", " + id(curves[0]));
            }
            else {
                return fail(QObject::tr("Select two lines, an end of each of two curves, or "
                                        "an end and a model edge"));
            }
            break;
        case Sketcher::Smooth:
            if (curves.empty() && points.size() == 2 && points[0].geoId != points[1].geoId) {
                con(quoted("Smooth") + ", " + pp(points[0]) + ", " + pp(points[1]));
            }
            else {
                return fail(QObject::tr("Select an end of each of two curves"));
            }
            break;
        case Sketcher::Equal:
            if (points.empty() && curves.size() >= 2) {
                for (std::size_t i = 1; i < curves.size(); ++i) {
                    con(quoted("Equal") + ", " + id(curves[0]) + ", " + id(curves[i]));
                }
            }
            else {
                return fail(QObject::tr("Select two or more lines, or circles and arcs"));
            }
            break;
        case Sketcher::Symmetric:
            if (curves.empty() && points.size() == 3) {
                con(quoted("Symmetric") + ", " + pp(points[0]) + ", " + pp(points[1]) + ", "
                    + pp(points[2]));
            }
            else if (points.size() == 2 && curves.size() == 1
                     && kind(curves[0]) == CurveKind::Plane) {
                // mirrored in the plane
                con(quoted("Symmetric") + ", " + pp(points[0]) + ", " + pp(points[1]) + ", "
                    + id(curves[0]));
            }
            else {
                return fail(
                    QObject::tr("Select two points and the point between them, or a plane"));
            }
            break;
        case Sketcher::Distance: {
            dimension = true;
            Base::Vector3d a, b;
            if (points.empty() && curves.size() == 1 && line(curves[0], a, b)) {
                con(quoted("Distance") + ", " + id(curves[0]) + ", " + num((b - a).Length()));
            }
            else if (curves.empty() && points.size() == 2) {
                con(quoted("Distance") + ", " + pp(points[0]) + ", " + pp(points[1]) + ", "
                    + num((pt(points[1]) - pt(points[0])).Length()));
            }
            else if (points.size() == 1 && curves.size() == 1 && line(curves[0], a, b)) {
                Base::Vector3d p = pt(points[0]), d = b - a;
                double v = ((p - a) % d).Length() / d.Length();
                con(quoted("Distance") + ", " + pp(points[0]) + ", " + id(curves[0]) + ", "
                    + num(v));
            }
            else if (points.size() == 1 && curves.size() == 1
                     && kind(curves[0]) == CurveKind::Plane) {
                Base::Vector3d o, n = planeNormal(sketch->getGeometry(curves[0].geoId), o);
                con(quoted("Distance") + ", " + pp(points[0]) + ", " + id(curves[0]) + ", "
                    + num((pt(points[0]) - o) * n));
            }
            else {
                return fail(
                    QObject::tr("Select a line, two points, or a point and a line or plane"));
            }
            break;
        }
        case Sketcher::DistanceX:
        case Sketcher::DistanceY:
        case Sketcher::DistanceZ: {
            dimension = true;
            const char* name = type == Sketcher::DistanceX
                ? "DistanceX"
                : (type == Sketcher::DistanceY ? "DistanceY" : "DistanceZ");
            int axis = type == Sketcher::DistanceX ? 0 : (type == Sketcher::DistanceY ? 1 : 2);
            auto comp = [axis](const Base::Vector3d& v) {
                return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
            };
            Base::Vector3d a, b;
            if (points.empty() && curves.size() == 1 && line(curves[0], a, b)) {
                con(quoted(name) + ", " + id(curves[0]) + ", " + num(comp(b - a)));
            }
            else if (curves.empty() && points.size() == 2) {
                con(quoted(name) + ", " + pp(points[0]) + ", " + pp(points[1]) + ", "
                    + num(comp(pt(points[1]) - pt(points[0]))));
            }
            else if (curves.empty() && points.size() == 1) {
                con(quoted(name) + ", " + pp(points[0]) + ", " + num(comp(pt(points[0]))));
            }
            else {
                return fail(QObject::tr("Select a line, two points or a point"));
            }
            break;
        }
        case Sketcher::Angle: {
            dimension = true;
            Base::Vector3d a1, d1, a2, d2;
            if (points.empty() && curves.size() == 2
                && axisOf(sketch->getGeometry(curves[0].geoId), a1, d1)
                && axisOf(sketch->getGeometry(curves[1].geoId), a2, d2)
                && (kind(curves[0]) == CurveKind::Line) == (kind(curves[1]) == CurveKind::Line)) {
                double v = d1.GetAngle(d2);
                con(quoted("Angle") + ", " + id(curves[0]) + ", " + id(curves[1]) + ", " + num(v));
            }
            else {
                return fail(QObject::tr("Select two lines, or two of planes, circles and arcs"));
            }
            break;
        }
        case Sketcher::Radius:
        case Sketcher::Diameter: {
            dimension = true;
            if (!points.empty() || curves.empty()) {
                return fail(QObject::tr("Select circles or arcs"));
            }
            for (const auto& c : curves) {
                auto geo = sketch->getGeometry(c.geoId);
                double r;
                if (auto circle = Base::freecad_dynamic_cast<Part::GeomCircle>(geo)) {
                    r = circle->getRadius();
                }
                else if (auto arc = Base::freecad_dynamic_cast<Part::GeomArcOfCircle>(geo)) {
                    r = arc->getRadius();
                }
                else {
                    return fail(QObject::tr("Select circles or arcs"));
                }
                bool diameter = type == Sketcher::Diameter;
                con(quoted(diameter ? "Diameter" : "Radius") + ", " + id(c) + ", "
                    + num(diameter ? 2 * r : r));
            }
            break;
        }
        case Sketcher::Block: {
            // A toggle: what is all blocked already is freed (a kept plane made
            // constrainable, section 6), otherwise the rest is blocked.
            if (!points.empty() || curves.empty()) {
                return fail(QObject::tr("Select the geometry to block"));
            }
            const auto& all = sketch->Constraints.getValues();
            auto blockOf = [&](int geoId) {
                for (std::size_t i = 0; i < all.size(); ++i) {
                    if (all[i]->Type == Sketcher::Block && all[i]->First == geoId) {
                        return int(i);
                    }
                }
                return -1;
            };
            bool allBlocked = std::all_of(curves.begin(), curves.end(), [&](const Element& c) {
                return blockOf(c.geoId) >= 0;
            });
            for (const auto& c : curves) {
                if (allBlocked) {
                    removed.push_back(blockOf(c.geoId));
                }
                else if (blockOf(c.geoId) < 0) {
                    con(quoted("Block") + ", " + id(c));
                }
            }
            break;
        }
        case Sketcher::Horizontal:
        case Sketcher::Vertical: {
            // Along the X or Y axis of a workplane: the one selected with the lines,
            // else the active kept plane (section 4.4, workplane-local).
            int plane = -1;
            std::vector<Element> lines;
            for (const auto& c : curves) {
                if (kind(c) == CurveKind::Plane && plane < 0) {
                    plane = c.geoId;
                }
                else if (kind(c) == CurveKind::Line) {
                    lines.push_back(c);
                }
                else {
                    lines.clear();
                    break;
                }
            }
            if (plane < 0) {
                plane = edit->keptPlane;
            }
            if (plane < 0) {
                return fail(QObject::tr(
                    "A 3D sketch has no horizontal or vertical without a plane: select one "
                    "with the lines, make a kept plane active, or use Along X, Y or Z"));
            }
            if (!points.empty() || lines.empty()) {
                return fail(QObject::tr("Select lines, and a plane unless one is active"));
            }
            const char* name = type == Sketcher::Horizontal ? "Horizontal" : "Vertical";
            for (const auto& l : lines) {
                con(quoted(name) + ", " + id(l) + ", " + std::to_string(plane));
            }
            break;
        }
        default:
            return fail(QObject::tr("Not a constraint the 3D sketch has"));
    }

    const std::string obj = Gui::Command::getObjectCmd(sketch);
    int first = sketch->Constraints.getSize();
    if (!removed.empty()) {
        std::sort(removed.rbegin(), removed.rend());
        Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Free 3D sketch geometry"));
        for (int i : removed) {
            Gui::Command::doCommand(Gui::Command::Doc, "%s.delConstraint(%d)", obj.c_str(), i);
        }
        clearSelection();
        Gui::Command::commitCommand();
        return true;
    }
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Add 3D sketch constraint"));
    for (const auto& c : made) {
        Gui::Command::doCommand(Gui::Command::Doc, "%s.addConstraint(%s)", obj.c_str(), c.c_str());
    }
    clearSelection();
    // A new dimension's value is typed next, as the 2D sketch's commands do it, in
    // the same undo step.
    if (dimension && made.size() == 1 && edit->viewer && editDatum(first)) {
        return true;
    }
    Gui::Command::commitCommand();
    return true;
}

bool ViewProviderSketch3D::lockSelection(QString* why)
{
    if (!edit || edit->selection.size() != 1
        || edit->selection[0].pos == Sketcher::PointPos::none) {
        if (why) {
            *why = QObject::tr("Select one point to lock");
        }
        return false;
    }
    const Element e = edit->selection[0];
    Base::Vector3d p;
    pointOf(e.geoId, e.pos, p);
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Lock 3D sketch point"));
    const char* names[3] = {"DistanceX", "DistanceY", "DistanceZ"};
    const double values[3] = {p.x, p.y, p.z};
    for (int k = 0; k < 3; ++k) {
        Gui::Command::doCommand(Gui::Command::Doc,
                                "%s.addConstraint(Sketcher.Constraint('%s', %d, %d, %.15g))",
                                obj.c_str(),
                                names[k],
                                e.geoId,
                                int(e.pos),
                                values[k]);
    }
    Gui::Command::commitCommand();
    clearSelection();
    return true;
}

bool ViewProviderSketch3D::filletAt(int geoId, Sketcher::PointPos pos, QString* why)
{
    using Sketcher::PointPos;
    auto fail = [&](const QString& text) {
        if (why) {
            *why = text;
        }
        return false;
    };
    auto sketch = getSketch3DObject();
    auto line = [&](int g) {
        return g >= 0 ? Base::freecad_dynamic_cast<Part::GeomLineSegment>(sketch->getGeometry(g))
                      : nullptr;
    };
    auto endOf = [&](int g, PointPos p) {
        auto l = line(g);
        return p == PointPos::start ? l->getStartPoint() : l->getEndPoint();
    };
    if (!line(geoId) || (pos != PointPos::start && pos != PointPos::end)) {
        return fail(QObject::tr("A fillet is at a corner where two lines meet"));
    }
    // The other line whose end is there.
    Base::Vector3d corner = endOf(geoId, pos);
    int other = -1;
    PointPos otherPos = PointPos::none;
    for (int g = 0; g < sketch->Geometry.getSize() && other < 0; ++g) {
        if (g == geoId || !line(g)) {
            continue;
        }
        for (PointPos p : {PointPos::start, PointPos::end}) {
            if ((endOf(g, p) - corner).Length() < Precision::Confusion()) {
                other = g;
                otherPos = p;
                break;
            }
        }
    }
    if (other < 0) {
        return fail(QObject::tr("No other line ends at that corner"));
    }
    // A radius that leaves each line most of its length: the tangent points a third
    // of the shorter line from the corner.
    Base::Vector3d u1 =
        endOf(geoId, pos == PointPos::start ? PointPos::end : PointPos::start) - corner;
    Base::Vector3d u2 =
        endOf(other, otherPos == PointPos::start ? PointPos::end : PointPos::start) - corner;
    double angle = u1.GetAngle(u2);
    if (angle < 1e-6 || angle > std::numbers::pi - 1e-6) {
        return fail(QObject::tr("The lines at that corner are in line: nothing to round"));
    }
    double radius = std::min(u1.Length(), u2.Length()) / 3 * std::tan(angle / 2);
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    int arc = sketch->Geometry.getSize();
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Fillet 3D sketch corner"));
    try {
        Gui::Command::doCommand(Gui::Command::Doc,
                                "%s.fillet(%d, %d, %d, %d, %.15g)",
                                obj.c_str(),
                                geoId,
                                int(pos),
                                other,
                                int(otherPos),
                                radius);
        // its radius, typed next as a new dimension's value is (one undo step)
        int index = sketch->Constraints.getSize();
        Gui::Command::doCommand(Gui::Command::Doc,
                                "%s.addConstraint(Sketcher.Constraint('Radius', %d, %.15g))",
                                obj.c_str(),
                                arc,
                                radius);
        clearSelection();
        if (edit && edit->viewer && editDatum(index)) {
            return true;
        }
        Gui::Command::commitCommand();
        return true;
    }
    catch (const Base::Exception& e) {
        Gui::Command::abortCommand();
        return fail(QString::fromUtf8(e.what()));
    }
}

bool ViewProviderSketch3D::filletSelection(QString* why)
{
    using Sketcher::PointPos;
    if (!edit) {
        return false;
    }
    auto sketch = getSketch3DObject();
    const auto sel = edit->selection;
    if (sel.empty()) {
        startTool(Tool::Fillet);
        return true;
    }
    if (sel.size() == 1 && sel[0].pos != PointPos::none) {
        return filletAt(sel[0].geoId, sel[0].pos, why);
    }
    if (sel.size() == 2 && sel[0].pos == PointPos::none && sel[1].pos == PointPos::none) {
        // the ends of the two lines that meet
        auto l1 =
            Base::freecad_dynamic_cast<Part::GeomLineSegment>(sketch->getGeometry(sel[0].geoId));
        auto l2 =
            Base::freecad_dynamic_cast<Part::GeomLineSegment>(sketch->getGeometry(sel[1].geoId));
        if (l1 && l2) {
            for (PointPos p : {PointPos::start, PointPos::end}) {
                Base::Vector3d at = p == PointPos::start ? l1->getStartPoint() : l1->getEndPoint();
                if ((at - l2->getStartPoint()).Length() < Precision::Confusion()
                    || (at - l2->getEndPoint()).Length() < Precision::Confusion()) {
                    return filletAt(sel[0].geoId, p, why);
                }
            }
        }
    }
    if (why) {
        *why = QObject::tr("Select two lines that meet, or the corner where they meet");
    }
    return false;
}

void ViewProviderSketch3D::deleteSelection()
{
    if (!edit) {
        return;
    }
    auto sketch = getSketch3DObject();
    const std::string obj = Gui::Command::getObjectCmd(sketch);
    if (!edit->selectedConstraints.empty()) {
        std::vector<int> cons = edit->selectedConstraints;
        std::sort(cons.rbegin(), cons.rend());
        Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Delete 3D sketch constraints"));
        for (int i : cons) {
            Gui::Command::doCommand(Gui::Command::Doc, "%s.delConstraint(%d)", obj.c_str(), i);
        }
        Gui::Command::commitCommand();
    }
    else {
        std::vector<int> geos;
        for (const auto& e : edit->selection) {
            if (e.geoId >= 0 && std::find(geos.begin(), geos.end(), e.geoId) == geos.end()) {
                geos.push_back(e.geoId);
            }
        }
        if (geos.empty()) {
            return;
        }
        std::sort(geos.rbegin(), geos.rend());
        Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Delete 3D sketch geometry"));
        for (int g : geos) {
            Gui::Command::doCommand(Gui::Command::Doc, "%s.delGeometry(%d)", obj.c_str(), g);
        }
        Gui::Command::commitCommand();
    }
    edit->selection.clear();
    edit->selectedConstraints.clear();
    draw();
}

bool ViewProviderSketch3D::editDatum(int index)
{
    if (!edit || !edit->viewer || !edit->labels.count(index)) {
        return false;
    }
    if (edit->datum) {
        edit->datum->finish(true);
    }
    edit->datum = new Datum3DSession(this, edit->viewer, index);
    return true;
}

bool ViewProviderSketch3D::isEditingDatum() const
{
    return edit && edit->datum;
}

void ViewProviderSketch3D::finishDatumEdit(bool accept)
{
    if (edit && edit->datum) {
        edit->datum->finish(accept);
    }
}

std::map<int, std::string> ViewProviderSketch3D::labelTexts() const
{
    std::map<int, std::string> res;
    if (edit) {
        for (const auto& [index, label] : edit->labels) {
            res[index] = label->string.getNum() ? label->string[0].getString() : "";
        }
    }
    return res;
}

int ViewProviderSketch3D::labelOnPath(SoPath* path) const
{
    if (!edit || !path) {
        return -1;
    }
    for (const auto& [index, node] : edit->labelNodes) {
        if (path->containsNode(node)) {
            return index;
        }
    }
    for (const auto& [node, index] : edit->iconNodes) {
        if (path->containsNode(node)) {
            return index;
        }
    }
    return -1;
}

namespace
{
// The icon a constraint shows with: the 2D sketch's, and the 3D sketch's own.
const char* constraintIconName(const Sketcher::Constraint* c)
{
    switch (c->Type) {
        case Sketcher::Coincident:
            return "Constraint_PointOnPoint";
        case Sketcher::PointOnObject:
            return "Constraint_PointOnObject";
        case Sketcher::Horizontal:
            return "Constraint_Horizontal";
        case Sketcher::Vertical:
            return "Constraint_Vertical";
        case Sketcher::Parallel:
            return "Constraint_Parallel";
        case Sketcher::Perpendicular:
            return "Constraint_Perpendicular";
        case Sketcher::Tangent:
            return "Constraint_Tangent";
        case Sketcher::Equal:
            return "Constraint_EqualLength";
        case Sketcher::Symmetric:
            return "Constraint_Symmetric";
        case Sketcher::Block:
            return "Constraint_Block";
        case Sketcher::AlongX:
            return "Constraint_AlongX";
        case Sketcher::AlongY:
            return "Constraint_AlongY";
        case Sketcher::AlongZ:
            return "Constraint_AlongZ";
        case Sketcher::Smooth:
            return "Constraint_Smooth";
        case Sketcher::Distance:
            return "Constraint_Length";
        case Sketcher::DistanceX:
            return "Constraint_HorizontalDistance";
        case Sketcher::DistanceY:
            return "Constraint_VerticalDistance";
        case Sketcher::DistanceZ:
            return "Constraint_DistanceZ";
        case Sketcher::Angle:
            return "Constraint_InternalAngle";
        case Sketcher::Radius:
            return "Constraint_Radius";
        case Sketcher::Diameter:
            return "Constraint_Diameter";
        case Sketcher::InternalAlignment:
            return "Constraint_InternalAlignment";
        default:
            return nullptr;
    }
}

// The icon in one colour, as the 2D sketch draws its constraint icons, with a border
// of border pixels in outline round it (none for 0).
const QImage& tintedIcon(const char* name,
                         int size,
                         const QColor& color,
                         const QColor& outline = QColor(),
                         int border = 0)
{
    static std::map<std::string, QImage> cache;
    std::string key = std::string(name) + "/" + std::to_string(size) + "/"
        + std::to_string(color.rgba()) + "/" + std::to_string(outline.rgba()) + "/"
        + std::to_string(border);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    QImage image = Gui::BitmapFactory()
                       .pixmapFromSvg(name, QSizeF(size, size))
                       .toImage()
                       .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(image.rect(), color);
    painter.end();
    if (border > 0) {
        // the silhouette in the outline's colour, spread by the border, under it
        QImage rim = image;
        QPainter tint(&rim);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(rim.rect(), outline);
        tint.end();
        QImage framed(size + 2 * border, size + 2 * border, QImage::Format_ARGB32_Premultiplied);
        framed.fill(Qt::transparent);
        QPainter p(&framed);
        for (int dx = -border; dx <= border; ++dx) {
            for (int dy = -border; dy <= border; ++dy) {
                if (dx * dx + dy * dy <= border * border + border) {
                    p.drawImage(border + dx, border + dy, rim);
                }
            }
        }
        p.drawImage(border, border, image);
        p.end();
        image = framed;
    }
    return cache.emplace(key, image).first->second;
}
}  // namespace

bool ViewProviderSketch3D::labelPlane(int index, Base::Placement& plane) const
{
    if (!edit || !edit->labelPlanes.count(index)) {
        return false;
    }
    plane = edit->labelPlanes.at(index);
    return true;
}

std::vector<std::pair<int, Base::Vector3d>> ViewProviderSketch3D::iconPositions() const
{
    return edit ? edit->icons : std::vector<std::pair<int, Base::Vector3d>>();
}

void ViewProviderSketch3D::cameraChanged(void* data, SoSensor* /*sensor*/)
{
    auto vp = static_cast<ViewProviderSketch3D*>(data);
    if (vp->edit && vp->edit->refaceTimer) {
        vp->edit->refaceTimer->start();  // again on each change: one redraw when it rests
    }
}

void ViewProviderSketch3D::reface()
{
    if (!edit) {
        return;
    }
    // a label being typed into stays as it is: the editor holds it
    if (!edit->datum) {
        drawLabels();
    }
    drawIcons();
}

void ViewProviderSketch3D::drawIcons()
{
    if (!edit) {
        return;
    }
    using Sketcher::PointPos;
    Gui::coinRemoveAllChildren(edit->iconRoot);
    edit->iconNodes.clear();
    edit->icons.clear();
    SoCamera* camera = edit->viewer ? edit->viewer->getCamera() : nullptr;
    if (!camera) {
        return;
    }
    // follow the camera, whichever node it is now
    if (edit->cameraSensor && edit->cameraSensor->getAttachedNode() != camera) {
        edit->cameraSensor->detach();
        edit->cameraSensor->attach(camera);
    }
    auto sketch = getSketch3DObject();

    // The screen's right and up in the sketch's frame.
    Base::Matrix4D inv = editingTransform();
    inv.inverseGauss();
    auto localDir = [&](const SbVec3f& v) {
        Base::Vector3d o, d;
        inv.multVec(Base::Vector3d(0, 0, 0), o);
        inv.multVec(Base::Vector3d(v[0], v[1], v[2]), d);
        return (d - o).Normalize();
    };
    SbVec3f r, u;
    camera->orientation.getValue().multVec(SbVec3f(1, 0, 0), r);
    camera->orientation.getValue().multVec(SbVec3f(0, 1, 0), u);
    Base::Vector3d right = localDir(r), up = localDir(u);

    auto viewGrp =
        App::GetApplication().GetParameterGroupByPath("User parameter:BaseApp/Preferences/View");
    using Params = Sketcher::SketcherParams;
    long px = viewGrp->GetInt("EditSketcherFontSize", Params::defaultEditSketcherFontSize());
    if (px <= 0) {
        px = 17;
    }
    const double dpr = edit->viewer->devicePixelRatio();
    const int size = std::max(8, int(std::lround(0.8 * px * dpr)));  // as the 2D sketch's
    auto toQColor = [](const SbColor& c) {
        return QColor::fromRgbF(c[0], c[1], c[2]);
    };
    // the 3D sketch's own, its teal (the 2D sketch's red is ConstrainedIcoColor), dark on
    // a light halo when the view's background is light
    const bool light = viewBackgroundIsLight();
    const QColor iconColor =
        toQColor(light ? editColor("Sketch3DIcoColorLight", Params::defaultSketch3DIcoColorLight())
                       : editColor("Sketch3DIcoColor", Params::defaultSketch3DIcoColor()));
    const QColor offColor = toQColor(
        editColor("DeactivatedConstrDimColor", Params::defaultDeactivatedConstrDimColor()));
    const QColor outlineColor = toQColor(
        light ? editColor("Sketch3DOutlineColorLight", Params::defaultSketch3DOutlineColorLight())
              : editColor("Sketch3DOutlineColor", Params::defaultSketch3DOutlineColor()));
    const int border = int(std::lround(
        viewGrp->GetInt("ConstraintOutlineWidth", Params::defaultConstraintOutlineWidth()) * dpr));
    SbColor selectColor;
    {
        float transparency;
        selectColor.setPackedValue(uint32_t(Gui::ViewParams::getSelectionColor()), transparency);
    }

    // Where a constraint binds: a point, or the middle of a curve.
    auto middle = [&](int geoId, Base::Vector3d& p) {
        auto geo = sketch->getGeometry(geoId);
        if (!geo) {
            return false;
        }
        if (auto pt = Base::freecad_dynamic_cast<Part::GeomPoint>(geo)) {
            p = pt->getPoint();
            return true;
        }
        if (auto pl = Base::freecad_dynamic_cast<Part::GeomPlane>(geo)) {
            Base::Vector3d o;
            planeNormal(pl, o);
            p = o;
            return true;
        }
        auto curve = Base::freecad_dynamic_cast<Part::GeomCurve>(geo);
        if (!curve) {
            return false;
        }
        double f = curve->getFirstParameter(), l = curve->getLastParameter();
        if (Precision::IsInfinite(f) || Precision::IsInfinite(l)) {
            return false;
        }
        p = curve->pointAtParameter((f + l) / 2);
        return true;
    };
    auto point = [&](int geoId, PointPos pos, Base::Vector3d& p) {
        return pointOf(geoId, pos, p);
    };

    std::map<std::array<long long, 3>, int> taken;  // icons at each place so far
    const auto& cons = sketch->Constraints.getValues();
    for (std::size_t i = 0; i < cons.size(); ++i) {
        const Sketcher::Constraint* c = cons[i];
        if (c->isDimensional() || c->Type == Sketcher::Coincident
            || c->Type == Sketcher::InternalAlignment) {
            continue;  // a label, a highlighted point, or the curve's own points
        }
        const char* name = constraintIconName(c);
        if (!name) {
            continue;
        }
        std::vector<Base::Vector3d> at;
        Base::Vector3d p, q;
        switch (c->Type) {
            case Sketcher::PointOnObject:
                if (c->FirstPos == PointPos::none ? middle(c->First, p)
                                                  : point(c->First, c->FirstPos, p)) {
                    at.push_back(p);
                }
                break;
            case Sketcher::Tangent:
            case Sketcher::Smooth:
                if (c->FirstPos != PointPos::none && point(c->First, c->FirstPos, p)) {
                    at.push_back(p);
                }
                else if (c->SecondPos != PointPos::none && point(c->Second, c->SecondPos, p)) {
                    at.push_back(p);
                }
                else {
                    if (middle(c->First, p)) {
                        at.push_back(p);
                    }
                    if (middle(c->Second, q)) {
                        at.push_back(q);
                    }
                }
                break;
            case Sketcher::Symmetric:
                if (c->ThirdPos != PointPos::none && point(c->Third, c->ThirdPos, p)) {
                    at.push_back(p);
                }
                else if (point(c->First, c->FirstPos, p) && point(c->Second, c->SecondPos, q)) {
                    at.push_back((p + q) / 2);
                }
                break;
            case Sketcher::AlongX:
            case Sketcher::AlongY:
            case Sketcher::AlongZ:
                if (c->Second == Sketcher::GeoEnum::GeoUndef) {
                    if (middle(c->First, p)) {
                        at.push_back(p);
                    }
                }
                else if (point(c->First, c->FirstPos, p) && point(c->Second, c->SecondPos, q)) {
                    at.push_back((p + q) / 2);
                }
                break;
            case Sketcher::Parallel:
            case Sketcher::Perpendicular:
            case Sketcher::Equal:
                // at each of the two, as the 2D sketch shows them
                if (middle(c->First, p)) {
                    at.push_back(p);
                }
                if (middle(c->Second, q)) {
                    at.push_back(q);
                }
                break;
            default:  // Horizontal, Vertical, Block: on the curve
                if (middle(c->First, p)) {
                    at.push_back(p);
                }
                break;
        }

        bool selected =
            std::find(edit->selectedConstraints.begin(), edit->selectedConstraints.end(), int(i))
            != edit->selectedConstraints.end();
        const QImage& image = tintedIcon(name,
                                         size,
                                         selected           ? toQColor(selectColor)
                                             : !c->isActive ? offColor
                                                            : iconColor,
                                         outlineColor,
                                         border);
        for (const auto& a : at) {
            // a few pixels up and to the right of the place, the next one there further
            // right: the world size of a pixel there, from the view
            SbVec2s v0, v1;
            if (!toView(a, v0) || !toView(a + right, v1)) {
                continue;
            }
            double perUnit = std::hypot(double(v1[0] - v0[0]), double(v1[1] - v0[1]));
            if (perUnit < 1e-6) {
                continue;
            }
            double unit = 1. / perUnit;
            std::array<long long, 3> key {std::llround(a.x * 1e6),
                                          std::llround(a.y * 1e6),
                                          std::llround(a.z * 1e6)};
            int slot = taken[key]++;
            Base::Vector3d pos =
                a + right * ((slot + 0.9) * (size + 3) * unit) + up * (0.9 * size * unit);
            auto node = new SoSeparator();
            auto move = new SoTranslation();
            move->translation.setValue(float(pos.x), float(pos.y), float(pos.z));
            auto icon = new SoImage();
            SoSFImage data;
            Gui::BitmapFactory().convert(image, data);
            SbVec2s dim(short(image.width()), short(image.height()));
            int nc = 4;
            icon->image.setValue(dim, 4, data.getValue(dim, nc));
            icon->vertAlignment = SoImage::HALF;
            icon->horAlignment = SoImage::CENTER;
            node->addChild(move);
            node->addChild(icon);
            edit->iconRoot->addChild(node);
            edit->iconNodes[node] = int(i);
            edit->icons.emplace_back(int(i), pos);
        }
    }
}

void ViewProviderSketch3D::listConstraints()
{
    if (!edit || !edit->dialog) {
        return;
    }
    auto sketch = getSketch3DObject();
    std::vector<TaskDlgSketch3D::Row> rows;
    const auto& cons = sketch->Constraints.getValues();
    for (std::size_t i = 0; i < cons.size(); ++i) {
        const Sketcher::Constraint* c = cons[i];
        if (c->Type == Sketcher::InternalAlignment) {
            continue;  // a spline's own points: they go with it
        }
        QString text = QStringLiteral("%1  %2").arg(i + 1).arg(
            QString::fromStdString(Sketcher::Constraint::typeToString(c->Type)));
        if (!c->Name.empty()) {
            text += QStringLiteral("  ") + QString::fromStdString(c->Name);
        }
        if (c->isDimensional()) {
            Base::Quantity value(c->getValue(), Base::Unit::Length);
            if (c->Type == Sketcher::Angle) {
                value = Base::Quantity(Base::toDegrees<double>(c->getValue()), Base::Unit::Angle);
            }
            text += QStringLiteral("  ") + QString::fromStdString(value.getUserString());
        }
        const char* icon = constraintIconName(c);
        rows.push_back(
            {int(i),
             icon ? Gui::BitmapFactory().iconFromTheme(icon) : QIcon(),
             text,
             std::find(edit->selectedConstraints.begin(), edit->selectedConstraints.end(), int(i))
                 != edit->selectedConstraints.end()});
    }
    edit->dialog->setConstraints(rows);
}

void ViewProviderSketch3D::drawLabels()
{
    // Each dimension in a plane through what it measures, facing the camera as it
    // was at the draw: SoDatumLabel draws in its XY (docs/Sketch3D.md section 7).
    if (!edit) {
        return;
    }
    using Sketcher::PointPos;
    auto sketch = getSketch3DObject();
    Gui::coinRemoveAllChildren(edit->labelRoot);
    edit->labelNodes.clear();
    edit->labels.clear();
    edit->labelPlanes.clear();

    // The view direction in the sketch's frame, for the label planes.
    Base::Vector3d toEye(0, 0, 1);
    if (edit->viewer) {
        SbVec3f v = edit->viewer->getViewDirection();
        Base::Matrix4D inv = editingTransform();
        inv.inverseGauss();
        Base::Vector3d o, d;
        inv.multVec(Base::Vector3d(0, 0, 0), o);
        inv.multVec(Base::Vector3d(-v[0], -v[1], -v[2]), d);
        toEye = (d - o).Normalize();
    }
    auto planeAlong = [&](const Base::Vector3d& origin, Base::Vector3d x) {
        x.Normalize();
        Base::Vector3d n = toEye - x * (toEye * x);
        if (n.Length() < 1e-6) {
            n = std::fabs(x.z) < 0.9 ? Base::Vector3d(0, 0, 1) : Base::Vector3d(1, 0, 0);
            n = n - x * (n * x);
        }
        n.Normalize();
        return Base::Placement(origin, Base::Rotation::makeRotationByAxes(x, n % x, n));
    };

    using Params = Sketcher::SketcherParams;
    // the 3D sketch's own, its teal (the 2D sketch's red is ConstrainedDimColor), dark on
    // a light halo when the view's background is light
    const bool light = viewBackgroundIsLight();
    const SbColor drivingColor = light
        ? editColor("Sketch3DDimColorLight", Params::defaultSketch3DDimColorLight())
        : editColor("Sketch3DDimColor", Params::defaultSketch3DDimColor());
    const SbColor referenceColor =
        editColor("NonDrivingConstrDimColor", Params::defaultNonDrivingConstrDimColor());
    // a border, so the light teal reads on a light background too
    const SbColor outlineColor = light
        ? editColor("Sketch3DOutlineColorLight", Params::defaultSketch3DOutlineColorLight())
        : editColor("Sketch3DOutlineColor", Params::defaultSketch3DOutlineColor());
    const long outlineWidth =
        App::GetApplication()
            .GetParameterGroupByPath("User parameter:BaseApp/Preferences/View")
            ->GetInt("ConstraintOutlineWidth", Params::defaultConstraintOutlineWidth());
    const SbColor expressionColor =
        editColor("ExprBasedConstrDimColor", Params::defaultExprBasedConstrDimColor());
    SbColor selectColor;
    float transparency;
    selectColor.setPackedValue(uint32_t(Gui::ViewParams::getSelectionColor()), transparency);
    auto viewGrp =
        App::GetApplication().GetParameterGroupByPath("User parameter:BaseApp/Preferences/View");
    long px = viewGrp->GetInt("EditSketcherFontSize", Params::defaultEditSketcherFontSize());
    if (px <= 0) {
        px = 17;
    }
    double dpr = edit->viewer ? edit->viewer->devicePixelRatio() : 1.;
    double dpi = edit->viewer ? edit->viewer->logicalDotsPerInchX() : 96.;
    if (dpi <= 0.) {
        dpi = 96.;
    }
    const int fontSize = int(std::lround(px * dpr * 72. / dpi));

    const auto& cons = sketch->Constraints.getValues();
    for (std::size_t i = 0; i < cons.size(); ++i) {
        const Sketcher::Constraint* c = cons[i];
        if (!c->isDimensional() || !c->isActive) {
            continue;
        }
        Base::Vector3d p, q;
        Base::Placement plane;
        int datumType = Gui::SoDatumLabel::DISTANCE;
        float param2 = 0.f, param3 = 0.f;
        bool ok = false;
        auto geo = sketch->getGeometry(c->First);
        switch (c->Type) {
            case Sketcher::Distance:
            case Sketcher::DistanceX:
            case Sketcher::DistanceY:
            case Sketcher::DistanceZ: {
                int axis = c->Type == Sketcher::DistanceX ? 0
                    : c->Type == Sketcher::DistanceY      ? 1
                    : c->Type == Sketcher::DistanceZ      ? 2
                                                          : -1;
                Base::Vector3d dir(axis == 0, axis == 1, axis == 2);
                if (c->Second == Sketcher::GeoEnum::GeoUndef && c->FirstPos == PointPos::none) {
                    ok = pointOf(c->First, PointPos::start, p)
                        && pointOf(c->First, PointPos::end, q);
                }
                else if (c->Second == Sketcher::GeoEnum::GeoUndef) {
                    ok = pointOf(c->First, c->FirstPos, q);  // a coordinate: from the frame's plane
                    p = q - dir * (q * dir);
                }
                else if (c->SecondPos != PointPos::none) {
                    ok = pointOf(c->First, c->FirstPos, p) && pointOf(c->Second, c->SecondPos, q);
                }
                else if (pointOf(c->First, c->FirstPos, p)) {
                    auto other = sketch->getGeometry(c->Second);
                    if (auto seg = Base::freecad_dynamic_cast<Part::GeomLineSegment>(other)) {
                        Base::Vector3d a = seg->getStartPoint(), d = seg->getEndPoint() - a;
                        q = a + d * (((p - a) * d) / d.Sqr());
                        ok = true;
                    }
                    else if (other && kindOf(other) == CurveKind::Plane) {
                        Base::Vector3d o, n = planeNormal(other, o);
                        q = p - n * ((p - o) * n);
                        ok = true;
                    }
                }
                if (ok && axis >= 0) {
                    q = p + dir * ((q - p) * dir);  // along the axis only
                }
                ok = ok && (q - p).Length() > 1e-9;
                if (ok) {
                    plane = planeAlong(p, q - p);
                }
                break;
            }
            case Sketcher::Radius:
            case Sketcher::Diameter: {
                Base::Vector3d c0, n, s0;
                if (auto circle = Base::freecad_dynamic_cast<Part::GeomCircle>(geo)) {
                    c0 = circle->getCenter();
                    n = circle->getAxisDirection();
                    auto h = Handle(Geom_Circle)::DownCast(circle->handle());
                    gp_Pnt s = h->Value(0.);
                    s0 = Base::Vector3d(s.X(), s.Y(), s.Z());
                    ok = true;
                }
                else if (auto arc = Base::freecad_dynamic_cast<Part::GeomArcOfCircle>(geo)) {
                    c0 = arc->getCenter();
                    n = arc->getAxisDirection();
                    double u0, u1;
                    arc->getRange(u0, u1, false);
                    s0 = arc->pointAtParameter((u0 + u1) / 2);
                    ok = true;
                }
                if (ok) {
                    Base::Vector3d x = (s0 - c0).Normalize();
                    // in the arc's plane, from its front: seen from behind the
                    // text reads mirrored
                    if (n * toEye < 0) {
                        n = -n;
                    }
                    plane = Base::Placement(c0, Base::Rotation::makeRotationByAxes(x, n % x, n));
                    double r = (s0 - c0).Length();
                    p = c0;
                    q = s0;
                    if (c->Type == Sketcher::Diameter) {
                        datumType = Gui::SoDatumLabel::DIAMETER;
                        p = c0 - (s0 - c0);
                    }
                    else {
                        datumType = Gui::SoDatumLabel::RADIUS;
                    }
                    (void)r;
                }
                break;
            }
            case Sketcher::Angle: {
                // between lines, or the normals of planes, circles and arcs, from where
                // the two come nearest
                Base::Vector3d a1, d1, a2, d2;
                if (axisOf(geo, a1, d1) && axisOf(sketch->getGeometry(c->Second), a2, d2)) {
                    Base::Vector3d n = d1 % d2;
                    if (n.Length() > 1e-9) {
                        // where the lines come nearest
                        Base::Vector3d w = a1 - a2;
                        double A = d1 * d1, B = d1 * d2, C = d2 * d2, D = d1 * w, E = d2 * w;
                        double den = A * C - B * B;
                        double s1 = (B * E - C * D) / den, s2 = (A * E - B * D) / den;
                        Base::Vector3d vertex = ((a1 + d1 * s1) + (a2 + d2 * s2)) / 2;
                        Base::Vector3d x = Base::Vector3d(d1).Normalize();
                        n.Normalize();
                        // from the front, the arc from the second line round to
                        // the first: the same arc, its text not mirrored
                        if (n * toEye < 0) {
                            n = -n;
                            x = Base::Vector3d(d2).Normalize();
                        }
                        plane = Base::Placement(vertex,
                                                Base::Rotation::makeRotationByAxes(x, n % x, n));
                        datumType = Gui::SoDatumLabel::ANGLE;
                        p = vertex;
                        param2 = 0.f;
                        param3 = float(d1.GetAngle(d2));
                        ok = true;
                    }
                }
                break;
            }
            default:
                break;
        }
        if (!ok) {
            continue;
        }

        // The points in the label's own plane.
        Base::Placement inv = plane.inverse();
        Base::Vector3d lp, lq;
        inv.multVec(p, lp);
        inv.multVec(q, lq);

        auto node = new SoSeparator();
        auto xform = new SoTransform();
        double q0, q1, q2, q3;
        plane.getRotation().getValue(q0, q1, q2, q3);
        const Base::Vector3d& t = plane.getPosition();
        xform->translation.setValue(float(t.x), float(t.y), float(t.z));
        xform->rotation.setValue(float(q0), float(q1), float(q2), float(q3));
        auto label = new Gui::SoDatumLabel();
        label->datumtype = datumType;
        label->norm.setValue(SbVec3f(0, 0, 1));
        label->size = fontSize;
        label->lineWidth = float(2. * dpr);
        label->outlineColor = outlineColor;
        label->outlineWidth = float(outlineWidth * dpr);
        bool selected =
            std::find(edit->selectedConstraints.begin(), edit->selectedConstraints.end(), int(i))
            != edit->selectedConstraints.end();
        bool hasExpression =
            bool(sketch->getExpression(sketch->Constraints.createPath(int(i))).expression);
        label->textColor = selected ? selectColor
            : !c->isDriving         ? referenceColor
            : hasExpression         ? expressionColor
                                    : drivingColor;
        Base::Quantity value(c->getValue(), Base::Unit::Length);
        if (c->Type == Sketcher::Angle) {
            value = Base::Quantity(Base::toDegrees<double>(c->getValue()), Base::Unit::Angle);
        }
        std::string text = value.getUserString();
        if (!c->isDriving) {
            text = "(" + text + ")";
        }
        label->string = SbString(text.c_str());
        if (datumType == Gui::SoDatumLabel::ANGLE) {
            label->pnts.setNum(1);
            label->pnts.set1Value(0, SbVec3f(0, 0, 0));
            label->param1 = c->LabelDistance;
            label->param2 = param2;
            label->param3 = param3;
        }
        else {
            label->pnts.setNum(2);
            label->pnts.set1Value(0, SbVec3f(float(lp.x), float(lp.y), float(lp.z)));
            label->pnts.set1Value(1, SbVec3f(float(lq.x), float(lq.y), float(lq.z)));
            label->param1 = c->LabelDistance;
            label->param2 = c->LabelPosition;
        }
        node->addChild(xform);
        node->addChild(label);
        if (auto image = label->getImageNode()) {
            node->addChild(image);
        }
        edit->labelRoot->addChild(node);
        edit->labelNodes[int(i)] = node;
        edit->labels[int(i)] = label;
        edit->labelPlanes[int(i)] = plane;
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
    edit->dragger->addStartCallback(&ViewProviderSketch3D::draggerStarted, this);
    edit->dragger->addFinishCallback(&ViewProviderSketch3D::draggerFinished, this);

    edit->deleteFilter = std::make_unique<Sketch3DDeleteFilter>(this);
    Gui::getMainWindow()->installEventFilter(edit->deleteFilter.get());
    edit->dialog->list->installEventFilter(edit->deleteFilter.get());  // Delete in the list
    edit->colorObserver = std::make_unique<Sketch3DColorObserver>(this);
    edit->solverUpdate = getSketch3DObject()->signalSolverUpdate.connect([this] {
        draw();
    });

    Workbench::enterEditMode();
    Gui::ToolBarManager::getInstance()->setState(sketch3DToolbars(),
                                                 Gui::ToolBarManager::State::ForceAvailable);
    setSketch3DToolIcons(true);
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
    finishDatumEdit(true);  // leaving is a press elsewhere
    if (edit && edit->placing) {
        edit->placing->finish();
    }
    if (edit && edit->turning) {
        edit->turning->finish();
    }
    Workbench::leaveEditMode();
    setSketch3DToolIcons(false);
    Gui::getMainWindow()->showMessage(QString());
    Gui::ToolBarManager::getInstance()->setState(sketch3DToolbars(),
                                                 Gui::ToolBarManager::State::RestoreDefault);

    if (edit) {
        Gui::getMainWindow()->removeEventFilter(edit->deleteFilter.get());
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
        // the labels and icons placed again when the view turns or zooms
        edit->refaceTimer = std::make_unique<QTimer>();
        edit->refaceTimer->setSingleShot(true);
        edit->refaceTimer->setInterval(40);
        QObject::connect(edit->refaceTimer.get(),
                         &QTimer::timeout,
                         edit->refaceTimer.get(),
                         [this]() {
                             reface();
                         });
        edit->cameraSensor =
            std::make_unique<SoNodeSensor>(&ViewProviderSketch3D::cameraChanged, this);
        edit->cameraSensor->attach(camera);
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
        edit->cameraSensor.reset();
        edit->refaceTimer.reset();
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

    // The active kept plane moved with a solve (a constrained one, section 6): the
    // manipulator turns with it and stays on it.
    if (edit->keptPlane >= 0) {
        auto kept = sketch->getGeometry(edit->keptPlane);
        if (kept && kept->isDerivedFrom<Part::GeomPlane>()) {
            auto pln = Handle(Geom_Plane)::DownCast(kept->handle());
            const gp_Ax3& ax = pln->Position();
            auto v = [](const gp_XYZ& x) {
                return Base::Vector3d(x.X(), x.Y(), x.Z());
            };
            Base::Vector3d o = v(ax.Location().XYZ()), n = v(ax.Direction().XYZ());
            edit->origin = edit->origin - n * ((edit->origin - o) * n);
            edit->rotation = Base::Rotation::makeRotationByAxes(v(ax.XDirection().XYZ()),
                                                                v(ax.YDirection().XYZ()),
                                                                n);
            edit->plane = Plane::XY;
            updateManipulator();
        }
        else {
            edit->keptPlane = -1;
        }
    }

    edit->curveRefs.clear();
    edit->pointRefs.clear();
    SbColor selectColor;
    {
        float transparency;
        selectColor.setPackedValue(uint32_t(Gui::ViewParams::getSelectionColor()), transparency);
    }
    auto selected = [&](int geoId, Sketcher::PointPos pos) {
        const auto& sel = edit->selection;
        return std::find(sel.begin(), sel.end(), Element {geoId, pos}) != sel.end();
    };
    auto addCurve = [&](const Part::Geometry* geo, const SbColor& color, int geoId) {
        std::size_t before = coords.size();
        sample(geo, coords);
        if (coords.size() > before) {
            counts.push_back(int32_t(coords.size() - before));
            colors.push_back(selected(geoId, Sketcher::PointPos::none) ? selectColor : color);
            edit->curveRefs.push_back(geoId);
        }
    };
    auto addPoint =
        [&](const Base::Vector3d& v, const SbColor& color, int geoId, Sketcher::PointPos pos) {
            pts.emplace_back(float(v.x), float(v.y), float(v.z));
            ptColors.push_back(selected(geoId, pos) ? selectColor : color);
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
            // A kept workplane (section 6): an outline square about its origin, three
            // gizmo lengths across as the view was at the draw, clear of the gizmo's
            // rings when the manipulator stands on it.
            auto pln = Handle(Geom_Plane)::DownCast(geo->handle());
            const gp_Ax3& ax = pln->Position();
            double h = 1.5 * GizmoAxis * std::max(1e-6f, edit->dragger->autoScaleResult.getValue());
            gp_Pnt o = ax.Location();
            gp_Vec x(ax.XDirection()), y(ax.YDirection());
            std::size_t before = coords.size();
            const double corners[5][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
            for (const auto& c : corners) {
                gp_Pnt p = o.Translated(x * (c[0] * h) + y * (c[1] * h));
                coords.emplace_back(float(p.X()), float(p.Y()), float(p.Z()));
            }
            counts.push_back(int32_t(coords.size() - before));
            colors.push_back(selected(geoId, Sketcher::PointPos::none) ? selectColor
                                 : geoId == edit->keptPlane            ? SbColor(1.f, 0.75f, 0.f)
                                                                       : constructionColor);
            edit->curveRefs.push_back(geoId);
            continue;
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
        else if (!geo->isDerivedFrom<Part::GeomSurface>()) {
            addCurve(geo, externalColor, extId);
        }
        --extId;
    }

    edit->curveCoords->point.setValues(0, int(coords.size()), coords.data());
    edit->curveCoords->point.setNum(int(coords.size()));
    edit->curveLines->numVertices.setValues(0, int(counts.size()), counts.data());
    edit->curveLines->numVertices.setNum(int(counts.size()));
    // a colour per segment, as the binding is: a curve's own for each of its
    // segments, or an arc's would take the colours of the curves after it
    std::vector<SbColor> segmentColors;
    for (std::size_t i = 0; i < counts.size() && i < colors.size(); ++i) {
        segmentColors.insert(segmentColors.end(), std::max(1, counts[i] - 1), colors[i]);
    }
    edit->curveMaterial->diffuseColor.setValues(0, int(segmentColors.size()), segmentColors.data());
    edit->curveMaterial->diffuseColor.setNum(int(segmentColors.size()));
    edit->curveStyle->lineWidth = 2.f * scale;

    edit->pointCoords->point.setValues(0, int(pts.size()), pts.data());
    edit->pointCoords->point.setNum(int(pts.size()));
    edit->pointMaterial->diffuseColor.setValues(0, int(ptColors.size()), ptColors.data());
    edit->pointMaterial->diffuseColor.setNum(int(ptColors.size()));
    edit->pointStyle->pointSize = 8.f * scale;
    edit->points->markerIndex = Gui::Inventor::MarkerBitmaps::getMarkerIndex("CIRCLE_FILLED", 7);
    // The geometry moved: drop selected ids it no longer has.
    {
        auto& sel = edit->selection;
        int n = sketch->Geometry.getSize();
        sel.erase(std::remove_if(sel.begin(),
                                 sel.end(),
                                 [n](const Element& e) {
                                     return e.geoId >= n;
                                 }),
                  sel.end());
        auto& cs = edit->selectedConstraints;
        int m = sketch->Constraints.getSize();
        cs.erase(std::remove_if(cs.begin(),
                                cs.end(),
                                [m](int i) {
                                    return i >= m;
                                }),
                 cs.end());
    }
    drawLabels();
    drawIcons();
    listConstraints();

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
            text += QStringLiteral("<br>")
                + QObject::tr("Redundant constraints: %1").arg(indexText(sketch->getRedundant()));
        }
        edit->dialog->setStatus(text);
    }
}
