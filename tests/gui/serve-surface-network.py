"""The network surface from a browser's connection (docs/Surface.md sec 8, U1).

The Network command goes on the browser allowlist, and its panel reaches a
client through the panel mirror; nothing in it runs an event loop of its
own, which a serving process cannot afford. A document served with no
window, one client with a camera, over a real socket. Asserted:

  - the `command` op runs Surface_Network: a network is made, the document
    is in its edit, its panel is open, and no modal dialog is up;
  - the panel is mirrored: the client subscribes and finds its lists, its
    buttons, its condition and tolerance fields and its status line;
  - the client's Cancel (the mirrored root's reject) takes the new network
    away and closes the panel;
  - on a network of a whole 3D sketch entered with the `edit` op, the
    client's click on "Separate the families" writes them, three and three;
    its write to the fit tolerance and to a continuity reaches the feature;
  - a condition with no face to meet is said in the panel, and the client's
    OK is refused with no dialog; set back, OK closes the panel and keeps
    the values as one step, which the client's undo takes back whole.

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
DOC = "ServeSurfaceNetwork"
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


def f(u, v):
    return V(10 * u, 10 * v, 2 * math.sin(math.pi * u) * math.sin(math.pi * v) + u)


def curve(alongU, fixed):
    c = Part.BSplineCurve()
    c.interpolate([f(t / 4, fixed) if alongU else f(fixed, t / 4) for t in range(5)])
    return c


def count(links):
    return sum(len(subs) for _, subs in links)


def sample(name):
    """The network `name` and the panel, on the host"""
    doc = FreeCAD.getDocument(DOC)
    gdoc = FreeCADGui.getDocument(DOC)
    n = doc.getObject(name)
    nets = [o.Name for o in doc.Objects if o.TypeId == "Surface::Network"]
    edit = gdoc.getInEdit()
    out = {
        "nets": nets,
        "edit": edit.Object.Name if edit is not None else None,
        "panel": bool(FreeCADGui.Control.activeDialog()),
        "modal": QtWidgets.QApplication.activeModalWidget() is not None,
    }
    if n is not None:
        out.update(
            {
                "profiles": count(n.Profiles),
                "guides": count(n.Guides),
                "fit": n.FitTolerance.Value,
                "continuity": n.FirstProfileContinuity,
                "valid": n.isValid(),
            }
        )
    status = [
        w.text() for w in FreeCADGui.getMainWindow().findChildren(QtWidgets.QLabel, "labelStatus")
    ]
    out["status"] = status[0] if status else None
    return out


NAMES = (
    "listProfiles",
    "listGuides",
    "buttonSeparate",
    "buttonAddProfile",
    "firstProfileContinuity",
    "fitTolerance",
    "labelStatus",
)


def mirrored():
    """The panel's root, and its widgets by object name, as mirrored"""
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

    def custom(self, ws, n, target, event):
        return self.op(
            ws, {"id": n, "op": "widgets.custom", "target": target, "content": {"event": event}}
        )

    def update(self, ws, n, target, value):
        return self.op(ws, {"id": n, "op": "widgets.update", "target": target, "state": value})

    def talk(self):
        got = self.got
        ws = WS(self.port)
        ws.hello("serve-surface-network")
        if ws.next_binary(20.0) is None:
            raise RuntimeError("no snapshot")
        ws.next_binary(0.5)
        ws.send(2, wsclient.camera_frame(EYE, QUAT, HEIGHT_ANGLE, NEAR, FAR, VW, VH))
        ws.drain(0.5)

        got["command"] = self.op(ws, {"id": 10, "op": "command", "name": "Surface_Network"})
        ws.drain(1.0)
        got["made"] = on_host(lambda: sample("Network"))
        got["subscribe"] = self.op(ws, {"id": 11, "op": "widgets.subscribe", "panels": True})
        ws.drain(1.0)
        if (got["command"] or {}).get("ok"):
            got["mirror"] = on_host(mirrored)
            root = got["mirror"][0]
            got["reject"] = self.custom(ws, 12, root, "reject") if root else None
            ws.drain(1.0)
            got["rejected"] = on_host(lambda: sample("Network"))

        got["edit"] = self.op(ws, {"id": 20, "op": "edit", "obj": "Whole", "mode": 0})
        ws.drain(1.0)
        got["editing"] = on_host(lambda: sample("Whole"))
        root, widgets = on_host(mirrored)
        got["mirror2"] = (root, widgets)
        if "buttonSeparate" in widgets:
            got["separate"] = self.custom(ws, 21, widgets["buttonSeparate"][0], "click")
            ws.drain(0.8)
            got["separated"] = on_host(lambda: sample("Whole"))
        if "fitTolerance" in widgets:
            got["fit"] = self.update(ws, 22, widgets["fitTolerance"][0], {"q_value": 0.001})
            ws.drain(0.8)
        if "firstProfileContinuity" in widgets:
            cont = widgets["firstProfileContinuity"][0]
            got["g1"] = self.update(ws, 23, cont, {"q_currentIndex": 1})
            ws.drain(0.8)
            got["unmet"] = on_host(lambda: sample("Whole"))
            got["ok1"] = self.custom(ws, 24, root, "accept")
            ws.drain(0.8)
            got["refused"] = on_host(lambda: sample("Whole"))
            got["g0"] = self.update(ws, 25, cont, {"q_currentIndex": 0})
            ws.drain(0.8)
            got["ok2"] = self.custom(ws, 26, root, "accept")
            ws.drain(1.0)
            got["accepted"] = on_host(lambda: sample("Whole"))
            got["undo"] = self.op(ws, {"id": 27, "op": "undo"})
            ws.drain(1.0)
            got["undone"] = on_host(lambda: sample("Whole"))
        self.op(ws, {"id": 30, "op": "widgets.unsubscribe"})
        ws.close()


