# PMI: product and manufacturing information -- GD&T on a part (docs/GDandT.md)

add_subdirectory(App)
if(BUILD_GUI)
    add_subdirectory(Gui)
endif(BUILD_GUI)

set(PMI_Scripts
    Init.py
    TestPMIApp.py
)

if(BUILD_GUI)
    list(APPEND PMI_Scripts InitGui.py)
endif(BUILD_GUI)

set(PMI_TestScripts
    PMITests/__init__.py
    PMITests/TestModel.py
    PMITests/TestStep.py
    PMITests/data/nist_ctc_01_asme1_ap242-e1.stp
)

set(PMI_Fonts
    Resources/fonts/osifont-lgpl3fe.ttf
    Resources/fonts/osifont.license
)

add_custom_target(PMIScripts ALL
    SOURCES ${PMI_Scripts} ${PMI_TestScripts} ${PMI_Fonts}
)

fc_target_copy_resource(PMIScripts
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/Mod/PMI
    ${PMI_Scripts}
    ${PMI_TestScripts}
)

fc_target_copy_resource(PMIScripts
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/Mod/PMI
    ${PMI_Fonts}
)
install(FILES ${PMI_Fonts} DESTINATION ${CMAKE_INSTALL_DATADIR}/Mod/PMI/Resources/fonts)

install(FILES ${PMI_Scripts} DESTINATION Mod/PMI)
install(FILES PMITests/__init__.py PMITests/TestModel.py PMITests/TestStep.py
        DESTINATION Mod/PMI/PMITests)
install(FILES PMITests/data/nist_ctc_01_asme1_ap242-e1.stp DESTINATION Mod/PMI/PMITests/data)
