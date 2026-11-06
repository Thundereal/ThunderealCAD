# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Project Chrono's core library (the submodule src/3rdParty/chrono, release
# 10.0.0, BSD-3) as a static library, for the Assembly's Chrono solver backend
# (docs/AssemblySolverInterface.md sec 6.15). Included by
# src/3rdParty/CMakeLists.txt when BUILD_ASSEMBLY_CHRONO is on.
#
# Chrono is built by its own CMake, as an external project: its lists address
# their own tree through CMAKE_SOURCE_DIR, so they cannot be added to this
# project with add_subdirectory(). Only the core target is built, with every
# optional module, demo, test, OpenMP, SIMD, CUDA, MPI and Thrust off. One
# cost of using Chrono's own top-level CMakeLists: its first configure copies
# Chrono's data directory (about 750 MB) into the nested build tree.
#
# What this defines for the Assembly module:
#   ChronoCore    an imported static library (Chrono_core plus its system
#                 libraries), built before anything that links it;
#   its properties CHRONO_INCLUDE_DIRS and CHRONO_DEFINITIONS, to be applied
#                 to the sources that include Chrono's headers only (Chrono's
#                 include root has a directory named "chrono", which must not
#                 shadow the standard header for every other source).
#   ChronoModal   Chrono's modal module (chrono_modal: modal reduction, for
#                 the flexible bodies, docs/AssemblyDynamics.md D4b), an
#                 imported static library, when Spectra's headers are found
#                 (the submodule src/3rdParty/spectra; CHRONO_SPECTRA_INCLUDE_DIR,
#                 the directory holding Spectra/); not defined without them.
#
# The nested build runs the build tool's default number of jobs unless
# CHRONO_BUILD_JOBS says otherwise.

include(ExternalProject)

