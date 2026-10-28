"""The older Surface panels from a browser's connection (docs/Surface.md sec
8): Sections, Filling and GeomFillSurface.

Their commands go on the browser allowlist. Each makes its object and
opens its panel in the client's view; their OK on an object that cannot
be built says why in the panel, never in a dialog (a dialog's event loop
would stop the serving process for every client), and their OK and Cancel
end the edit on the object's own document, which a served process's
Gui.ActiveDocument is not. A document served with no window, one client
with a camera, over a real socket. For each of the three, asserted:

  - the command op runs it: the object is made, in its edit, its panel
    open, no dialog up;
  - the panel is mirrored;
  - the client's OK on the empty object is refused, the panel stays, and no
    dialog is up;
  - the client's Cancel takes the object away and closes the panel.

Run through scripts/gui-test.sh (xvfb, isolated configuration, external
timeout), or by hand as `FreeCAD <this script>` with GT_OUT set and this
directory on PYTHONPATH.
"""

import json
import math
import os
import queue
import threading
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

import wsclient
from wsclient import WS, free_port

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeSurfacePanels"
V = FreeCAD.Vector

EYE = (5.0, 5.0, 120.0)
QUAT = (0.0, 0.0, 0.0, 1.0)
HEIGHT_ANGLE = math.radians(45.0)
NEAR, FAR = 10.0, 400.0
VW, VH = 800, 600

state = {"done": False}
asked = queue.Queue()
finished = threading.Event()
client_done = threading.Event()


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def on_host(fn, timeout=30.0):
    """Run `fn` on the GUI thread, from the client's thread; its result."""
    box = {"done": threading.Event()}
    asked.put((fn, box))
    if not box["done"].wait(timeout):
        raise RuntimeError("the host did not answer")
    if "error" in box:
        raise RuntimeError(box["error"])
    return box.get("result")


def serve_asked():
    while True:
        try:
            fn, box = asked.get_nowait()
        except queue.Empty:
            break
        try:
            box["result"] = fn()
        except Exception:
            box["error"] = traceback.format_exc()
        box["done"].set()
    if client_done.is_set():
        verify()
    elif not state["done"]:
        QtCore.QTimer.singleShot(20, serve_asked)


COMMANDS = {
    "Surface_Sections": "Surface::Sections",
    "Surface_Filling": "Surface::Filling",
    "Surface_GeomFillSurface": "Surface::GeomFillSurface",
}


def sample():
    doc = FreeCAD.getDocument(DOC)
    gdoc = FreeCADGui.getDocument(DOC)
    edit = gdoc.getInEdit()
    return {
        "objects": sorted(o.TypeId for o in doc.Objects),
        "edit": edit.Object.TypeId if edit is not None else None,
        "panel": bool(FreeCADGui.Control.activeDialog()),
        "modal": QtWidgets.QApplication.activeModalWidget() is not None,
    }


def panel_root():
    fw = FreeCADGui.FormWidgets
    roots = [w for w in fw.ids() if w.startswith("panel:")]
    return roots[-1] if roots else None


class Client(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.error = None
        self.got = {}

    def run(self):
        try:
            self.talk()
        except Exception:
            self.error = traceback.format_exc()
        client_done.set()

    def op(self, ws, payload):
        raw = ws.op(json.dumps(payload))
        return json.loads(raw.decode("utf-8")) if raw else None

    def custom(self, ws, n, target, event):
        return self.op(
            ws, {"id": n, "op": "widgets.custom", "target": target, "content": {"event": event}}
        )

    def talk(self):
        ws = WS(self.port)
        ws.hello("serve-surface-panels")
        if ws.next_binary(20.0) is None:
            raise RuntimeError("no snapshot")
        ws.next_binary(0.5)
        ws.send(2, wsclient.camera_frame(EYE, QUAT, HEIGHT_ANGLE, NEAR, FAR, VW, VH))
        ws.drain(0.5)
        self.op(ws, {"id": 3, "op": "widgets.subscribe", "panels": True})
        ws.drain(0.5)
        n = 10
        for name in COMMANDS:
            got = self.got[name] = {}
            got["command"] = self.op(ws, {"id": n, "op": "command", "name": name})
            ws.drain(1.0)
            got["made"] = on_host(sample)
            root = on_host(panel_root)
            got["root"] = root
            if root and (got["command"] or {}).get("ok"):
                got["ok"] = self.custom(ws, n + 1, root, "accept")
                ws.drain(1.0)
                got["refused"] = on_host(sample)
                got["cancel"] = self.custom(ws, n + 2, root, "reject")
                ws.drain(1.0)
                got["cancelled"] = on_host(sample)
            n += 10
        self.op(ws, {"id": 99, "op": "widgets.unsubscribe"})
        ws.close()


def build():
    try:
        import SurfaceGui  # noqa: F401 -- the commands and the view providers

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", "bgfx - OpenGL"
        )
        doc = FreeCAD.newDocument(DOC, hidden=True)
        p = doc.addObject("Part::Plane", "Plane")
        p.Length = p.Width = 10
        doc.recompute()
        port = free_port()
        if not check(
            "the document is served with no window",
            FreeCADGui.serveDocument(doc, port),
            "port %d" % port,
        ):
            finish()
            return
        client = Client(port)
        state["client"] = client
        QtCore.QTimer.singleShot(20, serve_asked)
        client.start()
        QtCore.QTimer.singleShot(150000, lambda: (check("the run ended", state["done"]), finish()))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
        finish()


def verify():
    if state["done"]:
        return
    try:
        client = state["client"]
        finished.set()
        if client.error:
            check("the client ran", False, client.error)
        for name, type_ in COMMANDS.items():
            got = client.got.get(name, {})
            made = got.get("made", {})
            check(
                "%s: the command op runs it, its object made in its edit, no dialog" % name,
                (got.get("command") or {}).get("ok") is True
                and type_ in made.get("objects", [])
                and made.get("edit") == type_
                and made.get("panel") is True
                and made.get("modal") is False,
                (got.get("command"), made),
            )
            check("%s: its panel is mirrored" % name, got.get("root") is not None)
            refused = got.get("refused", {})
            check(
                "%s: OK on the empty object is refused, with no dialog" % name,
                refused.get("panel") is True and refused.get("modal") is False,
                refused,
            )
            cancelled = got.get("cancelled", {})
            check(
                "%s: Cancel takes the object away and closes the panel" % name,
                type_ not in cancelled.get("objects", [type_])
                and cancelled.get("panel") is False
                and cancelled.get("modal") is False,
                cancelled,
            )
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    finished.set()
    try:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, build)
