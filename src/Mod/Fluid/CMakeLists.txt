# SPDX-License-Identifier: LGPL-2.1-or-later

# The fluid simulation workbench (docs/FluidSimulation.md).
add_subdirectory(App)

set(Fluid_Scripts
    Init.py
    TestFluidWorkbench.py
)

SET(FluidTests_SRCS
    FluidTests/__init__.py
    FluidTests/TestStudy.py
)

SET(all_files
    ${Fluid_Scripts}
    ${FluidTests_SRCS}
)

ADD_CUSTOM_TARGET(FluidScripts ALL
    SOURCES ${all_files}
)

fc_copy_sources(FluidScripts "${CMAKE_BINARY_DIR}/Mod/Fluid" ${all_files})

INSTALL(
    FILES
    ${Fluid_Scripts}
    DESTINATION
    Mod/Fluid
)

INSTALL(
    FILES
    ${FluidTests_SRCS}
    DESTINATION
    Mod/Fluid/FluidTests
)
