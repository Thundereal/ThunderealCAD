/***************************************************************************
 *   Copyright (C) 2015 Alexander Golubev (Fat-Zer) <fatzer2@gmail.com>    *
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
#include <QApplication>
#include <QMessageBox>
#include <QCheckBox>
#endif
#include <algorithm>
#include <QEvent>
#include <QKeyEvent>
#include <QTimer>

#include <App/PropertyGeo.h>
#include <App/PropertyLinks.h>
#include <Base/Tools.h>
#include <Gui/Application.h>
#include <Gui/CommandT.h>
#include <Gui/Inventor/Draggers/Gizmo.h>
#include <Gui/MainWindow.h>
#include <Gui/OnViewValue.h>
#include <Gui/BitmapFactory.h>
#include <Gui/MDIView.h>
#include <Gui/PrefWidgets.h>
#include <Gui/TaskOwner.h>
#include <Gui/ViewerContext.h>
#include <Gui/ViewParams.h>
#include <Gui/WaitCursor.h>
#include <Mod/Part/Gui/PartParams.h>
#include <Mod/PartDesign/App/FeatureAddSub.h>
#include <Mod/PartDesign/App/Body.h>

#include "TaskFeatureParameters.h"
#include "TaskSketchBasedParameters.h"

using namespace PartDesignGui;
using namespace Gui;

/*********************************************************************
 *                      Task Feature Parameters                      *
 *********************************************************************/

namespace {

/** Opens a step for every user event that reaches a panel, before the
 * panel's widgets see it (TaskFeatureParameters::openStepForEvent): no
 * change the event makes then lands outside the panel's own steps
 * (docs/HandsOnQueue.md entry 232). On the application, so that a popup
 * of the panel's -- a combo box's list -- is seen as the panel's too.
 */
class StepOpener : public QObject
{
public:
    explicit StepOpener(TaskFeatureParameters *panel)
        : QObject(panel), panel(panel)
    {
        qApp->installEventFilter(this);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::KeyPress:
        case QEvent::Wheel:
            if (panel->ownsItsSteps() && (inside(watched, panel) || inside(watched, panel->editView())))
                panel->openStepForEvent();
            break;
        default:
            break;
        }
        return false;
    }

private:
    /// The parent chain, across windows: a popup is its widget's. The edit's
    /// own 3D view counts too -- a pick there may change the feature.
    static bool inside(QObject *obj, QObject *ancestor)
    {
        if (!ancestor)
            return false;
        for (; obj; obj = obj->parent()) {
            if (obj == ancestor)
                return true;
        }
        return false;
    }

    TaskFeatureParameters *panel;
};

} // namespace

TaskFeatureParameters::TaskFeatureParameters(PartDesignGui::ViewProvider *vp, QWidget *parent,
                                                     const std::string& pixmapname, const QString& parname)
    : TaskBox(Gui::BitmapFactory().pixmap(pixmapname.c_str()),parname,true, parent),
      vp(vp), blockUpdate(false)
{
    Gui::Document* doc = vp->getDocument();
    this->attachDocument(doc);
    App::GetApplication().getActiveTransaction(&transactionID, doc->getDocument());
    perViewAtOpen = Gui::ViewParams::getPerViewEdit();
    new StepOpener(this);
}

TaskFeatureParameters::TaskFeatureParameters(PartDesignGui::ViewProvider *vp, QWidget *parent,
                                             const QString& parname)
    : TaskBox(vp->getIcon().pixmap(64), parname,true, parent),
      vp(vp), blockUpdate(false)
{
    Gui::Document* doc = vp->getDocument();
    this->attachDocument(doc);
    App::GetApplication().getActiveTransaction(&transactionID, doc->getDocument());
    perViewAtOpen = Gui::ViewParams::getPerViewEdit();
    new StepOpener(this);
}

