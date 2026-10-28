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

#include <Standard_Failure.hxx>

#include "CurveOnSurfacePy.h"
#include "CurveOnSurfacePy.cpp"
#include "Geom2d/Curve2dPy.h"
#include "Geometry2d.h"
#include "GeometrySurfacePy.h"
#include "OCCError.h"
#include "TopoShapeEdgePy.h"

using namespace Part;

std::string CurveOnSurfacePy::representation() const
{
    return "<CurveOnSurface object>";
}

PyObject* CurveOnSurfacePy::PyMake(struct _typeobject*, PyObject*, PyObject*)
{
    return new CurveOnSurfacePy(new GeomCurveOnSurface);
}

namespace
{
Handle(Geom2d_Curve) curve2dOf(PyObject* p)
{
    if (!PyObject_TypeCheck(p, &Curve2dPy::Type)) {
        throw Py::TypeError("expected a Part.Geom2d curve");
    }
    return Handle(Geom2d_Curve)::DownCast(static_cast<Curve2dPy*>(p)->getGeometry2dPtr()->handle());
}

Handle(Geom_Surface) surfaceOf(PyObject* p)
{
    if (!PyObject_TypeCheck(p, &GeometrySurfacePy::Type)) {
        throw Py::TypeError("expected a Part surface");
    }
    return Handle(Geom_Surface)::DownCast(
        static_cast<GeometrySurfacePy*>(p)->getGeometryPtr()->handle());
}
}  // namespace

int CurveOnSurfacePy::PyInit(PyObject* args, PyObject* /*kwd*/)
{
    PyObject *curve, *surface;
    if (!PyArg_ParseTuple(args, "OO", &curve, &surface)) {
        return -1;
    }
    try {
        getGeomCurveOnSurfacePtr()->setHandle(
            new Geom_CurveOnSurface(Handle(Geom2d_Curve)::DownCast(curve2dOf(curve)->Copy()),
                                    Handle(Geom_Surface)::DownCast(surfaceOf(surface)->Copy())));
        return 0;
    }
    catch (const Py::Exception&) {
        return -1;
    }
    catch (Standard_Failure& e) {
        PyErr_SetString(PartExceptionOCCError, e.GetMessageString());
        return -1;
    }
}

PyObject* CurveOnSurfacePy::toShape(PyObject* args) const
{
    auto geo = getGeomCurveOnSurfacePtr();
    double first = geo->getFirstParameter(), last = geo->getLastParameter();
    if (!PyArg_ParseTuple(args, "|dd", &first, &last)) {
        return nullptr;
    }
    PY_TRY
    {
        return new TopoShapeEdgePy(new TopoShape(geo->toShape(first, last)));
    }
    PY_CATCH_OCC
}

Py::Object CurveOnSurfacePy::getCurve2d() const
{
    auto c = makeFromCurve2d(getGeomCurveOnSurfacePtr()->getCurve2d());
    return Py::asObject(c->getPyObject());
}

void CurveOnSurfacePy::setCurve2d(Py::Object arg)
{
    getGeomCurveOnSurfacePtr()->setCurve2d(
        Handle(Geom2d_Curve)::DownCast(curve2dOf(arg.ptr())->Copy()));
}

Py::Object CurveOnSurfacePy::getSurface() const
{
    auto s = makeFromSurface(getGeomCurveOnSurfacePtr()->getSurface());
    return Py::asObject(s->getPyObject());
}

void CurveOnSurfacePy::setSurface(Py::Object arg)
{
    getGeomCurveOnSurfacePtr()->setSurface(
        Handle(Geom_Surface)::DownCast(surfaceOf(arg.ptr())->Copy()));
}

PyObject* CurveOnSurfacePy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int CurveOnSurfacePy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}
