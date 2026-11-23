// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <map>
#include <string>
#include <vector>

#include <Mod/Fluid/FluidGlobal.h>

#include "Backend.h"

// A free-surface study as an OpenFOAM case (docs/FluidSimulation.md secs
// 5.5, 12 S2): the files interFoam and its meshers read, written from the
// model, and what they write back read into frames. Text in, text out: no
// OpenFOAM is needed to make a case or to test one, and none of its code is
// in this process (sec 3.1).
namespace Fluid::Solver::OpenFoam
{

struct CaseOptions
{
    int processes = 1;  // above 1: decomposed, run under MPI
};

// What the case's steps are called on, in order.
struct Step
{
    std::string name;     // the log is log.<name>
    std::string command;  // run in the case's folder
    bool solver = false;  // the step that makes the frames
};

struct Case
{
    // Each file by its path in the case's folder, and its text.
    std::map<std::string, std::string> files;
    std::vector<Step> steps;
    // The background mesh's cells along x, y and z.
    std::array<long, 3> cells {0, 0, 0};
    // A point of the fluid that is in no cell's face: where the mesher is
    // told the mesh is.
    Vec3 inside {0.0, 0.0, 0.0};
};

// The case of a model. why says what is wrong when it returns false: no
// fluid, no space, a resolution that is not positive.
FluidExport bool makeCase(const Model& model, const CaseOptions& options, Case& made, std::string& why);

// The check an install is put to (docs/FluidSimulation.md sec 14.2): a
// laminar flow along a square duct 10 mm across and 100 long, laid along
// one of the model's axes -- 0 x, 1 y, 2 z -- with the pressure probed at
// two points of its axis 50 mm apart. The flow loses the same pressure
// whichever axis it is along; conda-forge's build of v2412 loses a quarter
// more along x.
FluidExport Case ductCase(int axis);

// What a duct's flow lost between its two probes, from the probes' table
// (m^2/s^2): its last row. NaN when the table has none.
FluidExport double ductLoss(const std::string& table);

// Whether two ducts' losses are one flow's: both real and above nothing,
// and within a hundredth of each other.
FluidExport bool sameLoss(double first, double second);

// A surface as an ASCII STL, one solid of that name.
FluidExport std::string stlOf(const Surface& surface, const std::string& name);

// The free surface OpenFOAM wrote as a legacy ASCII VTK POLYDATA: its
// points and triangles (a polygon of more corners is fanned). False when
// the text is not one, or is cut short -- a file still being written.
FluidExport bool readSurfaceVtk(const std::string& text, Frame& frame);

// The rows of a function object's .dat file -- probes' or
// interfaceHeight's: each row the time and the values after it; lines
// starting with # are left out.
FluidExport std::vector<std::vector<double>> readTable(const std::string& text);

// The row of a table at a time, to within tolerance; nullptr when there is
// none.
FluidExport const std::vector<double>* rowAt(const std::vector<std::vector<double>>& table,
                                             double time,
                                             double tolerance);

// The times of the solver's log so far ("Time = ..."): the last one.
// Negative when there is none.
FluidExport double lastTimeOf(const std::string& log);

}  // namespace Fluid::Solver::OpenFoam
