// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <functional>
#include <string>

#include <FCGlobal.h>

namespace App::CacheSweep
{

/// What one sweep did
struct Counts
{
    int readers = 0;  ///< reader folders removed
    int documents = 0;  ///< empty document folders removed
    int failed = 0;  ///< folders that would not go
};

/// Removes from the user cache \a cache the folders of processes that no
/// longer run and that cannot hold anything to recover
/// (docs/WorkerProcess.md sec 49): a store's reader folder
/// `<exe>_Reader_<uuid>_<pid>`, and an empty document folder
/// `<exe>_Doc_<uuid>_<hash>_<pid>`. A document folder with anything in it
/// is left for the recovery to judge. Only folders last changed before
/// \a before (seconds since the epoch) are looked at, so a folder a
/// process makes while this runs is never one of them. \a stopped, when it
/// says so, ends the sweep at the next folder.
AppExport Counts run(const std::string& cache,
                     const std::string& exe,
                     double before,
                     const std::function<bool()>& stopped = {});

/// The sweep of this application's user cache, on the shared thread pool,
/// ending at the next folder when the application goes. Not in a worker:
/// its pool removes its folders (sec 48), and a pool starts many.
AppExport void start();

}  // namespace App::CacheSweep
