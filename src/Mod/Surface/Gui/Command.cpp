/***************************************************************************
 *   Copyright (c) 2014-2015 Nathan Miller <Nathan.A.Mill[at]gmail.com>    *
 *   Copyright (c) 2014-2015 Balázs Bámer                                  *
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
#include <QApplication>
#include <QMessageBox>
#include <algorithm>
#include <sstream>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#endif

#include "Mod/Part/App/PartFeature.h"
#include "Import.h"
#include <App/Document.h>
#include <App/PropertyLinks.h>
#include <Base/Console.h>
#include <Gui/ActiveObjectList.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/Document.h>
#include <Gui/MDIView.h>
#include <Gui/MainWindow.h>
#include <Gui/SelectionFilter.h>
#include <Gui/SelectionObject.h>
#include <Mod/Surface/App/Geom/Continuity.h>


//===========================================================================
// CmdSurfaceCut THIS IS THE SURFACE CUT COMMAND
//===========================================================================
DEF_STD_CMD(CmdSurfaceCut)

CmdSurfaceCut::CmdSurfaceCut()
    : Command("Surface_Cut")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Surface Cut function");
    sToolTipText = QT_TR_NOOP("Cuts a shape with another Shape.\n"
                              "It returns a modified version of the first shape");
    sWhatsThis = "Surface_Cut";
    sStatusTip = sToolTipText;
    sPixmap = "Surface_Cut";
    // sAccel        = "CTRL+H";
}

void CmdSurfaceCut::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    /*    std::vector<Gui::SelectionObject> Sel = getSelection().getSelectionEx(0,
       Part::Feature::getClassTypeId()); if (Sel.size() != 2) {
            QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Invalid selection"),
                QObject::tr("Select two shapes please."));
            return;
        }

        bool askUser = false;
        for (std::vector<Gui::SelectionObject>::iterator it = Sel.begin(); it != Sel.end(); ++it) {
            App::DocumentObject* obj = it->getObject();
            if (obj->isDerivedFrom<Part::Feature>()) {
                const TopoDS_Shape& shape = static_cast<Part::Feature*>(obj)->Shape.getValue();
                if (!PartGui::checkForSolids(shape) && !askUser) {
                    int ret = QMessageBox::warning(Gui::getMainWindow(), QObject::tr("Non-solids
       selected"), QObject::tr("The use of non-solids for boolean operations may lead to unexpected
       results.\n" "Do you want to continue?"), QMessageBox::Yes, QMessageBox::No); if (ret ==
       QMessageBox::No) return; askUser = true;
                }
            }
        }

        std::string FeatName = getUniqueObjectName("Cut");
        std::string BaseName  = Sel[0].getFeatName();
        std::string ToolName  = Sel[1].getFeatName();

        openCommand(QT_TRANSLATE_NOOP("Command", "Part Cut"));
        doCommand(Doc,"App.activeDocument().addObject(\"Part::Cut\",\"%s\")",FeatName.c_str());
        doCommand(Doc,"App.activeDocument().%s.Base =
       App.activeDocument().%s",FeatName.c_str(),BaseName.c_str());
        doCommand(Doc,"App.activeDocument().%s.Tool =
       App.activeDocument().%s",FeatName.c_str(),ToolName.c_str());
        doCommand(Gui,"Gui.activeDocument().hide('%s')",BaseName.c_str());
        doCommand(Gui,"Gui.activeDocument().hide('%s')",ToolName.c_str());
        copyVisual(FeatName.c_str(), "ShapeColor", BaseName.c_str());
        copyVisual(FeatName.c_str(), "DisplayMode", BaseName.c_str());
        updateActive();
        commitCommand();*/
}



