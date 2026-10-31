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
        elements.append(d);
    }
    Py::Dict res;
    res.setItem("Thickness", Py::Float(model.thickness));
    res.setItem("Elements", elements);
    return Py::new_reference_to(res);
}

void PropertySheetModel::setPyObject(PyObject* /*value*/)
{
    THROWM(Base::TypeError, "The sheet metal model is made by its feature, and read only");
}

void PropertySheetModel::Save(Base::Writer& writer) const
{
    writer.Stream() << writer.ind() << "<SheetModel thickness=\"" << std::setprecision(17)
                    << model.thickness << "\" count=\"" << model.elements.size() << "\">\n";
    writer.incInd();
    for (const Element& e : model.elements) {
        writer.Stream() << writer.ind();
        if (e.kind == Element::Kind::Wall) {
            writer.Stream() << "<Wall name=\"" << encodeAttribute(e.name) << "\" frame=\""
                            << frameText(e.frame) << "\"/>\n";
        }
        else {
            writer.Stream() << "<Bend name=\"" << encodeAttribute(e.name) << "\" frame=\""
                            << frameText(e.bend.frame) << "\" radius=\"" << e.bend.radius
                            << "\" thickness=\"" << e.bend.thickness << "\" angle=\""
                            << e.bend.angle << "\" k=\"" << e.bend.k << "\" before=\"" << e.before
                            << "\" after=\"" << e.after << "\"/>\n";
        }
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
    for (long i = 0; i < count; ++i) {
        reader.readElement();
        const char* name = reader.getAttribute("name");
        if (reader.localName() == std::string("Wall")) {
            value.addWall(name, frameOf(reader.getAttribute("frame")));
            continue;
        }
        SheetMetal::CylBend bend;
        bend.frame = frameOf(reader.getAttribute("frame"));
        bend.radius = reader.getAttributeAsFloat("radius");
        bend.thickness = reader.getAttributeAsFloat("thickness");
        bend.angle = reader.getAttributeAsFloat("angle");
        bend.k = reader.getAttributeAsFloat("k");
        value.addBend(name,
                      bend,
                      static_cast<int>(reader.getAttributeAsInteger("before")),
                      static_cast<int>(reader.getAttributeAsInteger("after")));
    }
    reader.readEndElement("SheetModel");
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
            || a.bend.angle != b.bend.angle || a.bend.k != b.bend.k) {
            return false;
        }
    }
    return true;
}
