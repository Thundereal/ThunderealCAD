// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2025 Sayantan Deb <sayantandebin[at]gmail.com>           *
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

#include "Gizmo.h"

#include <cmath>
#include <utility>
#include <numbers>
#include <QApplication>
#include <QStyle>

#include <QCursor>
#include <QHelpEvent>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolTip>
#include <Inventor/SoPickedPoint.h>
#include <Inventor/SoRenderManager.h>
#include <Inventor/actions/SoHandleEventAction.h>
#include <Inventor/actions/SoSearchAction.h>
#include <Inventor/SoEventManager.h>
#include <Inventor/draggers/SoDragger.h>
#include <Inventor/events/SoKeyboardEvent.h>
#include <Inventor/events/SoLocation2Event.h>
#include <Inventor/events/SoMouseButtonEvent.h>
#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCube.h>
#include <Inventor/nodes/SoEventCallback.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoRotationXYZ.h>
#include <Inventor/nodes/SoScale.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSphere.h>
#include <Inventor/nodes/SoSwitch.h>
#include <Inventor/nodes/SoTransform.h>
#include <Inventor/nodes/SoTranslation.h>
#include <algorithm>

#include <App/Application.h>
#include <App/GeoFeature.h>
#include <Base/Converter.h>
#include <Base/Parameter.h>
#include <Base/Precision.h>
#include <Base/ServiceProvider.h>
#include <Base/Tools.h>
#include <Gui/Document.h>

#include <Gui/MiscParams.h>
#include <Gui/Inventor/Draggers/GizmoStyleParameters.h>
#include <Gui/Inventor/So3DAnnotation.h>
#include <Gui/Inventor/SoToggleSwitch.h>
#include <Gui/ViewProviderDragger.h>
#include <Gui/QuantitySpinBox.h>
#include <Gui/Utilities.h>
#include <Gui/ViewerContext.h>
#include <Gui/ViewParams.h>
#include <Gui/Widgets.h>

#include "SoLinearDragger.h"
#include "SoLinearDraggerGeometry.h"
#include "SoRotationDragger.h"
#include "SoRotationDraggerGeometry.h"

using namespace Gui;

namespace
{
enum class DefaultDragBehavior
{
    Coarse = 0,
    Fine = 1,
};


int getCoarseLinearSnapMultiplier()
{
    int multiplier = static_cast<int>(
        MiscParams::getCoarseLinearSnapMultiplier()
    );
    return std::max(1, multiplier);
}

int getCoarseRotationSnapMultiplier()
{
    int multiplier = static_cast<int>(
        MiscParams::getCoarseRotationSnapMultiplier()
    );
    return std::max(1, multiplier);
}

double snapToStep(double value, double step)
{
    if (step <= 0.0) {
        return value;
    }

    return std::round(value / step) * step;
}

double getRotationPeriod(double multFactor)
{
    const double factor = std::abs(multFactor);
    if (factor <= Base::Precision::Confusion()) {
        return 360.0;
    }

    return 2.0 * std::numbers::pi / factor;
}

double getClosestEquivalentAngle(double angle, double reference, double period)
{
    if (period <= Base::Precision::Confusion()) {
        return angle;
    }

    return angle + std::round((reference - angle) / period) * period;
}

double clampDragAngle(double value, double min, double max, double period)
{
    if (max - min >= period - Base::Precision::Confusion()) {
        return std::clamp(value, min, max);
    }

    return Base::clampAngle(value, min, max, Base::Precision::Confusion());
}
}  // namespace

void Gizmo::setDraggerPlacement(const Base::Vector3d& pos, const Base::Vector3d& dir)
{
    setDraggerPlacement(Base::convertTo<SbVec3f>(pos), Base::convertTo<SbVec3f>(dir));
}

double Gizmo::reverseIfPast(double value)
{
    if (!reverseCallback) {
        return value;
    }
    // Over the start one way or the other: the feature turned, or back
    const bool past = value < 0.0;
    if (past != reversedInDrag && reverseCallback()) {
        reversedInDrag = past;
    }
    return reversedInDrag ? -value : value;
}

bool Gizmo::endReverse(bool cancelled)
{
    const bool turned = reversedInDrag;
    if (turned && cancelled && reverseCallback) {
        reverseCallback();
    }
    reversedInDrag = false;
    // Left where it was while the drag had the feature turned
    if (turned && replaceCallback) {
        replaceCallback();
    }
    return turned;
}

bool Gizmo::isDelayedUpdateEnabled()
{
    return MiscParams::getDelayedGizmoUpdate();
}

double Gizmo::getMultFactor()
{
    return multFactor;
}

double Gizmo::getAddFactor()
{
    return addFactor;
}

bool Gizmo::getVisibility()
{
    return visible;
}

/** Whether the fine-snap modifier is held for the drag \a dragger is in: read
 * from the event it is handling -- a browser's drag is replayed into a mirror
 * viewer with the client's keys on its events, and the host's own keyboard
 * says nothing about them (docs/OnViewValueEditing.md sec 9.2 #2). On the
 * desktop the event carries the same keys as the keyboard.
 */
static bool fineModifierHeld(const SoDragger* dragger)
{
    const SoEvent* event = dragger ? dragger->getEvent() : nullptr;
    Qt::KeyboardModifiers modifiers;
    if (event) {
        if (event->wasShiftDown()) {
            modifiers |= Qt::ShiftModifier;
        }
        if (event->wasCtrlDown()) {
            modifiers |= Qt::ControlModifier;
        }
        if (event->wasAltDown()) {
            modifiers |= Qt::AltModifier;
        }
    }
    else {
        modifiers = QApplication::queryKeyboardModifiers();
    }
    return modifiers == GizmoContainer::getFineSnapModifier();
}

void Gizmo::otherDragEvent(void* data, SoDragger* dragger)
{
    // While the dragger holds the pointer, the events that are no motion and
    // no release of its button: Esc, or another mouse button, cancels
    auto self = static_cast<Gizmo*>(data);
    const SoEvent* event = dragger->getEvent();
    if (!self || !event || !self->dragging) {
        return;
    }
    bool cancel = false;
    if (event->isOfType(SoKeyboardEvent::getClassTypeId())) {
        const auto* key = static_cast<const SoKeyboardEvent*>(event);
        cancel = key->getKey() == SoKeyboardEvent::ESCAPE;
    }
    else if (event->isOfType(SoMouseButtonEvent::getClassTypeId())) {
        const auto* button = static_cast<const SoMouseButtonEvent*>(event);
        cancel = button->getButton() != SoMouseButtonEvent::BUTTON1;
    }
    if (!cancel) {
        return;
    }
    // Taken: Escape would leave the edit, another button open a menu
    if (SoHandleEventAction* action = dragger->getHandleEventAction()) {
        action->setHandled();
    }
    // On the press; its release is only taken
    if (static_cast<const SoButtonEvent*>(event)->getState() == SoButtonEvent::DOWN) {
        self->cancelDrag();
    }
}

SoNode* Gizmo::getExtraNode()
{
    return eventNode;
}

// Gui/Inventor/SoFCRenderCache.cpp: a highlight made distinct from what it
// lights, where the two are closer than View/SelectionColorDifference
bool makeDistinctColor(SbColor& res, const SbColor& color, const SbColor& other);

