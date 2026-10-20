/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com>*
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

#ifndef GUI_DLG_IMPORT_LINKED_SET_H
#define GUI_DLG_IMPORT_LINKED_SET_H

#include <QDialog>

#include <App/ImportSet.h>
#include <FCGlobal.h>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace App
{
class Document;
}

namespace Gui
{

/** File > Import a Linked Set (docs/TransactionLog.md sec 35 L6): another
 * copy of the document's set -- its top chosen -- read and paired with ours
 * file by file (L2, L3), the pairing changeable; brought in as branches of
 * ours (L4, L5); then merged, each file merged, taken whole or kept ours, or
 * for a file of theirs alone written or left out, as one undo step.
 */
class GuiExport DlgImportLinkedSet: public QDialog
{
    Q_OBJECT

public:
    explicit DlgImportLinkedSet(App::Document* top, QWidget* parent = nullptr);
    /// A set imported before and not merged (sec 35 Q9), at its merge.
    void resume(const App::PendingSet& set);

private:
    void browse();
    /// Their set read and paired, listed.
    void readSet();
    /// The pairing as listed, imported.
    void bringIn();
    /// The actions as listed, merged.
    void merge();
    /// The actions a row offers, for whether it is paired.
    void fillActions(QTreeWidgetItem* item, bool paired, bool certain);
    /// The rows' states, as the import left them; the merge next.
    void showImported();
    App::Document* top() const;

    std::string _docName;
    QLineEdit* _theirs;
    QTreeWidget* _files;
    QPushButton* _read;
    QPushButton* _import;
    QPushButton* _merge;
    QLabel* _state;
    /// Our documents, by name, a file of theirs may be paired with.
    std::vector<std::pair<std::string, QString>> _ours;
    App::SetImportResult _imported;
};

}  // namespace Gui

#endif  // GUI_DLG_IMPORT_LINKED_SET_H
