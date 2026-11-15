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
#include <array>
#include <cmath>
#include <map>
#include <set>
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

/// A labelled piece of a source edge, the longest the view shows
struct Found
{
    std::string text;
    Base::Vector3d start;
    Base::Vector3d end;
    double length = -1;
    /// The source edge of the piece labelled
    std::string source;
    /// One label for the text, not for each edge (EdgeInfo::oncePerLabel)
    bool once = false;
    std::string bubble;
    /// A part's number: every visible piece of it, its middle and its source
    /// edge, for the leader to start on the one that suits its balloon
    std::vector<std::pair<Base::Vector3d, std::string>> pieces;
};

/// Where a leader starts: a point on a piece of its part, and the piece's
/// source edge
using Anchor = std::pair<Base::Vector3d, std::string>;

/** Where each part's number to lay stands, by its text: on the box of the
 * view's visible edges grown by a balloon and a gap, where the line from the
 * box's middle through its piece meets it, then spread along the box so no
 * two meet -- nor meet one the user moved (`kept`) that stands on it. Each
 * leader then starts on the piece of its part whose way to its balloon
 * crosses fewest of the drawing's lines (`anchors`, by text), and two whose
 * leaders cross swap places, the pieces chosen again after. In the view's
 * scaled frame, Y down, as the edges are. `laid`: the texts to lay, new
 * ones and those not moved since the last import.
 */