/// The colour a handle of \a own colour wears: the selection's while it is
/// selected, the pre-selection's while the pointer is over it, each made
/// distinct from \a own where the two are close, as a shape's highlight is
/// (SoFCRenderCache.cpp's makeDistinctColor; docs/HandsOnQueue.md entry 144)
static SbColor litColor(const SbColor& own, bool selected, bool preselected)
{
    if (!selected && !preselected) {
        return own;
    }
    // Under the pointer wins over selected, as on a shape; the selected
    // colour is back once the pointer leaves
    float transparency = 0.0F;
    SbColor lit;
    lit.setPackedValue(static_cast<uint32_t>(preselected ? ViewParams::getHighlightColor()
                                                         : ViewParams::getSelectionColor()),
                       transparency);
    makeDistinctColor(lit, lit, own);
    return lit;
}

namespace
{
/// The handle the pointer is over, whose hint the tip shows
Gizmo* tipHandle = nullptr;

/** The handle's tip is Gui::ToolTip's (entry 246, the reporter: "the
 * tooltip of each handle has a round rectangle border but show as rectangle
 * widget shape, so the pointy corner looks bad"): the label the measure
 * macro's preselection tip shows in, a child of the window whose shape is
 * its border, here beside the pointer ("unlike preselection macro tooltip,
 * those handle tooltip shall appeare near the mouser pointer").
 */
struct HandleTip
{
    QString text;
    QPointer<QWidget> widget;

    /// The label up now shows \a what
    bool shows(const QString& what) const
    {
        if (what.isEmpty() || what != text || !widget) {
            return false;
        }
        auto label = TipLabel::instance(widget, QPoint(), true);
        return label && label->isVisible();
    }
    void show(const QPoint& global, const QString& what, QWidget* where)
    {
        text = what;
        widget = where;
        ToolTip::showText(global, what, where, true, ToolTip::NoCorner);
    }
    void hide(const QString& what)
    {
        if (shows(what)) {
            ToolTip::hideText();
        }
        if (what == text) {
            text.clear();
        }
    }
    /// The label up now reads \a what instead, where it stands
    void change(const QString& old, const QString& what)
    {
        if (!shows(old)) {
            return;
        }
        text = what;
        if (auto label = TipLabel::instance(widget, QPoint(), true)) {
            label->set(what);
        }
    }
};

HandleTip& handleTip()
{
    static HandleTip tip;
    return tip;
}

/** Answers Qt's own tool tip request -- the QEvent::ToolTip it sends to the
 * widget under a pointer that rests, after the style's delay, and again at
 * once on each move while a tip shows -- with the hint of the handle the
 * pointer is over (docs/HandsOnQueue.md entry 141). A tip of its own on a
 * timer of its own was taken down by Qt's request coming at the same moment:
 * a request nobody answers hides the tip that shows.
 */
class HandleTipFilter: public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::ToolTip || !tipHandle || !watched->isWidgetType()) {
            return false;
        }
        // only a request in a 3D view, the one whose handle it is
        bool inView = false;
        for (auto w = static_cast<QWidget*>(watched); w; w = w->parentWidget()) {
            if (w->inherits("Gui::View3DInventorViewer")) {
                inView = true;
                break;
            }
        }
        const QString text = tipHandle->getMenuToolTip();
        if (!inView || text.isEmpty()) {
            return false;
        }
        // Taken, not answered: the handle's own rest shows its tip
        // (Gizmo::restTip()), which hides it while the pointer moves; Qt's
        // request comes 20 ms after a move while a tip was up a moment ago
        return true;
    }
};
}  // namespace

void Gizmo::setMenu(MenuCallback callback, const QString& toolTip)
{
    menuCallback = std::move(callback);
    menuToolTip = toolTip;
    static bool filtered = false;
    if (!filtered && !toolTip.isEmpty() && qApp) {
        filtered = true;
        qApp->installEventFilter(new HandleTipFilter(qApp));
    }
}

void Gizmo::restTip(const SbVec2s& pos)
{
    if (menuToolTip.isEmpty()) {
        return;
    }
    // a view with no widget has its hint from setHovered()
    ViewerContext* view = ViewerContext::current();
    QWidget* widget = view ? view->getGLWidget() : nullptr;
    if (!widget && view) {
        widget = view->getWidget();
    }
    if (!widget) {
        return;
    }
    tipHandle = this;
    handleTip().hide(menuToolTip);
    const int serial = ++tipSerial;
    std::weak_ptr<bool> life = alive;
    QPointer<QWidget> where(widget);
    const QPoint local = view->toQPoint(pos);
    const int wait = qApp ? qApp->style()->styleHint(QStyle::SH_ToolTip_WakeUpDelay) : 700;
    QTimer::singleShot(wait, [this, life, serial, where, local]() {
        auto still = life.lock();
        if (!still || !*still || serial != tipSerial || !where || tipHandle != this
            || !(hovered || isDragging())) {
            return;
        }
        handleTip().show(where->mapToGlobal(local), menuToolTip, where);
    });
}

void Gizmo::setMenuToolTip(const QString& toolTip)
{
    if (toolTip == menuToolTip) {
        return;
    }
    const QString old = menuToolTip;
    menuToolTip = toolTip;
    // the tip or the hint up now, of this handle, reads anew
    if (tipHandle == this) {
        handleTip().change(old, toolTip);
    }
    ViewerContext* view = hintView;
    if (view && !hintViewLife.expired() && view->hint() == old) {
        view->setHint(toolTip);
    }
}

void Gizmo::dropViewHint()
{
    ViewerContext* view = hintView;
    hintView = nullptr;
    // only its own hint: another handle's may have come up since
    if (view && !hintViewLife.expired() && view->hint() == menuToolTip) {
        view->setHint(QString());
    }
    hintViewLife.reset();
}

void Gizmo::dropTip()
{
    dropViewHint();
    if (tipHandle != this) {
        return;
    }
    tipHandle = nullptr;
    handleTip().hide(menuToolTip);
}

void Gizmo::buildEventNode(SoNode* own)
{
    releaseEventNode();
    ownNode = own;
    eventNode = new SoEventCallback;
    eventNode->ref();
    eventNode->addEventCallback(SoMouseButtonEvent::getClassTypeId(), eventNodeCallback, this);
    eventNode->addEventCallback(SoLocation2Event::getClassTypeId(), eventNodeCallback, this);
}

void Gizmo::releaseEventNode()
{
    dropTip();
    hovered = false;
    hoverHeld = false;
    ++hoverSerial;
    menuPressed = false;
    ownNode = nullptr;
    if (eventNode) {
        eventNode->unref();
        eventNode = nullptr;
    }
}

void Gizmo::setHovered(bool over)
{
    if (over == hovered) {
        return;
    }
    hovered = over;
    // Pressed on: told off at the press already, and not on again until the
    // pointer has left (pressedOn())
    const bool held = hoverHeld;
    if (!over) {
        hoverHeld = false;
    }
    if (hoverCallback && !held) {
        hoverCallback(over);
    }
    const int serial = ++hoverSerial;
    std::weak_ptr<bool> life = alive;
    if (over) {
        // "have a timer and before showing the tooltip check if still hitting
        // the arrow" (the reporter, 2026-10-15 15:29): Qt's tool tip request
        // is that timer, and it is answered while this handle is the one
        // (HandleTipFilter)
        if (menuToolTip.isEmpty()) {
            return;
        }
        ViewerContext* view = ViewerContext::current();
        if (!view || view->getWidget()) {
            tipHandle = this;
            return;
        }
        // A view with no widget, a browser's (docs/OnViewValueEditing.md
        // 9.2 #6): no Qt request comes, so the same wait is kept here, and
        // the view hands the hint to its client, which shows it at its
        // pointer
        std::weak_ptr<const void> viewLife = view->lifetime();
        const int wait = qApp ? qApp->style()->styleHint(QStyle::SH_ToolTip_WakeUpDelay) : 700;
        QTimer::singleShot(wait, [this, life, serial, view, viewLife]() {
            auto still = life.lock();
            if (!still || !*still || serial != hoverSerial || !hovered || viewLife.expired()) {
                return;
            }
            hintView = view;
            hintViewLife = viewLife;
            view->setHint(menuToolTip);
        });
        return;
    }
    // "any repeated hitting do not hide tooltip": gone only when the pointer
    // stays off the handle
    QTimer::singleShot(250, [this, life, serial]() {
        auto still = life.lock();
        if (!still || !*still || serial != hoverSerial || hovered) {
            return;
        }
        dropTip();
    });
}

