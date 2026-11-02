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

#include <Gui/Selection/Selection.h>

#include "TaskFeatureParameters.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QToolButton;

namespace Gui
{
class QuantitySpinBox;
}

namespace PartDesign
{
class Thicken;
}

namespace PartDesignGui
{

/** Thicken's panel: the faces, picked in the view or taken out of the list
 * (Remove, or Delete), the thickness, either side or both, the join, and
 * why it is not built
 */
class TaskThickenParameters: public TaskFeatureParameters, public Gui::SelectionObserver
{
    Q_OBJECT

public:
    explicit TaskThickenParameters(ViewProvider* vp, QWidget* parent = nullptr);
    ~TaskThickenParameters() override;

    void refresh() override;

protected:
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;
    void finishedRecomputeFeature() override;

private:
    PartDesign::Thicken* feature() const;
    void startPick(bool on);
    void removeCurrent();
    void fillList();
    void showStatus();

    QWidget* proxy;
    QListWidget* list;
    QToolButton* add;
    Gui::QuantitySpinBox* thickness;
    QCheckBox* reversed;
    QCheckBox* midplane;
    QComboBox* join;
    QLabel* status;
    bool picking = false;
};

class TaskDlgThickenParameters: public TaskDlgFeatureParameters
{
    Q_OBJECT

public:
    explicit TaskDlgThickenParameters(ViewProvider* vp);
};

}  // namespace PartDesignGui
