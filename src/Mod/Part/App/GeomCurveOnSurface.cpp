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
#include <sstream>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepLib.hxx>
#include <Geom2dConvert.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GeomTools.hxx>
#include <Geom_Plane.hxx>
#include <Precision.hxx>
#include <Standard_NotImplemented.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_GTrsf2d.hxx>

#include <Base/Base64.h>
#include <Base/Exception.h>
#include <Base/Reader.h>
#include <Base/Writer.h>

#include "CurveOnSurfacePy.h"
#include "GeomCurveOnSurface.h"

using namespace Part;

// -------------------------------------------------------------------------------------
// Geom_CurveOnSurface

Geom_CurveOnSurface::Geom_CurveOnSurface(const Handle(Geom2d_Curve) & curve2d,
                                         const Handle(Geom_Surface) & surface)
{
    SetCurve2d(curve2d);
    SetSurface(surface);
}

void Geom_CurveOnSurface::SetCurve2d(const Handle(Geom2d_Curve) & curve2d)
{
    if (curve2d.IsNull()) {
        throw Standard_ConstructionError("Geom_CurveOnSurface: no 2D curve");
    }
    myCurve2d = curve2d;
}

void Geom_CurveOnSurface::SetSurface(const Handle(Geom_Surface) & surface)
{
    if (surface.IsNull()) {
        throw Standard_ConstructionError("Geom_CurveOnSurface: no surface");
    }
    mySurface = surface;
}

void Geom_CurveOnSurface::Reverse()
{
    myCurve2d->Reverse();
}

double Geom_CurveOnSurface::ReversedParameter(const double U) const
{
    return myCurve2d->ReversedParameter(U);
}

double Geom_CurveOnSurface::FirstParameter() const
{
    return myCurve2d->FirstParameter();
}

double Geom_CurveOnSurface::LastParameter() const
{
    return myCurve2d->LastParameter();
}

bool Geom_CurveOnSurface::IsClosed() const
{
    return StartPoint().Distance(EndPoint()) <= Precision::Confusion();
}

bool Geom_CurveOnSurface::IsPeriodic() const
{
    return myCurve2d->IsPeriodic();
}

double Geom_CurveOnSurface::Period() const
{
    return myCurve2d->Period();
}

GeomAbs_Shape Geom_CurveOnSurface::Continuity() const
{
    return std::min(myCurve2d->Continuity(), mySurface->Continuity());
}

bool Geom_CurveOnSurface::IsCN(const int N) const
{
    return myCurve2d->IsCN(N) && mySurface->IsCNu(N) && mySurface->IsCNv(N);
}

gp_Pnt Geom_CurveOnSurface::EvalD0(const double U) const
{
    gp_Pnt2d uv = myCurve2d->Value(U);
    gp_Pnt p;
    mySurface->D0(uv.X(), uv.Y(), p);
    return p;
}

// S(c(t)), c = (u, v): C' = Su u' + Sv v';
// C'' = Suu u'^2 + 2 Suv u'v' + Svv v'^2 + Su u'' + Sv v''.
Geom_Curve::ResD1 Geom_CurveOnSurface::EvalD1(const double U) const
{
    gp_Pnt2d uv;
    gp_Vec2d d1;
    myCurve2d->D1(U, uv, d1);
    ResD1 r;
    gp_Vec su, sv;
    mySurface->D1(uv.X(), uv.Y(), r.Point, su, sv);
    r.D1 = su * d1.X() + sv * d1.Y();
    return r;
}

Geom_Curve::ResD2 Geom_CurveOnSurface::EvalD2(const double U) const
{
    gp_Pnt2d uv;
    gp_Vec2d d1, d2;
    myCurve2d->D2(U, uv, d1, d2);
    ResD2 r;
    gp_Vec su, sv, suu, svv, suv;
    mySurface->D2(uv.X(), uv.Y(), r.Point, su, sv, suu, svv, suv);
    const double u1 = d1.X(), v1 = d1.Y();
    r.D1 = su * u1 + sv * v1;
    r.D2 = suu * (u1 * u1) + suv * (2 * u1 * v1) + svv * (v1 * v1) + su * d2.X() + sv * d2.Y();
    return r;
}

