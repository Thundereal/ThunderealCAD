# SPDX-License-Identifier: LGPL-2.1-or-later

# Upstream carries these on the targets it links against; this fork still sets
# them per module, the way every other Mod/*/App does.
include_directories(
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${Boost_INCLUDE_DIRS}
    ${OCC_INCLUDE_DIR}
    ${ZLIB_INCLUDE_DIR}
    ${PYTHON_INCLUDE_DIRS}
    ${XercesC_INCLUDE_DIRS}
    ${EIGEN3_INCLUDE_DIR}
)
link_directories(${OCC_LIBRARY_DIR})

set(Assembly_LIBS
    Part
    Materials
    PartDesign
    Spreadsheet
    FreeCADApp
    OndselSolver
)

generate_from_py(AssemblyObject)
generate_from_py(AssemblyLink)
generate_from_py(BomObject)
generate_from_py(BomGroup)
generate_from_py(JointGroup)
generate_from_py(ViewGroup)
generate_from_py(SimulationGroup)
generate_from_py(SnapshotGroup)
generate_from_py(Joint)
generate_from_py(SpringDamper)
generate_from_py(AssemblyGroup)


SET(Python_SRCS
    AssemblyObject.pyi
    AssemblyObjectPyImp.cpp
    AssemblyLink.pyi
    AssemblyLinkPyImp.cpp
    BomObject.pyi
    BomObjectPyImp.cpp
    BomGroup.pyi
    BomGroupPyImp.cpp
    JointGroup.pyi
    JointGroupPyImp.cpp
    ViewGroup.pyi
    ViewGroupPyImp.cpp
    SimulationGroup.pyi
    SimulationGroupPyImp.cpp
    SnapshotGroup.pyi
    SnapshotGroupPyImp.cpp
    Joint.pyi
    JointPyImp.cpp
    SpringDamper.pyi
    SpringDamperPyImp.cpp
    AssemblyGroup.pyi
    AssemblyGroupPyImp.cpp
)
SOURCE_GROUP("Python" FILES ${Python_SRCS})

SET(Module_SRCS
    AppAssembly.cpp
    AppAssemblyPy.cpp
    PreCompiled.h
)
SOURCE_GROUP("Module" FILES ${Module_SRCS})

# The solver-neutral interface (docs/AssemblySolverInterface.md).
SET(Solver_SRCS
    Solver/Backend.cpp
    Solver/Backend.h
    Solver/Curve.cpp
    Solver/Curve.h
    Solver/Formula.cpp
    Solver/Formula.h
    Solver/JointEquations.cpp
    Solver/JointEquations.h
    Solver/Model.h
    Solver/Model.cpp
    Solver/OndselBackend.cpp
    Solver/OndselBackend.h
    Solver/ReferenceBackend.cpp
    Solver/ReferenceBackend.h
    Solver/Registry.cpp
    Solver/Registry.h
    Solver/RigidGroups.cpp
    Solver/RigidGroups.h
    Solver/Units.h
)
# The Chrono backend (sec 6.15), only with BUILD_ASSEMBLY_CHRONO: Chrono's
# headers and defines go to its source alone (src/3rdParty/ChronoCore.cmake).
if (BUILD_ASSEMBLY_CHRONO)
    list(APPEND Solver_SRCS Solver/ChronoBackend.cpp Solver/ChronoBackend.h)
endif()

SOURCE_GROUP("Solver" FILES ${Solver_SRCS})

SET(Assembly_SRCS
    AssemblyParams.cpp
    AssemblyParams.h
    AssemblyObject.cpp
    AssemblyObject.h
    AssemblyLink.cpp
    AssemblyLink.h
    AssemblyUtils.cpp
    AssemblyUtils.h
    BomObject.cpp
    BomObject.h
    CamJoint.cpp
    CamJoint.h
    ElementGeometry.h
    Groups.cpp
    Groups.h
    AssemblyGroup.cpp
    AssemblyGroup.h
    Grounding.cpp
    Grounding.h
    ContactGeometry.cpp
    ContactGeometry.h
    Interference.cpp
    Interference.h
    Joint.cpp
    Joint.h
    MassProperties.cpp
    MassProperties.h
    Physics.cpp
    Physics.h
    Simulation.cpp
    Simulation.h
    Views.cpp
    Views.h
    ${Module_SRCS}
    ${Solver_SRCS}
    ${Python_SRCS}
)

add_library(Assembly SHARED ${Assembly_SRCS})
target_include_directories(
    Assembly
    PRIVATE
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)
if (NOT FREECAD_USE_EXTERNAL_ONDSELSOLVER)
    target_include_directories(Assembly PRIVATE ${CMAKE_SOURCE_DIR}/src/3rdParty/OndselSolver)
endif ()
target_link_libraries(Assembly ${Assembly_LIBS})
if (BUILD_ASSEMBLY_CHRONO)
    target_link_libraries(Assembly ChronoCore)
    target_compile_definitions(Assembly PRIVATE ASSEMBLY_WITH_CHRONO)
    get_target_property(CHRONO_INCLUDE_DIRS ChronoCore CHRONO_INCLUDE_DIRS)
    get_target_property(CHRONO_DEFINITIONS ChronoCore CHRONO_DEFINITIONS)
    set_source_files_properties(Solver/ChronoBackend.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${CHRONO_INCLUDE_DIRS}"
        COMPILE_DEFINITIONS "${CHRONO_DEFINITIONS}"
    )
endif()
if (FREECAD_WARN_ERROR)
    target_compile_warn_error(Assembly)
endif()

if(FREECAD_USE_PCH)
    target_precompile_headers(Assembly PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:"${CMAKE_CURRENT_LIST_DIR}/PreCompiled.h">
    )
endif(FREECAD_USE_PCH)

SET_BIN_DIR(Assembly AssemblyApp /Mod/Assembly)
SET_PYTHON_PREFIX_SUFFIX(Assembly)

INSTALL(TARGETS Assembly DESTINATION ${CMAKE_INSTALL_LIBDIR})
