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

// Sheet metal's commands in PartDesign (docs/SheetMetal.md sec 5, 8.1)

#include "PreCompiled.h"

#include <iomanip>
#include <sstream>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>

#include <boost/algorithm/string/predicate.hpp>

#include <TopoDS.hxx>

#include <App/Document.h>
#include <App/DocumentObserver.h>
#include <Base/Console.h>
#include <Base/Tools.h>
#include <Gui/Action.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/CommandT.h>
#include <Gui/FileDialog.h>
#include <Gui/Macro.h>
#include <Gui/Control.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection.h>
#include <Gui/SelectionObject.h>
#include <Mod/Part/App/Part2DObject.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/FeatureSheetMetalFlatPattern.h>
#include <Mod/PartDesign/App/FeatureSheetMetalRule.h>
#include <Mod/PartDesign/App/SheetMetal/Recognise.h>
#include <Mod/PartDesign/App/SheetMetalExtension.h>
#include <Mod/PartDesign/App/SheetMetalFlat.h>
#include <Mod/PartDesign/App/SolidBody.h>

#include "TaskSheetMetalCheck.h"
#include "Utils.h"

// Command.cpp's: recompute, look as the base, edit if it has a panel
void finishFeature(const Gui::Command* cmd,
                   App::DocumentObject* Feat,
                   App::DocumentObject* prevSolidFeature = nullptr,
                   const bool hidePrevSolid = true,
                   const bool updateDocument = true);

namespace
{

/// Said in the report view, not a dialog: the commands are run from a
/// browser's connection too (docs/SheetMetal.md sec 8.2)
void warn(const char* title, const char* text)
{
    Base::Console().userWarning("%s: %s\n",
                                QObject::tr(title).toUtf8().constData(),
                                QObject::tr(text).toUtf8().constData());
}

/// The active body, or the document's only one; said in the report view if
/// there is none
PartDesign::Body* activeBody()
{
    PartDesign::Body* body = PartDesignGui::getBody(/*messageIfNot*/ false);
    if (!body) {
        warn(QT_TR_NOOP("No active body"),
             QT_TR_NOOP("Make a body active, or make one, for the sheet metal feature."));
    }
    return body;
}

/// The feature's panel, if it opened, commits on OK and takes it away on
/// Cancel; without one the command commits now
void commitUnlessEditing(Gui::Command* cmd)
{
    if (!Gui::Control().activeDialog()) {
        cmd->commitCommand();
    }
}

/// Make a feature of that type in the body; null if it was not made
App::DocumentObject* makeFeature(PartDesign::Body* body, const char* type, const char* name)
{
    const std::string unique = Gui::Command::getUniqueObjectName(name, body);
    Gui::cmdAppObject(body,
                      std::ostringstream() << "newObject('" << type << "','" << unique << "')");
    return body->getDocument()->getObject(unique.c_str());
}

}  // namespace

//===========================================================================
// PartDesign_SheetMetalBaseWall
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalBaseWall)

CmdPartDesignSheetMetalBaseWall::CmdPartDesignSheetMetalBaseWall()
    : Command("PartDesign_SheetMetalBaseWall")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Base Wall");
    sToolTipText = QT_TR_NOOP("A sheet's first wall from the selected sketch: a plate from a\n"
                              "closed one, a profile wall bent at each corner from an open one");
    sWhatsThis = "PartDesign_SheetMetalBaseWall";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalBaseWall";
}

void CmdPartDesignSheetMetalBaseWall::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* sketch = nullptr;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        if (sel.getObject() && sel.getObject()->isDerivedFrom<Part::Part2DObject>()) {
            sketch = sel.getObject();
            break;
        }
    }
    if (!sketch) {
        warn(QT_TR_NOOP("No sketch"),
             QT_TR_NOOP("Select a sketch for the base wall: a closed one for a plate, an open "
                        "one of lines for a profile wall."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Base Wall"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalBaseWall", "BaseWall");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat, std::ostringstream() << "Profile = " << getObjectCmd(sketch));
    FCMD_OBJ_HIDE(sketch);
    finishFeature(this, feat);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalBaseWall::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalFlange
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalFlange)

CmdPartDesignSheetMetalFlange::CmdPartDesignSheetMetalFlange()
    : Command("PartDesign_SheetMetalFlange")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Flange");
    sToolTipText = QT_TR_NOOP("A bend and a leg on each selected edge where a wall's skin\n"
                              "meets its end, toward the side of that skin");
    sWhatsThis = "PartDesign_SheetMetalFlange";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalFlange";
}