std::map<std::string, Base::Vector3d> ringStands(DrawViewPart* dvp,
                                                 const std::map<std::string, Found>& found,
                                                 const std::set<std::string>& laid,
                                                 const std::vector<Base::Vector3d>& kept,
                                                 std::map<std::string, Anchor>& anchors)
{
    // a balloon of a number or two, its circle about 13 mm across on the
    // paper at the default label size (measured on the vg page), and a gap
    constexpr double diameter = 14.0, gap = 3.0;
    std::map<std::string, Base::Vector3d> stands;
    double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
    for (const auto& geom : dvp->getEdgeGeometry()) {
        if (!geom || !geom->getHlrVisible()) {
            continue;
        }
        for (const auto& p : {geom->getStartPoint(), geom->getEndPoint(), geom->getMidPoint()}) {
            xmin = std::min(xmin, p.x);
            xmax = std::max(xmax, p.x);
            ymin = std::min(ymin, p.y);
            ymax = std::max(ymax, p.y);
        }
    }
    if (xmin > xmax) {
        return stands;
    }
    // The drawing's lines, as a leader would cross them: each visible edge
    // by its ends and middle
    std::vector<std::array<Base::Vector3d, 3>> lines;
    for (const auto& geom : dvp->getEdgeGeometry()) {
        if (geom && geom->getHlrVisible()) {
            lines.push_back({geom->getStartPoint(), geom->getMidPoint(), geom->getEndPoint()});
        }
    }
    auto side = [](const Base::Vector3d& p, const Base::Vector3d& q, const Base::Vector3d& r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    // Strictly: a segment ending on the other, as a leader on its own piece
    // does, does not cross it
    auto meets = [&side](const Base::Vector3d& a, const Base::Vector3d& b,
                         const Base::Vector3d& c, const Base::Vector3d& d) {
        return side(a, b, c) * side(a, b, d) < -1e-9 && side(c, d, a) * side(c, d, b) < -1e-9;
    };
    auto crossings = [&](const Base::Vector3d& from, const Base::Vector3d& to) {
        int count = 0;
        for (const auto& l : lines) {
            count += meets(from, to, l[0], l[1]) ? 1 : 0;
            count += meets(from, to, l[1], l[2]) ? 1 : 0;
        }
        return count;
    };
    const Base::Vector3d middle((xmin + xmax) / 2, (ymin + ymax) / 2, 0);
    const double hw = (xmax - xmin) / 2 + diameter / 2 + gap;
    const double hh = (ymax - ymin) / 2 + diameter / 2 + gap;
    const double perimeter = 4 * (hw + hh);
    // A place on the ring by its distance along it from the right side's
    // middle, the way Y grows first
    auto point = [&](double t) {
        t = std::fmod(t, perimeter);
        if (t < 0) {
            t += perimeter;
        }
        const double sides[] = {hh, 2 * hw, 2 * hh, 2 * hw, hh};
        using V = Base::Vector3d;
        const V corners[] = {V(hw, 0, 0), V(hw, hh, 0), V(-hw, hh, 0), V(-hw, -hh, 0),
                             V(hw, -hh, 0)};
        const V ways[] = {V(0, 1, 0), V(-1, 0, 0), V(0, -1, 0), V(1, 0, 0), V(0, 1, 0)};
        for (int k = 0; k < 5; ++k) {
            if (t <= sides[k] || k == 4) {
                return middle + corners[k] + ways[k] * t;
            }
            t -= sides[k];
        }
        return middle;
    };
    // Where the line from the middle through `at` meets the ring
    auto place = [&](Base::Vector3d at) {
        at = at - middle;
        if (at.Length() < 1e-9) {
            at = Base::Vector3d(0, -1, 0);
        }
        const double k = std::min(std::abs(at.x) > 1e-12 ? hw / std::abs(at.x) : 1e300,
                                  std::abs(at.y) > 1e-12 ? hh / std::abs(at.y) : 1e300);
        const Base::Vector3d p = at * k;
        if (std::abs(p.x - hw) < 1e-6 && p.y >= 0) {
            return p.y;
        }
        if (std::abs(p.y - hh) < 1e-6) {
            return hh + (hw - p.x);
        }
        if (std::abs(p.x + hw) < 1e-6) {
            return hh + 2 * hw + (hh - p.y);
        }
        if (std::abs(p.y + hh) < 1e-6) {
            return 3 * hh + 2 * hw + (p.x + hw);
        }
        return 3 * hh + 4 * hw + (p.y + hh);
    };
    const double need = diameter + gap;
    struct Stand
    {
        std::string text;
        double t;
        /// Where its leader starts: the middle of its longest piece, then
        /// of the piece whose leader to the stand crosses fewest lines
        Base::Vector3d anchor;
        /// One the user moved, standing on the ring: the others keep clear
        bool fixed = false;
        const Found* found = nullptr;
        std::string edge;
    };
    std::vector<Stand> todo;
    for (const auto& [key, f] : found) {
        if (f.once && laid.count(f.text)) {
            const Base::Vector3d anchor = (f.start + f.end) / 2;
            todo.push_back({f.text, place(anchor), anchor, false, &f, f.source});
        }
    }
    if (todo.empty()) {
        return stands;
    }
    for (const auto& at : kept) {
        const double t = place(at);
        if ((point(t) - at).Length() < need) {
            todo.push_back({std::string(), t, at, true});
        }
    }
    std::sort(todo.begin(), todo.end(), [](const Stand& l, const Stand& r) {
        return l.t < r.t;
    });
    // Push neighbours apart along the ring until none meet, or as near as a
    // ring that full allows; one the user moved stays
    const std::size_t n = todo.size();
    for (int round = 0; round < 500 && n > 1; ++round) {
        bool moved = false;
        for (std::size_t i = 0; i < n; ++i) {
            Stand& l = todo[i];
            Stand& r = todo[(i + 1) % n];
            if (l.fixed && r.fixed) {
                continue;
            }
            const double apart = (point(r.t) - point(l.t)).Length();
            if (apart >= need - 1e-6) {
                continue;
            }
            const double push = std::min(need, need - apart) / 2 + 1e-3;
            l.t -= r.fixed ? 2 * push : (l.fixed ? 0 : push);
            r.t += l.fixed ? 2 * push : (r.fixed ? 0 : push);
            moved = true;
        }
        if (!moved) {
            break;
        }
    }
    // Each leader from the piece of its part that crosses fewest of the
    // drawing's lines on the way to its stand, the nearest of those; then
    // two leaders crossing swap their places -- each swap makes the leaders
    // shorter in all, so it ends, and the places stay as spread -- and the
    // pieces are chosen again for the places swapped
    auto choose = [&](Stand& st) {
        const Base::Vector3d to = point(st.t);
        int best = -1;
        double nearest = 0;
        for (const auto& [at, edge] : st.found->pieces) {
            const int count = crossings(at, to);
            const double far = (to - at).Length();
            if (best < 0 || count < best || (count == best && far < nearest - 1e-9)) {
                best = count;
                nearest = far;
                st.anchor = at;
                st.edge = edge;
            }
        }
    };
    auto crosses = [&](const Stand& a, const Stand& b) {
        return meets(a.anchor, point(a.t), b.anchor, point(b.t));
    };
    for (int pass = 0; pass < 4; ++pass) {
        for (auto& st : todo) {
            if (!st.fixed) {
                choose(st);
            }
        }
        bool swappedAny = false;
        for (std::size_t round = 0; round < 4 * n * n; ++round) {
            bool swapped = false;
            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = i + 1; j < n; ++j) {
                    if (!todo[i].fixed && !todo[j].fixed && crosses(todo[i], todo[j])) {
                        std::swap(todo[i].t, todo[j].t);
                        swapped = swappedAny = true;
                    }
                }
            }
            if (!swapped) {
                break;
            }
        }
        if (!swappedAny) {
            break;
        }
    }
    for (const auto& st : todo) {
        if (!st.fixed) {
            stands[st.text] = point(st.t);
            anchors[st.text] = {st.anchor, st.edge};
        }
    }
    return stands;
}

