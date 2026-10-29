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

#ifndef GUI_DLG_EXPORT_LINKED_SET_H
#define GUI_DLG_EXPORT_LINKED_SET_H

#include <QDialog>

#include <FCGlobal.h>

class QCheckBox;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace App
{
class Document;
}

namespace Gui
{

/** File > Export with linked files (docs/TransactionLogJournal.md sec 35 L7): the
 * set of the document -- every file its links reach -- listed with what
 * each is and the name it will be written under, the folder, and whether
 * the files keep their history -- and, when library links reach some, whether
 * those stay in their library (docs/LibraryPaths.md step 4). Accepted, the
 * set is written by
 * Document.exportLinkedSet(), recorded as a macro command.
 */
class GuiExport DlgExportLinkedSet: public QDialog
{
    Q_OBJECT

public:
    explicit DlgExportLinkedSet(App::Document* top, QWidget* parent = nullptr);
    void accept() override;

private:
    void browse();
    /// The list, from the plan of the set with the options as they are.
    void fill();

    std::string _docName;
    QTreeWidget* _files;
    QLineEdit* _folder;
    QCheckBox* _history;
    QCheckBox* _libraries;
    QPushButton* _export;
    int _written = 0;
};

}  // namespace Gui

#endif  // GUI_DLG_EXPORT_LINKED_SET_H