void CmdPartDesignSheetMetalFlange::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* base = nullptr;
    std::ostringstream edges;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        for (const auto& sub : sel.getSubNames()) {
            if (!boost::starts_with(sub, "Edge")) {
                continue;
            }
            if (base && base != sel.getObject()) {
                continue;
            }
            base = sel.getObject();
            edges << "'" << sub << "', ";
        }
    }
    if (!base || base != body->Tip.getValue()) {
        warn(QT_TR_NOOP("No edge"),
             QT_TR_NOOP("Select edges of the body's tip, each where a wall's skin meets its "
                        "end."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Flange"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalFlange", "Flange");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream()
                          << "Edges = (" << getObjectCmd(base) << ", [" << edges.str() << "])");
    finishFeature(this, feat, base);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalFlange::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalContourFlange
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalContourFlange)

CmdPartDesignSheetMetalContourFlange::CmdPartDesignSheetMetalContourFlange()
    : Command("PartDesign_SheetMetalContourFlange")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Contour Flange");
    sToolTipText = QT_TR_NOOP("The selected sketch's chain of lines on each selected edge,\n"
                              "sketched square to an edge from it: a bend there and at each "
                              "corner");
    sWhatsThis = "PartDesign_SheetMetalContourFlange";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalContourFlange";
}

void CmdPartDesignSheetMetalContourFlange::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* base = nullptr;
    App::DocumentObject* sketch = nullptr;
    std::ostringstream edges;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        if (!sketch && sel.getObject() && sel.getObject()->isDerivedFrom<Part::Part2DObject>()) {
            sketch = sel.getObject();
            continue;
        }
        for (const auto& sub : sel.getSubNames()) {
            if (!boost::starts_with(sub, "Edge")) {
                continue;
            }
            if (base && base != sel.getObject()) {
                continue;
            }
            base = sel.getObject();
            edges << "'" << sub << "', ";
        }
    }
    if (!base || base != body->Tip.getValue() || !sketch) {
        warn(QT_TR_NOOP("No edge and sketch"),
             QT_TR_NOOP("Select edges of the body's tip, each where a wall's skin meets its end, "
                        "and an open sketch of lines drawn square to one of them, starting on "
                        "it."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Contour Flange"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalContourFlange", "ContourFlange");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream()
                          << "Edges = (" << getObjectCmd(base) << ", [" << edges.str() << "])");
    Gui::cmdAppObject(feat, std::ostringstream() << "Profile = " << getObjectCmd(sketch));
    FCMD_OBJ_HIDE(sketch);
    finishFeature(this, feat, base);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalContourFlange::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalJoin
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalJoin)

CmdPartDesignSheetMetalJoin::CmdPartDesignSheetMetalJoin()
    : Command("PartDesign_SheetMetalJoin")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Join by Bend");
    sToolTipText = QT_TR_NOOP("Joins the walls of the two selected edges by a bend, each wall\n"
                              "extended or trimmed to it");
    sWhatsThis = "PartDesign_SheetMetalJoin";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalJoin";
}

void CmdPartDesignSheetMetalJoin::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* base = nullptr;
    std::ostringstream edges;
    int count = 0;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        for (const auto& sub : sel.getSubNames()) {
            if (!boost::starts_with(sub, "Edge") || (base && base != sel.getObject())) {
                continue;
            }
            base = sel.getObject();
            edges << "'" << sub << "', ";
            ++count;
        }
    }
    if (!base || base != body->Tip.getValue() || count != 2) {
        warn(QT_TR_NOOP("No two edges"),
             QT_TR_NOOP("Select two edges of the body's tip, one at the end of each wall to "
                        "join, where a skin meets it."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Join by Bend"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalJoin", "Join");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream()
                          << "Edges = (" << getObjectCmd(base) << ", [" << edges.str() << "])");
    finishFeature(this, feat, base);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalJoin::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalBendCuts
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalBendCuts)

CmdPartDesignSheetMetalBendCuts::CmdPartDesignSheetMetalBendCuts()
    : Command("PartDesign_SheetMetalBendCuts")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Bend Cuts");
    sToolTipText = QT_TR_NOOP("Cuts slots across the bends of the selected faces, to bend them\n"
                              "by hand, or a living hinge; drawn in the flat");
    sWhatsThis = "PartDesign_SheetMetalBendCuts";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalBendCuts";
}

void CmdPartDesignSheetMetalBendCuts::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* base = nullptr;
    std::ostringstream faces;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        for (const auto& sub : sel.getSubNames()) {
            if (!boost::starts_with(sub, "Face") || (base && base != sel.getObject())) {
                continue;
            }
            base = sel.getObject();
            faces << "'" << sub << "', ";
        }
    }
    if (!base || base != body->Tip.getValue()) {
        warn(QT_TR_NOOP("No bend"),
             QT_TR_NOOP("Select a face of each bend to cut, on the body's tip."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Bend Cuts"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalBendCuts", "BendCuts");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream()
                          << "Bends = (" << getObjectCmd(base) << ", [" << faces.str() << "])");
    finishFeature(this, feat, base);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalBendCuts::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalExtendWall
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalExtendWall)