TaskFeatureParameters::~TaskFeatureParameters()
{
    hideDraggerHints();
    if (failCursor)
        QApplication::restoreOverrideCursor();
}

bool TaskFeatureParameters::isDragFailing() const
{
    auto obj = vp ? vp->getObject() : nullptr;
    return obj && obj->isError();
}

void TaskFeatureParameters::updateFailCursor()
{
    // While a drag recomputes the feature: a held step and the left button
    // down
    const bool dragging = heldSteps > 0
        && (QApplication::mouseButtons() & Qt::LeftButton);
    const bool fail = dragging && isDragFailing();
    if (fail == failCursor)
        return;
    failCursor = fail;
    if (fail)
        QApplication::setOverrideCursor(Qt::ForbiddenCursor);
    else
        QApplication::restoreOverrideCursor();
}

void TaskFeatureParameters::showDraggerHints()
{
    if (!Gui::GizmoContainer::isEnabled() || !Gui::GizmoContainer::isCoarseSnapEnabled()) {
        return;
    }

    const Gui::InputHint::UserInput key = Gui::GizmoContainer::getFineSnapKey();
    const bool coarseByDefault = Gui::GizmoContainer::isCoarseByDefault();

    QString message;
    if (coarseByDefault) {
        message = tr("%1 fine dragging");
    }
    else {
        message = tr("%1 coarse dragging");
    }

    Gui::getMainWindow()->showHints({{
        .message = message,
        .sequences = {{key}},
    }});
    draggerHintsShown = true;
}

void TaskFeatureParameters::hideDraggerHints()
{
    // Only the hints this panel put up: another tool may own the bar.
    if (draggerHintsShown) {
        draggerHintsShown = false;
        if (auto mw = Gui::getMainWindow()) {
            mw->hideHints();
        }
    }
}

void TaskFeatureParameters::slotDeletedObject(const Gui::ViewProviderDocumentObject& Obj)
{
    if (this->vp == &Obj)
        this->vp = nullptr;
}

void TaskFeatureParameters::slotDeleteDocument(const Gui::Document& Doc)
{
    if (this->vp && this->vp->getDocument() == &Doc)
        this->vp = nullptr;
}

void TaskFeatureParameters::slotUndoDocument(const Gui::Document &)
{
    _refresh();
    refresh();
}

void TaskFeatureParameters::slotRedoDocument(const Gui::Document &)
{
    _refresh();
    refresh();
}

void TaskFeatureParameters::_refresh()
{
    if (!this->vp)
        return;

    auto feat = Base::freecad_dynamic_cast<PartDesign::Feature>(this->vp->getObject());
    if (feat) {
        if (checkBoxNewSolid) {
            QSignalBlocker guard(checkBoxNewSolid);
            checkBoxNewSolid->setChecked(feat->NewSolid.getValue());
        } else if (comboAddSubType) {
            QSignalBlocker guard(comboAddSubType);
            if (feat->NewSolid.getValue())
                comboAddSubType->setCurrentIndex(0);
            else {
                switch(static_cast<PartDesign::FeatureAddSub*>(feat)->getAddSubType()) {
                case PartDesign::FeatureAddSub::Subtractive:
                    comboAddSubType->setCurrentIndex(2);
                    break;
                case PartDesign::FeatureAddSub::Intersecting:
                    comboAddSubType->setCurrentIndex(3);
                    break;
                default:
                    comboAddSubType->setCurrentIndex(1);
                }
            }
        }
    }
}

void TaskFeatureParameters::onUpdateView(bool on)
{
    blockUpdate = !on;
    recomputeFeature();
}

void TaskFeatureParameters::addBlinkWidget(QWidget *widget, const QString &txt)
{
    QString text;
    if (auto *w = qobject_cast<QLineEdit*>(widget))
        text = w->placeholderText();
    else if (auto *w = qobject_cast<QLabel*>(widget))
        text = w->text();
    else if (auto *w = qobject_cast<QAbstractButton*>(widget))
        text = w->text();
    else
        return;
    bool added = !blinkWidgets.count(widget);
    auto &info = blinkWidgets[widget];
    if (added)
        info.minWidth = widget->minimumWidth();
    info.widget = widget;
    info.text = text;
    info.altText = txt;
    if (blinkTimerId == 0)
        blinkTimerId = startTimer(500);
}

