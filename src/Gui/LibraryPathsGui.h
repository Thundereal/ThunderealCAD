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

#ifndef GUI_LIBRARY_PATHS_GUI_H
#define GUI_LIBRARY_PATHS_GUI_H

#include <string>
#include <vector>

#include <QGroupBox>

#include <FCGlobal.h>

class QTableWidget;

namespace Gui
{

/** The library paths of the settings as a table, for the Document
 * preference page (docs/LibraryPaths.md 3.9): name, folder, browse; add and
 * remove. save() commits the change, and the open documents it affects are
 * asked about (Q8).
 */
class GuiExport LibraryPathsEditor: public QGroupBox
{
public:
    explicit LibraryPathsEditor(QWidget* parent = nullptr);

    void load();
    void save();

private:
    void addRow(const QString& name, const QString& folder);
    void browse(int row);

    QTableWidget* table;
};

namespace LibraryPathsGui
{

/// Connect to the App signals: the question on a committed change of the
/// setting (Q8), and the report of the documents opened (3.9).
GuiExport void init();

/** Ask whether to apply the setting to the open documents whose library
 * links it resolves differently, listing them; apply to all if so. Nothing
 * is asked when there are none.
 */
GuiExport void askToApply(QWidget* parent = nullptr);

/** Show what `docNames`' library links met at the open -- a name resolved
 * through no folder, the document's folder taken where the settings'
 * differs -- with the actions for each; nothing when they met nothing.
 */
GuiExport void reportOpened(const std::vector<std::string>& docNames);

}  // namespace LibraryPathsGui

}  // namespace Gui

#endif  // GUI_LIBRARY_PATHS_GUI_H
