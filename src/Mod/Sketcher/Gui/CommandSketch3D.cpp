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

// The manipulator's switches (section 7, decision j): commands, so Tools > Customize
// rebinds them, active only while a 3D sketch is in edit.

namespace
{
bool manipulatorActive()
{
    return activeSketch3DInEdit() != nullptr;
}
}  // namespace

DEF_STD_CMD_A(CmdSketcherSketch3DCyclePlane)

CmdSketcherSketch3DCyclePlane::CmdSketcherSketch3DCyclePlane()
    : Command("Sketcher_Sketch3DCyclePlane")
{
    sAppModule = "Sketcher";
    sGroup = "Sketcher";
    sMenuText = QT_TR_NOOP("Cycle Working Plane");
    sToolTipText = QT_TR_NOOP("Switches the 3D sketch manipulator's active plane: XY, YZ, ZX");
    sWhatsThis = "Sketcher_Sketch3DCyclePlane";
    sStatusTip = sToolTipText;
    // Inventor's key for it; Fusion's Alt cannot be a shortcut on its own.
    sAccel = "Shift+Space";
}

void CmdSketcherSketch3DCyclePlane::activated(int /*iMsg*/)
{
    if (auto vp = activeSketch3DInEdit()) {
        vp->cycleWorkingPlane();
    }
}

bool CmdSketcherSketch3DCyclePlane::isActive()
{
    return manipulatorActive();
}

DEF_STD_CMD_A(CmdSketcherSketch3DLockHeight)

CmdSketcherSketch3DLockHeight::CmdSketcherSketch3DLockHeight()
    : Command("Sketcher_Sketch3DLockHeight")
{
    sAppModule = "Sketcher";
    sGroup = "Sketcher";
    sMenuText = QT_TR_NOOP("Lock Height");
    sToolTipText = QT_TR_NOOP(
        "Keeps the position on the working plane and moves along its normal; again to release");
    sWhatsThis = "Sketcher_Sketch3DLockHeight";
    sStatusTip = sToolTipText;
    sAccel = "Up";  // Fusion's
}

void CmdSketcherSketch3DLockHeight::activated(int /*iMsg*/)
{
    if (auto vp = activeSketch3DInEdit()) {
        vp->toggleHeightLock();
    }
}

bool CmdSketcherSketch3DLockHeight::isActive()
{
    return manipulatorActive();
}

DEF_STD_CMD_A(CmdSketcherSketch3DResetManipulator)

CmdSketcherSketch3DResetManipulator::CmdSketcherSketch3DResetManipulator()
    : Command("Sketcher_Sketch3DResetManipulator")
{
    sAppModule = "Sketcher";
    sGroup = "Sketcher";
    sMenuText = QT_TR_NOOP("Reset Manipulator");
    sToolTipText = QT_TR_NOOP("Turns the 3D sketch manipulator back to the sketch's frame");
    sWhatsThis = "Sketcher_Sketch3DResetManipulator";
    sStatusTip = sToolTipText;
}

void CmdSketcherSketch3DResetManipulator::activated(int /*iMsg*/)
{
    if (auto vp = activeSketch3DInEdit()) {
        vp->resetManipulator();
    }
}

bool CmdSketcherSketch3DResetManipulator::isActive()
{
    return manipulatorActive();
}

DEF_STD_CMD_A(CmdSketcherSketch3DRotationHandles)

CmdSketcherSketch3DRotationHandles::CmdSketcherSketch3DRotationHandles()
    : Command("Sketcher_Sketch3DRotationHandles")
{
    sAppModule = "Sketcher";
    sGroup = "Sketcher";
    sMenuText = QT_TR_NOOP("Manipulator Rotation Handles");
    sToolTipText = QT_TR_NOOP("Shows or hides the rings that tilt the 3D sketch manipulator");
    sWhatsThis = "Sketcher_Sketch3DRotationHandles";
    sStatusTip = sToolTipText;
}

void CmdSketcherSketch3DRotationHandles::activated(int /*iMsg*/)
{
    if (auto vp = activeSketch3DInEdit()) {
        vp->setRotationHandlesShown(!vp->rotationHandlesShown());
    }
}

bool CmdSketcherSketch3DRotationHandles::isActive()
{
    return manipulatorActive();
}

void CreateSketcherCommandsSketch3D()
{
    Gui::CommandManager& rcCmdMgr = Gui::Application::Instance->commandManager();
    rcCmdMgr.addCommand(new CmdSketcherNewSketch3D());
    rcCmdMgr.addCommand(new CmdSketcherSketch3DCyclePlane());
    rcCmdMgr.addCommand(new CmdSketcherSketch3DLockHeight());
    rcCmdMgr.addCommand(new CmdSketcherSketch3DResetManipulator());
    rcCmdMgr.addCommand(new CmdSketcherSketch3DRotationHandles());
}
