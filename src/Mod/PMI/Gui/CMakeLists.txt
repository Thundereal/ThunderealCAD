include_directories(
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${COIN3D_INCLUDE_DIRS}
    ${OCC_INCLUDE_DIR}
    ${PYTHON_INCLUDE_DIRS}
    ${XercesC_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIR}
)

set(PMIGui_LIBS
    PMI
    PartGui
    FreeCADGui
)

qt_add_resources(PMIGui_QRC_SRCS Resources/PMI.qrc)

set(PMIGui_SRCS
    ${PMIGui_QRC_SRCS}
    AppPMIGui.cpp
    Command.cpp
    PreCompiled.cpp
    PreCompiled.h
    TaskGeometricTolerance.cpp
    TaskGeometricTolerance.h
    TaskQuery.cpp
    TaskQuery.h
    ViewProviderCallout.cpp
    ViewProviderCallout.h
    ViewProviderView.cpp
    ViewProviderView.h
    Workbench.cpp
    Workbench.h
)

set(PMIGuiIcon_SVG
    Resources/icons/PMI_Workbench.svg
)

if(FREECAD_USE_PCH)
    add_definitions(-D_PreComp_)
    GET_MSVC_PRECOMPILED_SOURCE("PreCompiled.cpp" PCH_SRCS ${PMIGui_SRCS})
    ADD_MSVC_PRECOMPILED_HEADER(PMIGui PreCompiled.h PreCompiled.cpp PCH_SRCS)
endif(FREECAD_USE_PCH)

link_directories(${OCC_LIBRARY_DIR})
add_library(PMIGui SHARED ${PMIGui_SRCS} ${PMIGuiIcon_SVG})
target_link_libraries(PMIGui ${PMIGui_LIBS})

SET_BIN_DIR(PMIGui PMIGui /Mod/PMI)
SET_PYTHON_PREFIX_SUFFIX(PMIGui)

fc_copy_sources(PMIGui "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/Mod/PMI" ${PMIGuiIcon_SVG})

install(TARGETS PMIGui DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${PMIGuiIcon_SVG} DESTINATION "${CMAKE_INSTALL_DATADIR}/Mod/PMI/Resources/icons")
