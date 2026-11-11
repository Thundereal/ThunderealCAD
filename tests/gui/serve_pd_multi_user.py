"""Two served users editing one PartDesign body, with PartDesign's edit
preview on or off (docs/MultiViewEdit.md sec 12.12): both are to work.
Run by serve-pd-multi-user-preview.py and serve-pd-multi-user-no-preview.py.

A body: Pad1, a 10 x 10 square padded 15, and Pad2, a 4 x 4 square on a
sketch at z = 15 padded 5 -- its solid's height and volume say which
lengths it was made of. The document has a desktop window and is served
to alice and bob, who may edit, each with a camera (so each has a view of
its own); PerViewEdit on. A user's panel is driven as a browser drives
it, through the panel mirror: a value written to its length field, its
OK and Cancel.

Claims, both ways:
  1. one editor: alice's Pad1 to 18 in her panel; with the preview her
     Pad1 waits for her OK, without it it is made at once; bob's
     Pad2 set outside an edit meanwhile is made on what the body has; her
     OK makes both; her one undo takes her edit, one group;
  2. Cancel: alice's Pad1 to 20, bob's Pad2 set meanwhile; her Cancel
     takes hers back and keeps his, asking nothing;
  3. two editors at once: alice in Pad1, bob in Pad2 -- each has a
     panel of their own and is sent no other's; alice's OK makes her
     Pad1 and leaves bob in his; bob's OK makes his; the body is both
     their values.
"""
import json
import os
import queue
import tempfile
import threading
import time
import traceback

import FreeCAD
import FreeCADGui
import Part
from PySide import QtCore, QtWidgets

import wsclient
from wsclient import WS, free_port

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
V = FreeCAD.Vector
ALICE = "alice@example.com"
BOB = "bob@example.com"
EYE = (5.0, -60.0, 40.0)
QUAT = (0.5, 0.0, 0.0, 0.8660254)
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
PART = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Part")
DOCP = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
DOCP.SetBool("AutoSaveEnabled", False)

state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()
    for _ in range(5):
        QtCore.QCoreApplication.processEvents()


def pace():
    """Past the log's folding of rapid writes (TransactionLogJournal.md sec 37.4 P4)"""
    settle(3500)


class User(threading.Thread):
    """A user's connection, doing what the GUI thread hands it, one at a time"""

    def __init__(self, port, name, identity):
        super().__init__(daemon=True)
        self.port = port
        self.name = name
        self.identity = identity
        self.jobs = queue.Queue()
        self.done = queue.Queue()
        self.n = 100
        self.ws = None

    def op(self, op, **fields):
        self.n += 1
        raw = self.ws.op(json.dumps(dict({"id": self.n, "op": op}, **fields)))
        self.ws.drain(0.3)
        return json.loads(raw.decode("utf-8")) if raw else None

    def run(self):
        while True:
            job, arg = self.jobs.get()
            if job == "stop":
                return
            try:
                if job == "join":
                    ws = WS(self.port,
                            headers="%s: %s\r\n" % (wsclient.IDENTITY_HEADER, self.identity))
                    ws.hello(self.name)
                    if ws.next_binary(20.0) is None:
                        raise RuntimeError("no snapshot for " + self.name)
                    ws.send(2, wsclient.camera_frame(EYE, QUAT, 0.785, 1.0, 500.0, 800, 600))
                    ws.drain(0.5)
                    self.ws = ws
                    result = True
                elif job == "opened":
                    # the panel roots this connection was sent an open of
                    self.ws.drain(0.3)
                    roots = []
                    for text in self.ws.pushes:
                        try:
                            msg = json.loads(text.decode("utf-8"))
                        except Exception:
                            continue
                        if msg.get("op") == "widgets" and msg.get("method") == "open" \
                                and str(msg.get("id", "")).startswith("panel:"):
                            roots.append(msg["id"])
                    result = roots
                elif job == "close":
                    self.ws.close()
                    result = True
                else:
                    result = self.op(job, **(arg or {}))
                self.done.put((True, result))
            except Exception:
                self.done.put((False, traceback.format_exc()))


