# PMI: product and manufacturing information -- GD&T on a part (docs/GDandT.md)

add_subdirectory(App)

set(PMI_Scripts
    Init.py
    TestPMIApp.py
)

set(PMI_TestScripts
    PMITests/__init__.py
    PMITests/TestModel.py
)

add_custom_target(PMIScripts ALL
    SOURCES ${PMI_Scripts} ${PMI_TestScripts}
)

fc_target_copy_resource(PMIScripts
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/Mod/PMI
    ${PMI_Scripts}
    ${PMI_TestScripts}
)

install(FILES ${PMI_Scripts} DESTINATION Mod/PMI)
install(FILES ${PMI_TestScripts} DESTINATION Mod/PMI/PMITests)
