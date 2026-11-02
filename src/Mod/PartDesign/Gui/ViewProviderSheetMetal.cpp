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

#include <Gui/Application.h>
#include <Gui/Control.h>
#include <Gui/Document.h>

#include "TaskSheetMetalFlatPattern.h"
#include "TaskSheetMetalParameters.h"
#include "TaskSheetMetalRule.h"
#include "ViewProviderSheetMetal.h"

using namespace PartDesignGui;

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalBaseWall, PartDesignGui::ViewProviderSketchBased)

ViewProviderSheetMetalBaseWall::ViewProviderSheetMetalBaseWall()
{
    sPixmap = "PartDesign_SheetMetalBaseWall";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalBaseWall::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalBaseWall",
                                     QObject::tr("Base wall parameters"),
                                     {"MaterialSide", "Length", "Reversed", "Midplane"}));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalFlange, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalFlange::ViewProviderSheetMetalFlange()
{
    sPixmap = "PartDesign_SheetMetalFlange";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalFlange::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(
            this,
            "PartDesign_SheetMetalFlange",
            QObject::tr("Flange parameters"),
            {"Angle",
             "Length",
             "LengthType",
             "BendPosition",
             "Offset",
             "Gap1",
             "Gap2",
             "Reversed",
             "Profile",
             "ReliefShape",
             "ReliefWidth",
             "ReliefDepth"},
            "Edges",
            "Edge"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalExtendWall, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalExtendWall::ViewProviderSheetMetalExtendWall()
{
    sPixmap = "PartDesign_SheetMetalExtendWall";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalExtendWall::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalExtendWall",
                                     QObject::tr("Extend wall parameters"),
                                     {"Length", "Gap1", "Gap2"},
                                     "Faces",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalHem, PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalHem::ViewProviderSheetMetalHem()
{
    sPixmap = "PartDesign_SheetMetalHem";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalHem::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalHem",
                                     QObject::tr("Hem parameters"),
                                     {"HemType",
                                      "Length",
                                      "Gap",
                                      "Radius",
                                      "Angle",
                                      "Position",
                                      "Gap1",
                                      "Gap2",
                                      "Reversed"},
                                     "Edges",
                                     "Edge"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalCornerClosure,
                PartDesignGui::ViewProviderAddSub)

ViewProviderSheetMetalCornerClosure::ViewProviderSheetMetalCornerClosure()
{
    sPixmap = "PartDesign_SheetMetalCornerClosure";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalCornerClosure::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalCornerClosure",
                                     QObject::tr("Corner closure parameters"),
                                     {"CornerType"},
                                     "Faces",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalCutout,
                PartDesignGui::ViewProviderSketchBased)

ViewProviderSheetMetalCutout::ViewProviderSheetMetalCutout()
{
    sPixmap = "PartDesign_SheetMetalCutout";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalCutout::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalCutout",
                                     QObject::tr("Cutout parameters"),
                                     {"Profile", "Reversed", "UpToFirst"}));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalFold, PartDesignGui::ViewProviderSketchBased)

ViewProviderSheetMetalFold::ViewProviderSheetMetalFold()
{
    sPixmap = "PartDesign_SheetMetalFold";
}

TaskDlgFeatureParameters* ViewProviderSheetMetalFold::getEditDialog()
{
    return new TaskDlgSheetMetalParameters(
        this,
        new TaskSheetMetalParameters(this,
                                     "PartDesign_SheetMetalFold",
                                     QObject::tr("Fold parameters"),
                                     {"Profile", "Angle", "LinePosition", "Reversed"},
                                     "Moving",
                                     "Face"));
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalFlatPattern, PartGui::ViewProviderPart)

ViewProviderSheetMetalFlatPattern::ViewProviderSheetMetalFlatPattern()
{
    sPixmap = "PartDesign_SheetMetalFlatPattern";
}

bool ViewProviderSheetMetalFlatPattern::doubleClicked()
{
    auto doc = Gui::Application::Instance->getDocument(getObject()->getDocument());
    return doc && doc->setEdit(this, ViewProvider::Default);
}

bool ViewProviderSheetMetalFlatPattern::setEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        return PartGui::ViewProviderPart::setEdit(ModNum);
    }
    if (Gui::Control().activeDialog()) {
        return false;
    }
    Gui::Control().showDialog(new TaskDlgSheetMetalFlatPattern(this));
    return true;
}

void ViewProviderSheetMetalFlatPattern::unsetEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        PartGui::ViewProviderPart::unsetEdit(ModNum);
        return;
    }
    Gui::Control().closeDialog();
}

PROPERTY_SOURCE(PartDesignGui::ViewProviderSheetMetalRule, PartGui::ViewProviderPart)

ViewProviderSheetMetalRule::ViewProviderSheetMetalRule() = default;

bool ViewProviderSheetMetalRule::doubleClicked()
{
    auto doc = Gui::Application::Instance->getDocument(getObject()->getDocument());
    return doc && doc->setEdit(this, ViewProvider::Default);
}

bool ViewProviderSheetMetalRule::setEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        return PartGui::ViewProviderPart::setEdit(ModNum);
    }
    if (Gui::Control().activeDialog()) {
        return false;
    }
    Gui::Control().showDialog(new TaskDlgSheetMetalRule(this));
    return true;
}

void ViewProviderSheetMetalRule::unsetEdit(int ModNum)
{
    if (ModNum != ViewProvider::Default) {
        PartGui::ViewProviderPart::unsetEdit(ModNum);
        return;
    }
    Gui::Control().closeDialog();
}