CmdPartDesignSheetMetalExtendWall::CmdPartDesignSheetMetalExtendWall()
    : Command("PartDesign_SheetMetalExtendWall")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Extend Wall");
    sToolTipText = QT_TR_NOOP("Carries the selected ends of walls out by a length, or up to a\n"
                              "face; a negative length cuts the wall back");
    sWhatsThis = "PartDesign_SheetMetalExtendWall";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalExtendWall";
}

void CmdPartDesignSheetMetalExtendWall::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* base = nullptr;
    std::ostringstream faces;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        for (const auto& sub : sel.getSubNames()) {
            if (!boost::starts_with(sub, "Face") || (base && base != sel.getObject())) {
                continue;
            }
            base = sel.getObject();
            faces << "'" << sub << "', ";
        }
    }
    if (!base || base != body->Tip.getValue()) {
        warn(QT_TR_NOOP("No wall end"),
             QT_TR_NOOP("Select ends of walls on the body's tip, each a face across the sheet."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Extend Wall"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalExtendWall", "ExtendWall");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream()
                          << "Faces = (" << getObjectCmd(base) << ", [" << faces.str() << "])");
    finishFeature(this, feat, base);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalExtendWall::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalHem
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalHem)

CmdPartDesignSheetMetalHem::CmdPartDesignSheetMetalHem()
    : Command("PartDesign_SheetMetalHem")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Hem");
    sToolTipText = QT_TR_NOOP("Folds the end of a wall back over its skin at each selected edge\n"
                              "where a wall's skin meets its end, closed or open");
    sWhatsThis = "PartDesign_SheetMetalHem";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalHem";
}

void CmdPartDesignSheetMetalHem::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* base = nullptr;
    std::ostringstream edges;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        for (const auto& sub : sel.getSubNames()) {
            if (!boost::starts_with(sub, "Edge") || (base && base != sel.getObject())) {
                continue;
            }
            base = sel.getObject();
            edges << "'" << sub << "', ";
        }
    }
    if (!base || base != body->Tip.getValue()) {
        warn(QT_TR_NOOP("No edge"),
             QT_TR_NOOP("Select edges of the body's tip, each where a wall's skin meets its "
                        "end."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Hem"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalHem", "Hem");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream()
                          << "Edges = (" << getObjectCmd(base) << ", [" << edges.str() << "])");
    finishFeature(this, feat, base);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalHem::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalCutout
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalCutout)

CmdPartDesignSheetMetalCutout::CmdPartDesignSheetMetalCutout()
    : Command("PartDesign_SheetMetalCutout")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Cutout");
    sToolTipText = QT_TR_NOOP("Cuts the selected closed sketch through the sheet, normal to it\n"
                              "on every wall and bend in the sketch's way");
    sWhatsThis = "PartDesign_SheetMetalCutout";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalCutout";
}

void CmdPartDesignSheetMetalCutout::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* sketch = nullptr;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        if (sel.getObject() && sel.getObject()->isDerivedFrom<Part::Part2DObject>()) {
            sketch = sel.getObject();
            break;
        }
    }
    if (!sketch) {
        warn(QT_TR_NOOP("No sketch"),
             QT_TR_NOOP("Select a closed sketch to cut through the sheet."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Cutout"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalCutout", "Cutout");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat, std::ostringstream() << "Profile = " << getObjectCmd(sketch));
    FCMD_OBJ_HIDE(sketch);
    finishFeature(this, feat);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalCutout::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalTab
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalTab)

CmdPartDesignSheetMetalTab::CmdPartDesignSheetMetalTab()
    : Command("PartDesign_SheetMetalTab")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Tab");
    sToolTipText = QT_TR_NOOP("Adds the selected closed sketch to the wall it lies on, as a\n"
                              "plate of the sheet's thickness");
    sWhatsThis = "PartDesign_SheetMetalTab";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalTab";
}

void CmdPartDesignSheetMetalTab::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* sketch = nullptr;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        if (sel.getObject() && sel.getObject()->isDerivedFrom<Part::Part2DObject>()) {
            sketch = sel.getObject();
            break;
        }
    }
    if (!sketch) {
        warn(QT_TR_NOOP("No sketch"),
             QT_TR_NOOP("Select a closed sketch lying on a wall to add it there."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Tab"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalTab", "Tab");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat, std::ostringstream() << "Profile = " << getObjectCmd(sketch));
    FCMD_OBJ_HIDE(sketch);
    finishFeature(this, feat);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalTab::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalConvert
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalConvert)

