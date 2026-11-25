"""A PMI view made active in a browser's own view (docs/GDandT.md sec 6.3).

A served document's client has a view of its own on the host (a
MirrorViewer). A PMI view made active from a callout's right-click menu --
its "PMI views" submenu, the one way to reach a view with no tree -- is
active there and nowhere else: what it does not show is hidden in that
client's visibility, and the client is asked to turn its camera to the
view's. Let go, the client is told to restore its camera and everything
shows again.

What is asserted, on a VIEW-ONLY connection (making a PMI view active
changes nothing of the document, so any connection may):

  - a ray onto the flatness's leader gives its menu, with a "PMI views"
    submenu naming the view it is in, kind "viewer" and allowed;
  - nothing in it opens a 3D view on the host ("In a new 3D view" is the
    desktop's alone);
  - triggered, the client is told what it now hides (the datum, not in the
    view) and is asked to take the view's camera: its rotation, target and
    height;
  - the menu again shows the view checked, and "Show all annotations";
    that triggered, the client is told to restore its camera and hides
    nothing;
  - no 3D view was opened on the host at any point.

Run through scripts/gui-test.sh (xvfb, isolated configuration, external
timeout).
"""
import json
import math
import os
import threading
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore

import wsclient
from wsclient import WS, free_port

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServePMIView"
CLIENT_WAIT_S = 120
VIEW_TOKEN = "view-only-pmi"

# Straight down onto the plate (a 40 x 20 x 10 box)
EYE = (20.0, 10.0, 200.0)
QUAT = (0.0, 0.0, 0.0, 1.0)
HEIGHT_ANGLE = math.radians(45.0)
NEAR, FAR = 10.0, 400.0
VW, VH = 800, 600

state = {"doc": None, "client": None, "done": False, "t0": clock(), "views": [],
         "aim": None}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def reply_of(raw):
    return json.loads(raw.decode("utf-8")) if raw else None


def walk(items):
    """Every entry of a menu, submenus flattened"""
    for item in items or []:
        if "items" in item:
            yield from walk(item["items"])
        elif not item.get("separator"):
            yield item


def submenu(items, text):
    for item in items or []:
        if "items" in item:
            if item.get("text", "").replace("&", "") == text:
                return item
            found = submenu(item["items"], text)
            if found:
                return found
    return None


class Client(threading.Thread):
    def __init__(self, port, aim):
        super().__init__(daemon=True)
        self.port = port
        self.aim = aim
        self.error = None
        self.snapshot = False
        self.r = {}

    def run(self):
        try:
            self.talk()
        except Exception:
            self.error = traceback.format_exc()

    def menu(self, ws, rid):
        ray = "[%g,%g,%g,0,0,-1]" % (self.aim[0], self.aim[1], EYE[2])
        return reply_of(ws.op('{"id":%d,"op":"contextMenu","ray":%s}' % (rid, ray))) or {}

    def talk(self):
        ws = WS(self.port, "/scene?token=%s" % VIEW_TOKEN)
        ws.hello("serve-pmi-view")
        self.snapshot = ws.next_binary(20.0) is not None
        if not self.snapshot:
            return
        ws.next_binary(0.5)
        r = self.r
        ws.send(2, wsclient.camera_frame(EYE, QUAT, HEIGHT_ANGLE, NEAR, FAR, VW, VH))
        ws.drain(0.5)

        menu = r["menu"] = self.menu(ws, 1)
        views = submenu(menu.get("items"), "PMI views")
        r["views"] = views
        r["texts"] = [e.get("text", "") for e in walk(menu.get("items"))]
        top = [e for e in walk((views or {}).get("items")) if e.get("text") == "Top"]
        if not top:
            ws.close()
            return
        r["top"] = top[0]
        since = len(ws.pushes)
        r["on"] = reply_of(ws.op('{"id":2,"op":"contextMenu.trigger","menu":%d,"item":%d}'
                                 % (menu["menu"], top[0]["id"])))
        r["on_vis"] = reply_of(ws.next_push("visibility", 5.0, since=since))
        r["on_cam"] = reply_of(ws.next_push("camera", 5.0, since=since))
        ws.drain(0.5)

        menu = r["menu2"] = self.menu(ws, 3)
        views = submenu(menu.get("items"), "PMI views")
        entries = list(walk((views or {}).get("items")))
        r["checked"] = [e.get("text") for e in entries if e.get("checked")]
        off = [e for e in entries if e.get("text") == "Show all annotations"]
        if off:
            r["off_entry"] = off[0]
            since = len(ws.pushes)
            r["off"] = reply_of(ws.op('{"id":4,"op":"contextMenu.trigger","menu":%d,"item":%d}'
                                      % (menu["menu"], off[0]["id"])))
            r["off_vis"] = reply_of(ws.next_push("visibility", 5.0, since=since))
            r["off_cam"] = reply_of(ws.next_push("camera", 5.0, since=since))
        ws.close()


