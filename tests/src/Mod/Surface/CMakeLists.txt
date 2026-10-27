target_include_directories(Surface_tests_run PUBLIC
    ${OCC_INCLUDE_DIR}
    ${Python3_INCLUDE_DIRS}
    ${XercesC_INCLUDE_DIRS}
)

# As for Sketcher_tests_run: Surface's directory-scoped link_directories()
# does not reach a target declared in tests/
target_link_directories(Surface_tests_run PRIVATE
    ${OCC_LIBRARY_DIR}
)

target_link_libraries(Surface_tests_run
    gtest_main
    ${Google_Tests_LIBS}
    Surface
    TKGeomAlgo
    TKGeomBase
    TKG3d
    TKMath
    TKernel
)

add_subdirectory(App)