void Gizmo::pressedOn()
{
    // The dragger takes the press before the event node: its start is where
    // the click begins
    if (!hovered || hoverHeld) {
        return;
    }
    hoverHeld = true;
    if (hoverCallback) {
        hoverCallback(false);
    }
}

void Gizmo::eventNodeCallback(void* data, SoEventCallback* cb)
{
    auto self = static_cast<Gizmo*>(data);
    if (!self->ownNode || !self->menuCallback) {
        return;
    }
    const SoPickedPoint* picked = cb->getPickedPoint();
    const bool over = picked && picked->getPath()->containsNode(self->ownNode);
    if (cb->getEvent()->isOfType(SoLocation2Event::getClassTypeId())) {
        // The hint, while the pointer is over the handle (entries 135, 141)
        self->setHovered(over);
        if (over) {
            self->restTip(cb->getEvent()->getPosition());
        }
        return;
    }
    const auto* event = static_cast<const SoMouseButtonEvent*>(cb->getEvent());
    if (SoMouseButtonEvent::isButtonPressEvent(event, SoMouseButtonEvent::BUTTON2)) {
        self->menuPressed = over;
        if (over) {
            // no hint over the menu
            handleTip().hide(self->menuToolTip);
            self->dropViewHint();
            cb->setHandled();
            // No grabEvents() here. A navigation style can take the release
            // before the scene sees it (one opens the view's menu on a right
            // release), and the grab was then never let go of; the handle
            // whose node held it went with a change made from the menu, and
            // the next mouse event traversed the dead grabber: a pure virtual
            // call in SoHandleEventAction::beginTraversal (docs/HandsOnQueue.md
            // entry 186, the reporter's session 2026-10-19 15:59). The release
            // reaches this callback through the scene as any event does.
        }
        return;
    }
    if (SoMouseButtonEvent::isButtonReleaseEvent(event, SoMouseButtonEvent::BUTTON2)) {
        const bool pressed = self->menuPressed;
        self->menuPressed = false;
        if (pressed) {
            cb->setHandled();
            if (over) {
                // After the event: the menu runs a loop of its own, and what
                // it does may take this handle away
                auto callback = self->menuCallback;
                QTimer::singleShot(0, [callback]() {
                    callback();
                });
            }
        }
    }
}

LinearGizmo::LinearGizmo(QuantitySpinBox* property)
{
    this->property = property;
}

LinearGizmo::~LinearGizmo()
{
    QuantitySpinBox::disconnect(quantityChangedConnection);
    QuantitySpinBox::disconnect(formulaDialogConnection);
    releaseEventNode();
}

SoInteractionKit* LinearGizmo::initDragger()
{
    draggerContainer = new SoLinearDraggerContainer;
    draggerContainer->color.setValue(1, 0, 0);
    dragger = draggerContainer->getDragger();

    dragger->addStartCallback(
        [](void* data, SoDragger*) { static_cast<LinearGizmo*>(data)->draggingStarted(); },
        this
    );
    dragger->addFinishCallback(
        [](void* data, SoDragger*) { static_cast<LinearGizmo*>(data)->draggingFinished(); },
        this
    );
    dragger->addMotionCallback(
        [](void* data, SoDragger*) { static_cast<LinearGizmo*>(data)->draggingContinued(); },
        this
    );
    dragger->addOtherEventCallback(&Gizmo::otherDragEvent, static_cast<Gizmo*>(this));

    dragger->labelVisible = false;

    if (draggerStyle == LinearDraggerStyle::Sphere) {
        dragger->setPart("arrow", new SoSphereGeometry);
    }
    else {
        dragger->instantiateBaseGeometry();

        auto arrow = SO_GET_PART(dragger, "arrow", SoArrowGeometry);
        arrow->cylinderHeight = 3.5;
        arrow->cylinderRadius = 0.2;
    }

    updateColorTheme();

    setProperty(property);

    buildEventNode(draggerContainer);

    return draggerContainer;
}

void LinearGizmo::uninitDragger()
{
    dragger = nullptr;
    draggerContainer = nullptr;
    releaseEventNode();
}

void LinearGizmo::setColor(const std::optional<Base::Color>& color)
{
    ownColor = color;
    if (dragger) {
        updateColorTheme();
    }
}

void LinearGizmo::updateColorTheme()
{
    auto* styleParameterManager = Base::provideService<Gui::StyleParameters::ParameterManager>();
    Base::Color baseColor = styleParameterManager->resolve(StyleParameters::LinearGizmoBaseColor);
    Base::Color activeColor = styleParameterManager->resolve(StyleParameters::LinearGizmoActiveColor);

    // a handle of its own colour (a customised edge's) keeps it
    if (ownColor) {
        baseColor = *ownColor;
    }
    // lit as a shape is: selected, or under the pointer (entry 144)
    dragger->color = litColor(baseColor.asValue<SbColor>(), highlighted, preselected);
    dragger->activeColor = activeColor.asValue<SbColor>();

    auto baseGeom = SO_GET_PART(dragger, "baseGeom", SoArrowBase);
    if (!baseGeom) {
        return;
    }
    Base::Color baseGeomColor = styleParameterManager->resolve(
        StyleParameters::DimensionVisualizerColor
    );
    baseGeom->color = baseGeomColor.asValue<SbColor>();
}

void LinearGizmo::setHighlighted(bool highlighted)
{
    if (this->highlighted == highlighted) {
        return;
    }
    this->highlighted = highlighted;
    if (dragger) {
        updateColorTheme();
    }
}

void LinearGizmo::setPreselected(bool preselected)
{
    if (this->preselected == preselected) {
        return;
    }
    this->preselected = preselected;
    if (dragger) {
        updateColorTheme();
    }
}

void LinearGizmo::setFocusBoxOnRelease(bool focus)
{
    focusBoxOnRelease = focus;
}

void LinearGizmo::setReleaseCallback(ReleaseCallback callback)
{
    releaseCallback = std::move(callback);
}

GizmoPlacement LinearGizmo::getDraggerPlacement()
{
    assert(draggerContainer && "Forgot to call GizmoContainer::initGizmos?");
    return {draggerContainer->translation.getValue(), draggerContainer->getPointerDirection()};
}

void LinearGizmo::setDraggerPlacement(const SbVec3f& pos, const SbVec3f& dir)
{
    assert(draggerContainer && "Forgot to call GizmoContainer::initGizmos?");
    draggerContainer->translation = pos;
    draggerContainer->setPointerDirection(dir);
}

std::optional<Gizmo::HandleBase> LinearGizmo::getHandleBase()
{
    if (!draggerContainer || !dragger) {
        return std::nullopt;
    }
    // The arrow stands as far along as its dragger is moved -- the value --
    // from the container's place
    SbVec3f along;
    draggerContainer->rotation.getValue().multVec(dragger->translation.getValue(), along);
    return HandleBase {draggerContainer->translation.getValue() + along,
                       draggerContainer->getPointerDirection(), false};
}

