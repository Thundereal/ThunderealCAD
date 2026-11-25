// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "Solver/Model.h"

// The surface of a frame of particles (docs/FluidSimulation.md sec 12.6):
// what a particle backend's fluid is drawn as, where a backend of cells
// hands its surface over ready.
namespace Fluid
{

struct ParticleSurface
{
    std::vector<Solver::Vec3> points;
    std::vector<Solver::Vec3> normals;  // one a point, outwards, of length 1
    std::vector<std::array<int, 3>> triangles;  // wound outwards
    std::vector<Solver::Vec3> spray;  // the particles left out of it, see sprayBelow
};

// A point on a solid the fluid meets, and the normal of the solid's face
// there, out of the solid: what the surface is told of the walls.
struct WallPoint
{
    Solver::Vec3 at;
    Solver::Vec3 out;  // of length 1
};

// Points over a surface's triangles no further apart than the step, each
// with its triangle's normal: the triangles wound out of the solid.
FluidExport std::vector<WallPoint> wallPointsOf(const Solver::Surface& surface, double step);

// The closed surface around particles that each stand for a cube of the
// spacing, in the particles' unit. Against a flat face of them it lies
// half a spacing past their middles, where the fluid's edge is. Particles
// apart from the body are drops: one alone a small ball, near ones run
// together. Every particle is inside the surface -- but for the spray,
// when sprayBelow is given: a particle with that little of the others
// about it is left out of the surface and handed back in spray, to be
// drawn as what it is. sprayBelow counts in what a particle alone has
// about it, itself: 1 is no particle, 1.42 one with another a spacing off.
// Nothing of no particles or no spacing.
//
// Given the walls (points on them no further apart than half the spacing),
// a particle within reach of one has its mirror image across it in the
// field, on the water's side of it alone: beside a wall half of what is
// about a particle is the wall's, and without its share the field there
// hovers about the level -- the surface dips to the particles, and the
// ones at the top count as drops or spray, flipping as they move.
FluidExport void surfaceOf(const std::vector<Solver::Vec3>& particles,
                           double spacing,
                           ParticleSurface& surface,
                           double sprayBelow = 0.0,
                           const std::vector<WallPoint>& walls = {});

// What a frame is drawn with: a particle alone, a pair, three in a row are
// spray; a clump of four and more is a drop of the surface. Thrown water
// at a particle's size is a cloud of single ones, and as so many balls of
// water it does not look like water at all.
constexpr double SprayBelow = 2.5;

}  // namespace Fluid
