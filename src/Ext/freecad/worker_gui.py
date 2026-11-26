# SPDX-License-Identifier: LGPL-2.1-or-later
"""FreeCADGui as a worker process has it (docs/WorkerProcess.md sec 72).

A worker is a console process: it has no GUI, and the FreeCADGui it would
import there has none of the functions an addon calls -- OpticsWorkbench's
OpticalObject.py calls Gui.addCommand at import, unguarded, so its classes
could not be imported, its objects were restored without them, and the
worker recomputed them with no code. install() puts this module in
sys.modules["FreeCADGui"] before the worker opens its copy; no GUI library
is loaded.

What needs no event loop is real: the command registry (addCommand,
listCommands, a command runs nothing), icons, languages, preference pages
and workbenches registered and listed, translate(), doCommand() run in
__main__, an empty Selection whose observers are kept, a Control with no
dialog, updateGui() doing nothing. Each of the worker's objects has a view
object standing in for its view provider (view_object): what a view
provider's property is of its object's -- Visibility, and on a
Part::Feature the looks (LineColor, ShapeColor, the Map* flags, ...) -- is
read from the object and written to it as the view provider writes it, so
Ray.execute's fp.ViewObject.LineColor is the object's look, made by the
worker and sent back with the rest.

Everything else -- getMainWindow(), what is the view's own (LineWidth,
DisplayMode), a name this module does not know -- is faked: it answers an
inert object that
takes any call and any attribute, and never raises. A fake is told
(FreeCAD._workerFaked) to the recompute being made: an object whose
execute() used one is not made by the worker but by the asking process,
which has the real thing and says so in its Report view; and a job of a
Python kind that used one is answered unfit (freecad.worker._run).
"""

import sys
import types

import FreeCAD as App

_count = 0  # fakes used since the worker started


def _fake(what):
    global _count
    _count += 1
    try:
        App._workerFaked(what)
    except Exception:  # pylint: disable=broad-except
        pass


def faked():
    """How many fakes were used since the worker started"""
    return _count


class _Inert:
    """What a faked name answers: any attribute, any call, any item is
    another of these; it is false, empty and iterates over nothing"""

    __slots__ = ("_what",)

    def __init__(self, what):
        object.__setattr__(self, "_what", what)

    def __getattr__(self, name):
        if name.startswith("__") and name.endswith("__"):
            raise AttributeError(name)
        what = "%s.%s" % (self._what, name)
        _fake(what)
        return _Inert(what)

    def __setattr__(self, name, value):
        _fake("%s.%s" % (self._what, name))

    def __call__(self, *args, **kwargs):
        _fake(self._what + "()")
        return _Inert(self._what + "()")

    def __getitem__(self, key):
        _fake(self._what + "[]")
        return _Inert(self._what + "[]")

    def __setitem__(self, key, value):
        _fake(self._what + "[]")

    def __bool__(self):
        return False

    def __len__(self):
        return 0

    def __iter__(self):
        return iter(())

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def __repr__(self):
        return "<stand-in for %s in a worker process>" % self._what


# What a view provider's property is of its object's (docs/WorkerProcess.md
# sec 72, ShapeAppearanceDesign.md sec 14.6.1). Where the looks are the
# object's -- a Part::Feature's -- ViewProviderPartExt gives a write of these
# to the object, through the functions the object's own names call:
# - the same name on the object, the same write (LineColor and
#   Feature::writeColors, ShapeColor and writeOwnColor, ...); read there too
_LOOK_NAMES = frozenset(("LineColor", "PointColor", "ShapeColor", "Transparency",
                         "ShapeAppearance", "MapFaceColor", "MapLineColor", "MapPointColor",
                         "MapTransparency", "ForceMapColors"))
# - no name on the object: written as the view provider writes the store
#   (Part._writeViewLook); a read is a fake
_LOOK_WRITES = frozenset(("LineMaterial", "PointMaterial", "LineColorArray", "PointColorArray"))
# Everything else is the view's own -- LineWidth, DisplayMode, Deviation,
# the Render_* properties, the view provider's Proxy -- or goes through an
# accessor of the GUI's own (DiffuseColor, ShapeMaterial): a fake, and the
# object is made by the asking process


