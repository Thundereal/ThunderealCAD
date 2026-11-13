"""Two real browsers, each working in its own task panel
(docs/TaskPanelPerView.md sec 16): the browser half of
tests/gui/serve_pd_multi_user.py, which drives the same panels through
sockets. What no socket client can check is the page (web/src/widgets/
panel.tsx): that each page's card draws its own user's panel and not the
other's, that typing into a field reaches the host, and that its OK and
Cancel end that user's edit and leave the other's.

Two PartDesign bodies side by side, PadA (length 15) and PadB (length 5),
served with PerViewEdit on to alice and bob, invited by name. Chrome
drives both pages through scripts/panel-two-users-drive.js: each user
right-clicks their pad in the view and takes "Edit" from the host's menu.
Asserted:

  - before any edit, neither page has a panel;
  - alice's page shows PadA's panel; bob's shows none;
  - bob's page shows PadB's panel; alice's still shows hers, not his;
  - alice types 18 and presses OK: PadA is 18, her panel goes, bob's stays;
  - bob types 9 and presses Cancel: PadB is 5 again, his panel goes;
  - nobody is left in edit;
  - every tool bar button shows its icon, as the desktop's do: Undo and
    Redo, and the primitive groups whose members are plain QActions;
  - the tool bars' card covers none of the task panel card, the card open
    with no panel and with each user's, and stands beside her panel card
    dragged over where it starts.

The pages are saved as alice.png and bob.png in the test's directory,
taken while both are in edit. Needs the built viewer (build/wasm/
fcviewer.html), node, PUPPETEER_PATH and CHROME; skips rather than fails
when any is missing.
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
import Part
from PySide import QtCore

import wsclient
from wsclient import free_port

# Metal on macOS: a Qt widget's GL context there is 2.1, below what the
# bgfx OpenGL backend takes (docs/Testing.md).
ENGINE = "bgfx - Metal" if sys.platform == "darwin" else "bgfx - OpenGL"

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WASM = os.path.join(REPO, "build", "wasm")
DRIVER = os.path.join(REPO, "scripts", "panel-two-users-drive.js")
DOC = "PanelTwoUsers"
TOKENS = {"alice": "panel-browser-alice", "bob": "panel-browser-bob"}
V = FreeCAD.Vector
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
DOCP = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document")

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


def pad(body, name, x0, length):
    """A 10 x 10 square at x0 on the body's XY plane, padded `length`"""
    sk = body.newObject("Sketcher::SketchObject", "Sketch" + name)
    sk.AttachmentSupport = [(body.Origin.OriginFeatures[3], "")]
    sk.MapMode = "FlatFace"
    p = [V(x0, 0, 0), V(x0 + 10, 0, 0), V(x0 + 10, 10, 0), V(x0, 10, 0)]
    for i in range(4):
        sk.addGeometry(Part.LineSegment(p[i], p[(i + 1) % 4]))
    feature = body.newObject("PartDesign::Pad", name)
    feature.Profile = sk
    feature.Length = length
    return feature


def length(name):
    return round(FreeCAD.getDocument(DOC).getObject(name).Length.Value, 3)


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
            proc = subprocess.Popen(["node", DRIVER] + self.urls + [OUT, "90000"],
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
        state["log"] = DOCP.GetInt("TransactionLog", 2)
        DOCP.SetInt("TransactionLog", 2)
        DOCP.SetInt("AutoSaveTimeout", 0)
        VIEW.SetBool("PerViewEdit", True)
        VIEW.SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE)
        doc = FreeCAD.newDocument(DOC)
        pad(doc.addObject("PartDesign::Body", "BodyA"), "PadA", 0, 15)
        pad(doc.addObject("PartDesign::Body", "BodyB"), "PadB", 30, 5)
        doc.recompute()
        doc.UndoMode = 1
        doc.saveAs(os.path.join(tempfile.mkdtemp(prefix="fc-panel-browser-"), "p.FCStd"))
        doc.clearUndos()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        port = free_port()
        if not check("the document is served", FreeCADGui.serveDocument(doc, port)):
            return finish()
        FreeCADGui.serveSetGrants(wsclient.invitation(TOKENS["alice"], "alice")
                                  + wsclient.invitation(TOKENS["bob"], "bob"))
        state["port"] = port
        state["http"] = serve_viewer()
        urls = ["http://127.0.0.1:%d/fcviewer.html?scene=http://127.0.0.1:%d&doc=%s"
                "&token=%s&client=%s&panel" % (state["http"], port, DOC, TOKENS[n], n)
                for n in ("alice", "bob")]
        run = Run(urls)
        state["run"] = run
        state["t0"] = time.time()
        run.start()
        QtCore.QTimer.singleShot(100, poll)
    except Exception:
        note("FAIL build:\n" + traceback.format_exc())
        finish()


