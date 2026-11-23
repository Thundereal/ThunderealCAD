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
#include <utility>

#include <BRep_Tool.hxx>
#include <gp_Trsf.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>

#include <Mod/Part/App/TopoShape.h>

#include "AnnotationSet.h"
#include "Datum.h"
#include "Dimension.h"
#include "Drawing.h"
#include "Font.h"
#include "GeometricTolerance.h"
#include "Note.h"
#include "TolerancedFeature.h"

// What each annotation looks like (docs/GDandT.md sec 6.1). ISO 1101 / ASME
// Y14.5 proportions: a frame is twice the text height, its first cell
// square, the others as wide as their content and a text height of margin.
// Symbols are written as code points here, never as literal characters.

using namespace PMI;

namespace
{
using GT = GeometricTolerance;

const char32_t DiameterSign = 0x2300;
const char32_t PlusMinus = 0x00B1;
const char32_t Degree = 0x00B0;

/// The characteristics' code points, for the equivalent text
char32_t symbolOf(int c)
{
    switch (c) {
        case GT::Straightness:
            return 0x23E4;
        case GT::Flatness:
            return 0x23E5;
        case GT::Circularity:
            return 0x25CB;
        case GT::Cylindricity:
            return 0x232D;
        case GT::ProfileOfLine:
            return 0x2312;
        case GT::ProfileOfSurface:
            return 0x2313;
        case GT::Angularity:
            return 0x2220;
        case GT::Perpendicularity:
            return 0x27C2;
        case GT::Parallelism:
            return 0x2225;
        case GT::Position:
            return 0x2316;
        case GT::Concentricity:
        case GT::Coaxiality:
            return 0x25CE;
        case GT::Symmetry:
            return 0x232F;
        case GT::CircularRunout:
            return 0x2197;
        case GT::TotalRunout:
            return 0x2330;
        default:
            return '?';
    }
}

/// A modifier written as a letter in a circle, and its code point (circled
/// Latin capital, U+24B6 + letter)
bool isCircled(const std::string& m)
{
    return m == "M" || m == "L" || m == "P" || m == "F" || m == "T" || m == "U" || m == "E";
}

char32_t circledCode(char letter)
{
    return 0x24B6 + (letter - 'A');
}

std::u32string u32(const std::string& s)
{
    return std::u32string(s.begin(), s.end());
}

int decimalsOf(const Dimension& dim)
{
    if (dim.DecimalPlaces.getValue() >= 0) {
        return dim.DecimalPlaces.getValue();
    }
    if (auto set = dim.getSet()) {
        return set->DecimalPlaces.getValue();
    }
    return 2;
}

/// A compartment's content, drawn from x0 at text height h in a frame of
/// height H; returns its right edge
struct Pen
{
    Drawing& d;
    double h, H, x;
    double base() const
    {
        return (H - h) / 2;
    }
    void text(const std::u32string& s)
    {
        x += d.text(s, h, x, base());
    }
    void space(double f = 0.3)
    {
        x += f * h;
    }
    void circled(char letter)
    {
        x += d.circled(char32_t(letter), h, x, H / 2);
    }
    void modifier(const std::string& m)
    {
        space(0.2);
        if (isCircled(m)) {
            circled(m[0]);
        }
        else {
            text(u32(m));
        }
    }
};
}  // namespace

// ---- Geometric tolerance: the feature control frame ----