namespace
{

/** A new feature placed: into the active PartDesign Body of the view the
 * command came through, as a sketch goes -- a body takes Surface features
 * (Part::BodyBase::allowType), not solid, never its Tip; through the body's
 * own Python, as Surface does not link PartDesign -- and into the body's
 * surface group if one is active (PDSURFACEKEY). Then `_edit`, the edit
 * context the picks are imported for (linkOf), set where it now is; the
 * command drops it with dropEditContext().
 */
void placeFeature(Gui::Document* gdoc, const std::string& feat)
{
    App::Document* doc = gdoc ? gdoc->getDocument() : nullptr;
    if (!doc) {
        return;
    }
    Gui::MDIView* view = gdoc->getActiveView();
    auto body = view ? view->getActiveObject<App::DocumentObject*>("pdbody") : nullptr;
    if (body && body->getDocument() == doc) {
        Gui::Command::doCommand(Gui::Command::Doc,
                                "App.getDocument('%s').getObject('%s').addObject("
                                "App.getDocument('%s').getObject('%s'))",
                                body->getDocument()->getName(),
                                body->getNameInDocument(),
                                doc->getName(),
                                feat.c_str());
        auto group = view->getActiveObject<App::DocumentObject*>(PDSURFACEKEY);
        auto groupBody = group ? dynamic_cast<App::PropertyLink*>(group->getPropertyByName("_Body"))
                               : nullptr;
        if (groupBody && groupBody->getValue() == body) {
            Gui::Command::doCommand(Gui::Command::Doc,
                                    "_group = App.getDocument('%s').getObject('%s')\n"
                                    "_group.Group = _group.Group + "
                                    "[App.getDocument('%s').getObject('%s')]\n"
                                    "del _group",
                                    group->getDocument()->getName(),
                                    group->getNameInDocument(),
                                    doc->getName(),
                                    feat.c_str());
        }
    }
    if (auto obj = doc->getObject(feat.c_str())) {
        Gui::Command::doCommand(Gui::Command::Doc,
                                "%s",
                                SurfaceGui::editContextPython(obj).c_str());
    }
}

void dropEditContext()
{
    Gui::Command::doCommand(Gui::Command::Doc, "_edit = None\ndel _edit");
}

/** A pick of the selection: as it was picked, from the top -- what import
 * needs to tell what is in the feature's scope -- and the Part::Feature and
 * element it resolves to
 */
struct Picked
{
    App::SubObjectT path;
    App::DocumentObject* obj;
    std::string element;
};

/// The selection by the object each pick is in, in the order picked
std::vector<std::vector<Picked>> selectedGroups()
{
    std::vector<std::vector<Picked>> groups;
    for (const auto& path : Gui::Selection().getSelectionT(nullptr, Gui::ResolveMode::NoResolve)) {
        App::DocumentObject* obj = path.getSubObject();
        if (!obj || !obj->isDerivedFrom<Part::Feature>()) {
            continue;
        }
        Picked pick {path, obj, path.getOldElementName()};
        auto it = std::find_if(groups.begin(), groups.end(), [obj](const auto& group) {
            return group.front().obj == obj;
        });
        if (it == groups.end()) {
            groups.push_back({pick});
        }
        else {
            it->push_back(pick);
        }
    }
    return groups;
}

/// A pick as a link of the feature placed (placeFeature), as Python: taken as
/// a sketch takes external geometry -- in scope directly, else through a
/// binder in the feature's body (Import.h)
std::string linkOf(const Picked& pick)
{
    return SurfaceGui::importPython(pick.path);
}

/// The same for the element `element` of the object `pick` is in
std::string linkOf(const Picked& pick, const std::string& element)
{
    App::SubObjectT path(pick.path.getObject(),
                         (pick.path.getSubNameNoElement() + element).c_str());
    return SurfaceGui::importPython(path);
}

/// A new surface of `type` in the document of the view the command came
/// through, its panel opened: named, not "Active", since a served document
/// is never Python's ActiveDocument
void makeAndEdit(Gui::Command* cmd, const char* type)
{
    Gui::Document* gdoc = cmd->getActiveGuiDocument();
    if (!gdoc) {
        return;
    }
    const char* doc = gdoc->getDocument()->getName();
    std::string feat = gdoc->getDocument()->getUniqueObjectName("Surface");
    Gui::Command::openCommand(QT_TRANSLATE_NOOP("Command", "Create surface"));
    Gui::Command::doCommand(Gui::Command::Doc,
                            "App.getDocument('%s').addObject('%s', '%s')",
                            doc,
                            type,
                            feat.c_str());
    placeFeature(gdoc, feat);
    dropEditContext();
    Gui::Command::doCommand(Gui::Command::Gui,
                            "Gui.getDocument('%s').setEdit('%s', 0)",
                            doc,
                            feat.c_str());
}

}  // namespace

DEF_STD_CMD_A(CmdSurfaceFilling)

CmdSurfaceFilling::CmdSurfaceFilling()
    : Command("Surface_Filling")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Filling...");
    sToolTipText = QT_TR_NOOP("Creates a surface from a series of picked boundary edges.\n"
                              "Additionally, the surface may be constrained by non-boundary edges\n"
                              "and non-boundary vertices.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_Filling";
    sPixmap = "Surface_Filling";
}

void CmdSurfaceFilling::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    makeAndEdit(this, "Surface::Filling");
}

bool CmdSurfaceFilling::isActive()
{
    return hasActiveDocument();
}

//===========================================================================
// Bezier and BSpline surfaces
//===========================================================================
DEF_STD_CMD_A(CmdSurfaceGeomFillSurface)

CmdSurfaceGeomFillSurface::CmdSurfaceGeomFillSurface()
    : Command("Surface_GeomFillSurface")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Fill boundary curves");
    sToolTipText = QT_TR_NOOP("Creates a surface from two, three or four boundary edges.");
    sWhatsThis = "Surface_GeomFillSurface";
    sStatusTip = sToolTipText;
    sPixmap = "Surface_GeomFillSurface";
}

bool CmdSurfaceGeomFillSurface::isActive()
{
    return hasActiveDocument();
}

void CmdSurfaceGeomFillSurface::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    makeAndEdit(this, "Surface::GeomFillSurface");
}


DEF_STD_CMD_A(CmdSurfaceCurveOnMesh)

CmdSurfaceCurveOnMesh::CmdSurfaceCurveOnMesh()
    : Command("Surface_CurveOnMesh")
{
    sAppModule = "MeshPart";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Curve on mesh...");
    sToolTipText = QT_TR_NOOP("Creates an approximated curve on top of a mesh.\n"
                              "This command only works with a 'mesh' object.");
    sWhatsThis = "Surface_CurveOnMesh";
    sStatusTip = sToolTipText;
    sPixmap = "Surface_CurveOnMesh";
}


void CmdSurfaceCurveOnMesh::activated(int)
{
    doCommand(Doc,
              "import MeshPartGui, FreeCADGui\n"
              "FreeCADGui.runCommand('MeshPart_CurveOnMesh')\n");
}