CmdPartDesignSheetMetalConvert::CmdPartDesignSheetMetalConvert()
    : Command("PartDesign_SheetMetalConvert")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Convert");
    sToolTipText = QT_TR_NOOP("Takes a part made elsewhere as sheet metal: a thin part as it\n"
                              "is, a solid or a shell thickened, its sharp edges bends. Select\n"
                              "the face the flat lies on first, then any faces to leave open and\n"
                              "edges to rip");
    sWhatsThis = "PartDesign_SheetMetalConvert";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalConvert";
}

void CmdPartDesignSheetMetalConvert::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    // the faces and edges picked on one part, in the order picked: the
    // first face the fixed one
    App::DocumentObject* part = nullptr;
    std::vector<std::string> faces, edges;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        if (!sel.getObject() || (part && part != sel.getObject())) {
            continue;
        }
        for (const auto& sub : sel.getSubNames()) {
            if (boost::starts_with(sub, "Face")) {
                faces.push_back(sub);
                part = sel.getObject();
            }
            else if (boost::starts_with(sub, "Edge")) {
                edges.push_back(sub);
                part = sel.getObject();
            }
        }
    }
    if (!part || faces.empty()) {
        warn(QT_TR_NOOP("No face"),
             QT_TR_NOOP("Select a face of the part, a wall's skin, for the flat to lie on; then "
                        "any faces to leave open and edges to rip."));
        return;
    }
    auto list = [](auto begin, auto end) {
        std::ostringstream res;
        for (auto it = begin; it != end; ++it) {
            res << "'" << *it << "', ";
        }
        return res.str();
    };
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Convert"));
    // a part in no body: a body made on it, as Body does with a solid
    // selected
    auto body = PartDesign::Body::findBodyOf(part);
    if (!body) {
        App::Document* doc = part->getDocument();
        const std::string name = getUniqueObjectName("Body");
        Gui::cmdAppDocument(
            doc, std::ostringstream() << "addObject('PartDesign::Body','" << name << "')");
        body = Base::freecad_dynamic_cast<PartDesign::Body>(doc->getObject(name.c_str()));
        if (!body) {
            abortCommand();
            return;
        }
        Gui::cmdAppObject(body, std::ostringstream() << "BaseFeature = " << getObjectCmd(part));
        addModule(Gui, "PartDesignGui");
        doCommand(Gui::Command::Gui,
                  "Gui.activeView().setActiveObject('%s', %s)",
                  PDBODYKEY,
                  getObjectCmd(body).c_str());
    }
    // what the body showed: the convert takes its place
    App::DocumentObject* before = body->Tip.getValue();
    auto feat = makeFeature(body, "PartDesign::SheetMetalConvert", "Convert");
    if (!feat) {
        abortCommand();
        return;
    }
    const std::string link = getObjectCmd(part);
    Gui::cmdAppObject(feat,
                      std::ostringstream() << "FixedFace = (" << link << ", ['" << faces.front()
                                           << "'])");
    if (faces.size() > 1) {
        Gui::cmdAppObject(feat,
                          std::ostringstream() << "OpenFaces = (" << link << ", ["
                                               << list(faces.begin() + 1, faces.end()) << "])");
    }
    if (!edges.empty()) {
        Gui::cmdAppObject(feat,
                          std::ostringstream() << "RipEdges = (" << link << ", ["
                                               << list(edges.begin(), edges.end()) << "])");
    }
    // A part picked with no face to open may be a thin part made elsewhere:
    // the sheet's rule, where it is the convert's own, takes the part's
    // thickness from it (docs/SheetMetal.md sec 5.3, Recognise). A rule
    // other features share is left as it is; the convert says if it differs.
    auto sheet = feat->getExtensionByType<PartDesign::SheetMetalExtension>(true);
    auto rule = sheet ? sheet->getRule() : nullptr;
    const auto users = rule ? rule->getInList() : std::vector<App::DocumentObject*>();
    const bool own = std::none_of(users.begin(), users.end(), [&](App::DocumentObject* o) {
        return o != feat
            && o->hasExtension(PartDesign::SheetMetalExtension::getExtensionClassTypeId());
    });
    if (rule && own && faces.size() == 1) {
        const Part::TopoShape shape = Part::Feature::getTopoShape(part);
        const TopoDS_Shape face = shape.getSubShape(faces.front().c_str(), true);
        const double thickness = face.IsNull() || face.ShapeType() != TopAbs_FACE
            ? -1
            : PartDesign::SheetMetal::facingThickness(shape.getShape(), TopoDS::Face(face));
        if (thickness > 0 && std::abs(thickness - rule->Thickness.getValue()) > 1e-9) {
            Gui::cmdAppObject(rule, std::ostringstream() << std::setprecision(17)
                                                         << "Thickness = " << thickness);
        }
    }
    finishFeature(this, feat, before ? before : part);
    // the part taken in, where it is not the body's own feature
    if (part != before) {
        FCMD_OBJ_HIDE(part);
    }
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalConvert::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalFold, PartDesign_SheetMetalJog
//===========================================================================