void TaskFeatureParameters::BlinkInfo::setText(const QString &text)
{
    if (auto *w = qobject_cast<QLineEdit*>(widget))
        w->setPlaceholderText(text);
    else if (auto *w = qobject_cast<QLabel*>(widget))
        w->setText(text);
    else if (auto *w = qobject_cast<QAbstractButton*>(widget)) {
        int width = w->width();
        w->setText(text);
        w->setMinimumWidth(std::max(width, w->sizeHint().width()));
    }
}

void TaskFeatureParameters::removeBlinkWidget(QWidget *widget)
{
    auto it = blinkWidgets.find(widget);
    if (it != blinkWidgets.end()) {
        it->second.setText(it->second.text);
        // the width held while it blinked let go: its own, or the one
        // fitTaskWidgets() gave it meanwhile
        if (auto w = it->second.widget.data()) {
            QVariant fitted = w->property("fitMinWidth");
            w->setMinimumWidth(fitted.isValid() ? fitted.toInt() : it->second.minWidth);
        }
        blinkWidgets.erase(it);
    }
}

void TaskFeatureParameters::timerEvent(QTimerEvent *ev)
{
    if (ev->timerId() == blinkTimerId) {
        for (auto &v : blinkWidgets)
            v.second.setText(blink ? v.second.altText : v.second.text);
        blink = !blink;
    }
}

void TaskFeatureParameters::recomputeFeature(bool delay)
{
    if (blockUpdate || !vp || !vp->getObject())
        return;

    if (delay && updateViewTimer) {
        int interval = PartGui::PartParams::getEditRecomputeWait();
        auto feat = Base::freecad_dynamic_cast<PartDesign::Feature>(vp->getObject());
        if (feat && feat->isRecomputePaused())
            interval /= 3;
        updateViewTimer->start(interval);
    } else {
        {
            Gui::WaitCursor cursor;
            setupTransaction();
            App::DocumentObject* obj = vp->getObject ();
            obj->getDocument()->recomputeFeature ( obj );
            saveComputedInput();
            finishedRecomputeFeature();
        }
        updateFailCursor();
        // A handle's drag that failed goes back to its last working value,
        // inside the drag's step (the user, 2026-10-19). Failed for what
        // the drag changes: a dress-up with a missing edge stays in error
        // while every drag of a working edge works (the user, 2026-10-20)
        if (App::DocumentObject* obj = vp ? vp->getObject() : nullptr)
            Gui::OnViewValueSet::featureRecomputed(obj->getDocument(), isDragFailing());
    }
}

static bool isComputedInput(const App::Property *prop)
{
    // Outputs are what a recompute writes; a shape is never a panel's input
    // and costs the most to copy
    return !(prop->getType() & App::Prop_Output)
        && !prop->testStatus(App::Property::Output)
        && !prop->isDerivedFrom(App::PropertyComplexGeoData::getClassTypeId());
}

static bool isSameInput(const App::Property &prop, const App::Property &saved)
{
    // PropertyLinkBase::isSame() takes one side's element names old style
    // and the other's new, so a link to a mapped element never equals its
    // own copy: compare both styles of both
    auto link = Base::freecad_dynamic_cast<const App::PropertyLinkBase>(&prop);
    if (!link)
        return prop.isSame(saved);
    auto savedLink = Base::freecad_dynamic_cast<const App::PropertyLinkBase>(&saved);
    if (!savedLink || link->getScope() != savedLink->getScope())
        return false;
    for (bool newStyle : {false, true}) {
        std::vector<App::DocumentObject*> objs, savedObjs;
        std::vector<std::string> subs, savedSubs;
        link->getLinks(objs, true, &subs, newStyle);
        savedLink->getLinks(savedObjs, true, &savedSubs, newStyle);
        if (objs != savedObjs || subs != savedSubs)
            return false;
    }
    return true;
}

