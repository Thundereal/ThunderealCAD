# SPDX-License-Identifier: LGPL-2.1-or-later

# The fluid simulation workbench (docs/FluidSimulation.md).
add_subdirectory(App)
if(BUILD_FLUID_SPLISHSPLASH)
    add_subdirectory(Runner)
endif()

set(Fluid_Scripts
    Init.py
    FluidOpenFoam.py
    FluidPlayback.py
    TestFluidWorkbench.py
)

# The workbench: its commands, its panel and its icons, which the scripts
# find beside themselves (FluidPlayback.iconPath()).
if(BUILD_GUI)
    list(APPEND Fluid_Scripts InitGui.py FluidCommands.py)
endif(BUILD_GUI)

SET(FluidIcons_SRCS
    Resources/icons/FluidWorkbench.svg
    Resources/icons/Fluid_CreateStudy.svg
    Resources/icons/Fluid_AddFluid.svg
    Resources/icons/Fluid_AddBody.svg
    Resources/icons/Fluid_AddMonitor.svg
    Resources/icons/Fluid_AddPressureMonitor.svg
    Resources/icons/Fluid_RunStudy.svg
)

SET(FluidTests_SRCS
    FluidTests/__init__.py
    FluidTests/TestStudy.py
    FluidTests/TestPlayback.py
    FluidTests/DamBreak.py
    FluidTests/StillWater.py
    FluidTests/Floating.py
    FluidTests/TestOpenFoam.py
    FluidTests/TestSplish.py
    FluidTests/TestDownload.py
)

SET(all_files
    ${Fluid_Scripts}
    ${FluidTests_SRCS}
    ${FluidIcons_SRCS}
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

INSTALL(
    FILES
    ${FluidIcons_SRCS}
    DESTINATION
    Mod/Fluid/Resources/icons
)
