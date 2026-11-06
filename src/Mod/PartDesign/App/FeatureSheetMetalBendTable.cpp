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

#include "PreCompiled.h"

#include <iomanip>
#include <map>
#include <sstream>
#include <string>

#include <App/Document.h>
#include <App/Range.h>
#include <Base/Exception.h>
#include <Mod/Spreadsheet/App/Cell.h>

#include "FeatureSheetMetalBendTable.h"
#include "SheetMetalCheck.h"

using namespace PartDesign;

PROPERTY_SOURCE(PartDesign::SheetMetalBendTable, Spreadsheet::Sheet)

SheetMetalBendTable::SheetMetalBendTable()
{
    static const char* group = "Bend Table";
    ADD_PROPERTY_TYPE(Source, (nullptr), group, App::Prop_None,
                      "The sheet whose bends the table lists: a sheet metal solid or a feature "
                      "of one");
    ADD_PROPERTY_TYPE(AutoSync, (true), group, App::Prop_None,
                      "Fill the table at each recompute; off, only when synced by hand");
    ADD_PROPERTY_TYPE(Generated, (), group,
                      App::PropertyType(App::Prop_Hidden | App::Prop_ReadOnly),
                      "Each cell the table wrote, and what it wrote: a cell reading otherwise is "
                      "the user's");
}

short SheetMetalBendTable::mustExecute() const
{
    if (Source.isTouched() || AutoSync.isTouched()) {
        return 1;
    }
    return Spreadsheet::Sheet::mustExecute();
}

App::DocumentObjectExecReturn* SheetMetalBendTable::execute()
{
    // a table never filled fills once, AutoSync or not
    if ((AutoSync.getValue() || Generated.getValues().empty()) && Source.getValue()) {
        try {
            sync();
        }
        catch (const Base::Exception& e) {
            return new App::DocumentObjectExecReturn(e.what());
        }
    }
    return Spreadsheet::Sheet::execute();
}

namespace
{

/// A number as the table writes it: no more decimals than it has, at most
/// \a decimals
std::string number(double v, int decimals)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(decimals) << v;
    std::string text = s.str();
    if (text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (text.back() == '.') {
            text.pop_back();
        }
    }
    return text;
}

/// A text as a cell takes it, never read as a number or a formula
std::string text(const std::string& value)
{
    return "'" + value;
}

}  // namespace

void SheetMetalBendTable::sync(bool force)
{
    auto source = Source.getValue();
    if (!source) {
        THROWM(Base::ValueError, "The bend table has no sheet");
    }
    const std::vector<SheetBendRow> rows = sheetBends(source);

    // what the table writes, by cell
    std::map<std::string, std::string> want;
    const char* columns = "ABCDEFG";
    const char* headers[] = {"Bend", "Feature", "Angle (deg)", "Radius (mm)", "K",
                             "Allowance (mm)", "Direction"};
    for (int c = 0; c < 7; ++c) {
        want[std::string(1, columns[c]) + "1"] = text(headers[c]);
    }
    auto doc = getDocument();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const SheetBendRow& row = rows[i];
        const std::string r = std::to_string(i + 2);
        auto maker = doc ? doc->getObject(row.feature.c_str()) : nullptr;
        want["A" + r] = text(row.name);
        want["B" + r] = text(maker ? maker->Label.getValue() : row.feature);
        want["C" + r] = number(row.angle, 2);
        want["D" + r] = number(row.radius, 3);
        want["E" + r] = number(row.kFactor, 4);
        want["F" + r] = number(row.allowance, 3);
        want["G" + r] = text(row.direction);
    }

    auto content = [this](const std::string& address) {
        std::string s;
        if (auto cell = getCell(App::stringToAddress(address.c_str()))) {
            cell->getStringContent(s);
        }
        return s;
    };
    const auto& before = Generated.getValues();
    std::map<std::string, std::string> generated;
    for (const auto& [address, value] : want) {
        const std::string now = content(address);
        auto it = before.find(address);
        // the user's: changed since the table wrote it, or written where it
        // never did
        const bool users = !force
            && (it != before.end() ? now != it->second : !now.empty());
        if (users) {
            if (it != before.end()) {
                generated[address] = it->second;
            }
            continue;
        }
        setCell(address.c_str(), value.c_str());
        generated[address] = content(address);
    }
    // a row the sheet no longer has: emptied but for what the user wrote
    for (const auto& [address, value] : before) {
        if (want.count(address)) {
            continue;
        }
        if (force || content(address) == value) {
            clear(App::stringToAddress(address.c_str()));
        }
    }
    Generated.setValues(generated);
}
