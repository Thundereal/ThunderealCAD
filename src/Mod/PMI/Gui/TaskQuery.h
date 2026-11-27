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

#ifndef PMIGUI_TASKQUERY_H
#define PMIGUI_TASKQUERY_H

#include <vector>

#include <App/DocumentObserver.h>
#include <Gui/Selection/Selection.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>

class QLabel;
class QListWidget;
class QListWidgetItem;

namespace PMIGui
{

/** What applies to a face (docs/GDandT.md sec 6.5; upstream's query of a
 * feature's specifications): pick a face, and the panel lists the callouts
 * on it and the tolerances citing a datum on it. It follows the pick; a
 * pick of no face leaves the list as it is, so a callout clicked in the
 * list is selected -- its faces outlined -- and the list stays.
 */
class TaskQuery: public QWidget, public Gui::SelectionObserver
{
    Q_OBJECT

public:
    explicit TaskQuery(QWidget* parent = nullptr);

    /// List what applies to this pick; false if it is no annotated face
    bool show(App::DocumentObject* top, const char* subname);

private:
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;
    void update();
    void clicked(QListWidgetItem* item);

    bool _pending = false;
    QLabel* _what;
    QListWidget* _list;
    std::vector<App::DocumentObjectT> _items;
};

class TaskDlgQuery: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    TaskDlgQuery();

    bool accept() override
    {
        return true;
    }
    bool reject() override
    {
        return true;
    }
    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Close;
    }

private:
    TaskQuery* _widget;
};

}  // namespace PMIGui

#endif  // PMIGUI_TASKQUERY_H
