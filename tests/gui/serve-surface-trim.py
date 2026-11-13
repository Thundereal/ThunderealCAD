"""The cutting commands from a browser's connection (docs/Surface.md sec 8,
U2).

Split, Trim, Untrim and Extend edges go on the browser allowlist, and so do
U3's Blend surface, Two-rail sweep and Pipeshell, U4's Edge continuity and
U5's curve tools. All but Trim open
nothing, and say a selection they cannot use in the report view; Trim
opens its panel, which reaches a client through
the panel mirror. A document served with no window, one client with a
camera, over a real socket. Asserted:

  - the `command` op runs each of them;
  - with nothing selected, those without a panel make nothing and no
    dialog is up;
  - Trim makes an empty trim, in its edit, its panel open, no dialog;
  - the panel is mirrored, with its pick of the sides to keep, its count and
    its status line;
  - the client's Cancel takes the new trim away and closes the panel.

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
import Part
from PySide import QtCore, QtWidgets

import wsclient
from wsclient import WS, free_port

# Metal on macOS: a Qt widget's GL context there is 2.1, below what the
# bgfx OpenGL backend takes (docs/Testing.md).
ENGINE = "bgfx - Metal" if sys.platform == "darwin" else "bgfx - OpenGL"

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeSurfaceTrim"
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


NAMES = ("buttonKeep", "labelKept", "labelStatus", "listBase", "listTools")
COMMANDS = (
    "Surface_Split",
    "Surface_Untrim",
    "Surface_ExtendEdges",
    "Surface_BlendSurface",
    "Surface_TwoRailSweep",
    "Surface_PipeShell",
    "Surface_EdgeContinuity",
    "Surface_Isocurve",
    "Surface_CurveOnSurface",
    "Surface_IntersectionCurve",
    "Surface_JoinCurves",
    "Surface_ExtendCurve",
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


def mirrored():
    fw = FreeCADGui.FormWidgets
    root, found = None, {}
    for wid in fw.ids():
        if wid.startswith("panel:"):
            root = wid
        elif wid.startswith("pw:"):
            snap = fw.snapshot(wid)
            name = snap["state"].get("q_objectName")
            if name in NAMES:
                found[name] = (wid, snap["model"])
    return root, found


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
        ws.hello("serve-surface-trim")
        if ws.next_binary(20.0) is None:
            raise RuntimeError("no snapshot")
        ws.next_binary(0.5)
        ws.send(2, wsclient.camera_frame(EYE, QUAT, HEIGHT_ANGLE, NEAR, FAR, VW, VH))
        ws.drain(0.5)
        for n, name in enumerate(COMMANDS):
            got[name] = self.op(ws, {"id": 10 + n, "op": "command", "name": name})
            ws.drain(0.3)
        got["after"] = on_host(sample)

        got["trim"] = self.op(ws, {"id": 20, "op": "command", "name": "Surface_Trim"})
        ws.drain(1.0)
        got["made"] = on_host(sample)
        got["subscribe"] = self.op(ws, {"id": 21, "op": "widgets.subscribe", "panels": True})
        ws.drain(1.0)
        root, widgets = on_host(mirrored)
        got["mirror"] = (root, widgets)
        if root:
            got["reject"] = self.op(
                ws,
                {"id": 22, "op": "widgets.custom", "target": root, "content": {"event": "reject"}},
            )
            ws.drain(1.0)
        got["rejected"] = on_host(sample)
        self.op(ws, {"id": 30, "op": "widgets.unsubscribe"})
        ws.close()


def build():
    try:
        import SurfaceGui  # noqa: F401 -- the commands and the view providers

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE
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
            "with nothing selected they make nothing, and no dialog is up",
            after.get("objects") == ["Part::Plane"] and after.get("modal") is False,
            after,
        )
        made = got.get("made", {})
        check(
            "the command op runs Surface_Trim",
            (got.get("trim") or {}).get("ok") is True,
            got.get("trim"),
        )
        check(
            "an empty trim is made, in its edit, with its panel open and no dialog",
            made.get("objects") == ["Part::Plane", "Surface::Trim"]
            and made.get("edit") == "Surface::Trim"
            and made.get("panel") is True
            and made.get("modal") is False,
            made,
        )
        root, widgets = got.get("mirror") or (None, {})
        check(
            "the panel is mirrored, with its pick of the sides, its count and its status",
            (got.get("subscribe") or {}).get("ok") is True
            and root is not None
            and set(widgets) == set(NAMES),
            (root, sorted(widgets)),
        )
        rejected = got.get("rejected", {})
        check(
            "the client's Cancel takes the new trim away and closes the panel",
            (got.get("reject") or {}).get("ok") is True
            and rejected.get("objects") == ["Part::Plane"]
            and rejected.get("panel") is False
            and rejected.get("modal") is False,
            (got.get("reject"), rejected),
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
