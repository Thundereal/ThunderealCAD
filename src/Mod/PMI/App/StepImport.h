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

#ifndef PMI_STEPIMPORT_H
#define PMI_STEPIMPORT_H

#include <string>
#include <vector>

#include <Mod/PMI/PMIGlobal.h>

namespace Import
{
struct PostImport;
}

namespace PMI
{

/** The GD&T of an imported STEP file, read (docs/GDandT.md sec 5.1, P2):
 * the datums, tolerances and dimensions OCCT put in the XCAF document's
 * DimTolTool, made into annotation sets beside the objects the import made,
 * on their faces. Their annotation planes and text points come from the
 * file; the graphic is drawn here.
 */
namespace Step
{

struct Report
{
    int datums = 0;
    int tolerances = 0;
    int dimensions = 0;
    int views = 0;
    int targets = 0;
    /// Hole occurrences (AP242 ed3 hole features)
    int holes = 0;
    /// PMI the file gives only as a graphic, made a note carrying it
    int notes = 0;
    /// Read twice (a streamed import may) and kept once
    int duplicates = 0;
    /// A graphic given apart from what was read with it as well: kept once
    int twice = 0;
    /// Dimensions with no semantics and no graphic either: not read
    int graphicOnly = 0;
    /// Not read, each with why
    std::vector<std::string> skipped;
    /// Read, with a value the model does not measure: the model's is kept
    std::vector<std::string> mismatched;
};

PMIExport Report read(const Import::PostImport& info);

/// Run read() after every import (ImportOCAF2::addPostImport)
PMIExport void registerImport();

}  // namespace Step
}  // namespace PMI

#endif  // PMI_STEPIMPORT_H