class ViewObject:
    """What obj.ViewObject is in a worker: a view provider without a GUI.
    What it has of the object's -- Visibility on any object, the looks on a
    Part::Feature -- is real: read from the object and written to it, the
    value then sent back with the object's others. Anything else asked of
    it or given to it is a fake. It is true, so that `if obj.ViewObject:`
    takes the path it takes in the GUI"""

    __slots__ = ("_object", "_looks", "_kept")

    def __init__(self, obj, looks):
        object.__setattr__(self, "_object", obj)
        object.__setattr__(self, "_looks", looks)
        object.__setattr__(self, "_kept", {})

    def _real(self, name):
        if name == "Visibility":
            return True
        return self._looks and name in _LOOK_NAMES and name in self._object.PropertiesList

    def __getattr__(self, name):
        if name.startswith("__") and name.endswith("__"):
            raise AttributeError(name)
        if name == "Object":
            return self._object
        if self._real(name):
            return getattr(self._object, name)
        if self._looks and name == "MappedColors" and name in self._kept:
            return self._kept[name]
        what = "ViewObject.%s" % name
        _fake(what)
        return _Inert(what)

    def __setattr__(self, name, value):
        if self._real(name):
            setattr(self._object, name, value)
            return
        if self._looks and name in _LOOK_WRITES:
            import Part

            Part._writeViewLook(self._object, name, value)
            return
        if self._looks and name == "MappedColors":
            # The view provider's alone where the looks are the object's:
            # a write there does nothing to the object
            self._kept[name] = value
            return
        _fake("ViewObject.%s" % name)

    def hide(self):
        self._object.Visibility = False

    def show(self):
        self._object.Visibility = True

    def isVisible(self):
        return self._object.Visibility

    def __bool__(self):
        return True

    def __repr__(self):
        try:
            name = self._object.FullName
        except Exception:  # pylint: disable=broad-except
            name = "an object"
        return "<view object of %s, standing in in a worker process>" % name


def view_object(obj):
    """The view object a worker's object is given (WorkerPool.cpp,
    attachViewStandIns)"""
    try:
        looks = obj.isDerivedFrom("Part::Feature")
    except Exception:  # pylint: disable=broad-except
        looks = False
    return ViewObject(obj, looks)


# --- what is real --------------------------------------------------------

_commands = {}
_icons = {}
_icon_paths = []
_language_paths = []
_preference_pages = []
_workbenches = {}


def addCommand(name, command, source=""):
    _commands[name] = command


def listCommands():
    return sorted(_commands)


def isCommandActive(name):
    return False


def runCommand(name, index=0):
    """A command is the GUI's to run: nothing here"""
    _fake("runCommand(%s)" % name)


def addIcon(name, data, format="XPM"):  # pylint: disable=redefined-builtin
    _icons[name] = (data, format)


def addIconPath(path):
    _icon_paths.append(path)


def addLanguagePath(path):
    _language_paths.append(path)


def addPreferencePage(page, group=""):
    _preference_pages.append((page, group))


def updateLocale():
    pass


def updateGui():
    pass


def translate(context, text, *args):
    return text


class Workbench:
    """The base of an addon's workbench, as FreeCADGuiInit.py's"""

    MenuText = ""
    ToolTip = ""
    Icon = None

    def Initialize(self):
        pass

    def Activated(self):
        pass

    def Deactivated(self):
        pass

    def ContextMenu(self, recipient):
        pass

    def GetClassName(self):
        return "Gui::PythonWorkbench"

    def appendToolbar(self, name, cmds):
        pass

    def appendMenu(self, name, cmds):
        pass

    def appendContextMenu(self, name, cmds):
        pass

    def removeToolbar(self, name):
        pass

    def removeMenu(self, name):
        pass

    def listToolbars(self):
        return []

    def listMenus(self):
        return []

    def reloadActive(self):
        pass

    def name(self):
        return type(self).__name__


def addWorkbench(workbench):
    wb = workbench() if isinstance(workbench, type) else workbench
    _workbenches[type(wb).__name__] = wb


def removeWorkbench(name):
    _workbenches.pop(name, None)


def listWorkbenches():
    return dict(_workbenches)


def getWorkbench(name):
    return _workbenches[name]


def activeWorkbench():
    return _workbenches.get("NoneWorkbench") or Workbench()


def activateWorkbench(name):
    _fake("activateWorkbench(%s)" % name)
    return False


def addWorkbenchManipulator(manipulator):
    pass


def removeWorkbenchManipulator(manipulator):
    pass


def doCommand(source):
    """Run in __main__, as the GUI runs it: with no GUI to record it in"""
    import __main__

    exec(source, __main__.__dict__)  # pylint: disable=exec-used


def doCommandGui(source):
    """A line of the GUI's own: nothing here"""


def doCommandSkip(source):
    pass


def addModule(name):
    import __main__

    __main__.__dict__[name.split(".")[0]] = __import__(name)