namespace
{

/// A fold or a jog on the sketch selected, a face selected too the one on
/// the side to move
void makeFold(Gui::Command* cmd, const char* type, const char* name, const char* undo,
              const char* hint)
{
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* sketch = nullptr;
    App::DocumentObject* faceOwner = nullptr;
    std::string face;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        App::DocumentObject* obj = sel.getObject();
        if (!obj) {
            continue;
        }
        if (obj->isDerivedFrom<Part::Part2DObject>()) {
            sketch = obj;
            continue;
        }
        for (const auto& sub : sel.getSubNames()) {
            if (face.empty() && sub.find("Face") != std::string::npos) {
                faceOwner = obj;
                face = sub;
            }
        }
    }
    if (!sketch) {
        warn(QT_TR_NOOP("No sketch"), hint);
        return;
    }
    Gui::Command::openCommand(undo);
    auto feat = makeFeature(body, type, name);
    if (!feat) {
        Gui::Command::abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream() << "Profile = " << Gui::Command::getObjectCmd(sketch));
    if (faceOwner) {
        Gui::cmdAppObject(feat,
                          std::ostringstream()
                              << "Moving = (" << Gui::Command::getObjectCmd(faceOwner) << ", ['"
                              << face << "'])");
    }
    FCMD_OBJ_HIDE(sketch);
    finishFeature(cmd, feat);
    commitUnlessEditing(cmd);
}

}  // namespace

DEF_STD_CMD_A(CmdPartDesignSheetMetalFold)

CmdPartDesignSheetMetalFold::CmdPartDesignSheetMetalFold()
    : Command("PartDesign_SheetMetalFold")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Fold");
    sToolTipText = QT_TR_NOOP("Bends a wall along the line of the selected sketch; a face\n"
                              "selected too says which side moves, else the smaller one does");
    sWhatsThis = "PartDesign_SheetMetalFold";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalFold";
}

void CmdPartDesignSheetMetalFold::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    makeFold(this,
             "PartDesign::SheetMetalFold",
             "Fold",
             QT_TRANSLATE_NOOP("Command", "Create Fold"),
             QT_TR_NOOP("Select a sketch of a line lying on a wall to fold it there, and a face "
                        "on the side to move if not the smaller."));
}

bool CmdPartDesignSheetMetalFold::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdPartDesignSheetMetalJog)

CmdPartDesignSheetMetalJog::CmdPartDesignSheetMetalJog()
    : Command("PartDesign_SheetMetalJog")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Jog");
    sToolTipText = QT_TR_NOOP("Steps a wall along the line of the selected sketch by two\n"
                              "opposite bends; a face selected too says which side moves,\n"
                              "else the smaller one does");
    sWhatsThis = "PartDesign_SheetMetalJog";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalJog";
}

void CmdPartDesignSheetMetalJog::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    makeFold(this,
             "PartDesign::SheetMetalJog",
             "Jog",
             QT_TRANSLATE_NOOP("Command", "Create Jog"),
             QT_TR_NOOP("Select a sketch of a line lying on a wall to step it there, and a face "
                        "on the side to move if not the smaller."));
}

bool CmdPartDesignSheetMetalJog::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalCornerClosure
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalCornerClosure)

CmdPartDesignSheetMetalCornerClosure::CmdPartDesignSheetMetalCornerClosure()
    : Command("PartDesign_SheetMetalCornerClosure")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Corner Closure");
    sToolTipText = QT_TR_NOOP("Carries two flanges' legs on to close the corner between them:\n"
                              "select the end of each leg there");
    sWhatsThis = "PartDesign_SheetMetalCornerClosure";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalCornerClosure";
}

void CmdPartDesignSheetMetalCornerClosure::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* base = nullptr;
    std::ostringstream faces;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        for (const auto& sub : sel.getSubNames()) {
            if (!boost::starts_with(sub, "Face") || (base && base != sel.getObject())) {
                continue;
            }
            base = sel.getObject();
            faces << "'" << sub << "', ";
        }
    }
    if (!base || base != body->Tip.getValue()) {
        warn(QT_TR_NOOP("No leg end"),
             QT_TR_NOOP("Select the end of each of two flanges' legs at a corner, on the "
                        "body's tip."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Corner Closure"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalCornerClosure", "CornerClosure");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream()
                          << "Faces = (" << getObjectCmd(base) << ", [" << faces.str() << "])");
    finishFeature(this, feat, base);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalCornerClosure::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalRip
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalRip)

