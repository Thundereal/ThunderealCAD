// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include "PreCompiled.h"

#include <algorithm>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QWidget>
#include <QWidgetAction>
#include <Inventor/nodes/SoCamera.h>
#include <Inventor/sensors/SoNodeSensor.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/Expression.h>
#include <App/PropertyUnits.h>
#include <Base/Placement.h>
#include <Base/Quantity.h>

#include "OnViewValue.h"
#include "Control.h"
#include "Document.h"
#include "EditableDatumLabel.h"
#include "MiscParams.h"
#include "OverlayManager.h"
#include "OverlayWidgets.h"
#include "QuantitySpinBox.h"
#include "SoDatumLabel.h"
#include "SpinBox.h"
#include "ViewerContext.h"
#include "ViewProvider.h"
#include "Inventor/Draggers/Gizmo.h"

using namespace Gui;

struct OnViewValueSet::Entry
{
    QPointer<QuantitySpinBox> box;
    QString title;
    Place place = Place::TopMiddle;
    std::vector<Gizmo*> handles;
    /// Where each handle stands, in world coordinates; by the handle's place
    std::vector<Base::Vector3d> anchors;
    MenuProvider menu;
    int group = 0;
    /// Not used by the feature now: no label, passed over by Tab
    bool hidden = false;
    QMetaObject::Connection boxConnection;
    std::unique_ptr<EditableDatumLabel> label;
};

namespace
{
/// The sketcher's colour of a dimension an expression drives
SbColor expressionColor()
{
    auto hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/View");
    SbColor color;
    float transparency = 0.f;
    color.setPackedValue(uint32_t(hGrp->GetUnsigned("ExprBasedConstrDimColor", 0xFF7F00FF)),
                         transparency);
    return color;
}

/// A key that starts a number
bool startsNumber(const QKeyEvent* event)
{
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        return false;
    }
    const QString text = event->text();
    if (text.size() != 1) {
        return false;
    }
    const QChar c = text.at(0);
    return c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+')
        || c == QLatin1Char('.') || c == QLatin1Char(',');
}

/// A box's value with its unit. A box a form model made (Gui::Fw) may hold
/// its unit in the model only: the property it is bound to has it (entry
/// 170, a pad's length read "Length 5").
Base::Quantity quantityOf(const QuantitySpinBox* box)
{
    Base::Quantity value = box->value();
    if (value.getUnit().isEmpty() && box->isBound()) {
        if (auto prop = dynamic_cast<App::PropertyQuantity*>(box->boundPath().getProperty())) {
            value.setUnit(prop->getUnit());
        }
    }
    return value;
}

/// The camera's turn as a placement's rotation: a plane facing the camera
Base::Rotation facingCamera(SoCamera* camera)
{
    float q0 {}, q1 {}, q2 {}, q3 {};
    camera->orientation.getValue().getValue(q0, q1, q2, q3);
    return {q0, q1, q2, q3};
}

/** The edit's menu button (entry 173, docs/OnViewValueEditing.md 8.3): the
 * view cell's menu button (ViewArea.cpp) -- three strokes, faint at rest, on
 * a ground of its own when hovered -- as tall as an on-view label, always
 * shown while the set is in a view with widgets. A click opens the edit's
 * menu below it.
 */
class OnViewMenuButton: public QWidget
{
public:
    OnViewMenuButton(OnViewValueSet* set, QWidget* parent)
        : QWidget(parent)
        , set(set)
    {
        setObjectName(QStringLiteral("OnViewMenuButton"));
        setCursor(Qt::ArrowCursor);
        setToolTip(OnViewValueSet::tr("Edit menu"));
        setAttribute(Qt::WA_Hover);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            return QWidget::mousePressEvent(event);
        }
        event->accept();
        if (set) {
            QPointer<OnViewValueSet> keep = set;
            // after the press: the menu runs a loop of its own
            QTimer::singleShot(0, set, [keep]() {
                if (keep) {
                    keep->openMenuBelowButton();
                }
            });
        }
    }
    void enterEvent(QEnterEvent* event) override
    {
        QWidget::enterEvent(event);
        hover = true;
        update();
    }
    void leaveEvent(QEvent* event) override
    {
        QWidget::leaveEvent(event);
        hover = false;
        update();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (hover) {
            // the cell button's chrome ground (ViewArea.cpp paintChromeGround)
            p.setPen(QPen(QColor(255, 255, 255), 1));
            p.setBrush(OverlayDragFrame::accentColor(this));
            p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        }
        QColor c = hover ? QColor(Qt::white) : palette().color(QPalette::WindowText);
        if (!hover) {
            c.setAlpha(110);
        }
        // The cell button's mark scaled up (the reporter 2026-10-19 10:29,
        // 10:34, 13:39: strips as thick as the gaps between them, the whole
        // "more squarer like the cell menu"): three strips of t with gaps of
        // t make a block 5t tall, as wide as it is tall, square ended, on
        // whole pixels so the strips stay sharp
        const int t = std::max(3, int(std::lround(height() * 0.13)));
        const int block = 5 * t;
        const int left = (width() - block) / 2;
        const int top = (height() - block) / 2;
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.setRenderHint(QPainter::Antialiasing, false);
        for (int i = 0; i < 3; ++i) {
            p.drawRect(QRect(left, top + i * 2 * t, block, t));
        }
    }

private:
    QPointer<OnViewValueSet> set;
    bool hover = false;
};

/// How wide and tall \a label draws its text, in the view widget's pixels:
/// the font and size SoDatumLabel::drawImage() takes
QSize labelTextSize(const EditableDatumLabel* label)
{
    const SoDatumLabel* datum = label->label;
    if (!datum || datum->string.getNum() == 0) {
        return {};
    }
    QFont font(QString::fromUtf8(datum->name.getValue().getString()), datum->size.getValue());
    QFontMetrics fm(font);
    const QString text = QString::fromUtf8(datum->string[0].getString());
    return {fm.horizontalAdvance(text), fm.height()};
}
}  // namespace

namespace
{
/// The sets alive, for a key of a view without a widget (takeViewerKey())
std::vector<OnViewValueSet*>& liveSets()
{
    static std::vector<OnViewValueSet*> sets;
    return sets;
}
}  // namespace

OnViewValueSet::OnViewValueSet(QObject* parent)
    : QObject(parent)
{
    liveSets().push_back(this);
}

