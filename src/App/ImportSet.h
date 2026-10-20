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

#ifndef APP_IMPORTSET_H
#define APP_IMPORTSET_H

#include <cstdint>
#include <string>
#include <vector>

#include <FCGlobal.h>

namespace App
{
class Document;

/** How a file of their set was paired with one of ours
 * (docs/TransactionLog.md sec 35 L3, 35.6 Q1), strongest first.
 */
enum class SetPairing
{
    History,    ///< a history both hold (forkBase)
    SaveId,     ///< the save of ours a copy without history names
    Manifest,   ///< our export's manifest says so (L7)
    Uid,        ///< the Uid in their Document.xml: guessed
    Path,       ///< the same path relative to the top: guessed
    None,       ///< a file of theirs alone
};

AppExport const char* setPairingName(SetPairing pairing);

/// A file of their set, and what of ours it is paired with.
struct AppExport SetImportFile
{
    std::string theirs;     ///< their file, its full path
    std::string relative;   ///< relative to their top's folder
    std::string uid;        ///< their Document.xml's Uid
    /// Their files it links, full paths, as their set has them.
    std::vector<std::string> links;

    Document* ours = nullptr;   ///< the document of ours it is paired with
    std::string ourFile;        ///< and its file
    SetPairing pairing = SetPairing::None;
    /// Two or more of ours answered alike: none is paired, the user says.
    bool ambiguous = false;
    /// What the manifest says it is of ours, when it was a version or a
    /// branch's document written as a file of its own (sec 35.6 Q5).
    std::string branch;
    int64_t version = 0;

    /// Paired by a signal that cannot be wrong (Q1 items 1 to 3).
    bool certain() const
    {
        return pairing == SetPairing::History || pairing == SetPairing::SaveId
            || pairing == SetPairing::Manifest;
    }
};

struct AppExport SetImportPlan
{
    /// Their set, the files each links before it, their top last.
    std::vector<SetImportFile> files;
    /// Files their set links that are ours already -- the same file on
    /// disk, a library both use: nothing of them to bring.
    std::vector<std::string> shared;
    /// Files their set links that are not there.
    std::vector<std::string> missing;
};

/** Their set and how its files pair with ours (sec 35 L2, L3), with nothing
 * imported: the files `theirTop`'s links reach, transitively, read from the
 * files, and ours -- the documents `ourTop`'s links reach -- each of theirs
 * paired by the first of 35.6 Q1's signals that answers. Throws when
 * `theirTop` cannot be read, or is our top's own file.
 */
AppExport SetImportPlan planSetImport(Document& ourTop, const std::string& theirTop);

}  // namespace App

#endif  // APP_IMPORTSET_H
