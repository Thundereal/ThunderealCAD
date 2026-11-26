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

#include <algorithm>
#include <cmath>

#include <QMessageBox>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp_Face.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <Inventor/nodes/SoCamera.h>
#include <Inventor/nodes/SoOrthographicCamera.h>
#include <Inventor/nodes/SoPerspectiveCamera.h>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/GeoFeature.h>
#include <App/GeoFeatureGroupExtension.h>
#include <Gui/ActiveObjectList.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/Control.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/MDIView.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionObject.h>
#include <Gui/ToolContext.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/Workbench.h>
#include <Gui/WorkbenchManager.h>
#include <Mod/Part/App/BodyBase.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PMI/App/Annotation.h>
#include <Mod/PMI/App/AnnotationSet.h>
#include <Mod/PMI/App/Api.h>
#include <Mod/PMI/App/Check.h>
#include <Mod/PMI/App/Datum.h>
#include <Mod/PMI/App/Geometry.h>
#include <Mod/PMI/App/View.h>

#include "TaskQuery.h"
#include "ViewProviderCallout.h"
#include "ViewProviderView.h"

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
    /// Where the face was picked, and the solid's placement there: what
    /// takes the point into the solid's own group
    Base::Vector3d point;
    Base::Placement solidGlobal;
};

