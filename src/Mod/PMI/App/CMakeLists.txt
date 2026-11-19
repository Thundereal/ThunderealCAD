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
