include_directories(
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/src
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${Boost_INCLUDE_DIRS}
  ${EIGEN3_INCLUDE_DIR}
  ${OCC_INCLUDE_DIR}
  ${PYTHON_INCLUDE_DIRS}
  ${ZLIB_INCLUDE_DIR}
  ${XercesC_INCLUDE_DIRS}
  ${FREETYPE_INCLUDE_DIRS}
)

link_directories(${OCC_LIBRARY_DIR})

set(PMI_LIBS
  FreeCADApp
  Part
  Import
  ${OCC_OCAF_LIBRARIES}
)
if(FREETYPE_FOUND)
    add_definitions(-DFCUseFreeType)
    list(APPEND PMI_LIBS ${FREETYPE_LIBRARIES})
endif(FREETYPE_FOUND)

set(PMI_SRCS
  AppPMI.cpp
  Api.cpp
  Api.h
  Advisor.cpp
  Advisor.h
  Annotation.cpp
  Annotation.h
  AnnotationSet.cpp
  AnnotationSet.h
  Callout.cpp
  Callout.h
  Check.cpp
  Check.h
  Datum.cpp
  Datum.h
  Dimension.cpp
  Dimension.h
  Drawing.cpp
  Drawing.h
  Font.cpp
  Font.h
  GeometricTolerance.cpp
  GeometricTolerance.h
  Geometry.cpp
  Geometry.h
  Layout.cpp
  Note.cpp
  Note.h
  View.cpp
  View.h
  Hole.cpp
  Hole.h
  PreCompiled.cpp
  PreCompiled.h
  TolerancedFeature.cpp
  TolerancedFeature.h
)

if(FREECAD_USE_PCH)
    add_definitions(-D_PreComp_)
    GET_MSVC_PRECOMPILED_SOURCE("PreCompiled.cpp" PCH_SRCS ${PMI_SRCS})
    ADD_MSVC_PRECOMPILED_HEADER(PMI PreCompiled.h PreCompiled.cpp PCH_SRCS)
endif(FREECAD_USE_PCH)

add_library(PMI SHARED ${PMI_SRCS})
target_link_libraries(PMI ${PMI_LIBS})

SET_BIN_DIR(PMI PMI /Mod/PMI)
SET_PYTHON_PREFIX_SUFFIX(PMI)

install(TARGETS PMI DESTINATION ${CMAKE_INSTALL_LIBDIR})

# The GD&T of STEP files, read and written: the one part of PMI that calls the
# OCCT fork's GD&T extensions of XCAF by their C++ names. A module of its own,
# which PMI loads only where the fork's marker is found, so PMI itself loads
# where the OCCT libraries are upstream's (AppPMIStep.cpp, docs/GDandT.md)
set(PMIStep_SRCS
  AppPMIStep.cpp
  StepExport.cpp
  StepExport.h
  StepImport.cpp
  StepImport.h
)

add_library(PMIStep SHARED ${PMIStep_SRCS})
target_link_libraries(PMIStep PMI ${PMI_LIBS})

SET_BIN_DIR(PMIStep PMIStep /Mod/PMI)
SET_PYTHON_PREFIX_SUFFIX(PMIStep)

install(TARGETS PMIStep DESTINATION ${CMAKE_INSTALL_LIBDIR})
