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

import json
import os
import re
import sys
import types
import zipfile

import FreeCAD as App

_count = 0  # fakes used since the worker started


def _where():
    """The first caller outside this module, as file:line"""
    frame = sys._getframe(1)  # pylint: disable=protected-access
    here = __file__
    while frame is not None and frame.f_code.co_filename == here:
        frame = frame.f_back
    if frame is None:
        return ""
    return "%s:%d" % (os.path.basename(frame.f_code.co_filename), frame.f_lineno)


def _fake(what):
    global _count
    _count += 1
    try:
        where = _where()
        App._workerFaked("%s (%s)" % (what, where) if where else what)
    except Exception:  # pylint: disable=broad-except
        pass


def faked():
    """How many fakes were used since the worker started"""
    return _count


def _under(path, root):
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:  # another drive
        return False


def origin(proxy):
    """(class, built-in) of an object's Python proxy: whether its code is the
    install's, which a worker is to make itself, or an addon's or a macro's,
    which may be made by the asking process (docs/WorkerProcess.md sec 73).
    By where its module's file is: under the install's home (its Mod, Ext
    and lib) it is built-in; under the user's data or macro directory, or
    anywhere else, it is not. A module may say it stands for an addon's
    (`__freecad_addon__ = True`), as a test's does"""
    cls = type(proxy)
    name = "%s.%s" % (cls.__module__, cls.__qualname__)
    module = sys.modules.get(cls.__module__)
    if module is None or getattr(module, "__freecad_addon__", False):
        return name, False
    path = getattr(module, "__file__", None)
    if not path:
        return name, True  # compiled in
    path = os.path.normcase(os.path.realpath(path))
    roots = []
    for get in ("getUserAppDataDir", "getUserMacroDir"):
        try:
            roots.append(os.path.normcase(os.path.realpath(getattr(App, get)())))
        except Exception:  # pylint: disable=broad-except
            pass
    if any(_under(path, r) for r in roots):
        return name, False
    home = os.path.normcase(os.path.realpath(App.getHomePath()))
    return name, _under(path, home)


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
#   (Part._writeViewLook)
_LOOK_WRITES = frozenset(("LineMaterial", "PointMaterial", "LineColorArray", "PointColorArray"))
# What a view provider is asked that is not a property: the scene's and the
# GUI's, which a worker has not got -- a fake
_VIEW_API = frozenset(("RootNode", "SwitchNode", "Annotation", "TypeId", "Document",
                       "getElementColors", "setElementColors", "getDisplayModes",
                       "claimChildren", "addProperty", "removeProperty",
                       "addDisplayMode", "setTransformation", "toString", "startEditing",
                       "finishEditing", "isEditing", "getActiveView", "getBoundingBox",
                       "getDetailPath", "getElementPicked", "Icon", "DropPrefix",
                       "CanRemoveChildrenFromRoot", "Visibility", "isVisible", "hide", "show",
                       "update", "Proxy", "Object", "Extensions"))


class _SavedViews:
    """A document's GuiDocument.xml as the worker's copy has it: each view
    provider's saved properties, its class's shared defaults under them
    (App::SharedDefaults), read when an object's are first asked for"""

    _element = re.compile(r'<Property name="([^"]+)" type="([^"]+)"[^>]*?(?:/>|>.*?</Property>)',
                          re.S)

    def __init__(self, text):
        self.text = text
        self.index = {}
        for m in re.finditer(r'<ViewProvider name="([^"]+)"', text):
            end = text.find("</ViewProvider>", m.end())
            self.index[m.group(1)] = (m.start(), end if end >= 0 else len(text))
        self.defaults = None
        self.cache = {}

    def _props(self, segment):
        return {m.group(1): (m.group(2), m.group(0)) for m in self._element.finditer(segment)}

    def _defaultsOf(self, vptype):
        if self.defaults is None:
            self.defaults = {}
            a = self.text.find("<Defaults")
            b = self.text.find("</Defaults>")
            if a >= 0 and b > a:
                block = self.text[a:b]
                for m in re.finditer(r'<Default type="([^"]+)">(.*?)</Default>', block, re.S):
                    self.defaults[m.group(1)] = self._props(m.group(2))
        return self.defaults.get(vptype, {})

    def props(self, name, vptype):
        """{property: (type, xml)} of an object's view provider, None if it
        saved none"""
        if name in self.cache:
            return self.cache[name]
        span = self.index.get(name)
        props = None
        if span is not None:
            props = dict(self._defaultsOf(vptype))
            props.update(self._props(self.text[span[0]:span[1]]))
        self.cache[name] = props
        return props


