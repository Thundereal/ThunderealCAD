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

// OndselSolver behind the solver interface (docs/AssemblySolverInterface.md
// sec 6.3 step 2). The adapter turns a Model into an ASMTAssembly with the
// names, order and value strings AssemblyObject used to write directly, so the
// solver gets the same input as before, bit for bit.

#include <memory>
#include <string>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

#include "Backend.h"

namespace MbD
{
class ASMTAssembly;
class ASMTJoint;
class ASMTPart;
}  // namespace MbD

namespace Assembly::Solver
{

namespace Ondsel
{

// The ASMT model of a Model, with the ASMT part of each body.
struct AsmtModel
{
    std::shared_ptr<MbD::ASMTAssembly> assembly;
    std::vector<std::shared_ptr<MbD::ASMTPart>> parts;  // by body id
    std::vector<std::shared_ptr<MbD::ASMTJoint>> joints;  // by joint id
};

// Builds the ASMT model. 'simulation' sets the simulation parameters; without
// it they keep OndselSolver's defaults, as a static solve always had them.
AssemblyExport AsmtModel
toAsmt(const Model& model, const SimulationSpec* simulation = nullptr, bool debug = false);

// The pose OndselSolver holds for a part.
AssemblyExport Base::Placement partPlacement(const std::shared_ptr<MbD::ASMTPart>& part);

// The ASMT text of a limit's bound, as AssemblyObject printed it.
AssemblyExport std::string limitValueString(LimitKind kind, double value);

}  // namespace Ondsel

class AssemblyExport OndselBackend: public Backend
{
public:
    std::string name() const override;
    std::string label() const override;
    Capabilities capabilities() const override;
    std::unique_ptr<Session> createSession() override;
};

// A session on one ASMT model.
class AssemblyExport OndselSession: public Session
{
public:
    // 'dumpFile', when not empty, receives the ASMT text of every model the
    // session is given (load, simulate): the identity gate's view of the input.
    explicit OndselSession(bool debug = false, std::string dumpFile = {});
    ~OndselSession() override;

    Status load(const Model& model) override;
    // Takes new drag weights on the live system; anything else needs a load.
    Status update(const Model& model) override;
    Result solve(const SolveOptions& options) override;
    Status beginDrag(const std::vector<Id>& bodies, const DragOptions& options) override;
    Result dragStep(const std::vector<BodyPose>& targets) override;
    Result endDrag() override;
    std::vector<BodyPose> poses() const override;
    Status setPoses(const std::vector<BodyPose>& poses) override;
    Diagnostics analyse() override;
    Status simulate(const SimulationSpec& spec, TrajectorySink& sink) override;
    Status exportNative(const std::string& path) override;

private:
    // Reads every body's pose back from the solver; returns the bodies that moved.
    std::vector<BodyPose> refreshPoses();
    void dump() const;

    bool debug;
    std::string dumpFile;
    Model model;
    Ondsel::AsmtModel current;
    std::vector<Base::Placement> currentPoses;  // by body id
};

}  // namespace Assembly::Solver
