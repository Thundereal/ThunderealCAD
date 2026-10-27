// SPDX-License-Identifier: LGPL-2.1-or-later

/****************************************************************************
 *   Copyright (c) 2018-2022 Zheng, Lei (realthunder)                       *
 *   <realthunder.dev@gmail.com>                                            *
 *   Copyright (c) 2023 FreeCAD Project Association                         *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#ifndef DATA_ELEMENTMAP_H
#define DATA_ELEMENTMAP_H

#include "FCGlobal.h"

#include "Application.h"
#include "MappedElement.h"
#include "StringHasher.h"

#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>


namespace Data
{

class ElementMap;
using ElementMapPtr = std::shared_ptr<ElementMap>;

struct AppExport MappedChildElements
{
    IndexedName indexedName;
    int count;
    int offset;
    long tag;
    ElementMapPtr elementMap;
    QByteArray postfix;
    ElementIDRefs sids;
};

/** `map`, whose names are of the table `from`, with the ids of `to`
 * (docs/TransactionLogJournal.md sec 27.76 item 2, 27.77): every name, held id,
 * child postfix and child map imported (StringHasher::importName). A copy,
 * cached per source map and target table while both live, so the shape a
 * link makes on demand, and every link to the same object, share one.
 * `map` itself when `from` or `to` is null or they are the same table.
 */
AppExport ElementMapPtr translateElementMap(const ElementMapPtr &map,
                                            const App::StringHasherRef &from,
                                            const App::StringHasherRef &to);

/** Element map ids of their own (docs/TransactionLogJournal.md sec 27.67).
 *
 * A document's save numbers its element maps (beforeSave) so a map shared
 * by several shapes is written once, and its restore reads each id once;
 * both tables are process-wide and reset by the save and restore signals.
 * A value the transaction log captures or restores is neither: its Save
 * runs without beforeSave, and wrote whatever id the last save left -- 0
 * for a map never saved -- and its restore found the map an earlier value
 * left under that id, and took it instead of its own. While one of these
 * lives on a thread, element maps there are numbered and read in tables of
 * its own, and the process-wide ones are not touched (a capture runs on the
 * log's worker while the main thread may be saving).
 */
class AppExport ElementMapIdScope
{
public:
    ElementMapIdScope();
    ~ElementMapIdScope();
    ElementMapIdScope(const ElementMapIdScope&) = delete;
    ElementMapIdScope& operator=(const ElementMapIdScope&) = delete;
    /// The innermost scope on this thread, or null.
    static ElementMapIdScope* current();
    /// The id `map` is saved under in this scope, given on first use.
    unsigned idOf(const ElementMap* map);
    /// The map restored under `id` in this scope, or null to fill.
    ElementMapPtr& restored(unsigned id);

private:
    std::unordered_map<const ElementMap*, unsigned> _toId;
    std::unordered_map<unsigned, ElementMapPtr> _fromId;
    ElementMapIdScope* _outer;
};

/** Start a save's numbering of element maps (docs/TransactionLogJournal.md sec
 * 38.15): forget which map this process's last save numbered how, so that
 * each map is given the id it keeps again. A document's save to its file
 * does this through the save signals; Document::Save calls it too, for the
 * saves no signal brackets (the auto saver's recovery file).
 */
AppExport void startElementMapNumbering();

/** The ids of the element maps a save numbers (beforeSave), writes, or a
 * restore reads on this thread while one of these lives, into `ids`
 * (docs/TransactionLogJournal.md sec 38.14). A document's save numbers its maps
 * across all its objects, so what an object's file says of them depends on
 * the others: a folder save keeps the file only if this save numbers the
 * object's maps as the file does. Not inside an ElementMapIdScope, whose
 * ids are its own.
 */
class AppExport ElementMapIdCollector
{
public:
    explicit ElementMapIdCollector(std::vector<unsigned>& ids);
    ~ElementMapIdCollector();
    ElementMapIdCollector(const ElementMapIdCollector&) = delete;
    ElementMapIdCollector& operator=(const ElementMapIdCollector&) = delete;
    /// Record `id`, when one lives on this thread.
    static void note(unsigned id);

private:
    std::vector<unsigned>* _outer;
};

}// namespace Data

#endif// DATA_ELEMENTMAP_H
