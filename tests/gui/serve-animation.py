"""A served document's Animation: the clip and the clock its clients are sent.

docs/Animation.md sec 9, "Streaming". When the player starts an Animation
of a served document, the host bakes what it does to each object into a
clip, sends it and says what its clock reads; a client then moves the
draws of the publish it holds by itself. A synthetic client cannot draw,
but it reads the wire, and the wire is what is asserted:

  - playing sends a clip with a track for the box that moves and none for
    the one that stays, its last pose the box's own move;
  - and the clock: active, playing, the clip's id, the range and the loop;
  - every publish is named with the Time its pose was taken at;
  - while it plays, the host publishes nothing for what the clip says --
    the box really moving on the host all the while;
  - pausing publishes the pose it stopped in and says so;
  - an Animation that also rebuilds a shape is not held: its clip still
    plays the box, and the publishes flow;
  - an edit while playing drops the clip, and it is baked again;
  - stopping says the document is as designed again.

The drawing half is tests/gui/serve-animation-browser.py.

  scripts/gui-test.sh tests/gui/serve-animation.py /tmp/serve-anim
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

# The platform's backend, as a user gets it: Direct3D 11 on Windows, Metal
# on macOS, OpenGL elsewhere (RenderParams::engineType, docs/Testing.md).
ENGINE = "Default"

clock = time.perf_counter

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "ServeAnimation"

state = {"done": False, "client": None}


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
    """One connection that only listens: every frame, when it came."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.error = None
        self.ready = threading.Event()
        self.stop = False
        self.lock = threading.Lock()
        self.frames = []  # (when, "scene" | cmd, parsed JSON or None)

    def run(self):
        try:
            ws = WS(self.port)
            ws.hello("anim")
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
                if m[0] == 2:
                    entry = (clock(), "scene", None)
                elif m[0] == 1:
                    try:
                        value = jsonlib.loads(m[1].decode("utf-8"))
                    except Exception:
                        continue
                    entry = (clock(), value.get("cmd", ""), value)
                else:
                    continue
                with self.lock:
                    self.frames.append(entry)
            ws.close()
        except Exception:
            self.error = traceback.format_exc()
            self.ready.set()

    def since(self, when, kind=None, until=None):
        with self.lock:
            return [f for f in self.frames
                    if f[0] >= when and (until is None or f[0] <= until)
                    and (kind is None or f[1] == kind)]

    def last(self, kind, when=0.0):
        found = self.since(when, kind)
        return found[-1][2] if found else None