// C''' = Suuu u'^3 + 3 Suuv u'^2 v' + 3 Suvv u' v'^2 + Svvv v'^3
//      + 3 (Suu u' u'' + Suv (u' v'' + u'' v') + Svv v' v'') + Su u''' + Sv v'''.
Geom_Curve::ResD3 Geom_CurveOnSurface::EvalD3(const double U) const
{
    gp_Pnt2d uv;
    gp_Vec2d d1, d2, d3;
    myCurve2d->D3(U, uv, d1, d2, d3);
    ResD3 r;
    gp_Vec su, sv, suu, svv, suv, suuu, svvv, suuv, suvv;
    mySurface->D3(uv.X(), uv.Y(), r.Point, su, sv, suu, svv, suv, suuu, svvv, suuv, suvv);
    const double u1 = d1.X(), v1 = d1.Y(), u2 = d2.X(), v2 = d2.Y();
    r.D1 = su * u1 + sv * v1;
    r.D2 = suu * (u1 * u1) + suv * (2 * u1 * v1) + svv * (v1 * v1) + su * u2 + sv * v2;
    r.D3 = suuu * (u1 * u1 * u1) + suuv * (3 * u1 * u1 * v1) + suvv * (3 * u1 * v1 * v1)
        + svvv * (v1 * v1 * v1) + suu * (3 * u1 * u2) + suv * (3 * (u1 * v2 + u2 * v1))
        + svv * (3 * v1 * v2) + su * d3.X() + sv * d3.Y();
    return r;
}

gp_Vec Geom_CurveOnSurface::EvalDN(const double U, const int N) const
{
    switch (N) {
        case 1:
            return EvalD1(U).D1;
        case 2:
            return EvalD2(U).D2;
        case 3:
            return EvalD3(U).D3;
        default:
            throw Standard_NotImplemented("Geom_CurveOnSurface: derivatives above the third");
    }
}

gp_Pnt Geom_CurveOnSurface::StartPoint() const
{
    return EvalD0(FirstParameter());
}

gp_Pnt Geom_CurveOnSurface::EndPoint() const
{
    return EvalD0(LastParameter());
}

void Geom_CurveOnSurface::Transform(const gp_Trsf& T)
{
    // The point (u, v) maps to on the moved surface may be at other parameters (a
    // plane scaled): the 2D curve goes there with it, its poles moved.
    gp_GTrsf2d g = mySurface->ParametricTransformation(T);
    mySurface->Transform(T);
    if (g.Form() == gp_Identity) {
        return;
    }
    Handle(Geom2d_BSplineCurve) bs = Geom2dConvert::CurveToBSplineCurve(myCurve2d);
    for (int i = 1; i <= bs->NbPoles(); ++i) {
        gp_XY xy = bs->Pole(i).XY();
        g.Transforms(xy);
        bs->SetPole(i, gp_Pnt2d(xy));
    }
    myCurve2d = bs;
}

Handle(Geom_Geometry) Geom_CurveOnSurface::Copy() const
{
    return new Geom_CurveOnSurface(Handle(Geom2d_Curve)::DownCast(myCurve2d->Copy()),
                                   Handle(Geom_Surface)::DownCast(mySurface->Copy()));
}

// -------------------------------------------------------------------------------------
// GeomCurveOnSurface

TYPESYSTEM_SOURCE(Part::GeomCurveOnSurface, Part::GeomBoundedCurve)

GeomCurveOnSurface::GeomCurveOnSurface()
{
    Handle(Geom2d_Curve) segment =
        new Geom2d_TrimmedCurve(new Geom2d_Line(gp_Pnt2d(0, 0), gp_Dir2d(1, 0)), 0., 1.);
    myCurve = new Geom_CurveOnSurface(segment, new Geom_Plane(gp::XOY()));
}

GeomCurveOnSurface::GeomCurveOnSurface(const Handle(Geom2d_Curve) & curve2d,
                                       const Handle(Geom_Surface) & surface)
{
    myCurve = new Geom_CurveOnSurface(curve2d, surface);
}

GeomCurveOnSurface::GeomCurveOnSurface(const Handle(Geom_CurveOnSurface) & curve)
{
    setHandle(curve);
}

GeomCurveOnSurface::~GeomCurveOnSurface() = default;

Geometry* GeomCurveOnSurface::copy() const
{
    auto* c = new GeomCurveOnSurface(Handle(Geom_CurveOnSurface)::DownCast(myCurve->Copy()));
    c->copyNonTag(this);
    return c;
}

