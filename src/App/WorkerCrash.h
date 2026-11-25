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

#ifndef APP_WORKERCRASH_H
#define APP_WORKERCRASH_H

#include <string>
#include <vector>

#include <FCGlobal.h>

namespace App
{

/** A worker's crash, told to the pool (docs/WorkerProcess.md sec 35).
 *
 * In the worker a handler of the crash signals writes, with nothing that
 * allocates, what it can to its log -- the signal, where in the job it was,
 * the frames of the thread that crashed, each with its image and the image's
 * base, and every Python thread's traceback -- and ends the process. The
 * pool reads that back from the log when the worker goes, names the frames,
 * and puts it in the application's crash log and the job's result.
 */
namespace WorkerCrash
{

/// In a worker, first thing: its handler installed
AppExport void install();
/// In a worker: where in the job it is, for the handler to say (Request::at)
AppExport void setAt(const std::string& where);

struct Frame
{
    unsigned long long address = 0;
    unsigned long long base = 0;  ///< where its image is loaded, 0 unknown
    std::string image;            ///< the image's path, empty unknown
    std::string symbol;           ///< demangled when read, empty unknown
    unsigned long long offset = 0;  ///< from the symbol
    std::string line;             ///< "File.cpp:12", by resolveLines()
};

struct Report
{
    bool found = false;
    int signal = 0;
    int code = 0;  ///< si_code; on Windows the exception's code, 0 for an abort
    long sender = 0;  ///< the pid that sent it, for one sent (kill)
    unsigned long long address = 0;  ///< si_addr
    std::string at;
    std::vector<Frame> frames;
    std::string python;  ///< the threads' tracebacks, as Python says them
    bool whole = false;  ///< the handler got to its end
};

/// The last crash a worker's log tells of, `found` false for none
AppExport Report read(const std::string& log);
/// The frames given their file and line where a tool can (atos on macOS,
/// addr2line elsewhere): seconds, not for a thread that must not wait
AppExport void resolveLines(Report& report);
/// "signal 11: Segmentation fault"
AppExport std::string signalName(int sig);
/// What one fault was, in a line: "signal 11: Segmentation fault, at address
/// 0x0" or "..., sent by process 12"
AppExport std::string what(const Report& report);
/// The report as the crash log has it
AppExport std::string text(const Report& report);

}  // namespace WorkerCrash

}  // namespace App

#endif  // APP_WORKERCRASH_H