void LinearGizmo::reverseDir()
{
    auto dir = getDraggerContainer()->getPointerDirection();
    getDraggerContainer()->setPointerDirection(dir * -1);
}

double LinearGizmo::getDragLength()
{
    double dragLength = dragger->translationIncrementCount.getValue()
        * dragger->translationIncrement.getValue();

    return (dragLength - addFactor) / multFactor;
}

void LinearGizmo::setDragLength(double dragLength)
{
    dragLength = dragLength * multFactor + addFactor;
    dragger->translation = {0, static_cast<float>(dragLength), 0};
}

void LinearGizmo::setGeometryScale(float scale)
{
    geometryScale = scale;
    dragger->geometryScale = SbVec3f(scale, scale, scale);
    if (valueStep) {
        dragger->translationIncrement = multFactor * *valueStep;
        return;
    }
    // Scales the dragger increment in exponents of 10 based on the zoom level (scale)
    constexpr float base = 10.0F;
    dragger->translationIncrement = multFactor * std::pow(base, std::floor(std::log10(scale)));
}

void LinearGizmo::setValueStep(std::optional<double> step)
{
    valueStep = step;
    if (dragger) {
        setGeometryScale(geometryScale);
    }
}

SoLinearDraggerContainer* LinearGizmo::getDraggerContainer()
{
    assert(draggerContainer && "Forgot to call GizmoContainer::initGizmos?");
    return draggerContainer;
}

void LinearGizmo::setProperty(QuantitySpinBox* property)
{
    QuantitySpinBox::disconnect(quantityChangedConnection);
    QuantitySpinBox::disconnect(formulaDialogConnection);

    this->property = property;
    quantityChangedConnection = QuantitySpinBox::connect(
        property,
        qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        [this](double value) { setDragLength(value); }
    );
    formulaDialogConnection
        = QuantitySpinBox::connect(property, &Gui::QuantitySpinBox::showFormulaDialog, [this](bool) {
              // This will set the visibility of the actual geometry to true or false
              // based on if an expression is bound and the externally set visibility
              setVisibility(visible);
          });

    // Updates the gizmo state based on the new property
    setDragLength(property->rawValue());
    setVisibility(visible);
}

void LinearGizmo::setMultFactor(const double val)
{
    multFactor = val;
    setDragLength(property->value().getValue());
    // the increment is a length: the value's step times the factor
    if (dragger && valueStep) {
        setGeometryScale(geometryScale);
    }
}

void LinearGizmo::setAddFactor(const double val)
{
    addFactor = val;
    setDragLength(property->value().getValue());
}

void LinearGizmo::setDraggerStyle(LinearDraggerStyle style)
{
    draggerStyle = style;
}

void LinearGizmo::setClickCallback(ClickCallback callback)
{
    clickCallback = std::move(callback);
}

void LinearGizmo::setVisibility(bool visible)
{
    this->visible = visible;
    getDraggerContainer()->visible = visible && !hiddenInView && !property->hasExpression();
}

void LinearGizmo::setHiddenInView(bool hidden)
{
    hiddenInView = hidden;
    // before initDragger() there is nothing to switch: it applies it
    if (draggerContainer && property) {
        setVisibility(visible);
    }
}

void LinearGizmo::draggingStarted()
{
    initialValue = property->value().getValue();
    hasDragged = false;
    dragging = true;
    dragCancelled = false;
    reversedInDrag = false;
    pressedOn();
    notifyDrag(DragEvent::Started);
    dragger->translationIncrementCount.setValue(0);

    if (isDelayedUpdateEnabled()) {
        property->blockSignals(true);
    }
}

void LinearGizmo::draggingFinished()
{
    if (dragCancelled) {
        dragging = false;
        // put back at the cancel already; nothing to commit or activate
        dragCancelled = false;
        if (isDelayedUpdateEnabled()) {
            property->blockSignals(false);
        }
        return;
    }
    // Where the turned feature puts the handle, the arrow as long as the
    // value the other way
    if (endReverse(false)) {
        setDragLength(property->value().getValue());
    }
    dragging = false;
    if (isDelayedUpdateEnabled()) {
        property->blockSignals(false);
        property->valueChanged(property->value().getValue());
    }

    if (!hasDragged && clickCallback) {
        clickCallback();
    }

    if (focusBoxOnRelease) {
        property->setFocus();
        property->selectAll();
    }
    // Last: it may make the handle the active one, which takes the focus
    if (releaseCallback) {
        releaseCallback(hasDragged);
    }
    notifyDrag(DragEvent::Finished);
}

void LinearGizmo::cancelDrag()
{
    if (!dragging || dragCancelled) {
        return;
    }
    dragCancelled = true;
    // The box back first, then the owner told: it rolls the drag's step back
    // and puts every handle of the value where the box says -- the others a
    // default value drives too (entry 145), which the blocked box does not
    // tell
    {
        const QSignalBlocker blocker(property);
        property->setValue(initialValue);
    }
    // a feature the drag turned, turned back
    endReverse(true);
    setDragLength(initialValue);
    // No longer shown dragged (the user, 2026-10-20: "the dragging is still
    // active, no aborting"): the dragger keeps the pointer until its button
    // is up, its moves undone as they come (draggingContinued()). Letting
    // the pointer go here would hand the rest of a held left button to the
    // navigation style, which never saw it pressed.
    dragger->active = false;
    notifyDrag(DragEvent::Cancelled);
}

void LinearGizmo::draggingContinued()
{
    if (dragCancelled) {
        // cancelled: the arrow stays where the drag began
        setDragLength(initialValue);
        return;
    }
    hasDragged = true;
    double value = initialValue + getDragLength();

    bool fineModifierPressed = fineModifierHeld(dragger);
    bool coarseByDefault = GizmoContainer::isCoarseByDefault();
    bool useCoarseSnap = coarseByDefault != fineModifierPressed;

    if (GizmoContainer::isCoarseSnapEnabled() && useCoarseSnap) {
        double baseStep = std::abs(dragger->translationIncrement.getValue() / multFactor);
        value = snapToStep(value, baseStep * getCoarseLinearSnapMultiplier());
    }

    // Back past the start: the feature turns, and the value grows again
    value = reverseIfPast(value);
    value = std::clamp(value, property->minimum(), property->maximum());

    property->setValue(value);
    // the arrow where the pointer is, behind its start once turned
    setDragLength(reversedInDrag ? -value : value);
    notifyDrag(DragEvent::Moved);
    if (const SoEvent* event = dragger->getEvent()) {
        restTip(event->getPosition());
    }
}


RotationGizmo::RotationGizmo(QuantitySpinBox* property)
{
    this->property = property;
}

RotationGizmo::~RotationGizmo()
{
    // Lets go of its spin box, as a linear one does: a box may outlive a
    // gizmo taken out early
    QuantitySpinBox::disconnect(quantityChangedConnection);
    QuantitySpinBox::disconnect(formulaDialogConnection);
    releaseEventNode();
    translationSensor.detach();
    translationSensor.setData(nullptr);
    translationSensor.setFunction(nullptr);
}

