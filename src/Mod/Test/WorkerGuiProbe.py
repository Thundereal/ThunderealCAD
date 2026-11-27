# SPDX-License-Identifier: LGPL-2.1-or-later
"""An addon's module as an external workbench writes one, for the worker
tests (tests/gui/addon-gui-by-worker.py, docs/WorkerProcess.md sec 72).

It does what OpticsWorkbench does (OpticalObject.py, Ray.py): it calls
FreeCADGui at import -- a command registered, an icon path added -- with no
`FreeCAD.GuiUp` guard, and three of its features use their view in their
execute(): a look, the view's own line width, and the scene graph. Imported in the GUI and in a worker, never in a plain console
(FreeCADGui there has no addCommand).
"""

import os

import FreeCAD
import FreeCADGui as Gui
import Part

# Installed with the Test module, it stands for an addon's: a worker leaves
# what it cannot make of it to the asking process, as it would an addon's
# (freecad.worker_gui.origin, docs/WorkerProcess.md sec 73)
__freecad_addon__ = True


class Plain:
    """A box of Size, made wherever it is recomputed; says which process
    made it"""

    def __init__(self, obj):
        obj.addProperty("App::PropertyLength", "Size")
        obj.addProperty("App::PropertyInteger", "MadeBy")
        obj.Size = 10
        obj.Proxy = self
        self.update = False

    def execute(self, obj):
        obj.Shape = Part.makeBox(obj.Size, obj.Size, obj.Size)
        obj.MadeBy = os.getpid()

    def dumps(self):
        return {"update": False}

    def loads(self, state):
        self.update = False


class Colored(Plain):
    """Writes its view in its execute() unguarded, as Ray.execute writes
    fp.ViewObject.LineColor: red by its size. A look, which is the
    object's: a worker writes it through to the object's LineColor"""

    def execute(self, obj):
        Plain.execute(self, obj)
        obj.ViewObject.LineColor = (min(1.0, float(obj.Size) / 100.0), 0.0, 0.0)


class Wide(Plain):
    """Writes what is the view's alone, its line width by its size: a worker
    has it as the copy saved it, and the write is done again to the view
    provider where the result is put in"""

    def execute(self, obj):
        Plain.execute(self, obj)
        obj.ViewObject.LineWidth = float(obj.Size) / 10.0


class Scener(Plain):
    """Reaches its view's scene graph, which no worker has: made in the GUI's
    process"""

    def execute(self, obj):
        Plain.execute(self, obj)
        obj.ViewObject.RootNode.getNumChildren()


class _Make:
    def GetResources(self):
        return {"MenuText": "Probe", "ToolTip": "A worker test's command",
                "Pixmap": "WorkerGuiProbe"}

    def Activated(self):
        Gui.doCommand("import WorkerGuiProbe")

    def IsActive(self):
        return FreeCAD.ActiveDocument is not None


Gui.addIconPath(os.path.dirname(__file__))
Gui.addCommand("WorkerGuiProbe_Make", _Make())