_views = {}
# The copies read that have no saved views: a console's (view_object())
_viewless = set()


def load_views(doc_name, data, path):
    """The saved views of a worker's copy (WorkerPool.cpp, attachViewStandIns):
    the version's GuiDocument.xml from the store, or the copy's file"""
    text = None
    try:
        if data:
            text = bytes(data).decode("utf-8", "replace")
        elif path and os.path.isfile(path):
            if zipfile.is_zipfile(path):
                with zipfile.ZipFile(path) as z:
                    if "GuiDocument.xml" in z.namelist():
                        text = z.read("GuiDocument.xml").decode("utf-8", "replace")
            else:
                with open(path, encoding="utf-8", errors="replace") as f:
                    text = f.read()
    except Exception:  # pylint: disable=broad-except
        text = None
    if text:
        _views[doc_name] = _SavedViews(text)
        _viewless.discard(doc_name)
    else:
        _views.pop(doc_name, None)
        _viewless.add(doc_name)


class _Unreadable(Exception):
    pass


def _record(obj, op):
    """What an execute did to a view, for the asking process to do again"""
    op["object"] = obj.Name
    try:
        App._workerViewWrite(json.dumps(op))
    except Exception:  # pylint: disable=broad-except
        pass


def _token(value, view):
    """A call's argument as it crosses: JSON, a view object or an object by
    name; anything else cannot be sent"""
    if value is view or isinstance(value, ViewObject):
        return {"__view__": value._object.Name if value is not view else None}
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, (list, tuple)):
        return [_token(v, view) for v in value]
    if hasattr(value, "isDerivedFrom") and hasattr(value, "Name"):
        return {"__object__": value.Name}
    raise _Unreadable(type(value).__name__)


class _ProxyStandIn:
    """A view provider's Python proxy in a worker: its state read from the
    restored instance, where there is one; a method called is done by the
    asking process, on the real one, and gives None here"""

    __slots__ = ("_view", "_real")

    def __init__(self, view, real):
        object.__setattr__(self, "_view", view)
        object.__setattr__(self, "_real", real)

    def __getattr__(self, name):
        if name.startswith("__") and name.endswith("__"):
            raise AttributeError(name)
        real = self._real
        if real is not None:
            try:
                value = getattr(real, name)
            except AttributeError:
                raise
            except Exception:  # pylint: disable=broad-except
                value = None
            if not callable(value):
                return value
        view = self._view

        def call(*args, **kwargs):
            try:
                tokens = [_token(a, view) for a in args]
                if kwargs:
                    raise _Unreadable("keyword arguments")
            except _Unreadable:
                _fake("ViewObject.Proxy.%s()" % name)
                return None
            _record(view._object, {"op": "proxy", "name": name, "args": tokens})
            return None

        return call

    def __setattr__(self, name, value):
        _fake("ViewObject.Proxy.%s" % name)

    def __bool__(self):
        return True