/// The solid's placement where a pick reached it. A Body's solid body is not
/// on the path a click makes -- that goes through the Body's tip feature,
/// whose own placement (a primitive's, say) is not the solid's -- so the
/// path is followed to the solid's group, then the solid's own is applied
Base::Placement solidPlacement(App::DocumentObject* solid, App::DocumentObject* root,
                               const std::string& sub)
{
    auto path = root->getSubObjectList(sub.c_str());
    if (std::find(path.begin(), path.end(), solid) != path.end()) {
        return App::GeoFeature::getGlobalPlacement(solid, root, sub);
    }
    auto group = App::GeoFeatureGroupExtension::getGroupOfObject(solid);
    if (group && std::find(path.begin(), path.end(), group) != path.end()) {
        Base::Placement own;
        if (auto geo = freecad_cast<App::GeoFeature*>(solid)) {
            own = geo->Placement.getValue();
        }
        return App::GeoFeature::getGlobalPlacement(group, root, sub) * own;
    }
    return App::GeoFeature::getGlobalPlacement(solid);
}

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
        const auto& points = sel.getPickedPoints();
        const auto& subs = sel.getSubNames();
        for (std::size_t i = 0; i < subs.size(); ++i) {
            const std::string& sub = subs[i];
            Pick pick;
            if (i < points.size()) {
                pick.point = points[i];
            }
            if (!PMI::Api::solidReference(sel.getObject(), sub, pick.solid, pick.face, why)) {
                QMessageBox::warning(Gui::getMainWindow(), QObject::tr("PMI"),
                                     QObject::tr("%1: %2").arg(QString::fromUtf8(what),
                                                               QString::fromUtf8(why.c_str())));
                return {};
            }
            auto shape = Part::Feature::getTopoShape(pick.solid, pick.face.c_str(), true);
            pick.surface = PMI::Geom::ofElement(shape.getShape());
            pick.solidGlobal = solidPlacement(pick.solid, sel.getObject(), sub);
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
                                 QObject::tr("%1: pick a face, an edge or a vertex of a part first")
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

/// Made in a workbench that takes the PMI tool bar as a context
/// (PartDesign) with no set active in the view: the set, _set, made active,
/// so the tool bar comes -- the first annotation of a Body made from the
/// Part Design menu's PMI submenu brings the rest (docs/GDandT.md sec 3)
void offerTools(Gui::Command* cmd)
{
    auto spec = Gui::ToolContext::find("PMI");
    auto wb = Gui::WorkbenchManager::instance()->active();
    auto gdoc = cmd->getActiveGuiDocument();
    auto view = gdoc ? qobject_cast<Gui::View3DInventor*>(gdoc->getActiveView()) : nullptr;
    if (!spec || !wb || !view
        || std::find(spec->workbenches.begin(), spec->workbenches.end(), wb->name())
            == spec->workbenches.end()
        || view->getActiveObject<App::DocumentObject*>(PMIGui::PMIKEY)) {
        return;
    }
    cmd->doCommand(Gui::Command::Gui, "Gui.ActiveDocument.ActiveView.setActiveObject('%s', _set)",
                   PMIGui::PMIKEY);
}

void addToSet(Gui::Command* cmd, const char* type, const char* name)
{
    cmd->doCommand(Gui::Command::Doc, "_obj = _set.Document.addObject('PMI::%s', '%s')", type,
                   name);
    cmd->doCommand(Gui::Command::Doc, "_set.addObject(_obj)");
}

void finish(Gui::Command* cmd)
{
    // On a feature that has one already: stacked under it (sec 6.4)
    cmd->doCommand(Gui::Command::Doc, "_obj.AttachedTo = PMI.stackUnder(_obj)");
    // Made in an active PMI view: shown in it, in its plane (sec 6.3)
    auto gdoc = cmd->getActiveGuiDocument();
    if (auto view = gdoc ? PMIGui::ViewProviderView::active(gdoc->getDocument()) : nullptr) {
        cmd->doCommand(Gui::Command::Doc, "_obj.Views = [_set.Document.getObject('%s')]",
                       view->getNameInDocument());
    }
    cmd->doCommand(Gui::Command::Doc, "_set.Document.recompute()");
    cmd->doCommand(Gui::Command::Gui, "Gui.Selection.clearSelection()");
    cmd->doCommand(Gui::Command::Gui, "Gui.Selection.addSelection(_obj)");
    cmd->commitCommand();
    offerTools(cmd);
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

// ---- PMI_DatumTarget ----

namespace
{
/// Where on the picked face a target goes, in the set's coordinates, z out
/// of the part: the point picked if it is on the face, else the face's
/// point nearest its middle
bool targetOn(const Pick& pick, Base::Placement& out)
{
    auto shape = Part::Feature::getTopoShape(pick.solid, pick.face.c_str(), true);
    if (shape.isNull() || shape.getShape().ShapeType() != TopAbs_FACE) {
        return false;
    }
    const TopoDS_Face face = TopoDS::Face(shape.getShape());
    Base::Placement own;
    if (auto geo = freecad_cast<App::GeoFeature*>(pick.solid)) {
        own = geo->Placement.getValue();
    }
    Base::Vector3d local;
    (own * pick.solidGlobal.inverse()).multVec(pick.point, local);
    Bnd_Box box;
    BRepBndLib::Add(face, box);
    double size = std::sqrt(box.SquareExtent());
    auto nearest = [&](const gp_Pnt& p, gp_Pnt& on, gp_Vec& n) {
        BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(p).Vertex(), face);
        if (!dist.IsDone() || dist.NbSolution() == 0) {
            return -1.0;
        }
        on = dist.PointOnShape2(1);
        n = gp_Vec();
        if (dist.SupportTypeShape2(1) == BRepExtrema_IsInFace) {
            double u, v;
            dist.ParOnFaceS2(1, u, v);
            gp_Pnt q;
            BRepGProp_Face(face).Normal(u, v, q, n);
        }
        return dist.Value();
    };
    gp_Pnt on;
    gp_Vec n;
    double d = nearest(gp_Pnt(local.x, local.y, local.z), on, n);
    if (d < 0 || d > 1e-3 * std::max(1.0, size) || n.Magnitude() < 1e-12) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        if (nearest(gp_Pnt((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2), on, n) < 0
            || n.Magnitude() < 1e-12) {
            return false;
        }
    }
    n.Normalize();
    out = Base::Placement(Base::Vector3d(on.X(), on.Y(), on.Z()),
                          Base::Rotation(Base::Vector3d(0, 0, 1),
                                         Base::Vector3d(n.X(), n.Y(), n.Z())));
    return true;
}
}  // namespace

DEF_STD_CMD_A(CmdPMIDatumTarget)

CmdPMIDatumTarget::CmdPMIDatumTarget()
    : Command("PMI_DatumTarget")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("Datum target");
    sToolTipText = QT_TR_NOOP("A datum target where the face is picked: of the datum picked "
                              "with it, else of a new datum with the next free letter");
    sWhatsThis = "PMI_DatumTarget";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_DatumTarget";
}

