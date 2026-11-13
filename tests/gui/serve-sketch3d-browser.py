"""A real browser edits a 3D sketch (docs/Sketch3D.md S4, the browser half).

tests/gui/serve-sketch3d-edit.py checks the server side against a
synthetic socket client. What only a browser can check is the viewer
(src/Gui/Renderer/wasm, web/): that it draws the 3D edit, and that its
pointer, its keys and the value editor it draws drive the 3D tools. So
this serves a document with a 3D sketch, serves the built viewer over
HTTP, and lets Chrome do it (scripts/sketch3d-drive.js). The document is
read HERE; the page reports only what it drew and showed.

What is asserted:

  - the page's own ask enters the 3D sketch's edit, with no 3D view made
    in this process for it;
  - the edit is drawn in the browser while it runs and not after: its
    curves, counted as magenta pixels (the edited-edge colour this sets);
  - the 3D sketch's constraint icon is drawn in its two colours. In
    render-cache mode 3 a two-colour icon is a BLEND texture -- the texel
    each pixel's place from the fill to the border, the fill the
    material, the border the texture's blend colour -- so its border
    colour (pure blue here) reaches the canvas only where the viewer
    applies BLEND: MODULATE or REPLACE would leave none of it. Counted in
    a window round the icon, which the server places on the client's
    canvas (the edit's view is the mirror, with the browser's camera): the
    manipulator's rings are green and blue too, and a dimension's leaders
    are lines drawn in the label's colours that would show them either way;
  - two clicks after Sketcher_CreateLine, sent by the page as a command
    op, draw a line;
  - a digit typed on the view while placing opens the value editor in the
    page, and what is typed into it places the next point that far: a
    line 25 long with its Distance;
  - End edit's shortcut ends the session and the page is told.

Needs the built viewer (build/wasm/fcviewer.html; `ninja -C build/wasm`),
node, a puppeteer-core install named by PUPPETEER_PATH, and a Chrome named
by CHROME. Skips rather than fails when any is missing.

  PUPPETEER_PATH=/path/to/node_modules/puppeteer-core \\
  CHROME=~/.cache/puppeteer/chrome/*/chrome-linux64/chrome \\
  scripts/gui-test.sh tests/gui/serve-sketch3d-browser.py /tmp/s3d-web \\
      --timeout 600
"""
import functools
import http.server
import json as jsonlib
import os
import sys
import subprocess
import threading
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui

import wsclient
from wsclient import free_port

# Metal on macOS: a Qt widget's GL context there is 2.1, below what the
# bgfx OpenGL backend takes (docs/Testing.md).
ENGINE = "bgfx - Metal" if sys.platform == "darwin" else "bgfx - OpenGL"

TOKEN = "sketch3d-browser-invite"
CLIENT = "sketch3d-browser"

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
REPO = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))
WASM = os.path.join(REPO, "build", "wasm")
DRIVER = os.path.join(REPO, "scripts", "sketch3d-drive.js")
DOC = "ServeSketch3DBrowser"
OBJ = "Sketch3D"
SETTLE_MS = 8000
RUN_WAIT_S = 300

state = {"doc": None, "done": False, "run": None, "seen_phases": 0,
         "samples": {}, "t0": clock()}


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


class Run(threading.Thread):
    """The browser, driven from off the GUI thread. Log, phase markers
    and result are files: node buffers stdout to a pipe."""

    def __init__(self, url):
        super().__init__(daemon=True)
        self.url = url
        self.error = None
        self.log_path = os.path.join(OUT, "drive.log")
        self.phase_path = os.path.join(OUT, "phases.txt")
        self.result_path = os.path.join(OUT, "drive-result.json")

    def run(self):
        try:
            env = dict(os.environ)
            # The bundled Chrome needs libasound from the conda env
            # (scripts/wasm-chrome.js).
            env["LD_LIBRARY_PATH"] = (
                os.path.join(REPO, ".conda", "freecad", "lib")
                + os.pathsep + env.get("LD_LIBRARY_PATH", ""))
            env["S3D_PHASES"] = self.phase_path
            env["S3D_RESULT"] = self.result_path
            env["S3D_SHOTS"] = OUT
            with open(self.log_path, "w") as log:
                proc = subprocess.Popen(
                    ["node", DRIVER, self.url, OBJ, str(SETTLE_MS)],
                    stdout=log, stderr=subprocess.STDOUT, env=env)
                proc.wait()
            if proc.returncode != 0:
                self.error = "node exited %d:\n%s" % (proc.returncode, self.tail())
        except Exception:
            self.error = traceback.format_exc()

    def tail(self, lines=25):
        try:
            with open(self.log_path) as f:
                return "".join(f.readlines()[-lines:])
        except OSError:
            return "(no driver log)"

    def phases(self):
        try:
            with open(self.phase_path) as f:
                return [line.strip() for line in f if line.strip()]
        except OSError:
            return []

    def result(self):
        try:
            with open(self.result_path) as f:
                return jsonlib.load(f)
        except Exception:
            return {}


