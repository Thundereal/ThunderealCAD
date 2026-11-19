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

#include <QMessageBox>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Gui/ActiveObjectList.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/MDIView.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionObject.h>
#include <Mod/Part/App/BodyBase.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PMI/App/AnnotationSet.h>
#include <Mod/PMI/App/Api.h>
#include <Mod/PMI/App/Check.h>
#include <Mod/PMI/App/Geometry.h>

#include "ViewProviderCallout.h"

// The PMI commands (docs/GDandT.md sec 5-8). Each resolves the faces picked
// to what an annotation refers to, in C++, then writes plain Python with
// the names in it -- the macro of a command replays it.

namespace
{
struct Pick
{
    App::DocumentObject* solid = nullptr;
    std::string face;
    PMI::Primitive surface;
};

/// The faces picked, as solid references; empty, with a message shown, if
/// the pick is not faces of one solid. A Body picked that has no annotation
/// set is given one first -- with it the Body gives its solid a solid body,
/// which is what the faces are named through (docs/GDandT.md sec 3).
std::vector<Pick> pickedFaces(Gui::Command* cmd, const char* what)
{
    std::vector<Pick> picks;
    std::string why;
    auto selection = Gui::Selection().getSelectionEx(nullptr,
                                                     App::DocumentObject::getClassTypeId(),
                                                     Gui::ResolveMode::NoResolve);
    bool made = false;
    for (auto& sel : selection) {
        for (auto& sub : sel.getSubNames()) {
            App::DocumentObject* owner = nullptr;
            Part::Feature::getTopoShape(sel.getObject(), sub.c_str(), true, nullptr, &owner);
            auto body = owner ? Part::BodyBase::findBodyOf(owner) : nullptr;
            if (body && !PMI::Api::getSet(body, false)) {
                cmd->doCommand(Gui::Command::Doc, "import PMI");
                cmd->doCommand(Gui::Command::Doc, "PMI.getSet(%s)",
                               body->getFullName(true).c_str());
                made = true;
            }
        }
    }
    if (made) {
        cmd->doCommand(Gui::Command::Doc, "App.ActiveDocument.recompute()");
    }
    for (auto& sel : selection) {
        for (auto& sub : sel.getSubNames()) {
            Pick pick;
            if (!PMI::Api::solidReference(sel.getObject(), sub, pick.solid, pick.face, why)) {
                QMessageBox::warning(Gui::getMainWindow(), QObject::tr("PMI"),
                                     QObject::tr("%1: %2").arg(QString::fromUtf8(what),
                                                               QString::fromUtf8(why.c_str())));
                return {};
            }
            auto shape = Part::Feature::getTopoShape(pick.solid, pick.face.c_str(), true);
            pick.surface = PMI::Geom::ofFace(shape.getShape());
            if (!picks.empty() && picks.front().solid != pick.solid) {
                QMessageBox::warning(Gui::getMainWindow(), QObject::tr("PMI"),
                                     QObject::tr("%1: pick faces of one solid")
                                         .arg(QString::fromUtf8(what)));
                return {};
            }
            picks.push_back(pick);
        }
    }
    if (picks.empty()) {
        QMessageBox::information(Gui::getMainWindow(), QObject::tr("PMI"),
                                 QObject::tr("%1: pick a face of a part first")
                                     .arg(QString::fromUtf8(what)));
    }
    return picks;
}

std::string pyObject(const App::DocumentObject* obj)
{
    return obj->getFullName(true);
}

std::string pyFaces(const std::vector<Pick>& picks, std::size_t from = 0, std::size_t to = ~0u)
{
    std::string s = "[";
    for (std::size_t i = from; i < picks.size() && i < to; ++i) {
        s += (i > from ? ", '" : "'") + picks[i].face + "'";
    }
    return s + "]";
}

/// The set and a feature on the picked faces, made if need be, as the
/// Python names _set and _feat
void makeFeature(Gui::Command* cmd, const std::vector<Pick>& picks, const char* kind,
                 std::size_t from = 0, std::size_t to = ~0u, const char* name = "_feat")
{
    cmd->doCommand(Gui::Command::Doc, "import PMI");
    cmd->doCommand(Gui::Command::Doc, "_set = PMI.getSet(%s)", pyObject(picks[0].solid).c_str());
    if (kind) {
        cmd->doCommand(Gui::Command::Doc, "%s = PMI.feature(_set, %s, %s, '%s')", name,
                       pyObject(picks[0].solid).c_str(), pyFaces(picks, from, to).c_str(), kind);
    }
    else {
        cmd->doCommand(Gui::Command::Doc, "%s = PMI.feature(_set, %s, %s)", name,
                       pyObject(picks[0].solid).c_str(), pyFaces(picks, from, to).c_str());
    }
}

void addToSet(Gui::Command* cmd, const char* type, const char* name)
{
    cmd->doCommand(Gui::Command::Doc, "_obj = _set.Document.addObject('PMI::%s', '%s')", type,
                   name);
    cmd->doCommand(Gui::Command::Doc, "_set.addObject(_obj)");
}

void finish(Gui::Command* cmd)
{
    cmd->doCommand(Gui::Command::Doc, "_set.Document.recompute()");
    cmd->doCommand(Gui::Command::Gui, "Gui.Selection.clearSelection()");
    cmd->doCommand(Gui::Command::Gui, "Gui.Selection.addSelection(_obj)");
    cmd->commitCommand();
}

bool haveDocument(Gui::Command* cmd)
{
    return cmd->getActiveGuiDocument() != nullptr;
}
}  // namespace

