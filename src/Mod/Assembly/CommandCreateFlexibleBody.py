# SPDX-License-Identifier: LGPL-2.1-or-later
"""The command making a component a flexible body (docs/AssemblyDynamics.md,
D4b): an Assembly::FlexibleBody of the component selected, used by every
Dynamic study of the assembly. Not upstream."""

import FreeCAD as App

if App.GuiUp:
    import FreeCADGui as Gui

import UtilsAssembly
import FlexibleBodies

__title__ = "Assembly Command Create Flexible Body"
__url__ = "https://www.freecad.org"

translate = App.Qt.translate
QT_TRANSLATE_NOOP = App.Qt.QT_TRANSLATE_NOOP


def selectedComponent(assembly):
    """The one component of the assembly selected, in the tree or in the 3D
    view, else None"""
    found = []
    for sel in Gui.Selection.getSelectionEx("", 0):
        for sub in sel.SubElementNames or [""]:
            part, _ = UtilsAssembly.getComponentReference(assembly, sel.Object, sub)
            if part is not None and part not in found:
                found.append(part)
    return found[0] if len(found) == 1 else None


class CommandCreateFlexibleBody:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_FlexibleBody",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_CreateFlexibleBody", "Flexible Body"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_CreateFlexibleBody",
                "A component that bends in a dynamic simulation: meshed, and reduced to its "
                "modes\nheld where its joints hold it. Select a component first.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        if not UtilsAssembly.isAssemblyCommandActive():
            return False
        assembly = UtilsAssembly.activeAssembly()
        return assembly is not None and selectedComponent(assembly) is not None

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        component = selectedComponent(assembly) if assembly else None
        if component is None:
            return
        flexible = FlexibleBodies.makeFlexibleBody(assembly, component)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(flexible)


if App.GuiUp:
    Gui.addCommand("Assembly_CreateFlexibleBody", CommandCreateFlexibleBody())
