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
#include <Mod/PartDesign/App/SheetMetalCheck.h>
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
 * otherwise. A bend's K or allowance double-clicked takes a value of the
 * bend's own (ruled 2026-11-05: kept on the feature that made the bend), an
 * undoable step that recomputes and checks again; cleared, the bend takes the
 * rule's again. Its widgets are named for the panel mirror: tableBends,
 * editBendValue, listProblems, labelSummary and buttonCheckAgain.
 */
class TaskSheetMetalCheck: public Gui::TaskView::TaskBox
{
    Q_OBJECT

public:
    explicit TaskSheetMetalCheck(App::DocumentObject* obj);

    /// Check the sheet again and show what is found
    void check();

private:
    enum
    {
        KColumn = 4,
        AllowanceColumn = 5,
    };
    /// A K, or an allowance it is found from, typed into a row; empty for the
    /// rule's
    void setBendValue(int row, int column, const QString& text);

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
    std::vector<PartDesign::SheetBendRow> bendRows;
    std::vector<Marks> bendMarks;
    std::vector<Marks> problemMarks;
};

/// The panel, closed with Close: each edit in it is a step of its own
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
