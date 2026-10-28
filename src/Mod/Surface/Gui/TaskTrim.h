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

#include <QWidget>

#include <App/DocumentObserver.h>
#include <Gui/DocumentObserver.h>
#include <Gui/Selection.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>
#include <Mod/Part/Gui/ViewProvider.h>
#include <Mod/Surface/App/FeatureTrim.h>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QToolButton;

namespace SurfaceGui
{

class ViewProviderTrim: public PartGui::ViewProviderPart
{
    PROPERTY_HEADER_WITH_OVERRIDE(SurfaceGui::ViewProviderTrim);

public:
    void setupContextMenu(QMenu*, QObject*, const char*) override;
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
    QIcon getIcon() const override;
};

/// The trim's task panel (docs/Surface.md sec 5.2): what is cut and by what,
/// the direction curves are laid on along, and the sides kept, picked in the
/// view from every piece of the cut. Nothing in it opens a dialog
class TrimPanel: public QWidget, public Gui::SelectionObserver, public Gui::DocumentObserver
{
    Q_OBJECT

public:
    TrimPanel(ViewProviderTrim* vp, Surface::Trim* obj);
    ~TrimPanel() override;

    void open();
    bool accept();
    bool reject();

protected:
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;
    void slotUndoDocument(const Gui::Document& Doc) override;
    void slotRedoDocument(const Gui::Document& Doc) override;

private:
    enum class Pick
    {
        None,
        Base,
        Tool,
        Remove,
        Keep,
    };
    class Gate;

    Surface::Trim* object() const;
    void checkOpenCommand();
    void startPick(Pick pick, bool on);
    void exitPick();
    void fill();
    void update();
    /// The piece at `index` of every piece shown kept, or let go
    void toggleKept(int index, const Base::Vector3d& point);

    ViewProviderTrim* vp;
    App::DocumentObjectT objT;
    bool checkCommand = true;
    Pick pick = Pick::None;
    bool filling = false;

    QListWidget* bases;
    QListWidget* tools;
    QToolButton* addBase;
    QToolButton* addTool;
    QToolButton* remove;
    QDoubleSpinBox* direction[3];
    QCheckBox* mutual;
    QToolButton* keep;
    QToolButton* clear;
    QLabel* kept;
    QLabel* status;
};

class TaskTrim: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    TaskTrim(ViewProviderTrim* vp, Surface::Trim* obj);

    void open() override;
    bool accept() override;
    bool reject() override;

    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Ok | QDialogButtonBox::Cancel;
    }

private:
    void resetEdit();

    std::string docName;
    TrimPanel* widget;
};

}  // namespace SurfaceGui