void TaskFeatureParameters::saveComputedInput()
{
    static unsigned long sequence;
    computedInput.clear();
    computedSequence = 0;
    auto obj = vp ? vp->getObject() : nullptr;
    auto feat = Base::freecad_dynamic_cast<PartDesign::Feature>(obj);
    // A paused feature (the edit preview) made only its AddSubShape
    if (!obj || !obj->isValid() || (feat && feat->isRecomputePaused()))
        return;
    std::vector<App::Property*> props;
    obj->getPropertyList(props);
    for (auto prop : props) {
        if (isComputedInput(prop))
            computedInput.emplace_back(prop->getName(), prop->Copy());
    }
    computedSequence = ++sequence;
}

bool TaskFeatureParameters::isInputComputed() const
{
    auto obj = vp ? vp->getObject() : nullptr;
    if (!obj || !computedSequence)
        return false;
    std::vector<App::Property*> props;
    obj->getPropertyList(props);
    std::size_t count = 0;
    for (auto prop : props) {
        if (!isComputedInput(prop))
            continue;
        if (count >= computedInput.size()
                || computedInput[count].first != prop->getName()
                || !isSameInput(*prop, *computedInput[count].second))
            return false;
        ++count;
    }
    return count == computedInput.size();
}

void TaskFeatureParameters::onNewSolidChanged()
{
    if (vp) {
        PartDesign::Feature* pc =
            Base::freecad_dynamic_cast<PartDesign::Feature>(vp->getObject());
        if (pc) {
            if (checkBoxNewSolid)
                pc->NewSolid.setValue(checkBoxNewSolid->isChecked());
            else if (comboAddSubType) {
                pc->NewSolid.setValue(comboAddSubType->currentIndex() == 0);
                auto feat = static_cast<PartDesign::FeatureAddSub*>(pc);
                if (pc->NewSolid.getValue())
                    feat->AddSubType.setValue("Additive");
                else {
                    switch(comboAddSubType->currentIndex()) {
                    case 2:
                        feat->AddSubType.setValue("Subtractive");
                        break;
                    case 3:
                        feat->AddSubType.setValue("Intersecting");
                        break;
                    default:
                        feat->AddSubType.setValue("Additive");
                        break;
                    }
                }
            }
            recomputeFeature();
        }
    }
}

void TaskFeatureParameters::saveHistory()
{
    if (checkBoxUpdateView)
        checkBoxUpdateView->onSave();
}

const char *TaskFeatureParameters::updateViewParameter() const
{
    return "User parameter:BaseApp/History/UpdateView";
}

void TaskFeatureParameters::addUpdateViewCheckBox(QBoxLayout *boxLayout)
{
    updateViewTimer = new QTimer(this);
    updateViewTimer->setSingleShot(true);
    connect(updateViewTimer, SIGNAL(timeout()), this, SLOT(onUpdateViewTimer()));

    checkBoxUpdateView = new Gui::PrefCheckBox(this);
    checkBoxUpdateView->setText(tr("Update view"));
    checkBoxUpdateView->setChecked(true);
    connect(checkBoxUpdateView, SIGNAL(toggled(bool)), this, SLOT(onUpdateView(bool)));
    boxLayout->addWidget(checkBoxUpdateView);
    checkBoxUpdateView->setParamGrpPath(QByteArray(updateViewParameter()));
    checkBoxUpdateView->onRestore();
}

void TaskFeatureParameters::onUpdateViewTimer()
{
    recomputeFeature(false);
}