const Handle(Geom_Geometry) & GeomCurveOnSurface::handle() const
{
    return myCurve;
}

void GeomCurveOnSurface::setHandle(const Handle(Geom_CurveOnSurface) & curve)
{
    myCurve = Handle(Geom_CurveOnSurface)::DownCast(curve->Copy());
}

Handle(Geom2d_Curve) GeomCurveOnSurface::getCurve2d() const
{
    return myCurve->Curve2d();
}

void GeomCurveOnSurface::setCurve2d(const Handle(Geom2d_Curve) & curve2d)
{
    myCurve->SetCurve2d(curve2d);
}

Handle(Geom_Surface) GeomCurveOnSurface::getSurface() const
{
    return myCurve->Surface();
}

void GeomCurveOnSurface::setSurface(const Handle(Geom_Surface) & surface)
{
    myCurve->SetSurface(surface);
}

TopoDS_Shape GeomCurveOnSurface::toShape() const
{
    return toShape(myCurve->FirstParameter(), myCurve->LastParameter());
}

TopoDS_Shape GeomCurveOnSurface::toShape(double first, double last) const
{
    BRepBuilderAPI_MakeEdge make(myCurve->Curve2d(), myCurve->Surface(), first, last);
    if (!make.IsDone()) {
        THROWM(Base::CADKernelError, "No edge from the curve on its surface");
    }
    TopoDS_Edge edge = make.Edge();
    // the 3D curve the edge needs, approximated to the 2D curve on the surface
    BRepLib::BuildCurves3d(edge, Precision::Confusion());
    return edge;
}

namespace
{
template<class T>
std::string writeGeom(const T& geom)
{
    std::ostringstream os;
    os.precision(17);
    GeomTools::Write(geom, os);
    std::string text = os.str();
    return Base::base64_encode(text.data(), text.size());
}

template<class T>
void readGeom(T& geom, const char* encoded)
{
    std::istringstream is(Base::base64_decode(std::string(encoded)));
    GeomTools::Read(geom, is);
    if (geom.IsNull()) {
        THROWM(Base::RestoreError, "GeomCurveOnSurface: an unreadable curve or surface");
    }
}
}  // namespace

unsigned int GeomCurveOnSurface::getMemSize() const
{
    return sizeof(Geom_CurveOnSurface) + 1024;
}

void GeomCurveOnSurface::Save(Base::Writer& writer) const
{
    GeomCurve::Save(writer);
    // OCCT's own text forms, which hold any surface and 2D curve
    writer.Stream() << writer.ind() << "<CurveOnSurface Surface=\"" << writeGeom(myCurve->Surface())
                    << "\" Curve2d=\"" << writeGeom(myCurve->Curve2d()) << "\"/>" << std::endl;
}

void GeomCurveOnSurface::Restore(Base::XMLReader& reader)
{
    GeomCurve::Restore(reader);
    reader.readElement("CurveOnSurface");
    Handle(Geom_Surface) surface;
    Handle(Geom2d_Curve) curve2d;
    readGeom(surface, reader.getAttribute<const char*>("Surface"));
    readGeom(curve2d, reader.getAttribute<const char*>("Curve2d"));
    myCurve = new Geom_CurveOnSurface(curve2d, surface);
}

PyObject* GeomCurveOnSurface::getPyObject()
{
    return new CurveOnSurfacePy(static_cast<GeomCurveOnSurface*>(this->clone()));
}

bool GeomCurveOnSurface::isSame(const Geometry& other, double tol, double /*atol*/) const
{
    if (other.getTypeId() != getTypeId()) {
        return false;
    }
    // the same points along the same parameters
    auto& o = static_cast<const GeomCurveOnSurface&>(other);
    double f = myCurve->FirstParameter(), l = myCurve->LastParameter();
    if (std::fabs(f - o.myCurve->FirstParameter()) > tol
        || std::fabs(l - o.myCurve->LastParameter()) > tol) {
        return false;
    }
    for (int i = 0; i <= 16; ++i) {
        double u = f + (l - f) * i / 16;
        if (myCurve->Value(u).SquareDistance(o.myCurve->Value(u)) > tol * tol) {
            return false;
        }
    }
    return true;
}