SoInteractionKit* RotationGizmo::initDragger()
{
    multFactor = std::numbers::pi_v<float> / 180.0;
    draggerContainer = new SoRotationDraggerContainer;

    draggerContainer->color.setValue(1, 0, 0);
    dragger = draggerContainer->getDragger();
    dragger->rotationIncrement = std::numbers::pi / 180.0;

    auto rotator = new SoRotatorGeometry;
    rotator->arcAngle = std::numbers::pi_v<float> / 6.0f;
    rotator->arcRadius = 16.0f;
    dragger->setPart("rotator", rotator);

    dragger->addStartCallback(
        [](void* data, SoDragger*) { static_cast<RotationGizmo*>(data)->draggingStarted(); },
        this
    );
    dragger->addFinishCallback(
        [](void* data, SoDragger*) { static_cast<RotationGizmo*>(data)->draggingFinished(); },
        this
    );
    dragger->addMotionCallback(
        [](void* data, SoDragger*) { static_cast<RotationGizmo*>(data)->draggingContinued(); },
        this
    );
    dragger->addOtherEventCallback(&Gizmo::otherDragEvent, static_cast<Gizmo*>(this));

    setProperty(property);

    updateColorTheme();

    buildEventNode(draggerContainer);

    return draggerContainer;
}

void RotationGizmo::uninitDragger()
{
    dragger = nullptr;
    draggerContainer = nullptr;
    releaseEventNode();

    translationSensor.detach();
    translationSensor.setData(nullptr);
    translationSensor.setFunction(nullptr);
}

void RotationGizmo::updateColorTheme()
{
    auto* styleParameterManager = Base::provideService<Gui::StyleParameters::ParameterManager>();
    Base::Color baseColor = styleParameterManager->resolve(StyleParameters::RotationGizmoBaseColor);
    Base::Color activeColor = styleParameterManager->resolve(StyleParameters::RotationGizmoActiveColor);

    // a handle of its own colour (a customised edge's) keeps it; lit as a
    // shape is, selected or under the pointer (entry 144)
    if (ownColor) {
        baseColor = *ownColor;
    }
    dragger->color = litColor(baseColor.asValue<SbColor>(), highlighted, preselected);
    dragger->activeColor = activeColor.asValue<SbColor>();
}

void RotationGizmo::setPreselected(bool preselected)
{
    if (this->preselected == preselected) {
        return;
    }
    this->preselected = preselected;
    if (dragger) {
        updateColorTheme();
    }
}

void RotationGizmo::setHighlighted(bool highlighted)
{
    if (this->highlighted == highlighted) {
        return;
    }
    this->highlighted = highlighted;
    if (dragger) {
        updateColorTheme();
    }
}

void RotationGizmo::setColor(const std::optional<Base::Color>& color)
{
    ownColor = color;
    if (dragger) {
        updateColorTheme();
    }
}

void RotationGizmo::setReleaseCallback(ReleaseCallback callback)
{
    releaseCallback = std::move(callback);
}

void RotationGizmo::setFocusBoxOnRelease(bool focus)
{
    focusBoxOnRelease = focus;
}

GizmoPlacement RotationGizmo::getDraggerPlacement()
{
    assert(draggerContainer && "Forgot to call GizmoContainer::initGizmos?");
    return {draggerContainer->translation.getValue(), draggerContainer->getPointerDirection()};
}

void RotationGizmo::setDraggerPlacement(const SbVec3f& pos, const SbVec3f& dir)
{
    assert(draggerContainer && "Forgot to call GizmoContainer::initGizmos?");
    draggerContainer->translation = pos;
    draggerContainer->setPointerDirection(dir);
}

std::optional<Gizmo::HandleBase> RotationGizmo::getHandleBase()
{
    if (!draggerContainer || !dragger) {
        return std::nullopt;
    }
    auto rotator = SO_GET_PART(dragger, "rotator", SoRotatorGeometryKit);
    if (!rotator) {
        return std::nullopt;
    }
    // The knob, turned by the angle; its place is in the geometry's scaled
    // space, sized to the screen
    SbVec3f knob = rotator->pivotPosition.getValue();
    const SbVec3f scale = dragger->geometryScale.getValue();
    knob = SbVec3f(knob[0] * scale[0], knob[1] * scale[1], knob[2] * scale[2]);
    dragger->rotation.getValue().multVec(knob, knob);
    draggerContainer->rotation.getValue().multVec(knob, knob);
    SbVec3f out = knob;
    if (out.normalize() == 0.0F) {
        out = draggerContainer->getPointerDirection();
    }
    return HandleBase {draggerContainer->translation.getValue() + knob, out, true};
}

void RotationGizmo::reverseDir()
{
    auto dir = getDraggerContainer()->getPointerDirection();
    getDraggerContainer()->setPointerDirection(dir * -1);
}

void RotationGizmo::placeOverLinearGizmo(LinearGizmo* gizmo)
{
    linearGizmo = gizmo;

    GizmoPlacement placement = gizmo->getDraggerPlacement();

    draggerContainer->translation = Base::convertTo<SbVec3f>(placement.pos);
    draggerContainer->setPointerDirection(placement.dir);

    translationSensor.setData(this);
    translationSensor.setFunction(translationSensorCB);
    translationSensor.setPriority(0);
    SoSFVec3f& translation = gizmo->getDraggerContainer()->getDragger()->translation;
    translationSensor.attach(&translation);
    translation.touch();

    automaticOrientation = true;
}

void RotationGizmo::translationSensorCB(void* data, SoSensor* sensor)
{
    auto sudoThis = static_cast<RotationGizmo*>(data);
    auto translationSensor = static_cast<SoFieldSensor*>(sensor);

    GizmoPlacement placement = sudoThis->linearGizmo->getDraggerPlacement();

    SbVec3f translation = static_cast<SoSFVec3f*>(translationSensor->getAttachedField())->getValue();
    float yComp = translation.getValue()[1];
    SbVec3f dir = placement.dir;
    dir.normalize();
    sudoThis->draggerContainer->translation = placement.pos + dir * (yComp + sudoThis->sepDistance);
}

void RotationGizmo::placeBelowLinearGizmo(LinearGizmo* gizmo)
{
    linearGizmo = gizmo;

    GizmoPlacement placement = gizmo->getDraggerPlacement();

    draggerContainer->translation = Base::convertTo<SbVec3f>(placement.pos);
    draggerContainer->setPointerDirection(-placement.dir);

    translationSensor.setData(this);
    translationSensor.setFunction(translationSensorCB);
    translationSensor.setPriority(0);
    SoSFVec3f& translation = gizmo->getDraggerContainer()->getDragger()->translation;
    translationSensor.attach(&translation);
    translation.touch();
}

double RotationGizmo::getRotAngle()
{
    double rotAngle = dragger->rotationIncrementCount.getValue()
        * dragger->rotationIncrement.getValue();

    return (rotAngle - addFactor) / multFactor;
}

void RotationGizmo::setRotAngle(double angle)
{
    angle = multFactor * angle + addFactor;
    dragger->rotation = SbRotation({0, 0, 1.0f}, static_cast<float>(angle));
}

void RotationGizmo::setGeometryScale(float scale)
{
    dragger->geometryScale = SbVec3f(scale, scale, scale);
}

SoRotationDraggerContainer* RotationGizmo::getDraggerContainer()
{
    assert(draggerContainer && "Forgot to call GizmoContainer::initGizmos?");
    return draggerContainer;
}

void RotationGizmo::draggingStarted()
{
    initialValue = property->value().getValue();
    lastDragOffset = 0.0;
    hasDragged = false;
    dragging = true;
    dragCancelled = false;
    reversedInDrag = false;
    pressedOn();
    notifyDrag(DragEvent::Started);
    dragger->rotationIncrementCount.setValue(0);

    if (isDelayedUpdateEnabled()) {
        property->blockSignals(true);
    }
}