void TaskFeatureParameters::setupTransaction(const QString& what)
{
    if (!vp || !vp->getObject())
        return;

    auto obj = vp->getObject();
    if (!obj)
        return;

    // This document's: an edit's transaction is its document's own while
    // it runs, and a panel's event may come before its view is the active
    // one (docs/HandsOnQueue.md entry 232)
    App::Document *doc = obj->getDocument();
    int tid = 0;
    const char *name = App::GetApplication().getActiveTransaction(&tid, doc);

    std::string n("Edit ");
    n += obj->getNameInDocument();

    if (stepwise()) {
        // A step of its own for what this event changes (entry 136), unless
        // one is open already -- this event's, or a held one's. Named for
        // what it does, where the caller says (entry 228).
        std::string step = n;
        if (!what.isEmpty()) {
            step = obj->Label.getValue();
            step += ": ";
            step += what.toUtf8().constData();
        }
        if (!tid || tid != stepTid) {
            stepTid = App::GetApplication().setActiveTransaction(step.c_str(), false, doc);
            stepGeneric = false;
            if (!transactionID)
                transactionID = stepTid;
            if (!firstStepTid)
                firstStepTid = stepTid;
            if (ownsItsSteps() && stepTid)
                ownStepTids.push_back(stepTid);
        }
        else if (stepGeneric && !what.isEmpty()) {
            // Opened for the event before the panel said what it changes.
            // Nothing in it yet, the document has not opened it, and opens
            // it with the active name: that is named instead
            if (doc->hasPendingTransaction())
                doc->renameTransaction(step.c_str(), stepTid);
            else if (int named = App::GetApplication().setActiveTransaction(step.c_str(), false, doc)) {
                std::replace(ownStepTids.begin(), ownStepTids.end(), stepTid, named);
                if (transactionID == stepTid)
                    transactionID = named;
                if (firstStepTid == stepTid)
                    firstStepTid = named;
                stepTid = named;
            }
            stepGeneric = false;
        }
        if (!heldSteps && !stepCloseQueued) {
            stepCloseQueued = true;
            QTimer::singleShot(0, this, [this]() {
                stepCloseQueued = false;
                closeStep();
            });
        }
        return;
    }

    if(tid && tid == transactionID)
        return;

    if(!name || n!=name)
        tid = App::GetApplication().setActiveTransaction(n.c_str(), false, doc);

    if(!transactionID)
        transactionID = tid;
}

std::vector<int> TaskFeatureParameters::ownSteps() const
{
    std::vector<int> steps = ownStepTids;
    // The edit's first: the command that started it -- a new feature made
    // in it -- is this edit's to take back too
    if (transactionID)
        steps.push_back(transactionID);
    return steps;
}

void TaskFeatureParameters::closeOwnStep()
{
    heldSteps = 0;
    if (updateViewTimer)
        updateViewTimer->stop();
    if (stepTid)
        App::GetApplication().closeActiveTransaction(false, stepTid);
    stepTid = 0;
    stepGeneric = false;
}

Gui::MDIView *TaskFeatureParameters::editView() const
{
    if (!vp)
        return nullptr;
    auto editVp = vp;
    auto doc = Gui::Application::Instance->editDocument(
        [editVp](Gui::Document *gdoc) { return gdoc->isEditViewProvider(editVp); });
    Gui::ViewerContext *viewer = doc ? doc->editingViewerOf(vp) : nullptr;
    return viewer ? Gui::TaskOwner(viewer).mdiView() : nullptr;
}

void TaskFeatureParameters::openStepForEvent()
{
    if (!ownsItsSteps() || !vp || !vp->getObject())
        return;
    int tid = 0;
    App::GetApplication().getActiveTransaction(&tid, vp->getObject()->getDocument());
    if (tid && tid == stepTid)
        return;
    setupTransaction();
    stepGeneric = true;
}

