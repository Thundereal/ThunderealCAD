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

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <FCGlobal.h>

namespace App
{

class Document;
class DocumentObject;

/** What makes the objects of a recompute up to date, other than their
 * execute() (docs/WorkerProcess.md sec 19, 20). While one is installed for
 * a document on this thread, Document::recompute() tells it before and
 * after each object it recomputes, and DocumentObject::recompute() asks it
 * in place of the object's execute(). Everything else of the recompute --
 * the order, what a class does around its execute() (a Part feature's
 * references and shape generations), the signals, the touched state, the
 * error log, the log's record -- is the recompute's own.
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
    /// how; false to have it executed. Asked again for the same object when
    /// its class retries the execute within one recompute of it
    virtual bool make(DocumentObject& obj, Made& made);
    /// Before `obj` is recomputed, made or executed
    virtual void executing(DocumentObject& obj);
    /// After it was: `res` as Document::_recomputeFeature's (0 made, 1 in
    /// error, < 0 the recompute stops)
    virtual void executed(DocumentObject& obj, int res, double seconds);

    /// Whether the recompute of `obj` now under way was made from another
    /// process's values, put in, rather than executed here
    virtual bool wasPutIn(const DocumentObject& obj) const;
    /** The same asked of the hook installed for `obj`'s document: false
     * with none. What a class does around its execute() after the values
     * are in -- a Part feature's shape generations, made again from the
     * shape put in -- came with them (docs/WorkerProcess.md sec 87)
     */
    static bool isPutIn(const DocumentObject& obj);

    /// The innermost hook installed for `doc` on this thread, or null
    static RecomputeHook* of(const Document& doc);

    /** Whether this process is not to execute `obj` at all, `why` the
     * error it is left in: an object whose execute() crashed a worker
     * process on what it reads now (docs/WorkerProcess.md sec 34). Asked by
     * Document::_recomputeFeature() before an execute; set by
     * WorkerRecompute. Not asked while a hook is installed for the
     * document: a worker's recompute, and a result being put in, decide
     * that themselves.
     */
    using Refusal = bool (*)(const DocumentObject& obj, std::string& why);
    static void setRefusal(Refusal refusal);
    static bool refuses(const DocumentObject& obj, std::string& why);

    /** What a recompute asked of this process is offered first, before it
     * runs: a recompute that reaches an object which crashed a worker
     * before -- one changed since, tried again -- is made by a worker and
     * waited for, so that a crash again costs the worker, whoever asked for
     * the recompute: a script, the console (docs/WorkerProcess.md sec 37).
     * What is left is made here as ever. Set by WorkerRecompute; not asked
     * while a hook is installed for the document. True if a worker made it
     * and its result was put in -- or, crashing again, left what crashed
     * refused here.
     */
    using Handover = bool (*)(Document& doc, const std::vector<DocumentObject*>& objs);
    static void setHandover(Handover handover);
    static bool handOver(Document& doc, const std::vector<DocumentObject*>& objs);

    /// How many numbers objects were given on this thread so far
    /// (Document::nextGeoId, nextObjectCount)
    static unsigned long minted();
    /// A number given: the count `kind` of `obj` (-1 its geometry ids) went
    /// from `before` to `after`
    static void noteMinted(const DocumentObject& obj, int kind, long before, long after);

    /** While one lives on this thread, the numbers given are noted in it: a
     * worker's recompute says which it gave, for the other process to take
     * them as they are when it has given none of its own since
     * (docs/WorkerProcess.md sec 36).
     */
    class AppExport Minting
    {
    public:
        struct Count
        {
            long from = 0;  ///< what it went from, the first time
            long to = 0;    ///< what it went to, the last
        };
        Minting();
        ~Minting();
        Minting(const Minting&) = delete;
        Minting& operator=(const Minting&) = delete;
        /// By the object and the kind
        std::map<std::pair<const DocumentObject*, int>, Count> counts;

    private:
        Minting* _outer;
    };

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
