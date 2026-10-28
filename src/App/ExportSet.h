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

#ifndef APP_EXPORTSET_H
#define APP_EXPORTSET_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <FCGlobal.h>

namespace App
{
class Document;

/** A set of linked files written to a folder (docs/TransactionLogJournal.md sec 35
 * L7, "pack and go"): the top document and every document its links reach,
 * each as a file of its own, the links between them written to those files.
 */
struct AppExport ExportSetOptions
{
    /// Each file with its history, for someone who also runs the log;
    /// without, a copy keeps its save id (sec 30.19 G2).
    bool withHistory = false;
    /// Write into a folder that is not empty.
    bool overwrite = false;
    /// A file in a library, reached only by library links, is left there:
    /// the copies link it in the library form, for a recipient with the
    /// same library (docs/LibraryPaths.md 3.8). Without, it is copied under
    /// `_external/` as any file outside the top's folder.
    bool keepLibraries = false;
};

struct AppExport ExportSetFile
{
    std::string path;     ///< written, relative to the folder; empty for a file left in its library
    std::string library;  ///< the library a file left there is in
    std::string libraryFile;  ///< and its path under the library's folder
    std::string file;     ///< the file here it is of
    std::string branch;   ///< for a branch's document
    int64_t version = 0;  ///< for a version's document
    std::string saveId;   ///< the copy's, as written
    std::string uid;      ///< the document's Uid
    Document* document = nullptr;   ///< the document, while it is open
};

struct AppExport ExportSetResult
{
    std::vector<ExportSetFile> files;   ///< the top first, the files left in a library last
    std::string manifest;               ///< the manifest's full path
};

/** What exportLinkedSet() would write of `top`'s set, and under which
 * names, with nothing written: every document of the set is read whole
 * first, as the export reads it. Throws as the export does for a document
 * of the set with no file. Of `options`, only `keepLibraries` is read.
 */
AppExport ExportSetResult planLinkedSet(Document& top, const ExportSetOptions& options = {});

/// The manifest's file name in the folder.
AppExport const char* exportSetManifestName();

/** Write `top` and the documents its links reach into `folder`. Throws when
 * the folder is not empty (unless `overwrite`), when a document of the set
 * has no file, or when a copy cannot be written.
 */
AppExport ExportSetResult exportLinkedSet(Document& top, const std::string& folder,
                                          const ExportSetOptions& options = {});

/** While a set's copies are written: a link of the copy being written to a
 * document of the set names that document's copy, as a plain link -- no
 * version, no branch, no library -- so the set opens in any FreeCAD.
 */
class AppExport ExportSetPaths
{
public:
    explicit ExportSetPaths(std::map<const Document*, std::string> files);
    ~ExportSetPaths();
    ExportSetPaths(const ExportSetPaths&) = delete;
    ExportSetPaths& operator=(const ExportSetPaths&) = delete;

    /// The copy of `doc` is the one being written now.
    static void writing(const Document* doc);
    /// The path a link of `owner`'s copy gives `target`'s; empty when no
    /// set is being written, `owner` is not the copy being written, or
    /// `target` is not of the set.
    static std::string pathTo(const Document* owner, const Document* target);
    /// Whether `owner`'s copy is the one being written now.
    static bool writingCopyOf(const Document* owner);

private:
    std::map<const Document*, std::string> _files;
    const Document* _writing = nullptr;
    ExportSetPaths* _outer;
};

}  // namespace App

#endif  // APP_EXPORTSET_H