def build():
    try:
        import SurfaceGui  # noqa: F401 -- the command and the view provider

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE
        )
        doc = FreeCAD.newDocument(DOC, hidden=True)
        doc.UndoMode = 1
        doc.openTransaction("base")
        sk = doc.addObject("Sketcher::Sketch3DObject", "Sketch3D")
        for c in [curve(True, v) for v in (0, 0.5, 1)] + [curve(False, u) for u in (0, 0.5, 1)]:
            sk.addGeometry(c)
        n = doc.addObject("Surface::Network", "Whole")
        n.Profiles = [(sk, "")]
        doc.recompute()
        doc.commitTransaction()
        check("the whole sketch's network builds", n.isValid(), n.getStatusString())

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
        got = client.got
        if client.error:
            check("the client ran", False, client.error)
        made = got.get("made", {})
        check(
            "the command op runs Surface_Network",
            (got.get("command") or {}).get("ok") is True,
            got.get("command"),
        )
        check(
            "a network is made, in its edit, with its panel open and no dialog",
            made.get("nets") == ["Whole", "Network"]
            and made.get("edit") == "Network"
            and made.get("panel") is True
            and made.get("modal") is False,
            made,
        )
        root, widgets = got.get("mirror") or (None, {})
        check(
            "the panel is mirrored, with its lists, buttons, fields and status",
            (got.get("subscribe") or {}).get("ok") is True
            and root is not None
            and set(widgets) == set(NAMES),
            (got.get("subscribe"), root, sorted(widgets)),
        )
        check(
            "and its widgets as what they are",
            widgets.get("listProfiles", (0, 0))[1] == "QListWidgetModel"
            and widgets.get("firstProfileContinuity", (0, 0))[1] == "QComboBoxModel"
            and widgets.get("buttonSeparate", (0, 0))[1] == "QPushButtonModel",
            {k: v[1] for k, v in widgets.items()},
        )
        rejected = got.get("rejected", {})
        check(
            "the client's Cancel takes the new network away and closes the panel",
            (got.get("reject") or {}).get("ok") is True
            and rejected.get("nets") == ["Whole"]
            and rejected.get("panel") is False
            and rejected.get("edit") is None,
            (got.get("reject"), rejected),
        )
        editing = got.get("editing", {})
        check(
            "the edit op opens the whole sketch's network's panel",
            (got.get("edit") or {}).get("ok") is True
            and editing.get("edit") == "Whole"
            and editing.get("panel") is True,
            (got.get("edit"), editing),
        )
        separated = got.get("separated", {})
        check(
            "the client's click separates the families, three and three",
            (got.get("separate") or {}).get("ok") is True
            and separated.get("profiles") == 3
            and separated.get("guides") == 3
            and separated.get("valid") is True,
            (got.get("separate"), separated),
        )
        unmet = got.get("unmet", {})
        check(
            "the client's continuity reaches the feature",
            (got.get("g1") or {}).get("ok") is True and unmet.get("continuity") == "G1",
            (got.get("g1"), unmet.get("continuity")),
        )
        check(
            "with no face to meet, it is said in the panel and not in a dialog",
            unmet.get("valid") is False and unmet.get("status") and unmet.get("modal") is False,
            unmet,
        )
        refused = got.get("refused", {})
        check(
            "the client's OK is refused while it is so, with no dialog",
            refused.get("panel") is True and refused.get("modal") is False,
            refused,
        )
        accepted = got.get("accepted", {})
        check(
            "set back, the client's OK closes the panel",
            (got.get("ok2") or {}).get("ok") is True
            and accepted.get("panel") is False
            and accepted.get("edit") is None,
            (got.get("ok2"), accepted),
        )
        check(
            "and keeps the values",
            accepted.get("profiles") == 3
            and accepted.get("continuity") == "G0"
            and abs((accepted.get("fit") or 0) - 0.001) < 1e-12
            and accepted.get("valid") is True,
            accepted,
        )
        undone = got.get("undone", {})
        check(
            "as one step, which the client's undo takes back whole",
            (got.get("undo") or {}).get("ok") is True
            and undone.get("profiles") == 1
            and undone.get("guides") == 0
            and abs((undone.get("fit") or 0) - 1e-4) < 1e-12,
            (got.get("undo"), undone),
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
