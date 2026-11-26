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

#ifndef APP_SERVICE_WORKERS_H
#define APP_SERVICE_WORKERS_H

#include <functional>
#include <string>
#include <vector>

#include <FCGlobal.h>

namespace App
{

/** Processes that do one kind of work for this one, with no document
 * (docs/WorkerProcess.md sec 65).
 *
 * A WorkerPool's workers hold a copy of a document and move along its log.
 * What is asked of these needs nothing of a document -- a shape to mesh is
 * sent whole -- so they hold none, are this process's rather than a
 * document's, and no change of a design reaches them. They are for work
 * whose crash or hang should not be this process's: a helper that dies
 * making a call, or runs past its time and is ended, is reported to the
 * caller, and another is started for the next call.
 *
 * call() blocks the thread it is made on, any thread, until the reply, the
 * helper's death or the timeout. Up to ServiceWorkers of
 * Preferences/Workers helpers (0, the default: one for each thread the
 * machine runs at once) serve the calls of several threads at once, each
 * owned by a thread of its own here. ServiceWorkersKept (2) of them stay
 * however long they are idle; the processes of the others end once idle
 * for ServiceIdleSeconds (30), and are started again when wanted
 * (docs/WorkerProcess.md sec 71).
 */
class AppExport ServiceWorkers
{
public:
    struct Reply
    {
        bool ok = false;
        /// The helper died making it, or ran past the timeout and was ended
        bool died = false;
        /// Why not ok; for a death how it ended, and the crash's first frames
        std::string error;
        std::string body;     ///< the handler's
        double seconds = 0;   ///< the call, end to end, on this process's clock
        double handler = 0;   ///< the handler's own time, as the helper says
        double started = 0;   ///< of `seconds`, a helper started for it
    };
    /// In a helper: what a kind does with a request, false with `error`
    /// set for a failure
    using Handler =
        std::function<bool(const std::string& request, std::string& reply, std::string& error)>;
    /// A kind a helper runs; registered by the module the call names
    static void registerKind(const std::string& kind, Handler handler);

    /// Have a helper do `kind` with `request`. `module`, if not empty, is
    /// imported by the helper before its first call of the kind
    static Reply call(const std::string& kind, const std::string& module,
                      const std::string& request, double timeoutSeconds);

    struct Info
    {
        int helpers = 0;   ///< running
        int spawned = 0;   ///< ever
        int limit = 0;     ///< the most at once (ServiceWorkers, 0 for one a thread)
        long idleEnded = 0;  ///< ended idle, past ServiceWorkersKept (sec 71)
        long calls = 0;
        long died = 0;
        double seconds = 0;  ///< all calls', end to end
        double handler = 0;  ///< all calls', the handlers' own
        std::size_t sent = 0;      ///< bytes of requests
        std::size_t received = 0;  ///< bytes of replies
        std::vector<long> pids;
        std::string directory;  ///< the helpers' logs
    };
    static Info info();
    /// On the application's thread, as it starts: the parameters the helpers
    /// are started by read there, and read again there as they change. A
    /// call comes on any thread, and a parameter group is not to be read
    /// beside the thread that writes it (docs/WorkerProcess.md sec 75)
    static void init();
    /// End the helpers; a call after starts them again
    static void stop();
    /// The helper's loop (FreeCAD._serveService()): its exit code
    static int serve();
};

}  // namespace App

#endif  // APP_SERVICE_WORKERS_H
