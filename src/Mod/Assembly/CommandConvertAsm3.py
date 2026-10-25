# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of FreeCAD.                                         *
# *                                                                         *
# *   FreeCAD is free software: you can redistribute it and/or modify it    *
# *   under the terms of the GNU Lesser General Public License as           *
# *   published by the Free Software Foundation, either version 2.1 of the  *
# *   License, or (at your option) any later version.                       *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful, but        *
# *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
# *   Lesser General Public License for more details.                       *
# *                                                                         *
# *   You should have received a copy of the GNU Lesser General Public      *
# *   License along with FreeCAD. If not, see                               *
# *   <https://www.gnu.org/licenses/>.                                      *
# *                                                                         *
# ***************************************************************************

"""Assembly_ConvertAssembly3: the active document's Assembly3 assemblies to
Assembly objects (AssemblyImportAsm3)."""

import sys

import FreeCAD as App

from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtWidgets

translate = App.Qt.translate


class CommandConvertAsm3:
    def GetResources(self):
        return {
            "Pixmap": "Geoassembly",
            "MenuText": QT_TRANSLATE_NOOP(
                "Assembly_ConvertAssembly3", "Convert Assembly3 Assemblies"
            ),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_ConvertAssembly3",
                "Converts the Assembly3 assemblies of the active document to assemblies, "
                "in place: their parts, elements and constraints become components, "
                "elements and joints.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        doc = App.ActiveDocument
        # The Assembly3 code loads when a document needs it: without it there
        # is nothing to convert, and no need to look
        if doc is None or not any(m.endswith("asm3.assembly") for m in sys.modules):
            return False
        import AssemblyImportAsm3

        return AssemblyImportAsm3.isAsm3Document(doc)

    def Activated(self):
        import AssemblyImportAsm3

        doc = App.ActiveDocument
        try:
            report = AssemblyImportAsm3.convert(doc)
        except Exception as e:
            App.Console.PrintError("Assembly3 conversion failed: %s\n" % e)
            QtWidgets.QMessageBox.critical(
                Gui.getMainWindow(),
                translate("Assembly", "Convert Assembly3 Assemblies"),
                translate("Assembly", "The conversion failed, and the document is as it was:")
                + "\n%s" % e,
            )
            return
        text = str(report)
        App.Console.PrintMessage(text + "\n")
        QtWidgets.QMessageBox.information(
            Gui.getMainWindow(), translate("Assembly", "Convert Assembly3 Assemblies"), text
        )


if App.GuiUp:
    Gui.addCommand("Assembly_ConvertAssembly3", CommandConvertAsm3())