def serve_viewer():
    """The built viewer over HTTP, in this process."""
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=WASM)
    handler.log_message = lambda *a, **k: None
    port = free_port()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    state["httpd"] = server
    return port


def sample():
    gdoc = FreeCADGui.getDocument(DOC)
    sk = state["doc"].getObject(OBJ)
    editing = gdoc.getInEdit() is not None
    return {
        "edit": editing,
        "icons": sk.ViewObject.getIcons() if editing else [],
        "views": len(gdoc.mdiViewsOfType("Gui::View3DInventor")),
        "lines": [(g.StartPoint, g.EndPoint) for g in sk.Geometry
                  if g.TypeId == "Part::GeomLineSegment"],
        "cons": [(c.Type, c.First, c.Value) for c in sk.Constraints],
    }


def build():
    try:
        import Part

        if not os.path.exists(os.path.join(WASM, "fcviewer.html")):
            skip("no built viewer at %s (ninja -C build/wasm)" % WASM)
            return
        if not os.environ.get("PUPPETEER_PATH"):
            skip("PUPPETEER_PATH is unset (a puppeteer-core install)")
            return
        if not os.environ.get("CHROME"):
            skip("CHROME is unset (a Chrome executable)")
            return

        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        view = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
        view.SetInt("RenderCache", 3)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE)
        # The colours the page counts: the edited edges magenta; the icons a
        # pure green fill in a pure blue border, on either background
        view.SetUnsigned("EditedEdgeColor", 0xFF00FFFF)
        for key in ("Sketch3DIcoColor", "Sketch3DIcoColorLight",
                    "Sketch3DDimColor", "Sketch3DDimColorLight"):
            view.SetUnsigned(key, 0x00FF00FF)
        for key in ("Sketch3DOutlineColor", "Sketch3DOutlineColorLight"):
            view.SetUnsigned(key, 0x0000FFFF)

        doc = FreeCAD.newDocument(DOC, hidden=True)
        state["doc"] = doc
        sk = doc.addObject("Sketcher::Sketch3DObject", OBJ)
        # One line with an icon and no dimension: Along X
        sk.addGeometry(Part.LineSegment(FreeCAD.Vector(-15, -8, 0),
                                        FreeCAD.Vector(15, -8, 0)))
        import Sketcher
        sk.addConstraint(Sketcher.Constraint("AlongX", 0))
        doc.recompute()
        state["before"] = sample()

        port = free_port()
        if not check("the document is served headless",
                     FreeCADGui.serveDocument(doc, port), "port %d" % port):
            finish()
            return
        http_port = serve_viewer()
        note("viewer on %d, scene on %d" % (http_port, port))
        FreeCADGui.serveSetGrants(wsclient.invitation(TOKEN, CLIENT))
        url = ("http://127.0.0.1:%d/fcviewer.html?scene=http://127.0.0.1:%d"
               "&token=%s&client=%s" % (http_port, port, TOKEN, CLIENT))
        state["run"] = Run(url)
        state["t0"] = clock()
        state["run"].start()
        QtCore.QTimer.singleShot(50, poll)
    except Exception:
        note("FAIL build:\n" + traceback.format_exc())
        finish()