bool CmdSurfaceCurveOnMesh::isActive()
{
    if (Gui::Control().activeDialog()) {
        return false;
    }

    // Check for the selected mesh feature (all Mesh types)
    Base::Type meshType = Base::Type::fromName("Mesh::Feature");
    App::Document* doc = App::GetApplication().getActiveDocument();
    if (doc && doc->countObjectsOfType(meshType) > 0) {
        return true;
    }

    return false;
}

//===========================================================================
// CmdBlendCurve : Blend Curve Command
//===========================================================================
DEF_STD_CMD_A(CmdBlendCurve)

CmdBlendCurve::CmdBlendCurve()
    : Command("Surface_BlendCurve")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Blend Curve");
    sToolTipText = QT_TR_NOOP("Join two edges with high continuity");
    sStatusTip = sToolTipText;
    sWhatsThis = "BlendCurve";
    sPixmap = "Surface_BlendCurve";
}

void CmdBlendCurve::activated(int)
{
    std::string featName = getUniqueObjectName("BlendCurve");
    // the two edges, in the order picked
    std::vector<Picked> edges;
    for (const auto& group : selectedGroups()) {
        for (const auto& pick : group) {
            if (pick.element.rfind("Edge", 0) == 0) {
                edges.push_back(pick);
            }
        }
    }
    if (edges.size() < 2) {
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Blend Curve"));
    doCommand(Doc,
              "App.ActiveDocument.addObject(\"Surface::FeatureBlendCurve\",\"%s\")",
              featName.c_str());
    placeFeature(getActiveGuiDocument(), featName);
    doCommand(Doc,
              "App.ActiveDocument.%s.StartEdge = %s",
              featName.c_str(),
              linkOf(edges[0]).c_str());
    doCommand(Doc,
              "App.ActiveDocument.%s.EndEdge = %s",
              featName.c_str(),
              linkOf(edges[1]).c_str());
    dropEditContext();
    updateActive();
    commitCommand();
}

bool CmdBlendCurve::isActive()
{
    Gui::SelectionFilter edgeFilter("SELECT Part::Feature SUBELEMENT Edge COUNT 2");
    return edgeFilter.match();
}

DEF_STD_CMD_A(CmdSurfaceExtendFace)

CmdSurfaceExtendFace::CmdSurfaceExtendFace()
    : Command("Surface_ExtendFace")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Extend face");
    sToolTipText = QT_TR_NOOP("Extrapolates the selected face or surface at its boundaries\n"
                              "with its local U and V parameters.");
    sWhatsThis = "Surface_ExtendFace";
    sStatusTip = sToolTipText;
    sPixmap = "Surface_ExtendFace";
}

void CmdSurfaceExtendFace::activated(int)
{
    Gui::SelectionFilter faceFilter("SELECT Part::Feature SUBELEMENT Face COUNT 1");
    if (faceFilter.match()) {
        const std::vector<std::string>& sub = faceFilter.Result[0][0].getSubNames();
        if (sub.size() == 1) {
            openCommand(QT_TRANSLATE_NOOP("Command", "Extend surface"));
            std::string FeatName = getUniqueObjectName("Surface");
            doCommand(Doc,
                      "App.ActiveDocument.addObject(\"Surface::Extend\",\"%s\")",
                      FeatName.c_str());
            placeFeature(getActiveGuiDocument(), FeatName);
            for (const auto& group : selectedGroups()) {
                for (const auto& pick : group) {
                    if (pick.element.rfind("Face", 0) == 0) {
                        doCommand(Doc,
                                  "App.ActiveDocument.%s.Face = %s",
                                  FeatName.c_str(),
                                  linkOf(pick).c_str());
                        break;
                    }
                }
            }
            dropEditContext();
            updateActive();
            commitCommand();
        }
    }
    else {
        QMessageBox::warning(Gui::getMainWindow(),
                             qApp->translate("Surface_ExtendFace", "Wrong selection"),
                             qApp->translate("Surface_ExtendFace", "Select a single face"));
    }
}

bool CmdSurfaceExtendFace::isActive()
{
    return Gui::Selection().countObjectsOfType(Part::Feature::getClassTypeId()) == 1;
}

DEF_STD_CMD_A(CmdSurfaceSections)

CmdSurfaceSections::CmdSurfaceSections()
    : Command("Surface_Sections")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Sections...");
    sToolTipText = QT_TR_NOOP("Creates a surface from a series of sectional edges.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_Sections";
    sPixmap = "Surface_Sections";
}

void CmdSurfaceSections::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    makeAndEdit(this, "Surface::Sections");
}

bool CmdSurfaceSections::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceNetwork)

CmdSurfaceNetwork::CmdSurfaceNetwork()
    : Command("Surface_Network")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Network...");
    sToolTipText = QT_TR_NOOP("Creates a surface through a network of curves, profiles one way\n"
                              "and guides the other, with an edge condition on each outer curve.\n"
                              "Selected edges, or a whole 3D sketch, are sorted into the two\n"
                              "families.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_Network";
    sPixmap = "Surface_Network";
}

