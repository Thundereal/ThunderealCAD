/***************************************************************************
 *   Copyright (c) 2004 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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


#ifndef GUI_DIALOG_DLGUNDOREDO_H
#define GUI_DIALOG_DLGUNDOREDO_H

#include <QMenu>

namespace Gui {
namespace Dialog {

/** The list of an undo or a redo button: what the active view would undo
 * (or redo), newest first. Picking a row undoes down to it.
 *
 * The steps of an edit ended with OK are one row (docs/MultiViewEdit.md sec
 * 12.11), as one undo takes them: picking the row takes the edit, and
 * hovering it opens its steps, to undo the edit down to one of them.
 */
class UndoRedoMenu : public QMenu
{
    Q_OBJECT

public:
  UndoRedoMenu(bool redo, QWidget* parent);
  ~UndoRedoMenu() override;

protected:
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

protected Q_SLOTS:
  void onSelected();
  void onStepSelected();
  void onFetchInfo();

private:
  /// The first \a rows rows taken, one undo (or redo) each
  bool takeRows(int rows);
  /// The row \a action is, or -1
  int rowOf(QAction* action) const;

  bool redo;
};

/** This class implements the undo dialog.
 * \author Werner Mayer
 */
class UndoDialog : public UndoRedoMenu
{
    Q_OBJECT

public:
  explicit UndoDialog( QWidget* parent = nullptr );
  ~UndoDialog() override;
};

/** This class implements the redo dialog.
 * \author Werner Mayer
 */
class RedoDialog : public UndoRedoMenu
{
    Q_OBJECT

public:
  explicit RedoDialog( QWidget* parent = nullptr );
  ~RedoDialog() override;
};

} // namespace Dialog
} // namespace Gui

#endif // GUI_DIALOG_DLGUNDOREDO_H
