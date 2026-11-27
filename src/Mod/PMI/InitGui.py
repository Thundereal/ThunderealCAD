# The PMI workbench: GD&T on a part (docs/GDandT.md)

import os

import FreeCAD as App
import FreeCADGui as Gui


class PMIWorkbench(Gui.Workbench):
    """GD&T on a part: datums, tolerances, dimensions, notes"""

    Icon = os.path.join(App.getResourceDir(), "Mod", "PMI", "Resources", "icons", "PMI_Workbench.svg")
    MenuText = "PMI"
    ToolTip = "PMI: GD&T on a part, for STEP AP242 and drawings"

    def Initialize(self):
        import PMI
        import PMIGui

    def GetClassName(self):
        return "PMIGui::Workbench"


Gui.addWorkbench(PMIWorkbench())

# Loaded with the GUI, so PartDesign offers the PMI tool bar while a Body's
# annotation set is active, without this workbench having been opened
try:
    import PMIGui
except ImportError:
    pass
