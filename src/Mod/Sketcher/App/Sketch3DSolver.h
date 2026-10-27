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

// The solver facade of the 3D sketch (docs/Sketch3D.md section 5): it maps a
// Sketch3DObject's geometry and Sketcher::Constraint list onto the PlaneGCS 3D layer,
// solves, and hands the solved geometry back -- what Sketcher::Sketch does for 2D.

#include <deque>
#include <memory>
#include <vector>

#include <Base/Vector3D.h>

#include "Constraint.h"
#include "GeoEnum.h"
#include "planegcs/Constraints3D.h"
#include "planegcs/GCS.h"

namespace Part
{
class Geometry;
}

namespace Sketcher
{

class SketcherExport Sketch3DSolver
{
public:
    Sketch3DSolver();
    ~Sketch3DSolver();
    Sketch3DSolver(const Sketch3DSolver&) = delete;
    Sketch3DSolver& operator=(const Sketch3DSolver&) = delete;

    /** Builds the system and diagnoses it.
     * @param geometry: the sketch's own geometry, geometry id = index
     * @param external: the frame's axes and planes and the external references, in
     *        sketch coordinates; geometry id -1 is external[0], -2 external[1], ...
     * @param constraints: the sketch's constraints; constraint i is solver tag i + 1
     * @return the DOF count, or -1 when there is nothing to solve
     */
    int setUp(const std::vector<const Part::Geometry*>& geometry,
              const std::vector<const Part::Geometry*>& external,
              const std::vector<Constraint*>& constraints);

    /// 0 when solved, -1 when the solver failed (the parameters are restored).
    int solve();

    /// Drags a point, or a whole curve (pos none), to a position and solves with the
    /// sketch's constraints taking priority. 0 when solved.
    int movePoint(int geoId, PointPos pos, const Base::Vector3d& to, bool relative);

    /// The sketch's geometry with the solved parameters, cloned from what setUp was
    /// given so the extensions (id, construction) are kept.
    std::vector<std::unique_ptr<Part::Geometry>> extractGeometry() const;

    /// The value a driven (reference) constraint measures after a solve, in the
    /// constraint's own units (mm, radians, diameter for Diameter).
    bool getDrivenValue(int constraintIndex, double& value) const;

    int getDoF() const
    {
        return dofs;
    }
    // Constraint indices (0-based).
    const std::vector<int>& getConflicting() const
    {
        return conflicting;
    }
    const std::vector<int>& getRedundant() const
    {
        return redundant;
    }
    const std::vector<int>& getPartiallyRedundant() const
    {
        return partiallyRedundant;
    }
    /// Constraints the 3D sketch cannot express, ignored by the solve.
    const std::vector<int>& getMalformed() const
    {
        return malformed;
    }

private:
    enum class Kind
    {
        Point,
        Line,
        Circle,
        Arc,
        BSpline,
        Plane,
        Curve,    // an external curve of any other kind
        Surface,  // an external curved face
    };

    struct Entity
    {
        Kind kind {Kind::Point};
        bool external {false};
        GCS::Point3 start, end, mid;  // mid: a circle's or arc's centre, a plane's origin
        GCS::Point3 normal;           // circle, arc
        double* rad {nullptr};        // circle
        GCS::Arc3 arc;
        GCS::Circle3 circle;
        GCS::BSpline3 bsp;
        GCS::VEC_D distinctKnots;  // BSpline: one per knot, as InternalAlignment indexes them
        GCS::Plane3 plane;
        std::shared_ptr<const GCS::FixedCurve3> fixedCurve;  // externals with a curve
        std::shared_ptr<const GCS::FixedSurface3> fixedSurface;  // an external face
        const Part::Geometry* source {nullptr};
    };

    double* param(double value, bool unknown);
    GCS::Point3 point(const Base::Vector3d& v, bool unknown);
    void addEntity(const Part::Geometry* geo, bool external, bool blocked);
    Entity* entity(int geoId);
    GCS::Point3* entityPoint(int geoId, PointPos pos);
    // The tangent direction of a curve at one of its ends.
    bool tangentAt(int geoId, PointPos pos, GCS::Dir3& dir);
    double closestParameter(const Entity& e, const GCS::Point3& p) const;
    bool addConstraint(const Constraint& c, int tag);

    std::deque<double> params;
    GCS::VEC_pD unknowns;
    GCS::VEC_pD driven;
    std::vector<Entity> entities;  // internal geometry first, then external
    std::size_t internalCount {0};
    std::vector<double*> valueParams;  // per constraint, or null
    std::vector<double> valueScale;    // Diameter: 2
    GCS::System sys;
    GCS::Point3 origin;  // the frame's origin, for a point's coordinate

    int dofs {-1};
    std::vector<int> conflicting, redundant, partiallyRedundant, malformed;
};

}  // namespace Sketcher
