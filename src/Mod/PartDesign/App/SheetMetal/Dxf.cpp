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

#include <cmath>
#include <iomanip>
#include <numbers>

#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>

#include <Base/Exception.h>

#include "Dxf.h"

namespace PartDesign::SheetMetal
{

namespace
{

struct Writer
{
    std::ostream& out;

    void pair(int code, const std::string& value)
    {
        out << code << "\n" << value << "\n";
    }
    void pair(int code, double value)
    {
        out << code << "\n" << std::setprecision(12) << value << "\n";
    }
    void point(int base, const gp_Pnt& p)
    {
        pair(base, p.X());
        pair(base + 10, p.Y());
        pair(base + 20, 0.0);
    }
    void line(const char* layer, const gp_Pnt& a, const gp_Pnt& b)
    {
        pair(0, "LINE");
        pair(8, layer);
        point(10, a);
        point(11, b);
    }
    void text(const char* layer, const gp_Pnt& at, double height, const std::string& s)
    {
        pair(0, "TEXT");
        pair(8, layer);
        point(10, at);
        pair(40, height);
        pair(1, s);
    }

    void edge(const char* layer, const TopoDS_Edge& e)
    {
        BRepAdaptor_Curve c(e);
        const double f = c.FirstParameter(), l = c.LastParameter();
        if (c.GetType() == GeomAbs_Line) {
            line(layer, c.Value(f), c.Value(l));
            return;
        }
        if (c.GetType() == GeomAbs_Circle) {
            const gp_Circ circ = c.Circle();
            const gp_Pnt center = circ.Location();
            pair(0, std::abs(l - f - 2 * std::numbers::pi) < 1e-9 ? "CIRCLE" : "ARC");
            pair(8, layer);
            point(10, center);
            pair(40, circ.Radius());
            if (std::abs(l - f - 2 * std::numbers::pi) < 1e-9) {
                return;
            }
            // counterclockwise about +Z from the one end to the other
            auto angle = [&](const gp_Pnt& p) {
                return std::atan2(p.Y() - center.Y(), p.X() - center.X()) * 180 / std::numbers::pi;
            };
            double a0 = angle(c.Value(f)), a1 = angle(c.Value(l));
            if (circ.Axis().Direction().Z() < 0) {
                std::swap(a0, a1);
            }
            pair(50, a0);
            pair(51, a1);
            return;
        }
        // any other curve as a polyline
        pair(0, "POLYLINE");
        pair(8, layer);
        pair(66, std::string("1"));
        point(10, gp_Pnt());
        constexpr int n = 64;
        for (int i = 0; i <= n; ++i) {
            pair(0, "VERTEX");
            pair(8, layer);
            point(10, c.Value(f + (l - f) * i / n));
        }
        pair(0, "SEQEND");
        pair(8, layer);
    }
};

}  // namespace

void writeDXF(std::ostream& out, const FlatResult& flat, bool tangents)
{
    const std::vector<TopoDS_Face> bottom = facesOnXY(flat.solid);

    Writer w {out};
    w.pair(0, "SECTION");
    w.pair(2, "ENTITIES");
    for (const TopoDS_Face& face : bottom) {
        const TopoDS_Wire outer = BRepTools::OuterWire(face);
        for (TopExp_Explorer it(face, TopAbs_WIRE); it.More(); it.Next()) {
            const TopoDS_Wire& wire = TopoDS::Wire(it.Current());
            const char* layer = wire.IsSame(outer) ? "OUTLINE" : "INNER";
            for (BRepTools_WireExplorer e(wire, face); e.More(); e.Next()) {
                w.edge(layer, e.Current());
            }
        }
    }
    for (const auto& bend : flat.bendLines) {
        TopoDS_Vertex v0, v1;
        TopExp::Vertices(bend.edge, v0, v1);
        const gp_Pnt a = BRep_Tool::Pnt(v0), b = BRep_Tool::Pnt(v1);
        // as far as it lies on the blank, labelled at its middle
        for (const TopoDS_Edge& segment : bend.segments) {
            w.edge(bend.up ? "BEND_UP" : "BEND_DOWN", segment);
        }
        const gp_Pnt mid((a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2, 0);
        w.text("LABELS", mid, 2.5, bend.name);
        if (!tangents) {
            continue;
        }
        for (const auto& segments : bend.tangentSegments) {
            for (const TopoDS_Edge& tangent : segments) {
                w.edge("BEND_TANGENT", tangent);
            }
        }
    }
    for (const auto& cross : flat.crossBreaks) {
        for (const TopoDS_Edge& e : cross.diagonals) {
            w.edge("CROSS_BREAK", e);
        }
        // its note where the diagonals cross; %%d is DXF's degree sign
        BRepAdaptor_Curve c(cross.diagonals[0]);
        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        w.text("LABELS", gp_Pnt(mid.X(), mid.Y(), 0), 2.5, crossBreakNote(cross, "%%d"));
    }
    for (const TopoDS_Edge& e : flat.grain) {
        w.edge("GRAIN", e);
    }
    for (const TopoDS_Edge& e : flat.markings) {
        w.edge("MARKING", e);
    }
    // each form's outline or cross (FormDisplay)
    for (const auto& form : flat.forms) {
        for (const TopoDS_Edge& e : form.edges) {
            w.edge("FORMS", e);
        }
    }
    w.pair(0, "ENDSEC");
    w.pair(0, "EOF");
}

}  // namespace PartDesign::SheetMetal
