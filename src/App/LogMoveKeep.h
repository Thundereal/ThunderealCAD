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

#ifndef APP_LOG_MOVE_KEEP_H
#define APP_LOG_MOVE_KEEP_H

#include <cstdint>
#include <functional>
#include <string>

#include <FCGlobal.h>

namespace App
{

class Document;

/** Values a move along the log leaves as the document holds them
 * (docs/WorkerProcess.md sec 29). While one is installed for a document on
 * this thread, Document::_moveAlongLog() asks it of each value a recompute
 * wrote that the move would put in -- by its object's id, its property and
 * the row that last wrote it -- and leaves the value as it is where it
 * says so: a worker moving across the rows its own result was put in as
 * holds those values already, as it made them, and the log's copy of
 * them, made from them in the other process, is not byte for byte the
 * same (a location written again after a read) though it is the same
 * value.
 */
class AppExport LogMoveKeep
{
public:
    using Keep = std::function<bool(long cid, const std::string& prop, int64_t txn)>;

    LogMoveKeep(const Document& doc, Keep keep);
    ~LogMoveKeep();
    LogMoveKeep(const LogMoveKeep&) = delete;
    LogMoveKeep& operator=(const LogMoveKeep&) = delete;

    /// The one installed for `doc` on this thread, or null
    static LogMoveKeep* of(const Document& doc);

    bool keeps(long cid, const std::string& prop, int64_t txn) const
    {
        return keep && keep(cid, prop, txn);
    }

    /// How many values the move left as they were
    long kept = 0;

private:
    const Document& doc;
    Keep keep;
    LogMoveKeep* outer;
};

}  // namespace App

#endif  // APP_LOG_MOVE_KEEP_H
