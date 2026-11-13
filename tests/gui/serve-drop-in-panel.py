"""A client that drops with a task panel open does not take the server down.

A client's view is the owner of the panel it opened, and when the client
goes, its view goes, and the panel is rejected on the way out
(docs/TaskPanelPerView.md sec 5.4) -- from inside the view's destructor. A
panel whose reject throws -- Surface's Sections ends its edit through
Gui.ActiveDocument, which a served process without a window does not have
-- threw out of that destructor, which is std::terminate: the serving
process died with every other client on it.

A document served with no window; client A states a camera, enters the
edit of a Sections surface, and drops. Asserted:

  - the edit op opens the panel;
  - after the drop the serving process is alive, the panel is gone and the
    document is out of its edit;
  - and a second client is served: its hello gets a snapshot.

Run through scripts/gui-test.sh (xvfb, isolated configuration, external
timeout), or by hand as `FreeCAD <this script>` with GT_OUT set and this
directory on PYTHONPATH.
"""

import json
import math
import os
import sys
import queue
import threading
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

import wsclient
from wsclient import WS, free_port

# Metal on macOS: a Qt widget's GL context there is 2.1, below what the
# bgfx OpenGL backend takes (docs/Testing.md).
ENGINE = "bgfx - Metal" if sys.platform == "darwin" else "bgfx - OpenGL"

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeDropInPanel"

EYE = (5.0, 5.0, 120.0)
QUAT = (0.0, 0.0, 0.0, 1.0)
HEIGHT_ANGLE = math.radians(45.0)
NEAR, FAR = 10.0, 400.0
VW, VH = 800, 600

state = {"done": False}
asked = queue.Queue()
client_done = threading.Event()


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def on_host(fn, timeout=30.0):
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


def sample():
    gdoc = FreeCADGui.getDocument(DOC)
    edit = gdoc.getInEdit()
    return {
        "edit": edit.Object.Name if edit is not None else None,
        "panel": bool(FreeCADGui.Control.activeDialog()),
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

    def talk(self):
        got = self.got
        ws = WS(self.port)
        ws.hello("serve-drop-in-panel-a")
        if ws.next_binary(20.0) is None:
            raise RuntimeError("no snapshot")
        ws.next_binary(0.5)
        ws.send(2, wsclient.camera_frame(EYE, QUAT, HEIGHT_ANGLE, NEAR, FAR, VW, VH))
        ws.drain(0.5)
        raw = ws.op(json.dumps({"id": 2, "op": "edit", "obj": "Sections", "mode": 0}))
        got["edit"] = json.loads(raw.decode("utf-8")) if raw else None
        ws.drain(1.0)
        got["in"] = on_host(sample)
        # gone, with the panel open
        ws.close()
        threading.Event().wait(2.0)
        got["after"] = on_host(sample)

        b = WS(self.port)
        b.hello("serve-drop-in-panel-b")
        got["second"] = b.next_binary(20.0) is not None
        b.close()


def build():
    try:
        import SurfaceGui  # noqa: F401 -- the view provider and its panel

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE
        )
        doc = FreeCAD.newDocument(DOC, hidden=True)
        lines = []
        for i, y in enumerate((0, 10)):
            o = doc.addObject("Part::Line", "L%d" % i)
            o.X1, o.Y1, o.Z1 = 0, y, 0
            o.X2, o.Y2, o.Z2 = 10, y, 2
            lines.append(o)
        s = doc.addObject("Surface::Sections", "Sections")
        s.NSections = [(o, "Edge1") for o in lines]
        doc.recompute()
        check("the surface builds", s.isValid(), s.getStatusString())

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
        QtCore.QTimer.singleShot(90000, lambda: (check("the run ended", state["done"]), finish()))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
        finish()


def verify():
    if state["done"]:
        return
    try:
        client = state["client"]
        got = client.got
        if client.error:
            check("the client ran", False, client.error)
        now = got.get("in", {})
        check(
            "the edit op opens the panel",
            (got.get("edit") or {}).get("ok") is True
            and now.get("edit") == "Sections"
            and now.get("panel") is True,
            (got.get("edit"), now),
        )
        after = got.get("after", {})
        check(
            "after the client drops, the server is alive, the panel gone, the edit ended",
            after.get("panel") is False and after.get("edit") is None,
            after,
        )
        check("and a second client is served", got.get("second") is True)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(1500, build)