void CmdSurfaceNetwork::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    // what is selected is the whole network: edges, or objects given whole
    // (a 3D sketch), sorted into the families as the surface is built
    std::ostringstream links;
    for (const auto& group : selectedGroups()) {
        for (const auto& pick : group) {
            if (pick.element.empty() || pick.element.rfind("Edge", 0) == 0) {
                links << linkOf(pick) << ", ";
            }
        }
    }
    // named, not "Active": in a served process the document is the one of
    // the view the command came through, which Python's ActiveDocument is not
    Gui::Document* gdoc = getActiveGuiDocument();
    if (!gdoc) {
        return;
    }
    const char* doc = gdoc->getDocument()->getName();
    std::string FeatName = gdoc->getDocument()->getUniqueObjectName("Network");

    openCommand(QT_TRANSLATE_NOOP("Command", "Create network surface"));
    doCommand(Doc,
              "App.getDocument('%s').addObject('Surface::Network', '%s')",
              doc,
              FeatName.c_str());
    placeFeature(gdoc, FeatName);
    if (!links.str().empty()) {
        doCommand(Doc,
                  "App.getDocument('%s').getObject('%s').Profiles = [%s]",
                  doc,
                  FeatName.c_str(),
                  links.str().c_str());
        doCommand(Doc, "App.getDocument('%s').recompute()", doc);
    }
    dropEditContext();
    doCommand(Gui, "Gui.getDocument('%s').setEdit('%s', 0)", doc, FeatName.c_str());
}

bool CmdSurfaceNetwork::isActive()
{
    return hasActiveDocument();
}

namespace
{

/// A selected object's links as Python (linkOf): its subs, or the object whole
std::string linksOf(const std::vector<Picked>& group, bool (*keep)(const std::string&))
{
    std::ostringstream links;
    for (const auto& pick : group) {
        if (pick.element.empty() || keep(pick.element)) {
            links << linkOf(pick) << ", ";
        }
    }
    return links.str();
}

bool isFace(const std::string& sub)
{
    return sub.rfind("Face", 0) == 0;
}

bool isEdgeOrFace(const std::string& sub)
{
    return isFace(sub) || sub.rfind("Edge", 0) == 0;
}

/// Said where a served process's user sees it too, never in a dialog
void tell(const char* text)
{
    Base::Console().Warning("%s\n", text);
}

/// Split and Trim from the selection: the first object picked is cut, by
/// the rest; nothing picked, an empty one to fill in its panel
void makeCut(Gui::Command* cmd, const char* type, const char* name, bool edit)
{
    Gui::Document* gdoc = cmd->getActiveGuiDocument();
    if (!gdoc) {
        return;
    }
    const char* doc = gdoc->getDocument()->getName();
    auto selection = selectedGroups();
    std::string base, tools;
    for (std::size_t i = 0; i < selection.size(); ++i) {
        (i == 0 ? base : tools) += linksOf(selection[i], i == 0 ? isFace : isEdgeOrFace);
    }
    if (!edit && (base.empty() || tools.empty())) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select the faces to cut first, then what cuts them"));
        return;
    }
    std::string feat = gdoc->getDocument()->getUniqueObjectName(name);
    Gui::Command::openCommand(name);
    Gui::Command::doCommand(Gui::Command::Doc,
                            "App.getDocument('%s').addObject('%s', '%s')",
                            doc,
                            type,
                            feat.c_str());
    placeFeature(gdoc, feat);
    if (!base.empty()) {
        Gui::Command::doCommand(Gui::Command::Doc,
                                "App.getDocument('%s').getObject('%s').Base = [%s]",
                                doc,
                                feat.c_str(),
                                base.c_str());
    }
    if (!tools.empty()) {
        Gui::Command::doCommand(Gui::Command::Doc,
                                "App.getDocument('%s').getObject('%s').Tools = [%s]",
                                doc,
                                feat.c_str(),
                                tools.c_str());
    }
    dropEditContext();
    if (edit) {
        Gui::Command::doCommand(Gui::Command::Gui,
                                "Gui.getDocument('%s').setEdit('%s', 0)",
                                doc,
                                feat.c_str());
    }
    else {
        Gui::Command::doCommand(Gui::Command::Doc, "App.getDocument('%s').recompute()", doc);
        Gui::Command::commitCommand();
    }
}

}  // namespace

DEF_STD_CMD_A(CmdSurfaceSplit)

CmdSurfaceSplit::CmdSurfaceSplit()
    : Command("Surface_Split")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Split");
    sToolTipText = QT_TR_NOOP("Cuts the faces selected first by the curves or surfaces selected\n"
                              "after them, and keeps every piece. Curves are laid onto the faces.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_Split";
    sPixmap = "Surface_Split";
}

void CmdSurfaceSplit::activated(int)
{
    makeCut(this, "Surface::Split", "Split", false);
}

bool CmdSurfaceSplit::isActive()
{
    return Gui::Selection().countObjectsOfType(Part::Feature::getClassTypeId()) > 1;
}

DEF_STD_CMD_A(CmdSurfaceTrim)

CmdSurfaceTrim::CmdSurfaceTrim()
    : Command("Surface_Trim")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Trim...");
    sToolTipText = QT_TR_NOOP("Cuts the faces selected first by the curves or surfaces selected\n"
                              "after them, and keeps the sides you pick. Curves are laid onto\n"
                              "the faces; the side kept is found again after a change.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_Trim";
    sPixmap = "Surface_Trim";
}

void CmdSurfaceTrim::activated(int)
{
    makeCut(this, "Surface::Trim", "Trim", true);
}

bool CmdSurfaceTrim::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceUntrim)

CmdSurfaceUntrim::CmdSurfaceUntrim()
    : Command("Surface_Untrim")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Untrim");
    sToolTipText = QT_TR_NOOP("Gives the selected face its surface's own boundary back.\n"
                              "Set Before to a trim to have the face as it was before it.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_Untrim";
    sPixmap = "Surface_Untrim";
}

