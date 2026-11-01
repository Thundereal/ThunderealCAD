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

#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>

class QLabel;
class QToolButton;
class QGridLayout;

namespace MatGui
{
class MaterialTreeWidget;
}

namespace PartDesign
{
class SheetMetalRule;
}

namespace PartDesignGui
{

class ViewProviderSheetMetalRule;

/** The rule's panel (docs/SheetMetal.md sec 4.6, 8.1): its card, chosen
 * from the material list or the body's; a box for each value; and for each
 * value the card gives, whether it is the card's or set by hand, with a
 * button giving it back to the card. Its widgets are named for the panel
 * mirror: materialCard, labelCard, a box per value named after its
 * property, source<Name> and reset<Name> for the card's values, labelUsers
 * and labelStatus.
 */
class TaskSheetMetalRule: public Gui::TaskView::TaskBox
{
    Q_OBJECT

public:
    explicit TaskSheetMetalRule(ViewProviderSheetMetalRule* vp);

    void refresh();

private:
    PartDesign::SheetMetalRule* rule() const;
    void addValue(QGridLayout* grid, int row, const char* name, bool fromCard);
    void showValue(const std::string& name);
    void showCard();
    void showStatus();
    void chooseCard(const QString& uuid);
    void followBody();
    void changed();

    ViewProviderSheetMetalRule* vp;
    QWidget* proxy;
    MatGui::MaterialTreeWidget* cards;
    QLabel* card;
    QLabel* users;
    QLabel* status;
    std::map<std::string, QWidget*> boxes;
    std::map<std::string, QLabel*> sources;
    std::map<std::string, QToolButton*> resets;
    fastsignals::scoped_connection changedObject;
    fastsignals::scoped_connection recomputed;
};

/// The rule's panel in one undo step: OK keeps it, Cancel puts it back
class TaskDlgSheetMetalRule: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    explicit TaskDlgSheetMetalRule(ViewProviderSheetMetalRule* vp);

    bool accept() override;
    bool reject() override;

    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Ok | QDialogButtonBox::Cancel;
    }

private:
    ViewProviderSheetMetalRule* vp;
};

}  // namespace PartDesignGui
