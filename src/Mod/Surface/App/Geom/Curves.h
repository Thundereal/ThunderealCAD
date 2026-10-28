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

#include <string>
#include <vector>

#include <Geom_BSplineCurve.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <Mod/Surface/SurfaceGlobal.h>

#include "Extend.h"

namespace Surface::Geom
{

/** Extend `curve` at its start or end by `length` (docs/Surface.md sec 5.3,
 * Curve extend)
 *
 * Natural carries its last span's polynomial on, exactly, so the curve is
 * smooth with itself there to every order; Linear goes on straight along
 * its tangent, C1. The length is the distance from the old end to the new
 * one. The curve keeps its degree and its parameters. Returns why not, or
 * empty.
 */
SurfaceExport std::string extendCurve(Handle(Geom_BSplineCurve)& curve,
                                      bool atEnd,
                                      double length,
                                      Extension mode);

/** Edges joined into smooth curves (docs/Surface.md sec 5.3, Join curves)
 *
 * The edges are chained end to end (any order, either way round, ends
 * within `gap`); the chain is broken where it turns by more than
 * `cornerAngle` (radians); each smooth run becomes one B-spline within
 * `tolerance` of the edges, closed when its ends meet. Returns the curves,
 * or `error` says why there are none.
 */
SurfaceExport std::vector<Handle(Geom_BSplineCurve)>
joinCurves(const std::vector<TopoDS_Edge>& edges,
           double tolerance,
           double cornerAngle,
           double gap,
           std::string& error);

/** Isoparametric curves of a face, clipped to it (docs/Surface.md sec 5.3,
 * Isocurve): `nu` along U at even parameters inside its parameter box
 * (each a curve of constant U), `nv` along V; each an edge on the face's
 * surface, as many pieces as the face's boundary cuts it into.
 */
SurfaceExport std::vector<TopoDS_Edge> isocurves(const TopoDS_Face& face, int nu, int nv);

}  // namespace Surface::Geom
