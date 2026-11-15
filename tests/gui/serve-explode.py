"""A served document's exploded view, played by an Animation: the clip its
clients are sent carries the parts' moves and the steps' trails.

docs/AssemblyExplode.md sec 4, on the wire of docs/Animation.md sec 9.1
(tests/gui/serve-animation.py reads the same for placement tracks). An
assembly of two boxes, B exploded up 20 mm then across 20 mm, played by an
Explode track over 2 s. What is asserted:

  - playing sends a clip of the Animation's range;
  - with a track for B, its last pose 20 mm across and 20 mm up from its
    first, and none for the box that stays;
  - the trails ride the clip too: a sequence, its draws at every frame, as
    they grow -- they are not a rigid move;
  - stopped, B is back where it rests and the view is not posed.

  scripts/gui-test.sh tests/gui/serve-explode.py /tmp/serve-explode
"""
import base64
import json as jsonlib
import os
import struct
import threading
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets

from wsclient import WS, free_port

ENGINE = "Default"
clock = time.perf_counter
OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))

state = {"client": None, "test": None}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = clock() + seconds
    while clock() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.005)


class Client(threading.Thread):
    """One connection that only listens: every text frame, when it came"""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.error = None
        self.ready = threading.Event()
        self.stop = False
        self.lock = threading.Lock()
        self.frames = []

    def run(self):
        try:
            ws = WS(self.port)
            ws.hello("explode")
            if ws.next_binary(20.0) is None:
                self.error = "no snapshot"
                self.ready.set()
                return
            self.ready.set()
            while not self.stop:
                m = ws.recv(0.1)
                if m is None:
                    continue
                if m[0] == 8:
                    break
                if m[0] != 1:
                    continue
                try:
                    value = jsonlib.loads(m[1].decode("utf-8"))
                except Exception:
                    continue
                with self.lock:
                    self.frames.append((clock(), value.get("cmd", ""), value))
            ws.close()
        except Exception:
            self.error = traceback.format_exc()
            self.ready.set()

    def last(self, kind, when=0.0):
        with self.lock:
            found = [f for f in self.frames if f[0] >= when and f[1] == kind]
        return found[-1][2] if found else None


def clip_poses(clip):
    """A clip message's data as {key: [(tx, ty, tz, qx, qy, qz, qw), ...]}"""
    raw = base64.b64decode(clip.get("data", ""))
    frames = clip.get("frames", 0)
    values = struct.unpack("<%df" % (len(raw) // 4), raw)
    out = {}
    for i, key in enumerate(clip.get("keys", [])):
        out[key] = [values[(i * frames + k) * 7:(i * frames + k) * 7 + 7] for k in range(frames)]
    return out


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        App.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString("Type", ENGINE)
        Gui.getMainWindow().resize(1300, 900)
        Gui.activateWorkbench("AssemblyWorkbench")
        import CommandCreateView
        from AssemblyTests.TestExplodedView import TestExplodeTrack
        from AssemblyTests.TestJoints import plc

        test = state["test"] = TestExplodeTrack("testPlay")
        test.setUp()
        doc, view, b = test.doc, test.view, test.b
        test.box("A")
        CommandCreateView.ViewProviderExplodedView(view.ViewObject)
        for step in view.Group:
            CommandCreateView.ViewProviderExplodedViewStep(step.ViewObject)
        anim = doc.addObject("App::Animation", "Anim")
        anim.FrameRate = 10
        anim.addKey(test.path, 0, 0, interp="Linear")
        anim.addKey(test.path, 2, 2, interp="Linear")
        doc.recompute()
        pump(0.5)

        port = free_port()
        if not check("the document is served", Gui.serveDocument(doc, port), "port %d" % port):
            return finish()
        client = state["client"] = Client(port)
        client.start()
        end = clock() + 30
        while not client.ready.is_set() and clock() < end:
            pump(0.05)
        if not check("the client has the scene", client.ready.is_set() and not client.error,
                     client.error or ""):
            return finish()
        pump(1.0)

        t0 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(2.0)
        clip = client.last("clip", t0)
        if not check("playing sends a clip", clip is not None and clip.get("id", 0) > 0,
                     clip and {k: clip.get(k) for k in ("id", "frames", "keys", "seqKeys")}):
            return finish()
        check("of the Animation's range at its frame rate", clip["frames"] == 21, clip["frames"])
        poses = clip_poses(clip)
        moved = {}
        for key, p in poses.items():
            d = [p[-1][i] - p[0][i] for i in range(3)]
            if max(abs(v) for v in d) > 1e-3:
                moved[key] = [round(v, 3) for v in d]
        check("a track for B, 20 mm across and 20 mm up at the end, none for A",
              len(moved) == 1 and list(moved.values())[0] == [20.0, 0.0, 20.0], moved)
        check("the trails ride the clip: a sequence, drawn at every frame",
              len(clip.get("seqKeys", [])) >= 1, clip.get("seqKeys"))

        Gui.runCommand("Std_AnimationStop")
        pump(0.5)
        if anim.Active:
            anim.stop()
        check("stopped, B is back where it rests and the view is not posed",
              b.Placement.isSame(plc(30, 0, 0), 1e-9) and view.posedAt() is None,
              (b.Placement, view.posedAt()))
    except Exception:
        note("ABORT:\n" + traceback.format_exc())
    finish()


def finish():
    client = state.get("client")
    if client:
        client.stop = True
    try:
        Gui.runCommand("Std_AnimationStop")
    except Exception:
        pass
    test = state.get("test")
    if test is not None:
        # the joint prefs it changed put back
        test.prefs.SetBool("SolveInJointCreation", test._solveInCreation)
    for d in list(App.listDocuments().values()):
        App.closeDocument(d.Name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