void CmdSurfaceUntrim::activated(int)
{
    Gui::Document* gdoc = getActiveGuiDocument();
    if (!gdoc) {
        return;
    }
    const char* doc = gdoc->getDocument()->getName();
    std::vector<Picked> faces;
    for (const auto& group : selectedGroups()) {
        for (const auto& pick : group) {
            if (isFace(pick.element)) {
                faces.push_back(pick);
            }
        }
    }
    if (faces.empty()) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select the faces to untrim"));
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Untrim"));
    for (const auto& pick : faces) {
        std::string feat = gdoc->getDocument()->getUniqueObjectName("Untrim");
        doCommand(Doc,
                  "App.getDocument('%s').addObject('Surface::Untrim', '%s')",
                  doc,
                  feat.c_str());
        placeFeature(gdoc, feat);
        doCommand(Doc,
                  "App.getDocument('%s').getObject('%s').Face = %s",
                  doc,
                  feat.c_str(),
                  linkOf(pick).c_str());
        dropEditContext();
    }
    doCommand(Doc, "App.getDocument('%s').recompute()", doc);
    commitCommand();
}

bool CmdSurfaceUntrim::isActive()
{
    return Gui::Selection().countObjectsOfType(Part::Feature::getClassTypeId()) > 0;
}

DEF_STD_CMD_A(CmdSurfaceExtendEdges)

CmdSurfaceExtendEdges::CmdSurfaceExtendEdges()
    : Command("Surface_ExtendEdges")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Extend edges");
    sToolTipText = QT_TR_NOOP("Carries a face on across the selected edges of it: naturally, the\n"
                              "same surface further, or straight on. Each edge has to be on the\n"
                              "boundary of the face's surface.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_ExtendEdges";
    sPixmap = "Surface_ExtendEdges";
}

void CmdSurfaceExtendEdges::activated(int)
{
    Gui::Document* gdoc = getActiveGuiDocument();
    if (!gdoc) {
        return;
    }
    const char* doc = gdoc->getDocument()->getName();
    auto selection = selectedGroups();
    if (selection.size() != 1) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select edges of one face to extend across"));
        return;
    }
    const auto& group = selection.front();
    auto shape = Part::Feature::getTopoShape(group.front().obj);
    // the one face every edge picked is an edge of
    int face = 0;
    std::ostringstream edges;
    for (const auto& pick : group) {
        const std::string& sub = pick.element;
        if (sub.rfind("Edge", 0) != 0) {
            continue;
        }
        auto faces = shape.findAncestors(shape.getSubShape(sub.c_str()), TopAbs_FACE);
        if (faces.size() != 1 || (face && faces.front() != face)) {
            tell(QT_TRANSLATE_NOOP("Surface",
                                   "The edges picked are not of one face: extend the face's "
                                   "own edges"));
            return;
        }
        face = faces.front();
        edges << linkOf(pick) << ", ";
    }
    if (!face) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select edges of one face to extend across"));
        return;
    }
    std::string feat = gdoc->getDocument()->getUniqueObjectName("Extend");
    openCommand(QT_TRANSLATE_NOOP("Command", "Extend edges"));
    doCommand(Doc,
              "App.getDocument('%s').addObject('Surface::Extend', '%s')",
              doc,
              feat.c_str());
    placeFeature(gdoc, feat);
    doCommand(Doc,
              "_ext = App.getDocument('%s').getObject('%s')\n"
              "_ext.Face = %s\n"
              "_ext.Mode = 'Edges'\n"
              "_ext.Edges = [%s]\n"
              "del _ext",
              doc,
              feat.c_str(),
              linkOf(group.front(), "Face" + std::to_string(face)).c_str(),
              edges.str().c_str());
    dropEditContext();
    doCommand(Doc, "App.getDocument('%s').recompute()", doc);
    commitCommand();
}

bool CmdSurfaceExtendEdges::isActive()
{
    return Gui::Selection().countObjectsOfType(Part::Feature::getClassTypeId()) > 0;
}

namespace
{

/// The edges selected, in the order picked, as links (linkOf)
std::vector<std::string> selectedEdges()
{
    std::vector<std::string> edges;
    for (const auto& group : selectedGroups()) {
        for (const auto& pick : group) {
            if (pick.element.rfind("Edge", 0) == 0) {
                edges.push_back(linkOf(pick));
            }
        }
    }
    return edges;
}

/// A feature of `type` made with `assignments` ("Prop = value" lines), recomputed
void makeFeature(Gui::Command* cmd,
                 const char* type,
                 const char* name,
                 const std::vector<std::string>& assignments)
{
    Gui::Document* gdoc = cmd->getActiveGuiDocument();
    if (!gdoc) {
        return;
    }
    const char* doc = gdoc->getDocument()->getName();
    std::string feat = gdoc->getDocument()->getUniqueObjectName(name);
    Gui::Command::openCommand(name);
    Gui::Command::doCommand(Gui::Command::Doc,
                            "_f = App.getDocument('%s').addObject('%s', '%s')",
                            doc,
                            type,
                            feat.c_str());
    // placed first: the links are made for it where it is (linkOf)
    placeFeature(gdoc, feat);
    for (const auto& a : assignments) {
        Gui::Command::doCommand(Gui::Command::Doc, "_f.%s", a.c_str());
    }
    Gui::Command::doCommand(Gui::Command::Doc, "del _f");
    dropEditContext();
    Gui::Command::doCommand(Gui::Command::Doc, "App.getDocument('%s').recompute()", doc);
    Gui::Command::commitCommand();
}

}  // namespace

