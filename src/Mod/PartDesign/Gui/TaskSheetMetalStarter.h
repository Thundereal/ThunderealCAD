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

#include <App/DocumentObserver.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>

class QCheckBox;
class QComboBox;
class QLabel;
class QTimer;

namespace Gui
{
class QuantitySpinBox;
}

namespace PartDesign
{
class Body;
}

namespace PartDesignGui
{

/** A starter shape's kind and sizes (docs/SheetMetal.md sec 5.1): the shape
 * is made as they are set, PartDesign.makeStarterShape() in the panel's own
 * transaction, again each time they change, and said in red where it cannot
 * be
 */
class TaskSheetMetalStarter: public Gui::TaskView::TaskBox
{
    Q_OBJECT

public:
    explicit TaskSheetMetalStarter(PartDesign::Body* body);

    /// The shape made as the panel stands, in a transaction of its own; false,
    /// and said, where it cannot be
    bool make();
    /// make(), and kept: false if it cannot be
    bool keep();
    /// What make() made taken away
    void drop();

private:
    /// The Python that makes the shape as the panel stands
    std::string script() const;
    void changed();
    void say(const QString& text, bool refused);

    App::DocumentObjectT body;
    QComboBox* kind;
    Gui::QuantitySpinBox* length;
    Gui::QuantitySpinBox* width;
    Gui::QuantitySpinBox* height;
    Gui::QuantitySpinBox* flangeWidth;
    QCheckBox* centered;
    QLabel* message;
    QTimer* timer;
    bool open = false;
    bool good = false;
};

/// The panel: OK keeps the shape, as one step to undo; Cancel takes it away
class TaskDlgSheetMetalStarter: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    explicit TaskDlgSheetMetalStarter(PartDesign::Body* body);

    bool accept() override;
    bool reject() override;

private:
    TaskSheetMetalStarter* starter;
};

}  // namespace PartDesignGui
