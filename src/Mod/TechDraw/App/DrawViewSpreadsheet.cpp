/***************************************************************************
 *   Copyright (c) 2015 Yorik van Havre <yorik@uncreated.net>              *
 *   Copyright (c) 2016 WandererFan <wandererfan@gmail.com>                *
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

#ifndef _PreComp_
#include <iomanip>
#include <sstream>
#include <boost_regex.hpp>
#endif

#include <QCoreApplication>

#include <App/Property.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Mod/Spreadsheet/App/Cell.h>
#include <Mod/Spreadsheet/App/Sheet.h>

#include "DrawUtil.h"
#include "Preferences.h"

#include "DrawViewSpreadsheet.h"

using namespace TechDraw;

//===========================================================================
// DrawViewSpreadsheet
//===========================================================================

PROPERTY_SOURCE(TechDraw::DrawViewSpreadsheet, TechDraw::DrawViewSymbol)

DrawViewSpreadsheet::DrawViewSpreadsheet()
{
    static const char *vgroup = "Spreadsheet";

    ADD_PROPERTY_TYPE(Source ,(nullptr), vgroup, App::Prop_None, "Spreadsheet to view");
    Source.setScope(App::LinkScope::Global);
    ADD_PROPERTY_TYPE(CellStart ,("A1"), vgroup, App::Prop_None, "The top left cell of the range to display");
    ADD_PROPERTY_TYPE(CellEnd ,("B2"), vgroup, App::Prop_None, "The bottom right cell of the range to display");
    ADD_PROPERTY_TYPE(Font ,(Preferences::labelFont().c_str()),
                                                         vgroup, App::Prop_None, "The name of the font to use");
    ADD_PROPERTY_TYPE(TextColor, (0.0f, 0.0f, 0.0f), vgroup, App::Prop_None, "The default color of the text and lines");
    ADD_PROPERTY_TYPE(TextSize, (12.0), vgroup, App::Prop_None, "The size of the text");
    ADD_PROPERTY_TYPE(LineWidth, (0.35), vgroup, App::Prop_None, "The thickness of the cell lines");

    EditableTexts.setStatus(App::Property::Hidden, true);

}

DrawViewSpreadsheet::~DrawViewSpreadsheet()
{
}

short DrawViewSpreadsheet::mustExecute() const
{
    if (!isRestoring()) {
        if (
            Source.isTouched()  ||
            CellStart.isTouched() ||
            CellEnd.isTouched() ||
            Font.isTouched() ||
            TextSize.isTouched() ||
            TextColor.isTouched() ||
            LineWidth.isTouched()
        ) {
            return 1;
        }
    }

    return TechDraw::DrawView::mustExecute();
}

void DrawViewSpreadsheet::onChanged(const App::Property* prop)
{
    TechDraw::DrawView::onChanged(prop);
}

App::DocumentObjectExecReturn *DrawViewSpreadsheet::execute()
{
    App::DocumentObject* link = Source.getValue();
    std::string scellstart = CellStart.getValue();
    std::string scellend = CellEnd.getValue();
    // a view with nothing to show says why (docs/TechDrawErrors.md 3.2)
    auto fail = [this](const QString& why) {
        return new App::DocumentObjectExecReturn(why.toStdString(), this);
    };
    if (!link)
        return fail(QCoreApplication::translate("TechDraw", "The view has no spreadsheet to show"));
    if (!link->isDerivedFrom<Spreadsheet::Sheet>())
        return fail(QCoreApplication::translate("TechDraw", "%1 is not a spreadsheet")
                        .arg(QString::fromUtf8(link->Label.getValue())));
    if (scellstart.empty() || scellend.empty())
        return fail(QCoreApplication::translate("TechDraw",
                                                "The cell range has no start or no end cell"));

    try {
        Symbol.setValue(getSheetImage());
    }
    catch (const Base::Exception& e) {
        return fail(QString::fromUtf8(e.what()));
    }

    overrideKeepUpdated(false);
    return TechDraw::DrawView::execute();
}

std::vector <std::string> DrawViewSpreadsheet::getAvailColumns()
{
    // builds a list of available columns: A, B, ... Y, Z, AA, AB, ... ZY, ZZ.
    const std::string alphabet [] {"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};

    std::vector <std::string> availcolumns { std::begin (alphabet), std::end (alphabet) };

    for (const std::string &left : alphabet)
        for (const std::string &right : alphabet)
            availcolumns.push_back(left + right);

    return availcolumns;
}

std::string DrawViewSpreadsheet::getSVGHead()
{
    return std::string("<svg\n") +
           std::string("	xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\"\n") +
           std::string("	xmlns:freecad=\"https://www.freecad.org/wiki/index.php?title=Svg_Namespace\">\n");
}

std::string DrawViewSpreadsheet::getSVGTail()
{
    return "\n</svg>";
}

std::string DrawViewSpreadsheet::getSheetImage()
{
    App::DocumentObject* link = Source.getValue();
    link->recomputeFeature();   //make sure s/s is up to date

    std::string scellstart = CellStart.getValue();
    std::string scellend = CellEnd.getValue();

    //s/s columns are A, B,C, ... ZX, ZY, ZZ
    //lower case characters are not valid
    transform(scellstart.begin(), scellstart.end(), scellstart.begin(), ::toupper);
    transform(scellend.begin(), scellend.end(), scellend.begin(), ::toupper);

    // what is wrong with the range is the view's error, naming the cell
    // (docs/TechDrawErrors.md 3.2); execute() returns it
    const std::vector <std::string> availcolumns = getAvailColumns();
    boost::regex re{"([A-Z]+)([0-9]+)"};
    auto parseCell = [&](const std::string& cell, const char* given, std::string& column,
                         int& row) {
        boost::smatch what;
        if (!boost::regex_match(cell, what, re)) {
            throw Base::ValueError(
                QCoreApplication::translate("TechDraw",
                                            "%1 is not a cell address such as A1")
                    .arg(QString::fromUtf8(given))
                    .toStdString());
        }
        column = what[1];
        try {
            row = std::stoi(what[2]);
        }
        catch (...) {
            row = -1;
        }
        if (row < 1) {
            throw Base::ValueError(
                QCoreApplication::translate("TechDraw", "The row of the cell %1 is not valid")
                    .arg(QString::fromUtf8(given))
                    .toStdString());
        }
        int index = colInList(availcolumns, column);
        if (index < 0) {
            throw Base::ValueError(
                QCoreApplication::translate("TechDraw",
                                            "The column of the cell %1 is outside A to ZZ")
                    .arg(QString::fromUtf8(given))
                    .toStdString());
        }
        return index;
    };
    int iRowStart = 0, iRowEnd = 0;
    std::string sColStart, sColEnd;
    int iAvailColStart = parseCell(scellstart, CellStart.getValue(), sColStart, iRowStart);
    int iAvailColEnd = parseCell(scellend, CellEnd.getValue(), sColEnd, iRowEnd);

    //check for logical range
    if ( (iAvailColStart > iAvailColEnd) ||
         (iRowStart > iRowEnd) ) {
        throw Base::ValueError(
            QCoreApplication::translate("TechDraw",
                                        "The cell range %1:%2 is reversed: its start cell is "
                                        "below or to the right of its end cell")
                .arg(QString::fromUtf8(CellStart.getValue()),
                     QString::fromUtf8(CellEnd.getValue()))
                .toStdString());
    }

    // build row and column ranges
    std::vector<std::string> validColNames;
    std::vector<int> validRowNumbers;

    int iCol = iAvailColStart;
    for (; iCol <= iAvailColEnd; iCol++) {
        validColNames.push_back(availcolumns.at(iCol));
    }

    int iRow = iRowStart;
    for ( ; iRow <= iRowEnd ; iRow++) {
        validRowNumbers.push_back(iRow);
    }

    // create the SVG code
    std::stringstream result;
    result << getSVGHead();

    std::string ViewName = Label.getValue();
    App::Color c = TextColor.getValue();
    result << "<g id=\"" << ViewName << "\">" << std::endl;

    // fill the cells
    float rowoffset = 0.0;
    float coloffset = 0.0;
    float cellheight = 100;
    float cellwidth = 100;
    std::string celltext;
    Spreadsheet::Sheet* sheet = static_cast<Spreadsheet::Sheet*>(link);
    std::vector<std::string> skiplist;

    for (std::vector<std::string>::const_iterator col = validColNames.begin();
         col != validColNames.end(); ++col) {
        // create a group for each column
        result << "  <g id=\"" << ViewName << "_col" << (*col) << "\">" << std::endl;
        for (std::vector<int>::const_iterator row = validRowNumbers.begin();
             row != validRowNumbers.end(); ++row) {
            // get cell size
            std::stringstream srow;
            srow << (*row);
            App::CellAddress address((*col) + srow.str());
            cellwidth = sheet->getColumnWidth(address.col());
            cellheight = sheet->getRowHeight(address.row());
            celltext = "";
            Spreadsheet::Cell* cell = sheet->getCell(address);
            // get the text
            App::Property* prop = sheet->getPropertyByName(address.toString().c_str());
            std::stringstream field;
            if (prop && cell) {
                if (
                    prop->isDerivedFrom(App::PropertyQuantity::getClassTypeId()) ||
                    prop->isDerivedFrom(App::PropertyFloat::getClassTypeId()) ||
                    prop->isDerivedFrom(App::PropertyInteger::getClassTypeId())
                ) {
                    std::string temp = cell->getFormattedQuantity();    //writable
                    DrawUtil::encodeXmlSpecialChars(temp);
                    field << temp;
                } else if (prop->isDerivedFrom(App::PropertyString::getClassTypeId())) {
                    std::string temp = static_cast<App::PropertyString*>(prop)->getValue();
                    DrawUtil::encodeXmlSpecialChars(temp);
                    field << temp;
                } else {
                    Base::Console().Error("DVSS: Unknown property type\n");
                }
                celltext = field.str();
            }
            // get colors, style, alignment and span
            int alignment = 0;
            std::string bcolor = "none";
            std::string fcolor = c.asHexString();
            std::string textstyle;
            if (cell) {
                App::Color f, b;
                std::set<std::string> st;
                int colspan, rowspan;
                if (cell->getBackground(b)) {
                    bcolor = b.asHexString();
                }
                if (cell->getForeground(f)) {
                    fcolor = f.asHexString();
                }
                if (cell->getStyle(st)) {
                    for (std::set<std::string>::const_iterator i = st.begin(); i != st.end(); ++i) {
                         if ((*i) == "bold")
                            textstyle += "font-weight: bold; ";
                        else if ((*i) == "italic")
                            textstyle += "font-style: italic; ";
                        else if ((*i) == "underline")
                            textstyle += "text-decoration: underline; ";
                    }
                }
                if (cell->getSpans(rowspan, colspan)) {
                    for (int i=0; i<colspan; ++i) {
                        for (int j=0; j<rowspan; ++j) {
                            App::CellAddress nextcell(address.row()+j, address.col()+i);
                            if (i > 0)
                                cellwidth += sheet->getColumnWidth(nextcell.col());
                            if (j > 0)
                                cellheight += sheet->getRowHeight(nextcell.row());
                            if ( (i > 0) || (j > 0) )
                                skiplist.push_back(nextcell.toString());
                        }
                    }
                }
                cell->getAlignment(alignment);
            }
            // skip cell if found in skiplist
            if (std::find(skiplist.begin(), skiplist.end(), address.toString()) == skiplist.end()) {
                result << "    <rect x=\"" << coloffset << "\" y=\"" << rowoffset << "\" width=\""
                       << cellwidth << "\" height=\"" << cellheight << "\" style=\"fill:" << bcolor
                       << ";stroke-width:" << LineWidth.getValue() / getScale()
                       << ";stroke:" << c.asHexString() << ";\" />" << std::endl;
                if (alignment & Spreadsheet::Cell::ALIGNMENT_LEFT)
                    result << "    <text style=\"" << textstyle << "\" x=\""
                           << coloffset + TextSize.getValue() / 2 << "\" y=\""
                           << rowoffset + 0.75 * cellheight << "\" font-family=\"";
                if (alignment & Spreadsheet::Cell::ALIGNMENT_HCENTER)
                    result << "    <text text-anchor=\"middle\" style=\"" << textstyle << "\" x=\""
                           << coloffset + cellwidth / 2 << "\" y=\""
                           << rowoffset + 0.75 * cellheight << "\" font-family=\"";
                if (alignment & Spreadsheet::Cell::ALIGNMENT_RIGHT)
                    result << "    <text text-anchor=\"end\" style=\"" << textstyle << "\" x=\""
                           << coloffset + (cellwidth - TextSize.getValue() / 2) << "\" y=\""
                           << rowoffset + 0.75 * cellheight << "\" font-family=\"";
                if ((alignment & Spreadsheet::Cell::ALIGNMENT_LEFT)
                    || (alignment & Spreadsheet::Cell::ALIGNMENT_HCENTER)
                    || (alignment & Spreadsheet::Cell::ALIGNMENT_RIGHT)) {
                    result << Font.getValue() << "\""
                           << " font-size=\"" << TextSize.getValue() << "\""
                           << " fill=\"" << fcolor << "\">" << celltext << "</text>" << std::endl;
                }
            }
            rowoffset += sheet->getRowHeight(address.row());
        }
        result << "  </g>" << std::endl;
        rowoffset = 0.0;
        coloffset += cellwidth;
    }

    // close the containing group
    result << "</g>" << std::endl;

    result << getSVGTail();

    return result.str();

}

//find index of column name "toFind" in "list" of column names
int DrawViewSpreadsheet::colInList(const std::vector<std::string>& list,
                                   const std::string& toFind)
{
    auto match = std::find(std::begin(list), std::end(list), toFind);
    if (match == std::end(list)) {
        return -1; // Error value
    }

    return match - std::begin(list);
}

// Python Drawing feature ---------------------------------------------------------

namespace App {
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(TechDraw::DrawViewSpreadsheetPython, TechDraw::DrawViewSpreadsheet)
template<> const char* TechDraw::DrawViewSpreadsheetPython::getViewProviderName() const {
    return "TechDrawGui::ViewProviderSpreadsheet";
}
/// @endcond

// explicit template instantiation
template class TechDrawExport FeaturePythonT<TechDraw::DrawViewSpreadsheet>;
}
