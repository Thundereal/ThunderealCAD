target_include_directories(PartDesign_tests_run PUBLIC
    ${OCC_INCLUDE_DIR}
    ${Python3_INCLUDE_DIRS}
    ${XercesC_INCLUDE_DIRS}
    # FeatureHole.h includes json_fwd.hpp as PartDesign's own sources do
    ${CMAKE_SOURCE_DIR}/src/3rdParty/json/single_include/nlohmann/
)

# As for Surface_tests_run: PartDesign's directory-scoped link_directories()
# does not reach a target declared in tests/
target_link_directories(PartDesign_tests_run PRIVATE
    ${OCC_LIBRARY_DIR}
)

target_link_libraries(PartDesign_tests_run
    gtest_main
    ${Google_Tests_LIBS}
    PartDesign
    TKMath
    TKernel
)

add_subdirectory(App)
