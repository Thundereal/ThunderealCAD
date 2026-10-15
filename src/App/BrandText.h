// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef APP_BRANDTEXT_H
#define APP_BRANDTEXT_H

#include <string>

#include <QString>

#include <FCGlobal.h>

namespace App
{

/** @name The program's name in text written for FreeCAD
 *
 * A build that goes by another name (ExeName, which THUNDEREAL_IDENTITY sets)
 * still carries upstream's text, and much of it says "FreeCAD" for this
 * program: "restart FreeCAD", "FreeCAD's macro directory". Much of the rest
 * says it for upstream itself and has to keep it: the FreeCAD wiki, forum and
 * website, FreeCAD 0.19, an earlier version of FreeCAD, the credits, the
 * Python module FreeCAD.ActiveDocument, a path like .FreeCAD/Mod. Where the
 * word only says the thing belongs to FreeCAD -- the themes FreeCAD Dark and
 * FreeCAD Light, a FreeCAD macro, a FreeCAD Addon -- it is dropped, which
 * reads true for either program. These functions put the program's
 * name in the first kind and leave the second, so the source strings, and every
 * language's translation of them, stay upstream's.
 *
 * Gui installs them under every translation (Gui/Language/Translator.cpp);
 * text that is not translated calls brandText() where it is written. With
 * ExeName FreeCAD both return their input unchanged.
 */
//@{

/// True when the program goes by a name other than FreeCAD.
AppExport bool isRebranded();

/** Where the program lives on the web, for the files it writes to say who
 * made them: ProgramUrl, which a build with a name of its own sets, else
 * upstream's site.
 */
AppExport std::string programUrl();

/// "Created by <name> <<url>>": what the exporters write into a file's header.
AppExport std::string createdBy();

/** English text with the program's name for each FreeCAD that means this
 * program. Two phrases this fork's own text uses read differently once the
 * program has a name of its own: "this FreeCAD" -- this build, as against
 * upstream -- becomes the name, and "other FreeCAD" -- upstream -- becomes
 * FreeCAD.
 */
AppExport QString brandText(const QString& text);
AppExport std::string brandText(const char* text);

/** A translation of @a source, with the program's name in it when every
 * FreeCAD in @a source means this program. A source that mixes the two
 * meanings, or reads differently only in English, leaves its translation as
 * it is: which FreeCAD is which cannot be told in another language.
 */
AppExport QString brandTranslation(const QString& source, const QString& translation);

/** Text, English or translated, in which every FreeCAD means this program --
 * a Help command's, once its site is the program's own: the name for each,
 * but in a name, a path or a link.
 */
AppExport QString brandEvery(const QString& text);

//@}

}  // namespace App

#endif  // APP_BRANDTEXT_H