def views_3d():
    return len(FreeCADGui.getDocument(DOC).mdiViewsOfType("Gui::View3DInventor"))


def face_at(shape, point):
    for i, face in enumerate(shape.Faces):
        if face.isInside(point, 1e-6, True):
            return "Face%d" % (i + 1)
    raise AssertionError("no face at %s" % point)


def build():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt(
            "RenderCache", 3)
        V = FreeCAD.Vector
        doc = FreeCAD.newDocument(DOC, hidden=True)
        state["doc"] = doc
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObjectAt("PartDesign::AdditiveBox", "Box")
        box.Length, box.Width, box.Height = 40, 20, 10
        doc.recompute()
        import PMI

        pmi = PMI.getSet(body)
        doc.recompute()
        solid = body.SolidBodies[0]
        top = PMI.feature(pmi, solid, [face_at(solid.Shape, V(5, 5, 10))])
        flat = doc.addObject("PMI::GeometricTolerance", "Flatness")
        pmi.addObject(flat)
        flat.Characteristic, flat.Feature, flat.Value = "Flatness", top, 0.05
        bottom = PMI.feature(pmi, solid, [face_at(solid.Shape, V(5, 5, 0))])
        datum = doc.addObject("PMI::Datum", "DatumA")
        pmi.addObject(datum)
        datum.Feature, datum.Letter = bottom, "A"
        view = doc.addObject("PMI::View", "Top")
        pmi.addObject(view)
        view.Label = "Top"
        view.Standard = "Top"
        view.Height = 30
        flat.Views = [view]
        doc.recompute()
        state["view"] = view
        # Aim at the end of the flatness's leader furthest from the part:
        # what stays put (the first sub-shape), above the top face
        stays = flat.Presentation.SubShapes[0]
        aim = max((v.Point for v in stays.Vertexes), key=lambda p: p.z)
        state["aim"] = (aim.x, aim.y)

        port = free_port()
        ok = FreeCADGui.serveDocument(doc, port)
        if not check("the document is served headless", ok, "port %d" % port):
            finish()
            return
        FreeCADGui.serveSetGrants([{"token": VIEW_TOKEN, "access": 1}, {"access": 0}])
        state["client"] = Client(port, state["aim"])
        state["client"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("FAIL build:\n" + traceback.format_exc())
        finish()


def poll():
    client = state["client"]
    state["views"].append(views_3d())
    if client.is_alive():
        if clock() - state["t0"] > CLIENT_WAIT_S:
            check("the client finished", False, "still talking after %ds" % CLIENT_WAIT_S)
            finish()
            return
        QtCore.QTimer.singleShot(20, poll)
        return
    QtCore.QTimer.singleShot(300, verify)


def verify():
    client = state["client"]
    try:
        r = client.r
        check("the client ran without error", client.error is None, client.error)
        check("a snapshot came", client.snapshot)
        views = r.get("views") or {}
        top = r.get("top") or {}
        check("the flatness's menu has a PMI views submenu naming its view",
              bool(top), [e.get("text") for e in walk(views.get("items"))])
        check("the entry changes only this view: kind viewer, allowed on a view-only "
              "connection", top.get("kind") == "viewer" and top.get("allowed") is True, top)
        check("nothing in it opens a 3D view on the host",
              not any("new 3D view" in t for t in r.get("texts", [])), r.get("texts"))
        check("triggered, it runs", r.get("on") is not None and r["on"].get("ok", True)
              and "error" not in r["on"], r.get("on"))
        on_vis = r.get("on_vis") or {}
        check("the client is told what it hides: the datum, not in the view",
              len(on_vis.get("hidden", [])) >= 1, on_vis)
        cam = r.get("on_cam") or {}
        view = state["view"]
        q = view.Camera.Rotation.Q
        at = view.Camera.Base
        ok = (len(cam.get("q", [])) == 4 and all(abs(a - b) < 1e-6 for a, b in zip(cam["q"], q))
              and all(abs(a - b) < 1e-4 for a, b in zip(cam.get("at", []), at))
              and abs(cam.get("height", 0) - 30) < 1e-6)
        check("and is asked to take the view's camera: its rotation, target, height", ok,
              (cam, q, at))
        check("the menu again shows it checked", r.get("checked") == ["Top"], r.get("checked"))
        off = r.get("off_entry") or {}
        check("and offers Show all annotations, also the view's own",
              off.get("kind") == "viewer" and off.get("allowed") is True, off)
        check("let go, the client is told to restore its camera",
              (r.get("off_cam") or {}).get("restore") is True, r.get("off_cam"))
        check("and hides nothing", (r.get("off_vis") or {}).get("hidden", None) == [],
              r.get("off_vis"))
        check("no 3D view was opened on the host", max(state["views"] or [0]) == 0,
              sorted(set(state["views"])))
    except Exception:
        note("ABORT verify:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


# Deferred: a script handed to FreeCAD runs before the event loop is up.
QtCore.QTimer.singleShot(1500, build)