class _Selection(types.ModuleType):
    """An empty selection: nothing is selected in a worker"""

    class Filter:
        def __init__(self, *args, **kwargs):
            pass

        def match(self):
            return False

        def result(self):
            return []

        def test(self, *args):
            return False

        def setFilter(self, *args):
            pass

    def __init__(self):
        super().__init__("Selection")
        self._observers = []

    def addObserver(self, observer, resolve=1):
        self._observers.append(observer)

    def removeObserver(self, observer):
        if observer in self._observers:
            self._observers.remove(observer)

    def getSelection(self, *args, **kwargs):
        return []

    def getSelectionEx(self, *args, **kwargs):
        return []

    def getCompleteSelection(self, *args, **kwargs):
        return []

    def getSelectionObject(self, *args, **kwargs):
        return None

    def getPreselection(self, *args, **kwargs):
        return None

    def hasSelection(self, *args, **kwargs):
        return False

    def isSelected(self, *args, **kwargs):
        return False

    def countObjectsOfType(self, *args, **kwargs):
        return 0

    def clearSelection(self, *args, **kwargs):
        pass

    def addSelection(self, *args, **kwargs):
        pass

    def removeSelection(self, *args, **kwargs):
        pass

    def addSelectionGate(self, *args, **kwargs):
        pass

    def removeSelectionGate(self, *args, **kwargs):
        pass

    def clearPreselection(self, *args, **kwargs):
        pass

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        _fake("Selection." + name)
        return _Inert("Selection." + name)


class _Control:
    """The task view, with no dialog ever shown"""

    def showDialog(self, dialog):
        _fake("Control.showDialog")

    def closeDialog(self):
        pass

    def activeDialog(self):
        return False

    def activeTaskDialog(self):
        return None

    def isAllowedAlterDocument(self):
        return True

    def isAllowedAlterView(self):
        return True

    def isAllowedAlterSelection(self):
        return True

    def showTaskView(self):
        pass

    def hideTaskView(self):
        pass

    def addTaskWatcher(self, *args):
        pass

    def clearTaskWatcher(self):
        pass

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        _fake("Control." + name)
        return _Inert("Control." + name)


class GuiDocument:
    """getDocument(name) in a worker: its App document, and the stand-in view
    objects of its objects; no view"""

    def __init__(self, doc):
        self.Document = doc

    def getObject(self, name):
        obj = self.Document.getObject(name)
        return obj.ViewObject if obj is not None else None

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        _fake("Document." + name)
        return _Inert("Document." + name)


def getDocument(name):
    doc = App.getDocument(name) if not isinstance(name, App.Document) else name
    return GuiDocument(doc)


def activeDocument():
    doc = App.ActiveDocument
    return GuiDocument(doc) if doc is not None else None


def getMainWindow():
    _fake("getMainWindow()")
    return _Inert("getMainWindow()")


def _module_getattr(name):
    if name.startswith("__"):
        raise AttributeError(name)
    if name == "ActiveDocument":
        return activeDocument()
    _fake("FreeCADGui." + name)
    return _Inert("FreeCADGui." + name)


def _make():
    module = types.ModuleType("FreeCADGui", __doc__)
    for name in ("addCommand", "listCommands", "isCommandActive", "runCommand", "addIcon",
                 "addIconPath", "addLanguagePath", "addPreferencePage", "updateLocale",
                 "updateGui", "translate", "Workbench", "addWorkbench", "removeWorkbench",
                 "listWorkbenches", "getWorkbench", "activeWorkbench", "activateWorkbench",
                 "addWorkbenchManipulator", "removeWorkbenchManipulator", "doCommand",
                 "doCommandGui", "doCommandSkip", "addModule", "getDocument",
                 "activeDocument", "getMainWindow"):
        setattr(module, name, globals()[name])
    module.Selection = _Selection()
    module.Control = _Control()
    module.__getattr__ = _module_getattr
    module._worker_stand_in = True
    return module


def install():
    """FreeCADGui is this, in this process; once"""
    current = sys.modules.get("FreeCADGui")
    if current is not None and getattr(current, "_worker_stand_in", False):
        return current
    module = _make()
    if current is not None:
        # The console's own FreeCADGui, imported before (an Init.py): what
        # holds it gets the same names
        for name, value in vars(module).items():
            if not name.startswith("__") and not hasattr(current, name):
                try:
                    setattr(current, name, value)
                except (AttributeError, TypeError):
                    pass
    sys.modules["FreeCADGui"] = module
    return module