class ViewObject:
    """What obj.ViewObject is in a worker: a view provider without a GUI.
    - What it has of the object's -- the looks on a Part::Feature -- is
      read from the object and written to it as the view provider does.
    - Its own properties -- Visibility, LineWidth, DisplayLength, its
      Python-added ones -- are read as the copy's GuiDocument.xml saved
      them; a write is kept, and done again to the real view provider where
      the result is put in (replay()), as is a call of update(), hide(),
      show() or of its Python proxy's methods. Visibility is the view
      provider's own, which can differ from the object's; a write of it
      is the object's too, as the view provider's onChanged makes it.
    - A name it has not, with its saved views to say so, is an
      AttributeError, as on the real one.
    Anything else -- the scene, a property of a type this process cannot
    read -- is a fake. It is true, so that `if obj.ViewObject:` takes the
    path it takes in the GUI"""

    __slots__ = ("_object", "_looks", "_kept", "_props", "_values")

    def __init__(self, obj, looks, props=None):
        object.__setattr__(self, "_object", obj)
        object.__setattr__(self, "_looks", looks)
        object.__setattr__(self, "_kept", {})
        object.__setattr__(self, "_props", props)
        object.__setattr__(self, "_values", {})

    def _look(self, name):
        return self._looks and name in _LOOK_NAMES and name in self._object.PropertiesList

    def _saved(self, name):
        """The saved value of a view property, or _Unreadable"""
        if name in self._values:
            return self._values[name]
        props = self._props
        if props is None or name not in props:
            raise _Unreadable(name)
        ptype, xml = props[name]
        try:
            value = App._propertyValue(ptype, xml)
        except Exception as e:  # pylint: disable=broad-except
            raise _Unreadable(str(e)) from e
        self._values[name] = value
        return value

    def _visible(self):
        try:
            return bool(self._saved("Visibility"))
        except _Unreadable:
            return bool(self._object.Visibility)

    def __getattr__(self, name):
        if name.startswith("__") and name.endswith("__"):
            raise AttributeError(name)
        if name == "Object":
            return self._object
        if name == "Visibility":
            return self._visible()
        if self._look(name):
            return getattr(self._object, name)
        if self._looks and name == "MappedColors" and name in self._kept:
            return self._kept[name]
        if name == "Proxy":
            try:
                real = self._saved("Proxy")
            except _Unreadable:
                real = None
            return _ProxyStandIn(self, real)
        try:
            return self._saved(name)
        except _Unreadable:
            pass
        if self._props is not None and name not in self._props and name not in _VIEW_API \
                and name not in _LOOK_WRITES and name != "MappedColors":
            # Saved views say what it has: one it has not is not there
            raise AttributeError("view provider of %s has no attribute '%s'"
                                 % (self._object.Name, name))
        what = "ViewObject.%s" % name
        _fake(what)
        return _Inert(what)

    def __setattr__(self, name, value):
        if name == "Visibility":
            self._setVisible(bool(value))
            return
        if self._look(name):
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
        props = self._props
        if props is not None and name in props:
            ptype = props[name][0]
            try:
                xml = App._propertyXml(ptype, value)
                kept = App._propertyValue(ptype, xml)
            except Exception:  # pylint: disable=broad-except
                _fake("ViewObject.%s" % name)
                return
            self._values[name] = kept
            _record(self._object, {"op": "set", "name": name, "type": ptype, "xml": xml})
            return
        _fake("ViewObject.%s" % name)

    def _setVisible(self, visible):
        # The view provider's, and the object's as its onChanged makes it;
        # done again to the real one, whose Visibility may not be the
        # object's (an open reads each from its own file)
        self._values["Visibility"] = visible
        self._object.Visibility = visible
        _record(self._object, {"op": "set", "name": "Visibility", "type": "App::PropertyBool",
                               "xml": App._propertyXml("App::PropertyBool", visible)})

    def hide(self):
        self._setVisible(False)

    def show(self):
        self._setVisible(True)

    def isVisible(self):
        # The mode switch, which follows Visibility outside an updateData
        return self._visible()

    def update(self):
        _record(self._object, {"op": "call", "name": "update"})

    def signalChangeIcon(self):
        # The tree's icon, which the view provider here draws again
        _record(self._object, {"op": "call", "name": "signalChangeIcon"})

    def __bool__(self):
        return True

    def __repr__(self):
        try:
            name = self._object.FullName
        except Exception:  # pylint: disable=broad-except
            name = "an object"
        return "<view object of %s, standing in in a worker process>" % name


def view_object(obj, vptype=""):
    """The view object a worker's object is given (WorkerPool.cpp,
    attachViewStandIns), with its saved view properties where the copy has
    them; None where the copy was read and has no saved views at all
    (load_views()).

    A copy with none comes from a process that has no GUI -- a console
    session writes no GuiDocument.xml -- where every obj.ViewObject is None,
    and an execute takes the path it takes there: Draft's WorkingPlaneProxy
    sized its plane from ViewObject.DisplaySize, a fake here, where the
    asking console made it 1 (sec 73)."""
    try:
        name = obj.Document.Name
    except Exception:  # pylint: disable=broad-except
        name = None
    if name in _viewless:
        return None
    saved = _views.get(name)
    try:
        looks = obj.isDerivedFrom("Part::Feature")
    except Exception:  # pylint: disable=broad-except
        looks = False
    props = None
    try:
        if saved is not None:
            props = saved.props(obj.Name, vptype)
    except Exception:  # pylint: disable=broad-except
        props = None
    return ViewObject(obj, looks, props)