def clip_poses(clip):
    """A clip message's data as {key: [(tx, ty, tz, qx, qy, qz, qw), ...]}"""
    raw = base64.b64decode(clip.get("data", ""))
    frames = clip.get("frames", 0)
    values = struct.unpack("<%df" % (len(raw) // 4), raw)
    out = {}
    for i, key in enumerate(clip.get("keys", [])):
        out[key] = [values[(i * frames + k) * 7:(i * frames + k) * 7 + 7]
                    for k in range(frames)]
    return out


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        App.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", ENGINE)
        Gui.getMainWindow().resize(1300, 900)
        doc = App.newDocument(DOC)
        box = doc.addObject("Part::Box", "Box")
        still = doc.addObject("Part::Box", "Still")
        still.Placement.Base = App.Vector(30, 0, 0)
        doc.recompute()
        anim = doc.addObject("App::Animation", "Anim")
        anim.FrameRate = 20
        anim.Loop = "Loop"
        anim.addKey("Box.Placement", 0.0)
        anim.addKey("Box.Placement", 2.0,
                    App.Placement(App.Vector(0, 0, 6), App.Rotation(App.Vector(0, 0, 1), 90)))
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
        check("nothing is said of an Animation before one plays",
              not client.since(0.0, "clip") and not client.since(0.0, "time"))

        # --- playing a placement
        t0 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(1.5)
        clip = client.last("clip", t0)
        if not check("playing sends a clip", clip is not None and clip.get("id", 0) > 0, clip
                     and {k: clip[k] for k in ("id", "start", "step", "frames", "keys")}):
            return finish()
        check("of the Animation's range at its frame rate",
              clip["frames"] == 41 and abs(clip["start"]) < 1e-9
              and abs(clip["step"] - 0.05) < 1e-9, (clip["frames"], clip["start"], clip["step"]))
        poses = clip_poses(clip)
        check("with a track for what moves and none for what stays",
              1 <= len(poses) <= 4, len(poses))
        lasts = [p[-1] for p in poses.values()]
        firsts = [p[0] for p in poses.values()]
        check("each track starts where the first frame is",
              all(max(abs(v) for v in f[:6]) < 1e-5 and abs(abs(f[6]) - 1) < 1e-5
                  for f in firsts), firsts)
        # A quarter turn about the axis the box is lifted along, wherever
        # the feed's axes point: 6 along it, and half the angle's sine
        check("and ends in the box's own move",
              all(abs((l[0] ** 2 + l[1] ** 2 + l[2] ** 2) ** 0.5 - 6.0) < 1e-3
                  and abs((l[3] ** 2 + l[4] ** 2 + l[5] ** 2) ** 0.5 - 0.5 ** 0.5) < 1e-3
                  for l in lasts), lasts)

        tick = client.last("time", t0)
        check("and the clock: active, playing, the clip's",
              tick and tick["active"] and tick["playing"] and tick["clip"] == clip["id"], tick)
        check("with the range and the loop",
              tick and abs(tick["start"]) < 1e-9 and abs(tick["end"] - 2.0) < 1e-9
              and tick["loop"] == 1 and abs(tick["speed"] - 1.0) < 1e-9, tick)
        based = [f[2] for f in client.since(t0, "time") if f[2].get("baseActive")]
        check("a publish is named with the Time its pose is of",
              bool(based) and based[-1]["v"] > 0 and 0.0 <= based[-1]["base"] <= 2.0,
              based[-1] if based else None)

        # Held: the box moves on the host and nothing is published for it
        t1 = clock()
        z0 = box.Placement.Base.z
        seen = set()
        end = clock() + 1.5
        while clock() < end:
            pump(0.05)
            seen.add(round(box.Placement.Base.z, 3))
        t2 = clock()
        scenes = client.since(t1, "scene", t2)
        check("while it plays the box moves on the host", len(seen) > 8, (z0, len(seen)))
        check("and nothing is published for it", len(scenes) == 0, len(scenes))
        beats = client.since(t1, "time", t2)
        check("the clock is said again as it plays", 1 <= len(beats) <= 4, len(beats))

        # --- pausing
        t3 = clock()
        Gui.runCommand("Std_AnimationPause")
        pump(0.8)
        paused = anim.Time.Value
        tick = client.last("time", t3)
        check("pausing publishes the pose it stopped in",
              len(client.since(t3, "scene")) >= 1, len(client.since(t3, "scene")))
        check("and says so: not playing, the publish at that Time",
              tick and tick["active"] and not tick["playing"]
              and abs(tick["t"] - paused) < 1e-6 and tick["baseActive"]
              and abs(tick["base"] - paused) < 1e-6, (paused, tick))

        # --- an Animation that rebuilds a shape as well
        t4 = clock()
        anim.addKey("Still.Height", 0.0, 10.0)
        anim.addKey("Still.Height", 2.0, 20.0)
        pump(0.3)
        dropped = client.last("clip", t4)
        check("changing the tracks drops the clip", dropped is not None and dropped["id"] == 0
              and not dropped["keys"], dropped and dropped["id"])
        t5 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(1.5)
        clip2 = client.last("clip", t5)
        check("playing bakes it again",
              clip2 is not None and clip2["id"] > clip["id"], clip2 and clip2["id"])
        check("the box is still played", clip2 and len(clip_poses(clip2)) == len(poses),
              clip2 and len(clip2["keys"]))
        t6 = clock()
        pump(1.0)
        flowing = client.since(t6, "scene")
        check("but what the clip cannot say is published as it was", len(flowing) >= 3,
              len(flowing))
        named = [f[2] for f in client.since(t6, "time") if f[2].get("baseActive")]
        check("each of those named with its Time", len(named) >= 3, len(named))
        Gui.runCommand("Std_AnimationStop")
        pump(0.5)

        # --- an edit while playing
        t7 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(1.0)
        clip3 = client.last("clip")
        t8 = clock()
        still.Length = 12
        doc.recompute()
        pump(0.2)
        dropped = client.last("clip", t8)
        check("an edit while playing drops the clip",
              clip3 is not None and clip3["id"] > 0 and dropped is not None
              and dropped["id"] == 0,
              (clip3 and clip3["id"], dropped and dropped["id"]))
        pump(1.5)
        clip4 = client.last("clip", t8)
        check("and it is baked again once the edits stop",
              clip3 is not None and clip4 is not None and clip4["id"] > clip3["id"],
              clip4 and clip4["id"])

        # --- stopping
        t9 = clock()
        Gui.runCommand("Std_AnimationStop")
        pump(0.8)
        tick = client.last("time", t9)
        check("stopping says the document is as designed",
              tick and not tick["active"] and not tick["playing"] and not tick["baseActive"],
              tick)
        check("and publishes it", len(client.since(t9, "scene")) >= 1)
        check("the client ran without error", client.error is None, client.error or "")
    except Exception:
        note("FAIL the script:\n" + traceback.format_exc())
    finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    if state["client"]:
        state["client"].stop = True
    try:
        Gui.runCommand("Std_AnimationStop")
    except Exception:
        pass
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