void TaskFeatureParameters::closeStep()
{
    if (heldSteps || !stepTid)
        return;
    // The recompute the change scheduled goes in its step: an undo then
    // takes the shape back with the value
    if (updateViewTimer && updateViewTimer->isActive()) {
        if (!stepCloseQueued) {
            stepCloseQueued = true;
            QTimer::singleShot(updateViewTimer->remainingTime() + 1, this, [this]() {
                stepCloseQueued = false;
                closeStep();
            });
        }
        return;
    }
    int tid = 0;
    App::GetApplication().getActiveTransaction(&tid, vp && vp->getObject() ? vp->getObject()->getDocument() : nullptr);
    if (tid && tid == stepTid)
        App::GetApplication().closeActiveTransaction(false, tid);
    stepTid = 0;
}

void TaskFeatureParameters::beginHeldStep()
{
    if (!stepwise())
        return;
    if (heldSteps++ == 0)
        heldStepBegun();
}

void TaskFeatureParameters::endHeldStep(bool abort)
{
    if (!stepwise() || heldSteps <= 0)
        return;
    if (--heldSteps)
        return;
    // the drag is over: no stop sign
    updateFailCursor();
    if (abort) {
        // A cancelled drag: its step rolled back, the shapes its recomputes
        // made with it; a recompute still to come is not wanted
        if (updateViewTimer)
            updateViewTimer->stop();
        int tid = 0;
        App::GetApplication().getActiveTransaction(&tid, vp && vp->getObject() ? vp->getObject()->getDocument() : nullptr);
        if (tid && tid == stepTid)
            App::GetApplication().closeActiveTransaction(true, tid);
        stepTid = 0;
        // No refresh: the dragged box is put back by its handle, and a
        // refresh writes the panel's boxes back to the feature -- a step of
        // its own for nothing. The handles are placed again, though: the
        // others of the value did not hear the box put back.
        heldStepRolledBack();
        return;
    }
    closeStep();
}

void TaskFeatureParameters::holdStepWhileFocused(QWidget* box)
{
    if (!box)
        return;
    box->installEventFilter(this);
    focusHeldBoxes.emplace_back(box);
}

void TaskFeatureParameters::holdStepsOf(Gui::OnViewValueSet* values)
{
    if (!values || fineUndo || !ownsItsSteps())
        return;
    connect(values, &Gui::OnViewValueSet::stepBegun, this, [this]() {
        beginHeldStep();
    });
    connect(values, &Gui::OnViewValueSet::stepEnded, this, [this](bool abort) {
        endHeldStep(abort);
    });
}

bool TaskFeatureParameters::eventFilter(QObject* watched, QEvent* event)
{
    const bool held = std::find_if(focusHeldBoxes.begin(), focusHeldBoxes.end(),
                                   [watched](const QPointer<QWidget>& box) {
                                       return box.data() == watched;
                                   })
        != focusHeldBoxes.end();
    if (stepwise() && held) {
        auto box = static_cast<QWidget*>(watched);
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                // the box takes the value first, then the step closes
                if (typingBox == box) {
                    typingBox = nullptr;
                    QTimer::singleShot(0, this, [this]() {
                        endHeldStep(false);
                    });
                }
            }
            else if (typingBox != box) {
                if (typingBox)
                    endHeldStep(false);
                typingBox = box;
                beginHeldStep();
            }
        }
        else if (event->type() == QEvent::FocusOut && typingBox == box) {
            typingBox = nullptr;
            endHeldStep(false);
        }
    }
    return Gui::TaskView::TaskBox::eventFilter(watched, event);
}