CmdPartDesignSheetMetalRip::CmdPartDesignSheetMetalRip()
    : Command("PartDesign_SheetMetalRip")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Rip");
    sToolTipText = QT_TR_NOOP("Cuts a closed sheet, a tube, open so it lies flat: select a face\n"
                              "of a bend to cut it open at its corner, or of a wall to cut it\n"
                              "down its middle");
    sWhatsThis = "PartDesign_SheetMetalRip";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalRip";
}

void CmdPartDesignSheetMetalRip::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    PartDesign::Body* body = activeBody();
    if (!body) {
        return;
    }
    App::DocumentObject* base = nullptr;
    std::ostringstream faces;
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        for (const auto& sub : sel.getSubNames()) {
            if (!boost::starts_with(sub, "Face") || (base && base != sel.getObject())) {
                continue;
            }
            base = sel.getObject();
            faces << "'" << sub << "', ";
        }
    }
    if (!base || base != body->Tip.getValue()) {
        warn(QT_TR_NOOP("Nothing to rip"),
             QT_TR_NOOP("Select a face of a bend or of a wall of a closed sheet, on the body's "
                        "tip."));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Create Rip"));
    auto feat = makeFeature(body, "PartDesign::SheetMetalRip", "Rip");
    if (!feat) {
        abortCommand();
        return;
    }
    Gui::cmdAppObject(feat,
                      std::ostringstream()
                          << "Faces = (" << getObjectCmd(base) << ", [" << faces.str() << "])");
    finishFeature(this, feat, base);
    commitUnlessEditing(this);
}

bool CmdPartDesignSheetMetalRip::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// PartDesign_SheetMetalFlatten
//===========================================================================

namespace
{
/// The sheet of the active body's tip, null if it is no sheet's; says nothing
PartDesign::SolidBody* activeSheet()
{
    auto body = PartDesignGui::getBody(/*messageIfNot*/ false, /*autoActivate*/ false);
    auto solid = body ? body->getTipSolidBody() : nullptr;
    return solid && solid->isSheetMetal() ? solid : nullptr;
}
}  // namespace

class CmdPartDesignSheetMetalFlatten: public Gui::Command
{
public:
    CmdPartDesignSheetMetalFlatten();
    const char* className() const override
    {
        return "CmdPartDesignSheetMetalFlatten";
    }

protected:
    void activated(int iMsg) override;
    bool isActive() override;
    Gui::Action* createAction() override;

private:
    /// The features shown when the sheet was flattened, shown again after
    std::vector<App::DocumentObjectT> folded;
};

CmdPartDesignSheetMetalFlatten::CmdPartDesignSheetMetalFlatten()
    : Command("PartDesign_SheetMetalFlatten")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Flatten");
    sToolTipText = QT_TR_NOOP("Shows the active body's sheet laid flat, its flat object, in\n"
                              "place of the folded part; again to fold it back");
    sWhatsThis = "PartDesign_SheetMetalFlatten";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalFlatten";
}

void CmdPartDesignSheetMetalFlatten::activated(int iMsg)
{
    // The flat object's visibility (docs/SheetMetal.md sec 12, ruled
    // 2026-11-04): no undo step, as no visibility is
    auto solid = activeSheet();
    auto flat = solid ? solid->flatPattern() : nullptr;
    if (!flat) {
        warn(QT_TR_NOOP("No sheet"),
             QT_TR_NOOP("Make a body of sheet metal active to lay it flat."));
        return;
    }
    if (iMsg) {
        folded.clear();
        for (auto obj : solid->Group.getValues()) {
            if (obj && obj->Visibility.getValue()) {
                folded.emplace_back(obj);
                Gui::cmdAppObject(obj, "Visibility = False");
            }
        }
        Gui::cmdAppObject(flat, "Visibility = True");
        return;
    }
    Gui::cmdAppObject(flat, "Visibility = False");
    bool shown = false;
    for (auto& objT : folded) {
        auto obj = objT.getObject();
        if (obj && solid->hasFeature(obj)) {
            Gui::cmdAppObject(obj, "Visibility = True");
            shown = true;
        }
    }
    folded.clear();
    // Else the tip, as the body would show
    if (!shown && solid->Tip.getValue()) {
        Gui::cmdAppObject(solid->Tip.getValue(), "Visibility = True");
    }
}

bool CmdPartDesignSheetMetalFlatten::isActive()
{
    auto solid = activeSheet();
    auto flat = solid ? solid->flatPattern() : nullptr;
    if (_pcAction) {
        bool on = flat && flat->Visibility.getValue();
        if (_pcAction->isChecked() != on)
            _pcAction->setChecked(on, true);
    }
    return flat != nullptr;
}

Gui::Action* CmdPartDesignSheetMetalFlatten::createAction()
{
    Gui::Action* action = Command::createAction();
    action->setCheckable(true);
    return action;
}

