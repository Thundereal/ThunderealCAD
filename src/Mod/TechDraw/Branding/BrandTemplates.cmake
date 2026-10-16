# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Writes the drawing templates that carry FreeCAD's logo in their title block
# into OUT, with the logo THUNDEREAL_IDENTITY picks: BRAND on swaps the paths
# of each template's freecad-logo group for those of LOGO, off writes the
# template as it is. The group keeps its id and placement, so a template that
# hides the logo with display:none still does.
#
#   cmake -DSRC=<Templates> -DOUT=<dir> -DLOGO=<thundereal-logo.svg> -DBRAND=ON|OFF
#         -P BrandTemplates.cmake
#
# A file is rewritten only when its content changes.

file(READ "${LOGO}" logo)
string(REGEX MATCHALL "<path [^>]*/>" logo_paths "${logo}")
if(NOT logo_paths)
    message(FATAL_ERROR "no paths in ${LOGO}")
endif()
# Joined with \2, the whitespace the template puts before its first path.
string(REPLACE ";" "\\2" logo_paths "${logo_paths}")

file(GLOB templates "${SRC}/*.svg")
foreach(template ${templates})
    file(READ "${template}" text)
    if(NOT text MATCHES "<g id=\"freecad-logo\"")
        continue()
    endif()
    if(BRAND)
        string(REGEX REPLACE
               "(<g id=\"freecad-logo\"[^>]*>)([^<]*)(<path[^>]*>[^<]*)*<path[^>]*>([^<]*)</g>"
               "\\1\\2${logo_paths}\\4</g>" branded "${text}")
        if(branded STREQUAL text)
            message(FATAL_ERROR "the logo group in ${template} is not a group of paths")
        endif()
        set(text "${branded}")
    endif()
    get_filename_component(name "${template}" NAME)
    set(old "")
    if(EXISTS "${OUT}/${name}")
        file(READ "${OUT}/${name}" old)
    endif()
    if(NOT old STREQUAL text)
        file(WRITE "${OUT}/${name}" "${text}")
    endif()
endforeach()