OnViewValueSet::~OnViewValueSet()
{
    auto& sets = liveSets();
    sets.erase(std::remove(sets.begin(), sets.end(), this), sets.end());
    attach(nullptr);
    truncate(0);
}

bool OnViewValueSet::takeViewerKey(ViewerContext* viewer, int key,
                                   Qt::KeyboardModifiers modifiers, bool press,
                                   const QString& text)
{
    if (!viewer) {
        return false;
    }
    for (OnViewValueSet* set : liveSets()) {
        if (set->liveView() == viewer && !set->keyWidget) {
            return set->takeMirrorKey(key, modifiers, press, text);
        }
    }
    return false;
}

void OnViewValueSet::viewCoverChanged(ViewerContext* viewer)
{
    for (OnViewValueSet* set : liveSets()) {
        if (viewer && set->liveView() == viewer) {
            set->placeLabels();
        }
    }
}

bool OnViewValueSet::buildViewerMenu(ViewerContext* viewer, QMenu& menu)
{
    if (!viewer) {
        return false;
    }
    for (OnViewValueSet* set : liveSets()) {
        if (set->liveView() != viewer || set->dragHandle) {
            continue;
        }
        for (const auto& entry : set->entries) {
            for (Gizmo* handle : entry->handles) {
                if (handle->isHovered()) {
                    set->buildMenu(menu, handle);
                    return true;
                }
            }
        }
    }
    return false;
}