// ---- PMI_Datum ----

DEF_STD_CMD_A(CmdPMIDatum)

CmdPMIDatum::CmdPMIDatum()
    : Command("PMI_Datum")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("Datum feature");
    sToolTipText = QT_TR_NOOP("Make the picked face a datum feature, with the next free letter");
    sWhatsThis = "PMI_Datum";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_Datum";
}

void CmdPMIDatum::activated(int)
{
    openCommand(QT_TRANSLATE_NOOP("Command", "PMI datum"));
    auto picks = pickedFaces(this, "Datum");
    if (picks.empty()) {
        abortCommand();
        return;
    }
    // A cylinder's datum is its axis
    const char* kind = picks[0].surface.type == PMI::Primitive::Cylinder ? "Axis" : nullptr;
    makeFeature(this, picks, kind);
    addToSet(this, "Datum", "Datum");
    doCommand(Doc, "_obj.Letter = PMI.nextDatumLetter(_set, %s)",
              pyObject(picks[0].solid).c_str());
    doCommand(Doc, "_obj.Feature = _feat");
    finish(this);
}

bool CmdPMIDatum::isActive()
{
    return haveDocument(this);
}

// ---- PMI_GeometricTolerance ----

DEF_STD_CMD_A(CmdPMIGeometricTolerance)

CmdPMIGeometricTolerance::CmdPMIGeometricTolerance()
    : Command("PMI_GeometricTolerance")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("Geometric tolerance");
    sToolTipText = QT_TR_NOOP("A feature control frame on the picked faces: flatness of a plane, "
                              "position of a hole's axis, profile of anything else. Edit it in "
                              "its properties.");
    sWhatsThis = "PMI_GeometricTolerance";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_GeometricTolerance";
}

void CmdPMIGeometricTolerance::activated(int)
{
    openCommand(QT_TRANSLATE_NOOP("Command", "PMI geometric tolerance"));
    auto picks = pickedFaces(this, "Geometric tolerance");
    if (picks.empty()) {
        abortCommand();
        return;
    }
    const auto type = picks[0].surface.type;
    const char* kind = type == PMI::Primitive::Cylinder ? "Axis" : nullptr;
    makeFeature(this, picks, kind);
    addToSet(this, "GeometricTolerance", "Tolerance");
    if (type == PMI::Primitive::Plane && picks.size() == 1) {
        doCommand(Doc, "_obj.Characteristic = 'Flatness'");
    }
    else if (type == PMI::Primitive::Cylinder) {
        doCommand(Doc, "_obj.Characteristic = 'Position'");
        doCommand(Doc, "_obj.ZoneShape = 'Diameter'");
    }
    else {
        doCommand(Doc, "_obj.Characteristic = 'ProfileOfSurface'");
    }
    doCommand(Doc, "_obj.Feature = _feat");
    finish(this);
}

bool CmdPMIGeometricTolerance::isActive()
{
    return haveDocument(this);
}

// ---- PMI_Dimension ----

DEF_STD_CMD_A(CmdPMIDimension)

CmdPMIDimension::CmdPMIDimension()
    : Command("PMI_Dimension")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("Dimension");
    sToolTipText = QT_TR_NOOP("A dimension measured from the model: a hole's diameter, the width "
                              "between two opposed faces, the distance between two features");
    sWhatsThis = "PMI_Dimension";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_Dimension";
}

