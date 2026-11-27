# SPDX-License-Identifier: LGPL-2.1-or-later

target_include_directories(Fluid_tests_run PUBLIC
    ${EIGEN3_INCLUDE_DIR}
    ${OCC_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${Python3_INCLUDE_DIRS}
    ${XercesC_INCLUDE_DIRS}
)

# Part (which Fluid links) gets these through a directory-scoped
# link_directories() in src/Mod/Part/App, which does not reach a target
# declared in tests/.
target_link_directories(Fluid_tests_run PRIVATE
    ${OCC_LIBRARY_DIR}
)

target_link_libraries(Fluid_tests_run
    gtest_main
    ${Google_Tests_LIBS}
    Fluid
)

add_subdirectory(App)
