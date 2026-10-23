# SPDX-License-Identifier: LGPL-2.1-or-later

# Upstream's tests/src/Mod/Assembly, in this fork's style (as Points does):
# the Ondsel adapter's test includes the solver's headers, hence its source
# directory (AssemblyObject.h no longer does).
target_include_directories(Assembly_tests_run PUBLIC
    ${EIGEN3_INCLUDE_DIR}
    ${OCC_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${Python3_INCLUDE_DIRS}
    ${XercesC_INCLUDE_DIRS}
)
if (NOT FREECAD_USE_EXTERNAL_ONDSELSOLVER)
    target_include_directories(Assembly_tests_run PUBLIC
        ${CMAKE_SOURCE_DIR}/src/3rdParty/OndselSolver
    )
endif ()

target_link_libraries(Assembly_tests_run
    gtest_main
    ${Google_Tests_LIBS}
    Assembly
)

add_subdirectory(App)
