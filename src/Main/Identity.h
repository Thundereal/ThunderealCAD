// SPDX-License-Identifier: LGPL-2.1-or-later

// What the executables call themselves and say about themselves, as the build
// switch THUNDEREAL_IDENTITY picks. src/Main/CMakeLists.txt defines
// FREECAD_APP_NAME for every target here, and THUNDEREAL_IDENTITY when it is ON;
// OFF is upstream's text, byte for byte.
//
// The copyright line keeps upstream's authors under either setting: it names
// the authors of the code, and the code is theirs. Thundereal's own goes in
// front of theirs.

#ifndef MAIN_IDENTITY_H
#define MAIN_IDENTITY_H

#ifndef FREECAD_APP_NAME
#define FREECAD_APP_NAME "FreeCAD"
#endif

#define FREECAD_UPSTREAM_AUTHORS "Juergen Riegel, Werner Mayer, Yorik van Havre and others 2001-2023\n"

#ifdef THUNDEREAL_IDENTITY

#define FREECAD_COPYRIGHT_AUTHORS "The Thundereal developers 2026; " FREECAD_UPSTREAM_AUTHORS

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
#define FREECAD_MAINTAINER_URL "https://github.com/Thundereal/ThunderealCAD"
#define FREECAD_ISSUE_PAGE "https://github.com/Thundereal/ThunderealCAD/issues"
// Where the files the program writes say it lives ("Created by Thundereal
// <url>", App/BrandText.h)
#define FREECAD_PROGRAM_URL "https://github.com/Thundereal/ThunderealCAD"
// The Help menu's sites (Gui/CommandStd.cpp, helpSiteUrl): the website, the
// repository's discussions, the help site's pages -- named as FreeCAD's wiki
// names them -- and the website's page on supporting the work, which lists the
// ways to donate. HelpWikiUrl is where the Help module (src/Mod/Help) finds a
// page by its wiki name, for F1 and What's This; UrlRetired lists what these
// keys used to open, so that a preference holding one of those follows the
// new default rather than passing for the user's own choice.
#define FREECAD_HELP_SITES                                                                         \
    {"UrlWebPage", "https://thundereal.org"},                                                      \
        {"UrlUserForum", "https://github.com/Thundereal/ThunderealCAD/discussions"},               \
        {"UrlOnlineHelp", "https://wiki.thundereal.org/Online_Help_Toc"},                          \
        {"UrlDocumentation", "https://wiki.thundereal.org/User_hub"},                              \
        {"UrlPowerUsers", "https://wiki.thundereal.org/Power_users_hub"},                          \
        {"UrlFAQ", "https://wiki.thundereal.org/Frequently_asked_questions"},                      \
        {"UrlDonatePage", "https://thundereal.org/#support"},                                      \
        {"HelpWikiUrl", "https://wiki.thundereal.org"},                                            \
        {"UrlRetired", "https://github.com/Thundereal/ThunderealCAD/wiki\n"                        \
                       "https://github.com/Thundereal/ThunderealCAD/wiki/Python-scripting\n"       \
                       "https://github.com/Thundereal/ThunderealCAD/wiki/FAQ\n"                    \
                       "https://github.com/Thundereal/Thundereal/wiki\n"                           \
                       "https://github.com/Thundereal/Thundereal/wiki/Python-scripting\n"          \
                       "https://github.com/Thundereal/Thundereal/wiki/FAQ"}
// The splash picture carries the wordmark, so Gui writes only the version, under it
#define FREECAD_SPLASH_INFO_EXE_NAME ""
#define FREECAD_SPLASH_INFO_POSITION "20,78"
// and its development warning, in as many lines as it takes to stay left of the mark
#define FREECAD_SPLASH_WARNING_WIDTH "225"

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
#define FREECAD_COPYRIGHT_AUTHORS FREECAD_UPSTREAM_AUTHORS
#define FREECAD_MAINTAINER_URL "https://www.freecad.org/wiki/Main_Page"
// FREECAD_ISSUE_PAGE unset: App falls back to upstream's tracker.
#define FREECAD_SPLASH_INFO_POSITION "6,75"

#endif

#endif  // MAIN_IDENTITY_H
