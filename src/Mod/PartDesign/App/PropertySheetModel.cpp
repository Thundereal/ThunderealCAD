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

#include <Base/Exception.h>
#include <Base/Reader.h>
#include <Base/VectorPy.h>
#include <Base/Writer.h>

#include "PropertySheetModel.h"

using namespace PartDesign;
using SheetMetal::Element;

TYPESYSTEM_SOURCE(PartDesign::PropertySheetModel, App::Property)

void PropertySheetModel::setValue(const SheetMetal::Model& value)
{
    aboutToSetValue();
    model = value;
    hasSetValue();
}

namespace
{

constexpr double degree = std::numbers::pi / 180;

Py::Object vector(const gp_XYZ& v)
{
    return Py::asObject(new Base::VectorPy(Base::Vector3d(v.X(), v.Y(), v.Z())));
}

/// A frame as nine numbers: its origin, Z and X
std::string frameText(const gp_Ax3& frame)
{
    std::ostringstream s;
    s << std::setprecision(17);
    for (const gp_XYZ& v : {frame.Location().XYZ(), frame.Direction().XYZ(),
                            frame.XDirection().XYZ()}) {
        s << v.X() << ' ' << v.Y() << ' ' << v.Z() << ' ';
    }
    return s.str();
}

gp_Ax3 frameOf(const char* text)
{
    std::istringstream s(text);
    double v[9];
    for (double& x : v) {
        if (!(s >> x)) {
            FC_THROWM(Base::ValueError, "Sheet metal model: bad frame '" << text << "'");
        }
    }
    return {gp_Pnt(v[0], v[1], v[2]), gp_Dir(v[3], v[4], v[5]), gp_Dir(v[6], v[7], v[8])};
}

/// Limit planes as six numbers each: a point on it and its normal
std::string limitsText(const std::vector<gp_Pln>& limits)
{
    std::ostringstream s;
    s << std::setprecision(17);
    for (const gp_Pln& plane : limits) {
        for (const gp_XYZ& v : {plane.Location().XYZ(), plane.Axis().Direction().XYZ()}) {
            s << v.X() << ' ' << v.Y() << ' ' << v.Z() << ' ';
        }
    }
    return s.str();
}

std::vector<gp_Pln> limitsOf(const char* text)
{
    std::istringstream s(text);
    std::vector<gp_Pln> res;
    double v[6];
    while (s >> v[0]) {
        for (int i = 1; i < 6; ++i) {
            if (!(s >> v[i])) {
                FC_THROWM(Base::ValueError, "Sheet metal model: bad limits '" << text << "'");
            }
        }
        res.emplace_back(gp_Pnt(v[0], v[1], v[2]), gp_Dir(v[3], v[4], v[5]));
    }
    return res;
}

bool sameLimits(const std::vector<gp_Pln>& a, const std::vector<gp_Pln>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!a[i].Location().IsEqual(b[i].Location(), 0)
            || !a[i].Axis().Direction().IsEqual(b[i].Axis().Direction(), 0)) {
            return false;
        }
    }
    return true;
}

bool sameFrame(const gp_Ax3& a, const gp_Ax3& b)
{
    return a.Location().IsEqual(b.Location(), 0) && a.Direction().IsEqual(b.Direction(), 0)
        && a.XDirection().IsEqual(b.XDirection(), 0);
}

}  // namespace

