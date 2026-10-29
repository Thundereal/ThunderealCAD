/***************************************************************************
 *   Copyright (c) 2026 Zheng, Lei <realthunder.dev@gmail.com>             *
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
#include <QAbstractSpinBox>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTimer>
#endif

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>

#include "EditSteps.h"
#include "Application.h"
#include "Document.h"
#include "ViewParams.h"
#include "ViewProviderDocumentObject.h"

using namespace Gui;

namespace
{

bool isTextEdit(const QObject* obj)
{
    return qobject_cast<const QTextEdit*>(obj) || qobject_cast<const QPlainTextEdit*>(obj);
}

/// The box a key to \a obj is typed in: a spin box, around its line edit,
/// a line edit or a text edit; null for none
QWidget* typingBoxOf(QObject* obj)
{
    for (QObject* o = obj; o; o = o->parent()) {
        if (auto spin = qobject_cast<QAbstractSpinBox*>(o))
            return spin;
        if (auto w = qobject_cast<QWidget*>(o)) {
            if (w->isWindow())
                break;
        }
    }
    if (qobject_cast<QLineEdit*>(obj) || isTextEdit(obj))
        return static_cast<QWidget*>(obj);
    // a text edit's keys may reach its viewport
    QObject* parent = obj ? obj->parent() : nullptr;
    if (isTextEdit(parent))
        return static_cast<QWidget*>(parent);
    return nullptr;
}

/// Whether \a key types in \a box, rather than leaving it or doing nothing
bool typesIn(QWidget* box, int key)
{
    switch (key) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // a new line in a text edit; the value taken in any other box
            return isTextEdit(box);
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Escape:
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_Meta:
        case Qt::Key_AltGr:
            return false;
        default:
            return true;
    }
}

}  // namespace

EditSteps::EditSteps(ViewProviderDocumentObject* vp, QObject* parent)
    : EditSteps(vp ? vp->getObject() : nullptr, parent)
{}

EditSteps::EditSteps(App::DocumentObject* obj, QObject* parent)
    : QObject(parent)
    , objT(obj)
    , on(ViewParams::getPerViewEdit())
{
    if (!on)
        return;
    if (auto obj = objT.getObject())
        App::GetApplication().getActiveTransaction(&firstTid, obj->getDocument());
    qApp->installEventFilter(this);
}

EditSteps::~EditSteps() = default;

ViewProviderDocumentObject* EditSteps::viewProvider() const
{
    return Base::freecad_dynamic_cast<ViewProviderDocumentObject>(
        Application::Instance->getViewProvider(objT.getObject()));
}

Gui::Document* EditSteps::editDocument() const
{
    ViewProviderDocumentObject* vp = viewProvider();
    if (!vp)
        return nullptr;
    // The edit is the document's whose object tops the edited path -- that
    // of a link to it, possibly another document
    return Application::Instance->editDocument([vp](Gui::Document* doc) {
        return doc->isEditViewProvider(vp);
    });
}

bool EditSteps::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
        // A release too: a button is clicked, a check box toggled, on it
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            break;
        case QEvent::FocusOut:
            // Out of the box being typed in: its step is done once the box
            // has taken what was typed, which it may do now. The event that
            // took the focus away -- a click elsewhere, which comes after --
            // is a step of its own.
            if (typingBox && watched == typingBox) {
                typingBox = nullptr;
                leaving = true;
                queueClose();
            }
            return false;
        default:
            return false;
    }

    // The edit's, as the application says whose an event is
    ViewProvider* vp = viewProvider();
    if (!vp || !EditStepScope::isSet() || EditStepScope::current() != vp)
        return false;

    if (event->type() == QEvent::KeyPress) {
        QWidget* box = typingBoxOf(watched);
        if (box && typesIn(box, static_cast<QKeyEvent*>(event)->key())) {
            if (typingBox && typingBox != box)
                closeStep();
            typingBox = box;
            // Again on every key: another view's step, opened in between,
            // committed the one held
            open();
            return false;
        }
        // Enter: the box takes the value, then the step closes
        if (box && box == typingBox)
            typingBox = nullptr;
    }
    else if (event->type() == QEvent::MouseButtonPress) {
        // A spin box's arrows repeat on a timer while held: held as typing
        if (auto spin = qobject_cast<QAbstractSpinBox*>(typingBoxOf(watched))) {
            if (typingBox && typingBox != spin)
                closeStep();
            typingBox = spin;
            open();
            return false;
        }
    }
    open();
    if (!typingBox)
        queueClose();
    return false;
}

void EditSteps::open()
{
    if (leaving) {
        leaving = false;
        closeStep();
    }
    App::DocumentObject* obj = objT.getObject();
    if (!obj)
        return;
    App::Document* doc = obj->getDocument();
    int tid = 0;
    App::GetApplication().getActiveTransaction(&tid, doc);
    if (tid && tid == stepTid)
        return;
    std::string name("Edit ");
    name += obj->Label.getValue();
    stepTid = App::GetApplication().setActiveTransaction(name.c_str(), false, doc);
}

void EditSteps::closeStep()
{
    if (!stepTid)
        return;
    App::DocumentObject* obj = objT.getObject();
    int tid = 0;
    App::GetApplication().getActiveTransaction(&tid, obj ? obj->getDocument() : nullptr);
    if (tid && tid == stepTid)
        App::GetApplication().closeActiveTransaction(false, tid);
    stepTid = 0;
}

void EditSteps::queueClose()
{
    if (closeQueued)
        return;
    closeQueued = true;
    QTimer::singleShot(0, this, [this]() {
        closeQueued = false;
        leaving = false;
        if (!typingBox)
            closeStep();
    });
}

void EditSteps::step()
{
    if (!on)
        return;
    open();
    if (!typingBox)
        queueClose();
}

void EditSteps::close()
{
    typingBox = nullptr;
    leaving = false;
    closeStep();
}

void EditSteps::endEdit()
{
    ViewProviderDocumentObject* vp = viewProvider();
    if (Gui::Document* doc = editDocument())
        doc->resetEditOf(vp);
}

bool EditSteps::reject()
{
    if (!on)
        return false;
    close();
    Gui::Document* doc = editDocument();
    ViewProviderDocumentObject* vp = viewProvider();
    std::vector<int> steps;
    if (firstTid)
        steps.push_back(firstTid);
    // Every step of the edit: read before the edit, which keeps them, ends
    if (doc) {
        for (int tid : doc->editStepsOf(vp))
            steps.push_back(tid);
    }
    // Ended first, so what the edit shows is gone before what it shows is
    // taken back
    endEdit();
    if (doc)
        doc->undoOwnSteps(steps);
    return true;
}
