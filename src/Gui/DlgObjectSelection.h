/****************************************************************************
 *   Copyright (c) 2018 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ****************************************************************************/
#ifndef GUI_DLGOBJECTSELECTION_H
#define GUI_DLGOBJECTSELECTION_H

#include <map>
#include <set>

#include <QDialog>
#include <QTimer>
#include <App/Document.h>
#include <App/DocumentObserver.h>
#include <App/DocumentObjectExtension.h>
#include <Base/Parameter.h>
#include <Base/Bitmask.h>

class QCheckBox;
class QTreeWidgetItem;
class QTreeWidget;

namespace Gui {

class PrefWidgetStates;

class Ui_DlgObjectSelection;

/** Dialog for object dependency selection
 */
class GuiExport DlgObjectSelection : public QDialog
{
    Q_OBJECT

public:
    /** Constructor
     *
     * Creates a dialog for selecting the given objects and their dependent
     * objects
     *
     * @param objs: initial objects
     * @param parent: optional parent widget
     * @param fl: optional window flags
     */
    DlgObjectSelection(const std::vector<App::DocumentObject*> &objs,
            QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());

    /** Constructor
     *
     * Creates a dialog for selecting the given objects and their dependent
     * objects with exclusions
     *
     * @param objs: initial objects
     * @param excludes: excluded objects. The objects and their dependents will
     *                  still be in the list but unchecked.
     * @param parent: optional parent widget
     * @param fl: optional window flags
     */
    DlgObjectSelection(const std::vector<App::DocumentObject*> &objs,
                       const std::vector<App::DocumentObject*> &excludes,
            QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());

    /// Destructor
    ~DlgObjectSelection() override;

    /// Options for getSelections()
    enum class SelectionOptions {
        /// Invert the selection, i.e. return the unselected objects
        Invert = 1,
        /// Sort the returned object in depending order
        Sort = 2,
        /// Return the unselected objects sorted in depending order
        InvertSort = 3,
    };
    /// Get the selected objects
    std::vector<App::DocumentObject*> getSelections(SelectionOptions options = SelectionOptions()) const;

    /// Add a user defined checkbox at the bottom of the dialog
    void addCheckBox(QCheckBox *box);

    /// Override the prompt message
    void setMessage(const QString &);

    /** Paste mode (docs/TransactionLogJournal.md sec 37.9): the objects listed are
     * to be brought into `target`, which has some of them already; `pairs`
     * maps each such to the one there. Its row gets a second check,
     * "Assign to" the one there (which then takes the state brought), and
     * unchecking a row leaves the object out (what is brought then refers
     * to the one there, or to nothing). An object another owns
     * (App::DocumentObject::getOwnedObjects) follows its owner and is not
     * chosen apart: an origin of an owner assigned or left out is the one
     * there, kept. One an assigned object depends on, and that is there
     * too, is assigned or left out, not duplicated.
     */
    void setPaste(App::Document* target,
                  const std::map<App::DocumentObject*, App::DocumentObject*>& pairs);
    /// Paste mode: what each object listed takes
    std::map<App::DocumentObject*, App::Document::ImportTake> getTakes() const;

    void accept() override;
    void reject() override;

private Q_SLOTS:
    void onDepItemChanged(QTreeWidgetItem * item, int);
    void onObjItemChanged(QTreeWidgetItem * item, int);
    void onItemSelectionChanged();
    void checkItemChanged();
    void onAutoDeps(bool);
    void onItemExpanded(QTreeWidgetItem *item);
    void onUseOriginalsBtnClicked();
    void onShowDeps();

private:
    QTreeWidgetItem *getItem(App::DocumentObject *obj,
                             std::vector<QTreeWidgetItem*> **items = nullptr,
                             QTreeWidgetItem *parent = nullptr);

    QTreeWidgetItem *createDepItem(QTreeWidget *parent, App::DocumentObject *obj);

    void init(const std::vector<App::DocumentObject*> &objs,
              const std::vector<App::DocumentObject*> &excludes);

    void setItemState(App::DocumentObject *obj, Qt::CheckState state, bool forced = false);
    void updateAllItemState();

    /// Paste mode: the rules of setPaste() applied to the checks as they are
    void applyPasteRules();
    /// Either mode: an object another listed one owns goes with it (sec 37.9)
    void applyOwnedRules();
    bool included(App::DocumentObject* obj) const;

private:
    Ui_DlgObjectSelection* ui;
    std::vector<App::DocumentObject*> initSels;
    std::vector<App::DocumentObject*> deps;
    std::set<App::DocumentObject*> depSet;
    std::map<App::SubObjectT, std::vector<QTreeWidgetItem*>> itemMap;
    std::map<App::SubObjectT, QTreeWidgetItem*> depMap;
    std::map<App::SubObjectT, QTreeWidgetItem*> inMap;
    std::map<App::SubObjectT, Qt::CheckState> itemChanged;
    QTreeWidgetItem *allItem = nullptr;

    QPushButton* useOriginalsBtn;
    bool returnOriginals = false;

    /// Paste mode (setPaste)
    bool pasteMode = false;
    std::map<App::DocumentObject*, App::DocumentObject*> pastePairs;
    struct PasteOwned
    {
        App::DocumentObject* owner = nullptr;
        App::DocumentObject* live = nullptr;   // the one there it stands for
        App::OwnedPaste how = App::OwnedPaste::FollowOwner;
    };
    std::map<App::DocumentObject*, PasteOwned> pasteOwned;
    std::set<App::DocumentObject*> pasteAssigned;
    bool applyingRules = false;
    /// Listed object -> the listed object that owns it (sec 37.9)
    std::map<App::DocumentObject*, App::DocumentObject*> ownedBy;

    std::unique_ptr<PrefWidgetStates> widgetStates;

    QTimer timer;
    ParameterGrp::handle hGrp;
};

} // namespace Gui

ENABLE_BITMASK_OPERATORS(Gui::DlgObjectSelection::SelectionOptions);

#endif // GUI_DLGOBJECTSELECTION_H

