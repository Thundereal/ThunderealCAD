/***************************************************************************
 *   Copyright (c) 2004 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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
#include <QAction>
#include <QKeyEvent>
#include <QList>
#include <QMouseEvent>
#endif

#include <map>

#include <App/Document.h>
#include <App/Transactions.h>

#include "DlgUndoRedo.h"
#include "Application.h"
#include "Document.h"
#include "MainWindow.h"
#include "MDIView.h"


using namespace Gui::Dialog;

/* TRANSLATOR Gui::Dialog::UndoRedoDialog */

UndoRedoMenu::UndoRedoMenu(bool redo, QWidget* parent)
  : QMenu(parent)
  , redo(redo)
{
    connect(this, &QMenu::aboutToShow, this, &UndoRedoMenu::onFetchInfo);
}

UndoRedoMenu::~UndoRedoMenu() = default;

/**
 *  This method fetches the undo / redo information from the
 *  active document and shows it in the undo / redo dialog.
 */
void UndoRedoMenu::onFetchInfo()
{
    clear(); // Remove first all items

    MDIView* mdi =  getMainWindow()->activeWindow();
    if (!mdi)
        return;
    const QStringList names = redo ? mdi->redoActions() : mdi->undoActions();
    const auto steps = mdi->undoSteps(redo);
    // A view whose undo is not its document's lists names only
    bool grouped = steps.size() == static_cast<std::size_t>(names.size());
    if (!grouped) {
        for (const auto& name : names)
            addAction(name, this, &UndoRedoMenu::onSelected);
        return;
    }

    // One row a step, or a group of an edit's steps (sec 12.11), in the
    // order the first of each is listed
    std::vector<std::vector<std::size_t>> rows;
    std::map<int64_t, std::size_t> rowOfGroup;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const int64_t group = steps[i].group;
        if (group) {
            auto it = rowOfGroup.find(group);
            if (it != rowOfGroup.end()) {
                rows[it->second].push_back(i);
                continue;
            }
            rowOfGroup[group] = rows.size();
        }
        rows.push_back({i});
    }
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto& members = rows[r];
        if (members.size() == 1) {
            QAction* action = addAction(names[int(members.front())], this,
                                        &UndoRedoMenu::onSelected);
            action->setData(int(r));
            continue;
        }
        // Named by the step the edit began with: the oldest on an undo's
        // list, the first on a redo's
        const std::size_t first = redo ? members.front() : members.back();
        QMenu* sub = addMenu(tr("%1 (%2 steps)").arg(names[int(first)]).arg(members.size()));
        sub->menuAction()->setData(int(r));
        for (std::size_t m : members) {
            QAction* action = sub->addAction(names[int(m)], this, &UndoRedoMenu::onStepSelected);
            action->setData(QVariantList {int(r), steps[m].id});
        }
    }
}

int UndoRedoMenu::rowOf(QAction* action) const
{
    if (!action)
        return -1;
    bool ok = false;
    int row = action->data().toInt(&ok);
    if (ok)
        return row;
    // a names-only list: its place
    return int(actions().indexOf(action));
}

bool UndoRedoMenu::takeRows(int rows)
{
    for (int i = 0; i < rows; ++i) {
        // A row that did not go -- refused, and the user did not go on past
        // it -- ends the run: the next message would try it again.
        MDIView* mdi = getMainWindow()->activeWindow();
        const auto before = mdi ? (redo ? mdi->redoActions() : mdi->undoActions()).size() : 0;
        Gui::Application::Instance->sendMsgToActiveView(redo ? "Redo" : "Undo");
        mdi = getMainWindow()->activeWindow();
        if (!mdi || (redo ? mdi->redoActions() : mdi->undoActions()).size() >= before)
            return false;
    }
    return true;
}

/** Closes the dialog and undoes (redoes) down to the row picked. */
void UndoRedoMenu::onSelected()
{
    takeRows(rowOf(static_cast<QAction*>(sender())) + 1);
}

/** A step of a group picked: the rows above it, then the group to it. */
void UndoRedoMenu::onStepSelected()
{
    auto action = static_cast<QAction*>(sender());
    const QVariantList data = action->data().toList();
    if (data.size() != 2)
        return;
    if (!takeRows(data[0].toInt()))
        return;
    MDIView* mdi = getMainWindow()->activeWindow();
    Gui::Document* doc = mdi ? mdi->getGuiDocument() : nullptr;
    if (!doc)
        return;
    App::UndoViewScope scope(mdi->undoView());
    doc->undoTo(data[1].toInt(), redo);
    Gui::Application::Instance->updateActions(true);
}

/** A group's row clicked, not only hovered: the whole edit */
void UndoRedoMenu::mouseReleaseEvent(QMouseEvent* event)
{
    QAction* action = actionAt(event->pos());
    if (action && action->menu() && event->button() == Qt::LeftButton) {
        const int row = rowOf(action);
        close();
        takeRows(row + 1);
        return;
    }
    QMenu::mouseReleaseEvent(event);
}

void UndoRedoMenu::keyPressEvent(QKeyEvent* event)
{
    QAction* action = activeAction();
    if (action && action->menu()
            && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        const int row = rowOf(action);
        close();
        takeRows(row + 1);
        return;
    }
    QMenu::keyPressEvent(event);
}

/* TRANSLATOR Gui::Dialog::UndoDialog */

UndoDialog::UndoDialog( QWidget* parent )
  : UndoRedoMenu(false, parent)
{
}

UndoDialog::~UndoDialog() = default;

/* TRANSLATOR Gui::Dialog::RedoDialog */

RedoDialog::RedoDialog( QWidget* parent )
  : UndoRedoMenu(true, parent)
{
}

RedoDialog::~RedoDialog() = default;

#include "moc_DlgUndoRedo.cpp"