def poll():
    run = state["run"]
    marked = run.phases()
    while len(marked) > state["seen_phases"]:
        name = marked[state["seen_phases"]]
        state["seen_phases"] += 1
        state["samples"][name] = sample()
    state.setdefault("views", []).append(
        len(FreeCADGui.getDocument(DOC).mdiViewsOfType("Gui::View3DInventor")))
    if run.is_alive():
        if clock() - state["t0"] > RUN_WAIT_S:
            check("the browser run finished", False, "still running after %ds" % RUN_WAIT_S)
            note("---- driver log")
            note(run.tail(25))
            finish()
            return
        QtCore.QTimer.singleShot(50, poll)
        return
    QtCore.QTimer.singleShot(500, verify)


def icon_colours(path, x, y, reach=24):
    """Pure green and pure blue pixels round view position (x, y) of the
    page's shot. View coordinates count y from the bottom."""
    img = QtGui.QImage(path)
    if img.isNull():
        return (0, 0)
    y = img.height() - 1 - y
    green = blue = 0
    for i in range(max(0, x - reach), min(img.width(), x + reach + 1)):
        for j in range(max(0, y - reach), min(img.height(), y + reach + 1)):
            c = img.pixelColor(i, j)
            r, g, b = c.red(), c.green(), c.blue()
            if g > 150 and r < 0.4 * g and b < 0.4 * g:
                green += 1
            elif b > 150 and r < 0.4 * b and g < 0.4 * b:
                blue += 1
    return (green, blue)


def verify():
    run = state["run"]
    out = run.result()
    s = state["samples"]
    try:
        if not check("the driver ran without error", run.error is None and "error" not in out,
                     run.error or out.get("error", "")):
            note("---- driver log")
            note(run.tail(40))
        check("the page asked for the edit", out.get("editCall") is True, out.get("editCall"))
        check("and believes it is in it", out.get("entered") is True, out.get("entered"))
        check("the 3D sketch entered edit", s.get("entered", {}).get("edit") is True,
              s.get("entered"))
        check("with no 3D view made for it", not any(state.get("views", [])),
              sorted(set(state.get("views", []))))

        view, edit, gone = out.get("view", {}), out.get("edit", {}), out.get("escaped", {})
        check("the edit's curves are drawn in the page",
              edit.get("magenta", 0) > 100 and view.get("magenta", 0) < 20, (view, edit))
        icons = s.get("entered", {}).get("icons", [])
        if check("the server placed its icon on the client's canvas", len(icons) == 1, icons):
            green, blue = icon_colours(os.path.join(OUT, "edit.png"), icons[0][1], icons[0][2])
            check("the icon's fill is drawn in the page", green > 10, (green, blue))
            check("and its border, which only a BLEND texture gives", blue > 8, (green, blue))

        before = len(state["before"]["lines"])
        r = out.get("lineReply") or {}
        check("Sketcher_CreateLine from the page is accepted", r.get("ok") is True, r)
        drawn = s.get("drawn", {}).get("lines", [])
        check("two clicks drew a line", len(drawn) == before + 1, len(drawn))

        check("a digit on the view opens the value editor in the page",
              out.get("editorShown") is True, out.get("editorOpened"))
        check("with the digit in it",
              (out.get("editorOpened") or {}).get("value", "").startswith("2"),
              out.get("editorOpened"))
        check("Enter closes it", out.get("editorClosed") is True)
        typed = s.get("typed", {})
        lines = typed.get("lines", [])
        if check("the typed length drew a line", len(lines) == before + 2, len(lines)):
            a, b = lines[-1]
            check("25 long", abs((b - a).Length - 25.0) < 1e-6, (b - a).Length)
            dist = [c for c in typed.get("cons", []) if c[0] == "Distance" and c[1] == len(lines) - 1]
            check("with its Distance", len(dist) == 1 and abs(dist[0][2] - 25.0) < 1e-9,
                  typed.get("cons"))

        check("End edit's shortcut ends the session", s.get("escaped", {}).get("edit") is False,
              s.get("escaped"))
        check("and the page is told", out.get("left") is True, out.get("left"))
        check("the edit is gone from the page", gone.get("magenta", 99) < 20, gone)
    except Exception:
        note("ABORT verify:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.getDocument(DOC).resetEdit()
    except Exception:
        pass
    try:
        state["httpd"].shutdown()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


# Deferred: a script handed to FreeCAD runs before the event loop is up.
QtCore.QTimer.singleShot(1500, build)
