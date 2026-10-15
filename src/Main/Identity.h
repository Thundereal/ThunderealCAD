// SPDX-License-Identifier: LGPL-2.1-or-later

// What the executables call themselves and say about themselves, as the build
// switch THUNDEREAL_IDENTITY picks. src/Main/CMakeLists.txt defines
// FREECAD_APP_NAME for every target here, and THUNDEREAL_IDENTITY when it is ON;
// OFF is upstream's text, byte for byte.
//
// The copyright line stays upstream's under either setting: it names the
// authors of the code, and the code is theirs.

#ifndef MAIN_IDENTITY_H
#define MAIN_IDENTITY_H

#ifndef FREECAD_APP_NAME
#define FREECAD_APP_NAME "FreeCAD"
#endif

#define FREECAD_COPYRIGHT_AUTHORS "Juergen Riegel, Werner Mayer, Yorik van Havre and others 2001-2023\n"

#ifdef THUNDEREAL_IDENTITY

// The folder the user's settings, macros, add-ons and cache live in, which App
// otherwise names after ExeName. It stays upstream's until there is a migration
// that carries them over (docs/RenameInventory.md, section 3). Unset when OFF,
// so a branding.xml that renames ExeName still moves the folder as upstream's
// does.
#define FREECAD_APP_DATA_NAME "FreeCAD"

#define FREECAD_LICENSE_INFO                                                                       \
    "Thundereal is free and open-source software licensed under the terms of LGPL2+ license.\n"
#define FREECAD_CREDITS_INFO                                                                       \
    "Thundereal is based on FreeCAD, and wouldn't be possible without the FreeCAD community.\n"
#define FREECAD_BANNER_ART                                                                         \
    "  #####  #                    #                         #\n"                                  \
    "    #    #                    #                         #\n"                                  \
    "    #    ###   #  #  ###    ###  ####  ##   ####  ###   #\n"                                  \
    "    #    #  #  #  #  #  #  #  #  #  #  # #  #  #     #  #\n"                                  \
    "    #    #  #  #  #  #  #  #  #  ####  #    ####   ###  #\n"                                  \
    "    #    #  #  #  #  #  #  #  #  #     #    #     #  #  #  ##  ##  ##\n"                      \
    "    #    #  #  ####  #  #   ###  ####  #    ####   ###  #  ##  ##  ##\n\n"
// Where the About dialog's name links to, and where Help > Report a bug goes:
// this program's own repository, so its bugs do not land in upstream's tracker.
#define FREECAD_MAINTAINER_URL "https://github.com/Thundereal/Thundereal"
#define FREECAD_ISSUE_PAGE "https://github.com/Thundereal/Thundereal/issues"

#else

#define FREECAD_LICENSE_INFO                                                                       \
    "FreeCAD is free and open-source software licensed under the terms of LGPL2+ license.\n"
#define FREECAD_CREDITS_INFO "FreeCAD wouldn't be possible without FreeCAD community.\n"
#define FREECAD_BANNER_ART                                                                         \
    "  #####                 ####  ###   ####  \n"                                                 \
    "  #                    #      # #   #   # \n"                                                 \
    "  #     ##  #### ####  #     #   #  #   # \n"                                                 \
    "  ####  # # #  # #  #  #     #####  #   # \n"                                                 \
    "  #     #   #### ####  #    #     # #   # \n"                                                 \
    "  #     #   #    #     #    #     # #   #  ##  ##  ##\n"                                      \
    "  #     #   #### ####   ### #     # ####   ##  ##  ##\n\n"
#define FREECAD_MAINTAINER_URL "https://www.freecad.org/wiki/Main_Page"
// Unset: App falls back to upstream's tracker.

#endif

#endif  // MAIN_IDENTITY_H
