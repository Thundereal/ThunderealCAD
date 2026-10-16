include(GNUInstallDirs)

if(NOT DEFINED APPDATA_RELEASE_DATE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(COMMAND git log -1 --pretty=%cd --date=short
            OUTPUT_VARIABLE APPDATA_RELEASE_DATE
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    else()
        if(NOT (CMAKE_VERSION VERSION_LESS 3.8.0))
            string(TIMESTAMP APPDATA_RELEASE_DATE "%Y-%m-%d")
        else()
            file(TIMESTAMP "${CMAKE_SOURCE_DIR}/CMakeLists.txt" APPDATA_RELEASE_DATE "%Y-%m-%d")
        endif()
    endif()
endif()

# The sources keep upstream's names; what is installed is named by the app id
# THUNDEREAL_IDENTITY picks, and the desktop entry and the AppStream data carry it.
# The AppStream data says who makes the program and where it lives.
if(THUNDEREAL_IDENTITY)
    set(_repo "https://github.com/Thundereal/Thundereal")
    set(APPDATA_DEVELOPER "The Thundereal developers")
    set(APPDATA_INTRO "Thundereal, based on FreeCAD,")
    set(APPDATA_URL_HOMEPAGE "https://thundereal.org/")
    set(APPDATA_URL_BUGTRACKER "${_repo}/issues")
    set(APPDATA_URL_FAQ "${_repo}/wiki/FAQ")
    set(APPDATA_URL_HELP "${_repo}/wiki")
    set(APPDATA_URL_DONATION "https://thundereal.org/#support")
    set(APPDATA_URL_VCS "${_repo}")
    set(APPDATA_URL_CONTRIBUTE "${_repo}/blob/main/CONTRIBUTING.md")
    set(APPDATA_UPDATE_CONTACT "")
else()
    set(APPDATA_DEVELOPER "The FreeCAD Team")
    set(APPDATA_INTRO "FreeCAD")
    set(APPDATA_URL_HOMEPAGE "https://www.freecad.org/")
    set(APPDATA_URL_BUGTRACKER "https://github.com/FreeCAD/FreeCAD/issues")
    set(APPDATA_URL_FAQ "https://wiki.freecad.org/Frequently_asked_questions")
    set(APPDATA_URL_HELP "https://wiki.freecad.org/User_hub")
    set(APPDATA_URL_DONATION "https://www.freecad.org/sponsor.php")
    set(APPDATA_URL_VCS "https://github.com/FreeCAD/FreeCAD")
    set(APPDATA_URL_CONTRIBUTE "https://github.com/FreeCAD/FreeCAD/blob/master/CONTRIBUTING.md")
    set(APPDATA_UPDATE_CONTACT "  <update_contact>yorik_AT_uncreated.net</update_contact>\n")
endif()
configure_file(
    org.freecad.FreeCAD.metainfo.xml.in
    ${CMAKE_BINARY_DIR}/${FREECAD_APP_ID}.metainfo.xml
)
install(
    FILES ${CMAKE_BINARY_DIR}/${FREECAD_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
)

configure_file(
    org.freecad.FreeCAD.desktop
    ${CMAKE_BINARY_DIR}/${FREECAD_APP_ID}.desktop
    @ONLY
)
install(
    FILES ${CMAKE_BINARY_DIR}/${FREECAD_APP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)

install(
    FILES org.freecad.FreeCAD.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/mime/packages
    RENAME ${FREECAD_APP_ID}.xml
)

install(
    FILES FreeCAD.thumbnailer
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/thumbnailers
)
