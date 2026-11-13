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
#include <sstream>
#include <string>
#include <vector>

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>

#include "Svg.h"

namespace PartDesign::SheetMetal
{

namespace
{

/// A layer: its name, how it is drawn, and what is on it
struct Layer
{
    const char* name;
    const char* style;
    std::ostringstream body;
};

/// Looking down on the flat: SVG's Y runs down
std::string xy(const gp_Pnt& p)
{
    std::ostringstream s;
    s << std::setprecision(12) << p.X() << ' ' << -p.Y();
    return s.str();
}

std::string num(double v)
{
    std::ostringstream s;
    s << std::setprecision(12) << v;
    return s.str();
}

void line(std::ostream& out, const gp_Pnt& a, const gp_Pnt& b)
{
    out << "    <line x1=\"" << num(a.X()) << "\" y1=\"" << num(-a.Y()) << "\" x2=\"" << num(b.X())
        << "\" y2=\"" << num(-b.Y()) << "\"/>\n";
}

void edge(std::ostream& out, const TopoDS_Edge& e)
{
    BRepAdaptor_Curve c(e);
    const double f = c.FirstParameter(), l = c.LastParameter();
    if (c.GetType() == GeomAbs_Line) {
        line(out, c.Value(f), c.Value(l));
        return;
    }
    if (c.GetType() == GeomAbs_Circle) {
        const gp_Circ circ = c.Circle();
        const gp_Pnt center = circ.Location();
        if (std::abs(l - f - 2 * std::numbers::pi) < 1e-9) {
            out << "    <circle cx=\"" << num(center.X()) << "\" cy=\"" << num(-center.Y())
                << "\" r=\"" << num(circ.Radius()) << "\"/>\n";
            return;
        }
        // counterclockwise about +Z from start to end; with Y turned to run
        // down, that is a negative angle in SVG's own frame: sweep 0
        gp_Pnt start = c.Value(f), end = c.Value(l);
        if (circ.Axis().Direction().Z() < 0) {
            std::swap(start, end);
        }
        const bool large = l - f > std::numbers::pi;
        out << "    <path d=\"M " << xy(start) << " A " << num(circ.Radius()) << ' '
            << num(circ.Radius()) << " 0 " << (large ? 1 : 0) << " 0 " << xy(end) << "\"/>\n";
        return;
    }
    // any other curve as a polyline
    constexpr int n = 64;
    out << "    <polyline points=\"";
    for (int i = 0; i <= n; ++i) {
        const gp_Pnt p = c.Value(f + (l - f) * i / n);
        out << (i ? " " : "") << num(p.X()) << ',' << num(-p.Y());
    }
    out << "\"/>\n";
}

}  // namespace

void writeSVG(std::ostream& out, const FlatResult& flat, bool tangents)
{
    const std::vector<TopoDS_Face> bottom = facesOnXY(flat.solid);

    Layer layers[] = {
        {"OUTLINE", "fill=\"none\" stroke=\"#000000\"", {}},
        {"INNER", "fill=\"none\" stroke=\"#000000\"", {}},
        {"BEND_UP", "fill=\"none\" stroke=\"#0050d0\" stroke-dasharray=\"2 1\"", {}},
        {"BEND_DOWN", "fill=\"none\" stroke=\"#d03030\" stroke-dasharray=\"2 1\"", {}},
        {"BEND_TANGENT", "fill=\"none\" stroke=\"#808080\" stroke-dasharray=\"0.5 0.5\"", {}},
        {"CROSS_BREAK", "fill=\"none\" stroke=\"#e08000\" stroke-dasharray=\"4 1 1 1\"", {}},
        {"GRAIN", "fill=\"none\" stroke=\"#208020\"", {}},
        {"MARKING", "fill=\"none\" stroke=\"#a020a0\"", {}},
        {"FORMS", "fill=\"none\" stroke=\"#008080\" stroke-dasharray=\"3 1\"", {}},
        {"LABELS",
         "fill=\"#000000\" stroke=\"none\" font-family=\"sans-serif\" font-size=\"2.5\"",
         {}},
    };
    auto layer = [&](const char* name) -> std::ostringstream& {
        for (Layer& l : layers) {
            if (std::string(l.name) == name) {
                return l.body;
            }
        }
        return layers[0].body;
    };

    Bnd_Box box;
    for (const TopoDS_Face& face : bottom) {
        BRepBndLib::AddOptimal(face, box, /*useTriangulation*/ false, /*useShapeTolerance*/ false);
        const TopoDS_Wire outer = BRepTools::OuterWire(face);
        for (TopExp_Explorer it(face, TopAbs_WIRE); it.More(); it.Next()) {
            const TopoDS_Wire& wire = TopoDS::Wire(it.Current());
            auto& to = layer(wire.IsSame(outer) ? "OUTLINE" : "INNER");
            for (BRepTools_WireExplorer e(wire, face); e.More(); e.Next()) {
                edge(to, e.Current());
            }
        }
    }
    for (const auto& bend : flat.bendLines) {
        TopoDS_Vertex v0, v1;
        TopExp::Vertices(bend.edge, v0, v1);
        const gp_Pnt a = BRep_Tool::Pnt(v0), b = BRep_Tool::Pnt(v1);
        // as far as it lies on the blank, labelled at its middle
        for (const TopoDS_Edge& segment : bend.segments) {
            edge(layer(bend.up ? "BEND_UP" : "BEND_DOWN"), segment);
        }
        const gp_Pnt mid((a.X() + b.X()) / 2, (a.Y() + b.Y()) / 2, 0);
        layer("LABELS") << "    <text x=\"" << num(mid.X()) << "\" y=\"" << num(-mid.Y())
                        << "\">" << bend.name << "</text>\n";
        if (tangents) {
            for (const auto& segments : bend.tangentSegments) {
                for (const TopoDS_Edge& tangent : segments) {
                    edge(layer("BEND_TANGENT"), tangent);
                }
            }
        }
    }
    for (const auto& cross : flat.crossBreaks) {
        for (const TopoDS_Edge& e : cross.diagonals) {
            edge(layer("CROSS_BREAK"), e);
        }
        // its note where the diagonals cross
        BRepAdaptor_Curve c(cross.diagonals[0]);
        const gp_Pnt mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        layer("LABELS") << "    <text x=\"" << num(mid.X()) << "\" y=\"" << num(-mid.Y())
                        << "\">" << crossBreakNote(cross, "&#176;") << "</text>\n";
    }
    for (const TopoDS_Edge& e : flat.grain) {
        edge(layer("GRAIN"), e);
    }
    for (const TopoDS_Edge& e : flat.markings) {
        edge(layer("MARKING"), e);
    }
    for (const auto& form : flat.forms) {
        for (const TopoDS_Edge& e : form.edges) {
            edge(layer("FORMS"), e);
        }
    }

    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    const double width = x1 - x0, height = y1 - y0;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<svg xmlns=\"http://www.w3.org/2000/svg\"\n"
        << "     xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\"\n"
        << "     width=\"" << num(width) << "mm\" height=\"" << num(height) << "mm\"\n"
        << "     viewBox=\"" << num(x0) << ' ' << num(-y1) << ' ' << num(width) << ' '
        << num(height) << "\">\n";
    for (Layer& l : layers) {
        const std::string body = l.body.str();
        if (body.empty()) {
            continue;
        }
        out << "  <g id=\"" << l.name << "\" inkscape:groupmode=\"layer\" inkscape:label=\""
            << l.name << "\" stroke-width=\"0.1\" " << l.style << ">\n"
            << body << "  </g>\n";
    }
    out << "</svg>\n";
}

}  // namespace PartDesign::SheetMetal
