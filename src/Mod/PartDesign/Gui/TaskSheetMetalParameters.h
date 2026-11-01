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
#include <vector>

#include <fastsignals/signal.h>

#include <Gui/Selection/Selection.h>

#include "TaskFeatureParameters.h"

class QLabel;
class QListWidget;
class QToolButton;

namespace App
{
class PropertyLinkSub;
}

namespace PartDesignGui
{

/** A sheet metal feature's panel (docs/SheetMetal.md sec 5.1)
 *
 * A box for each value it is given, by the property's name and of its type
 * -- a quantity, a choice, a switch -- named after it, so the panel mirror
 * carries it to a browser; the rule's values said; and what is wrong said in
 * the panel, never in a dialog. With refs, the property listing the
 * feature's edges or faces (element), their list, picked in the view from the
 * feature built on.
 */
class TaskSheetMetalParameters: public TaskFeatureParameters, public Gui::SelectionObserver
{
    Q_OBJECT

public:
    TaskSheetMetalParameters(ViewProvider* vp,
                             const std::string& pixmap,
                             const QString& title,
                             const std::vector<const char*>& values,
                             const char* refs = nullptr,
                             const char* element = "Edge");
    ~TaskSheetMetalParameters() override;

    void refresh() override;

protected:
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;
    void finishedRecomputeFeature() override;

private:
    App::Property* property(const char* name) const;
    App::PropertyLinkSub* references() const;
    App::DocumentObject* builtOn() const;
    void addValue(class QGridLayout* grid, int row, const char* name);
    void showValue(const std::string& name);
    void fillList();
    void startPick(bool on);
    void removeCurrent();
    void showStatus();

    QWidget* proxy;
    QListWidget* refList = nullptr;
    QToolButton* addButton = nullptr;
    QLabel* rule;
    QLabel* status;
    std::vector<const char*> names;
    const char* refs;
    std::string element;
    std::map<std::string, QWidget*> boxes;
    bool picking = false;
    fastsignals::scoped_connection recomputed;
    fastsignals::scoped_connection changed;
};

class TaskDlgSheetMetalParameters: public TaskDlgFeatureParameters
{
    Q_OBJECT

public:
    TaskDlgSheetMetalParameters(ViewProvider* vp, TaskSheetMetalParameters* panel);
};

}  // namespace PartDesignGui