def replay(doc_name, text):
    """In the process the result is put in: what the objects a worker made
    did to their views there, done to their view providers here, in order
    (WorkerRecompute.cpp, apply)"""
    try:
        doc = App.getDocument(doc_name)
    except Exception:  # pylint: disable=broad-except
        return
    for entry in json.loads(text):
        made = doc.getObject(entry.get("object", ""))
        if made is None:
            continue

        def arg(a, view=None):
            if isinstance(a, dict) and "__view__" in a:
                if a["__view__"] is None:
                    return view
                other = doc.getObject(a["__view__"])
                return other.ViewObject if other is not None else None
            if isinstance(a, dict) and "__object__" in a:
                return doc.getObject(a["__object__"])
            if isinstance(a, list):
                return [arg(x, view) for x in a]
            return a

        for op in entry.get("ops", []):
            # The view it was done to: the executed object's own, or another's
            # (ArchWall's execute shows its base)
            obj = doc.getObject(op.get("object", "")) or made
            view = getattr(obj, "ViewObject", None)
            if view is None or isinstance(view, ViewObject):
                continue  # no view provider here: nothing to do again
            try:
                kind = op.get("op")
                if kind == "set":
                    setattr(view, op["name"], App._propertyValue(op["type"], op["xml"]))
                elif kind == "call":
                    getattr(view, op["name"])()
                elif kind == "proxy":
                    getattr(view.Proxy, op["name"])(*[arg(a, view) for a in op.get("args", [])])
            except Exception as e:  # pylint: disable=broad-except
                App.Console.PrintWarning("%s: what a worker did to its view not done here: "
                                         "%s %s: %s\n" % (obj.FullName, op.get("op"),
                                                          op.get("name"), e))


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


class _ActiveView:
    """The asking process's active 3D view, as the job said it was
    (WorkerRecompute::setViewState): its camera's direction is real, the rest
    of a view a fake"""

    def __init__(self, state):
        self._state = state

    def getViewDirection(self):
        d = self._state.get("direction") or (0.0, 0.0, -1.0)
        return App.Vector(*d)

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        _fake("ActiveView." + name)
        return _Inert("ActiveView." + name)


_active_view = None


def set_active_view(text):
    """What a recompute job says the asking process's active view is (none
    when it has no 3D view active)"""
    global _active_view
    try:
        state = json.loads(text) if text else {}
    except ValueError:
        state = {}
    _active_view = _ActiveView(state) if state.get("direction") else None


def worker_active_view():
    """The active 3D view in a worker: the asking process's, as its job said,
    or None -- what Draft's gui_utils.get_3d_view() answers there"""
    return _active_view


class GuiDocument:
    """getDocument(name) in a worker: its App document, and the stand-in view
    objects of its objects; no view"""

    def __init__(self, doc):
        self.Document = doc

    def getObject(self, name):
        obj = self.Document.getObject(name)
        return obj.ViewObject if obj is not None else None

    @property
    def ActiveView(self):
        if _active_view is None:
            _fake("Document.ActiveView")
            return _Inert("Document.ActiveView")
        return _active_view

    def activeView(self):
        return self.ActiveView

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
                 "activeDocument", "getMainWindow", "worker_active_view"):
        setattr(module, name, globals()[name])
    module.Selection = _Selection()
    module.Control = _Control()
    module.__getattr__ = _module_getattr
    module._worker_stand_in = True
    return module


class _GuiUpNoted:
    """FreeCAD.GuiUp under Workers/NoteGuiUp, a diagnostic: false, as in any
    worker, and each read inside an execute told as a fake. An execute that
    skips work when there is no GUI sends a result without it; the sweep of
    sec 73 finds those this way"""

    def _read(self):
        _fake("FreeCAD.GuiUp")
        return 0

    def __bool__(self):
        return bool(self._read())

    def __int__(self):
        return self._read()

    __index__ = __int__

    def __eq__(self, other):
        return self._read() == other

    def __ne__(self, other):
        return self._read() != other

    def __hash__(self):
        return hash(0)

    def __repr__(self):
        return "0"


def install():
    """FreeCADGui is this, in this process; once"""
    try:
        if App.ParamGet("User parameter:BaseApp/Preferences/Workers").GetBool("NoteGuiUp", False):
            App.GuiUp = _GuiUpNoted()
    except Exception:  # pylint: disable=broad-except
        pass
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
