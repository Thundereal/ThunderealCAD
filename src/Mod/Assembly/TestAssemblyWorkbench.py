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
    TestDistanceRoleMappings,
    TestElementRecognition,
    TestDistanceRefusal,
    TestSolveStatus,
    TestDrag,
    TestLimits,
    TestCoupledJoints,
)
from AssemblyTests.TestSolverGate import (
    TestSolverGateJoints,
    TestSolverGateDistance,
    TestSolverGateDistanceMapped,
    TestSolverGateRoleMappings,
    TestSolverGateLimits,
    TestSolverGateOrder,
    TestSolverGateSimulation,
    TestSolverGateBench,
    TestSolverGateRigid,
    TestSolverGateCam,
    BACKEND_GATE_CLASSES,
)
from AssemblyTests.TestCamJoint import TestCamJoint, TestCamJointTyped, TestSimulationMotions
from AssemblyTests.TestGuiCommands import TestGuiCommands
from AssemblyTests.TestAsm3Bundle import TestAsm3Bundle
from AssemblyTests.TestElementJoint import TestElementJoint
from AssemblyTests.TestMigrateJoints import TestMigrateJoints
from AssemblyTests.TestMigrateTwins import TestMigrateTwins
from AssemblyTests.TestAssemblyGroups import TestAssemblyGroups
from AssemblyTests.TestInterference import (
    TestDragCollision,
    TestInterference,
    TestInterferenceOverFrames,
)
from AssemblyTests.TestImportAsm3 import TestImportAsm3
from AssemblyTests.TestMassProperties import TestMassProperties
from AssemblyTests.TestDynamics import TestDynamics
from AssemblyTests.TestContact import TestContact
from AssemblyTests.TestSpringEnds import (
    TestBushingEnd,
    TestLoadEnd,
    TestSpringEnds,
    TestSpringEndOnAnUnloadedChild,
)
from AssemblyTests.TestMultiplyJoint import TestMultiplyJoint
from AssemblyTests.TestFreeze import TestFreeze
from AssemblyTests.TestAssemblyExport import TestAssemblyExport
from AssemblyTests.TestUnloadedChild import TestUnloadedChild
from AssemblyTests.TestReplaceComponent import TestReplaceComponent
from AssemblyTests.TestFlexibleSubAssembly import TestFlexibleSubAssembly

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
True if TestDistanceRoleMappings else False
True if TestElementRecognition else False
True if TestDistanceRefusal else False
True if TestSolveStatus else False
True if TestDrag else False
True if TestLimits else False
True if TestCoupledJoints else False
True if TestSolverGateJoints else False
True if TestSolverGateDistance else False
True if TestSolverGateDistanceMapped else False
True if TestSolverGateRoleMappings else False
True if TestSolverGateLimits else False
True if TestSolverGateOrder else False
True if TestSolverGateSimulation else False
True if TestSolverGateBench else False
True if TestSolverGateRigid else False
True if TestSolverGateCam else False
True if TestCamJoint else False
True if TestCamJointTyped else False
True if TestSimulationMotions else False
True if TestGuiCommands else False
True if TestAsm3Bundle else False
True if TestElementJoint else False
True if TestMigrateJoints else False
True if TestMigrateTwins else False
True if TestAssemblyGroups else False
True if TestInterference else False
True if TestInterferenceOverFrames else False
True if TestDragCollision else False
True if TestImportAsm3 else False
True if TestMassProperties else False
True if TestDynamics else False
True if TestContact else False
True if TestSpringEnds else False
True if TestLoadEnd else False
True if TestBushingEnd else False
True if TestSpringEndOnAnUnloadedChild else False
True if TestMultiplyJoint else False
True if TestFreeze else False
True if TestAssemblyExport else False
True if TestUnloadedChild else False
True if TestReplaceComponent else False
True if TestFlexibleSubAssembly else False

# The gate once more per registered backend other than OndselSolver
# (TestSolverGateJointsReference, ...), compared with OndselSolver's records.
globals().update(BACKEND_GATE_CLASSES)
