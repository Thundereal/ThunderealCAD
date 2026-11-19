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

set(PMI_SRCS
  AppPMI.cpp
  Annotation.cpp
  Annotation.h
  AnnotationSet.cpp
  AnnotationSet.h
  Check.cpp
  Check.h
  Datum.cpp
  Datum.h
  Dimension.cpp
  Dimension.h
  GeometricTolerance.cpp
  GeometricTolerance.h
  Geometry.cpp
  Geometry.h
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
