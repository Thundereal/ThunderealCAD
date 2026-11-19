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
#include <cctype>
#include <set>

#include <BRepBuilderAPI_MakeVertex.hxx>

#include "AnnotationSet.h"
#include "Datum.h"
#include "TolerancedFeature.h"

using namespace PMI;

PROPERTY_SOURCE(PMI::Datum, PMI::Annotation)

Datum::Datum()
{
    ADD_PROPERTY_TYPE(Letter, ("A"), "PMI", App::Prop_None, "The datum's letter");
    ADD_PROPERTY_TYPE(Feature, (nullptr), "PMI", App::Prop_None, "The datum feature");
    ADD_PROPERTY_TYPE(Shape, (TopoDS_Shape()), "PMI",
                      App::PropertyType(App::Prop_ReadOnly | App::Prop_Transient),
                      "The simulated datum: a plane, an axis or a point");
}

TolerancedFeature* Datum::getFeature() const
{
    return freecad_cast<TolerancedFeature*>(Feature.getValue());
}

bool Datum::isLegalLetter(const std::string& letter, bool asme, std::string& why)
{
    if (letter.empty()) {
        why = "a datum needs a letter";
        return false;
    }
    for (char c : letter) {
        if (c < 'A' || c > 'Z') {
            why = "a datum letter is upper case A to Z: '" + letter + "'";
            return false;
        }
        if (asme && (c == 'I' || c == 'O' || c == 'Q')) {
            why = std::string("ASME Y14.5 does not use the letter ") + c;
            return false;
        }
    }
    return true;
}

App::DocumentObjectExecReturn* Datum::execute()
{
    auto feat = getFeature();
    if (!feat) {
        Shape.setValue(TopoDS_Shape());
        setState(Broken, "no datum feature");
        return App::DocumentObject::StdReturn;
    }
    const Primitive& p = feat->primitive();
    TopoDS_Shape shape;
    switch (p.type) {
        case Primitive::Plane:
            // The plane a datum feature simulator touches: the feature's
            // own (or derived) planar shape stands for it
            shape = feat->Shape.getShape().getShape();
            break;
        case Primitive::Cylinder:
        case Primitive::Cone:
        case Primitive::Line: {
            const TopoDS_Shape fs = feat->Shape.getShape().getShape();
            if (feat->getKind() == TolerancedFeature::Axis) {
                shape = fs;
            }
            else {
                // The axis, as long as the feature
                Primitive axis = p.reference();
                shape = Geom::axisEdge(axis.loc, axis.dir, fs);
            }
            break;
        }
        case Primitive::Sphere:
        case Primitive::Point:
            shape = BRepBuilderAPI_MakeVertex(p.loc).Shape();
            break;
        default:
            break;
    }
    Shape.setValue(shape);

    std::string why;
    auto set = getSet();
    if (!isLegalLetter(Letter.getStrValue(), set && set->isASME(), why)) {
        setState(Invalid, why);
    }
    else if (feat->getState() == Broken) {
        setState(Broken, "its feature is broken: " + feat->StateMessage.getStrValue());
    }
    else if (p.type == Primitive::None) {
        setState(Invalid, "a datum feature must be a plane, a cylinder, a cone or a sphere");
    }
    else {
        setState(Ok);
    }
    return App::DocumentObject::StdReturn;
}

PROPERTY_SOURCE(PMI::DatumReferenceFrame, PMI::Annotation)

DatumReferenceFrame::DatumReferenceFrame()
{
    ADD_PROPERTY_TYPE(Compartments, (), "PMI", App::Prop_None,
                      "Up to three, in order: a datum or a common datum, with modifiers: "
                      "\"A\", \"B(M)\", \"C-D\"");
}

const std::vector<std::string>& DatumReferenceFrame::modifierNames()
{
    // MMR, LMR; free state; basic; projected; translation; point, line, plane
    // (ISO 5459 [PT] [SL] [PL]); any cross section, any longitudinal
    // section; contacting feature; distance variable; diameters
    static const std::vector<std::string> names = {"M",  "L",   "F",  "BSC", "P",  "><", "PT", "SL",
                                                   "PL", "ACS", "ALS", "CF", "DV", "LD", "MD", "PD"};
    return names;
}

namespace
{
std::string trim(const std::string& s)
{
    auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return {};
    }
    auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> res;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            res.push_back(trim(cur));
            cur.clear();
        }
        else {
            cur += c;
        }
    }
    res.push_back(trim(cur));
    return res;
}
}  // namespace

bool DatumReferenceFrame::parse(std::vector<Compartment>& result, std::string& why) const
{
    result.clear();
    const auto& values = Compartments.getValues();
    if (values.empty()) {
        why = "a datum reference frame needs a primary datum";
        return false;
    }
    if (values.size() > 3) {
        why = "a datum reference frame has at most three compartments";
        return false;
    }
    std::set<std::string> seen;
    for (auto& raw : values) {
        std::string text = trim(raw);
        Compartment comp;
        std::string body = text;
        auto open = text.find('(');
        if (open != std::string::npos) {
            if (text.back() != ')') {
                why = "unclosed modifiers in '" + raw + "'";
                return false;
            }
            body = trim(text.substr(0, open));
            for (auto& m : split(text.substr(open + 1, text.size() - open - 2), ',')) {
                const auto& names = modifierNames();
                if (std::find(names.begin(), names.end(), m) == names.end()) {
                    why = "unknown datum modifier '" + m + "' in '" + raw + "'";
                    return false;
                }
                comp.modifiers.push_back(m);
            }
        }
        for (auto& d : split(body, '-')) {
            std::string dummy;
            if (!Datum::isLegalLetter(d, false, dummy)) {
                why = "'" + raw + "' is not a datum or a common datum (A, A-B)";
                return false;
            }
            if (!seen.insert(d).second) {
                why = "datum " + d + " is in the frame twice";
                return false;
            }
            comp.datums.push_back(d);
        }
        result.push_back(std::move(comp));
    }
    return true;
}

App::DocumentObjectExecReturn* DatumReferenceFrame::execute()
{
    std::vector<Compartment> comps;
    std::string why;
    if (!parse(comps, why)) {
        setState(Invalid, why);
    }
    else {
        setState(Ok);
    }
    return App::DocumentObject::StdReturn;
}
