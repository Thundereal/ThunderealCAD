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
#include <Gui/Command.h>
#include <Gui/Document.h>

#include "ViewProviderSketch3D.h"

// The 3D sketch's commands (docs/Sketch3D.md section 7).

using namespace SketcherGui;

DEF_STD_CMD_A(CmdSketcherNewSketch3D)

CmdSketcherNewSketch3D::CmdSketcherNewSketch3D()
    : Command("Sketcher_NewSketch3D")
{
    sAppModule = "Sketcher";
    sGroup = "Sketcher";
    sMenuText = QT_TR_NOOP("Create 3D Sketch");
    sToolTipText =
        QT_TR_NOOP("Creates a 3D sketch: constrained geometry in space, in a frame of its own");
    sWhatsThis = "Sketcher_NewSketch3D";
    sStatusTip = sToolTipText;
    sPixmap = "Sketcher_NewSketch";
}

void CmdSketcherNewSketch3D::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    // Into the active Body when there is one: a Body takes a 3D sketch as it takes a
    // sketch, so a pipe in it can follow the path (section 5).
    std::string name = getUniqueObjectName("Sketch3D");
    openCommand(QT_TRANSLATE_NOOP("Command", "Create a 3D sketch"));
    doCommand(Doc, "App.ActiveDocument.addObject('Sketcher::Sketch3DObject', '%s')", name.c_str());
    doCommand(Gui,
              "_body = Gui.ActiveDocument.ActiveView.getActiveObject('pdbody')\n"
              "if _body:\n"
              "    _body.addObject(App.ActiveDocument.getObject('%s'))\n"
              "del _body",
              name.c_str());
    doCommand(Gui, "Gui.activeDocument().setEdit('%s')", name.c_str());
    commitCommand();
}

bool CmdSketcherNewSketch3D::isActive()
{
    auto doc = getActiveGuiDocument();
    return doc && !doc->getInEdit();
}

void CreateSketcherCommandsSketch3D()
{
    Gui::CommandManager& rcCmdMgr = Gui::Application::Instance->commandManager();
    rcCmdMgr.addCommand(new CmdSketcherNewSketch3D());
}
