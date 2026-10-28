// SPDX-License-Identifier: LGPL-2.1-or-later

// The element geometry is Part's (Mod/Part/App/ElementGeometry.h), with the
// element frame, which Part::ElementBinding needs; the Assembly names stay.

#pragma once

#include <Mod/Part/App/ElementGeometry.h>

namespace Assembly
{
using Part::analyzeEdge;
using Part::analyzeFace;
using Part::clearElementGeometryCache;
using Part::describeCurveType;
using Part::describeSurfaceType;
using Part::ElementCurve;
using Part::elementGeometryConversions;
using Part::ElementSurface;
using Part::findElementFrame;
using Part::isAnalyticCurve;
using Part::isAnalyticSurface;
using Part::recognitionTolerance;
}  // namespace Assembly
