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

#include <array>
#include <memory>
#include <set>

#include <QWidget>

#include <App/DocumentObserver.h>
#include <Gui/DocumentObserver.h>
#include <Gui/Selection/Selection.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>
#include <Mod/Part/Gui/ViewProviderSpline.h>
#include <Mod/Surface/App/FeatureNetwork.h>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QToolButton;

namespace SurfaceGui
{

class EdgeMarkers;

class ViewProviderNetwork: public PartGui::ViewProviderSpline
{
    PROPERTY_HEADER_WITH_OVERRIDE(SurfaceGui::ViewProviderNetwork);

public:
    void setupContextMenu(QMenu*, QObject*, const char*) override;
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
    QIcon getIcon() const override;
    /// Colour the network's curves on the objects they come from: the
    /// profiles one colour, the guides another
    void highlightCurves(bool on);

private:
    /// The objects whose edges it coloured, to clear
    std::set<App::DocumentObjectT> highlighted;
};

/// The network's task panel (docs/Surface.md sec 5.1, 6, 8): its profiles and
/// guides, the families of a whole sketch separated and corrected, the four
/// edge conditions, the tolerances, and what was achieved. Nothing in it
/// opens a dialog, so it is as usable from a browser's mirror
class NetworkPanel: public QWidget, public Gui::SelectionObserver, public Gui::DocumentObserver
{
    Q_OBJECT

public:
    NetworkPanel(ViewProviderNetwork* vp, Surface::Network* obj);
    ~NetworkPanel() override;

    void open();
    bool accept();
    bool reject();

protected:
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;
    void slotUndoDocument(const Gui::Document& Doc) override;
    void slotRedoDocument(const Gui::Document& Doc) override;
    void slotDeletedObject(const Gui::ViewProviderDocumentObject& Obj) override;

private:
    enum class Pick
    {
        None,
        Profile,
        Guide,
        Remove,
        Support,
    };
    class Gate;
    struct Side
    {
        Surface::ConditionProperties props;
        QComboBox* continuity;
        QDoubleSpinBox* weight;
        QToolButton* pick;
        QLabel* support;
    };

    Surface::Network* object() const;
    void checkOpenCommand();
    void startPick(Pick pick, int side, bool on);
    void exitPick();
    void fillLists();
    void fillSides();
    /// Recompute, then show what the network built or why not
    void update();
    void moveCurve();
    void removeCurrent();
    void separate();
    void applyToAll();
    bool listed(App::DocumentObject* obj, const std::string& sub) const;

    ViewProviderNetwork* vp;
    App::DocumentObjectT objT;
    bool checkCommand = true;
    Pick pick = Pick::None;
    int pickSide = -1;
    bool filling = false;

    QListWidget* profiles;
    QListWidget* guides;
    QListWidget* lastList = nullptr;
    QToolButton* addProfile;
    QToolButton* addGuide;
    QToolButton* remove;
    QToolButton* move;
    QPushButton* separateButton;
    QLabel* familiesNote;
    std::array<Side, 4> sides;
    QDoubleSpinBox* tolerance;
    QDoubleSpinBox* fitTolerance;
    QLabel* readout;
    QLabel* achieved;
    QLabel* status;
    /// The four conditions in the view
    std::unique_ptr<EdgeMarkers> markers;
};

class TaskNetwork: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    TaskNetwork(ViewProviderNetwork* vp, Surface::Network* obj);

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
    NetworkPanel* widget;
};

}  // namespace SurfaceGui
