// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#pragma once

#include <string>
#include <vector>

#include <Base/Placement.h>
#include <Mod/Assembly/AssemblyGlobal.h>

#include "Solver/Backend.h"

namespace Assembly
{

// A run's results as its study keeps them in the document
// (docs/AssemblyDynamics.md sec 8.4): a zip of index.json -- the schema, the
// units, the bodies and the parts each moves, the channels and the events --
// and one .npy per block, which numpy.load reads as it is:
//   times.npy   (frames,)              s
//   poses.npy   (frames, bodies, 7)    x, y, z (mm), then the rotation's
//                                      quaternion x, y, z, w
//   values.npy  (frames, width)        the channels' components in order, in
//                                      the model's units; kept with All only
// Each block is float64, little endian, deflated in the zip.
struct ArchivedPart
{
    std::string name;  // its full name, "Document#Object"
    Solver::Id body = Solver::NoId;
    Base::Placement offset;  // within the body it moves with
};

struct SimulationArchive
{
    Solver::Trajectory frames;
    std::vector<ArchivedPart> parts;
    bool values = true;  // the channels and their values kept (All), or not (Motion)
};

// Writes the archive of frames and the parts they move to path; without
// values, the channels and values.npy are left out. Throws
// Base::FileException when it cannot.
AssemblyExport void writeSimulationArchive(const std::string& path,
                                           const Solver::Trajectory& frames,
                                           const std::vector<ArchivedPart>& parts,
                                           bool values);

// Reads one written by writeSimulationArchive(). Throws Base::FileException
// when the file cannot be read, Base::RuntimeError when it is not such an
// archive or a later schema's.
AssemblyExport SimulationArchive readSimulationArchive(const std::string& path);

}  // namespace Assembly