void RotationGizmo::draggingFinished()
{
    if (dragCancelled) {
        dragging = false;
        dragCancelled = false;
        if (isDelayedUpdateEnabled()) {
            property->blockSignals(false);
        }
        return;
    }
    // Where the turned feature puts the handle, the arc as wide as the value
    // the other way
    if (endReverse(false)) {
        setRotAngle(property->value().getValue());
    }
    dragging = false;
    if (isDelayedUpdateEnabled()) {
        property->blockSignals(false);
        property->valueChanged(property->value().getValue());
    }

    if (!hasDragged && clickCallback) {
        clickCallback();
    }

    if (focusBoxOnRelease) {
        property->setFocus();
        property->selectAll();
    }
    // Last: it may make the handle the active one, which takes the focus
    if (releaseCallback) {
        releaseCallback(hasDragged);
    }
    notifyDrag(DragEvent::Finished);
}

void RotationGizmo::cancelDrag()
{
    if (!dragging || dragCancelled) {
        return;
    }
    dragCancelled = true;
    // the box back first, then the owner told (as a linear handle's)
    {
        const QSignalBlocker blocker(property);
        property->setValue(initialValue);
    }
    // a feature the drag turned, turned back
    endReverse(true);
    setRotAngle(initialValue);
    // no longer shown dragged, its moves undone until the button is up (as
    // a linear handle's)
    dragger->active = false;
    notifyDrag(DragEvent::Cancelled);
}

void RotationGizmo::draggingContinued()
{
    if (dragCancelled) {
        // cancelled: the arc stays where the drag began
        setRotAngle(initialValue);
        return;
    }
    hasDragged = true;
    const double period = getRotationPeriod(multFactor);
    double dragOffset = getClosestEquivalentAngle(getRotAngle(), lastDragOffset, period);
    lastDragOffset = dragOffset;
    double value = initialValue + dragOffset;

    bool fineModifierPressed = fineModifierHeld(dragger);
    bool coarseByDefault = GizmoContainer::isCoarseByDefault();
    bool useCoarseSnap = coarseByDefault != fineModifierPressed;

    if (GizmoContainer::isCoarseSnapEnabled() && useCoarseSnap) {
        value = snapToStep(value, getCoarseRotationSnapMultiplier());
    }

    // Back past the start: the feature turns, and the angle grows again
    value = reverseIfPast(value);
    value = clampDragAngle(value, property->minimum(), property->maximum(), period);

    property->setValue(value);
    // the arc where the pointer is, behind its start once turned
    setRotAngle(reversedInDrag ? -value : value);
    notifyDrag(DragEvent::Moved);
    if (const SoEvent* event = dragger->getEvent()) {
        restTip(event->getPosition());
    }
}

void RotationGizmo::orientAlongCamera(SoCamera* camera)
{
    if (!automaticOrientation) {
        return;
    }

    SbVec3f cameraDir {0, 0, 1};
    camera->orientation.getValue().multVec(cameraDir, cameraDir);
    SbVec3f pointerDir = getDraggerContainer()->getPointerDirection();

    pointerDir.normalize();
    auto proj = cameraDir - cameraDir.dot(pointerDir) * pointerDir;
    if (proj.equals(SbVec3f {0, 0, 0}, 0.001)) {
        return;
    }

    assert(draggerContainer && "Forgot to call GizmoContainer::initGizmos?");
    draggerContainer->setArcNormalDirection(proj);
}

void RotationGizmo::setProperty(QuantitySpinBox* property)
{
    QuantitySpinBox::disconnect(quantityChangedConnection);
    QuantitySpinBox::disconnect(formulaDialogConnection);

    this->property = property;
    quantityChangedConnection = QuantitySpinBox::connect(
        property,
        qOverload<double>(&Gui::QuantitySpinBox::valueChanged),
        [this](double value) { setRotAngle(value); }
    );
    formulaDialogConnection
        = QuantitySpinBox::connect(property, &Gui::QuantitySpinBox::showFormulaDialog, [this](bool) {
              // This will set the visibility of the actual geometry to true or false
              // based on if an expression is bound and the externally set visibility
              setVisibility(visible);
          });

    // Updates the gizmo state based on the new property
    setRotAngle(property->rawValue());
    setVisibility(visible);
}

void RotationGizmo::setMultFactor(const double val)
{
    multFactor = val;
    setRotAngle(property->value().getValue());
}

void RotationGizmo::setAddFactor(const double val)
{
    addFactor = val;
    setRotAngle(property->value().getValue());
}

void RotationGizmo::setClickCallback(ClickCallback callback)
{
    clickCallback = std::move(callback);
}

void RotationGizmo::setVisibility(bool visible)
{
    this->visible = visible;
    getDraggerContainer()->visible = visible && !hiddenInView && !property->hasExpression();
}

void RotationGizmo::setHiddenInView(bool hidden)
{
    hiddenInView = hidden;
    // before initDragger() there is nothing to switch: it applies it
    if (draggerContainer && property) {
        setVisibility(visible);
    }
}

DirectedRotationGizmo::DirectedRotationGizmo(QuantitySpinBox* property)
    : RotationGizmo(property)
{}

SoInteractionKit* DirectedRotationGizmo::initDragger()
{
    SoInteractionKit* ret = inherited::initDragger();

    auto dragger = getDraggerContainer()->getDragger();
    auto rotator = new SoRotatorGeometry2;
    rotator->arcAngle = std::numbers::pi_v<float> / 6.0f;
    rotator->arcRadius = 16.0f;
    rotator->rightArrowVisible = false;
    dragger->setPart("rotator", rotator);

    updateColorTheme();

    return ret;
}

void DirectedRotationGizmo::flipArrow()
{
    auto dragger = getDraggerContainer()->getDragger();
    auto rotator = SO_GET_PART(dragger, "rotator", SoRotatorGeometry2);

    rotator->toggleArrowVisibility();
}


RadialGizmo::RadialGizmo(QuantitySpinBox* property)
    : RotationGizmo(property)
{}

SoInteractionKit* RadialGizmo::initDragger()
{
    SoInteractionKit* ret = inherited::initDragger();

    auto dragger = getDraggerContainer()->getDragger();
    auto rotator = new SoRotatorArrow;
    rotator->geometryScale.connectFrom(&dragger->geometryScale);
    dragger->setPart("rotator", rotator);

    dragger->instantiateBaseGeometry();

    updateColorTheme();

    return ret;
}

void RadialGizmo::setRadius(float radius)
{
    auto dragger = getDraggerContainer()->getDragger();
    auto rotator = SO_GET_PART(dragger, "rotator", SoRotatorArrow);
    auto baseGeom = SO_GET_PART(dragger, "baseGeom", SoRotatorBase);

    rotator->radius = baseGeom->arcRadius = radius;
}

void RadialGizmo::flipArrow()
{
    auto dragger = getDraggerContainer()->getDragger();
    auto rotator = SO_GET_PART(dragger, "rotator", SoRotatorArrow);

    rotator->flipArrow();
}

void RadialGizmo::updateColorTheme()
{
    auto dragger = getDraggerContainer()->getDragger();

    auto* styleParameterManager = Base::provideService<Gui::StyleParameters::ParameterManager>();
    Base::Color baseColor = styleParameterManager->resolve(StyleParameters::RotationGizmoBaseColor);
    Base::Color activeColor = styleParameterManager->resolve(StyleParameters::RotationGizmoActiveColor);

    dragger->color = baseColor.asValue<SbColor>();
    dragger->activeColor = activeColor.asValue<SbColor>();

    auto baseGeom = SO_GET_PART(dragger, "baseGeom", SoRotatorBase);
    Base::Color baseGeomColor = styleParameterManager->resolve(
        StyleParameters::DimensionVisualizerColor
    );
    baseGeom->color = baseGeomColor.asValue<SbColor>();
}