void CmdPMIDimension::activated(int)
{
    openCommand(QT_TRANSLATE_NOOP("Command", "PMI dimension"));
    auto picks = pickedFaces(this, "Dimension");
    if (picks.empty()) {
        abortCommand();
        return;
    }
    const auto& a = picks[0].surface;
    if (picks.size() == 1) {
        makeFeature(this, picks, nullptr);
        addToSet(this, "Dimension", "Dimension");
        doCommand(Doc, "_obj.Type = '%s'",
                  a.type == PMI::Primitive::Sphere ? "SphericalDiameter" : "Diameter");
        doCommand(Doc, "_obj.Features = [_feat]");
    }
    else {
        const auto& b = picks[1].surface;
        bool width = picks.size() == 2 && a.type == PMI::Primitive::Plane
            && b.type == PMI::Primitive::Plane && a.dir.IsOpposite(b.dir, 1e-9);
        if (width) {
            makeFeature(this, picks, nullptr);
            addToSet(this, "Dimension", "Dimension");
            doCommand(Doc, "_obj.Type = 'Width'");
            doCommand(Doc, "_obj.Features = [_feat]");
        }
        else {
            makeFeature(this, picks, a.type == PMI::Primitive::Cylinder ? "Axis" : nullptr, 0, 1,
                        "_first");
            makeFeature(this, picks, b.type == PMI::Primitive::Cylinder ? "Axis" : nullptr, 1, 2,
                        "_second");
            addToSet(this, "Dimension", "Dimension");
            bool parallel = a.dir.IsParallel(b.dir, 1e-9) || a.isAxial() || b.isAxial();
            doCommand(Doc, "_obj.Type = '%s'", parallel ? "Distance" : "Angle");
            doCommand(Doc, "_obj.Features = [_first, _second]");
        }
    }
    finish(this);
}

bool CmdPMIDimension::isActive()
{
    return haveDocument(this);
}

// ---- PMI_Note ----

DEF_STD_CMD_A(CmdPMINote)

CmdPMINote::CmdPMINote()
    : Command("PMI_Note")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("Note");
    sToolTipText = QT_TR_NOOP("A note on the picked face; type its text in its properties");
    sWhatsThis = "PMI_Note";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_Note";
}

void CmdPMINote::activated(int)
{
    openCommand(QT_TRANSLATE_NOOP("Command", "PMI note"));
    auto picks = pickedFaces(this, "Note");
    if (picks.empty()) {
        abortCommand();
        return;
    }
    makeFeature(this, picks, nullptr);
    addToSet(this, "Note", "Note");
    doCommand(Doc, "_obj.Text = ['NOTE']");
    doCommand(Doc, "_obj.Feature = _feat");
    finish(this);
}

bool CmdPMINote::isActive()
{
    return haveDocument(this);
}

// ---- PMI_Check ----

DEF_STD_CMD_A(CmdPMICheck)

CmdPMICheck::CmdPMICheck()
    : Command("PMI_Check")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("Check");
    sToolTipText = QT_TR_NOOP("Check every annotation of the document against its standard and "
                              "its model");
    sWhatsThis = "PMI_Check";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_Check";
}

void CmdPMICheck::activated(int)
{
    auto doc = getActiveGuiDocument();
    if (!doc) {
        return;
    }
    int errors = 0, warnings = 0;
    QStringList lines;
    for (auto obj : doc->getDocument()->getObjectsOfType(PMI::AnnotationSet::getClassTypeId())) {
        for (auto& issue : PMI::Check::set(static_cast<PMI::AnnotationSet*>(obj))) {
            bool error = issue.severity == PMI::Check::Error;
            (error ? errors : warnings)++;
            QString line = QStringLiteral("%1 %2: %3")
                               .arg(error ? QStringLiteral("Error") : QStringLiteral("Warning"),
                                    issue.object
                                        ? QString::fromUtf8(issue.object->Label.getValue())
                                        : QString(),
                                    QString::fromUtf8(issue.message.c_str()));
            lines << line;
            Base::Console().Message("PMI check: %s\n", line.toUtf8().constData());
        }
    }
    QString summary = lines.isEmpty()
        ? QObject::tr("No problems found.")
        : QObject::tr("%1 error(s), %2 warning(s):\n\n%3")
              .arg(errors)
              .arg(warnings)
              .arg(lines.mid(0, 30).join(QLatin1Char('\n')));
    QMessageBox::information(Gui::getMainWindow(), QObject::tr("PMI check"), summary);
}

bool CmdPMICheck::isActive()
{
    return haveDocument(this);
}

void CreatePMICommands()
{
    Gui::CommandManager& mgr = Gui::Application::Instance->commandManager();
    mgr.addCommand(new CmdPMIDatum());
    mgr.addCommand(new CmdPMIGeometricTolerance());
    mgr.addCommand(new CmdPMIDimension());
    mgr.addCommand(new CmdPMINote());
    mgr.addCommand(new CmdPMICheck());
}
