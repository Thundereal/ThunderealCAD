# SPDX-License-Identifier: LGPL-2.1-or-later

target_sources(
    Fluid_tests_run
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/FluidModel.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/OpenFoamCase.cpp
)