SO_KIT_SOURCE(GizmoContainer)

void GizmoContainer::initClass()
{
    SO_KIT_INIT_CLASS(GizmoContainer, SoBaseKit, "BaseKit");
}

GizmoContainer::GizmoContainer()
    : viewProvider(nullptr)
{
    SO_KIT_CONSTRUCTOR(GizmoContainer);

#if defined(Q_OS_MACOS) || defined(Q_OS_FREEBSD) || defined(Q_OS_OPENBSD)
    this->ref();
#endif

    FC_ADD_CATALOG_ENTRY(annotation, So3DAnnotation, this);
    FC_ADD_CATALOG_ENTRY(pickStyle, SoPickStyle, annotation);
    FC_ADD_CATALOG_ENTRY(toggleSwitch, SoToggleSwitch, annotation);
    FC_ADD_CATALOG_ENTRY(geometry, SoSeparator, toggleSwitch);
    // beside the switch, not under it: shown when the gizmos are not
    FC_ADD_CATALOG_ENTRY(marks, SoSeparator, annotation);

    SO_KIT_INIT_INSTANCE();

    SO_KIT_ADD_FIELD(visible, (1));

    auto pickStyle = SO_GET_ANY_PART(this, "pickStyle", SoPickStyle);
    pickStyle->style = SoPickStyle::SHAPE_ON_TOP;

    // Drawn over a dress-up's on-top preview (its priority -2) and under the
    // on-view labels (1): the handles were drawn before the preview, which
    // covered them (docs/HandsOnQueue.md entry 185, the reporter 2026-10-19
    // 15:49). Picking is not ordered by this.
    SO_GET_ANY_PART(this, "annotation", So3DAnnotation)->priority = -1;

    auto toggleSwitch = SO_GET_ANY_PART(this, "toggleSwitch", SoToggleSwitch);
    toggleSwitch->on.connectFrom(&visible);

    setPart("geometry", new SoSeparator);

    // the marks are not picked: what is under them is (entry 180, "it can be
    // unpicable, since the face is already pickable")
    auto marks = new SoSeparator;
    auto marksPick = new SoPickStyle;
    marksPick->style = SoPickStyle::UNPICKABLE;
    marks->addChild(marksPick);
    setPart("marks", marks);

    cameraSensor.setFunction(&GizmoContainer::cameraChangeCallback);
    cameraSensor.setData(this);

    cameraPositionSensor.setData(this);
    cameraPositionSensor.setFunction(cameraPositionChangeCallback);
}

GizmoContainer::~GizmoContainer()
{
    cameraSensor.setData(nullptr);
    cameraSensor.detach();

    cameraPositionSensor.setData(nullptr);
    cameraPositionSensor.detach();

    // a grab held by any of the gizmos goes before they do (entry 186)
    releaseGrabUnder(SO_GET_ANY_PART(this, "annotation", So3DAnnotation));

    uninitGizmos();

    if (!viewProvider.expired()) {
        viewProvider->setGizmoContainer(nullptr);
    }
}

void GizmoContainer::initGizmos()
{
    auto geometry = SO_GET_ANY_PART(this, "geometry", SoSeparator);
    gizmoNodes.clear();
    for (auto gizmo : gizmos) {
        SoNode* dragger = gizmo->initDragger();
        geometry->addChild(dragger);
        SoNode* extra = gizmo->getExtraNode();
        if (extra) {
            geometry->addChild(extra);
        }
        gizmoNodes.emplace_back(dragger, extra);
    }
}

void GizmoContainer::uninitGizmos()
{
    for (auto gizmo : gizmos) {
        gizmo->uninitDragger();
        delete gizmo;
    }
    gizmos.clear();
    gizmoNodes.clear();
}

void GizmoContainer::appendGizmo(Gizmo* gizmo)
{
    if (!gizmo || std::ranges::find(gizmos, gizmo) != gizmos.end()) {
        return;
    }
    gizmos.push_back(gizmo);
    auto geometry = SO_GET_ANY_PART(this, "geometry", SoSeparator);
    SoNode* dragger = gizmo->initDragger();
    geometry->addChild(dragger);
    SoNode* extra = gizmo->getExtraNode();
    if (extra) {
        geometry->addChild(extra);
    }
    gizmoNodes.emplace_back(dragger, extra);
    // its size and turn, as the others have them
    calculateScaleAndOrientation();
}

void GizmoContainer::releaseGrabUnder(SoNode* node)
{
    if (!node || !viewer || viewerLife.expired()) {
        return;
    }
    SoEventManager* manager = viewer->getSoEventManager();
    SoHandleEventAction* action = manager ? manager->getHandleEventAction() : nullptr;
    SoNode* grabber = action ? action->getGrabber() : nullptr;
    if (!grabber) {
        return;
    }
    bool under = grabber == node;
    if (!under) {
        SoSearchAction search;
        search.setNode(grabber);
        search.setInterest(SoSearchAction::FIRST);
        search.setSearchingAll(TRUE);
        // into the draggers' kits, and back as it was: the switch is global
        const SbBool wasSearching = SoBaseKit::isSearchingChildren();
        SoBaseKit::setSearchingChildren(TRUE);
        search.apply(node);
        SoBaseKit::setSearchingChildren(wasSearching);
        under = search.getPath() != nullptr;
    }
    if (under) {
        // while the grabber still lives: releaseGrabber() calls into it
        action->releaseGrabber();
    }
}

void GizmoContainer::removeGizmo(Gizmo* gizmo)
{
    auto it = std::ranges::find(gizmos, gizmo);
    if (it == gizmos.end()) {
        return;
    }
    const auto index = std::distance(gizmos.begin(), it);
    auto geometry = SO_GET_ANY_PART(this, "geometry", SoSeparator);
    if (index < static_cast<decltype(index)>(gizmoNodes.size())) {
        auto [dragger, extra] = gizmoNodes[index];
        for (SoNode* node : {dragger, extra}) {
            releaseGrabUnder(node);
        }
        for (SoNode* node : {dragger, extra}) {
            if (!node) {
                continue;
            }
            int child = geometry->findChild(node);
            if (child >= 0) {
                geometry->removeChild(child);
            }
        }
        gizmoNodes.erase(gizmoNodes.begin() + index);
    }
    gizmos.erase(it);
    gizmo->uninitDragger();
    delete gizmo;
}

void GizmoContainer::addGizmos(std::initializer_list<Gui::Gizmo*> gizmos)
{
    assert(this->gizmos.size() == 0 && "Already called GizmoContainer::addGizmos?");

    for (auto gizmo : gizmos) {
        addGizmo(gizmo);
    }
    initGizmos();
}

void GizmoContainer::addGizmo(Gizmo* gizmo)
{
    assert(std::ranges::find(gizmos, gizmo) == gizmos.end() && "this gizmo is already added!");
    gizmos.push_back(gizmo);
}

void GizmoContainer::attachViewer(Gui::ViewerContext* viewer, Base::Placement& origin)
{
    if (!viewer) {
        return;
    }

    setUpAutoScale(viewer->getSoRenderManager()->getCamera());

    auto mat = origin.toMatrix();
    this->origin = mat;
    this->viewer = viewer;
    viewerLife = viewer->lifetime();

    // `origin` is the gizmos' whole frame (ViewProviderDragger: the editing
    // transform less the object's own placement), and it takes the editing
    // root's transform: the gizmos are all the edit puts there.
    So3DAnnotation* annotation = SO_GET_ANY_PART(this, "annotation", So3DAnnotation);
    viewer->setupEditingRoot(annotation, &mat);

    connEditingTransform.disconnect();
    if (auto vp = viewProvider.get()) {
        if (auto doc = vp->getDocument()) {
            connEditingTransform = doc->signalEditingTransformChanged.connect(
                [this](const Gui::Document&) {
                    onEditingTransformChanged();
                });
        }
    }
}