DEF_STD_CMD_A(CmdSurfaceBlendSurface)

CmdSurfaceBlendSurface::CmdSurfaceBlendSurface()
    : Command("Surface_BlendSurface")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Blend surface");
    sToolTipText = QT_TR_NOOP("Bridges the two edges selected, each on a face, with a surface\n"
                              "tangent to both (G0 to G2 and a weight on each side).");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_BlendSurface";
    sPixmap = "Surface_BlendSurface";
}

void CmdSurfaceBlendSurface::activated(int)
{
    auto edges = selectedEdges();
    if (edges.size() != 2) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select the two edges to bridge"));
        return;
    }
    makeFeature(this,
                "Surface::BlendSurface",
                "BlendSurface",
                {"Edge1 = " + edges[0], "Edge2 = " + edges[1]});
}

bool CmdSurfaceBlendSurface::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceTwoRailSweep)

CmdSurfaceTwoRailSweep::CmdSurfaceTwoRailSweep()
    : Command("Surface_TwoRailSweep")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Two-rail sweep");
    sToolTipText = QT_TR_NOOP("Sweeps sections between two rails that scale them: select the\n"
                              "two rails first, then the sections, each from one rail to the\n"
                              "other.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_TwoRailSweep";
    sPixmap = "Surface_TwoRailSweep";
}

void CmdSurfaceTwoRailSweep::activated(int)
{
    auto edges = selectedEdges();
    if (edges.size() < 3) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select two rails, then the sections between them"));
        return;
    }
    std::string sections;
    for (std::size_t i = 2; i < edges.size(); ++i) {
        sections += edges[i] + ", ";
    }
    makeFeature(this,
                "Surface::TwoRailSweep",
                "TwoRailSweep",
                {"Rail1 = " + edges[0], "Rail2 = " + edges[1], "Sections = [" + sections + "]"});
}

bool CmdSurfaceTwoRailSweep::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfacePipeShell)

CmdSurfacePipeShell::CmdSurfacePipeShell()
    : Command("Surface_PipeShell")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Pipeshell");
    sToolTipText = QT_TR_NOOP("Sweeps profiles along a spine with every mode of the pipe shell:\n"
                              "select the spine first, then the profiles.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_PipeShell";
    sPixmap = "Surface_PipeShell";
}

void CmdSurfacePipeShell::activated(int)
{
    // the first object picked is the spine, the rest the profiles; a whole
    // object, or its edges
    auto selection = selectedGroups();
    if (selection.size() < 2) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select the spine first, then the profiles"));
        return;
    }
    auto any = [](const std::string& sub) {
        return sub.rfind("Edge", 0) == 0 || sub.rfind("Vertex", 0) == 0;
    };
    std::string spine = linksOf(selection[0], any), profiles;
    for (std::size_t i = 1; i < selection.size(); ++i) {
        profiles += linksOf(selection[i], any);
    }
    makeFeature(this,
                "Surface::PipeShell",
                "PipeShell",
                {"Spine = [" + spine + "]", "Profiles = [" + profiles + "]"});
}

bool CmdSurfacePipeShell::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceEdgeContinuity)

CmdSurfaceEdgeContinuity::CmdSurfaceEdgeContinuity()
    : Command("Surface_EdgeContinuity")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Edge continuity");
    sToolTipText = QT_TR_NOOP("Measures how two faces meet along an edge: the gap, the angle\n"
                              "and the jump of curvature across it, and where each is worst.\n"
                              "Select an edge between two faces, an edge and a face, or two\n"
                              "faces.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_EdgeContinuity";
    sPixmap = "Surface_EdgeContinuity";
}

namespace
{

/// The faces of `owner` that `edge` is an edge of
std::vector<TopoDS_Shape> facesOf(const Part::TopoShape& owner, const TopoDS_Shape& edge)
{
    return owner.findAncestorsShapes(edge, TopAbs_FACE);
}

/// An edge of `a` lying on `b`: its middle and ends on it
TopoDS_Shape sharedEdge(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    TopoDS_Shape best;
    double length = 0;
    for (TopExp_Explorer x(a, TopAbs_EDGE); x.More(); x.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(x.Current()));
        bool on = true;
        for (double f : {0.0, 0.5, 1.0}) {
            gp_Pnt p = c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * f);
            BRepExtrema_DistShapeShape d(BRepBuilderAPI_MakeVertex(p).Vertex(), b);
            on = on && d.IsDone() && d.Value() < 1e-4;
        }
        double l = GCPnts_AbscissaPoint::Length(c);
        if (on && l > length) {
            best = x.Current();
            length = l;
        }
    }
    return best;
}

}  // namespace

