# SPDX-License-Identifier: LGPL-2.1-or-later

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
    ${QtCore_INCLUDE_DIRS}
    # ResultsArchive's index.json
    ${CMAKE_SOURCE_DIR}/src/3rdParty/json/single_include/nlohmann/
)
link_directories(${OCC_LIBRARY_DIR})

set(Fluid_LIBS
    Part
    FreeCADApp
    # QProcess: the solvers that are separate programs
    ${QtCore_LIBRARIES}
)

# A study whose parts a mechanism moves runs with the Assembly's simulation
# (docs/FluidSimulation.md sec 12.9): without that module it says it cannot.
if(BUILD_ASSEMBLY)
    list(APPEND Fluid_LIBS Assembly)
    add_definitions(-DFLUID_WITH_ASSEMBLY)
endif()

generate_from_py(Study)

SET(Python_SRCS
    Study.pyi
    StudyPyImp.cpp
)
SOURCE_GROUP("Python" FILES ${Python_SRCS})

SET(Module_SRCS
    AppFluid.cpp
    AppFluidPy.cpp
)
SOURCE_GROUP("Module" FILES ${Module_SRCS})

# The solver-neutral interface (docs/FluidSimulation.md sec 11).
SET(Solver_SRCS
    Solver/Backend.h
    Solver/FreeFallBackend.cpp
    Solver/FreeFallBackend.h
    Solver/OpenFoamBackend.cpp
    Solver/OpenFoamBackend.h
    Solver/OpenFoamCase.cpp
    Solver/OpenFoamCase.h
    Solver/SplishBackend.cpp
    Solver/SplishBackend.h
    Solver/SplishCase.cpp
    Solver/SplishCase.h
    Solver/WallExchange.cpp
    Solver/WallExchange.h
    Solver/Model.cpp
    Solver/Model.h
    Solver/Registry.cpp
    Solver/Registry.h
)
SOURCE_GROUP("Solver" FILES ${Solver_SRCS})

SET(Fluid_SRCS
    ParticleSurface.cpp
    ParticleSurface.h
    ResultsArchive.cpp
    ResultsArchive.h
    Section.cpp
    Section.h
    Study.cpp
    Study.h
    ${Module_SRCS}
    ${Solver_SRCS}
    ${Python_SRCS}
)

add_library(Fluid SHARED ${Fluid_SRCS})
target_include_directories(
    Fluid
    PRIVATE
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(Fluid ${Fluid_LIBS})
if (FREECAD_WARN_ERROR)
    target_compile_warn_error(Fluid)
endif()

SET_BIN_DIR(Fluid FluidApp /Mod/Fluid)
SET_PYTHON_PREFIX_SUFFIX(Fluid)

INSTALL(TARGETS Fluid DESTINATION ${CMAKE_INSTALL_LIBDIR})
