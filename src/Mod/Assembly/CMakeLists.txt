# SPDX-License-Identifier: LGPL-2.1-or-later

# Only App/Solver/OndselBackend.cpp includes the solver's headers:
# App/CMakeLists.txt gives the App target the in-tree include path. Gui does
# without.
if (FREECAD_USE_EXTERNAL_ONDSELSOLVER)
    check_include_file_cxx(OndselSolver/enum.h HAVE_ONDSELSOLVER_H)
    if (NOT HAVE_ONDSELSOLVER_H)
        message(FATAL_ERROR "FREECAD_USE_EXTERNAL_ONDSELSOLVER is set but the required system includes are not available")
    endif ()
endif ()

add_subdirectory(App)

if(BUILD_GUI)
    add_subdirectory(Gui)
endif(BUILD_GUI)

set(Assembly_Scripts
    Init.py
    AssemblyPyParams.py
    CommandCreateAssembly.py
    CommandCreateBom.py
    CommandInsertLink.py
    CommandInsertNewPart.py
    CommandSolveAssembly.py
    CommandCheckInterference.py
    CommandCreateJoint.py
    CommandCreateView.py
    CommandCreateSimulation.py
    CommandMassProperties.py
    CommandCreateSnapshot.py
    CommandExportASMT.py
    CommandConvertAsm3.py
    TestAssemblyWorkbench.py
    JointObject.py
    JointCam.py
    Preferences.py
    AssemblyImport.py
    AssemblyAsm3Bundle.py
    AssemblyMigrate.py
    AssemblyImportAsm3.py
    SoSwitchMarker.py
    UtilsAssembly.py
)

if(BUILD_GUI)
    list (APPEND Assembly_Scripts InitGui.py)
endif(BUILD_GUI)

INSTALL(
    FILES
    ${Assembly_Scripts}
    DESTINATION
    Mod/Assembly
)

SET(AssemblyScripts_SRCS
    Assembly/__init__.py
)


SET(AssemblyTests_SRCS
    AssemblyTests/__init__.py
    AssemblyTests/TestCore.py
    AssemblyTests/TestInterference.py
    AssemblyTests/TestMassProperties.py
    AssemblyTests/TestCommandCreateView.py
    AssemblyTests/TestCommandInsertLink.py
    AssemblyTests/TestSimulationExport.py
    AssemblyTests/TestSettings.py
    AssemblyTests/TestJoints.py
    AssemblyTests/TestSolverGate.py
    AssemblyTests/TestCamJoint.py
    AssemblyTests/SolverGateReference.json
    AssemblyTests/TestGuiCommands.py
    AssemblyTests/TestAsm3Bundle.py
    AssemblyTests/TestElementJoint.py
    AssemblyTests/TestMigrateJoints.py
    AssemblyTests/TestMigrateTwins.py
    AssemblyTests/TestAssemblyGroups.py
    AssemblyTests/TestImportAsm3.py
    AssemblyTests/TestMultiplyJoint.py
    AssemblyTests/TestFreeze.py
    AssemblyTests/TestAssemblyExport.py
    AssemblyTests/TestUnloadedChild.py
    AssemblyTests/TestReplaceComponent.py
    AssemblyTests/TestFlexibleSubAssembly.py
    AssemblyTests/mocks/__init__.py
    AssemblyTests/mocks/MockGui.py
)


# Assembly3, bundled to migrate its documents (AssemblyAsm3Bundle.py)
file(GLOB_RECURSE Assembly3_SRCS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    Assembly3/*.py Assembly3/*.svg Assembly3/*.md Assembly3/LICENSE)

SET(all_files
    ${AssemblyTests_SRCS}
    ${AssemblyScripts_SRCS}
    ${Assembly3_SRCS}
)

ADD_CUSTOM_TARGET(AssemblyScripts ALL
    SOURCES ${all_files}
)

SET(test_files
    ${Assembly_Scripts}
    ${AssemblyTests_SRCS}
)

ADD_CUSTOM_TARGET(AssemblyTests ALL
    SOURCES ${test_files}
)

# Target AssemblyScripts handles copying AssemblyTests_SRCS and AssemblyScripts_SRCS
fc_copy_sources(AssemblyScripts "${CMAKE_BINARY_DIR}/Mod/Assembly" ${all_files})

# Target AssemblyTests should only handle copying the remaining Assembly_Scripts
# to avoid racing with AssemblyScripts over the same files.
fc_copy_sources(AssemblyTests "${CMAKE_BINARY_DIR}/Mod/Assembly" ${Assembly_Scripts})

# Ensure AssemblyScripts finishes its directory creation and copying before
# AssemblyTests begins.
add_dependencies(AssemblyTests AssemblyScripts)

INSTALL(
    FILES
        ${AssemblyTests_SRCS}
    DESTINATION
        Mod/Assembly/AssemblyTests
)
INSTALL(
    FILES
        ${AssemblyScripts_SRCS}
    DESTINATION
        Mod/Assembly/Assembly
)
INSTALL(
    DIRECTORY
        Assembly3
    DESTINATION
        Mod/Assembly
)
