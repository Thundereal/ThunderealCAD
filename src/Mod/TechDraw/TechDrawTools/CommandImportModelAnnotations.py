# SPDX-License-Identifier: LGPL-2.1-or-later
# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This library is free software; you can redistribute it and/or         *
# *   modify it under the terms of the GNU Library General Public           *
# *   License as published by the Free Software Foundation; either          *
# *   version 2 of the License, or (at your option) any later version.      *
# *                                                                         *
# ***************************************************************************
"""Provides the TechDraw ImportModelAnnotations GuiCommand: the 3D model's
PMI callouts in the selected part views (TechDrawPMI)."""

from PySide.QtCore import QT_TRANSLATE_NOOP

import FreeCAD as App
import FreeCADGui as Gui


def _views():
    return [o for o in Gui.Selection.getSelection()
            if o.isDerivedFrom("TechDraw::DrawViewPart")]


class CommandImportModelAnnotations:
    """Import or refresh the 3D model's PMI callouts in the selected views"""

    def GetResources(self):
        return {"Pixmap": "TechDraw_Balloon",
                "MenuText": QT_TRANSLATE_NOOP("TechDraw_ImportModelAnnotations",
                                              "Import Model Annotations"),
                "ToolTip": QT_TRANSLATE_NOOP(
                    "TechDraw_ImportModelAnnotations",
                    "The 3D model's GD&T in the selected views: each feature control frame, "
                    "datum, datum target and note whose plane faces the view and whose "
                    "feature it shows, linked to it. Again, it follows the model and keeps "
                    "what you moved")}

    def IsActive(self):
        return bool(_views())

    def Activated(self):
        import TechDrawPMI

        App.ActiveDocument.openTransaction("Import Model Annotations")
        try:
            for view in _views():
                result = TechDrawPMI.importAnnotations(view)
                if result is None:
                    App.Console.PrintWarning(
                        "%s: no projection yet -- recompute it, then import\n" % view.Label)
                    continue
                made, updated, removed = result
                App.Console.PrintMessage(
                    "%s: %d model annotations imported, %d updated, %d removed\n"
                    % (view.Label, len(made), len(updated), len(removed)))
            App.ActiveDocument.recompute()
        finally:
            App.ActiveDocument.commitTransaction()


Gui.addCommand("TechDraw_ImportModelAnnotations", CommandImportModelAnnotations())
