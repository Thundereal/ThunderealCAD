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

#ifndef APP_LIBRARY_PATHS_H
#define APP_LIBRARY_PATHS_H

#include <map>
#include <string>
#include <vector>

#include <fastsignals/signal.h>
#include <FCGlobal.h>

namespace App
{

class Document;
class PropertyXLink;

/** Library paths (docs/LibraryPaths.md): user-defined names for the folders
 * external links point into.
 *
 * The system's names are a setting; a document keeps a copy of the ones its
 * links use (Document::LibraryPaths), and a link into one is saved as the
 * name and the path under it (`lib` and `libfile`) beside its old path. A
 * link resolves through the document's folder for the name, then the
 * system's, then its old path.
 */
class AppExport LibraryPaths
{
public:
    /// name -> folder
    using Map = std::map<std::string, std::string>;

    /** @name The system's setting (3.1)
     * `User parameter:BaseApp/Preferences/Document/LibraryPaths`, one string
     * per name. The setters write only: a caller that commits a change
     * calls changed() with what was there before (Q8).
     */
    //@{
    static Map getSystem();
    /// Throws Base::ValueError for a bad name or a folder that is not absolute.
    static void setSystem(const std::string& name, const std::string& folder);
    static void removeSystem(const std::string& name);
    //@}

    /// `[A-Za-z][A-Za-z0-9_]*`
    static bool isValidName(const std::string& name);
    /// Absolute, cleaned, `~` expanded; throws Base::ValueError otherwise.
    static std::string cleanFolder(const std::string& folder);

    /** @name The token form (3.3): `%Name%/path/under/it`
     * The display and API form of a library link; the file keeps the two
     * halves as attributes.
     */
    //@{
    static std::string makeToken(const std::string& name, const std::string& file);
    /// Whether `path` is in the token form, and its halves if so.
    static bool parseToken(const std::string& path, std::string& name, std::string& file);
    /** The full path of a token, through `doc`'s folders then the system's
     * (3.6 steps 1 and 2); empty when neither holds it. A path that is no
     * token comes back as it is.
     */
    static std::string resolveToken(const std::string& path, const Document* doc = nullptr);
    //@}

    /// A link's target matched to a library (3.4).
    struct Match
    {
        std::string name;
        std::string folder;
        std::string file;
        /// The name is not the document's yet: added to its map when taken.
        bool added = false;
        /// A new name, for the system's folder under a name the document
        /// maps elsewhere (Q4).
        bool renamed = false;
    };
    /** Match `fullPath` against the document's folders, then the system's
     * (3.4): the deepest wins, a folder under two names takes the
     * document's, a name the document maps elsewhere is given the next free
     * `_<n>`. No match when `ownerFile` is inside a library that holds the
     * target (Q5).
     */
    static bool match(const std::string& fullPath,
                      const Map& docMap,
                      const Map& sysMap,
                      const std::string& ownerFile,
                      Match& out);

    /// A library link resolved through a folder (3.6).
    struct Resolved
    {
        std::string path;
        std::string folder;
        bool fromDocument = false;
        /// Both the document's and the system's folder hold the file and
        /// they differ: the document's was taken (Q2); `other` is the system's.
        bool conflict = false;
        std::string other;
    };
    /// Resolve `name`/`file` through the document's folder, then the system's.
    static bool resolve(const std::string& name,
                        const std::string& file,
                        const Map& docMap,
                        const Map& sysMap,
                        Resolved& out);

    /** @name Open documents and a change (3.7, Q8)
     */
    //@{
    /// An open document a change resolves links of differently.
    struct Affected
    {
        App::Document* document = nullptr;
        int links = 0;
    };
    /// The open documents whose library links the current setting
    /// resolves differently from what they resolve to now.
    static std::vector<Affected> affected();
    /** A change of the setting committed: `before` is what it was. Returns
     * affected() as the change left them, and emits signalChanged; the
     * documents are left as they are -- the Gui asks, and applies to them
     * if the user says so.
     */
    static std::vector<Affected> changed(const Map& before);
    /** Take the setting's folders into `doc` for the names whose links it
     * resolves differently: the document's map is remapped (3.7), one
     * undo step. Returns the number of links resolved again.
     */
    static int apply(App::Document& doc);
    /// The Q2 action: the document's folder for `name` set to the system's.
    static int useSystemFolder(App::Document& doc, const std::string& name);
    /** Resolve `doc`'s library links again after its map changed: those
     * whose folder now gives another file are linked to it. Returns how
     * many.
     */
    static int remap(App::Document& doc);
    /// Emitted by changed(): before, after.
    static fastsignals::signal<void(const Map&, const Map&)>& signalChanged();
    //@}

    /// What a document's library links met at its open (3.6, 3.9).
    struct Problem
    {
        std::string name;
        /// The document's folder for the name; empty when it has none.
        std::string documentFolder;
        /// The settings' folder for the name; empty when they have none.
        std::string systemFolder;
        /// Resolved through neither folder (3.6 step 3 or 4); otherwise both
        /// hold the files and differ, and the document's was taken (Q2).
        bool unresolved = false;
        int links = 0;
    };
    /// One per name that resolved through no folder, or through the
    /// document's where the settings' differs and holds the file too.
    static std::vector<Problem> problems(const App::Document& doc);

    /** The library links of `doc`'s objects. */
    static std::vector<PropertyXLink*> getLinks(const App::Document& doc);

    /** The names `doc`'s links use, each with the folder they resolved
     * through here (empty for one that resolved through none).
     */
    static Map getUsed(const App::Document& doc);

    /** Make `doc`'s map hold the folder each name resolved through here
     * (Q3), without a change of the document: a name its links resolve
     * through two folders keeps the one the map has, the other takes a new
     * name (Q4). With `prune`, the names no link uses are dropped (3.2), as
     * at a save. `extra` is a link being set, not yet among the document's.
     */
    static void settle(App::Document& doc, bool prune, PropertyXLink* extra = nullptr);

    /** A saved link value as the transaction log's merge compares it
     * (docs/LibraryPaths.md 3.8): in each `<XLink>` saved with a library,
     * the old path and the stamp are left out -- they say where the library
     * was on the machine that wrote it, and when the file was saved there,
     * not what is linked. Other text comes back as it is.
     */
    static std::string comparableSaved(const std::string& fragment);

    /// Whether `path` is under `folder`, as written or canonical (3.5).
    static bool isUnder(const std::string& path, const std::string& folder);

    /// `fullPath`, cleaned, for comparing two paths of one file.
    static std::string normalize(const std::string& path);
    static bool samePath(const std::string& a, const std::string& b);
};

}  // namespace App

#endif  // APP_LIBRARY_PATHS_H
