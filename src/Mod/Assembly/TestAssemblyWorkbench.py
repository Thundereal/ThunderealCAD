# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# **************************************************************************/

import TestApp

from AssemblyTests.TestCore import TestCore, TestSolverBackend, TestSubNames
from AssemblyTests.TestCommandInsertLink import TestCommandInsertLink
from AssemblyTests.TestCommandCreateView import TestCommandCreateView
from AssemblyTests.TestSimulationExport import TestSimulationExport
from AssemblyTests.TestSettings import TestSettings
from AssemblyTests.TestJoints import (
    TestJointTypes,
    TestDistanceJoint,
    TestDistanceMapped,
    TestSolveStatus,
    TestDrag,
    TestLimits,
    TestCoupledJoints,
)
from AssemblyTests.TestSolverGate import (
    TestSolverGateJoints,
    TestSolverGateDistance,
    TestSolverGateDistanceMapped,
    TestSolverGateLimits,
    TestSolverGateOrder,
    TestSolverGateSimulation,
    TestSolverGateBench,
    TestSolverGateRigid,
)
from AssemblyTests.TestGuiCommands import TestGuiCommands

# Use the modules so that code checkers don't complain (flake8)
True if TestCore else False
True if TestSolverBackend else False
True if TestSubNames else False
True if TestCommandInsertLink else False
True if TestCommandCreateView else False
True if TestSimulationExport else False
True if TestSettings else False
True if TestJointTypes else False
True if TestDistanceJoint else False
True if TestDistanceMapped else False
True if TestSolveStatus else False
True if TestDrag else False
True if TestLimits else False
True if TestCoupledJoints else False
True if TestSolverGateJoints else False
True if TestSolverGateDistance else False
True if TestSolverGateDistanceMapped else False
True if TestSolverGateLimits else False
True if TestSolverGateOrder else False
True if TestSolverGateSimulation else False
True if TestSolverGateBench else False
True if TestSolverGateRigid else False
True if TestGuiCommands else False
