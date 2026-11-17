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

#ifndef APP_RECOMPUTE_HOOK_H
#define APP_RECOMPUTE_HOOK_H

#include <string>

#include <FCGlobal.h>

namespace App
{

class Document;
class DocumentObject;

/** What makes the objects of a recompute up to date, other than their
 * execute() (docs/WorkerProcess.md sec 19). While one is installed for a
 * document on this thread, Document::recompute() asks it before each
 * object it would execute, and tells it after each one it did execute.
 * Everything else of the recompute -- the order, the signals, the touched
 * state, the error log, the log's record -- is the recompute's own.
 *
 * A worker installs one that notes what each execute() wrote; the process
 * that asked installs one that puts those values in instead of executing.
 * Installed for the scope of the object, innermost first; not for another
 * document, nor another thread.
 */
class AppExport RecomputeHook
{
public:
    /// How the object came out, as the hook made it
    struct Made
    {
        bool ok = true;
        std::string error;  ///< the message, when not ok
        double seconds = 0;  ///< its execute()'s time where it ran
    };

    explicit RecomputeHook(const Document& doc);
    virtual ~RecomputeHook();
    RecomputeHook(const RecomputeHook&) = delete;
    RecomputeHook& operator=(const RecomputeHook&) = delete;

    /// Make `obj` instead of its execute(): true if it did, `made` saying
    /// how; false to have it executed
    virtual bool make(DocumentObject& obj, Made& made);
    /// Before `obj`'s own execute() runs, asked by make() returning false
    virtual void executing(DocumentObject& obj);
    /// After it ran: `res` as Document::_recomputeFeature's (0 made, 1 in
    /// error, < 0 the recompute stops)
    virtual void executed(DocumentObject& obj, int res, double seconds);

    /// The innermost hook installed for `doc` on this thread, or null
    static RecomputeHook* of(const Document& doc);

    /// How many numbers objects were given on this thread so far
    /// (Document::nextGeoId, nextObjectCount): a recompute in a worker that
    /// gave one cannot be taken in by another process as it is
    static unsigned long minted();
    static void noteMinted();

    /** While one lives on this thread, the values restored are another
     * process's results put in, checked where they were made: what a
     * value's own check on setting would find (a shape's validity, its
     * fix) came with it, as with a value an undo puts back.
     */
    class AppExport PuttingIn
    {
    public:
        PuttingIn();
        ~PuttingIn();
        PuttingIn(const PuttingIn&) = delete;
        PuttingIn& operator=(const PuttingIn&) = delete;
    };
    static bool puttingIn();

private:
    const Document* _doc;
    RecomputeHook* _outer;
};

}  // namespace App

#endif  // APP_RECOMPUTE_HOOK_H