void CmdPMIDatumTarget::activated(int)
{
    // The datum picked with the face, if any: kept before the face pick
    // is read, as it is not a face. It may be selected itself or through a
    // path (Body, "Datum001."): the entry it was selected by is taken out
    PMI::Datum* datum = nullptr;
    std::vector<std::pair<App::DocumentObject*, std::string>> picked;
    for (auto& sel : Gui::Selection().getSelectionEx(nullptr,
                                                     App::DocumentObject::getClassTypeId(),
                                                     Gui::ResolveMode::NoResolve)) {
        auto obj = sel.getObject();
        if (sel.getSubNames().empty()) {
            if (auto d = freecad_cast<PMI::Datum*>(obj)) {
                datum = d;
                picked.emplace_back(obj, std::string());
            }
            continue;
        }
        for (auto& sub : sel.getSubNames()) {
            if (auto d = freecad_cast<PMI::Datum*>(obj->getSubObject(sub.c_str()))) {
                datum = d;
                picked.emplace_back(obj, sub);
            }
        }
    }
    for (auto& [obj, sub] : picked) {
        Gui::Selection().rmvSelection(obj->getDocument()->getName(), obj->getNameInDocument(),
                                      sub.empty() ? nullptr : sub.c_str());
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "PMI datum target"));
    auto picks = pickedFaces(this, "Datum target");
    if (picks.empty()) {
        abortCommand();
        return;
    }
    Base::Placement at;
    if (!targetOn(picks[0], at)) {
        abortCommand();
        QMessageBox::warning(Gui::getMainWindow(), QObject::tr("PMI"),
                             QObject::tr("Datum target: no point on the picked face"));
        return;
    }
    makeFeature(this, {picks[0]}, nullptr);
    addToSet(this, "DatumTarget", "DatumTarget");
    doCommand(Doc, "_obj.Feature = _feat");
    Base::Vector3d p = at.getPosition();
    double q0, q1, q2, q3;
    at.getRotation().getValue(q0, q1, q2, q3);
    doCommand(Doc, "_obj.Target = App.Placement(App.Vector(%.17g, %.17g, %.17g), "
                   "App.Rotation(%.17g, %.17g, %.17g, %.17g))",
              p.x, p.y, p.z, q0, q1, q2, q3);
    if (datum) {
        doCommand(Doc, "_datum = _set.Document.getObject('%s')", datum->getNameInDocument());
    }
    else {
        doCommand(Doc, "_datum = _set.Document.addObject('PMI::Datum', 'Datum')");
        doCommand(Doc, "_set.addObject(_datum)");
        doCommand(Doc, "_datum.Letter = PMI.nextDatumLetter(_set, %s)",
                  pyObject(picks[0].solid).c_str());
    }
    doCommand(Doc, "_obj.Number = max([t.Number for t in _datum.Targets] + [0]) + 1");
    doCommand(Doc, "_datum.Targets = _datum.Targets + [_obj]");
    finish(this);
}

