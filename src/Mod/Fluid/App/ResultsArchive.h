// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "Solver/Backend.h"

// A run's results as its study keeps them in the document
// (docs/FluidSimulation.md sec 11.4): a zip of
//   index.json                    the schema, the units, the backend, the
//                                 model's signature, the particles'
//                                 spacing, the monitors
//   times.npy     (frames,)       float64, s: the frames' times
//   samples.npy   (samples,)      float64, s: the monitors' times, which
//                                 are their own and far more than the frames
//   monitors.npy  (samples, n)    float64, each monitor in its unit
//   energy.npy    (n, 6)          float64, J, when the solver kept account
//                                 of it: the time (s), the fluid's kinetic
//                                 and potential energy, its work on the
//                                 walls it moves, and what a mechanism's
//                                 lag made and what went over (Backend.h)
//   frames/000000.points.npy      (points, 3) float32, mm: one per frame
//   frames/000000.triangles.npy   (triangles, 3) int32: a surface's frames
//   frames/000000.values.npy      (points,) float32, Pa: the pressure at
//                                 each point, of a frame of fields
//   openings.npy  (frames, n, 2)  float64: of a steady flow, at each frame
//                                 each inlet's then each outlet's mean
//                                 pressure, Pa, and what goes through it,
//                                 m^3/s
//   bodies.npy    (frames, n, 7)  float64: where each wall the fluid moved
//                                 is at each frame, as the motion from
//                                 where it stood: the translation, mm, and
//                                 the rotation's quaternion x, y, z, w.
//                                 Only when there is such a wall.
// which numpy.load reads as they are. A frame is written as the run makes
// it and read only when it is asked for: a run's frames do not fit in
// memory together, and playback needs one at a time.
namespace Fluid
{

struct ArchivedMonitor
{
    std::string source;  // as the model names it
    Solver::MonitorKind kind = Solver::MonitorKind::Height;
};

// A face the fluid came in or went out through.
struct ArchivedOpening
{
    std::string source;  // as the model names it
    bool outlet = false;
};

struct ResultsIndex
{
    std::string backend;
    std::string version;
    Solver::ResultKind kind = Solver::ResultKind::Particles;
    std::uint64_t signature = 0;
    double spacing = 0.0;  // between particles at rest, mm; 0 when not kept
    std::vector<ArchivedMonitor> monitors;
    std::vector<std::string> bodies;  // the walls the fluid moved, as the model names them
    std::vector<double> times;    // the frames', s
    std::vector<double> samples;  // the monitors' times, s
    std::vector<double> values;   // samples x monitors: Height mm, Pressure Pa, Force N
    std::vector<double> motions;  // frames x bodies x 7, as bodies.npy
    // Of a steady flow: the frames' times are the steps it had settled
    // for, and the samples' too.
    bool steady = false;
    // Whether it settled before the steps it was given were up.
    bool settled = false;
    std::vector<ArchivedOpening> openings;  // the inlets, then the outlets
    std::vector<double> flows;  // frames x openings x 2, as openings.npy
    // The fluid's energy as the run went, J; none from a solver that keeps
    // no account of it, or in results made before it was kept.
    std::vector<Solver::Energy> energy;
    // Whether the run was fed: fluid came in at inlets, with energy the
    // account does not have.
    bool fed = false;
};

// Whether a run's own account of its energy holds (docs/FluidSimulation.md
// sec 12.11). Both checks leave out the first quarter of the run: fluid
// seeded against a part is pressed apart in the first steps, and a
// mechanism's parts start by a push of none.
//
// It says a run went wrong, never that it went right: a run on particles
// too coarse for what is asked of it keeps its account in order.
struct EnergyVerdict
{
    bool kept = false;  // the run kept an account, long enough to judge
    // The fluid's own: its energy and its work on the walls it moves
    // cannot rise together. Not judged of a run that was fed.
    bool budget = false;
    double rise = 0.0;  // the most it rose above its lowest before, as a
                        // share of the fluid's greatest kinetic energy
    bool rose = false;  // by more than riseLimit
    // A mechanism's: what its lag made of what went over.
    bool handOver = false;  // something went over
    double made = 0.0;      // the share
    bool lagged = false;    // more than madeLimit
    static constexpr double riseLimit = 0.1;
    static constexpr double madeLimit = 0.2;
};
FluidExport EnergyVerdict judge(const ResultsIndex& index);

// The unit a monitor's values are kept in.
FluidExport const char* unitOf(Solver::MonitorKind kind);

// One frame as kept: the points in mm, float32, x y z each.
struct ArchivedFrame
{
    double time = 0.0;
    std::vector<float> points;
    std::vector<std::int32_t> triangles;
    std::vector<float> values;  // a frame of fields: the pressure a point, Pa
    // the moved walls, in the index's order; the translation in mm
    std::vector<Solver::Motion> bodies;
};

class FluidExport ArchiveWriter
{
public:
    // Throws Base::FileException when the file cannot be written.
    explicit ArchiveWriter(const std::string& path);
    ~ArchiveWriter();
    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    // A frame in the model's units (m); its time is kept for close().
    void frame(const Solver::Frame& frame);
    // The monitors at a time, in the model's units, kept for close().
    void sample(double time, const std::vector<double>& values);
    // The fluid's energy at a time, kept for close().
    void energy(const Solver::Energy& energy);
    std::size_t frames() const;
    double lastTime() const;
    // Writes the times, the monitors' values and the index (its times,
    // samples and values are taken from what was written), and closes the
    // file.
    void close(ResultsIndex index);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

class FluidExport ArchiveReader
{
public:
    // Throws Base::FileException when the file cannot be read,
    // Base::RuntimeError when it is not such an archive or a later schema's.
    explicit ArchiveReader(const std::string& path);
    ~ArchiveReader();
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;

    const ResultsIndex& index() const;
    // Throws Base::IndexError past the last frame.
    ArchivedFrame frame(std::size_t at) const;
    // Where the moved walls are at a frame, without its points; throws as
    // frame().
    std::vector<Solver::Motion> bodies(std::size_t at) const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

}  // namespace Fluid