def poll():
    run = state["run"]
    if run.is_alive():
        if time.time() - state["t0"] > 400:
            check("the browser run finished", False)
            note(run.tail())
            return finish()
        QtCore.QTimer.singleShot(100, poll)
        return
    QtCore.QTimer.singleShot(1000, verify)


def fields(p):
    return [round(float(v.split()[0]), 3) for v in (p or {}).get("fields", [])
            if v.split() and v.split()[0].replace(".", "", 1).lstrip("-").isdigit()]


def verify():
    run = state["run"]
    r = run.report()
    note("  report %s" % json.dumps(r))
    no_panel = lambda p: p is not None and not (p.get("fields") or [])
    check("before any edit, neither page has a panel",
          no_panel(r.get("aliceBefore")) and no_panel(r.get("bobBefore")),
          (r.get("aliceBefore"), r.get("bobBefore")))
    check("alice took Edit PadA from the host's menu, her page shows its panel",
          r.get("aliceMenu") and 15.0 in fields(r.get("alice")),
          (r.get("aliceMenu"), r.get("alice")))
    check("bob's page shows no panel meanwhile", no_panel(r.get("bobWhileAlice")),
          r.get("bobWhileAlice"))
    check("bob took Edit PadB, his page shows its panel",
          r.get("bobMenu") and 5.0 in fields(r.get("bob")) and 15.0 not in fields(r.get("bob")),
          (r.get("bobMenu"), r.get("bob")))
    check("alice's page still shows hers, not his",
          15.0 in fields(r.get("aliceWhileBob")) and 5.0 not in fields(r.get("aliceWhileBob")),
          r.get("aliceWhileBob"))
    check("alice typed 18; bob's field did not move",
          r.get("aliceTyped") and 18.0 in fields(r.get("aliceAfterTyping"))
          and 5.0 in fields(r.get("bobAfterAliceTyped")),
          (r.get("aliceAfterTyping"), r.get("bobAfterAliceTyped")))
    check("her OK closes her panel and leaves bob's",
          r.get("aliceOk") and no_panel(r.get("aliceAfterOk"))
          and 5.0 in fields(r.get("bobAfterAliceOk")),
          (r.get("aliceAfterOk"), r.get("bobAfterAliceOk")))
    check("bob typed 9 and pressed Cancel: his panel closes",
          r.get("bobTyped") and 9.0 in fields(r.get("bobAfterTyping"))
          and r.get("bobCancel") and no_panel(r.get("bobAfterCancel")),
          (r.get("bobAfterTyping"), r.get("bobAfterCancel")))
    check("the host: PadA 18 from her OK, PadB 5 again after his Cancel",
          length("PadA") == 18.0 and length("PadB") == 5.0, (length("PadA"), length("PadB")))
    check("nobody is left in edit", FreeCADGui.getDocument(DOC).getInEdit() is None)
    faces = r.get("faces") or {}
    check("every tool bar button shows its icon, as on the desktop",
          faces.get("buttons") and faces.get("icons") == faces["buttons"], faces)
    covers = [r.get(k) or {} for k in ("overlapOpen", "overlapAlice", "overlapBob")]
    check("the tool bars keep clear of the task panel: opened, and in each edit",
          all(c.get("bars") and c.get("panel") and c.get("area") == 0 for c in covers),
          covers)
    dragged = r.get("overlapDragged") or {}
    check("her panel dragged over where the bars start: they stand beside it",
          dragged.get("bars") and dragged.get("panel") and dragged.get("area") == 0
          and dragged["bars"][0] >= dragged["panel"][2], dragged)
    check("the driver ran every step", run.code == 0, run.code)
    if run.code != 0:
        note("---- driver log")
        note(run.tail())
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
    if "log" in state:
        DOCP.SetInt("TransactionLog", state["log"])
    VIEW.RemBool("PerViewEdit")
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, build)