//===========================================================================
// PartDesign_SheetMetalExportDXF
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalExportDXF)

CmdPartDesignSheetMetalExportDXF::CmdPartDesignSheetMetalExportDXF()
    : Command("PartDesign_SheetMetalExportDXF")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Export Flat...");
    sToolTipText = QT_TR_NOOP("Writes the flat of the selected feature or sheet, else of the\n"
                              "active body's sheet, as DXF or SVG: the outline, the inner\n"
                              "contours, and the bend lines up and down on layers of their own");
    sWhatsThis = "PartDesign_SheetMetalExportDXF";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalExportDXF";
}

namespace
{
/// What Export Flat writes: a selected feature or solid of a sheet, else the
/// active body's sheet; null if none
App::DocumentObject* flatToExport()
{
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        auto obj = sel.getObject();
        // the flat object writes its sheet's
        if (dynamic_cast<PartDesign::SheetMetalFlatPattern*>(obj)) {
            return obj;
        }
        auto solid = dynamic_cast<PartDesign::SolidBody*>(obj);
        if (!solid) {
            auto body = PartDesign::Body::findBodyOf(obj);
            solid = body ? body->getSolidBody(obj) : nullptr;
        }
        if (solid && solid->isSheetMetal()) {
            return obj;
        }
    }
    return activeSheet();
}
}  // namespace

void CmdPartDesignSheetMetalExportDXF::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    auto flat = flatToExport();
    if (!flat) {
        return;
    }
    const QString svg = QStringLiteral("SVG (*.svg)");
    QString filter;
    QString path = Gui::FileDialog::getSaveFileName(
        Gui::getMainWindow(),
        QObject::tr("Export Flat"),
        QString::fromUtf8(flat->Label.getValue()) + QStringLiteral(".dxf"),
        QStringLiteral("DXF (*.dxf);;") + svg,
        &filter);
    if (path.isEmpty()) {
        return;
    }
    // by the name's suffix, else by the filter chosen
    const bool asSVG = path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)
        || (filter == svg && !path.endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive));
    if (asSVG && !path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".svg");
    }
    const std::string file = Base::Tools::escapeEncodeFilename(path.toStdString());
    doCommand(Doc, "import PartDesign");
    doCommand(Doc, "PartDesign.exportFlat(%s, \"%s\")", getObjectCmd(flat).c_str(), file.c_str());
}

bool CmdPartDesignSheetMetalExportDXF::isActive()
{
    return flatToExport() != nullptr;
}

//===========================================================================
// PartDesign_SheetMetalExportFlats
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalExportFlats)

CmdPartDesignSheetMetalExportFlats::CmdPartDesignSheetMetalExportFlats()
    : Command("PartDesign_SheetMetalExportFlats")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Export Every Flat...");
    sToolTipText = QT_TR_NOOP("Writes the flat of every sheet in the document into a folder,\n"
                              "one DXF or SVG file each, named after its body");
    sWhatsThis = "PartDesign_SheetMetalExportFlats";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalExportDXF";
}

namespace
{
/// Whether the document holds a sheet
bool hasSheet(const App::Document* doc)
{
    if (!doc) {
        return false;
    }
    for (auto obj : doc->getObjectsOfType(PartDesign::SolidBody::getClassTypeId())) {
        if (static_cast<PartDesign::SolidBody*>(obj)->isSheetMetal()) {
            return true;
        }
    }
    return false;
}
}  // namespace

