"""Sheet metal's commands from a browser's connection (docs/SheetMetal.md
sec 8.2: every command on the browser allowlist, as Surface's).

Base Wall, Flange, Extend Wall and Flatten go on the allowlist; none opens a dialog,
and each says a selection or a body it cannot use in the report view. A
document served with no window, one client with a camera, over a real
socket. Asserted:

  - the `command` op runs each of them;
  - with no active body and nothing selected, they make nothing, and no
    dialog is up.

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

# The platform's backend, as a user gets it: Direct3D 11 on Windows, Metal
# on macOS, OpenGL elsewhere (RenderParams::engineType, docs/Testing.md).
ENGINE = "Default"

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeSheetMetal"
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


COMMANDS = (
    "PartDesign_SheetMetalBaseWall",
    "PartDesign_SheetMetalFlange",
    "PartDesign_SheetMetalContourFlange",
    "PartDesign_SheetMetalJoin",
    "PartDesign_SheetMetalBendCuts",
    "PartDesign_SheetMetalFlatten",
    "PartDesign_SheetMetalExtendWall",
    "PartDesign_SheetMetalHem",
    "PartDesign_SheetMetalCornerClosure",
    "PartDesign_SheetMetalRip",
    "PartDesign_SheetMetalCrossBreak",
    "PartDesign_SheetMetalCutout",
    "PartDesign_SheetMetalFold",
    "PartDesign_SheetMetalJog",
    "PartDesign_SheetMetalTab",
)


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

    def talk(self):
        got = self.got
        ws = WS(self.port)
        ws.hello("serve-sheet-metal")
        if ws.next_binary(20.0) is None:
            raise RuntimeError("no snapshot")
        ws.next_binary(0.5)
        ws.send(2, wsclient.camera_frame(EYE, QUAT, HEIGHT_ANGLE, NEAR, FAR, VW, VH))
        ws.drain(0.5)
        for n, name in enumerate(COMMANDS):
            got[name] = self.op(ws, {"id": 10 + n, "op": "command", "name": name})
            ws.drain(0.3)
        got["after"] = on_host(sample)
        ws.close()


def build():
    try:
        import PartDesignGui  # noqa: F401 -- the commands and the view providers

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE
        )
        doc = FreeCAD.newDocument(DOC, hidden=True)
        body = doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Sketch")
        pts = [(0, 0), (10, 0), (10, 10), (0, 10)]
        for i in range(4):
            a, b = pts[i], pts[(i + 1) % 4]
            sketch.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
        doc.recompute()
        state["objects"] = sorted(o.TypeId for o in doc.Objects)
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
        QtCore.QTimer.singleShot(120000, lambda: (check("the run ended", state["done"]), finish()))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
        finish()


def verify():
    if state["done"]:
        return
    try:
        client = state["client"]
        finished.set()
        got = client.got
        if client.error:
            check("the client ran", False, client.error)
        for name in COMMANDS:
            check(
                "the command op runs " + name,
                (got.get(name) or {}).get("ok") is True,
                got.get(name),
            )
        after = got.get("after", {})
        check(
            "with no active body and nothing selected they make nothing, and no dialog is up",
            after.get("objects") == state["objects"] and after.get("modal") is False,
            after,
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
