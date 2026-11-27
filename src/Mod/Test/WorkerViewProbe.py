# SPDX-License-Identifier: LGPL-2.1-or-later
"""Built-in features that do in their execute() what the install's own do to
their views, for the worker tests (tests/gui/views-by-worker.py,
docs/WorkerProcess.md sec 73). Installed under the home Mod, they are
built-in: a worker is to make each of them itself.

- Hider shows or hides its Base by its Show, as ArchWall.execute shows its
  base and Fixture.execute sets its own Visibility.
- Sized is a box of its view's DisplaySize, as ArchSectionPlane.execute
  takes DisplayLength and wpproxy DisplaySize from their views.
- Refresher has its view redraw (Dimension.execute's ViewObject.update())
  and calls its view proxy (BuildingPart.execute's
  ViewObject.Proxy.onChanged(...)).
"""

import os

import FreeCAD
import Part


class Hider:
    def __init__(self, obj):
        obj.addProperty("App::PropertyLink", "Base")
        obj.addProperty("App::PropertyBool", "Show")
        obj.addProperty("App::PropertyInteger", "MadeBy")
        obj.Proxy = self

    def execute(self, obj):
        obj.Shape = Part.makeBox(1, 1, 1)
        if obj.Base and obj.ViewObject:
            obj.Base.ViewObject.Visibility = obj.Show
        obj.MadeBy = os.getpid()

    def dumps(self):
        return None

    def loads(self, state):
        return None


class Sized:
    def __init__(self, obj):
        obj.addProperty("App::PropertyInteger", "Input")
        obj.addProperty("App::PropertyInteger", "MadeBy")
        obj.Proxy = self

    def execute(self, obj):
        size = 1.0
        if obj.ViewObject and hasattr(obj.ViewObject, "DisplaySize"):
            size = obj.ViewObject.DisplaySize.Value
        obj.Shape = Part.makeBox(size, size, size + obj.Input)
        obj.MadeBy = os.getpid()

    def dumps(self):
        return None

    def loads(self, state):
        return None


class SizedView:
    """Sized's view: a DisplaySize of its own, as ArchSectionPlane's has"""

    def __init__(self, vobj):
        vobj.addProperty("App::PropertyLength", "DisplaySize", "Probe", "the box's size")
        vobj.DisplaySize = 10
        vobj.Proxy = self

    def attach(self, vobj):
        self.Object = vobj.Object

    def dumps(self):
        return None

    def loads(self, state):
        return None


class Facing:
    """Takes the normal of its Base's face as Draft does, which turns it
    toward the active view's camera (DraftGeomUtils.get_normal), as
    ArchComponent's extrusion of a structure's base does"""

    def __init__(self, obj):
        obj.addProperty("App::PropertyLink", "Base")
        obj.addProperty("App::PropertyFloat", "NormalZ")
        obj.addProperty("App::PropertyInteger", "MadeBy")
        obj.Proxy = self

    def execute(self, obj):
        import DraftGeomUtils

        obj.Shape = Part.makeBox(1, 1, 1)
        normal = DraftGeomUtils.get_normal(obj.Base.Shape) if obj.Base else None
        obj.NormalZ = normal.z if normal is not None else 0.0
        obj.MadeBy = os.getpid()

    def dumps(self):
        return None

    def loads(self, state):
        return None


class Refresher:
    def __init__(self, obj):
        obj.addProperty("App::PropertyInteger", "Input")
        obj.addProperty("App::PropertyInteger", "MadeBy")
        obj.Proxy = self

    def execute(self, obj):
        obj.Shape = Part.makeBox(1, 1, 1 + obj.Input)
        obj.MadeBy = os.getpid()
        if obj.ViewObject:
            obj.ViewObject.update()
            if obj.ViewObject.Proxy is not None:
                obj.ViewObject.Proxy.mark(obj.ViewObject, "made %d" % obj.Input)

    def dumps(self):
        return None

    def loads(self, state):
        return None


class RefresherView:
    """Counts what its object's execute() asks of it, in properties of the
    view provider"""

    def __init__(self, vobj):
        vobj.addProperty("App::PropertyStringList", "Marks", "Probe", "what mark() was told")
        vobj.Proxy = self

    def attach(self, vobj):
        self.Object = vobj.Object

    def mark(self, vobj, what):
        vobj.Marks = list(vobj.Marks) + [what]

    def dumps(self):
        return None

    def loads(self, state):
        return None
