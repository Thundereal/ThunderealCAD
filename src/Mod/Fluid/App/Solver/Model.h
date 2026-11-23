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

// A flat face the fluid comes in through, at one speed all over it.
struct Inlet
{
    std::string source;
    Surface surface;  // the face, open: its triangles
    Vec3 direction {0.0, 0.0, 1.0};  // the way the fluid goes, a unit vector
                                     // square to the face
    double speed = 0.0;  // m/s
    Material material;
    double start = 0.0;  // s
    double end = 0.0;    // s; not after start: to the run's end
};

// A flat face the fluid leaves through, held at one pressure.
struct Outlet
{
    std::string source;
    Surface surface;        // the face, open: its triangles
    double pressure = 0.0;  // Pa, over the surroundings'
};

// What kind of flow the model is of.
enum class Analysis
{
    FreeSurface,  // a liquid under the air, from how it stands at the start,
                  // through time
    Steady,       // one fluid filling the closed space inside the walls, in
                  // at its inlets and out at its outlets, as it settles to
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
    Force,     // the fluid's on a wall, along direction, N
};

struct Monitor
{
    std::string source;
    MonitorKind kind = MonitorKind::Height;
    Vec3 point {0.0, 0.0, 0.0};
    // Force: the wall, as its place in the model's walls, and the unit
    // vector the force is taken along
    int wall = -1;
    Vec3 direction {0.0, 0.0, 0.0};
};

struct Model
{
    int version = 1;
    std::vector<Wall> walls;
    std::vector<FluidVolume> fluids;
    std::vector<Inlet> inlets;
    Analysis analysis = Analysis::FreeSurface;
    // Of a steady flow only, what follows to the space.
    std::vector<Outlet> outlets;
    // The space the fluid fills, closed, its triangles wound outwards. Its
    // patches say what each triangle is: 0 a wall, 1 + i the model's inlet
    // i, 1 + the count of inlets + j its outlet j.
    Surface domain;
    // How many steps the flow is given to settle in, at most.
    int iterations = 2000;
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

// What all the fluid of the model is made of: the first volume's, or with
// none the first inlet's. false when the model has neither.
FluidExport bool liquidOf(const Model& model, Material& liquid);

// An inlet's face: its area (m^2), its middle, and how far its triangles
// are from one plane square to the inlet's direction -- the sine of the
// widest angle between a triangle and that plane. false for a face of no
// area.
FluidExport bool faceOf(const Inlet& inlet, double& area, Vec3& middle, double& slant);

// What a closed surface wound outwards holds, m^3.
FluidExport double volumeOf(const Surface& surface);

// The area of each patch of a surface, m^2: count of them, a triangle whose
// patch is none of them left out. A surface without patches is all patch 0.
FluidExport std::vector<double> patchAreas(const Surface& surface, std::size_t count);

// The box of a surface's points: false when it has none.
FluidExport bool boxOf(const Surface& surface, Vec3& min, Vec3& max);

}  // namespace Fluid::Solver
