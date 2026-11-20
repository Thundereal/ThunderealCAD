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

#ifndef PMI_GEOMETRY_H
#define PMI_GEOMETRY_H

#include <string>
#include <vector>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <TopoDS_Shape.hxx>

#include <Mod/PMI/PMIGlobal.h>

namespace PMI
{

/** What a toleranced feature is, geometrically: the surface of a face, or
 * the exact element derived from faces (an axis, a centre plane, a point).
 * The measurements of a dimension and the simulated datum are made from
 * these, never from the faces' boundaries (docs/GDandT.md sec 4.2, 4.4).
 */
struct PMIExport Primitive
{
    enum Type
    {
        None,
        Plane,     ///< loc on the plane, dir its normal (outward for a face)
        Cylinder,  ///< loc on the axis, dir the axis, radius
        Sphere,    ///< loc the centre, radius
        Cone,      ///< loc the apex, dir the axis, angle the half angle
        Line,      ///< loc on it, dir along it
        Point,     ///< loc
        Torus,     ///< loc the centre, dir the axis, radius the tube's, major the ring's
    };
    Type type = None;
    gp_Pnt loc;
    gp_Dir dir;
    double radius = 0.0;
    double angle = 0.0;
    double major = 0.0;

    bool isAxial() const
    {
        return type == Cylinder || type == Cone || type == Line;
    }
    /// The element a location is measured to: the axis of a cylinder or a
    /// cone, the centre of a sphere, else itself
    Primitive reference() const;
    const char* typeName() const;
};

namespace Geom
{
/// The surface of a face; None for one that is not a plane, a cylinder, a
/// sphere or a cone
PMIExport Primitive ofFace(const TopoDS_Shape& face);

/// Whether two face primitives are the same surface, within tol (lengths)
/// and atol (radians); a plane's normal may be reversed
PMIExport bool sameSurface(const Primitive& a, const Primitive& b, double tol = 1e-6,
                           double atol = 1e-9);

/// The distance between two reference primitives (planes parallel, axes
/// parallel, a point and anything); false if they have no single distance
PMIExport bool distance(const Primitive& a, const Primitive& b, double& result, std::string& why);

/// The angle in radians between two primitives with a direction (planes by
/// their normals, axes and lines by their directions), in [0, pi/2]
PMIExport bool angle(const Primitive& a, const Primitive& b, double& result, std::string& why);

/// A width (ASME Y14.5 1.3.32): planes in pairs facing opposite ways, each
/// pair \a width apart -- one, or a pattern of them ("2X"), slots or tabs
/// alike -- and a slot's round ends, half cylinders of half the width
PMIExport bool width(const std::vector<Primitive>& faces, double& width);

/// Holes: cylinders grouped by axis, every axis with the same radii (a
/// counterbore's hole and its bore), with the planes across the axes (the
/// bores' floors) and the cones on them (a countersink, a drill point) --
/// a hole, or a pattern of them. \a hole the first of the smallest radius
PMIExport bool holes(const std::vector<Primitive>& faces, std::size_t& hole);

/// An edge along the line (loc, dir) spanning \a extent's bounding box;
/// null if the extent is empty
PMIExport TopoDS_Shape axisEdge(const gp_Pnt& loc, const gp_Dir& dir, const TopoDS_Shape& extent);

/// Faces of a shape, in order
PMIExport std::vector<TopoDS_Shape> faces(const TopoDS_Shape& shape);

/// A compound of the shapes, or the shape alone
PMIExport TopoDS_Shape compound(const std::vector<TopoDS_Shape>& shapes);
}  // namespace Geom

}  // namespace PMI

#endif  // PMI_GEOMETRY_H
