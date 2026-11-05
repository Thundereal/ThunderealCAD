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
#include <map>
#include <string>
#include <vector>

#include <App/Document.h>
#include <Base/Console.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Gui/Selection.h>
#include <Gui/ViewProvider.h>
#include <Mod/TechDraw/App/DrawEdgeLabel.h>
#include <Mod/TechDraw/App/DrawPage.h>
#include <Mod/TechDraw/App/DrawViewPart.h>
#include <Mod/TechDraw/App/Geometry.h>

#include "DrawGuiUtil.h"

using namespace TechDraw;

namespace
{

/** The labels a drawing view's sources give its edges, made or brought up
 * to date (Gui::ViewProvider::getEdgeInfo(), docs/SheetMetal.md sec 5.6)
 *
 * One label for each labelled source edge, past an end of its longest piece
 * -- the ends taken in turn across the view, so labels of edges side by side
 * do not meet -- its leader to the piece a tenth in from that end. One there
 * already moves with its edge, keeping
 * where the user put it against the edge; its text is the source's new one
 * unless the user wrote their own. One whose edge is gone goes, unless the
 * user wrote its text. Returns how many labels there are.
 */
int importEdgeLabels(DrawViewPart* dvp)
{
    DrawPage* page = dvp->findParentPage();
    App::Document* doc = dvp->getDocument();
    if (!page || !doc) {
        return 0;
    }
    std::vector<Gui::ViewProvider*> views;
    for (auto source : dvp->getAllSources()) {
        if (auto view = Gui::Application::Instance->getViewProvider(source)) {
            views.push_back(view);
        }
    }
    struct Found
    {
        std::string text;
        Base::Vector3d start;
        Base::Vector3d end;
        double length = -1;
    };
    std::map<std::string, Found> found;
    for (const auto& geom : dvp->getEdgeGeometry()) {
        if (!geom || geom->getCosmetic() || geom->getSource3d().empty()) {
            continue;
        }
        const std::string& name = geom->getSource3d();
        Gui::ViewProvider::EdgeInfo info;
        bool given = false;
        for (auto view : views) {
            if (view->getEdgeInfo(name.c_str(), info)) {
                given = true;
                break;
            }
        }
        if (!given || info.label.empty()) {
            continue;
        }
        const Base::Vector3d start = geom->getStartPoint(), end = geom->getEndPoint();
        Found& f = found[name];
        if ((end - start).Length() > f.length) {
            f = {info.label, start, end, (end - start).Length()};
        }
    }

    std::map<std::string, DrawEdgeLabel*> existing;
    for (auto obj : doc->getObjectsOfType(DrawEdgeLabel::getClassTypeId())) {
        auto label = static_cast<DrawEdgeLabel*>(obj);
        if (label->SourceView.getValue() == dvp) {
            existing[label->SourceEdge.getValue()] = label;
        }
    }

    // the view's geometry is scaled, Y down; a balloon's places are the
    // view's own, unscaled, Y up
    const double scale = dvp->getScale() > 0 ? dvp->getScale() : 1;
    // the edges in order across the view, by their middles
    std::vector<std::pair<std::string, const Found*>> order;
    for (const auto& [name, f] : found) {
        order.emplace_back(name, &f);
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        const Base::Vector3d ma = (a.second->start + a.second->end) / 2;
        const Base::Vector3d mb = (b.second->start + b.second->end) / 2;
        return ma.x != mb.x ? ma.x < mb.x : ma.y < mb.y;
    });
    for (std::size_t i = 0; i < order.size(); ++i) {
        const std::string& name = order[i].first;
        const Found& f = *order[i].second;
        // the end it stands past -- the upper one, or the left one of an
        // edge across, then the other for the next edge -- and the way out
        // of the edge there; the view's Y runs down
        const bool startFirst = std::abs(f.start.y - f.end.y) > 1e-6 ? f.start.y < f.end.y
                                                                       : f.start.x < f.end.x;
        const bool atStart = (i % 2 == 0) == startFirst;
        const Base::Vector3d tip = atStart ? f.start : f.end;
        const Base::Vector3d from = atStart ? f.end : f.start;
        Base::Vector3d out = tip - from;
        if (out.Length() < 1e-9) {
            out = Base::Vector3d(0, -1, 0);
        }
        out.Normalize();
        const Base::Vector3d at = tip - out * (f.length / 10);
        const double ox = at.x / scale, oy = -at.y / scale;
        DrawEdgeLabel* label = nullptr;
        double dx, dy;
        auto it = existing.find(name);
        if (it != existing.end()) {
            label = it->second;
            existing.erase(it);
            dx = label->X.getValue() - label->OriginX.getValue();
            dy = label->Y.getValue() - label->OriginY.getValue();
        }
        else {
            label = static_cast<DrawEdgeLabel*>(
                doc->addObject("TechDraw::DrawEdgeLabel", "EdgeLabel"));
            page->addView(label);
            label->SourceView.setValue(dvp);
            label->SourceEdge.setValue(name);
            label->BubbleShape.setValue("None");
            // 12 mm on the paper past the end, clear of the view's frame and
            // its name; Y up
            const Base::Vector3d stand = tip + out * 12;
            dx = stand.x / scale - ox;
            dy = -stand.y / scale - oy;
        }
        label->OriginX.setValue(ox);
        label->OriginY.setValue(oy);
        label->X.setValue(ox + dx);
        label->Y.setValue(oy + dy);
        if (!label->isCustomized()) {
            label->Text.setValue(f.text);
        }
        label->SourceText.setValue(f.text);
    }
    for (const auto& [name, label] : existing) {
        if (label->isCustomized()) {
            Base::Console().userWarning("%s: its edge is gone; kept, as its text is your own\n",
                                        label->Label.getValue());
            continue;
        }
        doc->removeObject(label->getNameInDocument());
    }
    return static_cast<int>(found.size());
}

}  // namespace

