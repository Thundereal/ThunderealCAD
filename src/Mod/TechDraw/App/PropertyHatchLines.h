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

#ifndef TECHDRAW_PROPERTYHATCHLINES_H
#define TECHDRAW_PROPERTYHATCHLINES_H

#include <string>
#include <vector>

#include <App/Property.h>
#include <Mod/TechDraw/TechDrawGlobal.h>


namespace TechDraw
{
class LineSet;

/*! The hatch lines of a PAT pattern, trimmed to the faces they fill: what a
 *  boolean of the pattern's lines with each face made, kept so that drawing
 *  the hatch -- every redraw of the view, every page sent to a renderer --
 *  does not run that boolean again (docs/TechDrawStoredGeometry.md sec 7).
 *
 *  The lines are straight, so a segment is its two end points. Each face
 *  keeps the box of the outline it was trimmed to, and the whole the
 *  parameters it was trimmed with: lines read back for a face whose outline
 *  or whose pattern has changed since are not the face's, and the caller
 *  trims again instead.
 */
class TechDrawExport PropertyHatchLines: public App::Property
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    //! the trimmed lines of one line of the pattern
    struct Lines
    {
        double box[4] {0.0, 0.0, 0.0, 0.0};   //!< min x, min y, max x, max y
        std::vector<double> coords;            //!< x0 y0 x1 y1 per segment
    };
    //! one face, its lines in the pattern's order
    struct Face
    {
        int face {-1};
        double outline[4] {0.0, 0.0, 0.0, 0.0};
        std::vector<Lines> lines;
    };

    PropertyHatchLines();
    ~PropertyHatchLines() override;

    void setValue(const std::string& signature, std::vector<Face> faces);
    void clear();
    bool isEmpty() const
    {
        return m_faces.empty();
    }
    const std::string& getSignature() const
    {
        return m_signature;
    }
    //! the face trimmed with `signature` to an outline with box `outline`,
    //! or null
    const Face* find(int face, const std::string& signature, const double outline[4]) const;

    //! what one face's trimming made, ready to store
    static Face capture(int face, const double outline[4], const std::vector<LineSet>& trimmed);
    //! the line sets `face` was stored as, each with its pattern line from
    //! `patternLines` (the pattern read the way it was when trimmed)
    static std::vector<LineSet> restore(const Face& face, const std::vector<LineSet>& patternLines);

    bool isSame(const App::Property& other) const override;
    App::Property* Copy() const override;
    void Paste(const App::Property& from) override;
    unsigned int getMemSize() const override;

    void Save(Base::Writer& writer) const override;
    void Restore(Base::XMLReader& reader) override;
    void SaveDocFile(Base::Writer& writer) const override;
    void RestoreDocFile(Base::Reader& reader) override;

    PyObject* getPyObject() override;
    void setPyObject(PyObject* value) override;

    const char* getEditorName() const override
    {
        return "";
    }

private:
    std::string m_signature;
    std::vector<Face> m_faces;
};

}  // namespace TechDraw

#endif  // TECHDRAW_PROPERTYHATCHLINES_H
