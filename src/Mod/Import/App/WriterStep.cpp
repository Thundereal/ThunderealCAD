// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2023 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of FreeCAD.                                         *
 *                                                                         *
 *   FreeCAD is free software: you can redistribute it and/or modify it    *
 *   under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1 of the  *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   FreeCAD is distributed in the hope that it will be useful, but        *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU      *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with FreeCAD. If not, see                               *
 *   <https://www.gnu.org/licenses/>.                                      *
 *                                                                         *
 **************************************************************************/


#include "PreCompiled.h"
#ifndef _PreComp_
#include <APIHeaderSection_MakeHeader.hxx>
#include <NCollection_Vector.hxx>
#include <Interface_Check.hxx>
#include <Interface_CheckIterator.hxx>
#include <Quantity_Color.hxx>
#include <XSControl_WorkSession.hxx>
#include <STEPCAFControl_Writer.hxx>
#endif

#include "Tools.h"
#include "WriterStep.h"
#include <Base/Console.h>
#include <Base/Exception.h>
#include <App/Application.h>
#include <Mod/Part/App/PartParams.h>
#include <Mod/Part/App/encodeFilename.h>
#include <Mod/Part/App/Interface.h>

using namespace Import;

WriterStep::WriterStep(const Base::FileInfo& file)  // NOLINT
    : file {file}
{}

void WriterStep::write(Handle(TDocStd_Document) hDoc) const  // NOLINT
{
    std::string utf8Name = file.filePath();
    std::string name8bit = Part::encodeFilename(utf8Name);

    // OCCT writes semantic GD&T only in AP242, and AP214's legacy form
    // otherwise: a document with PMI is written AP242, whatever the
    // preference, and the export says so (docs/GDandT.md sec 5.2,
    // decision 3)
    struct Schema
    {
        std::string was;
        ~Schema()
        {
            if (!was.empty()) {
                Part::Interface::writeStepScheme(was.c_str());
            }
        }
    } schema;
    if (Tools::hasGDT(hDoc)) {
        std::string current = Part::Interface::writeStepScheme();
        if (current != "AP242DIS") {
            schema.was = current;
            Part::Interface::writeStepScheme("AP242DIS");
            Base::Console().message("STEP written as AP242, not %s: it has PMI (GD&T)\n",
                                    current.c_str());
        }
    }

    STEPCAFControl_Writer writer;
    if (Tools::hasGDT(hDoc)) {
        // The PMI's graphics in the colour the 3D view draws them in, not
        // OCCT's white
        auto pmi = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Mod/PMI");
        App::Color color;
        color.setPackedValue(
            pmi->GetUnsigned("ExportColor", App::Color(0.1f, 0.25f, 0.6f).getPackedValue()));
        writer.SetGDTPresentationColor(Tools::convertColor(color).GetRGB());
    }
    // Styles carrying a visualization material (per-face ShapeAppearance,
    // see ExportOCAF2) write surface_style_rendering_with_properties
    // instead of collapsing to the colour.
    writer.SetVisualMaterialMode(true);
    Part::Interface::writeStepAssembly(Part::Interface::Assembly::On);
    writer.Transfer(hDoc, STEPControl_AsIs);

    APIHeaderSection_MakeHeader makeHeader(writer.ChangeWriter().Model());
    Base::Reference<ParameterGrp> hGrp = App::GetApplication()
                                             .GetUserParameter()
                                             .GetGroup("BaseApp")
                                             ->GetGroup("Preferences")
                                             ->GetGroup("Mod/Part")
                                             ->GetGroup("STEP");

    // Don't set name because STEP doesn't support UTF-8
    // https://forum.freecad.org/viewtopic.php?f=8&t=52967
    makeHeader.SetAuthorValue(
        1,
        new TCollection_HAsciiString(hGrp->GetASCII("Author", Part::PartParams::defaultStepAuthor().c_str()).c_str()));
    makeHeader.SetOrganizationValue(
        1,
        new TCollection_HAsciiString(hGrp->GetASCII("Company", Part::PartParams::defaultStepCompany().c_str()).c_str()));
    makeHeader.SetOriginatingSystem(
        new TCollection_HAsciiString(App::Application::getExecutableName().c_str()));
    makeHeader.SetDescriptionValue(
        1,
        new TCollection_HAsciiString((App::Application::getExecutableName() + " Model").c_str()));
    IFSelect_ReturnStatus ret = writer.Write(name8bit.c_str());
    if (ret == IFSelect_RetError || ret == IFSelect_RetFail || ret == IFSelect_RetStop) {
        // What the writer says went wrong: a file it could not open is one
        // of them, a model it could not write (an exception while sending
        // it, kept in its checks) another
        std::string why;
        Interface_CheckIterator checks = writer.ChangeWriter().WS()->LastRunCheckList();
        for (checks.Start(); checks.More(); checks.Next()) {
            const Handle(Interface_Check)& check = checks.Value();
            for (int i = 1; i <= check->NbFails(); ++i) {
                why += (why.empty() ? "" : "; ") + std::string(check->CFail(i));
            }
        }
        if (why.empty()) {
            THROWM(Base::FileException, "Cannot open file: ", file);
        }
        FC_THROWM(Base::FileException, "Cannot write " << file.filePath() << ": " << why);
    }
}
