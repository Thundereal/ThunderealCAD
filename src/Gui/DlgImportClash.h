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

#ifndef GUI_DLG_IMPORT_CLASH_H
#define GUI_DLG_IMPORT_CLASH_H

#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <App/Document.h>
#include <FCGlobal.h>

namespace Gui
{

/** Objects brought into a document -- pasted, merged, dropped -- of which a
 * live object there has the UID (docs/TransactionLogJournal.md sec 36.4): a list,
 * each row Assign (the live object takes the state brought) or Duplicate (a
 * new object, as a paste was), with Assign all and Duplicate all. Sets each
 * clash's `assign`; false when cancelled.
 */
/// `copyLook`, when given: whether what is assigned takes the look brought
/// too (sec 35.18), as the dialog's check says it
GuiExport bool askImportClashes(std::vector<App::Document::ImportClash>& clashes,
                                bool* copyLook = nullptr);

/** What a paste into `doc` takes of the objects `brought` -- objects of
 * another document: a scratch import of what is pasted, or the source of a
 * copy -- asked before anything is imported (docs/TransactionLogJournal.md sec
 * 37.9, 37.7 item 13). Each is paired with `doc`'s object of its UID and
 * type; with none paired nothing is asked and `takes` stays empty. Asked in
 * the dependency dialog (DlgObjectSelection::setPaste): a check leaves an
 * object out, a second assigns it. `takes` names each object by its name in
 * its own document; `copyLook`, when given, says whether what is assigned
 * takes the look brought too (sec 35.18), checked by default. False when
 * cancelled.
 */
GuiExport bool askPaste(App::Document* doc, const std::vector<App::DocumentObject*>& brought,
                        std::map<std::string, App::Document::ImportTake>& takes,
                        bool* copyLook = nullptr);

/** A paste into `doc` of the stream `open()` gives, asked first (sec 37.9,
 * 37.7 item 13 (c)): the stream's UIDs are read off it; where one names an
 * object of `doc`, the stream is imported into a scratch document and
 * askPaste() asks of that. Then `transaction` is opened in `doc` and the
 * stream imported with the answers, into `imported`. False when cancelled,
 * with nothing imported and no transaction opened; an import that throws
 * leaves the transaction open for the caller to abort.
 */
GuiExport bool pasteAsked(App::Document* doc,
                          const std::function<std::unique_ptr<std::istream>()>& open,
                          const char* transaction,
                          std::vector<App::DocumentObject*>& imported);

/// What the user command brings is asked about (sec 36.4), for its scope.
class GuiExport ImportClashAsk: public App::Document::ImportClashScope
{
public:
    ImportClashAsk();
    /// Every clash the last import asked about was assigned (a macro line
    /// can say so), and there was one.
    bool allAssigned() const { return _all; }

private:
    bool _all {false};
};

}  // namespace Gui

#endif  // GUI_DLG_IMPORT_CLASH_H