void CmdPartDesignSheetMetalExportFlats::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    App::Document* doc = App::GetApplication().getActiveDocument();
    if (!hasSheet(doc)) {
        return;
    }
    // the folder and format last used, else the document's folder
    auto hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/PartDesign/SheetMetal");
    QString folder = QString::fromUtf8(hGrp->GetASCII("ExportFolder", "").c_str());
    if (folder.isEmpty()) {
        const QString file = QString::fromUtf8(doc->FileName.getValue());
        folder = file.isEmpty() ? QDir::homePath() : QFileInfo(file).absolutePath();
    }

    QDialog dlg(Gui::getMainWindow());
    dlg.setObjectName(QStringLiteral("SheetMetalExportFlats"));
    dlg.setWindowTitle(QObject::tr("Export Every Flat"));
    auto form = new QFormLayout(&dlg);
    auto chooser = new Gui::FileChooser(&dlg);
    chooser->setObjectName(QStringLiteral("folder"));
    chooser->setMode(Gui::FileChooser::Directory);
    chooser->setFileName(folder);
    form->addRow(QObject::tr("Folder"), chooser);
    auto format = new QComboBox(&dlg);
    format->setObjectName(QStringLiteral("format"));
    format->addItems({QStringLiteral("DXF"), QStringLiteral("SVG")});
    format->setCurrentIndex(hGrp->GetASCII("ExportFormat", "dxf") == "svg" ? 1 : 0);
    form->addRow(QObject::tr("Format"), format);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    dlg.resize(480, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    const std::string dir = chooser->fileName().toUtf8().constData();
    const std::string ext = format->currentIndex() == 1 ? "svg" : "dxf";
    hGrp->SetASCII("ExportFolder", dir.c_str());
    hGrp->SetASCII("ExportFormat", ext.c_str());

    // the call, as the macro has it
    std::ostringstream line;
    line << "PartDesign.exportFlats(App.getDocument(\"" << doc->getName() << "\"), \""
         << Base::Tools::escapeEncodeFilename(dir) << "\", \"" << ext << "\")";
    auto macro = Gui::Application::Instance->macroManager();
    macro->addLine(Gui::MacroManager::App, "import PartDesign");
    macro->addLine(Gui::MacroManager::App, line.str().c_str());
    std::vector<PartDesign::FlatExport> written;
    try {
        written = PartDesign::exportFlats(doc, dir.c_str(), ext.c_str());
    }
    catch (const Base::Exception& e) {
        warn(QT_TRANSLATE_NOOP("Command", "Export Every Flat"), e.what());
        return;
    }
    for (const auto& item : written) {
        const std::string label = item.sheet ? item.sheet->getFullLabel() : std::string("?");
        if (item.error.empty()) {
            Base::Console().message("%s: %s\n",
                                    QObject::tr("Export Every Flat").toUtf8().constData(),
                                    item.path.c_str());
        }
        else {
            Base::Console().userWarning("%s: %s: %s\n",
                                        QObject::tr("Export Every Flat").toUtf8().constData(),
                                        label.c_str(),
                                        item.error.c_str());
        }
    }
}

bool CmdPartDesignSheetMetalExportFlats::isActive()
{
    return hasSheet(App::GetApplication().getActiveDocument());
}

//===========================================================================
// PartDesign_SheetMetalCheck
//===========================================================================

DEF_STD_CMD_A(CmdPartDesignSheetMetalCheck)

CmdPartDesignSheetMetalCheck::CmdPartDesignSheetMetalCheck()
    : Command("PartDesign_SheetMetalCheck")
{
    sAppModule = "PartDesign";
    sGroup = QT_TR_NOOP("PartDesign");
    sMenuText = QT_TR_NOOP("Check Sheet");
    sToolTipText = QT_TR_NOOP("Checks the selected feature or sheet, else the active body's\n"
                              "sheet: its bend table, and flanges too short, holes too near a\n"
                              "bend, reliefs too small and material overlapping in the flat");
    sWhatsThis = "PartDesign_SheetMetalCheck";
    sStatusTip = sToolTipText;
    sPixmap = "PartDesign_SheetMetalCheck";
}

void CmdPartDesignSheetMetalCheck::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    // what Export Flat would write
    auto obj = flatToExport();
    if (!obj || Gui::Control().activeDialog()) {
        return;
    }
    Gui::Control().showDialog(new PartDesignGui::TaskDlgSheetMetalCheck(obj));
}

bool CmdPartDesignSheetMetalCheck::isActive()
{
    return !Gui::Control().activeDialog() && flatToExport() != nullptr;
}

void CreatePartDesignSheetMetalCommands()
{
    Gui::CommandManager& mgr = Gui::Application::Instance->commandManager();
    mgr.addCommand(new CmdPartDesignSheetMetalBaseWall());
    mgr.addCommand(new CmdPartDesignSheetMetalFlange());
    mgr.addCommand(new CmdPartDesignSheetMetalContourFlange());
    mgr.addCommand(new CmdPartDesignSheetMetalJoin());
    mgr.addCommand(new CmdPartDesignSheetMetalBendCuts());
    mgr.addCommand(new CmdPartDesignSheetMetalExtendWall());
    mgr.addCommand(new CmdPartDesignSheetMetalHem());
    mgr.addCommand(new CmdPartDesignSheetMetalCornerClosure());
    mgr.addCommand(new CmdPartDesignSheetMetalRip());
    mgr.addCommand(new CmdPartDesignSheetMetalCutout());
    mgr.addCommand(new CmdPartDesignSheetMetalFold());
    mgr.addCommand(new CmdPartDesignSheetMetalJog());
    mgr.addCommand(new CmdPartDesignSheetMetalTab());
    mgr.addCommand(new CmdPartDesignSheetMetalConvert());
    mgr.addCommand(new CmdPartDesignSheetMetalFlatten());
    mgr.addCommand(new CmdPartDesignSheetMetalExportDXF());
    mgr.addCommand(new CmdPartDesignSheetMetalExportFlats());
    mgr.addCommand(new CmdPartDesignSheetMetalCheck());
}
