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

#pragma once

#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <Mod/PartDesign/PartDesignGlobal.h>
#include <Mod/Spreadsheet/App/Sheet.h>

namespace PartDesign
{

/** A sheet's bend table, as a drawing shows it (docs/SheetMetal.md sec 5.6,
 * ruled 2026-11-05): a spreadsheet of its own, filled from the bends of
 * Source, a sheet metal solid or a feature of one (sheetBends()): a header
 * row, then a row a bend -- its name, the feature that made it, its angle,
 * radius, K, allowance and which way it lies flat.
 *
 * AutoSync fills it at each recompute; off, sync() does when asked, but for
 * its first fill, at its first recompute. It is
 * edited as any spreadsheet: a cell the user changed since the table wrote
 * it stays theirs, and a cell the table never wrote is the user's -- a
 * column of notes beside it; sync(true) takes every cell of its own back.
 * Generated keeps what it wrote in each cell. TechDraw's spreadsheet view
 * draws it on a page.
 */
class PartDesignExport SheetMetalBendTable: public Spreadsheet::Sheet
{
    PROPERTY_HEADER_WITH_OVERRIDE(PartDesign::SheetMetalBendTable);

public:
    SheetMetalBendTable();

    App::PropertyLink Source;
    App::PropertyBool AutoSync;
    /// Each cell the table wrote, and what it wrote, as the cell reads back
    App::PropertyMap Generated;

    /** Fill the table from Source; a cell the user changed stays theirs
     * unless \a force. Throws Base::ValueError for a Source of no sheet.
     */
    void sync(bool force = false);

    short mustExecute() const override;
    App::DocumentObjectExecReturn* execute() override;
};

}  // namespace PartDesign