PyObject* PropertySheetModel::getPyObject()
{
    Py::List elements;
    for (const Element& e : model.elements) {
        Py::Dict d;
        d.setItem("Name", Py::String(e.name));
        if (e.kind == Element::Kind::Wall) {
            d.setItem("Kind", Py::String("Wall"));
            d.setItem("Origin", vector(e.frame.Location().XYZ()));
            d.setItem("Normal", vector(e.frame.Direction().XYZ()));
            if (e.crossBreak != 0) {
                d.setItem("CrossBreak", Py::Float(e.crossBreak / degree));
            }
        }
        else {
            d.setItem("Kind", Py::String("Bend"));
            d.setItem("Radius", Py::Float(e.bend.radius));
            d.setItem("Angle", Py::Float(e.bend.angle / degree));
            d.setItem("KFactor", Py::Float(e.bend.k));
            d.setItem("Center", vector(e.bend.frame.Location().XYZ()));
            d.setItem("Axis", vector(e.bend.frame.Direction().XYZ()));
            d.setItem("Before", Py::String(model.elements[e.before].name));
            d.setItem("After", Py::String(model.elements[e.after].name));
        }
        if (!e.limits.empty()) {
            Py::List limits;
            for (const gp_Pln& plane : e.limits) {
                limits.append(Py::TupleN(vector(plane.Location().XYZ()),
                                         vector(plane.Axis().Direction().XYZ())));
            }
            d.setItem("Limits", limits);
        }
        elements.append(d);
    }
    Py::Dict res;
    res.setItem("Thickness", Py::Float(model.thickness));
    res.setItem("Elements", elements);
    // only where there are any, as a cross break is
    if (!model.forms.empty()) {
        Py::List forms;
        for (const SheetMetal::Form& f : model.forms) {
            Py::Dict d;
            d.setItem("Name", Py::String(f.name));
            d.setItem("Wall", Py::String(model.elements[f.wall].name));
            d.setItem("Center", vector(f.frame.Location().XYZ()));
            d.setItem("Normal", vector(f.frame.Direction().XYZ()));
            d.setItem("XDirection", vector(f.frame.XDirection().XYZ()));
            d.setItem("Size",
                      Py::TupleN(Py::Float(f.halfLength),
                                 Py::Float(f.halfWidth),
                                 Py::Float(f.zMin),
                                 Py::Float(f.zMax)));
            forms.append(d);
        }
        res.setItem("Forms", forms);
    }
    return Py::new_reference_to(res);
}

void PropertySheetModel::setPyObject(PyObject* /*value*/)
{
    THROWM(Base::TypeError, "The sheet metal model is made by its feature, and read only");
}

void PropertySheetModel::Save(Base::Writer& writer) const
{
    writer.Stream() << writer.ind() << "<SheetModel thickness=\"" << std::setprecision(17)
                    << model.thickness << "\" count=\"" << model.elements.size() << "\"";
    // only where there are any, so a file without reads as before
    if (!model.forms.empty()) {
        writer.Stream() << " forms=\"" << model.forms.size() << "\"";
    }
    writer.Stream() << ">\n";
    writer.incInd();
    for (const Element& e : model.elements) {
        writer.Stream() << writer.ind();
        if (e.kind == Element::Kind::Wall) {
            writer.Stream() << "<Wall name=\"" << encodeAttribute(e.name) << "\" frame=\""
                            << frameText(e.frame) << "\"";
            if (e.crossBreak != 0) {
                writer.Stream() << " crossBreak=\"" << e.crossBreak << "\"";
            }
        }
        else {
            writer.Stream() << "<Bend name=\"" << encodeAttribute(e.name) << "\" frame=\""
                            << frameText(e.bend.frame) << "\" radius=\"" << e.bend.radius
                            << "\" thickness=\"" << e.bend.thickness << "\" angle=\""
                            << e.bend.angle << "\" k=\"" << e.bend.k << "\" before=\"" << e.before
                            << "\" after=\"" << e.after << "\"";
        }
        // only where there are any, so a file without miters reads as before
        if (!e.limits.empty()) {
            writer.Stream() << " limits=\"" << limitsText(e.limits) << "\"";
        }
        writer.Stream() << "/>\n";
    }
    for (const SheetMetal::Form& f : model.forms) {
        writer.Stream() << writer.ind() << "<Form name=\"" << encodeAttribute(f.name)
                        << "\" wall=\"" << f.wall << "\" frame=\"" << frameText(f.frame)
                        << "\" size=\"" << f.halfLength << " " << f.halfWidth << " " << f.zMin
                        << " " << f.zMax << "\"/>\n";
    }
    writer.decInd();
    writer.Stream() << writer.ind() << "</SheetModel>\n";
}