bool CmdPMIDatumTarget::isActive()
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
    // A width or a slot is positioned; holes, a counterbore's too, by their
    // axes, whichever face was picked first
    std::vector<PMI::Primitive> all;
    for (auto& pick : picks) {
        all.push_back(pick.surface);
    }
    double width = 0;
    std::size_t hole = 0;
    bool slot = picks.size() > 1 && PMI::Geom::width(all, width);
    bool holes = !slot && PMI::Geom::holes(all, hole);
    const char* kind = type == PMI::Primitive::Cylinder || holes ? "Axis" : nullptr;
    makeFeature(this, picks, kind);
    addToSet(this, "GeometricTolerance", "Tolerance");
    if (type == PMI::Primitive::Plane && picks.size() == 1) {
        doCommand(Doc, "_obj.Characteristic = 'Flatness'");
    }
    else if (slot) {
        doCommand(Doc, "_obj.Characteristic = 'Position'");
    }
    else if (type == PMI::Primitive::Cylinder || type == PMI::Primitive::Circle || holes) {
        doCommand(Doc, "_obj.Characteristic = 'Position'");
        doCommand(Doc, "_obj.ZoneShape = 'Diameter'");
    }
    else if (type == PMI::Primitive::Line) {
        // A line element of the part
        doCommand(Doc, "_obj.Characteristic = 'Straightness'");
    }
    else if (type == PMI::Primitive::Point) {
        doCommand(Doc, "_obj.Characteristic = 'Position'");
    }
    else {
        doCommand(Doc, "_obj.Characteristic = 'ProfileOfSurface'");
    }
    doCommand(Doc, "_obj.Feature = _feat");
    finish(this);
    // Then its frame is built in the panel
    doCommand(Gui, "Gui.ActiveDocument.setEdit(_obj)");
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
    if (picks.size() == 1 && a.type == PMI::Primitive::Line) {
        QMessageBox::information(Gui::getMainWindow(), QObject::tr("PMI"),
                                 QObject::tr("Dimension: an edge's length is not a dimension "
                                             "yet; pick two elements for the distance between "
                                             "them"));
        abortCommand();
        return;
    }
    if (picks.size() == 1) {
        // The size its surface has: none for a plane or a point alone
        const char* type = nullptr;
        switch (a.type) {
            case PMI::Primitive::Cylinder:
            case PMI::Primitive::Circle:
                type = "Diameter";
                break;
            case PMI::Primitive::Sphere:
                type = "SphericalDiameter";
                break;
            case PMI::Primitive::Torus:
                type = "Radius";
                break;
            case PMI::Primitive::Cone:
                type = "AngularSize";
                break;
            default:
                break;
        }
        if (!type) {
            QMessageBox::information(Gui::getMainWindow(), QObject::tr("PMI"),
                                     QObject::tr("Dimension: one face has a size when it is a "
                                                 "cylinder, a sphere, a cone or a fillet; pick "
                                                 "two elements for the distance between them"));
            abortCommand();
            return;
        }
        makeFeature(this, picks, nullptr);
        addToSet(this, "Dimension", "Dimension");
        doCommand(Doc, "_obj.Type = '%s'", type);
        doCommand(Doc, "_obj.Features = [_feat]");
    }
    else {
        const auto& b = picks[1].surface;
        // Two faces facing apart, a pattern of such pairs, a slot
        std::vector<PMI::Primitive> all;
        for (auto& pick : picks) {
            all.push_back(pick.surface);
        }
        double size = 0;
        bool width = PMI::Geom::width(all, size);
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
            bool parallel = a.dir.IsParallel(b.dir, 1e-9) || a.isAxial() || b.isAxial()
                || a.type == PMI::Primitive::Point || b.type == PMI::Primitive::Point;
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

// ---- PMI view ----

DEF_STD_CMD_A(CmdPMIQuery)

CmdPMIQuery::CmdPMIQuery()
    : Command("PMI_Query")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("What applies");
    sToolTipText = QT_TR_NOOP("Pick a face: list the annotations on it and the tolerances that "
                              "cite a datum on it");
    sWhatsThis = "PMI_Query";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_Query";
}

void CmdPMIQuery::activated(int)
{
    if (Gui::Control().activeDialog()) {
        return;
    }
    Gui::Control().showDialog(new PMIGui::TaskDlgQuery);
}

bool CmdPMIQuery::isActive()
{
    return haveDocument(this) && !Gui::Control().activeDialog();
}

DEF_STD_CMD_A(CmdPMIView)

CmdPMIView::CmdPMIView()
    : Command("PMI_View")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("PMI view");
    sToolTipText = QT_TR_NOOP("A view of the annotations from the camera as it is, made "
                              "active: what is annotated now is shown in it and lies in its "
                              "plane. Double-click a view to activate it or let it go.");
    sWhatsThis = "PMI_View";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_View";
}