//===========================================================================
// TechDraw_ImportEdgeLabels
//===========================================================================

DEF_STD_CMD_A(CmdTechDrawImportEdgeLabels)

CmdTechDrawImportEdgeLabels::CmdTechDrawImportEdgeLabels()
    : Command("TechDraw_ImportEdgeLabels")
{
    sAppModule = "TechDraw";
    sGroup = QT_TR_NOOP("TechDraw");
    sMenuText = QT_TR_NOOP("Import Edge Labels");
    sToolTipText = QT_TR_NOOP("Labels the edges of the selected views as their source gives them\n"
                              "-- a sheet metal flat's bend notes -- each a balloon to edit;\n"
                              "again, brings them up to date, keeping the texts you wrote");
    sWhatsThis = "TechDraw_ImportEdgeLabels";
    sStatusTip = sToolTipText;
    sPixmap = "TechDraw_Balloon";
}

void CmdTechDrawImportEdgeLabels::activated(int iMsg)
{
    Q_UNUSED(iMsg);
    std::vector<DrawViewPart*> parts;
    for (auto obj : getSelection().getObjectsOfType(DrawViewPart::getClassTypeId())) {
        parts.push_back(static_cast<DrawViewPart*>(obj));
    }
    if (parts.empty()) {
        Base::Console().userWarning("%s\n", "Import Edge Labels: select a view of a part");
        return;
    }
    openCommand(QT_TRANSLATE_NOOP("Command", "Import Edge Labels"));
    int count = 0;
    for (auto dvp : parts) {
        count += importEdgeLabels(dvp);
    }
    updateActive();
    commitCommand();
    Base::Console().message("Import Edge Labels: %d label(s)\n", count);
}

bool CmdTechDrawImportEdgeLabels::isActive()
{
    return TechDrawGui::DrawGuiUtil::needView(this, false);
}

void CreateTechDrawCommandsEdgeLabels()
{
    Gui::CommandManager& rcCmdMgr = Gui::Application::Instance->commandManager();
    rcCmdMgr.addCommand(new CmdTechDrawImportEdgeLabels());
}