set(CHRONO_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/chrono")
if (NOT EXISTS "${CHRONO_SOURCE_DIR}/CMakeLists.txt")
    message(SEND_ERROR
        "BUILD_ASSEMBLY_CHRONO is on, but the chrono submodule is not available. Run\n"
        "  git submodule update --init src/3rdParty/chrono\n"
        "or configure with -DBUILD_ASSEMBLY_CHRONO=OFF.")
    return()
endif()

set(CHRONO_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/chrono")
set(CHRONO_CORE_LIBRARY
    "${CHRONO_BINARY_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}Chrono_core${CMAKE_STATIC_LIBRARY_SUFFIX}")

set(CHRONO_BUILD_JOBS "" CACHE STRING
    "Parallel jobs of the nested Chrono build (empty: the build tool's default)")
set(_chrono_jobs)
if (CHRONO_BUILD_JOBS)
    set(_chrono_jobs --parallel ${CHRONO_BUILD_JOBS})
endif()

set(_chrono_eigen)
if (Eigen3_DIR)
    list(APPEND _chrono_eigen -DEigen3_DIR:PATH=${Eigen3_DIR})
endif()
if (EIGEN3_INCLUDE_DIR)
    list(APPEND _chrono_eigen -DEIGEN3_INCLUDE_DIR:PATH=${EIGEN3_INCLUDE_DIR})
endif()

# The modal module, with Spectra (header only): the submodule
# src/3rdParty/spectra, Spectra's develop branch -- Chrono's modal module
# takes its Krylov-Schur solvers, which no release has (1.2.0 neither)
find_path(CHRONO_SPECTRA_INCLUDE_DIR Spectra/KrylovSchurGEigsSolver.h
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/spectra/include" NO_DEFAULT_PATH
    DOC "The directory holding Spectra/, for Chrono's modal module")
set(CHRONO_MODAL_LIBRARY
    "${CHRONO_BINARY_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}Chrono_modal${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_chrono_modal -DCH_ENABLE_MODULE_MODAL:BOOL=OFF)
set(_chrono_targets Chrono_core)
set(_chrono_products "${CHRONO_CORE_LIBRARY}")
if (CHRONO_SPECTRA_INCLUDE_DIR)
    set(_chrono_modal
        -DCH_ENABLE_MODULE_MODAL:BOOL=ON
        -Dspectra_INCLUDE_DIR:PATH=${CHRONO_SPECTRA_INCLUDE_DIR})
    list(APPEND _chrono_targets Chrono_modal)
    list(APPEND _chrono_products "${CHRONO_MODAL_LIBRARY}")
    message(STATUS "Chrono's modal module, with Spectra in ${CHRONO_SPECTRA_INCLUDE_DIR}")
else()
    message(STATUS "Chrono's modal module is not built: the spectra submodule is not "
                   "available (git submodule update --init src/3rdParty/spectra); the "
                   "Assembly has no flexible bodies")
endif()

ExternalProject_Add(ChronoCoreBuild
    SOURCE_DIR "${CHRONO_SOURCE_DIR}"
    BINARY_DIR "${CHRONO_BINARY_DIR}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_CACHE_ARGS
        -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
        -DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS}
        -DCMAKE_CXX_FLAGS:STRING=${CMAKE_CXX_FLAGS}
        -DCMAKE_CXX_FLAGS_RELEASE:STRING=${CMAKE_CXX_FLAGS_RELEASE}
        -DCMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=${CMAKE_CXX_FLAGS_RELWITHDEBINFO}
        -DCMAKE_CXX_FLAGS_DEBUG:STRING=${CMAKE_CXX_FLAGS_DEBUG}
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
        -DBUILD_SHARED_LIBS:BOOL=OFF
        -DBUILD_DEMOS:BOOL=OFF
        -DBUILD_TESTING:BOOL=OFF
        -DBUILD_BENCHMARKING:BOOL=OFF
        -DCH_ENABLE_OPENMP:BOOL=OFF
        -DCH_USE_SIMD:BOOL=OFF
        -DCH_ENABLE_HDF5:BOOL=OFF
        # Bullet in double: its GJK's tolerances are absolute, 1e-6 on
        # squared lengths in float -- a square millimetre in the backend's
        # metres -- and a cylinder lying on a plate was not found until it
        # was 6 mm into it.
        -DCH_USE_BULLET_DOUBLE:BOOL=ON
        -DCMAKE_CUDA_COMPILER:FILEPATH=CMAKE_CUDA_COMPILER-NOTFOUND
        -DCMAKE_DISABLE_FIND_PACKAGE_MPI:BOOL=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Thrust:BOOL=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_OpenMP:BOOL=ON
        ${_chrono_eigen}
        ${_chrono_modal}
    BUILD_COMMAND
        ${CMAKE_COMMAND} --build <BINARY_DIR> --target ${_chrono_targets} --config $<CONFIG> ${_chrono_jobs}
    INSTALL_COMMAND ""
    TEST_COMMAND ""
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS ${_chrono_products}
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
)

# ChConfig.h and ChVersion.h are generated into the nested build tree, which an
# imported target's include directories must name before it exists.
file(MAKE_DIRECTORY "${CHRONO_BINARY_DIR}")

add_library(ChronoCore STATIC IMPORTED GLOBAL)
set_target_properties(ChronoCore PROPERTIES
    IMPORTED_LOCATION "${CHRONO_CORE_LIBRARY}"
    CHRONO_INCLUDE_DIRS
        "${CHRONO_SOURCE_DIR}/src;${CHRONO_BINARY_DIR};${CHRONO_SOURCE_DIR}/src/chrono/collision/bullet;${CHRONO_SOURCE_DIR}/src/chrono/collision/gimpact"
    # What Chrono_core's own target makes PUBLIC, less the warning options,
    # and the PRIVATE precision of its Bullet: a source that reaches Bullet's
    # types (the Chrono backend's contact, polyhedral features and clipping)
    # sees them as Chrono_core built them, not cbtScalar as float.
    CHRONO_DEFINITIONS "CH_STATIC;CH_IGNORE_DEPRECATED;BT_USE_DOUBLE_PRECISION"
)
if (WIN32)
    set_property(TARGET ChronoCore PROPERTY INTERFACE_LINK_LIBRARIES Ws2_32)
endif()
add_dependencies(ChronoCore ChronoCoreBuild)

if (CHRONO_SPECTRA_INCLUDE_DIR)
    # Its headers are Chrono's; Spectra is in its sources only
    add_library(ChronoModal STATIC IMPORTED GLOBAL)
    set_target_properties(ChronoModal PROPERTIES
        IMPORTED_LOCATION "${CHRONO_MODAL_LIBRARY}"
        INTERFACE_LINK_LIBRARIES ChronoCore)
    add_dependencies(ChronoModal ChronoCoreBuild)
endif()