void PropertySheetModel::Restore(Base::XMLReader& reader)
{
    reader.readElement("SheetModel");
    SheetMetal::Model value;
    value.thickness = reader.getAttributeAsFloat("thickness");
    const long count = reader.getAttributeAsInteger("count");
    const long formCount = reader.hasAttribute("forms") ? reader.getAttributeAsInteger("forms") : 0;
    for (long i = 0; i < count; ++i) {
        reader.readElement();
        const char* name = reader.getAttribute("name");
        const std::vector<gp_Pln> limits =
            reader.hasAttribute("limits") ? limitsOf(reader.getAttribute("limits"))
                                          : std::vector<gp_Pln>();
        if (reader.localName() == std::string("Wall")) {
            const int index = value.addWall(name, frameOf(reader.getAttribute("frame")));
            Element& wall = value.elements[index];
            wall.limits = limits;
            if (reader.hasAttribute("crossBreak")) {
                wall.crossBreak = reader.getAttributeAsFloat("crossBreak");
            }
            continue;
        }
        // A bend a fold or a jog carries starts from that feature's wall,
        // made after it: its walls are checked once all are read
        Element element;
        element.kind = Element::Kind::Bend;
        element.name = name;
        element.bend.frame = frameOf(reader.getAttribute("frame"));
        element.bend.radius = reader.getAttributeAsFloat("radius");
        element.bend.thickness = reader.getAttributeAsFloat("thickness");
        element.bend.angle = reader.getAttributeAsFloat("angle");
        element.bend.k = reader.getAttributeAsFloat("k");
        element.before = static_cast<int>(reader.getAttributeAsInteger("before"));
        element.after = static_cast<int>(reader.getAttributeAsInteger("after"));
        element.limits = limits;
        value.elements.push_back(element);
    }
    for (long i = 0; i < formCount; ++i) {
        reader.readElement("Form");
        SheetMetal::Form form;
        form.name = reader.getAttribute("name");
        form.wall = static_cast<int>(reader.getAttributeAsInteger("wall"));
        form.frame = frameOf(reader.getAttribute("frame"));
        std::istringstream size(reader.getAttribute("size"));
        size >> form.halfLength >> form.halfWidth >> form.zMin >> form.zMax;
        // a wall's, or a gusset's bend's
        if (form.wall < 0 || form.wall >= static_cast<int>(value.elements.size())) {
            FC_THROWM(Base::IndexError,
                      "Sheet metal: " << form.name << " is on element " << form.wall
                                      << ", which is no wall or bend");
        }
        value.forms.push_back(form);
    }
    reader.readEndElement("SheetModel");
    for (const Element& e : value.elements) {
        if (e.kind != Element::Kind::Bend) {
            continue;
        }
        for (int index : {e.before, e.after}) {
            if (index < 0 || index >= static_cast<int>(value.elements.size())
                || value.elements[index].kind != Element::Kind::Wall) {
                FC_THROWM(Base::IndexError,
                          "Sheet metal: " << e.name << " joins element " << index
                                          << ", which is no wall");
            }
        }
    }
    setValue(value);
}

App::Property* PropertySheetModel::Copy() const
{
    auto p = new PropertySheetModel;
    p->model = model;
    return p;
}

void PropertySheetModel::Paste(const Property& from)
{
    setValue(dynamic_cast<const PropertySheetModel&>(from).model);
}

unsigned int PropertySheetModel::getMemSize() const
{
    return static_cast<unsigned int>(sizeof(model) + model.elements.size() * sizeof(Element));
}

bool PropertySheetModel::isSame(const Property& other) const
{
    if (&other == this) {
        return true;
    }
    auto p = dynamic_cast<const PropertySheetModel*>(&other);
    if (!p || p->model.thickness != model.thickness
        || p->model.elements.size() != model.elements.size()) {
        return false;
    }
    for (std::size_t i = 0; i < model.elements.size(); ++i) {
        const Element &a = model.elements[i], &b = p->model.elements[i];
        if (a.kind != b.kind || a.name != b.name || a.before != b.before || a.after != b.after
            || !sameFrame(a.frame, b.frame) || !sameFrame(a.bend.frame, b.bend.frame)
            || a.bend.radius != b.bend.radius || a.bend.thickness != b.bend.thickness
            || a.bend.angle != b.bend.angle || a.bend.k != b.bend.k
            || !sameLimits(a.limits, b.limits) || a.crossBreak != b.crossBreak) {
            return false;
        }
    }
    if (p->model.forms.size() != model.forms.size()) {
        return false;
    }
    for (std::size_t i = 0; i < model.forms.size(); ++i) {
        const SheetMetal::Form &a = model.forms[i], &b = p->model.forms[i];
        if (a.name != b.name || a.wall != b.wall || !sameFrame(a.frame, b.frame)
            || a.halfLength != b.halfLength || a.halfWidth != b.halfWidth || a.zMin != b.zMin
            || a.zMax != b.zMax) {
            return false;
        }
    }
    return true;
}