void GeometricTolerance::layout(Drawing& d, double h) const
{
    const double H = 2 * h;
    const double margin = 0.5 * h;
    std::vector<double> walls;

    d.characteristic(Characteristic.getValue(), 0, 0, H);
    walls.push_back(H);

    Pen pen {d, h, H, H + margin};
    switch (ZoneShape.getValue()) {
        case DiameterZone:
            pen.text(std::u32string(1, DiameterSign));
            break;
        case SphericalDiameterZone:
            pen.text(U"S" + std::u32string(1, DiameterSign));
            break;
        default:
            break;
    }
    pen.text(Callout::number(Value.getValue()));
    if (MaterialCondition.getValue() == Maximum) {
        pen.modifier("M");
    }
    else if (MaterialCondition.getValue() == Least) {
        pen.modifier("L");
    }
    for (auto& m : Modifiers.getValues()) {
        pen.modifier(m);
        if (m == "P" && ProjectedLength.getValue() > 0) {
            pen.space(0.2);
            pen.text(Callout::number(ProjectedLength.getValue()));
        }
        else if (m == "U") {
            pen.space(0.2);
            pen.text(Callout::number(UnequalValue.getValue()));
        }
    }
    switch (UnitBasis.getValue()) {
        case PerLength:
            pen.text(U"/" + Callout::number(UnitSize.getValue()));
            break;
        case PerSquare:
            pen.text(U"/" + Callout::number(UnitSize.getValue()) + U"X"
                     + Callout::number(UnitSize.getValue()));
            break;
        case PerRectangle:
            pen.text(U"/" + Callout::number(UnitSize.getValue()) + U"X"
                     + Callout::number(UnitSize2.getValue()));
            break;
        case PerCircle:
            pen.text(U"/" + std::u32string(1, DiameterSign) + Callout::number(UnitSize.getValue()));
            break;
        default:
            break;
    }
    if (MaxValue.getValue() > 0) {
        pen.space();
        pen.text(U"MAX " + Callout::number(MaxValue.getValue()));
    }
    pen.x += margin;
    walls.push_back(pen.x);

    if (auto drf = getDatumSystem()) {
        std::vector<DatumReferenceFrame::Compartment> comps;
        std::string why;
        if (drf->parse(comps, why)) {
            for (auto& comp : comps) {
                double start = pen.x;
                pen.x += margin;
                std::string label;
                for (auto& dl : comp.datums) {
                    label += (label.empty() ? "" : "-") + dl;
                }
                pen.text(u32(label));
                for (auto& m : comp.modifiers) {
                    pen.modifier(m);
                }
                pen.x += margin;
                pen.x = std::max(pen.x, start + H);
                walls.push_back(pen.x);
            }
        }
    }
    d.rect(0, 0, walls.back(), H);
    for (std::size_t i = 0; i + 1 < walls.size(); ++i) {
        d.line(walls[i], 0, walls[i], H);
    }
}

std::u32string GeometricTolerance::equivalentText() const
{
    std::u32string s(1, symbolOf(Characteristic.getValue()));
    s += U"|";
    if (ZoneShape.getValue() == DiameterZone) {
        s += DiameterSign;
    }
    else if (ZoneShape.getValue() == SphericalDiameterZone) {
        s += U"S";
        s += DiameterSign;
    }
    s += Callout::number(Value.getValue());
    if (MaterialCondition.getValue() == Maximum) {
        s += circledCode('M');
    }
    else if (MaterialCondition.getValue() == Least) {
        s += circledCode('L');
    }
    for (auto& m : Modifiers.getValues()) {
        if (isCircled(m)) {
            s += circledCode(m[0]);
        }
        else {
            s += U" " + u32(m);
        }
        if (m == "P" && ProjectedLength.getValue() > 0) {
            s += Callout::number(ProjectedLength.getValue());
        }
    }
    if (auto drf = getDatumSystem()) {
        std::vector<DatumReferenceFrame::Compartment> comps;
        std::string why;
        if (drf->parse(comps, why)) {
            for (auto& comp : comps) {
                s += U"|";
                std::string label;
                for (auto& dl : comp.datums) {
                    label += (label.empty() ? "" : "-") + dl;
                }
                s += u32(label);
                for (auto& m : comp.modifiers) {
                    if (isCircled(m)) {
                        s += circledCode(m[0]);
                    }
                    else {
                        s += u32("[" + m + "]");
                    }
                }
            }
        }
    }
    return s;
}

// ---- Datum feature: a letter in a square, a leader to a triangle ----

void Datum::layout(Drawing& d, double h) const
{
    const double H = 2 * h;
    std::u32string letter = u32(Letter.getStrValue());
    double w = std::max(H, d.textWidth(letter, h) + h);
    d.rect(0, 0, w, H);
    double tw = d.textWidth(letter, h);
    d.text(letter, h, (w - tw) / 2, (H - h) / 2);
}