bool OnViewValueSet::takeMirrorKey(int key, Qt::KeyboardModifiers modifiers, bool press,
                                   const QString& text)
{
    if (!press) {
        // the release of a key taken on its press is not the view's either
        auto it = std::find(takenKeys.begin(), takenKeys.end(), key);
        if (it == takenKeys.end()) {
            return false;
        }
        takenKeys.erase(it);
        return true;
    }
    // What the view widget's filter does (eventFilter(), takeKey()), less
    // Space: it opens a QMenu on the host's screen; the client's right
    // click on a handle is its menu (buildViewerMenu()). Enter and digits
    // open the active value's label: on a mirror its box is built unshown
    // and streamed to the client as an on-view entry, which then has the
    // keys (EditableDatumLabel::startEdit, docs/ThinClient.md sec 8.7)
    const bool plain =
        !(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    bool taken = false;
    if (dragHandle && key == Qt::Key_Escape) {
        dragHandle->cancelDrag();
        taken = true;
    }
    else if (typing < 0 && active >= 0 && plain
             && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
        taken = startTyping(active);
    }
    else if (typing < 0 && active >= 0 && !dragHandle) {
        QKeyEvent first(QEvent::KeyPress, key, modifiers, text);
        if (startsNumber(&first)) {
            taken = startTyping(active, &first);
        }
    }
    else if (typing < 0 && !entries.empty() && plain
             && (key == Qt::Key_Tab || key == Qt::Key_Backtab)) {
        moveActive(key == Qt::Key_Backtab || (modifiers & Qt::ShiftModifier));
        taken = true;
    }
    else if (typing < 0 && active >= 0 && plain && key == Qt::Key_Escape) {
        setActive(-1);
        taken = true;
    }
    if (taken && std::find(takenKeys.begin(), takenKeys.end(), key) == takenKeys.end()) {
        takenKeys.push_back(key);
    }
    return taken;
}

void OnViewValueSet::setupHandle(int index, Gizmo* handle)
{
    // The view keeps the focus: a value is typed at the handle, not in the
    // panel's box
    handle->setFocusBoxOnRelease(false);
    handle->setReleaseCallback([this, index, handle](bool) {
        setActive(index, handle);
    });
    // A right click on the handle opens its menu, and the pointer over it
    // says so (entry 135, the reporter 2026-10-15 11:30 and 11:35)
    handle->setMenu(
        [self = QPointer<OnViewValueSet>(this), index, handle]() {
            if (!self || index >= self->count()) {
                return;
            }
            self->setActive(index, handle);
            self->openMenu(handle, true);
        },
        tr("Drag to change the value; click to type it.\n"
           "Right click for the menu."));
    // The pointer over it lights its value's handles and its group's
    // (entry 144)
    handle->setHoverCallback([self = QPointer<OnViewValueSet>(this), index](bool over) {
        if (!self) {
            return;
        }
        if (over) {
            self->preselected = index;
        }
        else if (self->preselected == index) {
            self->preselected = -1;
        }
        self->highlight();
    });
    // A drag is one undo step (entry 136)
    handle->setDragCallback([self = QPointer<OnViewValueSet>(this), index, handle](
                                Gizmo::DragEvent event) {
        if (!self) {
            return;
        }
        // A cancelled drag put the box back without a word: its owner writes
        // the property from the box's signal, unless it rolls the drag back
        // itself
        if (event == Gizmo::DragEvent::Cancelled && self->cancelWritesBack
            && index < self->count()) {
            if (QuantitySpinBox* box = self->entries[index]->box) {
                Q_EMIT box->valueChanged(box->value());
                Q_EMIT box->valueChanged(box->value().getValue());
            }
        }
        switch (event) {
            case Gizmo::DragEvent::Started:
                self->dragHandle = handle;
                Q_EMIT self->stepBegun();
                break;
            case Gizmo::DragEvent::Finished:
            case Gizmo::DragEvent::Cancelled:
                if (self->dragHandle == handle) {
                    self->dragHandle = nullptr;
                }
                Q_EMIT self->stepEnded(event == Gizmo::DragEvent::Cancelled);
                break;
        }
    });
}

int OnViewValueSet::addValue(QuantitySpinBox* box, const QString& title, Place place,
                             Gizmo* handle)
{
    auto entry = std::make_unique<Entry>();
    entry->box = box;
    entry->title = title;
    entry->place = place;
    const int index = static_cast<int>(entries.size());
    // The label follows the box, whoever changes it: the panel, a drag, a
    // value typed here
    entry->boxConnection =
        connect(box, qOverload<double>(&QuantitySpinBox::valueChanged), this, [this, index]() {
            updateLabel(index);
        });
    entries.push_back(std::move(entry));
    if (handle) {
        addHandle(index, handle);
    }
    if (liveView()) {
        makeLabels();
        placeLabels();
    }
    return index;
}

void OnViewValueSet::addHandle(int index, Gizmo* handle)
{
    if (index < 0 || index >= count() || !handle) {
        return;
    }
    auto& entry = entries[index];
    entry->handles.push_back(handle);
    entry->anchors.emplace_back();
    setupHandle(index, handle);
    handle->setHighlighted(index == active);
    handle->setHiddenInView(!handlesShown);
}

void OnViewValueSet::clearHandles(int index)
{
    if (index < 0 || index >= count()) {
        return;
    }
    auto& entry = entries[index];
    for (Gizmo* handle : entry->handles) {
        if (handle == activeHandle) {
            activeHandle = nullptr;
        }
        handle->setReleaseCallback(nullptr);
        handle->setMenu(nullptr);
        handle->setDragCallback(nullptr);
        handle->setHoverCallback(nullptr);
        handle->setPreselected(false);
        handle->setHiddenInView(false);
        if (handle == rightPressHandle) {
            rightPressHandle = nullptr;
        }
        // taken away in its drag: the drag's step ends here
        if (handle == dragHandle) {
            dragHandle = nullptr;
            Q_EMIT stepEnded(false);
        }
        handle->setFocusBoxOnRelease(true);
    }
    entry->handles.clear();
    entry->anchors.clear();
}

void OnViewValueSet::setHandleAnchor(Gizmo* handle, const Base::Vector3d& world)
{
    for (auto& entry : entries) {
        for (std::size_t i = 0; i < entry->handles.size(); ++i) {
            if (entry->handles[i] == handle) {
                entry->anchors[i] = world;
                if (handle == activeHandle) {
                    placeLabels();
                }
                return;
            }
        }
    }
}

void OnViewValueSet::placeAnchors(const Base::Matrix4D& toWorld)
{
    for (auto& entry : entries) {
        for (std::size_t i = 0; i < entry->handles.size(); ++i) {
            const SbVec3f pos = entry->handles[i]->getDraggerPlacement().pos;
            entry->anchors[i] = toWorld * Base::Vector3d(pos[0], pos[1], pos[2]);
        }
    }
    if (activeHandle) {
        placeLabels();
    }
}

void OnViewValueSet::setHidden(int index, bool hidden)
{
    if (index < 0 || index >= count() || entries[index]->hidden == hidden) {
        return;
    }
    entries[index]->hidden = hidden;
    if (hidden && typing == index) {
        endTyping(false);
    }
    if (hidden && active == index) {
        setActive(-1);
        return;
    }
    showLabels();
    placeLabels();
}

void OnViewValueSet::setMenu(int index, MenuProvider provider)
{
    if (index >= 0 && index < count()) {
        entries[index]->menu = std::move(provider);
    }
}

void OnViewValueSet::setGroup(int index, int group)
{
    if (index >= 0 && index < count()) {
        entries[index]->group = group;
        highlight();
    }
}

void OnViewValueSet::truncate(int keep)
{
    keep = std::max(0, keep);
    if (keep >= count()) {
        return;
    }
    if (typing >= keep) {
        endTyping(false);
    }
    if (active >= keep) {
        setActive(-1);
    }
    while (count() > keep) {
        disconnect(entries.back()->boxConnection);
        // The handles stay with their owner, who deletes them next; what the
        // set gave them goes
        clearHandles(count() - 1);
        entries.pop_back();
    }
}

int OnViewValueSet::count() const
{
    return static_cast<int>(entries.size());
}

int OnViewValueSet::valueOf(const Gizmo* handle) const
{
    for (int i = 0; i < count(); ++i) {
        const auto& handles = entries[i]->handles;
        if (std::find(handles.begin(), handles.end(), handle) != handles.end()) {
            return i;
        }
    }
    return -1;
}

ViewerContext* OnViewValueSet::getView() const
{
    return liveView();
}

ViewerContext* OnViewValueSet::liveView() const
{
    return viewLife.expired() ? nullptr : view;
}

void OnViewValueSet::attach(ViewerContext* newView)
{
    // A view that is gone is only let go of: every use of it below asks
    // liveView(), its labels forgot it with it (OnViewEntry::forgetViewer),
    // and the camera sensor and the connection let go of what is gone
    if (newView == view && liveView()) {
        return;
    }
    if (view) {
        endTyping(false);
        setActive(-1);
        dropLabels();
        connCameraReplaced.disconnect();
        if (cameraSensor) {
            cameraSensor->detach();
            delete cameraSensor;
            cameraSensor = nullptr;
        }
        if (keyWidget) {
            keyWidget->removeEventFilter(this);
        }
        keyWidget = nullptr;
        delete menuButton.data();
        QObject::disconnect(coverConnection);
    }
    view = newView;
    viewLife = view ? view->lifetime() : std::weak_ptr<const void>();
    if (!view) {
        return;
    }

    cameraSensor = new SoNodeSensor(
        [](void* data, SoSensor*) {
            static_cast<OnViewValueSet*>(data)->onCamera();
        },
        this);
    if (SoCamera* camera = view->getCamera()) {
        cameraSensor->attach(camera);
    }
    // A change of projection replaces the camera node
    connCameraReplaced = view->signalCameraReplaced.connect([this]() {
        if (!cameraSensor || !view) {
            return;
        }
        cameraSensor->detach();
        if (SoCamera* camera = view->getCamera()) {
            cameraSensor->attach(camera);
        }
        onCamera();
    });

    // Keys typed in the view go to the viewer itself (setFocusToView); a view
    // without widgets (a mirror) has none
    keyWidget = view->getWidget();
    if (keyWidget) {
        keyWidget->installEventFilter(this);
        // The edit's menu button, always shown (entry 173); the column of
        // top-middle labels and it keep clear of what is laid over the view
        // (8.4), placed again when that moves
        menuButton = new OnViewMenuButton(this, keyWidget);
        menuButton->show();
        coverConnection = connect(OverlayManager::instance(), &OverlayManager::coverChanged,
                                  this, [this]() {
                                      placeLabels();
                                  });
    }

    makeLabels();
    placeLabels();
}

void OnViewValueSet::attachToEdit(const ViewProvider* vp, const GizmoContainer* container)
{
    if (vp) {
        if (auto editView = vp->getEditViewer()) {
            attach(editView);
        }
    }
    if (container) {
        placeAnchors(container->getWorldMatrix());
    }
    refresh();
}

void OnViewValueSet::makeLabels()
{
    for (auto& entry : entries) {
        if (entry->label) {
            continue;
        }
        auto label = std::make_unique<EditableDatumLabel>(view, Base::Placement());
        label->setLabelType(SoDatumLabel::TEXT, EditableDatumLabel::Function::Dimensioning);
        label->setPoints(SbVec3f(0, 0, 0), SbVec3f(0, 0, 0));
        label->setPickable(true);
        connect(label.get(), &EditableDatumLabel::clicked, this, &OnViewValueSet::onLabelClicked);
        entry->label = std::move(label);
    }
    for (int i = 0; i < count(); ++i) {
        updateLabel(i);
    }
    showLabels();
}

void OnViewValueSet::dropLabels()
{
    for (auto& entry : entries) {
        entry->label.reset();
    }
}

void OnViewValueSet::showLabels()
{
    // The top middle ones always, a handle's while its value is active
    for (int i = 0; i < count(); ++i) {
        auto& label = entries[i]->label;
        if (!label) {
            continue;
        }
        const bool shown =
            !entries[i]->hidden && (entries[i]->place == Place::TopMiddle || i == active);
        if (shown && !label->isActive()) {
            label->activate();
        }
        else if (!shown && label->isActive()) {
            label->deactivate();
        }
    }
}

void OnViewValueSet::placeLabels()
{
    if (!liveView()) {
        return;
    }
    SoCamera* camera = view->getCamera();
    if (!camera) {
        return;
    }
    const SbVec2s size = view->getViewportRegion().getViewportSizePixels();
    const double ratio = std::max(1.0, view->devicePixelRatio());
    // Facing the camera, so that the text is upright on the screen
    const Base::Rotation rotation = facingCamera(camera);

    // At the top middle: a feature's default value, and an active value
    // whose handles this view hides, as there is none to stand by (8.5)
    auto atTop = [this](int i) {
        const auto& entry = entries[i];
        return !entry->hidden && entry->label
            && (entry->place == Place::TopMiddle || (!handlesShown && i == active));
    };
    // The column stands in the middle of what no overlay covers (8.4), the
    // menu button at the first label's left
    int widest = 0;
    int firstWidth = 0;
    bool first = true;
    for (int i = 0; i < count(); ++i) {
        if (atTop(i)) {
            const int w = labelTextSize(entries[i]->label.get()).width();
            widest = std::max(widest, w);
            if (first) {
                firstWidth = w;
                first = false;
            }
        }
    }
    const int needed = widest + 2 * (menuButtonSide() + 8);
    int middle = clearMiddle(needed);
    const short middlePixel = middle >= 0 ? short(middle * ratio) : short(size[0] / 2);
    placeMenuButton(middle, firstWidth);
    // A view with no widget is a client's, whose own interface may cover
    // its top -- a browser's tool bars: the column goes below what covers
    // its middle (docs/OnViewValueEditing.md sec 9.5a)
    int below = 0;
    if (!keyWidget) {
        const double width = size[0] / ratio;
        const double height = size[1] / ratio;
        const double left = width / 2 - needed / 2.0;
        const double right = width / 2 + needed / 2.0;
        for (const auto& r : view->coveredRects()) {
            if (r[0] < right && r[0] + r[2] > left && r[1] < height / 3) {
                below = std::max(below, r[1] + r[3]);
            }
        }
    }

    int row = 0;
    for (int i = 0; i < count(); ++i) {
        auto& entry = entries[i];
        if (!entry->label) {
            continue;
        }
        SbVec2s pixel;
        if (entry->hidden) {
            continue;
        }
        if (atTop(i)) {
            // One under the other, from the top middle down
            pixel = SbVec2s(middlePixel,
                            short(size[1] - int((below + TopMargin + row * TopMargin) * ratio)));
            ++row;
        }
        else {
            // Beside the active handle, up and to the right of it
            if (i != active || entry->handles.empty()) {
                continue;
            }
            std::size_t h = 0;
            auto it = std::find(entry->handles.begin(), entry->handles.end(), activeHandle);
            if (it != entry->handles.end()) {
                h = std::distance(entry->handles.begin(), it);
            }
            const Base::Vector3d& anchor = entry->anchors[h];
            const SbVec2s at = view->getPointOnViewport(SbVec3f(float(anchor.x), float(anchor.y), float(anchor.z)));
            const int offset = int(HandleOffset * ratio);
            pixel = SbVec2s(short(at[0] + offset), short(at[1] + offset));
        }
        const SbVec3f p = view->getPointOnFocalPlane(pixel);
        entry->label->setPlacement(Base::Placement(Base::Vector3d(p[0], p[1], p[2]), rotation));
        entry->label->setPoints(SbVec3f(0, 0, 0), SbVec3f(0, 0, 0));
    }
}

void OnViewValueSet::updateLabel(int index)
{
    if (index < 0 || index >= count()) {
        return;
    }
    auto& entry = entries[index];
    if (!entry->label || !entry->box || entry->label->isInEdit()) {
        return;
    }
    const Base::Quantity value = quantityOf(entry->box);
    // "Radius 2 mm", not "Radius 2.00 mm" (as the omni search writes it)
    QString text = trimTrailingZeros(QString::fromStdString(value.getUserString()), QLocale());
    if (!entry->title.isEmpty()) {
        text = entry->title + QLatin1Char(' ') + text;
    }
    if (entry->box->hasExpression()) {
        // what drives it, after the value
        const App::ObjectIdentifier& path = entry->box->boundPath();
        if (auto obj = path.getDocumentObject()) {
            auto info = obj->getExpression(path);
            if (info.expression) {
                text += QStringLiteral(" = ") + QString::fromStdString(info.expression->toString());
            }
        }
        entry->label->setColor(expressionColor());
    }
    else {
        entry->label->setActivatedColor();
    }
    entry->label->label->string = text.toUtf8().constData();
}

void OnViewValueSet::highlight()
{
    // Every handle of the active value -- a default drives all its edges --
    // and of every value of its group, selected; those of the value under
    // the pointer and of its group, pre-selected (entry 144)
    const int group = active >= 0 && active < count() ? entries[active]->group : 0;
    const int hovered = preselected >= 0 && preselected < count() ? preselected : -1;
    const int hoverGroup = hovered >= 0 ? entries[hovered]->group : 0;
    for (int i = 0; i < count(); ++i) {
        const bool lit = i == active || (group > 0 && entries[i]->group == group);
        const bool prelit = i == hovered || (hoverGroup > 0 && entries[i]->group == hoverGroup);
        for (Gizmo* handle : entries[i]->handles) {
            handle->setHighlighted(lit);
            handle->setPreselected(prelit && !lit);
        }
    }
}

void OnViewValueSet::refresh()
{
    highlight();
    for (int i = 0; i < count(); ++i) {
        updateLabel(i);
    }
    showLabels();
    placeLabels();
}

void OnViewValueSet::onCamera()
{
    placeLabels();
    Q_EMIT cameraChanged();
}

void OnViewValueSet::setActive(int index, Gizmo* handle)
{
    if (index < -1 || index >= count()) {
        index = -1;
    }
    // what a click on nothing checks for: a value made active meanwhile
    ++activations;
    // A value an expression drives is not typed in the view, nor one the
    // feature does not use now
    if (index >= 0
        && ((entries[index]->box && entries[index]->box->hasExpression())
            || entries[index]->hidden)) {
        index = -1;
    }
    if (typing >= 0 && typing != index) {
        endTyping(false);
    }
    const bool changed = index != active;
    active = index;
    activeHandle = nullptr;
    if (active >= 0) {
        const auto& handles = entries[active]->handles;
        if (handle && std::find(handles.begin(), handles.end(), handle) != handles.end()) {
            activeHandle = handle;
        }
        else if (!handles.empty()) {
            activeHandle = handles.front();
        }
    }
    highlight();
    showLabels();
    placeLabels();
    if (active >= 0) {
        focusView();
    }
    if (changed) {
        Q_EMIT activeChanged(active);
    }
}

int OnViewValueSet::getActive() const
{
    return active;
}

Gizmo* OnViewValueSet::getActiveHandle() const
{
    return active >= 0 ? activeHandle : nullptr;
}

int OnViewValueSet::getTyping() const
{
    return typing;
}

void OnViewValueSet::openMenuBelowButton()
{
    // The button's click: below the button, whatever handle is active -- at
    // the active handle it opened far from where the user clicked (entry
    // 173, the reporter 2026-10-19 14:09)
    if (!menuButton || !menuButton->isVisible()) {
        openMenu(nullptr, true);
        return;
    }
    openMenu(nullptr, false, menuButton->mapToGlobal(QPoint(0, menuButton->height())));
}

void OnViewValueSet::openMenu(Gizmo* handle, bool atPointer, std::optional<QPoint> at)
{
    if (!handle) {
        handle = getActiveHandle();
    }
    const int index = handle ? valueOf(handle) : active;
    QMenu menu(keyWidget);
    buildMenu(menu, handle);
    if (menu.isEmpty()) {
        return;
    }
    if (at) {
        menu.exec(*at);
        focusView();
        return;
    }
    // Opened by a click, where the pointer is; by a key, at the handle, or
    // below the menu button with no handle to stand by
    QPoint where = QCursor::pos();
    if (!atPointer && !handle && menuButton && menuButton->isVisible()) {
        where = menuButton->mapToGlobal(QPoint(0, menuButton->height()));
    }
    if (!atPointer && handle && index >= 0 && liveView() && keyWidget) {
        const auto& entry = entries[index];
        auto it = std::find(entry->handles.begin(), entry->handles.end(), handle);
        if (it != entry->handles.end()) {
            const Base::Vector3d& anchor = entry->anchors[std::distance(entry->handles.begin(), it)];
            const SbVec2s at = view->getPointOnViewport(
                SbVec3f(float(anchor.x), float(anchor.y), float(anchor.z)));
            where = keyWidget->mapToGlobal(view->toQPoint(at));
        }
    }
    menu.exec(where);
    focusView();
}

namespace
{
std::vector<OnViewValueSet::MenuExtension>& menuExtensions()
{
    static std::vector<OnViewValueSet::MenuExtension> extensions;
    return extensions;
}

/// A row of a menu that is a widget: a label and \a field, laid out as the
/// menu's text is
QWidgetAction* fieldRow(QMenu* menu, const QString& text, QWidget* field)
{
    auto action = new QWidgetAction(menu);
    auto row = new QWidget(menu);
    auto layout = new QHBoxLayout(row);
    layout->setContentsMargins(menu->style()->pixelMetric(QStyle::PM_MenuHMargin) + 24, 2, 8, 2);
    auto label = new QLabel(text, row);
    layout->addWidget(label);
    layout->addStretch();
    field->setParent(row);
    layout->addWidget(field);
    action->setDefaultWidget(row);
    return action;
}

/// A check box row of a menu
QWidgetAction* checkRow(QMenu* menu, const QString& text, QCheckBox*& box)
{
    auto action = new QWidgetAction(menu);
    auto row = new QWidget(menu);
    auto layout = new QHBoxLayout(row);
    layout->setContentsMargins(menu->style()->pixelMetric(QStyle::PM_MenuHMargin) + 4, 2, 8, 2);
    box = new QCheckBox(text, row);
    layout->addWidget(box);
    action->setDefaultWidget(row);
    return action;
}
}  // namespace

void OnViewValueSet::addMenuExtension(MenuExtension extension)
{
    menuExtensions().push_back(std::move(extension));
}

void OnViewValueSet::buildMenu(QMenu& menu, Gizmo* handle)
{
    const int index = handle ? valueOf(handle) : active;
    if (index >= 0 && index < count() && entries[index]->menu) {
        entries[index]->menu(menu, handle);
    }
    if (!menu.isEmpty()) {
        menu.addSeparator();
    }
    // The view's own, not a preference (8.5); after the menu has closed: a
    // handle hidden under the pointer while its menu runs is taken from it
    QAction* shown = menu.addAction(tr("Show handles"));
    shown->setCheckable(true);
    shown->setChecked(handlesShown);
    connect(shown, &QAction::toggled, this, [this](bool on) {
        QTimer::singleShot(0, this, [this, on]() {
            setHandlesShown(on);
        });
    });
    for (const auto& extension : menuExtensions()) {
        extension(*this, menu);
    }
    addSettingsMenu(menu);
}

void OnViewValueSet::addSettingsMenu(QMenu& menu)
{
    // The drag settings of Preferences > Part/Part Design > General, as
    // widgets written to the preference the moment they change (entry 171:
    // "change it takes effect immediately"); a widget in a menu does not
    // close it, so several can be set at once. Global for now
    // (docs/UserSettings.md: per user later, through these same accessors).
    QMenu* settings = menu.addMenu(tr("Settings"));

    QCheckBox* delayed = nullptr;
    settings->addAction(checkRow(settings, tr("Disable recompute while dragging"), delayed));
    // each named after its preference, which is what a test finds it by
    delayed->setObjectName(QStringLiteral("OnViewSettingDelayedGizmoUpdate"));
    delayed->setChecked(MiscParams::getDelayedGizmoUpdate());
    delayed->setToolTip(tr("Prevents the model from recalculating while manipulating draggers. "
                           "The shape updates only after release of the mouse button."));
    connect(delayed, &QCheckBox::toggled, [](bool on) {
        MiscParams::setDelayedGizmoUpdate(on);
    });

    QCheckBox* coarse = nullptr;
    settings->addAction(checkRow(settings, tr("Enable coarse snapping while dragging"), coarse));
    coarse->setObjectName(QStringLiteral("OnViewSettingEnableCoarseSnap"));
    coarse->setChecked(MiscParams::getEnableCoarseSnap());
    coarse->setToolTip(tr("Enables larger snapping increments while manipulating draggers"));
    connect(coarse, &QCheckBox::toggled, [](bool on) {
        MiscParams::setEnableCoarseSnap(on);
    });

    auto modifier = new QComboBox;
    modifier->setObjectName(QStringLiteral("OnViewSettingFineSnapModifier"));
    modifier->addItem(tr("Shift"), static_cast<int>(Qt::ShiftModifier));
    modifier->addItem(tr("Ctrl"), static_cast<int>(Qt::ControlModifier));
    modifier->setCurrentIndex(
        GizmoContainer::getFineSnapModifier() == Qt::ControlModifier ? 1 : 0);
    modifier->setToolTip(tr("Defines the modifier key used for fine snapping while dragging"));
    settings->addAction(fieldRow(settings, tr("Fine snap modifier"), modifier));
    connect(modifier, qOverload<int>(&QComboBox::currentIndexChanged), [modifier](int i) {
        MiscParams::setFineSnapModifier(modifier->itemData(i).toInt());
    });

    auto behavior = new QComboBox;
    behavior->setObjectName(QStringLiteral("OnViewSettingDefaultCoarseDragBehavior"));
    behavior->addItem(tr("Coarse"), 0);
    behavior->addItem(tr("Fine"), 1);
    behavior->setCurrentIndex(GizmoContainer::isCoarseByDefault() ? 0 : 1);
    behavior->setToolTip(
        tr("Determines whether the drag is coarse or fine without holding the modifier key"));
    settings->addAction(fieldRow(settings, tr("Default coarse drag behavior"), behavior));
    connect(behavior, qOverload<int>(&QComboBox::currentIndexChanged), [behavior](int i) {
        MiscParams::setDefaultCoarseDragBehavior(behavior->itemData(i).toInt());
    });

    auto linear = new QSpinBox;
    linear->setObjectName(QStringLiteral("OnViewSettingCoarseLinearSnapMultiplier"));
    linear->setRange(1, 100);
    linear->setValue(int(MiscParams::getCoarseLinearSnapMultiplier()));
    linear->setToolTip(tr("Multiplies the base movement increment when coarse snapping is active"));
    settings->addAction(fieldRow(settings, tr("Coarse movement multiplier"), linear));
    connect(linear, qOverload<int>(&QSpinBox::valueChanged), [](int v) {
        MiscParams::setCoarseLinearSnapMultiplier(v);
    });

    auto rotation = new QSpinBox;
    rotation->setObjectName(QStringLiteral("OnViewSettingCoarseRotationSnapMultiplier"));
    rotation->setRange(1, 100);
    rotation->setValue(int(MiscParams::getCoarseRotationSnapMultiplier()));
    rotation->setToolTip(
        tr("Sets the rotation step in degrees applied when coarse snapping is active"));
    settings->addAction(fieldRow(settings, tr("Coarse rotation step (degrees)"), rotation));
    connect(rotation, qOverload<int>(&QSpinBox::valueChanged), [](int v) {
        MiscParams::setCoarseRotationSnapMultiplier(v);
    });
}

int OnViewValueSet::menuButtonSide() const
{
    // as tall as a label: its text's height and a margin
    for (const auto& entry : entries) {
        if (entry->label) {
            const int h = labelTextSize(entry->label.get()).height();
            if (h > 0) {
                return h + 8;
            }
        }
    }
    return 24;
}

int OnViewValueSet::clearMiddle(int needed) const
{
    if (!keyWidget) {
        return -1;
    }
    const int middle = keyWidget->width() / 2;
    const QRegion covered = OverlayManager::instance()->covered(keyWidget);
    if (covered.isEmpty()) {
        return middle;
    }
    // Only the band the column stands in counts: an overlay lower down hides
    // none of it
    const QRect band(0, 0, keyWidget->width(), TopMargin * 2 + menuButtonSide());
    const QRect clear = (QRegion(band) - covered).boundingRect();
    if (clear.isEmpty() || clear.width() < needed) {
        return middle;
    }
    return clear.center().x();
}

void OnViewValueSet::placeMenuButton(int middle, int labelWidth)
{
    if (!menuButton || !keyWidget || middle < 0) {
        return;
    }
    const int side = menuButtonSide();
    // at the first top-middle label's left, centred with it; with none, at
    // the top middle itself
    const int x = labelWidth > 0 ? middle - labelWidth / 2 - 8 - side : middle - side / 2;
    menuButton->setGeometry(std::max(0, x), std::max(0, TopMargin - side / 2), side, side);
    menuButton->raise();
}

void OnViewValueSet::setHandlesShown(bool shown)
{
    if (handlesShown == shown) {
        return;
    }
    handlesShown = shown;
    for (const auto& entry : entries) {
        for (Gizmo* handle : entry->handles) {
            handle->setHiddenInView(!shown);
        }
    }
    // an active value with no handle to stand by has its label at the top
    // middle (8.5)
    showLabels();
    placeLabels();
}

bool OnViewValueSet::startTyping(int index, QKeyEvent* first)
{
    if (index < 0 || index >= count() || !liveView()) {
        return false;
    }
    auto& entry = entries[index];
    if (!entry->label || !entry->box || entry->box->hasExpression()) {
        return false;
    }
    endTyping(false);
    if (active != index) {
        setActive(index);
    }
    typing = index;
    EditableDatumLabel* label = entry->label.get();
    const Base::Quantity value = quantityOf(entry->box);
    // An open label is one undo step, however often Enter commits in it
    Q_EMIT stepBegun();
    // This set's filter on the label's box, ahead of the label's own: Enter,
    // Tab and Escape are handled here (takeLabelKey)
    label->startEdit(value.getValue(), this);
    label->setSpinboxValue(value.getValue(), value.getUnit());
    // Setting the value again unselected the number; selected, the first key
    // replaces it
    label->setFocusToSpinbox();

    // The key that closes the label is pressed in its box, and the focus is
    // back in the view by the time it is let go: its release is the view's
    // to ignore -- Escape's would leave the edit
    auto closedBy = [this](std::initializer_list<int> keys) {
        for (int k : keys) {
            if (std::find(takenKeys.begin(), takenKeys.end(), k) == takenKeys.end()) {
                takenKeys.push_back(k);
            }
        }
    };
    // Emptied: a default has no "no value of its own", the box keeps it
    typingConnections.push_back(
        connect(label, &EditableDatumLabel::parameterUnset, this, [this, closedBy]() {
            closedBy({Qt::Key_Return, Qt::Key_Enter, Qt::Key_Tab});
            endTyping(false);
        }));
    typingConnections.push_back(
        connect(label, &EditableDatumLabel::focusLost, this, [this]() {
            // Later: the focus may be going to what Enter or Tab opens. The
            // focus going elsewhere commits what was typed and closes the
            // label (the reporter, 10:13: "typed then click away without
            // enter will commit and dismiss").
            QTimer::singleShot(0, this, [this]() {
                if (typing >= 0) {
                    writeTyped();
                    endTyping(true);
                }
            });
        }));

    if (first) {
        label->sendKeyEvent(first);
    }
    return true;
}

void OnViewValueSet::commit(int index, double value)
{
    QPointer<QuantitySpinBox> box = entries[index]->box;
    // Written while the label is open: inside its step
    if (box) {
        value = std::clamp(value, box->minimum(), box->maximum());
        // The box's owner writes the property from here, as from the panel
        box->setValue(value);
    }
    endTyping(true);
    if (index < count()) {
        updateLabel(index);
    }
    focusView();
}

void OnViewValueSet::writeTyped()
{
    if (typing < 0 || typing >= count()) {
        return;
    }
    auto& entry = entries[typing];
    // What the label's box parsed as it was typed (isSet: something valid
    // was), in the value's own unit -- its text read again lost the unit
    if (!entry->label || !entry->box || !entry->label->isSet) {
        return;
    }
    const double value = std::clamp(entry->label->value, entry->box->minimum(),
                                    entry->box->maximum());
    if (value != entry->box->rawValue()) {
        // The box's owner writes the property from here, as from the panel
        entry->box->setValue(value);
    }
}

bool OnViewValueSet::takeLabelKey(QKeyEvent* event)
{
    if (typing < 0 || typing >= count()) {
        return false;
    }
    const int key = event->key();
    auto taken = [this](int k) {
        if (std::find(takenKeys.begin(), takenKeys.end(), k) == takenKeys.end()) {
            takenKeys.push_back(k);
        }
    };
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        // Committed, the label left open, its number selected for the next
        // (the reporter, 10:07: "Press enter, commit change and trigger
        // fillet partial recompute, but shall not dismiss the edit box")
        writeTyped();
        taken(key);
        auto& label = entries[typing]->label;
        if (label && entries[typing]->box) {
            const Base::Quantity value = entries[typing]->box->value();
            label->setSpinboxValue(value.getValue(), value.getUnit());
            label->setFocusToSpinbox();
        }
        return true;
    }
    if (key == Qt::Key_Escape) {
        // What was typed since the last Enter goes; what Enter committed stays
        taken(key);
        endTyping(false);
        return true;
    }
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
        // Committed, closed, and on to the next value (10:13)
        const bool back = key == Qt::Key_Backtab || (event->modifiers() & Qt::ShiftModifier);
        taken(key);
        writeTyped();
        endTyping(true);
        moveActive(back);
        return true;
    }
    return false;
}

