/***************************************************************************
 *   Copyright (c) 2026 The FreeCAD project                                *
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

#ifndef _PreComp_
#include <cmath>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <Bnd_Box.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Edge.hxx>
#endif

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/Reader.h>
#include <Base/Stream.h>
#include <Base/Writer.h>

#include "PropertyHatchLines.h"
#include "Geometry.h"
#include "HatchLine.h"


using namespace TechDraw;

TYPESYSTEM_SOURCE(TechDraw::PropertyHatchLines, App::Property)

//! the schema this build writes.  Lines written by a later one are not read
//! back; the hatch is trimmed again, as it was before any were stored.
static const int currentFormat = 1;

namespace
{
//! the same box, as near as a box worked out twice from the same outline is
bool sameBox(const double a[4], const double b[4])
{
    for (int i = 0; i < 4; i++) {
        if (std::fabs(a[i] - b[i]) > 1e-7 * (1.0 + std::fabs(a[i]))) {
            return false;
        }
    }
    return true;
}
}  // namespace

PropertyHatchLines::PropertyHatchLines() = default;

PropertyHatchLines::~PropertyHatchLines() = default;

void PropertyHatchLines::setValue(const std::string& signature, std::vector<Face> faces)
{
    aboutToSetValue();
    m_signature = signature;
    m_faces = std::move(faces);
    hasSetValue();
}

void PropertyHatchLines::clear()
{
    if (isEmpty() && m_signature.empty()) {
        return;
    }
    setValue(std::string(), {});
}

const PropertyHatchLines::Face*
PropertyHatchLines::find(int face, const std::string& signature, const double outline[4]) const
{
    if (signature != m_signature) {
        return nullptr;
    }
    for (const auto& entry : m_faces) {
        if (entry.face == face) {
            return sameBox(entry.outline, outline) ? &entry : nullptr;
        }
    }
    return nullptr;
}

PropertyHatchLines::Face
PropertyHatchLines::capture(int face, const double outline[4], const std::vector<LineSet>& trimmed)
{
    Face result;
    result.face = face;
    for (int i = 0; i < 4; i++) {
        result.outline[i] = outline[i];
    }
    for (const auto& constSet : trimmed) {
        auto& set = const_cast<LineSet&>(constSet);   // its getters are not const
        Lines lines;
        Bnd_Box box = set.getBBox();
        if (!box.IsVoid()) {
            double zmin, zmax;
            box.Get(lines.box[0], lines.box[1], zmin, lines.box[2], lines.box[3], zmax);
        }
        for (const auto& geom : set.getGeoms()) {
            // the ends the drawing reads, which is where a dash starts
            // (PATPathMaker); an edge made from them reads them back
            Base::Vector3d start = geom->getStartPoint();
            Base::Vector3d end = geom->getEndPoint();
            lines.coords.insert(lines.coords.end(), {start.x, start.y, end.x, end.y});
        }
        result.lines.push_back(std::move(lines));
    }
    return result;
}

std::vector<LineSet> PropertyHatchLines::restore(const Face& face,
                                                 const std::vector<LineSet>& patternLines)
{
    std::vector<LineSet> result;
    if (face.lines.size() != patternLines.size()) {
        return result;
    }
    for (size_t i = 0; i < face.lines.size(); i++) {
        const Lines& lines = face.lines[i];
        LineSet set = patternLines[i];
        Bnd_Box box;
        box.SetGap(0.0);
        if (lines.box[0] <= lines.box[2]) {
            box.Update(lines.box[0], lines.box[1], 0.0, lines.box[2], lines.box[3], 0.0);
        }
        set.setBBox(box);
        std::vector<TopoDS_Edge> edges;
        std::vector<BaseGeomPtr> geoms;
        edges.reserve(lines.coords.size() / 4);
        geoms.reserve(lines.coords.size() / 4);
        for (size_t j = 0; j + 3 < lines.coords.size(); j += 4) {
            gp_Pnt start(lines.coords[j], lines.coords[j + 1], 0.0);
            gp_Pnt end(lines.coords[j + 2], lines.coords[j + 3], 0.0);
            if (start.Distance(end) <= 0.0) {
                continue;
            }
            TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(start, end);
            BaseGeomPtr geom = BaseGeom::baseFactory(edge);
            if (!geom) {
                continue;
            }
            edges.push_back(edge);
            geoms.push_back(geom);
        }
        set.setEdges(edges);
        set.setGeoms(geoms);
        result.push_back(set);
    }
    return result;
}

bool PropertyHatchLines::isSame(const App::Property& other) const
{
    if (&other == this) {
        return true;
    }
    const auto* twin = dynamic_cast<const PropertyHatchLines*>(&other);
    if (!twin || twin->m_signature != m_signature || twin->m_faces.size() != m_faces.size()) {
        return false;
    }
    for (size_t i = 0; i < m_faces.size(); i++) {
        const Face& a = m_faces[i];
        const Face& b = twin->m_faces[i];
        if (a.face != b.face || !sameBox(a.outline, b.outline) || a.lines.size() != b.lines.size()) {
            return false;
        }
        for (size_t j = 0; j < a.lines.size(); j++) {
            if (a.lines[j].coords != b.lines[j].coords) {
                return false;
            }
        }
    }
    return true;
}

App::Property* PropertyHatchLines::Copy() const
{
    auto* copy = new PropertyHatchLines();
    copy->m_signature = m_signature;
    copy->m_faces = m_faces;
    return copy;
}

void PropertyHatchLines::Paste(const App::Property& from)
{
    const auto& other = dynamic_cast<const PropertyHatchLines&>(from);
    setValue(other.m_signature, other.m_faces);
}

unsigned int PropertyHatchLines::getMemSize() const
{
    auto size = static_cast<unsigned int>(sizeof(*this) + m_signature.size());
    for (const auto& face : m_faces) {
        size += static_cast<unsigned int>(sizeof(face));
        for (const auto& lines : face.lines) {
            size += static_cast<unsigned int>(sizeof(lines) + lines.coords.size() * sizeof(double));
        }
    }
    return size;
}

void PropertyHatchLines::Save(Base::Writer& writer) const
{
    writer.Stream() << writer.ind() << "<HatchLines format=\"" << currentFormat
                    << "\" signature=\"" << encodeAttribute(m_signature) << "\" faces=\""
                    << m_faces.size() << "\"";
    if (isEmpty()) {
        writer.Stream() << "/>\n";
        return;
    }
    writer.Stream() << " file=\"" << writer.addFile(getFileName(".bin"), this) << "\">\n";
    writer.incInd();
    for (const auto& face : m_faces) {
        writer.Stream() << writer.ind() << "<Face i=\"" << face.face << "\" x0=\""
                        << face.outline[0] << "\" y0=\"" << face.outline[1] << "\" x1=\""
                        << face.outline[2] << "\" y1=\"" << face.outline[3] << "\" lines=\""
                        << face.lines.size() << "\">\n";
        writer.incInd();
        for (const auto& lines : face.lines) {
            writer.Stream() << writer.ind() << "<Lines x0=\"" << lines.box[0] << "\" y0=\""
                            << lines.box[1] << "\" x1=\"" << lines.box[2] << "\" y1=\""
                            << lines.box[3] << "\" segments=\"" << lines.coords.size() / 4
                            << "\"/>\n";
        }
        writer.decInd();
        writer.Stream() << writer.ind() << "</Face>\n";
    }
    writer.decInd();
    writer.Stream() << writer.ind() << "</HatchLines>\n";
}

void PropertyHatchLines::Restore(Base::XMLReader& reader)
{
    reader.readElement("HatchLines");
    const int format = reader.getAttributeAsInteger("format");
    const std::string signature = reader.getAttribute("signature");
    const int faceCount = reader.getAttributeAsInteger("faces");
    const bool hasFile = reader.hasAttribute("file");
    const std::string file = hasFile ? std::string(reader.getAttribute("file")) : std::string();

    aboutToSetValue();
    m_signature.clear();
    m_faces.clear();
    if (!hasFile) {
        hasSetValue();
        return;
    }
    if (format > currentFormat) {
        // a newer schema than this build reads: nothing is kept, and the
        // hatch is trimmed when drawn
        reader.readEndElement("HatchLines");
        hasSetValue();
        return;
    }

    m_signature = signature;
    m_faces.reserve(faceCount);
    for (int i = 0; i < faceCount; i++) {
        reader.readElement("Face");
        Face face;
        face.face = reader.getAttributeAsInteger("i");
        face.outline[0] = reader.getAttributeAsFloat("x0");
        face.outline[1] = reader.getAttributeAsFloat("y0");
        face.outline[2] = reader.getAttributeAsFloat("x1");
        face.outline[3] = reader.getAttributeAsFloat("y1");
        const int lineCount = reader.getAttributeAsInteger("lines");
        for (int j = 0; j < lineCount; j++) {
            reader.readElement("Lines");
            Lines lines;
            lines.box[0] = reader.getAttributeAsFloat("x0");
            lines.box[1] = reader.getAttributeAsFloat("y0");
            lines.box[2] = reader.getAttributeAsFloat("x1");
            lines.box[3] = reader.getAttributeAsFloat("y1");
            // sized here, filled from the file
            lines.coords.resize(4 * reader.getAttributeAsUnsigned("segments"));
            face.lines.push_back(std::move(lines));
        }
        reader.readEndElement("Face");
        m_faces.push_back(std::move(face));
    }
    reader.readEndElement("HatchLines");
    reader.addFile(file.c_str(), this);
    hasSetValue();
}

void PropertyHatchLines::SaveDocFile(Base::Writer& writer) const
{
    Base::OutputStream str(writer.Stream(), true);
    for (const auto& face : m_faces) {
        for (const auto& lines : face.lines) {
            for (double value : lines.coords) {
                str << value;
            }
        }
    }
}

void PropertyHatchLines::RestoreDocFile(Base::Reader& reader)
{
    Base::InputStream str(reader, true);
    aboutToSetValue();
    for (auto& face : m_faces) {
        for (auto& lines : face.lines) {
            for (double& value : lines.coords) {
                str >> value;
            }
        }
    }
    if (!reader.good() && !reader.eof()) {
        Base::Console().Warning("TechDraw - stored hatch lines do not read back\n");
        m_signature.clear();
        m_faces.clear();
    }
    hasSetValue();
}

PyObject* PropertyHatchLines::getPyObject()
{
    // storage: how many faces it holds lines for, and how many segments
    long segments = 0;
    for (const auto& face : m_faces) {
        for (const auto& lines : face.lines) {
            segments += static_cast<long>(lines.coords.size() / 4);
        }
    }
    return Py_BuildValue("(il)", static_cast<int>(m_faces.size()), segments);
}

void PropertyHatchLines::setPyObject(PyObject* value)
{
    (void)value;
    THROWM(Base::AttributeError, "the stored hatch lines are derived state and are not assignable");
}