void TaskFeatureParameters::beginAcceptStep()
{
    if (!stepwise() || !vp || !vp->getObject() || acceptStepOpen)
        return;
    acceptStepOpen = true;
    // Whatever is held is done; OK's recompute is a step of its own (the
    // reporter, 09:58: "Ok would be separate as it normally triggers the
    // recompute")
    heldSteps = 0;
    if (updateViewTimer)
        updateViewTimer->stop();
    int tid = 0;
    App::GetApplication().getActiveTransaction(&tid, vp && vp->getObject() ? vp->getObject()->getDocument() : nullptr);
    if (tid && tid == stepTid)
        App::GetApplication().closeActiveTransaction(false, tid);
    // what it is, as each step says (entry 228)
    std::string n = vp->getObject()->Label.getValue();
    n += ": ";
    n += tr("OK, recompute").toUtf8().constData();
    stepTid = App::GetApplication().setActiveTransaction(n.c_str(), false,
                                                         vp->getObject()->getDocument());
    if (!transactionID)
        transactionID = stepTid;
    if (ownsItsSteps() && stepTid)
        ownStepTids.push_back(stepTid);
}

void TaskFeatureParameters::addOperationCombo(QBoxLayout *layout)
{
    if (!vp || !vp->getObject() || !layout)
        return;

    auto feat = Base::freecad_dynamic_cast<PartDesign::FeatureAddSub>(vp->getObject());
    if (feat && !feat->AddSubType.testStatus(App::Property::Hidden)) {
        QHBoxLayout *l = new QHBoxLayout;
        l->addWidget(new QLabel(tr("Operation:")));
        comboAddSubType = new QComboBox(this);
        comboAddSubType->addItem(tr("New shape"));
        comboAddSubType->addItem(tr("Additive"));
        comboAddSubType->addItem(tr("Subtractive"));
        comboAddSubType->addItem(tr("Intersecting"));
        l->addWidget(comboAddSubType);
        layout->insertLayout(0, l);
        connect(comboAddSubType, SIGNAL(currentIndexChanged(int)), this, SLOT(onNewSolidChanged()));
    } else {
        checkBoxNewSolid = new QCheckBox(this);
        checkBoxNewSolid->setText(tr("New shape"));
        checkBoxNewSolid->setToolTip(tr("Make a new separate solid using this feature"));
        layout->insertWidget(0, checkBoxNewSolid);
        connect(checkBoxNewSolid, SIGNAL(toggled(bool)), this, SLOT(onNewSolidChanged()));
    }
}

/*********************************************************************
 *                            Task Dialog                            *
 *********************************************************************/
TaskDlgFeatureParameters::TaskDlgFeatureParameters(PartDesignGui::ViewProvider *vp)
    : TaskDialog(),vp(vp)
{
    assert(vp);
}

TaskDlgFeatureParameters::~TaskDlgFeatureParameters() = default;

bool TaskDlgFeatureParameters::accept() {
    if (!vp)
        return true;

    App::DocumentObject* feature = vp->getObject();

    // A panel with an undo step per operation: OK's work a step of its own
    // (entry 136)
    for (QWidget* wgt : Content) {
        if (auto param = qobject_cast<TaskFeatureParameters*>(wgt)) {
            param->beginAcceptStep();
        }
    }

    try {
        // Iterate over parameter dialogs and apply all parameters from them
        for ( QWidget *wgt : Content ) {
            TaskFeatureParameters *param = qobject_cast<TaskFeatureParameters *> (wgt);
            if(!param)
                continue;

            param->saveHistory ();
            param->apply ();
        }
        // Make sure the feature is what we are expecting
        // Should be fine but you never know...
        if ( !feature->isDerivedFrom<PartDesign::Feature>() ) {
            THROWM(Base::TypeError, "Bad object processed in the feature dialog.")
        }

        resetOwnEdit();

        // The panel's own recompute leaves the feature touched, so the
        // document's would make it all over again (upstream c1c9cb63e0) -- a
        // dress-up every time, a Pad without the edit preview. When nothing
        // changed since the panel's last full recompute, only the objects
        // that use the feature are left to make. Not the whole document's
        // purgeTouched(), which upstream does: those need recomputing.
        TaskFeatureParameters *last = nullptr;
        for (QWidget *wgt : Content) {
            auto param = qobject_cast<TaskFeatureParameters*>(wgt);
            if (param && (!last || param->getComputedSequence() > last->getComputedSequence()))
                last = param;
        }
        if (feature->isTouched() && last && last->isInputComputed()) {
            feature->purgeTouched();
            for (auto obj : feature->getInList())
                obj->enforceRecompute();
        }
        Gui::cmdAppDocument(feature, "recompute()");

        if (!feature->isValid()) {
            THROWM(Base::RuntimeError, vp->getObject()->getStatusString())
        }

        // detach the task panel from the selection to avoid to handling of selection change
        std::vector<QWidget*> subwidgets = getDialogContent();
        for (auto it : subwidgets) {
            TaskSketchBasedParameters* param = qobject_cast<TaskSketchBasedParameters*>(it);
            if (param)
                param->exitSelectionMode();
        }

        Gui::Command::commitCommand();

    } catch (const Base::Exception& e) {
        // Generally the only thing that should fail is feature->isValid() others should be fine
        QString errorText = QApplication::translate(feature->getTypeId().getName(), e.what());
        QMessageBox::warning(Gui::getMainWindow(), tr("Input error"), errorText);
        return false;
    }

    return true;
}