void OnViewValueSet::endTyping(bool keep)
{
    for (const auto& connection : typingConnections) {
        disconnect(connection);
    }
    typingConnections.clear();
    const int index = typing;
    typing = -1;
    if (index < 0) {
        return;
    }
    // the label's step ends, open since startTyping()
    Q_EMIT stepEnded(false);
    // A view with no widget keeps which entry has its keys itself: the
    // closed label's no longer, so the next key comes to this set again
    if (!keyWidget && liveView()) {
        view->onViewParameterFocused(nullptr);
    }
    if (index >= count()) {
        return;
    }
    if (auto& label = entries[index]->label) {
        if (label->isInEdit()) {
            label->stopEdit(keep);
        }
    }
    updateLabel(index);
    focusView();
}

void OnViewValueSet::focusView()
{
    if (liveView() && keyWidget && !keyWidget->hasFocus()) {
        view->setFocusToView();
    }
}

void OnViewValueSet::onLabelClicked(EditableDatumLabel* label)
{
    for (int i = 0; i < count(); ++i) {
        auto& entry = entries[i];
        if (entry->label.get() != label) {
            continue;
        }
        if (entry->box && entry->box->hasExpression()) {
            // its expression is where it is changed: the panel's box shows it
            entry->box->setFocus();
            entry->box->selectAll();
            return;
        }
        if (typing != i) {
            startTyping(i);
        }
        return;
    }
}

