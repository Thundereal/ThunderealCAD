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
//   frames/000000.points.npy      (points, 3) float32, mm: one per frame
//   frames/000000.triangles.npy   (triangles, 3) int32: a surface's frames
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
};

// The unit a monitor's values are kept in.
FluidExport const char* unitOf(Solver::MonitorKind kind);

// One frame as kept: the points in mm, float32, x y z each.
struct ArchivedFrame
{
    double time = 0.0;
    std::vector<float> points;
    std::vector<std::int32_t> triangles;
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
