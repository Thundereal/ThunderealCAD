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

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QWidget>

#include <App/DocumentObserver.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Gui/DocumentObserver.h>
#include <Gui/Selection.h>
#include <Gui/TaskView/TaskDialog.h>
#include <Gui/TaskView/TaskView.h>
#include <Mod/Part/Gui/ViewProvider.h>
#include <Mod/Part/Gui/ViewProviderSpline.h>
#include <Mod/Surface/App/EdgeConditions.h>

#include "EdgeMarkers.h"

class QLabel;
class QListWidget;
class QToolButton;
class QVBoxLayout;

namespace SurfaceGui
{

/// What a pick takes
enum class PickKind
{
    Edges,         ///< edges
    Faces,         ///< faces
    EdgesOrWhole,  ///< edges, or objects given whole from the tree
    Any,           ///< any element, or objects whole
};

/** A task panel over one feature's properties (docs/Surface.md sec 8): picks
 * in the view, the feature recomputed on every change, why it is not built
 * said in the panel and never in a dialog, OK one undo step and Cancel
 * putting everything back, the edit ended on the feature's own document.
 */
class ToolPanel: public QWidget, public Gui::SelectionObserver, public Gui::DocumentObserver
{
    Q_OBJECT

public:
    ToolPanel(App::DocumentObject* obj, const char* name, const QString& title);
    ~ToolPanel() override;

    App::DocumentObject* object() const;
    QVBoxLayout* body() const
    {
        return layout;
    }

    using Accept = std::function<bool(App::DocumentObject*, const std::string&)>;
    using Take = std::function<void(App::DocumentObject*, const std::string&)>;
    /// One pick at a time: what it accepts, what it does with it; `button`
    /// is raised again when another starts or it ends
    void startPick(QToolButton* button, Accept accept, Take take);
    void endPick();
    bool picking() const
    {
        return gateOn;
    }
    bool allows(App::DocumentObject* obj, const std::string& sub) const;

    /// A property changed: open the transaction if need be, recompute, say
    void changed();
    void checkOpenCommand();
    /// Each widget's refill from the properties, and all of them
    void addFill(std::function<void()> fill);
    void refill();
    bool filling = false;
    /// The edge conditions in the view, made with the first
    EdgeMarkers* edgeMarkers();

    void open();
    bool accept();
    bool reject();

protected:
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;
    void slotUndoDocument(const Gui::Document&) override;
    void slotRedoDocument(const Gui::Document&) override;

private:
    class Gate;
    App::DocumentObjectT objT;
    QVBoxLayout* layout;
    QLabel* status;
    QToolButton* pickButton = nullptr;
    Accept pickAccept;
    Take pickTake;
    bool gateOn = false;
    bool checkCommand = true;
    std::vector<std::function<void()>> fills;
    std::unique_ptr<EdgeMarkers> markers;
};

/// The task dialog around a ToolPanel
class TaskTool: public Gui::TaskView::TaskDialog
{
    Q_OBJECT

public:
    TaskTool(ToolPanel* panel, const char* icon);

    void open() override;
    bool accept() override;
    bool reject() override;

    QDialogButtonBox::StandardButtons getStandardButtons() const override
    {
        return QDialogButtonBox::Ok | QDialogButtonBox::Cancel;
    }

private:
    std::string docName;
    ToolPanel* panel;
};

/// A list of picks bound to a link list, or one pick bound to a link
QWidget* pickList(ToolPanel* panel,
                  const QString& title,
                  const char* name,
                  App::PropertyLinkSubList* list,
                  PickKind kind);
QWidget* pickOne(ToolPanel* panel,
                 const QString& title,
                 const char* name,
                 App::PropertyLinkSub* link,
                 PickKind kind);
/// An edge condition: continuity, weight, and a support face; with a marker
/// in the view where `where` says
QWidget* conditionRow(ToolPanel* panel,
                      const QString& title,
                      const char* name,
                      const Surface::ConditionProperties& props,
                      EdgeMarkers::Where where = {});
/// Plain properties
QWidget* enumField(ToolPanel* panel,
                  const QString& title,
                  const char* name,
                  App::PropertyEnumeration* p);
QWidget* intField(ToolPanel* panel,
                 const QString& title,
                 const char* name,
                 App::PropertyInteger* p);
QWidget* boolField(ToolPanel* panel,
                  const QString& title,
                  const char* name,
                  App::PropertyBool* p);
QWidget* vectorField(ToolPanel* panel,
                    const QString& title,
                    const char* name,
                    App::PropertyVector* p);

class ViewProviderBlendSurface: public PartGui::ViewProviderSpline
{
    PROPERTY_HEADER_WITH_OVERRIDE(SurfaceGui::ViewProviderBlendSurface);

public:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
    QIcon getIcon() const override;
};

class ViewProviderTwoRailSweep: public PartGui::ViewProviderSpline
{
    PROPERTY_HEADER_WITH_OVERRIDE(SurfaceGui::ViewProviderTwoRailSweep);

public:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
    QIcon getIcon() const override;
};

class ViewProviderPipeShell: public PartGui::ViewProviderPart
{
    PROPERTY_HEADER_WITH_OVERRIDE(SurfaceGui::ViewProviderPipeShell);

public:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
    QIcon getIcon() const override;
};

}  // namespace SurfaceGui
