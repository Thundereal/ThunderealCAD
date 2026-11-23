// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

// The solver-neutral model of a fluid study (docs/FluidSimulation.md sec
// 11.1): plain data, SI units (m, kg, s, Pa), in the document's global
// coordinates, and nothing that points back into the document. It says what
// is to be simulated, never how: nothing here names particles or cells.
namespace Fluid::Solver
{

using Vec3 = std::array<double, 3>;

// A closed triangulation, its triangles wound outwards.
struct Surface
{
    std::vector<Vec3> points;
    std::vector<std::array<int, 3>> triangles;
    // One per triangle: which face group of its part it lies on. Empty when
    // nobody asks.
    std::vector<int> patches;
};

enum class WallMotion
{
    Fixed,  // where it is, for the whole run
    Given,  // moved as a kept run has it (not yet)
    Free,   // moved by the fluid and by gravity, held by nothing
};

// What a wall the fluid moves weighs, and how.
struct MassProps
{
    double mass = 0.0;  // kg
    Vec3 centre {0.0, 0.0, 0.0};
    // about the centre, along the model's axes, kg m^2: xx, yy, zz, xy, xz,
    // yz, the products as the tensor's entries (minus the integrals)
    std::array<double, 6> inertia {};
};

// A rigid motion from where a wall stood at the start: a point p of it is
// at rotation * p + translation.
struct Motion
{
    Vec3 translation {0.0, 0.0, 0.0};
    std::array<double, 4> rotation {0.0, 0.0, 0.0, 1.0};  // a unit quaternion: x, y, z, w
};

// A part the fluid cannot pass.
struct Wall
{
    std::string source;  // the document object, by name; "Document#Object" in
                         // another document than the study's
    Surface surface;
    WallMotion motion = WallMotion::Fixed;
    MassProps mass;  // of a Free one
};

struct Material
{
    double density = 1000.0;    // kg/m^3
    double viscosity = 1.0e-3;  // dynamic, Pa s
};

// A closed volume full of fluid when the run starts.
struct FluidVolume
{
    std::string source;
    Surface surface;
    Material material;
};

// The box nothing leaves. A side that is open lets the fluid out, and it is
// gone; the order is -x, +x, -y, +y, -z, +z.
struct Space
{
    Vec3 min {0.0, 0.0, 0.0};
    Vec3 max {0.0, 0.0, 0.0};
    std::array<bool, 6> open {};
};

enum class MonitorKind
{
    Height,    // of the fluid's surface on the line through point along
               // -gravity, measured from point, m
    Pressure,  // at point, Pa, over the surroundings'
};

struct Monitor
{
    std::string source;
    MonitorKind kind = MonitorKind::Height;
    Vec3 point {0.0, 0.0, 0.0};
};

struct Model
{
    int version = 1;
    std::vector<Wall> walls;
    std::vector<FluidVolume> fluids;
    Space space;
    Vec3 gravity {0.0, 0.0, -9.80665};  // m/s^2
    double timeEnd = 1.0;               // s
    double outputInterval = 0.01;       // s
    // The size of what is to be resolved, m. A particle backend takes it as
    // its spacing, a mesh backend as its base cell.
    double resolution = 0.01;
    std::vector<Monitor> monitors;
};

// A number that changes when anything in the model does: what a kept result
// is compared by, to tell whether it is still of this model.
FluidExport std::uint64_t signature(const Model& model);

// Whether the point is inside the closed surface.
FluidExport bool inside(const Surface& surface, const Vec3& point);

// The points of a grid of that spacing inside the closed surface, each the
// centre of a cube of the spacing's side; the grid starts half a spacing in
// from the surface's box, so a box of whole spacings is filled exactly.
FluidExport std::vector<Vec3> sampled(const Surface& surface, double spacing);

// The walls the fluid moves, as their places in the model's walls.
FluidExport std::vector<std::size_t> freeWalls(const Model& model);

// The box of a surface's points: false when it has none.
FluidExport bool boxOf(const Surface& surface, Vec3& min, Vec3& max);

}  // namespace Fluid::Solver
