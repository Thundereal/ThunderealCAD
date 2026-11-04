/****************************************************************************
 *   Copyright (c) 2020 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of the FreeCAD CAx development system.               *
 *                                                                          *
 *   This library is free software; you can redistribute it and/or          *
 *   modify it under the terms of the GNU Library General Public            *
 *   License as published by the Free Software Foundation; either           *
 *   version 2 of the License, or (at your option) any later version.       *
 *                                                                          *
 *   This library  is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Library General Public License for more details.                   *
 *                                                                          *
 *   You should have received a copy of the GNU Library General Public      *
 *   License along with this library; see the file COPYING.LIB. If not,     *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,          *
 *   Suite 330, Boston, MA  02111-1307, USA                                 *
 *                                                                          *
 ****************************************************************************/

#include "PreCompiled.h"

#ifndef _PreComp_
#endif

#include <Base/Tools.h>
#include <Gui/Command.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/ActiveObjectList.h>
#include <Gui/MDIView.h>
#include <Mod/PartDesign/App/AuxGroup.h>
#include <Mod/PartDesign/App/Body.h>

#include "ViewProviderAuxGroup.h"

using namespace PartDesignGui;

PROPERTY_SOURCE(PartDesignGui::ViewProviderAuxGroup, Gui::ViewProviderAuxGroup)

ViewProviderAuxGroup::ViewProviderAuxGroup()
{
    sPixmap = "PartDesign_Group.svg";
}

ViewProviderAuxGroup::~ViewProviderAuxGroup()
{
}

void ViewProviderAuxGroup::attach(App::DocumentObject* obj)
{
    inherited::attach(obj);
    auto group = Base::freecad_dynamic_cast<PartDesign::AuxGroup>(obj);
    if (group) {
        switch (group->getGroupType()) {
        case PartDesign::AuxGroup::SketchGroup:
            sPixmap = "PartDesign_GroupSketch.svg";
            break;
        case PartDesign::AuxGroup::DatumGroup:
            sPixmap = "PartDesign_GroupDatum.svg";
            break;
        case PartDesign::AuxGroup::MiscGroup:
            sPixmap = "PartDesign_GroupMisc.svg";
            break;
        case PartDesign::AuxGroup::SurfaceGroup:
            sPixmap = "PartDesign_GroupSurface.svg";
            break;
        default:
            sPixmap = "PartDesign_Group.svg";
        }
    }
}

bool ViewProviderAuxGroup::doubleClicked()
{
    auto owner = Base::freecad_dynamic_cast<PartDesign::AuxGroup>(getObject());
    if (!owner || owner->getGroupType() != PartDesign::AuxGroup::SurfaceGroup)
        return inherited::doubleClicked();

    auto activeDoc = Gui::Application::Instance->activeDocument();
    if (!activeDoc)
        activeDoc = getDocument();
    auto view = activeDoc->setActiveView(this);
    if (!view)
        return false;
    if (view->getActiveObject<App::DocumentObject*>(PDSURFACEKEY) == owner) {
        Gui::Command::doCommand(Gui::Command::Gui,
                "Gui.ActiveDocument.ActiveView.setActiveObject('%s', None)", PDSURFACEKEY);
        return true;
    }
    // its body active too, where what the Surface commands make goes
    auto body = owner->getBody();
    if (body && view->getActiveObject<App::DocumentObject*>(PDBODYKEY) != body) {
        if (auto bodyVp = Gui::Application::Instance->getViewProvider(body))
            bodyVp->doubleClicked();
    }
    Gui::Command::doCommand(Gui::Command::Gui,
            "Gui.ActiveDocument.ActiveView.setActiveObject('%s', %s)",
            PDSURFACEKEY, owner->getFullName(true).c_str());
    return true;
}