bool TaskDlgFeatureParameters::reject()
{
    // detach the task panel from the selection to avoid handling selection change
    std::vector<QWidget*> subwidgets = getDialogContent();
    for (auto it : subwidgets) {
        TaskSketchBasedParameters* param = qobject_cast<TaskSketchBasedParameters*>(it);
        if (param)
            param->exitSelectionMode();
    }

    if (rejectOwnSteps())
        return true;

    // roll back the done things which may delete the feature
    Gui::Command::abortCommand();
    Gui::Command::doCommand(Gui::Command::Gui,"Gui.activeDocument().resetEdit()");
    return true;
}

bool TaskDlgFeatureParameters::holdsTransaction() const
{
    bool own = false;
    for (QWidget *wgt : Content) {
        if (auto param = qobject_cast<TaskFeatureParameters*>(wgt)) {
            if (!param->ownsItsSteps())
                return true;
            own = true;
        }
    }
    return !own;
}

bool TaskDlgFeatureParameters::rejectOwnSteps()
{
    if (holdsTransaction())
        return false;
    // Every step of its own, and those only: another view's edit of the
    // document may have made steps between them (docs/HandsOnQueue.md
    // entry 232). A new feature goes with the edit's first.
    std::vector<int> steps;
    for (QWidget *wgt : Content) {
        if (auto param = qobject_cast<TaskFeatureParameters*>(wgt)) {
            param->closeOwnStep();
            for (int tid : param->ownSteps())
                steps.push_back(tid);
        }
    }
    Gui::Document *gdoc = vp ? vp->getDocument() : nullptr;
    // Ended first, so the edit's preview is gone before what it showed is
    // taken back -- the feature itself, when the edit made it
    resetOwnEdit();
    if (gdoc)
        gdoc->undoOwnSteps(steps);
    return true;
}

void TaskDlgFeatureParameters::resetOwnEdit()
{
    App::DocumentObject *feature = vp ? vp->getObject() : nullptr;
    if (!feature) {
        Gui::Command::doCommand(Gui::Command::Gui, "Gui.activeDocument().resetEdit()");
        return;
    }
    // In the view this edit runs in, whichever is active: another view of
    // the document may have an edit of its own (docs/HandsOnQueue.md
    // entry 232)
    Gui::ViewerContext *viewer = nullptr;
    auto editVp = vp;
    if (auto editDoc = Gui::Application::Instance->editDocument(
            [editVp](Gui::Document *doc) { return doc->isEditViewProvider(editVp); }))
        viewer = editDoc->editingViewerOf(vp);
    Gui::ViewerScope scope(viewer);
    Gui::cmdGuiDocument(feature, "resetEdit()");
}

#include "moc_TaskFeatureParameters.cpp"