def does(user, job, arg=None, timeout=60.0):
    """Hand a user a job; the GUI turns until it is done"""
    user.jobs.put((job, arg))
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ok, result = user.done.get_nowait()
        except queue.Empty:
            settle(50)
            continue
        if not ok:
            raise RuntimeError("%s, %s: %s" % (user.name, job, result))
        settle(300)
        return result
    raise RuntimeError("%s, %s: no answer" % (user.name, job))


ASKED = []


def dismiss_dialogs():
    """Any question an undo or a Cancel puts on the desktop: kept, and closed
    as "Keep the edit" / Cancel would, so nothing waits on it"""
    for w in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(w, QtWidgets.QMessageBox) and w.isVisible() and w.isModal():
            ASKED.append((w.objectName(), w.text()))
            w.reject()
    if not state["done"]:
        QtCore.QTimer.singleShot(200, dismiss_dialogs)


def field_under(root, name):
    """The mirrored widget `name` under the panel root `root`"""
    fw = FreeCADGui.FormWidgets
    for wid in fw.ids():
        if not wid.startswith("pw:"):
            continue
        snap = fw.snapshot(wid)
        if snap["state"].get("q_objectName") != name:
            continue
        up, seen = wid, set()
        while up and up not in seen:
            seen.add(up)
            if up == root:
                return wid
            parent = fw.snapshot(up).get("parent") if up in fw.ids() else None
            up = parent[len("IPY_MODEL_"):] if parent else None
    return None


def own_panel(user):
    """The panel root the user is told is theirs (widgets.subscribe)"""
    reply = does(user, "widgets.subscribe", {"panels": True}) or {}
    settle(800)
    return reply.get("panel")


def type_length(user, value):
    """The user's own panel's length field written, as the browser writes it"""
    root = own_panel(user)
    field = field_under(root, "lengthEdit") if root else None
    if not field:
        note("  %s: no length field in its own panel %s" % (user.name, root))
        return False
    reply = does(user, "widgets.update", {"target": field, "state": {"q_rawValue": value}})
    settle(1500)
    return bool(reply and reply.get("ok"))


def press(user, event):
    """The user's own panel's OK ("accept") or Cancel ("reject")"""
    root = own_panel(user)
    if not root:
        return False
    reply = does(user, "widgets.custom", {"target": root, "content": {"event": event}})
    settle(2000)
    return bool(reply and reply.get("ok"))


def rectangle(body, name, x0, y0, size, z):
    doc = body.Document
    sk = body.newObject("Sketcher::SketchObject", name)
    sk.AttachmentSupport = [(body.Origin.OriginFeatures[3], "")]   # XY plane
    sk.MapMode = "FlatFace"
    sk.AttachmentOffset = FreeCAD.Placement(V(0, 0, z), FreeCAD.Rotation())
    p = [V(x0, y0, 0), V(x0 + size, y0, 0), V(x0 + size, y0 + size, 0), V(x0, y0 + size, 0)]
    for i in range(4):
        sk.addGeometry(Part.LineSegment(p[i], p[(i + 1) % 4]))
    doc.recompute()
    return sk


def solid(doc):
    """The body's solid: (height, volume), rounded"""
    # the tip's own solid, as the views show the body; Body.Shape follows
    # only when the body recomputes
    shape = doc.getObject("Pad2").Shape
    return round(shape.BoundBox.ZMax, 3), round(shape.Volume, 1)


def expect(l1, l2):
    """What the solid is for Pad1 length l1 and Pad2 length l2"""
    return round(max(l1, 15 + l2), 3), round(100 * l1 + 16 * max(0, 15 + l2 - l1), 1)


def length(doc, name):
    return round(doc.getObject(name).Length.Value, 3)


