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
  - an Animation that also rebuilds a shape and hides an object is held
    all the same: the shape is a sequence of its draws at every frame,
    the hidden one a track with the frames it is shown at and its draws
    kept, every blob servable under the key the clip names;
  - with no room for those blobs the clip still plays the box, it is not
    exact, and the publishes flow;
  - an edit while playing drops the clip, and it is baked again;
  - stopping says the document is as designed again.

The drawing half is tests/gui/serve-animation-browser.py.

  scripts/gui-test.sh tests/gui/serve-animation.py /tmp/serve-anim
"""
import base64
import hashlib
import json as jsonlib
import math
import os
import urllib.request
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
        lamp = doc.addObject("Part::Box", "Lamp")
        lamp.Placement.Base = App.Vector(-30, 0, 0)
        paint = doc.addObject("Part::Box", "Paint")
        paint.Placement.Base = App.Vector(0, 30, 0)
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

        # A Cycles stream traces each publish and has no clip to play: while
        # one runs, nothing is held, or its picture would stay at the pose
        # play started from (SceneServeClip::Host::unclipped)
        tracer = WS(port)
        tracer.hello("tracer")
        tracer.next_binary(20.0)
        asked = [70]

        def ask(fields, timeout=30.0):
            # The host answers a cycles op on the GUI thread, which is this
            # one: pump it while the answer is waited for
            asked[0] += 1
            fields = dict(fields, op="cycles", id=asked[0])
            tracer.send(1, jsonlib.dumps(fields).encode())
            deadline = clock() + timeout
            while clock() < deadline:
                pump(0.02)
                text = tracer.next_text(0.02)
                if not text:
                    continue
                try:
                    answer = jsonlib.loads(text.decode("utf-8"))
                except Exception:
                    continue
                if answer.get("id") == asked[0]:
                    return answer
            return {}

        f = 1.0 / math.tan(math.radians(22.5))
        proj = [f / (4.0 / 3.0), 0, 0, 0, 0, f, 0, 0, 0, 0, -1.002, -1, 0, 0, -2.002, 0]
        view = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -200, 1]
        started = ask({"action": "start", "device": "CPU", "samples": 2, "denoise": False,
                       "view": view, "proj": proj, "width": 64, "height": 48})
        pump(1.0)
        before = ask({"action": "status"}).get("updates", 0)
        t1c = clock()
        pump(1.5)
        traced = client.since(t1c, "scene", clock())
        after = ask({"action": "status"}).get("updates", 0)
        check("a Cycles stream starts on the served document",
              started.get("ok") and started.get("running"), started)
        check("while it traces, the frames are published", len(traced) >= 5, len(traced))
        check("and the stream takes them", after > before, (before, after))
        ask({"action": "stop"})
        tracer.close()
        pump(0.5)
        t1d = clock()
        pump(1.0)
        check("with it stopped they are held again",
              len(client.since(t1d, "scene", clock())) == 0,
              len(client.since(t1d, "scene", clock())))

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

        # --- an Animation that rebuilds a shape and hides an object as well
        t4 = clock()
        anim.addKey("Still.Height", 0.0, 10.0)
        anim.addKey("Still.Height", 2.0, 20.0)
        anim.addKey("Lamp.Visibility", 0.0, True)
        anim.addKey("Lamp.Visibility", 0.5, False)
        anim.addKey("Lamp.Visibility", 1.5, True)
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
        if clip2 is None:
            return finish()
        frames = clip2["frames"]
        tracks = clip_poses(clip2)
        seq = struct.unpack("<%di" % (len(base64.b64decode(clip2["seq"])) // 4),
                            base64.b64decode(clip2["seq"]))
        shown = base64.b64decode(clip2["shown"])
        refs = clip2["refs"]
        blobs = clip2["blobs"]
        check("the rebuilt shape is a sequence, a blob at every frame",
              len(clip2["seqKeys"]) == 1 and len(seq) == frames
              and all(0 <= b < len(blobs) for b in seq) and len(set(seq)) > frames // 2,
              (clip2["seqKeys"], len(seq), len(set(seq))))
        lamp_rows = [i for i in range(len(clip2["keys"]))
                     if 0 in shown[i * frames:(i + 1) * frames]]
        row = lamp_rows[0] if lamp_rows else None
        mask = list(shown[row * frames:(row + 1) * frames]) if row is not None else []
        check("the hidden one is a track, hidden from 0.5 s to 1.5 s",
              len(lamp_rows) == 1 and mask[5] == 1 and mask[10] == 0 and mask[29] == 0
              and mask[30] == 1, mask)
        check("with its draws kept, as they were where it was first shown",
              row is not None and refs[2 * row] == 0 and 0 <= refs[2 * row + 1] < len(blobs),
              refs)
        check("the box is still played",
              len([k for k in tracks if k not in [clip2["keys"][r] for r in lamp_rows]])
              == len(poses), clip2["keys"])
        got = []
        for key, size in list(zip(blobs, clip2["sizes"]))[:3] + [(blobs[-1], clip2["sizes"][-1])]:
            try:
                body = urllib.request.urlopen(
                    "http://127.0.0.1:%d/blob?key=%s" % (port, key), timeout=5).read()
            except Exception as e:
                body = repr(e).encode()
            got.append(hashlib.sha1(body).hexdigest() == key and len(body) == size)
        check("every blob is served under its key", all(got), got)
        t6 = clock()
        pump(1.0)
        flowing = client.since(t6, "scene")
        check("and nothing is published while it plays", len(flowing) == 0, len(flowing))
        Gui.runCommand("Std_AnimationStop")
        pump(0.5)
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/blob?key=%s" % (port, blobs[0]),
                                   timeout=5).read()
            kept = True
        except Exception:
            kept = False
        check("the blobs stay while the clip does", kept)

        # --- no room for the blobs
        hGrp = App.ParamGet("User parameter:BaseApp/Preferences/Animation")
        hGrp.SetInt("StreamBlobMB", 0)
        anim.addKey("Still.Height", 1.0, 30.0)
        pump(0.3)
        t5 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(1.5)
        clip5 = client.last("clip", t5)
        # The hidden one keeps its track, without its draws: a viewer can
        # still hide it, not show it from a publish that lacks it
        check("with no room for blobs the box and the hidden one are still played",
              clip5 is not None and len(clip_poses(clip5)) == len(poses) + 1
              and -1 in clip5["refs"][1::2], clip5 and (clip5["keys"], clip5["refs"]))
        check("the rebuilt shape left out", clip5 is not None and not clip5["seqKeys"]
              and not clip5["blobs"], clip5 and clip5["seqKeys"])
        t6 = clock()
        pump(1.0)
        flowing = client.since(t6, "scene")
        check("and what the clip cannot say is published as it was", len(flowing) >= 3,
              len(flowing))
        named = [f[2] for f in client.since(t6, "time") if f[2].get("baseActive")]
        check("each of those named with its Time", len(named) >= 3, len(named))
        hGrp.RemInt("StreamBlobMB")
        Gui.runCommand("Std_AnimationStop")
        pump(0.5)

        # --- a transparency that changes: a record a frame, not a blob
        anim.addKey("Paint.ViewObject.Transparency", 0.0, 0)
        anim.addKey("Paint.ViewObject.Transparency", 2.0, 80)
        pump(0.3)
        t10 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(1.5)
        clip6 = client.last("clip", t10)
        looked = clip6["lookTracks"] if clip6 else []
        check("a fading object rides a track with its looks",
              clip6 is not None and len(looked) == 1 and len(clip6["seqKeys"]) == 1
              and clip6["lookCount"][0] >= 1,
              clip6 and (looked, clip6["lookCount"], clip6["seqKeys"]))
        if looked:
            frames6 = clip6["frames"]
            at = struct.unpack("<%di" % (len(base64.b64decode(clip6["lookAt"])) // 4),
                               base64.b64decode(clip6["lookAt"]))
            record = 40
            looks = base64.b64decode(clip6["looks"])
            row = looked[0]
            check("its look changes from frame to frame",
                  len(at) == frames6 and len(set(at)) > frames6 // 2
                  and len(looks) % record == 0, (len(at), len(set(at))))
            # The first draw's diffuse colour, RGBA: its alpha goes down
            alpha = [struct.unpack_from("<I", looks, at[f] * record + 4)[0] & 0xFF
                     for f in (0, frames6 - 1)]
            check("from opaque to 80 % transparent", alpha[0] == 255 and 40 <= alpha[1] <= 60,
                  alpha)
            check("and no blob of it is kept",
                  not clip6["refs"] or clip6["refs"][2 * row + 1] == -1, clip6["refs"])
        t11 = clock()
        pump(1.0)
        flowing = client.since(t11, "scene")
        check("nothing is published for it while it plays", len(flowing) == 0, len(flowing))
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
