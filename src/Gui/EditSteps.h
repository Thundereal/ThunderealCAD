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

#ifndef GUI_EDITSTEPS_H
#define GUI_EDITSTEPS_H

#include <vector>

#include <QObject>
#include <QPointer>

#include <App/DocumentObserver.h>
#include <FCGlobal.h>

class QWidget;

namespace Gui
{

class Document;
class ViewProviderDocumentObject;

/** Each change of an edit's panel an undo step of its own, so that the
 * edit may run beside another view's edit of the same document
 * (docs/HandsOnQueue.md entry 232).
 *
 * With PerViewEdit on when it is made, a step is named for every user event
 * that is the edit's -- one to its dialog or to the view it runs in, as
 * Gui::EditStepScope says -- and committed once the event is done. The
 * document opens it on the first change the event makes, and nothing is
 * recorded for an event that changes nothing. What is typed in a box is one
 * step, held from its first key to Enter or the focus leaving the box; so is
 * what a click in a spin box does, its arrows repeating while held.
 *
 * The steps are not kept here: the edit's document keeps them
 * (Gui::Document::editStepsOf()). OK commits the one open; Cancel takes
 * them back (reject()).
 *
 * With PerViewEdit off it does nothing, and the panel goes on holding its
 * transaction as before (isOn()).
 */
class GuiExport EditSteps: public QObject
{
public:
    /// For the edit of \a vp; parented to \a parent, the panel
    EditSteps(ViewProviderDocumentObject* vp, QObject* parent);
    /// For the edit of \a obj; parented to \a parent, the panel
    EditSteps(App::DocumentObject* obj, QObject* parent);
    ~EditSteps() override;

    /// wanted() when it was made: whether the panel's changes are steps
    bool isOn() const
    {
        return on;
    }
    /** Whether an edit started now makes each change of its panel a step of
     * its own: with View/EditStepEachChange (on by default), and always with
     * PerViewEdit, whose edits run beside each other by it
     */
    static bool wanted();
    /** Opens a step for a change made outside an event of the edit's -- a
     * deferred apply, OK's work -- unless one is open; committed once the
     * event being handled is done
     */
    void step();
    /// Commits the step open now, if it is this edit's: OK's
    void close();
    /** Cancel: the edit ended and its steps taken back
     * (Gui::Document::undoOwnSteps). False, doing nothing, when it is not
     * on: the panel aborts its transaction as before.
     */
    bool reject();
    /// Ends the edit -- its own, of the several there may be
    void endEdit();
    /// The document the edit runs in, null when it is not in edit
    Gui::Document* editDocument() const;
    /// The view provider in edit, null once it is gone
    ViewProviderDocumentObject* viewProvider() const;

    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void open();
    void closeStep();
    void queueClose();

    App::DocumentObjectT objT;
    bool on = false;
    /// The step named for the event, 0 for none
    int stepTid = 0;
    /// The transaction active when the panel was made: the command that
    /// started the edit, a new object made in it -- the edit's first
    int firstTid = 0;
    bool closeQueued = false;
    /// The box being typed in, its step held
    QPointer<QWidget> typingBox;
    /// The focus left the box typed in: its step closes before the next
    bool leaving = false;
};

}  // namespace Gui

#endif  // GUI_EDITSTEPS_H