/** The labels a drawing view's sources give its edges, made or brought up
 * to date (Gui::ViewProvider::getEdgeInfo(), docs/SheetMetal.md sec 5.6)
 *
 * One label for each labelled source edge, past an end of its longest piece
 * -- the ends taken in turn across the view, so labels of edges side by side
 * do not meet -- its leader to the piece a tenth in from that end. One there
 * already moves with its edge, keeping
 * where the user put it against the edge; its text is the source's new one
 * unless the user wrote their own. One whose edge is gone goes, unless the
 * user wrote its text. A part's number (EdgeInfo::oncePerLabel) is one
 * label for the text, on the middle of the longest piece the view shows of
 * any edge with it, its balloon on a ring around the view (ringStands()).
 * Returns how many labels there are.
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
        // One for a text -- a part's number -- on a piece the view shows
        if (info.oncePerLabel && !geom->getHlrVisible()) {
            continue;
        }
        const std::string key = info.oncePerLabel ? "\x1f" + info.label : name;
        const Base::Vector3d start = geom->getStartPoint(), end = geom->getEndPoint();
        Found& f = found[key];
        if ((end - start).Length() > f.length) {
            auto pieces = std::move(f.pieces);
            f = {info.label, start, end, (end - start).Length(), name, info.oncePerLabel,
                 info.bubble};
            f.pieces = std::move(pieces);
        }
        if (info.oncePerLabel) {
            f.pieces.emplace_back(geom->getMidPoint(), name);
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
    // A part's number is laid on the ring when new, or again when the user
    // has not moved it since the last import; one moved stays, and the
    // others keep clear of it
    auto earlier = [&existing](const Found& f) -> DrawEdgeLabel* {
        auto it = existing.find(f.source);
        if (it == existing.end() && f.once) {
            it = std::find_if(existing.begin(), existing.end(), [&f](const auto& e) {
                return f.text == e.second->SourceText.getValue();
            });
        }
        return it != existing.end() ? it->second : nullptr;
    };
    std::set<std::string> laid;
    std::vector<Base::Vector3d> kept;
    for (const auto& [key, f] : found) {
        if (!f.once) {
            continue;
        }
        auto label = earlier(f);
        if (!label || !label->isMoved()) {
            laid.insert(f.text);
        }
        else {
            // where it will stand: at its offset from its piece's middle
            const Base::Vector3d at = (f.start + f.end) / 2;
            kept.emplace_back(at.x + (label->X.getValue() - label->OriginX.getValue()) * scale,
                              at.y - (label->Y.getValue() - label->OriginY.getValue()) * scale,
                              0);
        }
    }
    std::map<std::string, Anchor> anchors;
    const std::map<std::string, Base::Vector3d> ring =
        ringStands(dvp, found, laid, kept, anchors);
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
        const Found& f = *order[i].second;
        std::string name = f.source;
        // the end it stands past -- the upper one, or the left one of an
        // edge across, then the other for the next edge -- and the way out
        // of the edge there; the view's Y runs down
        const bool startFirst = std::abs(f.start.y - f.end.y) > 1e-6 ? f.start.y < f.end.y
                                                                       : f.start.x < f.end.x;
        const bool atStart = (i % 2 == 0) == startFirst;
        const Base::Vector3d tip = atStart ? f.start : f.end;
        const Base::Vector3d from = atStart ? f.end : f.start;
        Base::Vector3d out = tip - from;
        Base::Vector3d at;
        if (f.once) {
            // A part's number: on the middle of its piece, standing out from
            // the view's middle, where the view is placed -- laid on the
            // ring, on the piece its leader crosses fewest lines from
            at = (f.start + f.end) / 2;
            auto anchor = anchors.find(f.text);
            if (anchor != anchors.end()) {
                at = anchor->second.first;
                name = anchor->second.second;
            }
            out = at;
        }
        if (out.Length() < 1e-9) {
            out = Base::Vector3d(0, -1, 0);
        }
        out.Normalize();
        if (!f.once) {
            at = tip - out * (f.length / 10);
        }
        const double ox = at.x / scale, oy = -at.y / scale;
        DrawEdgeLabel* label = nullptr;
        double dx, dy;
        auto it = existing.find(name);
        if (it == existing.end() && f.once) {
            // Its text's label from before, on another piece then
            it = std::find_if(existing.begin(), existing.end(), [&f](const auto& e) {
                return f.text == e.second->SourceText.getValue();
            });
        }
        // a part's number on the ring around the view; Y up
        auto onRing = f.once ? ring.find(f.text) : ring.end();
        bool stood = false;
        if (it != existing.end()) {
            label = it->second;
            existing.erase(it);
            label->SourceEdge.setValue(name);
            dx = label->X.getValue() - label->OriginX.getValue();
            dy = label->Y.getValue() - label->OriginY.getValue();
        }
        else {
            label = static_cast<DrawEdgeLabel*>(
                doc->addObject("TechDraw::DrawEdgeLabel", "EdgeLabel"));
            page->addView(label);
            label->SourceView.setValue(dvp);
            label->SourceEdge.setValue(name);
            label->BubbleShape.setValue(f.bubble.empty() ? "None" : f.bubble.c_str());
            if (f.once) {
                // a straight leader, as the ring lays them: a kink's
                // shoulder could cross a neighbour's
                label->KinkLength.setValue(0.0);
            }
            // 12 mm on the paper past the end, clear of the view's frame and
            // its name
            const Base::Vector3d stand = (f.once ? at : tip) + out * 12;
            dx = stand.x / scale - ox;
            dy = -stand.y / scale - oy;
            stood = true;
        }
        if (onRing != ring.end()) {
            dx = onRing->second.x / scale - ox;
            dy = -onRing->second.y / scale - oy;
            stood = true;
        }
        label->OriginX.setValue(ox);
        label->OriginY.setValue(oy);
        label->X.setValue(ox + dx);
        label->Y.setValue(oy + dy);
        if (stood) {
            label->setImported();
        }
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
