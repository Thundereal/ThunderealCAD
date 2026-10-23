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

#include <cstddef>
#include <cstdint>
#include <map>
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
    User,       ///< the user said so (pairByHand)
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
            || pairing == SetPairing::Manifest || pairing == SetPairing::User;
    }
};

struct AppExport SetImportPlan
{
    /// Our top, whose log holds their files alone (L5).
    Document* ourTop = nullptr;
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

/** The user's pairing (sec 35 L3: "can be changed by hand"): their file
 * `theirs` paired with `ours`, or with none for null. Throws when `theirs`
 * is no file of the plan.
 */
AppExport void pairByHand(SetImportPlan& plan, const std::string& theirs, Document* ours);

/// What the set's merge does with a file (sec 35 L6).
enum class SetMergeAction
{
    Merge,        ///< its branch merged into ours
    TakeTheirs,   ///< ours made theirs, one row (Q8); ours kept in the history
    KeepOurs,     ///< ours as it is; the branch is not asked again
    Write,        ///< a file of theirs alone written where `writeTo` says
    Drop,         ///< a file of theirs alone left out
};

/// What the import of a file of their set did.
struct AppExport SetImportedFile
{
    std::string theirs;    ///< their file
    std::string relative;  ///< its path under their top's folder
    SetPairing pairing = SetPairing::None;   ///< how it was paired
    std::string ourFile;   ///< ours it came to; empty for a file of theirs alone
    /// The branch of ours it came as; empty when it brought nothing new.
    std::string branch;
    std::size_t rows = 0;
    bool independent = false;
    /// Where the import stopped, and why (Document::ImportResult).
    int64_t stoppedAt = 0;
    std::string reason;
    /// A file of theirs alone, held in our top's log (L5): the row that
    /// holds it, and where it is written to be opened, read-only.
    int64_t held = 0;
    std::string staged;
    /// Links of its branch made links to what the files of the set came
    /// as, in a row of their own: the links the copies had in common.
    int relinked = 0;
    /// It brought nothing new, but links a file of the set that did: its
    /// branch holds only the row that links what that file came as (sec
    /// 35.15 G1), and the set's merge keeps ours by default.
    bool linksOnly = false;
    /// What the set's merge does with it by default (Q8), and for a file of
    /// theirs alone where it is written: its path in their set, under our
    /// top's folder (Q2).
    SetMergeAction action = SetMergeAction::Merge;
    std::string writeTo;
    /// The import could not be made at all.
    std::string error;
};

struct AppExport SetImportResult
{
    std::vector<SetImportedFile> files;   ///< in the plan's order
    /// The set (Q9): its id, its name -- their top's file name -- and their
    /// top, as the top's record holds them.
    std::string id;
    std::string name;
    std::string theirTop;
    /// The record in our top's log that holds the set's map; 0 for none.
    int64_t record = 0;
};

/** Bring their set in (sec 35 L4): each file of the plan paired with one of
 * ours imported as a branch of it (Document::importFork), the files each
 * links first; in the rows brought, a link to a file of theirs paired with
 * ours is a link to ours, live to the branch that file came to -- or, when
 * it brought nothing new, to what the manifest says it was of ours, a
 * branch or a version, else plain. A link the copies had in common, in no
 * row brought, is made so by a row of the branch after the import. A file
 * of theirs alone is held in our top's log (L5) -- written to its transient
 * directory to be opened, and links to it made to that file, held -- for
 * the top's branch, let go when that is merged or deleted; with no branch
 * of the top, it waits as a sent file does. It is not brought,
 * and links to it stay theirs (L5). A file that cannot be imported is
 * reported, and the rest go on.
 */
AppExport SetImportResult importSet(const SetImportPlan& plan);

AppExport const char* setMergeActionName(SetMergeAction action);
/// The action a file is given by default (Q8): Merge for one paired with
/// certainty, Take theirs for one paired by a guess, Write for one of theirs
/// alone.
AppExport SetMergeAction defaultSetMergeAction(const SetImportFile& file);

/// A file of the set, as the merge takes it.
struct AppExport SetMergeFile
{
    std::string theirs;
    std::string ourFile;    ///< empty for a file of theirs alone
    std::string branch;     ///< what it came as; empty when it brought nothing new
    int64_t held = 0;       ///< a file of theirs alone, held in our top's log
    SetMergeAction action = SetMergeAction::Merge;
    std::string writeTo;    ///< Write: the full path
    /// Merge: the sides of its conflicts, as Document::mergeBranch() takes them.
    std::map<std::string, std::string> picks;
    std::string fallback;
};

struct AppExport SetMergeOptions
{
    std::string name = "Merge their set";   ///< the undo step's
    bool overwrite = false;   ///< Write over a file that is there
    /// The set merged (SetImportResult::id): our top's log says so, and it is
    /// no longer pending. Empty for none.
    std::string set;
};

struct AppExport SetMergedFile
{
    std::string theirs;
    std::string ourFile;
    SetMergeAction action = SetMergeAction::Merge;
    int64_t seq = 0;          ///< the merge's or the restore's row
    std::string written;      ///< Write: where
    int undoId = 0;           ///< the undo step it is in; 0 for none
    std::size_t unresolved = 0;
    std::vector<std::string> failed;   ///< objects whose recompute failed
    std::string error;
};

struct AppExport SetMergeResult
{
    std::vector<SetMergedFile> files;
    int transaction = 0;      ///< the one undo step's id
    /// Nothing moved: a merge has conflicts with no side, or a file to be
    /// written is there; `files` says which.
    bool refused = false;
};

/** The set's merge (sec 35 L6): `files` in the order importSet() gives them,
 * the files each links first. Before anything moves, a Merge with a
 * conflict that has no side, or a Write to a file that is there without
 * `overwrite`, refuses the whole. Then their files alone are written; each
 * branch is given a row making its links to the set's branches plain links
 * to ours, now that ours holds what they had, and its held links links to
 * the files written; and each file is merged, taken or kept, as one undo
 * step across the documents (Q3).
 */
AppExport SetMergeResult mergeSet(Document& ourTop, const std::vector<SetMergeFile>& files,
                                  const SetMergeOptions& options = {});

/** The sets imported into `ourTop` and not merged yet (sec 35 Q9), oldest
 * first, as the records of the branch it is on hold them: what importSet()
 * returned for each, with `when` it was imported. A set's dialog opened
 * again shows them.
 */
struct AppExport PendingSet
{
    SetImportResult imported;
    double when = 0;
};
AppExport std::vector<PendingSet> pendingSets(Document& ourTop);

}  // namespace App

#endif  // APP_IMPORTSET_H
