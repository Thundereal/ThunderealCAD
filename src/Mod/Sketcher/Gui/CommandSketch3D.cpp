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

#include <map>

#include <Base/Console.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/Sketcher/App/Sketch3DObject.h>

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

// The Sketcher's constraint commands in a 3D sketch's edit: its constraint of the
// selection (section 7, decision g).
bool SketcherGui::runSketch3DConstraint(const char* commandName)
{
    auto vp = activeSketch3DInEdit();
    if (!vp) {
        return false;
    }
    static const std::map<std::string, Sketcher::ConstraintType> types = {
        {"Sketcher_ConstrainCoincidentUnified", Sketcher::Coincident},
        {"Sketcher_ConstrainCoincident", Sketcher::Coincident},
        {"Sketcher_ConstrainPointOnObject", Sketcher::PointOnObject},
        {"Sketcher_ConstrainParallel", Sketcher::Parallel},
        {"Sketcher_ConstrainPerpendicular", Sketcher::Perpendicular},
        {"Sketcher_ConstrainTangent", Sketcher::Tangent},
        {"Sketcher_ConstrainEqual", Sketcher::Equal},
        {"Sketcher_ConstrainSymmetric", Sketcher::Symmetric},
        {"Sketcher_ConstrainDistance", Sketcher::Distance},
        {"Sketcher_ConstrainDistanceX", Sketcher::DistanceX},
        {"Sketcher_ConstrainDistanceY", Sketcher::DistanceY},
        {"Sketcher_ConstrainDistanceZ", Sketcher::DistanceZ},
        {"Sketcher_ConstrainAngle", Sketcher::Angle},
        {"Sketcher_ConstrainRadius", Sketcher::Radius},
        {"Sketcher_ConstrainDiameter", Sketcher::Diameter},
        {"Sketcher_ConstrainBlock", Sketcher::Block},
        {"Sketcher_ConstrainHorVer", Sketcher::Horizontal},
        {"Sketcher_ConstrainHorizontal", Sketcher::Horizontal},
        {"Sketcher_ConstrainVertical", Sketcher::Vertical},
        {"Sketcher_Constrain3DAlongX", Sketcher::AlongX},
        {"Sketcher_Constrain3DAlongY", Sketcher::AlongY},
        {"Sketcher_Constrain3DAlongZ", Sketcher::AlongZ},
    };
    QString why;
    bool ok;
    std::string name(commandName);
    if (name == "Sketcher_ConstrainLock") {
        ok = vp->lockSelection(&why);
    }
    else if (name == "Sketcher_ConstrainRadiam") {
        // a diameter for whole circles, a radius otherwise, as the 2D one decides
        bool circles = !vp->selectedElements().empty();
        for (const auto& e : vp->selectedElements()) {
            auto geo = vp->getSketch3DObject()->getGeometry(e.geoId);
            circles = circles && geo && geo->isDerivedFrom<Part::GeomCircle>();
        }
        ok = vp->constrainSelection(circles ? Sketcher::Diameter : Sketcher::Radius, &why);
    }
    else if (auto it = types.find(name); it != types.end()) {
        ok = vp->constrainSelection(it->second, &why);
    }
    else {
        ok = false;
        why = QObject::tr("Not a constraint the 3D sketch has");
    }
    if (!ok && !why.isEmpty()) {
        Gui::getMainWindow()->showMessage(why, 5000);
        Base::Console().warning("%s\n", why.toUtf8().constData());
    }
    return true;
}

namespace
{
class CmdSketcherConstrain3D: public Gui::Command
{
public:
    CmdSketcherConstrain3D(const char* name,
                           const char* menu,
                           const char* tip,
                           const char* accel = nullptr)
        : Command(name)
    {
        sAppModule = "Sketcher";
        sGroup = "Sketcher";
        sMenuText = menu;
        sToolTipText = tip;
        sWhatsThis = name;
        sStatusTip = sToolTipText;
        if (accel) {
            sAccel = accel;
        }
    }
    const char* className() const override
    {
        return "CmdSketcherConstrain3D";
    }

protected:
    void activated(int /*iMsg*/) override
    {
        runSketch3DConstraint(getName());
    }
    bool isActive() override
    {
        return activeSketch3DInEdit() != nullptr;
    }
};
}  // namespace

void CreateSketcherCommandsSketch3D()
{
    Gui::CommandManager& rcCmdMgr = Gui::Application::Instance->commandManager();
    rcCmdMgr.addCommand(new CmdSketcherNewSketch3D());
    rcCmdMgr.addCommand(new CmdSketcherSketch3DCyclePlane());
    rcCmdMgr.addCommand(new CmdSketcherSketch3DLockHeight());
    rcCmdMgr.addCommand(new CmdSketcherSketch3DResetManipulator());
    rcCmdMgr.addCommand(new CmdSketcherSketch3DRotationHandles());
    rcCmdMgr.addCommand(new CmdSketcherConstrain3D(
        "Sketcher_Constrain3DAlongX",
        QT_TR_NOOP("Along X"),
        QT_TR_NOOP("Constrains lines, or two points, to run along the 3D sketch's X axis")));
    rcCmdMgr.addCommand(new CmdSketcherConstrain3D(
        "Sketcher_Constrain3DAlongY",
        QT_TR_NOOP("Along Y"),
        QT_TR_NOOP("Constrains lines, or two points, to run along the 3D sketch's Y axis")));
    rcCmdMgr.addCommand(new CmdSketcherConstrain3D(
        "Sketcher_Constrain3DAlongZ",
        QT_TR_NOOP("Along Z"),
        QT_TR_NOOP("Constrains lines, or two points, to run along the 3D sketch's Z axis")));
    rcCmdMgr.addCommand(new CmdSketcherConstrain3D(
        "Sketcher_ConstrainDistanceZ",
        QT_TR_NOOP("Distance Z"),
        QT_TR_NOOP("Fixes the Z distance of a line, two points or a point in a 3D sketch")));
}
