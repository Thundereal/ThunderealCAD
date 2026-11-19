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

#ifndef APP_WORKER_RECOMPUTE_H
#define APP_WORKER_RECOMPUTE_H

#include <cstddef>
#include <string>
#include <vector>

#include <fastsignals/signal.h>

#include <FCGlobal.h>

namespace App
{

class Document;
class DocumentObject;

/** A document's recompute made by its worker processes, the second client of
 * the pool (docs/WorkerProcess.md sec 19). start() hands what a recompute
 * would reach to a worker, which recomputes it on its copy and writes what
 * each execute() wrote; this process carries on meanwhile. When the result
 * comes, a recompute is run here in which each object is made from the
 * worker's values instead of executing (RecomputeHook): the signals, the
 * touched state, the errors and the log's record are a recompute's.
 *
 * Nothing depends on it for correctness: a recompute the workers did not
 * make leaves the document touched, as it was. A change of what the job
 * reads drops it; a recompute run here meanwhile takes its result if it
 * came, waits for it if it is being made, and else does it itself.
 */
class AppExport WorkerRecompute
{
public:
    struct Info
    {
        bool running = false;  ///< with a worker, or come and not put in yet
        long started = 0;      ///< handed to workers, this document
        long applied = 0;      ///< of them, put in here
        long borrowed = 0;     ///< of them, taken by a recompute run here
        long cancelled = 0;    ///< dropped: the design changed, or cancel()
        long failed = 0;       ///< the worker could not make it
        long unfit = 0;        ///< the worker cannot make it faithfully
        // Of the last one put in:
        long objects = 0;      ///< what it asked for
        long supplied = 0;     ///< objects made from the worker's values
        long executedHere = 0; ///< objects executed here all the same
        long madeHere = 0;     ///< of the others, executed here by their type
        long unused = 0;       ///< objects the worker made that were not asked
        /// The first of those executed here, by name, for a look
        std::vector<std::string> executedHereNames;
        bool byId = false;     ///< its strings taken in under their ids
        std::string byContentWhy;  ///< why not, when they were read by content
        double workerSeconds = 0;  ///< the worker's recompute
        double applySeconds = 0;   ///< putting it in here, the views included
        std::size_t bytes = 0;     ///< its file
        /// Why the last one was not started, made or put in
        std::string reason;
    };

    /** Hand the recompute of `objs` -- with none, of what is touched -- and
     * of everything depending on them to the document's workers. False,
     * `why` set, when it cannot be: no workers, nothing to recompute, one
     * with the workers already.
     */
    static bool start(Document& doc, const std::vector<DocumentObject*>& objs = {},
                      std::string* why = nullptr);
    /// Whether one is with the workers, or has come and is not put in yet
    static bool running(const Document& doc);
    /// Take in what comes, putting in a result that came, until none is
    /// running or `seconds` pass (< 0: no end); true if none is
    static bool wait(Document& doc, double seconds = -1);
    /// Drop the one running, its result not put in
    static void cancel(Document& doc, const std::string& why);
    static Info info(const Document& doc);

    /** What the recompute of `objs` -- with none, of what is touched --
     * would take here, in seconds (docs/WorkerProcess.md sec 28): what each
     * object it reaches took when it was last made, here or by a worker,
     * and for one not made since trackTimes() began, the most it took in
     * the log's recompute records (sec 30). `unknown`, if given, counts the
     * objects it reaches with neither, which count for nothing.
     */
    static double expectedSeconds(const Document& doc,
                                  const std::vector<DocumentObject*>& objs = {},
                                  long* unknown = nullptr);
    /// Note from now what each object's recompute here takes, for
    /// expectedSeconds(); once is enough
    static void trackTimes();

    /// In a worker: whether its last recompute job sent the value of
    /// `prop` of `obj` (docs/WorkerProcess.md sec 29)
    static bool sent(const std::string& obj, const std::string& prop);

    /// Started, made, put in, dropped
    static fastsignals::signal<void(const Document&)> signalChanged;
};

}  // namespace App

#endif  // APP_WORKER_RECOMPUTE_H
