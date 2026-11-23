# SPDX-License-Identifier: LGPL-2.1-or-later

# The fluid simulation workbench (docs/FluidSimulation.md).


class FluidWorkbench(Workbench):
    "Fluid workbench"

    def __init__(self):
        import FluidPlayback

        self.__class__.Icon = FluidPlayback.iconPath("FluidWorkbench.svg")
        self.__class__.MenuText = "Fluid"
        self.__class__.ToolTip = "Fluid simulation workbench"

    def Initialize(self):
        from PySide.QtCore import QT_TRANSLATE_NOOP

        import FluidApp
        import FluidCommands

        commands = [
            "Fluid_CreateStudy",
            "Fluid_AddFluid",
            "Fluid_AddBody",
            "Fluid_AddMonitor",
            "Fluid_AddPressureMonitor",
            "Fluid_RunStudy",
        ]
        self.appendToolbar(QT_TRANSLATE_NOOP("Workbench", "Fluid"), commands)
        self.appendMenu([QT_TRANSLATE_NOOP("Workbench", "&Fluid")], commands)

    def GetClassName(self):
        return "Gui::PythonWorkbench"


Gui.addWorkbench(FluidWorkbench())
