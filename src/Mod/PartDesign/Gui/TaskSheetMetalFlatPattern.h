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

#include <map>
#include <string>

#include <fastsignals/signal.h>

#include <Gui/Selection/Selection.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>

class QLabel;
class QListWidget;
class QToolButton;

namespace PartDesign
{
class SheetMetalFlatPattern;
}

namespace PartDesignGui
{

class ViewProviderSheetMetalFlatPattern;

/** The flat pattern's panel (docs/SheetMetal.md sec 8.1): what it lays flat,
 * the wall's skin laid on XY picked in the view, and the values a bill of
 * materials takes. Its widgets are named for the panel mirror: labelSource,
 * listRootFace, buttonPick, buttonFirstWall, labelStatus, and a label per
 * value named after its property.
 */
class TaskSheetMetalFlatPattern: public Gui::TaskView::TaskBox, public Gui::SelectionObserver
{
    Q_OBJECT

public:
    explicit TaskSheetMetalFlatPattern(ViewProviderSheetMetalFlatPattern* vp);
    ~TaskSheetMetalFlatPattern() override;

    void refresh();

protected:
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;

private:
    PartDesign::SheetMetalFlatPattern* flat() const;
    void startPick(bool on);
    void firstWall();
    void setRoot(App::DocumentObject* source, const std::string& face);

    ViewProviderSheetMetalFlatPattern* vp;
    QLabel* source;
    QListWidget* roots;
    QToolButton* pick;
    QLabel* status;
    std::map<std::string, QLabel*> values;
    bool picking = false;
    fastsignals::scoped_connection changed;
    fastsignals::scoped_connection recomputed;
};

/// The flat pattern's edit: one undo step for OK, Cancel puts it back
class TaskDlgSheetMetalFlatPattern: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    explicit TaskDlgSheetMetalFlatPattern(ViewProviderSheetMetalFlatPattern* vp);

    bool accept() override;
    bool reject() override;
    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Ok | QDialogButtonBox::Cancel;
    }

private:
    ViewProviderSheetMetalFlatPattern* vp;
};

}  // namespace PartDesignGui