void CmdPMIView::activated(int)
{
    auto gdoc = getActiveGuiDocument();
    auto mdi = gdoc ? qobject_cast<Gui::View3DInventor*>(gdoc->getActiveView()) : nullptr;
    if (!mdi) {
        return;
    }
    // The set: the active one, the one of what is picked, the active
    // Body's, or the document's only one
    App::DocumentObject* set = mdi->getActiveObject<App::DocumentObject*>(PMIGui::PMIKEY);
    App::DocumentObject* part = nullptr;
    if (!set) {
        for (auto& sel : Gui::Selection().getSelectionEx()) {
            auto obj = sel.getObject();
            if (obj && obj->isDerivedFrom<PMI::AnnotationSet>()) {
                set = obj;
            }
            else if (auto anno = freecad_cast<PMI::Annotation*>(obj)) {
                set = anno->getSet();
            }
            if (set) {
                break;
            }
        }
    }
    if (!set) {
        part = mdi->getActiveObject<App::DocumentObject*>("pdbody");
    }
    if (!set && !part) {
        auto sets = gdoc->getDocument()->getObjectsOfType(PMI::AnnotationSet::getClassTypeId());
        if (sets.size() == 1) {
            set = sets.front();
        }
    }
    if (!set && !part) {
        Base::Console().Warning("PMI view: no annotation set to put it in -- activate one "
                                "(double-click it) or a Body\n");
        return;
    }

    SoCamera* cam = mdi->getViewer()->getSoRenderManager()->getCamera();
    SbRotation rot = cam->orientation.getValue();
    float q0, q1, q2, q3;
    rot.getValue(q0, q1, q2, q3);
    SbVec3f z;
    rot.multVec(SbVec3f(0, 0, 1), z);
    SbVec3f target = cam->position.getValue() - z * cam->focalDistance.getValue();
    bool ortho = cam->isOfType(SoOrthographicCamera::getClassTypeId());

    openCommand(QT_TRANSLATE_NOOP("Command", "PMI view"));
    doCommand(Doc, "import PMI");
    if (set) {
        doCommand(Doc, "_set = %s", Gui::Command::getObjectCmd(set).c_str());
    }
    else {
        doCommand(Doc, "_set = PMI.getSet(%s)", Gui::Command::getObjectCmd(part).c_str());
    }
    addToSet(this, "View", "View");
    doCommand(Doc,
              "_obj.Camera = App.Placement(App.Vector(%.9g, %.9g, %.9g), "
              "App.Rotation(%.9g, %.9g, %.9g, %.9g))",
              target[0], target[1], target[2], q0, q1, q2, q3);
    if (ortho) {
        doCommand(Doc, "_obj.Height = %.9g",
                  static_cast<SoOrthographicCamera*>(cam)->height.getValue());
    }
    else if (cam->isOfType(SoPerspectiveCamera::getClassTypeId())) {
        // The height seen at the focal distance, as the view gives it back
        // (none was kept: shown again, it was fitted to the whole model)
        auto persp = static_cast<SoPerspectiveCamera*>(cam);
        double height = 2.0 * cam->focalDistance.getValue()
            * std::tan(persp->heightAngle.getValue() / 2.0);
        doCommand(Doc, "_obj.Perspective = True");
        doCommand(Doc, "_obj.Height = %.9g", height);
    }
    else {
        doCommand(Doc, "_obj.Perspective = True");
    }
    doCommand(Doc, "_set.Document.recompute()");
    commitCommand();
    doCommand(Gui, "Gui.ActiveDocument.ActiveView.setActiveObject('%s', _obj)",
              PMIGui::PMIVIEWKEY);
}

bool CmdPMIView::isActive()
{
    return haveDocument(this);
}

DEF_STD_CMD_A(CmdPMIViewNewWindow)

CmdPMIViewNewWindow::CmdPMIViewNewWindow()
    : Command("PMI_ViewNewWindow")
{
    sAppModule = "PMI";
    sGroup = QT_TR_NOOP("PMI");
    sMenuText = QT_TR_NOOP("Show in new 3D view");
    sToolTipText = QT_TR_NOOP("Make the selected PMI view active in a new 3D view of the "
                              "document, beside the views there are: each 3D view shows a PMI "
                              "view of its own");
    sWhatsThis = "PMI_ViewNewWindow";
    sStatusTip = sToolTipText;
    sPixmap = "PMI_View";
}

namespace
{
PMIGui::ViewProviderView* selectedView()
{
    for (auto& sel : Gui::Selection().getSelectionEx()) {
        auto obj = sel.getObject();
        if (obj && obj->isDerivedFrom<PMI::View>()) {
            return freecad_cast<PMIGui::ViewProviderView*>(
                Gui::Application::Instance->getViewProvider(obj));
        }
    }
    return nullptr;
}
}  // namespace

void CmdPMIViewNewWindow::activated(int)
{
    if (auto vp = selectedView()) {
        vp->showInNewView();
    }
}

bool CmdPMIViewNewWindow::isActive()
{
    return selectedView() != nullptr;
}

void CreatePMICommands()
{
    Gui::CommandManager& mgr = Gui::Application::Instance->commandManager();
    mgr.addCommand(new CmdPMIDatum());
    mgr.addCommand(new CmdPMIDatumTarget());
    mgr.addCommand(new CmdPMIGeometricTolerance());
    mgr.addCommand(new CmdPMIDimension());
    mgr.addCommand(new CmdPMINote());
    mgr.addCommand(new CmdPMIView());
    mgr.addCommand(new CmdPMIViewNewWindow());
    mgr.addCommand(new CmdPMICheck());
    mgr.addCommand(new CmdPMIQuery());
}