void OnViewValueSet::moveActive(bool back)
{
    // from none to the first or the last; each value an expression drives is
    // passed over
    int index = active;
    for (int tries = 0; tries < count(); ++tries) {
        if (index < 0) {
            index = back ? count() - 1 : 0;
        }
        else {
            index = (index + (back ? count() - 1 : 1)) % count();
        }
        if (entries[index]->box && !entries[index]->box->hasExpression()
            && !entries[index]->hidden) {
            setActive(index);
            return;
        }
    }
}

bool OnViewValueSet::takeKey(QKeyEvent* event, bool overrideOnly)
{
    // An open label's box has the keys itself
    if (typing >= 0 || entries.empty()) {
        return false;
    }
    const int key = event->key();
    const bool plain = !(event->modifiers()
                         & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if ((key == Qt::Key_Tab || key == Qt::Key_Backtab) && plain) {
        if (overrideOnly) {
            return true;
        }
        moveActive(key == Qt::Key_Backtab || (event->modifiers() & Qt::ShiftModifier));
        return true;
    }
    // Space: the edit's menu (docs/OnViewValueEditing.md 8.2) -- the active
    // handle's, as a right click on it opens it, or with no active value
    // the menu button's. In an edit with values Space no longer toggles
    // visibility.
    if (key == Qt::Key_Space && plain) {
        if (!overrideOnly) {
            // after the key: the menu runs a loop of its own
            QTimer::singleShot(0, this, [this]() {
                openMenu();
            });
        }
        return true;
    }
    if (active < 0) {
        return false;
    }
    if (key == Qt::Key_Escape && plain) {
        if (!overrideOnly) {
            setActive(-1);
        }
        return true;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && plain) {
        if (!overrideOnly) {
            startTyping(active);
        }
        return true;
    }
    if (startsNumber(event)) {
        if (!overrideOnly) {
            startTyping(active, event);
        }
        return true;
    }
    return false;
}

bool OnViewValueSet::eventFilter(QObject* watched, QEvent* event)
{
    // The open label's box: Enter, Tab and Escape are this set's, ahead of
    // the label (it is the only box this set watches)
    if (watched != keyWidget && typing >= 0 && qobject_cast<QuantitySpinBox*>(watched)) {
        if (event->type() == QEvent::ShortcutOverride) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape
                || key == Qt::Key_Tab || key == Qt::Key_Backtab) {
                event->accept();
                return true;
            }
        }
        else if (event->type() == QEvent::KeyPress
                 && takeLabelKey(static_cast<QKeyEvent*>(event))) {
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
    if (watched == keyWidget && dragHandle) {
        // During a drag Esc or another mouse button cancels it, the value back
        // to where it began (entry 136); the key and the button are taken,
        // so neither leaves the edit nor opens a menu
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            if (std::find(takenKeys.begin(), takenKeys.end(), int(Qt::Key_Escape))
                == takenKeys.end()) {
                takenKeys.push_back(Qt::Key_Escape);
            }
            dragHandle->cancelDrag();
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) {
            cancelButton = static_cast<QMouseEvent*>(event)->button();
            dragHandle->cancelDrag();
            return true;
        }
    }
    if (watched == keyWidget && cancelButton && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent*>(event)->button() == cancelButton) {
        cancelButton = 0;
        return true;
    }
    // A right click on a handle: its menu (entry 135), not the view's
    if (watched == keyWidget && !dragHandle && event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent*>(event)->button() == Qt::RightButton) {
        rightPressHandle = nullptr;
        for (const auto& entry : entries) {
            for (Gizmo* handle : entry->handles) {
                if (handle->isHovered()) {
                    rightPressHandle = handle;
                }
            }
        }
        if (rightPressHandle) {
            return true;
        }
    }
    if (watched == keyWidget && rightPressHandle && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent*>(event)->button() == Qt::RightButton) {
        Gizmo* handle = rightPressHandle;
        rightPressHandle = nullptr;
        if (valueOf(handle) >= 0 && handle->isHovered()) {
            // after the event: the menu runs a loop of its own
            QTimer::singleShot(0, this, [this, handle]() {
                if (valueOf(handle) >= 0) {
                    handle->openMenu();
                }
            });
        }
        return true;
    }
    // A click anywhere but on a handle -- on nothing, or picking anything
    // else -- leaves no value active (entry 159, the reporter 2026-10-16
    // 10:37, and 2026-10-19 15:18 and 15:21: "click picking anything else
    // should also clear the active handle"); what it picks is selected as
    // usual. Only a click: a press that moves is the camera's. Looked at
    // after the scene had the release, and only if nothing was made active
    // meanwhile -- a click on a handle or on a label makes its value active.
    if (watched == keyWidget && !dragHandle && event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        clickMayClear = false;
        if (active >= 0) {
            bool overHandle = false;
            for (const auto& entry : entries) {
                for (Gizmo* handle : entry->handles) {
                    overHandle = overHandle || handle->isHovered();
                }
            }
            if (!overHandle) {
                clickMayClear = true;
                clickPos = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
                clickActivations = activations;
            }
        }
    }
    if (watched == keyWidget && clickMayClear && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        clickMayClear = false;
        const QPoint at = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
        if ((at - clickPos).manhattanLength() <= QApplication::startDragDistance()) {
            const int before = clickActivations;
            QTimer::singleShot(0, this, [this, before]() {
                if (activations == before && active >= 0 && typing < 0 && !dragHandle) {
                    setActive(-1);
                }
            });
        }
    }
    if (watched == keyWidget && event->type() == QEvent::Resize) {
        // the column and the menu button stand by the view's middle
        QTimer::singleShot(0, this, [this]() {
            placeLabels();
        });
    }
    if (watched == keyWidget) {
        switch (event->type()) {
            case QEvent::ShortcutOverride:
                // A key taken here is not a shortcut: digits change the view
                // and Space toggles visibility otherwise
                if (takeKey(static_cast<QKeyEvent*>(event), true)) {
                    event->accept();
                    return true;
                }
                break;
            case QEvent::KeyPress: {
                auto ke = static_cast<QKeyEvent*>(event);
                if (!ke->isAutoRepeat()) {
                    // a fresh press: whatever release was waiting is gone
                    takenKeys.erase(std::remove(takenKeys.begin(), takenKeys.end(), ke->key()),
                                    takenKeys.end());
                }
                if (takeKey(ke, false)) {
                    if (std::find(takenKeys.begin(), takenKeys.end(), ke->key())
                        == takenKeys.end()) {
                        takenKeys.push_back(ke->key());
                    }
                    return true;
                }
                break;
            }
            case QEvent::KeyRelease: {
                // the release of a key taken on its press is not the view's
                auto ke = static_cast<QKeyEvent*>(event);
                auto it = std::find(takenKeys.begin(), takenKeys.end(), ke->key());
                if (it != takenKeys.end()) {
                    if (!ke->isAutoRepeat()) {
                        takenKeys.erase(it);
                    }
                    return true;
                }
                break;
            }
            case QEvent::Resize:
                // the top middle moved
                QTimer::singleShot(0, this, [this]() {
                    placeLabels();
                });
                break;
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}

#include "moc_OnViewValue.cpp"