def run(preview):
    users = []
    tag = "preview" if preview else "no preview"
    try:
        state["log"] = DOCP.GetInt("TransactionLog", 2)
        DOCP.SetInt("TransactionLog", 2)
        DOCP.SetInt("AutoSaveTimeout", 0)
        VIEW.SetBool("PerViewEdit", True)
        PART.SetBool("PreviewOnEdit", preview)
        doc = FreeCAD.newDocument("PdMultiUser")
        body = doc.addObject("PartDesign::Body", "Body")
        pad1 = body.newObject("PartDesign::Pad", "Pad1")
        pad1.Profile = rectangle(body, "Sketch1", 0, 0, 10, 0)
        pad1.Length = 15
        pad2 = body.newObject("PartDesign::Pad", "Pad2")
        pad2.Profile = rectangle(body, "Sketch2", 3, 3, 4, 15)
        pad2.Length = 5
        doc.recompute()
        doc.UndoMode = 1
        doc.saveAs(os.path.join(tempfile.mkdtemp(prefix="fc-pd-multi-"), "m.FCStd"))
        doc.clearUndos()
        settle(1000)
        check("the body as built (%s)" % tag, solid(doc) == expect(15, 5), solid(doc))

        port = free_port()
        if not check("the document is served", FreeCADGui.serveDocument(doc, port)):
            return
        FreeCADGui.serveSetGrants([{"identity": ALICE, "access": 0},
                                   {"identity": BOB, "access": 0}, {"access": 1}])
        alice = User(port, "alice", ALICE)
        bob = User(port, "bob", BOB)
        users = [alice, bob]
        for u in users:
            u.start()
            does(u, "join")
        QtCore.QTimer.singleShot(200, dismiss_dialogs)

        # 1. One editor, the other changing the body outside an edit
        entered = does(alice, "edit", {"obj": "Pad1"})
        typed = type_length(alice, 18.0)
        check("1: alice's Pad1 in edit, 18 typed in her panel (%s)" % tag,
              (entered or {}).get("ok") and typed and length(doc, "Pad1") == 18.0,
              (entered, typed, length(doc, "Pad1")))
        # her Pad1 as made: typing makes the feature in edit, and what is
        # after it follows at the next recompute
        pad1_made = round(doc.getObject("Pad1").Shape.BoundBox.ZMax, 3)
        if preview:
            check("1: with the preview her Pad1 waits for her OK",
                  pad1_made == 15.0 and solid(doc) == expect(15, 5), (pad1_made, solid(doc)))
        else:
            check("1: without the preview her Pad1 follows at once",
                  pad1_made == 18.0, pad1_made)
        pace()
        does(bob, "setProperty", {"obj": "Pad2", "target": "object", "name": "Length",
                                  "value": 7.0})
        made_on = 15 if preview else 18
        check("1: bob's Pad2 set meanwhile is made on what the body has (%s)" % tag,
              length(doc, "Pad2") == 7.0 and solid(doc) == expect(made_on, 7),
              (solid(doc), expect(made_on, 7)))
        pace()
        pressed = press(alice, "accept")
        check("1: her OK makes both (%s)" % tag,
              pressed and FreeCADGui.getDocument(doc.Name).getInEdit() is None
              and solid(doc) == expect(18, 7), (pressed, solid(doc), expect(18, 7)))
        pace()
        listed = does(alice, "undo.list") or {}
        mine = listed.get("undoSteps", [])
        groups = {s.get("group") for s in mine if s.get("group")}
        # one step needs no group: without the preview her Pad1 is made as
        # she types, her OK has nothing left to make, and a step that
        # changed nothing is none
        check("1: her edit is one undo in her list, one group or one step (%s)" % tag,
              len(groups) == 1 or (not groups and len(mine) == 1),
              [(s.get("name"), s.get("group")) for s in mine])
        undone = does(alice, "undo")
        settle(1000)
        before = solid(doc)
        doc.recompute()
        check("1: her one undo takes her edit, bob's stays (%s)" % tag,
              (undone or {}).get("ok") and length(doc, "Pad1") == 15.0
              and length(doc, "Pad2") == 7.0 and solid(doc) == expect(15, 7),
              (undone, length(doc, "Pad1"), before, solid(doc), expect(15, 7)))
        pace()

        # 2. Cancel, with bob's change meanwhile
        does(alice, "edit", {"obj": "Pad1"})
        type_length(alice, 20.0)
        pace()
        does(bob, "setProperty", {"obj": "Pad2", "target": "object", "name": "Length",
                                  "value": 9.0})
        pace()
        asked = len(ASKED)
        pressed = press(alice, "reject")
        settle(1500)
        before = solid(doc)
        doc.recompute()
        check("2: her Cancel takes hers back and keeps his, asking nothing (%s)" % tag,
              pressed and length(doc, "Pad1") == 15.0 and length(doc, "Pad2") == 9.0
              and solid(doc) == expect(15, 9) and len(ASKED) == asked,
              (pressed, length(doc, "Pad1"), length(doc, "Pad2"), before, solid(doc),
               ASKED[asked:]))
        pace()

        # 3. Two editors at once
        a_in = does(alice, "edit", {"obj": "Pad1"})
        a_typed = type_length(alice, 18.0)
        pace()
        b_in = does(bob, "edit", {"obj": "Pad2"})
        b_typed = type_length(bob, 7.0)
        a_root, b_root = own_panel(alice), own_panel(bob)
        check("3: both in edit at once, each value typed (%s)" % tag,
              (a_in or {}).get("ok") and (b_in or {}).get("ok") and a_typed and b_typed
              and length(doc, "Pad1") == 18.0 and length(doc, "Pad2") == 7.0,
              (a_in, b_in, a_typed, b_typed, length(doc, "Pad1"), length(doc, "Pad2")))
        a_sent, b_sent = does(alice, "opened"), does(bob, "opened")
        check("3: each keeps a panel of their own, and is sent no other's (%s)" % tag,
              a_root and b_root and a_root != b_root
              and a_root in a_sent and b_root not in a_sent
              and b_root in b_sent and a_root not in b_sent,
              (a_root, b_root, a_sent, b_sent))
        pace()
        # alice's OK in her own panel, with bob's open beside it
        pressed = press(alice, "accept")
        settle(1500)
        pad1_made = doc.getObject("Pad1").Shape.BoundBox.ZMax
        check("3: alice's OK makes her Pad1, bob still in his edit (%s)" % tag,
              pressed and round(pad1_made, 3) == 18.0 and own_panel(bob) == b_root,
              (pressed, pad1_made, own_panel(bob), b_root))
        if preview:
            check("3: and leaves bob's Pad2 in its preview, waiting for his OK",
                  solid(doc) == expect(15, 9), (solid(doc), expect(15, 9)))
        pace()
        pressed = press(bob, "accept")
        settle(1500)
        check("3: bob's OK makes his; the body is both their values (%s)" % tag,
              pressed and length(doc, "Pad1") == 18.0 and length(doc, "Pad2") == 7.0
              and solid(doc) == expect(18, 7), (pressed, solid(doc), expect(18, 7)))
        check("3: nobody is left in edit (%s)" % tag,
              FreeCADGui.getDocument(doc.Name).getInEdit() is None)
        note("  questions put on the desktop: %s" % ASKED)
    except Exception:
        note("FAIL run:\n" + traceback.format_exc())
    finally:
        for u in users:
            u.jobs.put(("stop", None))
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.stopServing()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    DOCP.SetInt("TransactionLog", state.get("log", 2))
    PART.RemBool("PreviewOnEdit")
    VIEW.RemBool("PerViewEdit")
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


def start(preview):
    QtCore.QTimer.singleShot(1500, lambda: run(preview))
