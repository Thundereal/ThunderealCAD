"""A real browser's undo list (docs/MultiViewEdit.md sec 12.11): the browser
half of tests/gui/serve-undo-consent.py, which checks the ops against
synthetic socket clients. What no socket client can check is the page
(web/src/undolist.tsx): the corner button's list, a group's row and its
steps, the card an undo refused for another's step raises, the question
on the other user's page and what the page says after.

A box served to two users invited by name, alice and bob. Their steps are
made through sockets of theirs before the pages open -- alice's two first,
grouped from the host as an edit's are, then one more of alice's, then one
of bob's on the same property -- and Chrome drives both pages through
scripts/undo-drive.js. Asserted:

  - alice's list has two rows: her newest step, and the group as one row
    of two steps, which opens to them;
  - taking her newest row is refused for bob's step: the card says it, and
    offers to ask bob and to keep his;
  - asked, her page says it waits; bob's page asks him, naming alice; he
    allows, and alice's page says it went;
  - the host has the box as alice's group left it.

Needs the built viewer (build/wasm/fcviewer.html; `ninja -C build/wasm`),
node, a puppeteer-core install named by PUPPETEER_PATH, and a Chrome named
by CHROME. Skips rather than fails when any is missing.
"""
import functools
import http.server
import json
import os
import sys
import subprocess
import tempfile
import threading
import time
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
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WASM = os.path.join(REPO, "build", "wasm")
DRIVER = os.path.join(REPO, "scripts", "undo-drive.js")
DOC = "ServeUndoBrowser"
TOKENS = {"alice": "undo-browser-alice", "bob": "undo-browser-bob"}

state = {"done": False}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def skip(why):
    note("SKIP " + why)
    finish()


def length():
    return round(FreeCAD.getDocument(DOC).getObject("Box").Length.Value, 3)


class Steps(threading.Thread):
    """The steps, made through alice's and bob's own sockets"""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.error = None
        self.n = 1
        self.ids = []

    def join_as(self, name):
        ws = WS(self.port, "/scene?token=%s" % TOKENS[name], headers="")
        ws.hello(name)
        if ws.next_binary(20.0) is None:
            raise RuntimeError("no snapshot for %s" % name)
        ws.drain(0.5)
        return ws

    def do(self, ws, op, **fields):
        self.n += 1
        raw = ws.op(json.dumps(dict({"id": self.n, "op": op}, **fields)))
        return json.loads(raw.decode("utf-8")) if raw else None

    def write(self, ws, value):
        self.do(ws, "setProperty", obj="Box", target="object", name="Length", value=value)
        ws.drain(0.3)
        time.sleep(3.5)   # past the log's folding of rapid writes

    def run(self):
        try:
            alice = self.join_as("alice")
            bob = self.join_as("bob")
            self.write(alice, 11.0)
            self.write(alice, 12.0)
            listed = self.do(alice, "undo.list") or {}
            self.ids = [s["id"] for s in listed.get("undoSteps", [])[:2]]
            self.write(alice, 20.0)
            self.write(bob, 25.0)
            alice.close()
            bob.close()
        except Exception:
            self.error = traceback.format_exc()


class Run(threading.Thread):
    """The browser, driven from off the GUI thread"""

    def __init__(self, urls):
        super().__init__(daemon=True)
        self.urls = urls
        self.code = None
        self.log_path = os.path.join(OUT, "drive.log")

    def run(self):
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = (os.path.join(REPO, ".conda", "freecad", "lib")
                                  + os.pathsep + env.get("LD_LIBRARY_PATH", ""))
        with open(self.log_path, "w") as log:
            proc = subprocess.Popen(["node", DRIVER] + self.urls + ["90000"],
                                    stdout=log, stderr=subprocess.STDOUT, env=env)
            proc.wait()
        self.code = proc.returncode

    def report(self):
        try:
            with open(self.log_path) as f:
                for line in f:
                    if line.startswith("REPORT "):
                        return json.loads(line[len("REPORT "):])
        except Exception:
            pass
        return {}

    def tail(self, lines=25):
        try:
            with open(self.log_path) as f:
                return "".join(f.readlines()[-lines:])
        except OSError:
            return "(no driver log)"