std::u32string Datum::equivalentText() const
{
    return u32(Letter.getStrValue());
}

// ---- DatumTarget: the target symbol, the target on the part ----

namespace
{
/// The size a target symbol writes above its line: an area's
std::u32string targetSize(const DatumTarget& t)
{
    switch (t.getTargetType()) {
        case DatumTarget::Circle:
            return std::u32string(1, DiameterSign) + Callout::number(t.Length.getValue());
        case DatumTarget::Rectangle:
            return Callout::number(t.Length.getValue()) + U"X"
                + Callout::number(t.Width.getValue());
        default:
            return {};
    }
}

const double Pi = 3.14159265358979323846;

/// A phantom line, long dash and two short ones, in units of \a u
void phantom(Drawing& d, double x0, double y0, double x1, double y1, double u)
{
    double len = std::hypot(x1 - x0, y1 - y0);
    if (len < 1e-9) {
        return;
    }
    double dx = (x1 - x0) / len, dy = (y1 - y0) / len;
    const double pattern[] = {4 * u, 0.6 * u, u, 0.6 * u, u, 0.6 * u};
    double at = 0;
    for (int i = 0; at < len; i = (i + 1) % 6) {
        double next = std::min(len, at + pattern[i]);
        if (i % 2 == 0) {
            d.line(x0 + dx * at, y0 + dy * at, x0 + dx * next, y0 + dy * next);
        }
        at = next;
    }
}
}  // namespace

void DatumTarget::layout(Drawing& d, double h) const
{
    // A circle split by a line: the area's size above, the letter and the
    // number below (ASME Y14.5 fig 7-34, ISO 5459)
    std::u32string below = u32(label());
    std::u32string above = targetSize(*this);
    double tw = std::max(d.textWidth(below, h), d.textWidth(above, h));
    double r = std::max(2 * h, (tw + 1.5 * h) / 2);
    d.circle(r, r, r);
    d.line(0, r, 2 * r, r);
    d.text(below, h, r - d.textWidth(below, h) / 2, r / 2 - h / 2);
    if (!above.empty()) {
        d.text(above, h, r - d.textWidth(above, h) / 2, 1.5 * r - h / 2);
    }
}

TopoDS_Shape DatumTarget::mark(double h) const
{
    if (UseImported.getValue()) {
        return {};
    }
    // In the target's own plane: x along it, z out of the part
    Drawing d;
    double len = Length.getValue(), wid = Width.getValue();
    double u = std::max(0.25 * h, 1e-3);
    // Hatching at 45 degrees, \a gap apart, over an extent \a reach either
    // side of the centre; \a chord gives each line's ends
    auto hatch = [&](double gap, double reach, auto chord) {
        const double s = std::sqrt(0.5);
        int n = int(std::ceil(reach / gap));
        for (int i = -n; i <= n; ++i) {
            double c = i * gap;
            double t0, t1;
            if (!chord(c, t0, t1)) {
                continue;
            }
            // Along u = (s, s), offset along n = (-s, s)
            d.line(-c * s + t0 * s, c * s + t0 * s, -c * s + t1 * s, c * s + t1 * s);
        }
    };
    switch (getTargetType()) {
        case Point: {
            double a = 0.75 * h;
            d.line(-a, -a, a, a);
            d.line(-a, a, a, -a);
            break;
        }
        case Line:
            if (len > 0) {
                phantom(d, -len / 2, 0, len / 2, 0, u);
            }
            break;
        case Rectangle:
            if (len > 0 && wid > 0) {
                double a = len / 2, b = wid / 2;
                phantom(d, -a, -b, a, -b, u);
                phantom(d, a, -b, a, b, u);
                phantom(d, a, b, -a, b, u);
                phantom(d, -a, b, -a, -b, u);
                const double k = std::sqrt(2.0);
                double gap = std::min(h, std::min(len, wid) / 3);
                hatch(gap, a + b, [&](double c, double& t0, double& t1) {
                    t0 = std::max(c - a * k, -c - b * k);
                    t1 = std::min(c + a * k, -c + b * k);
                    return t1 - t0 > 1e-9;
                });
            }
            break;
        case Circle:
            if (len > 0) {
                double r = len / 2;
                // Dashed: long and short arcs
                double step = std::max(u, 1e-3) / r;
                double a = 0;
                for (int i = 0; a < 2 * Pi; ++i) {
                    double next = std::min(2 * Pi, a + (i % 2 == 0 ? 4 * step : step));
                    d.arc(0, 0, r, a, next);
                    a = std::min(2 * Pi, next + 0.6 * step);
                }
                hatch(std::min(h, len / 3), r, [&](double c, double& t0, double& t1) {
                    if (std::fabs(c) >= r) {
                        return false;
                    }
                    t1 = std::sqrt(r * r - c * c);
                    t0 = -t1;
                    return true;
                });
            }
            break;
        case Area:
            break;
    }
    if (d.isEmpty()) {
        return {};
    }
    gp_Trsf trsf;
    Part::TopoShape::convertTogpTrsf(Target.getValue().toMatrix(), trsf);
    return d.shape().Moved(TopLoc_Location(trsf));
}

