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

#pragma once

#include <string>
#include <vector>

#include <App/DocumentObserver.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>

class QLabel;
class QListWidget;
class QTableWidget;

namespace PartDesignGui
{

/** Check Sheet's panel (docs/SheetMetal.md sec 7, ruled 2026-11-03: on
 * demand): the bend table of a feature of a sheet or of its solid, each
 * value with where it comes from, and what is wrong with its flat
 * (PartDesign::checkSheet()). A row picked selects its faces, on the flat
 * where that is drawn; Check Again checks once more, nothing is checked
 * otherwise. Its widgets are named for the panel mirror: tableBends,
 * listProblems, labelSummary and buttonCheckAgain.
 */
class TaskSheetMetalCheck: public Gui::TaskView::TaskBox
{
    Q_OBJECT

public:
    explicit TaskSheetMetalCheck(App::DocumentObject* obj);

    /// Check the sheet again and show what is found
    void check();

private:
    /// What a row marks: the faces of the part checked, folded and flat,
    /// or the feature it is of where it names none
    struct Marks
    {
        std::string feature;
        std::vector<std::string> elements;
        std::vector<std::string> flatElements;
    };
    void select(const Marks& marks);

    App::DocumentObjectT target;
    App::DocumentObjectT part;
    QTableWidget* bends;
    QListWidget* problems;
    QLabel* summary;
    std::vector<Marks> bendMarks;
    std::vector<Marks> problemMarks;
};

/// The panel, closed with Close: it changes nothing
class TaskDlgSheetMetalCheck: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    explicit TaskDlgSheetMetalCheck(App::DocumentObject* obj);

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
};

}  // namespace PartDesignGui
