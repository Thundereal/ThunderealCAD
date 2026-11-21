// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
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

#ifndef PMI_STEPEXPORT_H
#define PMI_STEPEXPORT_H

#include <string>
#include <vector>

#include <Mod/PMI/PMIGlobal.h>

namespace Import
{
struct PostExport;
}

namespace PMI
{
namespace Step
{

/** What an export wrote of the annotations of the parts exported */
struct WriteReport
{
    int datums = 0;
    int targets = 0;
    int tolerances = 0;
    int dimensions = 0;
    int notes = 0;
    /// Not written, each with why
    std::vector<std::string> skipped;
    /// Written without something STEP, or OCCT, has no place for
    std::vector<std::string> partly;
};

/** The GD&T of the parts an export wrote, into its XCAF document
 * (docs/GDandT.md sec 5.2, P3): every annotation set whose part was exported
 * -- its datums and datum targets, tolerances with their datum systems,
 * dimensions and notes, on the exported faces, each with its plane, its text
 * point and its graphic, tessellated. A writer then writes them; STEP's
 * writes them AP242 (Import::WriterStep).
 */
PMIExport WriteReport write(const Import::PostExport& info);

/// Run write() at the end of every export (ExportOCAF2::addPostExport)
PMIExport void registerExport();

}  // namespace Step
}  // namespace PMI

#endif  // PMI_STEPEXPORT_H
