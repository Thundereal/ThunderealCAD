"""A served user's undo with another's steps in its way asks that user
(docs/MultiViewEdit.md sec 12.11), against synthetic socket clients.

A box served to two users who may edit, alice and bob, each by a verified
identity. Asserted:

  - `undo.list` lists the asker's steps with their ids and groups; two of
    alice's grouped from the host are one group, and one undo takes both;
  - alice's step, its property set again since by bob, is refused for it:
    `InTheWay`, naming bob's step and saying what it changed may be kept;
  - alice asks to undo with it: bob is asked on his connection, with what
    and whose; he agrees, and alice is told it went -- the box as before
    either step;
  - the same asked again, bob says no: alice is told, and nothing moved;
  - the desktop user's step in the way: asked in a dialog that does not
    stop the desktop; agreeing there, alice's undo goes;
  - a user with no connection cannot be asked.
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
from PySide import QtCore

import wsclient
from wsclient import WS, free_port

# The platform's backend, as a user gets it: Direct3D 11 on Windows, Metal
# on macOS, OpenGL elsewhere (RenderParams::engineType, docs/Testing.md).
ENGINE = "Default"

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeUndoConsent"
ALICE = "alice@example.com"
BOB = "bob@example.com"

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


class Client(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.error = None
        self.got = {}
        self.n = 100

    def run(self):
        try:
            self.talk()
        except Exception:
            self.error = traceback.format_exc()
        client_done.set()

    def do(self, ws, op, **fields):
        self.n += 1
        raw = ws.op(json.dumps(dict({"id": self.n, "op": op}, **fields)))
        return json.loads(raw.decode("utf-8")) if raw else None

    def join(self, label, identity):
        ws = WS(self.port, headers="%s: %s\r\n" % (wsclient.IDENTITY_HEADER, identity))
        ws.hello(label)
        if ws.next_binary(20.0) is None:
            raise RuntimeError("no snapshot for %s" % label)
        ws.drain(0.5)
        return ws

    def write(self, ws, value):
        reply = self.do(ws, "setProperty", obj="Box", target="object", name="Length",
                        value=value)
        ws.drain(0.3)
        time.sleep(3.5)   # past the log's folding of rapid writes (sec 37.4 P4)
        return reply

    def talk(self):
        got = self.got
        alice = self.join("alice", ALICE)
        bob = self.join("bob", BOB)

        # alice's two steps, grouped from the host: one undo takes both
        got["len0"] = on_host(length)
        self.write(alice, 11.0)
        self.write(alice, 12.0)
        got["list"] = self.do(alice, "undo.list")
        ids = [s["id"] for s in (got["list"] or {}).get("undoSteps", [])[:2]]
        got["grouped"] = on_host(lambda: FreeCAD.getDocument(DOC).groupSteps(ids))
        got["list2"] = self.do(alice, "undo.list")
        got["undo_group"] = self.do(alice, "undo")
        got["len_group"] = on_host(length)

        # alice's step, its property set again since by bob
        self.write(alice, 20.0)
        self.write(bob, 25.0)
        got["refused"] = self.do(alice, "undo")
        got["len_refused"] = on_host(length)
        theirs = [s["id"] for s in (got["refused"] or {}).get("steps", [])]

        # asked with them: bob agrees
        got["asked"] = self.do(alice, "undo", **{"with": theirs})
        got["question"] = wait_op(bob, "consent.ask", 10.0)
        request = (got["question"] or {}).get("request", 0)
        got["answer"] = self.do(bob, "consent.answer", request=request, allow=True)
        got["result"] = wait_op(alice, "consent.result", 10.0)
        got["len_allowed"] = on_host(length)

        # again, and bob says no
        self.write(alice, 30.0)
        self.write(bob, 35.0)
        refused = self.do(alice, "undo")
        theirs = [s["id"] for s in (refused or {}).get("steps", [])]
        self.do(alice, "undo", **{"with": theirs})
        question = wait_op(bob, "consent.ask", 10.0)
        self.do(bob, "consent.answer", request=(question or {}).get("request", 0), allow=False)
        got["denied"] = wait_op(alice, "consent.result", 10.0)
        got["len_denied"] = on_host(length)

        # the desktop user's step in the way: asked in a dialog that does
        # not stop the desktop, and agreeing there
        self.write(alice, 40.0)

        def host_write():
            doc = FreeCAD.getDocument(DOC)
            doc.openTransaction("Host length")
            doc.getObject("Box").Length = 45.0
            doc.commitTransaction()
        on_host(host_write)
        time.sleep(3.5)
        refused = self.do(alice, "undo")
        got["host_refused"] = refused
        theirs = [s["id"] for s in (refused or {}).get("steps", [])]
        got["host_asked"] = self.do(alice, "undo", **{"with": theirs})

        def allow_on_desktop():
            from PySide import QtWidgets
            for w in QtWidgets.QApplication.topLevelWidgets():
                if isinstance(w, QtWidgets.QMessageBox) and w.objectName() == "undoConsentAsk":
                    text = w.text()
                    for b in w.buttons():
                        if b.text() == "Allow":
                            b.click()
                            return text
            return None
        time.sleep(0.5)
        got["host_question"] = on_host(allow_on_desktop)
        got["host_result"] = wait_op(alice, "consent.result", 10.0)
        got["len_host"] = on_host(length)

        # bob gone: he cannot be asked
        self.write(alice, 50.0)
        self.write(bob, 55.0)
        refused = self.do(alice, "undo")
        theirs = [s["id"] for s in (refused or {}).get("steps", [])]
        bob.close()
        time.sleep(1.0)
        got["gone"] = self.do(alice, "undo", **{"with": theirs})
        alice.close()


def verify():
    if state["done"]:
        return
    client = state["client"]
    got = client.got
    if client.error:
        note("ABORT client " + client.error.replace("\n", " | "))
    note("  got %s" % json.dumps({k: v for k, v in got.items()}, default=str)[:3000])
    groups = [s.get("group") for s in (got.get("list2") or {}).get("undoSteps", [])[:2]]
    check("undo.list lists the asker's steps, two grouped as one group",
          got.get("grouped") and groups[0] and groups[0] == groups[1], groups)
    check("one undo takes the group", got.get("len_group") == got.get("len0"),
          (got.get("len_group"), got.get("len0")))
    refused = got.get("refused") or {}
    steps = refused.get("steps", [])
    check("the undo is refused for bob's step, which it names, theirs keepable",
          refused.get("code") == "InTheWay" and steps and steps[0].get("author") == BOB
          and refused.get("keepable") is True and got.get("len_refused") == 25.0, refused)
    check("asked with it, alice is told it waits for bob",
          (got.get("asked") or {}).get("code") == "Asked", got.get("asked"))
    question = got.get("question") or {}
    check("bob is asked, with what and whose",
          question.get("from") == ALICE and question.get("steps"), question)
    result = got.get("result") or {}
    check("bob agrees: alice is told it went, the box as before either step",
          (got.get("answer") or {}).get("ok") and result.get("allowed") and result.get("done")
          and got.get("len_allowed") == got.get("len0"), (result, got.get("len_allowed")))
    denied = got.get("denied") or {}
    check("bob says no: alice is told, and nothing moved",
          denied.get("allowed") is False and not denied.get("done")
          and got.get("len_denied") == 35.0, (denied, got.get("len_denied")))
    host_refused = got.get("host_refused") or {}
    check("the desktop user's step in the way is named as the host's",
          host_refused.get("code") == "InTheWay"
          and (host_refused.get("steps") or [{}])[0].get("author") == "host", host_refused)
    host_result = got.get("host_result") or {}
    check("the desktop user is asked in a dialog, agrees, and alice's undo goes",
          got.get("host_question") and "alice" in got.get("host_question")
          and host_result.get("allowed") and host_result.get("done")
          and got.get("len_host") == 35.0,
          (got.get("host_question"), host_result, got.get("len_host")))
    check("a user with no connection cannot be asked",
          (got.get("gone") or {}).get("code") == "NotConnected", got.get("gone"))
    finish()


def build():
    try:
        p = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")
        state["log"] = p.GetInt("TransactionLog", 2)
        p.SetInt("TransactionLog", 2)
        p.SetInt("AutoSaveTimeout", 0)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE)
        doc = FreeCAD.newDocument(DOC, hidden=True)
        doc.UndoMode = 1
        doc.openTransaction("base")
        box = doc.addObject("Part::Box", "Box")
        doc.recompute()
        doc.commitTransaction()
        doc.saveAs(os.path.join(tempfile.mkdtemp(prefix="fc-serve-consent-"), "c.FCStd"))
        port = free_port()
        if not check("the document is served with no window",
                     FreeCADGui.serveDocument(doc, port), "port %d" % port):
            finish()
            return
        # 0 edit, 1 view-only (Render::SceneGrant)
        FreeCADGui.serveSetGrants([{"identity": ALICE, "access": 0},
                                   {"identity": BOB, "access": 0}, {"access": 1}])
        client = Client(port)
        state["client"] = client
        QtCore.QTimer.singleShot(20, serve_asked)
        client.start()
        QtCore.QTimer.singleShot(150000, lambda: (check("the run ended", state["done"]), finish()))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
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
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
        "TransactionLog", state.get("log", 2))
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, build)