std::u32string DatumTarget::equivalentText() const
{
    std::u32string size = targetSize(*this);
    return size.empty() ? u32(label()) : size + U" " + u32(label());
}

// ---- Dimension: its value and tolerance ----

namespace
{
std::u32string dimensionText(const Dimension& dim)
{
    std::u32string s;
    switch (dim.getType()) {
        case Dimension::Diameter:
            s += DiameterSign;
            break;
        case Dimension::Radius:
            s += U"R";
            break;
        case Dimension::SphericalDiameter:
            s += U"S";
            s += DiameterSign;
            break;
        case Dimension::SphericalRadius:
            s += U"SR";
            break;
        default:
            break;
    }
    int dec = decimalsOf(dim);
    std::u32string unit = dim.isAngular() ? std::u32string(1, Degree) : std::u32string();
    double up = dim.Upper.getValue(), lo = dim.Lower.getValue();
    if (dim.Reference.getValue()) {
        // For information: in parentheses, no tolerance
        return U"(" + s + Callout::number(dim.Nominal.getValue(), dec) + unit + U")";
    }
    if (dim.Limits.getValue() && !dim.Basic.getValue() && dim.Fit.getStrValue().empty()
        && (up != 0.0 || lo != 0.0)) {
        // The limits, the lower first on one line: 34.8-35.2
        double n = dim.Nominal.getValue();
        return s + Callout::number(n + lo, dec) + unit + U"-" + Callout::number(n + up, dec) + unit;
    }
    s += Callout::number(dim.Nominal.getValue(), dec);
    s += unit;
    if (dim.Basic.getValue()) {
        return s;
    }
    if (!dim.Fit.getStrValue().empty()) {
        s += U" " + u32(dim.Fit.getStrValue());
    }
    else {
        if (up != 0.0 || lo != 0.0) {
            if (std::fabs(up + lo) < 1e-12) {
                s += U" ";
                s += PlusMinus;
                s += Callout::number(up);
            }
            else {
                s += U" " + std::u32string(up >= 0 ? U"+" : U"") + Callout::number(up) + U"/"
                    + std::u32string(lo >= 0 ? U"+" : U"") + Callout::number(lo);
            }
        }
    }
    for (auto& m : dim.Modifiers.getValues()) {
        s += U" ";
        s += isCircled(m) ? std::u32string(1, circledCode(m[0])) : u32(m);
    }
    return s;
}
}  // namespace

double Dimension::label(Drawing& d, double h, double x0, double y0) const
{
    std::u32string s = dimensionText(*this);
    // Drawn with its modifiers' circles: the text up to them, then each
    double x = x0;
    std::u32string plain;
    for (char32_t c : s) {
        if (c >= 0x24B6 && c <= 0x24CF) {
            x += d.text(plain, h, x, y0);
            plain.clear();
            x += d.circled(char32_t('A' + (c - 0x24B6)), h, x, y0 + h / 2);
        }
        else {
            plain += c;
        }
    }
    x += d.text(plain, h, x, y0);
    if (Basic.getValue()) {
        // A basic (theoretically exact) dimension is boxed
        d.rect(x0 - 0.3 * h, y0 - 0.5 * h, x - x0 + 0.6 * h, 2 * h);
    }
    return x - x0;
}

