# SPDX-License-Identifier: LGPL-2.1-or-later

# FluidSplish: SPlisHSPlasH (MIT) run on a case the Fluid module wrote
# (docs/FluidSimulation.md sec 12 S3). A program of its own: it is compiled
# as the solver was -- the solver's definitions, its bundled Eigen, its
# vector instructions -- which FreeCAD's own code is not.
#
# The solver comes one of two ways:
#
# 1. The package of splishsplash-feedstock, in the environment or named by
#    SPlisHSPlasH_DIR: its CMake package says how it was compiled.
#
# 2. A tree it was built in by hand:
#      FLUID_SPLISHSPLASH_SOURCE  the solver's source tree
#      FLUID_SPLISHSPLASH_BUILD   where it was built:
#          cmake -DSPH_LIBS_ONLY=ON -DUSE_PYTHON_BINDINGS=OFF
#                -DEIGEN3_INCLUDE_DIR=<source>/extern/eigen ...
#          ninja SPlisHSPlasH
#      FLUID_SPLISHSPLASH_FLAGS   the vector instructions it was built with

set(FLUID_SPLISHSPLASH_SOURCE "" CACHE PATH "SPlisHSPlasH's source tree")
set(FLUID_SPLISHSPLASH_BUILD "" CACHE PATH "The tree SPlisHSPlasH was built in")

add_executable(FluidSplish SplishRunner.cpp)
set_target_properties(FluidSplish PROPERTIES
    CXX_STANDARD 17
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(FLUID_SPLISHSPLASH_SOURCE OR FLUID_SPLISHSPLASH_BUILD)
    if(MSVC)
        set(_fluid_splish_flags "/arch:AVX2")
    else()
        set(_fluid_splish_flags "-march=native -mavx2 -mfma")
    endif()
    set(FLUID_SPLISHSPLASH_FLAGS "${_fluid_splish_flags}" CACHE STRING
        "The vector instructions SPlisHSPlasH was built with")

    set(_fluid_splish_libs)
    foreach(_lib
            lib/SPlisHSPlasH lib/Utilities lib/tinyexpr lib/partio lib/zlib lib/MD5
            extern/install/NeighborhoodSearch/lib/CompactNSearch
            extern/install/Discregrid/lib/Discregrid)
        get_filename_component(_dir "${FLUID_SPLISHSPLASH_BUILD}/${_lib}" DIRECTORY)
        get_filename_component(_name "${_lib}" NAME)
        find_library(FLUID_SPLISH_${_name}_LIB
            NAMES ${_name}
            PATHS "${_dir}"
            NO_DEFAULT_PATH)
        if(NOT FLUID_SPLISH_${_name}_LIB)
            message(FATAL_ERROR
                "BUILD_FLUID_SPLISHSPLASH: ${_name} was not found in ${_dir}. "
                "Set FLUID_SPLISHSPLASH_BUILD to the tree SPlisHSPlasH was built in "
                "(docs/FluidSimulation.md sec 12 S3).")
        endif()
        list(APPEND _fluid_splish_libs "${FLUID_SPLISH_${_name}_LIB}")
    endforeach()
    if(NOT EXISTS "${FLUID_SPLISHSPLASH_SOURCE}/SPlisHSPlasH/Simulation.h")
        message(FATAL_ERROR
            "BUILD_FLUID_SPLISHSPLASH: set FLUID_SPLISHSPLASH_SOURCE to SPlisHSPlasH's "
            "source tree.")
    endif()

    find_package(OpenMP REQUIRED COMPONENTS CXX)
    find_package(Threads REQUIRED)
    # the solver's own Eigen before any other
    target_include_directories(FluidSplish SYSTEM BEFORE PRIVATE
        "${FLUID_SPLISHSPLASH_SOURCE}/extern/eigen"
        "${FLUID_SPLISHSPLASH_SOURCE}"
        "${FLUID_SPLISHSPLASH_BUILD}"
        "${FLUID_SPLISHSPLASH_BUILD}/extern/install/GenericParameters/include"
        "${FLUID_SPLISHSPLASH_BUILD}/extern/install/NeighborhoodSearch/include"
        "${FLUID_SPLISHSPLASH_BUILD}/extern/install/Discregrid/include")
    target_compile_definitions(FluidSplish PRIVATE
        EIGEN_DISABLE_UNALIGNED_ARRAY_ASSERT
        USE_AVX
        USE_CompactNSearch
        USE_PERFORMANCE_OPTIMIZATION
        USE_THIRD_PARTY_METHODS)
    separate_arguments(_fluid_splish_flag_list NATIVE_COMMAND "${FLUID_SPLISHSPLASH_FLAGS}")
    target_compile_options(FluidSplish PRIVATE ${_fluid_splish_flag_list})
    target_link_libraries(FluidSplish PRIVATE
        ${_fluid_splish_libs}
        OpenMP::OpenMP_CXX
        Threads::Threads)
else()
    find_package(SPlisHSPlasH CONFIG)
    if(NOT SPlisHSPlasH_FOUND)
        message(FATAL_ERROR
            "BUILD_FLUID_SPLISHSPLASH: SPlisHSPlasH was not found. Install the package "
            "of splishsplash-feedstock or set SPlisHSPlasH_DIR to its CMake package, or "
            "set FLUID_SPLISHSPLASH_SOURCE and FLUID_SPLISHSPLASH_BUILD to a tree it was "
            "built in (docs/FluidSimulation.md sec 12 S3).")
    endif()
    # The package's include folders are its own Eigen first: BEFORE keeps
    # them ahead of the environment's.
    get_target_property(_fluid_splish_inc SPlisHSPlasH::SPlisHSPlasH INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(FluidSplish SYSTEM BEFORE PRIVATE ${_fluid_splish_inc})
    target_link_libraries(FluidSplish PRIVATE SPlisHSPlasH::SPlisHSPlasH)
endif()

install(TARGETS FluidSplish RUNTIME DESTINATION bin)
