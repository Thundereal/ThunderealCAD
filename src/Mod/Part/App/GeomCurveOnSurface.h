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

#pragma once

// A curve that lies on a surface: a 2D curve in the surface's (u, v), so it is on the
// surface exactly rather than within a tolerance (docs/Sketch3D.md, decision f). The 3D
// sketch's curves on a face are these; as a Part geometry it is a curve like the others.

#include <Geom2d_Curve.hxx>
#include <Geom_BoundedCurve.hxx>
#include <Geom_Surface.hxx>

#include "Geometry.h"

namespace Part
{

/** The OCCT side: a Geom_BoundedCurve evaluated as S(c(t)), the surface S along the 2D
 * curve c, derivatives by the chain rule (to the third). So everything that works on a
 * Geom_Curve through its evaluators -- projection, adaptors, approximation -- works on it.
 * It is not a type BRep can store: GeomCurveOnSurface::toShape makes the edge from the
 * 2D curve on the surface instead.
 */
class PartExport Geom_CurveOnSurface: public Geom_BoundedCurve
{
public:
    Geom_CurveOnSurface(const Handle(Geom2d_Curve) & curve2d, const Handle(Geom_Surface) & surface);

    const Handle(Geom2d_Curve) & Curve2d() const
    {
        return myCurve2d;
    }
    const Handle(Geom_Surface) & Surface() const
    {
        return mySurface;
    }
    void SetCurve2d(const Handle(Geom2d_Curve) & curve2d);
    void SetSurface(const Handle(Geom_Surface) & surface);

    void Reverse() override;
    double ReversedParameter(const double U) const override;
    double FirstParameter() const override;
    double LastParameter() const override;
    bool IsClosed() const override;
    bool IsPeriodic() const override;
    double Period() const override;
    GeomAbs_Shape Continuity() const override;
    bool IsCN(const int N) const override;
    gp_Pnt EvalD0(const double U) const override;
    ResD1 EvalD1(const double U) const override;
    ResD2 EvalD2(const double U) const override;
    ResD3 EvalD3(const double U) const override;
    gp_Vec EvalDN(const double U, const int N) const override;
    gp_Pnt StartPoint() const override;
    gp_Pnt EndPoint() const override;
    /// Transforms the surface; when that changes the surface's parametrisation (a
    /// plane scaled), the 2D curve follows it.
    void Transform(const gp_Trsf& T) override;
    Handle(Geom_Geometry) Copy() const override;

    DEFINE_STANDARD_RTTIEXT(Geom_CurveOnSurface, Geom_BoundedCurve)

private:
    Handle(Geom2d_Curve) myCurve2d;
    Handle(Geom_Surface) mySurface;
};

/** A curve on a surface as a Part geometry: Part.CurveOnSurface(curve2d, surface). */
class PartExport GeomCurveOnSurface: public GeomBoundedCurve
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    /// A unit segment along u on the XY plane, until it is given a curve.
    GeomCurveOnSurface();
    GeomCurveOnSurface(const Handle(Geom2d_Curve) & curve2d, const Handle(Geom_Surface) & surface);
    explicit GeomCurveOnSurface(const Handle(Geom_CurveOnSurface) & curve);
    ~GeomCurveOnSurface() override;
    Geometry* copy() const override;

    /// An edge made from the 2D curve on the surface, carrying that curve on it, with a
    /// 3D curve built to match.
    TopoDS_Shape toShape() const override;
    TopoDS_Shape toShape(double first, double last) const;

    // Persistence implementer ---------------------
    unsigned int getMemSize() const override;
    void Save(Base::Writer& writer) const override;
    void Restore(Base::XMLReader& reader) override;
    // Base implementer ----------------------------
    PyObject* getPyObject() override;
    bool isSame(const Geometry& other, double tol, double atol) const override;

    const Handle(Geom_Geometry) & handle() const override;
    void setHandle(const Handle(Geom_CurveOnSurface) & curve);

    Handle(Geom2d_Curve) getCurve2d() const;
    void setCurve2d(const Handle(Geom2d_Curve) & curve2d);
    Handle(Geom_Surface) getSurface() const;
    void setSurface(const Handle(Geom_Surface) & surface);

private:
    Handle(Geom_CurveOnSurface) myCurve;
};

}  // namespace Part