void Dimension::layout(Drawing& d, double h) const
{
    _linear = false;
    gp_Vec dir;
    gp_Pnt p1, p2;
    TopoDS_Shape s1, s2;
    gp_Trsf toPlane;
    Part::TopoShape::convertTogpTrsf(Plane.getValue().toMatrix(), toPlane);
    gp_Vec px = gp_Vec(1, 0, 0).Transformed(toPlane);
    toPlane.Invert();
    // A width or a distance whose plane runs along what it measures: the
    // dimension line along the plane's x axis, through its origin, between
    // extension lines from the two sides (ISO 129-1)
    if (!linear(dir, p1, p2, s1, s2) || std::fabs(px.Dot(dir)) < 1 - 1e-6) {
        label(d, h, 0, 0);
        return;
    }
    _linear = true;
    const double gap = 0.3 * h, beyond = 0.6 * h;
    double u[2];
    int i = 0;
    for (auto [p, s] : {std::pair {p1, &s1}, std::pair {p2, &s2}}) {
        u[i] = p.Transformed(toPlane).X();
        double lo = 1e100, hi = -1e100;
        for (TopExp_Explorer xp(*s, TopAbs_VERTEX); xp.More(); xp.Next()) {
            double y = BRep_Tool::Pnt(TopoDS::Vertex(xp.Current())).Transformed(toPlane).Y();
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
        // From the side to a little past the dimension line; none where the
        // line crosses the side
        if (hi < -gap) {
            d.line(u[i], hi + gap, u[i], beyond);
        }
        else if (lo > gap) {
            d.line(u[i], lo - gap, u[i], -beyond);
        }
        ++i;
    }
    double a = std::min(u[0], u[1]), b = std::max(u[0], u[1]);
    Drawing scratch;
    double w = label(scratch, h, 0, 0);
    if (b - a >= 2.5 * h) {
        d.line(a, 0, b, 0);
        d.arrow(a, 0, -1, 0, h);
        d.arrow(b, 0, 1, 0, h);
    }
    else {
        // Too narrow for the arrows: they point in from outside
        d.line(a - 2 * h, 0, b + 2 * h, 0);
        d.arrow(a, 0, 1, 0, h);
        d.arrow(b, 0, -1, 0, h);
    }
    // Its value turns to read upright; its lines stay
    d.beginUpright();
    label(d, h, (a + b) / 2 - w / 2, 0.5 * h);
    d.endUpright();
}

std::u32string Dimension::equivalentText() const
{
    std::u32string s = dimensionText(*this);
    if (Basic.getValue()) {
        return U"[" + s + U"]";
    }
    return s;
}

// ---- Note: its lines ----

void Note::layout(Drawing& d, double h) const
{
    const auto& lines = Text.getValues();
    double y = 0.0;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        d.text(Font::fromUtf8(*it), h, 0, y);
        y += 1.6 * h;
    }
    if (Flag.getValue() > 0) {
        // A flag note: its number in a triangle before the text
        double s = 2 * h;
        d.polyline({{-s - 0.5 * h, -0.3 * h}, {-0.5 * h, -0.3 * h}, {-0.5 * h - s / 2, s * 0.8}},
                   true);
        std::u32string n = Callout::number(Flag.getValue(), 0);
        double w = d.textWidth(n, 0.8 * h);
        d.text(n, 0.8 * h, -0.5 * h - s / 2 - w / 2, 0);
    }
}

std::u32string Note::equivalentText() const
{
    std::u32string s;
    for (auto& line : Text.getValues()) {
        if (!s.empty()) {
            s += U"\n";
        }
        s += Font::fromUtf8(line);
    }
    return s;
}
