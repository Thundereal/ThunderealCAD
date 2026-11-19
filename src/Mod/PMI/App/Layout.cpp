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
    s += Callout::number(dim.Nominal.getValue(), dec);
    if (dim.isAngular()) {
        s += Degree;
    }
    if (dim.Basic.getValue()) {
        return s;
    }
    if (!dim.Fit.getStrValue().empty()) {
        s += U" " + u32(dim.Fit.getStrValue());
    }
    else {
        double up = dim.Upper.getValue(), lo = dim.Lower.getValue();
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

void Dimension::layout(Drawing& d, double h) const
{
    std::u32string s = dimensionText(*this);
    // Drawn with its modifiers' circles: the text up to them, then each
    double x = 0.0;
    std::u32string plain;
    for (char32_t c : s) {
        if (c >= 0x24B6 && c <= 0x24CF) {
            x += d.text(plain, h, x, 0);
            plain.clear();
            x += d.circled(char32_t('A' + (c - 0x24B6)), h, x, h / 2);
        }
        else {
            plain += c;
        }
    }
    x += d.text(plain, h, x, 0);
    if (Basic.getValue()) {
        // A basic (theoretically exact) dimension is boxed
        d.rect(-0.3 * h, -0.5 * h, x + 0.6 * h, 2 * h);
    }
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