void GizmoContainer::onEditingTransformChanged()
{
    // A PartDesign box attached to the YZ plane in its own panel: its new
    // placement was put on the editing root with the rest of the editing
    // transform, and its handles, placed by that placement in the frame,
    // had it twice -- along the wrong edges, the length handle on the width
    // (docs/HandsOnQueue.md entry 236, the reporter: "the handle then
    // controls the wrong dimenion"). Set back to the frame they are placed
    // in.
    if (!viewer || viewerLife.expired()) {
        return;
    }
    origin = getWorldMatrix();
    viewer->setEditingTransform(origin);
}

Base::Matrix4D GizmoContainer::getWorldMatrix() const
{
    // The frame attachViewer() was given -- the editing transform less the
    // object's placement -- as it is now: it used to be put on top of the
    // editing transform once more, and a value's label stood where its
    // handle would with the object's placement applied twice
    auto vp = viewProvider.get();
    auto doc = vp ? vp->getDocument() : nullptr;
    if (!doc) {
        return origin;
    }
    // the edit's own path, not the document's copy of its transform, which
    // a panel re-placing its handles on the same change may read before it
    // is brought up to date
    Base::Matrix4D mat;
    ViewProviderDocumentObject* parentVp = nullptr;
    std::string subname;
    if (doc->getInEdit(&parentVp, &subname) != vp || !parentVp || !parentVp->getObject()
        || parentVp->getObject()->getSubObject(subname.c_str(), nullptr, &mat)
            != vp->getObject()) {
        mat = doc->getEditingTransform();
    }
    if (auto feat = dynamic_cast<App::GeoFeature*>(vp->getObject())) {
        Base::Matrix4D inverse = feat->Placement.getValue().toMatrix();
        inverse.inverse();
        mat *= inverse;
    }
    return mat;
}

void GizmoContainer::setUpAutoScale(SoCamera* cameraIn)
{
    if (cameraIn->getTypeId() == SoOrthographicCamera::getClassTypeId()) {
        auto localCamera = dynamic_cast<SoOrthographicCamera*>(cameraIn);
        cameraSensor.attach(&localCamera->height);
        cameraPositionSensor.attach(&localCamera->orientation);
        calculateScaleAndOrientation();
    }
    else if (cameraIn->getTypeId() == SoPerspectiveCamera::getClassTypeId()) {
        auto localCamera = dynamic_cast<SoPerspectiveCamera*>(cameraIn);
        cameraSensor.attach(&localCamera->position);
        cameraPositionSensor.attach(&localCamera->orientation);
        calculateScaleAndOrientation();
    }
}

void GizmoContainer::calculateScaleAndOrientation()
{
    if (cameraSensor.getAttachedField()) {
        cameraChangeCallback(this, nullptr);
        cameraPositionChangeCallback(this, nullptr);
    }
}

void GizmoContainer::cameraChangeCallback(void* data, SoSensor*)
{
    auto sudoThis = static_cast<GizmoContainer*>(data);

    SoField* field = sudoThis->cameraSensor.getAttachedField();
    if (!field) {
        return;
    }

    auto camera = static_cast<SoCamera*>(field->getContainer());

    SbViewVolume viewVolume = camera->getViewVolume();
    for (auto gizmo : sudoThis->gizmos) {
        float localScale = viewVolume.getWorldToScreenScale(gizmo->getDraggerPlacement().pos, 0.015);
        gizmo->setGeometryScale(localScale);
    }
    // the crosses at the gizmos' size
    for (auto& [pos, transform] : sudoThis->crosses) {
        const float s = viewVolume.getWorldToScreenScale(pos, 0.015);
        transform->scaleFactor.setValue(s, s, s);
    }
}

void GizmoContainer::cameraPositionChangeCallback(void* data, SoSensor*)
{
    auto sudoThis = static_cast<GizmoContainer*>(data);

    SoField* field = sudoThis->cameraSensor.getAttachedField();
    if (field) {
        auto camera = static_cast<SoCamera*>(field->getContainer());

        for (auto gizmo : sudoThis->gizmos) {
            gizmo->orientAlongCamera(camera);
        }
        // a cross lies in the screen's plane
        for (auto& cross : sudoThis->crosses) {
            cross.second->rotation = camera->orientation.getValue();
        }
    }
}

SoSeparator* GizmoContainer::getMarks()
{
    return SO_GET_ANY_PART(this, "marks", SoSeparator);
}

void GizmoContainer::clearMarks()
{
    auto marks = getMarks();
    // the pick style stays
    while (marks->getNumChildren() > 1) {
        marks->removeChild(marks->getNumChildren() - 1);
    }
    crosses.clear();
}

void GizmoContainer::addCross(const SbVec3f& pos, const SbColor& color)
{
    // A linear handle's shaft is 0.4 across (SoArrowGeometry, cylinderRadius
    // 0.2), in the units its geometry scale makes a size on the screen; the
    // bars are that thick, and about as long as the bitmap cross they
    // replace was wide
    constexpr float thickness = 0.4F;
    constexpr float length = 2.2F;
    auto cross = new SoSeparator;
    auto transform = new SoTransform;
    transform->translation = pos;
    cross->addChild(transform);
    // lit as the view draws, as the handles it stands among (entry 196)
    auto base = new SoBaseColor;
    base->rgb = color;
    cross->addChild(base);
    for (float angle : {float(M_PI / 4), float(-M_PI / 4)}) {
        auto bar = new SoSeparator;
        auto turn = new SoRotationXYZ;
        turn->axis = SoRotationXYZ::Z;
        turn->angle = angle;
        bar->addChild(turn);
        auto cube = new SoCube;
        cube->width = length;
        cube->height = thickness;
        cube->depth = thickness;
        bar->addChild(cube);
        cross->addChild(bar);
    }
    getMarks()->addChild(cross);
    crosses.emplace_back(pos, transform);
    calculateScaleAndOrientation();
}

bool GizmoContainer::isEnabled()
{
    return MiscParams::getEnableGizmos();
}

bool GizmoContainer::isCoarseSnapEnabled()
{
    return MiscParams::getEnableCoarseSnap();
}

Qt::KeyboardModifier GizmoContainer::getFineSnapModifier()
{
    auto modifier = static_cast<Qt::KeyboardModifier>(MiscParams::getFineSnapModifier());
    if (modifier == Qt::ControlModifier) {
        return modifier;
    }
    return Qt::ShiftModifier;
}

InputHint::UserInput GizmoContainer::getFineSnapKey()
{
    if (getFineSnapModifier() == Qt::ControlModifier) {
        return InputHint::UserInput::ModifierCtrl;
    }
    return InputHint::UserInput::ModifierShift;
}

bool GizmoContainer::isCoarseByDefault()
{
    return MiscParams::getDefaultCoarseDragBehavior()
        == static_cast<int>(DefaultDragBehavior::Coarse);
}

std::unique_ptr<GizmoContainer> GizmoContainer::create(
    std::initializer_list<Gui::Gizmo*> gizmos,
    ViewProviderDragger* vp
)
{
    auto gizmoContainer = std::make_unique<GizmoContainer>();
    gizmoContainer->addGizmos(gizmos);
    gizmoContainer->viewProvider = vp;

    vp->setGizmoContainer(gizmoContainer.get());

    return gizmoContainer;
}