void CmdSurfaceEdgeContinuity::activated(int)
{
    // what is picked: edges and faces, each with its owner's shape
    struct Picked
    {
        Part::TopoShape owner;
        TopoDS_Shape shape;
    };
    std::vector<Picked> edges, faces;
    for (const auto& sel :
         Gui::Selection().getSelectionEx(nullptr, Part::Feature::getClassTypeId())) {
        auto owner = Part::Feature::getTopoShape(sel.getObject());
        for (const auto& sub : sel.getSubNames()) {
            TopoDS_Shape s = owner.getSubShape(sub.c_str(), true);
            if (s.IsNull()) {
                continue;
            }
            (s.ShapeType() == TopAbs_EDGE ? edges : faces).push_back({owner, s});
        }
    }
    TopoDS_Shape edge, face1, face2;
    if (edges.size() == 1 && faces.empty()) {
        auto f = facesOf(edges[0].owner, edges[0].shape);
        if (f.size() == 2) {
            edge = edges[0].shape, face1 = f[0], face2 = f[1];
        }
    }
    else if (edges.size() == 1 && faces.size() == 1) {
        auto f = facesOf(edges[0].owner, edges[0].shape);
        if (f.size() == 1) {
            edge = edges[0].shape, face1 = f[0], face2 = faces[0].shape;
        }
    }
    else if (edges.empty() && faces.size() == 2) {
        edge = sharedEdge(faces[0].shape, faces[1].shape);
        face1 = faces[0].shape, face2 = faces[1].shape;
    }
    if (edge.IsNull() || face1.IsNull() || face2.IsNull()) {
        tell(QT_TRANSLATE_NOOP("Surface",
                               "Select an edge between two faces, an edge and a face, or two "
                               "faces that share an edge"));
        return;
    }
    auto r = Surface::Geom::edgeContinuity(TopoDS::Edge(edge),
                                           TopoDS::Face(face1),
                                           TopoDS::Face(face2));
    if (!r.error.empty()) {
        Base::Console().Warning("Edge continuity: %s\n", r.error.c_str());
        return;
    }
    const char* names[] = {"not touching", "G0", "G1", "G2"};
    auto at = [](const gp_Pnt& p) {
        return "(" + std::to_string(p.X()) + ", " + std::to_string(p.Y()) + ", "
            + std::to_string(p.Z()) + ")";
    };
    Base::Console().Message(
        "Edge continuity: %s. Gap %g at %s; angle %g deg at %s; curvature gap %g at %s\n",
        names[r.order() + 1],
        r.maxGap,
        at(r.worstGap).c_str(),
        r.maxAngle * 180 / M_PI,
        at(r.worstAngle).c_str(),
        r.maxCurvatureGap,
        at(r.worstCurvature).c_str());
}

bool CmdSurfaceEdgeContinuity::isActive()
{
    return Gui::Selection().countObjectsOfType(Part::Feature::getClassTypeId()) > 0;
}

namespace
{

/// What is selected, by kind: edges (and objects given whole with no
/// faces), and faces (and objects given whole with faces)
struct Picks
{
    std::vector<std::string> curves, faces, all;
    /// How many of the faces were picked as faces, not objects whole
    int faceElements = 0;
};

Picks picks()
{
    Picks p;
    for (const auto& group : selectedGroups()) {
        for (const auto& pick : group) {
            std::string link = linkOf(pick);
            const std::string& sub = pick.element;
            if (sub.empty()) {
                auto shape = Part::Feature::getTopoShape(pick.obj);
                (shape.countSubShapes(TopAbs_FACE) > 0 ? p.faces : p.curves).push_back(link);
            }
            else if (sub.rfind("Face", 0) == 0) {
                p.faces.push_back(link);
                ++p.faceElements;
            }
            else if (sub.rfind("Edge", 0) == 0) {
                p.curves.push_back(link);
            }
            p.all.push_back(link);
        }
    }
    return p;
}

std::string listOf(const std::vector<std::string>& links)
{
    std::string out = "[";
    for (const auto& l : links) {
        out += l + ", ";
    }
    return out + "]";
}

}  // namespace

DEF_STD_CMD_A(CmdSurfaceIsocurve)

CmdSurfaceIsocurve::CmdSurfaceIsocurve()
    : Command("Surface_Isocurve")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Isocurves");
    sToolTipText = QT_TR_NOOP("Draws isoparametric curves of the selected face, clipped to it.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_Isocurve";
    sPixmap = "Surface_Isocurve";
}

void CmdSurfaceIsocurve::activated(int)
{
    auto p = picks();
    if (p.faces.size() != 1 || p.faceElements != 1) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select one face"));
        return;
    }
    makeFeature(this, "Surface::Isocurve", "Isocurve", {"Face = " + p.faces.front()});
}

bool CmdSurfaceIsocurve::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceCurveOnSurface)

CmdSurfaceCurveOnSurface::CmdSurfaceCurveOnSurface()
    : Command("Surface_CurveOnSurface")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Curve on surface");
    sToolTipText = QT_TR_NOOP("Lays the selected curves onto the selected faces, along their\n"
                              "normals, as curves of their surfaces.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_CurveOnSurface";
    sPixmap = "Surface_CurveOnSurface";
}

void CmdSurfaceCurveOnSurface::activated(int)
{
    auto p = picks();
    if (p.curves.empty() || p.faces.empty()) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select curves and the faces to lay them on"));
        return;
    }
    makeFeature(this,
                "Surface::CurveOnSurface",
                "CurveOnSurface",
                {"Curves = " + listOf(p.curves), "Faces = " + listOf(p.faces)});
}

bool CmdSurfaceCurveOnSurface::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceIntersectionCurve)

CmdSurfaceIntersectionCurve::CmdSurfaceIntersectionCurve()
    : Command("Surface_IntersectionCurve")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Intersection curve");
    sToolTipText = QT_TR_NOOP("Makes the curves where the two selected shapes or faces meet.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_IntersectionCurve";
    sPixmap = "Surface_IntersectionCurve";
}

void CmdSurfaceIntersectionCurve::activated(int)
{
    auto p = picks();
    if (p.all.size() != 2) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select the two shapes to intersect"));
        return;
    }
    makeFeature(this,
                "Surface::IntersectionCurve",
                "IntersectionCurve",
                {"Shape1 = [" + p.all[0] + "]", "Shape2 = [" + p.all[1] + "]"});
}

