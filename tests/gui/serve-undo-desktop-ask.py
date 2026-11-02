"""The desktop user's undo with a served user's step in its way asks that
user (docs/MultiViewEdit.md sec 12.11): the desktop half of
tests/gui/serve-undo-consent.py, which has a served user ask.

A box in a document open on the desktop and served to bob, who may edit,
by a verified identity. The desktop sets its length, then bob sets it
again; the desktop's Std_Undo, in its 3D view, is refused for bob's step.

Claims:
  - the dialog names bob's step, by bob, and offers to ask him in place of
    undoing with it;
  - asked, the desktop is told it waits; bob is asked on his connection,
    as from the desktop user, with his step by name; he agrees, the
    desktop's undo goes with his step, and the desktop is told in a box
    that does not stop it;
  - asked again, bob says no: nothing moved, and the desktop is told;
  - bob gone, the desktop is told he cannot be asked, and nothing moved.
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
from PySide import QtCore, QtWidgets

import wsclient
from wsclient import WS, free_port

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeUndoDesktopAsk"
BOB = "bob@example.com"
FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
    "AutoSaveEnabled", False)


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


def length():
    return round(FreeCAD.getDocument(DOC).getObject("Box").Length.Value, 3)


def wait_op(ws, op, timeout):
    """The next push of `op`, looking first at what op() set aside"""
    key = b'"op":"' + op.encode() + b'"'
    for i, text in enumerate(ws.pushes):
        if key in text:
            ws.pushes.pop(i)
            return json.loads(text.decode("utf-8"))
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = ws.next_text(deadline - time.time())
        if text is None:
            return None
        if key in text:
            return json.loads(text.decode("utf-8"))
        ws.pushes.append(text)
    return None


class Bob(threading.Thread):
    """bob's connection, doing what the GUI thread hands it, one at a time"""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.jobs = queue.Queue()
        self.done = queue.Queue()
        self.n = 100

    def do(self, ws, op, **fields):
        self.n += 1
        raw = ws.op(json.dumps(dict({"id": self.n, "op": op}, **fields)))
        return json.loads(raw.decode("utf-8")) if raw else None

    def run(self):
        ws = None
        while True:
            job, arg = self.jobs.get()
            try:
                if job == "join":
                    ws = WS(self.port, headers="%s: %s\r\n" % (wsclient.IDENTITY_HEADER, BOB))
                    ws.hello("bob")
                    if ws.next_binary(20.0) is None:
                        raise RuntimeError("no snapshot for bob")
                    ws.drain(0.5)
                    result = True
                elif job == "write":
                    result = self.do(ws, "setProperty", obj="Box", target="object",
                                     name="Length", value=arg)
                    ws.drain(0.3)
                elif job == "answer":
                    question = wait_op(ws, "consent.ask", 15.0)
                    if question:
                        self.do(ws, "consent.answer", request=question.get("request", 0),
                                allow=arg)
                    result = question
                elif job == "close":
                    ws.close()
                    result = True
                else:
                    return
                self.done.put((True, result))
            except Exception:
                self.done.put((False, traceback.format_exc()))


def bob_does(bob, job, arg=None, timeout=30.0):
    """Hand bob a job; the GUI turns until he is done with it"""
    bob.jobs.put((job, arg))
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ok, result = bob.done.get_nowait()
        except queue.Empty:
            settle(50)
            continue
        if not ok:
            raise RuntimeError("bob, %s: %s" % (job, result))
        return result
    raise RuntimeError("bob, %s: no answer" % job)


SEEN = {}


def answer_dialog():
    """Click the in-the-way dialog's "Ask ..." once it is up; what it said"""
    SEEN.clear()

    def find():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMessageBox) and w.objectName() == "undoInTheWay" \
                    and w.isVisible():
                SEEN["text"] = w.text()
                SEEN["steps"] = w.informativeText()
                SEEN["buttons"] = [b.text() for b in w.buttons()]
                for b in w.buttons():
                    if b.text().startswith("Ask "):
                        b.click()
                        return
                w.reject()
                return
        QtCore.QTimer.singleShot(200, find)
    QtCore.QTimer.singleShot(300, find)


def boxes(name):
    """The texts of the visible message boxes named `name`, closed"""
    out = []
    for w in QtWidgets.QApplication.topLevelWidgets():
        if isinstance(w, QtWidgets.QMessageBox) and w.objectName() == name and w.isVisible():
            out.append(w.text())
            w.close()
    return out