def serve_viewer():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=WASM)
    handler.log_message = lambda *a, **k: None
    port = free_port()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    state["httpd"] = server
    return port


def build():
    try:
        if not os.path.exists(os.path.join(WASM, "fcviewer.html")):
            return skip("no built viewer at %s (ninja -C build/wasm)" % WASM)
        if not os.environ.get("PUPPETEER_PATH"):
            return skip("PUPPETEER_PATH is unset (a puppeteer-core install)")
        if not os.environ.get("CHROME"):
            return skip("CHROME is unset (a Chrome executable)")
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
        doc.addObject("Part::Box", "Box")
        doc.recompute()
        doc.commitTransaction()
        doc.saveAs(os.path.join(tempfile.mkdtemp(prefix="fc-undo-browser-"), "u.FCStd"))
        port = free_port()
        if not check("the document is served headless", FreeCADGui.serveDocument(doc, port)):
            return finish()
        FreeCADGui.serveSetGrants(wsclient.invitation(TOKENS["alice"], "alice")
                                  + wsclient.invitation(TOKENS["bob"], "bob"))
        state["port"] = port
        state["http"] = serve_viewer()
        steps = Steps(port)
        state["steps"] = steps
        steps.start()
        QtCore.QTimer.singleShot(100, made)
    except Exception:
        note("FAIL build:\n" + traceback.format_exc())
        finish()


def made():
    steps = state["steps"]
    if steps.is_alive():
        QtCore.QTimer.singleShot(100, made)
        return
    if steps.error:
        note("FAIL steps:\n" + steps.error)
        return finish()
    # alice's first two are one edit's, grouped as an edit ended with OK is
    state["grouped"] = FreeCAD.getDocument(DOC).groupSteps(steps.ids)
    state["len_group"] = 12.0
    urls = ["http://127.0.0.1:%d/fcviewer.html?scene=http://127.0.0.1:%d&doc=%s"
            "&token=%s&client=%s" % (state["http"], state["port"], DOC, TOKENS[n], n)
            for n in ("alice", "bob")]
    run = Run(urls)
    state["run"] = run
    state["t0"] = time.time()
    run.start()
    QtCore.QTimer.singleShot(100, poll)


def poll():
    run = state["run"]
    if run.is_alive():
        if time.time() - state["t0"] > 200:
            check("the browser run finished", False)
            note(run.tail())
            return finish()
        QtCore.QTimer.singleShot(100, poll)
        return
    QtCore.QTimer.singleShot(500, verify)


def verify():
    run = state["run"]
    r = run.report()
    note("  report %s" % json.dumps(r))
    rows = r.get("rows", [])
    check("alice's list: her newest step, and the group as one row of two steps",
          state.get("grouped") and len(rows) == 2 and "(2 steps)" in rows[1]
          and "(" not in rows[0], rows)
    check("the group's row opens to its steps", len(r.get("opened", [])) == 2, r.get("opened"))
    buttons = r.get("buttons", [])
    check("her newest step is refused for bob's: the card says it, offers to ask him "
          "and to keep his",
          r.get("card") and "cannot be undone" in r.get("card")
          and "Ask bob" in buttons and "Keep theirs" in buttons, (r.get("card"), buttons))
    check("asked, her page says it waits", r.get("asked") and "Asked" in r.get("asked"),
          r.get("asked"))
    check("bob's page asks him, naming alice",
          r.get("question") and "alice" in r.get("question"), r.get("question"))
    check("he allows, and alice's page says it went",
          run.code == 0 and "agreed" in (r.get("said") or ""), (run.code, r.get("said")))
    check("the host has the box as alice's group left it", length() == state["len_group"],
          length())
    if run.code != 0:
        note("---- driver log")
        note(run.tail())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    if "log" in state:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "TransactionLog", state["log"])
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, build)
