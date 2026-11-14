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
#include <memory>
#include <string>
#include <vector>

#include <QPointer>

#include <fastsignals/signal.h>

#include <App/DocumentObserver.h>

#include <Gui/InventorBase.h>
#include <Gui/Selection/Selection.h>

#include "TaskFeatureParameters.h"
#include "ViewProviderSheetMetal.h"

class QComboBox;
class QLabel;
class QListWidget;
class SoSeparator;
class SoTransform;
class QToolButton;

namespace App
{
class PropertyLinkSub;
}

namespace PartDesignGui
{

/// A property's or a choice's name as words, as the panels say it:
/// MaterialSide is "Material side", Gap1 "Gap 1"
QString sheetMetalWords(const char* name);

/// A property listing elements of the feature built on, picked in the view
struct SheetMetalReferences
{
    const char* property;
    /// "Edge" or "Face"
    const char* element = "Edge";
    /// One only: a pick takes the place of what it held
    bool single = false;
};

/** A sheet metal feature's panel (docs/SheetMetal.md sec 5.1)
 *
 * A box for each value it is given, by the property's name and of its type
 * -- a quantity, a choice, a switch -- named after it, so the panel mirror
 * carries it to a browser; the rule's values said; and what is wrong said in
 * the panel, never in a dialog. With refs, the property listing the
 * feature's edges or faces (element), their list, picked in the view from the
 * feature built on; or several such lists, each said by its property's name.
 *
 * A reference the feature built on has no more -- its edge trimmed by a
 * feature put in or suppressed before -- leaves the feature in error, as a
 * dress-up's does. Opened on it, the panel finds the element again by its
 * name's history where one element answers (findAgain()), as a dress-up's
 * panel does, and shows what it found for the user to see: its row marked
 * and saying what it was, the element lit in the view, and a line in the
 * panel; OK keeps it, Cancel leaves the reference as it was.
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
    TaskSheetMetalParameters(ViewProvider* vp,
                             const std::string& pixmap,
                             const QString& title,
                             const std::vector<const char*>& values,
                             const std::vector<SheetMetalReferences>& lists);
    ~TaskSheetMetalParameters() override;

    void refresh() override;
    /// The flat drawing gone, the folded part drawn as it was
    void hideFlat();
    /// The panel closing: the flat gone, and not drawn again
    void closeFlat();

protected:
    void onSelectionChanged(const Gui::SelectionChanges& msg) override;
    void finishedRecomputeFeature() override;

private:
    App::Property* property(const char* name) const;
    App::PropertyLinkSub* references(int index) const;
    App::DocumentObject* builtOn() const;
    void addValue(class QGridLayout* grid, int row, const char* name);
    void showValue(const std::string& name);
    void fillList();
    void startPick(int index, bool on);
    void removeCurrent(int index);
    void showStatus();
    /** Each reference the feature built on has no more, found again by its
     * name's history (Part::Feature::getRelatedElements()) where that gives
     * one element of the list's kind that no other reference has; none
     * where it gives several, or none. Inside the panel's transaction,
     * recomputed, and shown (refound).
     */
    void findAgain();
    /** The feature as View says (docs/SheetMetal.md sec 12, 2026-11-04):
     * folded, laid flat in its place, or both, the flat moved off it by the
     * sheet's flat object's placement, else beside it; drawn under the
     * feature's view, not picked
     */
    void showSheetView();
    /// What the body drew, drawn again
    void restoreChildren();

    QWidget* proxy;
    /// A list of references, its widgets
    struct RefList
    {
        SheetMetalReferences refs;
        QListWidget* list = nullptr;
        QToolButton* add = nullptr;
    };
    std::vector<RefList> refLists;
    /// A reference found again when the panel opened: its list, what it
    /// was and what it is now
    struct Refound
    {
        int list;
        std::string was, now;
    };
    std::vector<Refound> refound;
    QLabel* refoundNote = nullptr;
    QLabel* rule;
    QLabel* status;
    QLabel* notice;
    QComboBox* sheetView = nullptr;
    QLabel* flatStatus = nullptr;
    /// The feature's flat, drawn under its view while View is not Folded
    std::unique_ptr<ViewProviderSheetMetalFlatPattern> flatView;
    Gui::CoinPtr<SoSeparator> flatRoot;
    Gui::CoinPtr<SoTransform> flatOffset;
    /// What the body drew, each view's mode switch's child and default
    /// child, kept while the flat is drawn alone: the feature and what its
    /// edit shows with it
    std::vector<std::pair<App::DocumentObjectT, std::pair<int, int>>> keptChildren;
    /// The edit's preview, taken away while the flat is drawn alone
    bool keptPreview = false;
    App::SubObjectT keptOccurrence;
    /// In an edit session: its document, the flat a node of it, and what
    /// its views were told to hide while the flat is drawn alone
    std::string sessionDoc;
    std::vector<App::SubObjectT> hiddenInSession;
    bool closing = false;
    std::vector<const char*> names;
    std::map<std::string, QWidget*> boxes;
    /// The list whose Add is down, or -1
    int picking = -1;
    fastsignals::scoped_connection recomputed;
    fastsignals::scoped_connection changed;
};

class TaskDlgSheetMetalParameters: public TaskDlgFeatureParameters
{
    Q_OBJECT

public:
    TaskDlgSheetMetalParameters(ViewProvider* vp, TaskSheetMetalParameters* panel);
    bool accept() override;
    bool reject() override;

private:
    QPointer<TaskSheetMetalParameters> panel;
};

}  // namespace PartDesignGui