def warning_later(found):
    """Close the next plain warning box (it is modal), keeping its text"""
    def find():
        for w in QtWidgets.QApplication.topLevelWidgets():
            if isinstance(w, QtWidgets.QMessageBox) and w.isVisible() \
                    and w.objectName() not in ("undoInTheWay", "undoConsentAsked",
                                               "undoConsentResult"):
                found.append(w.text())
                w.accept()
                return
        QtCore.QTimer.singleShot(200, find)
    QtCore.QTimer.singleShot(300, find)


def desktop_write(value):
    doc = FreeCAD.getDocument(DOC)
    doc.openTransaction("Desktop length")
    doc.getObject("Box").Length = value
    doc.commitTransaction()


def desktop_undo(view):
    FreeCADGui.getMainWindow().setActiveWindow(view)
    settle(150)
    FreeCADGui.runCommand("Std_Undo")
    settle(800)


def run():
    bob = None
    try:
        p = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
        state["log"] = p.GetInt("TransactionLog", 2)
        p.SetInt("TransactionLog", 2)
        p.SetInt("AutoSaveTimeout", 0)
        doc = FreeCAD.newDocument(DOC)
        doc.UndoMode = 1
        doc.openTransaction("base")
        doc.addObject("Part::Box", "Box")
        doc.recompute()
        doc.commitTransaction()
        doc.saveAs(os.path.join(tempfile.mkdtemp(prefix="fc-undo-desktop-"), "d.FCStd"))
        settle(1000)
        view = FreeCADGui.getDocument(DOC).mdiViewsOfType("Gui::View3DInventor")[0]
        port = free_port()
        if not check("the document is served", FreeCADGui.serveDocument(doc, port),
                     "port %d" % port):
            return
        # 0 edit, 1 view-only (Render::SceneGrant)
        FreeCADGui.serveSetGrants([{"identity": BOB, "access": 0}, {"access": 1}])
        bob = Bob(port)
        bob.start()
        bob_does(bob, "join")
        len0 = length()

        # the desktop's step, then bob's on the same property
        desktop_write(11.0)
        pace()
        bob_does(bob, "write", 25.0)
        pace()
        check("both steps made", length() == 25.0, length())

        # Std_Undo: refused for bob's step, asked of him; he agrees
        answer_dialog()
        desktop_undo(view)
        asked = boxes("undoConsentAsked")
        check("the dialog names bob's step, by bob, and offers to ask him",
              SEEN.get("text", "").startswith("'Desktop length' cannot be undone")
              and BOB in SEEN.get("steps", "")
              and ("Ask " + BOB) in SEEN.get("buttons", [])
              and "Undo with them" not in SEEN.get("buttons", []), SEEN)
        check("asked, the desktop is told it waits, and nothing moved yet",
              asked and "Asked" in asked[0] and length() == 25.0, (asked, length()))
        question = bob_does(bob, "answer", True) or {}
        check("bob is asked, as from the desktop user, with his step by name",
              question.get("from") == "the desktop user"
              and question.get("what") == "Desktop length" and question.get("steps"),
              question)
        settle(1500)
        said = boxes("undoConsentResult")
        check("he agrees: the desktop's undo goes with his step, the box as before both",
              length() == len0, (length(), len0))
        check("and the desktop is told in a box that does not stop it",
              said and "agreed" in said[0] and "is undone" in said[0], said)

        # again, and bob says no
        desktop_write(30.0)
        pace()
        bob_does(bob, "write", 35.0)
        pace()
        answer_dialog()
        desktop_undo(view)
        boxes("undoConsentAsked")
        bob_does(bob, "answer", False)
        settle(1500)
        said = boxes("undoConsentResult")
        check("bob says no: nothing moved, and the desktop is told",
              length() == 35.0 and said and "did not agree" in said[0], (length(), said))

        # bob gone: he cannot be asked
        desktop_write(40.0)
        pace()
        bob_does(bob, "write", 45.0)
        pace()
        bob_does(bob, "close")
        settle(1000)
        warned = []
        answer_dialog()
        warning_later(warned)
        desktop_undo(view)
        settle(1000)
        check("bob gone: the desktop is told he cannot be asked, and nothing moved",
              length() == 45.0 and warned and "cannot be asked" in warned[0]
              and "not connected" in warned[0], (length(), warned))
    except Exception:
        note("FAIL run:\n" + traceback.format_exc())
    finally:
        if bob is not None:
            bob.jobs.put(("stop", None))
        finish()


state = {"done": False}


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
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
        "TransactionLog", state.get("log", 2))
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
