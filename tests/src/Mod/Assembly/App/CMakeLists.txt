target_sources(
    Assembly_tests_run
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/AssemblyObject.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/FormulaEvaluator.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/SolverRegistry.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/OndselAdapter.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/ReferenceBackend.cpp
)
# The Chrono backend's tests, when it is built (BUILD_ASSEMBLY_CHRONO).
if (BUILD_ASSEMBLY_CHRONO)
    target_sources(Assembly_tests_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ChronoBackend.cpp)
endif()