bool CmdSurfaceIntersectionCurve::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceJoinCurves)

CmdSurfaceJoinCurves::CmdSurfaceJoinCurves()
    : Command("Surface_JoinCurves")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Join curves");
    sToolTipText = QT_TR_NOOP("Joins the selected curves into smooth ones, broken at corners.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_JoinCurves";
    sPixmap = "Surface_JoinCurves";
}

void CmdSurfaceJoinCurves::activated(int)
{
    auto p = picks();
    if (p.curves.empty()) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select the curves to join"));
        return;
    }
    makeFeature(this, "Surface::JoinCurves", "JoinCurves", {"Curves = " + listOf(p.curves)});
}

bool CmdSurfaceJoinCurves::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceExtendCurve)

CmdSurfaceExtendCurve::CmdSurfaceExtendCurve()
    : Command("Surface_ExtendCurve")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Extend curve");
    sToolTipText = QT_TR_NOOP("Carries the selected edge on past its end, the same curve further\n"
                              "or straight on.");
    sStatusTip = sToolTipText;
    sWhatsThis = "Surface_ExtendCurve";
    sPixmap = "Surface_ExtendCurve";
}

void CmdSurfaceExtendCurve::activated(int)
{
    auto p = picks();
    if (p.curves.size() != 1 || p.curves.front().find("'Edge") == std::string::npos) {
        tell(QT_TRANSLATE_NOOP("Surface", "Select one edge to extend"));
        return;
    }
    makeFeature(this, "Surface::ExtendCurve", "ExtendCurve", {"Curve = " + p.curves.front()});
}

bool CmdSurfaceExtendCurve::isActive()
{
    return hasActiveDocument();
}

DEF_STD_CMD_A(CmdSurfaceSewing)

CmdSurfaceSewing::CmdSurfaceSewing()
    : Command("Surface_Sewing")
{
    sAppModule = "Surface";
    sGroup = QT_TR_NOOP("Surface");
    sMenuText = QT_TR_NOOP("Sew");
    sToolTipText = QT_TR_NOOP("Sews the selected faces or shapes into a shell.\n"
                              "Its FreeEdges and MaxGap properties tell what is left open\n"
                              "and the widest gap closed.");
    sWhatsThis = "Surface_Sewing";
    sStatusTip = sToolTipText;
    sPixmap = "Surface_Sewing";
}

void CmdSurfaceSewing::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    std::ostringstream links;
    for (const auto& group : selectedGroups()) {
        for (const auto& pick : group) {
            links << linkOf(pick) << ", ";
        }
    }
    if (links.str().empty()) {
        QMessageBox::warning(Gui::getMainWindow(),
                             qApp->translate("Surface_Sewing", "Wrong selection"),
                             qApp->translate("Surface_Sewing", "Select faces or shapes to sew"));
        return;
    }
    std::string FeatName = getUniqueObjectName("Sewing");
    openCommand(QT_TRANSLATE_NOOP("Command", "Sew"));
    doCommand(Doc, "App.ActiveDocument.addObject(\"Surface::Sewing\",\"%s\")", FeatName.c_str());
    placeFeature(getActiveGuiDocument(), FeatName);
    doCommand(Doc,
              "App.ActiveDocument.%s.ShapeList = [%s]",
              FeatName.c_str(),
              links.str().c_str());
    dropEditContext();
    updateActive();
    commitCommand();
}

bool CmdSurfaceSewing::isActive()
{
    return Gui::Selection().countObjectsOfType(Part::Feature::getClassTypeId()) > 0;
}

void CreateSurfaceCommands()
{
    Gui::CommandManager& rcCmdMgr = Gui::Application::Instance->commandManager();
    /*
        rcCmdMgr.addCommand(new CmdSurfaceCut());
    */
    rcCmdMgr.addCommand(new CmdSurfaceFilling());
    rcCmdMgr.addCommand(new CmdSurfaceGeomFillSurface());
    rcCmdMgr.addCommand(new CmdSurfaceSections());
    rcCmdMgr.addCommand(new CmdSurfaceNetwork());
    rcCmdMgr.addCommand(new CmdSurfaceSplit());
    rcCmdMgr.addCommand(new CmdSurfaceTrim());
    rcCmdMgr.addCommand(new CmdSurfaceUntrim());
    rcCmdMgr.addCommand(new CmdSurfaceExtendEdges());
    rcCmdMgr.addCommand(new CmdSurfaceBlendSurface());
    rcCmdMgr.addCommand(new CmdSurfaceTwoRailSweep());
    rcCmdMgr.addCommand(new CmdSurfacePipeShell());
    rcCmdMgr.addCommand(new CmdSurfaceEdgeContinuity());
    rcCmdMgr.addCommand(new CmdSurfaceIsocurve());
    rcCmdMgr.addCommand(new CmdSurfaceCurveOnSurface());
    rcCmdMgr.addCommand(new CmdSurfaceIntersectionCurve());
    rcCmdMgr.addCommand(new CmdSurfaceJoinCurves());
    rcCmdMgr.addCommand(new CmdSurfaceExtendCurve());
    rcCmdMgr.addCommand(new CmdSurfaceSewing());
    rcCmdMgr.addCommand(new CmdSurfaceExtendFace());
    rcCmdMgr.addCommand(new CmdSurfaceCurveOnMesh());
    rcCmdMgr.addCommand(new CmdBlendCurve());
}
