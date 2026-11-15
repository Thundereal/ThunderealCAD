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
  - a render roughness that changes, which no look carries, makes the
    object a sequence, its draws at every frame -- not a still object
    whose publishes are held with the roughness Play started with;
  - a long range, a part rebuilt at every frame slowly, is baked a frame
    of its rate each (601 for 20 s at 30 fps), between events: they go on
    meanwhile, the clock says the player waits at its Time, a pause ends
    the bake with the Time put back, and the player plays on from where
    it was when the clip is ready;
  - an edit while playing drops the clip, and it is baked again;
  - an edit session while playing (the box's Transform) sets the clip
    aside: the clients are told an empty one, the clock names none, the
    host's pose is published as it moves; the edit over, the same clip is
    told again and nothing is published;
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

state = {"done": False, "client": None, "port": 0}


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
        msg = found[-1][2] if found else None
        return expand(msg) if kind == "clip" else msg


BODIES = {}


def expand(clip):
    """A clip message with its body apart: the body fetched by its key, as
    a viewer fetches it, and its five arrays laid into the message the way
    they were once sent inline (base64), for the checks that read them"""
    if not clip or not clip.get("body") or "data" in clip:
        return clip
    key = clip["body"]
    if key not in BODIES:
        BODIES[key] = urllib.request.urlopen(
            "http://127.0.0.1:%d/blob?key=%s" % (state["port"], key), timeout=10).read()
    raw = BODIES[key]
    assert raw[:4] == b"FMC1", raw[:4]
    at = 4
    for name in ("data", "shown", "seq", "lookAt", "looks"):
        n = struct.unpack_from("<I", raw, at)[0]
        clip[name] = base64.b64encode(raw[at + 4:at + 4 + n]).decode()
        at += 4 + n
    clip["bodyRead"] = len(raw) == clip.get("bodySize") and at == len(raw)
    return clip


class Slow:
    """A box that takes SLOW_S to rebuild, the cost of a heavily parametric
    part: what makes a bake take long enough to see the events go on"""

    SLOW_S = 0.005

    def __init__(self, obj):
        obj.addProperty("App::PropertyLength", "Height").Height = 5
        obj.Proxy = self

    def execute(self, obj):
        import Part
        end = clock() + self.SLOW_S
        while clock() < end:
            pass
        obj.Shape = Part.makeBox(5, 5, max(0.1, obj.Height.Value))

    def dumps(self):
        return None

    def loads(self, state):
        return None


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

        port = state["port"] = free_port()
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
        raw = [f[2] for f in client.since(t0, "clip") if f[2].get("id", 0) > 0]
        check("its poses ride a body apart, fetched by its key, not the message",
              bool(raw) and raw[-1].get("bodyRead") and len(raw[-1]["body"]) == 40
              and hashlib.sha1(BODIES[raw[-1]["body"]]).hexdigest() == raw[-1]["body"],
              raw and {k: raw[-1].get(k) for k in ("body", "bodySize", "bodyRead")})
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

        # A click a viewer made on the box where it drew it, a second away
        # on the clock from the host: the pick names the clip, that Time and
        # the box, and the host moves the ray to meet the box where it has it
        import Part

        picker = WS(port)
        picker.hello("picker")
        picker.next_binary(20.0)
        down = App.Vector(0, 0, -1)
        # A Time at which a ray straight down through the box's centre
        # misses the box where the host has it now
        misses_here = False
        for step in range(5, 16):
            drawn_at = (paused + 0.1 * step) % 2.0
            centre = anim.value("Box.Placement", drawn_at).multVec(App.Vector(5, 5, 5))
            origin = centre + App.Vector(0, 0, 50)
            ray = Part.LineSegment(origin, origin + down * 100).toShape()
            if box.Shape.distToShape(ray)[0] > 1e-3:
                misses_here = True
                break

        def pick(timed):
            msg = struct.pack("<cB6f", b"P", 0, origin.x, origin.y, origin.z,
                              down.x, down.y, down.z)
            if timed:
                msg += struct.pack("<IdQ", clip["id"], drawn_at, int(clip["keys"][0], 16))
            picker.send(2, msg)
            # What it selected comes back to this client alone
            end = clock() + 3.0
            while clock() < end:
                pump(0.05)
                text = picker.next_text(0.05)
                if not text:
                    continue
                try:
                    said = jsonlib.loads(text.decode("utf-8"))
                except Exception:
                    continue
                if said.get("cmd") == "selection":
                    return [item.get("obj") for item in said.get("items", [])]
            return None

        # The timed one first: the untimed one then has a box to clear,
        # and says so (a miss that changes nothing tells nothing back)
        timed, untimed = pick(True), pick(False)
        picker.close()
        check("the ray where the viewer drew the box misses it where the host has it",
              misses_here and untimed is not None and "Box" not in untimed,
              (drawn_at, paused, untimed))
        check("with the clip's Time and the box, the host moves it: the box is picked",
              timed == ["Box"], timed)

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
        chunks = clip2.get("chunks") or []
        check("the blobs' meshes are chunks of their own, named with the clip",
              len(chunks) >= 1 and len(clip2.get("chunkSizes") or []) == len(chunks),
              (len(chunks), clip2.get("chunkSizes")))
        got = []
        for key, size in list(zip(chunks, clip2.get("chunkSizes") or []))[:3]:
            try:
                body = urllib.request.urlopen(
                    "http://127.0.0.1:%d/blob?key=%s" % (port, key), timeout=5).read()
            except Exception as e:
                body = repr(e).encode()
            got.append(hashlib.sha1(body).hexdigest() == key and len(body) == size)
        check("and served under their keys", got and all(got), got)
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

        # --- rebuilt and moved: a blob a frame, its meshes shared by them
        step = doc.addObject("Part::Box", "Step")
        # A footprint of its own: at the Still's 10 by 10 its two heights
        # are meshes the Still's frames already hold, shared across objects
        step.Length = step.Width = 7
        step.Placement.Base = App.Vector(0, -30, 0)
        doc.recompute()
        for t, h in ((0.0, 10.0), (0.5, 20.0), (1.0, 10.0), (1.5, 20.0)):
            anim.addKey("Step.Height", t, h, interp="Constant")
        anim.addKey("Step.Placement.Base.x", 0.0, 0.0)
        anim.addKey("Step.Placement.Base.x", 2.0, 20.0)
        doc.recompute()
        pump(0.3)
        t12 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(2.0)
        clip7 = client.last("clip", t12)
        if clip7 is not None and len(clip7["seqKeys"]) >= 1:
            frames7 = clip7["frames"]
            seq7 = struct.unpack("<%di" % (len(base64.b64decode(clip7["seq"])) // 4),
                                 base64.b64decode(clip7["seq"]))
            mine = None
            for i in range(len(clip7["seqKeys"])):
                row7 = seq7[i * frames7:(i + 1) * frames7]
                if len(set(row7)) > frames7 // 2:
                    mine = row7
            check("a shape rebuilt as it moves is a blob a frame",
                  mine is not None and all(b >= 0 for b in mine), mine and len(set(mine)))
            shared = clip7.get("chunks") or []
            # Against the clip before, of the same tracks but these
            before = set((clip6 or {}).get("chunks") or [])
            fresh = [c for c in shared if c not in before]
            # Two heights, each a mesh of faces, one of edges and one of
            # vertices -- however many frames name them
            note("blobs %d (%d bytes), meshes %d (%d bytes)"
                 % (len(clip7["blobs"]), sum(clip7["sizes"]), len(shared),
                    sum(clip7.get("chunkSizes") or [])))
            check("its frames' meshes are two heights' worth, not a frame's each",
                  mine is not None and 1 <= len(fresh) <= 6 < len(set(mine)),
                  (len(fresh), mine and len(set(mine))))
        else:
            check("a shape rebuilt as it moves is a blob a frame", False,
                  clip7 and clip7["seqKeys"])
        Gui.runCommand("Std_AnimationStop")
        pump(0.5)
        anim.removeTrack("Step.Height")
        anim.removeTrack("Step.Placement.Base.x")
        doc.removeObject("Step")
        doc.recompute()

        # --- a PBR parameter that changes: no look carries it, so the
        # object is its draws at every frame
        shine = doc.addObject("Part::Box", "Shine")
        shine.Placement.Base = App.Vector(-30, -30, 0)
        doc.recompute()
        shine.ViewObject.addProperty("App::PropertyFloat", "Render_Roughness", "Render")
        shine.ViewObject.Render_Roughness = 0.1
        pump(0.3)
        # The sequences before it is keyed
        t13 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(1.5)
        before9 = client.last("clip", t13)
        Gui.runCommand("Std_AnimationStop")
        pump(0.5)
        anim.addKey("Shine.ViewObject.Render_Roughness", 0.0, 0.1)
        anim.addKey("Shine.ViewObject.Render_Roughness", 2.0, 0.9)
        pump(0.3)
        t13 = clock()
        Gui.runCommand("Std_AnimationPlay")
        pump(1.5)
        clip9 = client.last("clip", t13)
        if clip9 is not None:
            frames9 = clip9["frames"]
            seq9 = struct.unpack("<%di" % (len(base64.b64decode(clip9["seq"])) // 4),
                                 base64.b64decode(clip9["seq"])) if clip9["seq"] else ()
            old = set((before9 or {}).get("seqKeys") or [])
            rows = [seq9[i * frames9:(i + 1) * frames9]
                    for i, key in enumerate(clip9["seqKeys"]) if key not in old]
            check("a roughness that changes is the object's draws at every frame",
                  before9 is not None and len(rows) == 1 and len(set(rows[0])) == frames9
                  and all(b >= 0 for b in rows[0]),
                  (sorted(old), clip9["seqKeys"], [len(set(r)) for r in rows]))
        else:
            check("a roughness that changes is the object's draws at every frame", False)
        t13b = clock()
        pump(1.0)
        check("and nothing is published for it either",
              len(client.since(t13b, "scene")) == 0, len(client.since(t13b, "scene")))
        Gui.runCommand("Std_AnimationStop")
        pump(0.5)
        anim.removeTrack("Shine.ViewObject.Render_Roughness")
        doc.removeObject("Shine")
        doc.recompute()
        pump(0.3)

        # --- a long range: a frame of its rate each, baked between events,
        # a part rebuilt at every one of them that takes its time
        slow = doc.addObject("Part::FeaturePython", "Slow")
        Slow(slow)
        slow.ViewObject.Proxy = 0
        slow.Placement.Base = App.Vector(0, -30, 0)
        doc.recompute()
        anim.addKey("Slow.Height", 0.0, 5.0)
        anim.addKey("Slow.Height", 20.0, 25.0)
        anim.FrameRate = 30
        anim.End = 20.0
        pump(0.3)
        beats = []
        metronome = QtCore.QTimer()
        metronome.setInterval(10)
        metronome.timeout.connect(lambda: beats.append(clock()))
        metronome.start()

        def baked(since, timeout):
            end = clock() + timeout
            while clock() < end:
                got = [f for f in client.since(since, "clip") if f[2].get("id", 0) > 0]
                if got:
                    return got[-1]
                pump(0.02)
            return None

        t14 = clock()
        Gui.runCommand("Std_AnimationPlay")
        kept14 = anim.Time.Value
        pump(0.1)
        waiting = [f[2] for f in client.since(t14, "time")]
        check("while it bakes the clock says playing, at the Time it was played from",
              bool(waiting) and waiting[-1]["playing"] and waiting[-1]["clip"] == 0
              and abs(waiting[-1]["t"] - kept14) < 1e-6,
              waiting[-1] if waiting else None)
        # Pausing it then: no clip, the Time put back
        Gui.runCommand("Std_AnimationPlay")
        pump(0.5)
        paused = client.last("time", t14)
        check("a pause while it bakes ends it, the Time where it was",
              not [f for f in client.since(t14, "clip") if f[2].get("id", 0) > 0]
              and abs(anim.Time.Value - kept14) < 1e-9
              and paused is not None and not paused["playing"],
              (anim.Time.Value, kept14, paused))
        del beats[:]
        t15 = clock()
        Gui.runCommand("Std_AnimationPlay")
        kept15 = anim.Time.Value
        long_clip = baked(t15, 120.0)
        took = (long_clip[0] - t15) if long_clip else None
        gaps = [b - a for a, b in zip(beats, beats[1:])]
        metronome.stop()
        clip8 = long_clip[2] if long_clip else None
        check("played again it bakes a frame of its rate each, past 240",
              clip8 is not None and clip8["frames"] == 601
              and abs(clip8["step"] - 1.0 / 30) < 1e-9,
              clip8 and (clip8["frames"], clip8["step"]))
        note("the bake of 601 frames took %s s, the longest the events waited %s s, %d beats"
             % (took and round(took, 2), gaps and round(max(gaps), 3), len(beats)))
        check("and the events go on while it bakes",
              took is not None and took > 601 * Slow.SLOW_S and gaps and max(gaps) < 0.25
              and len(beats) > took / 0.1, (took, gaps and max(gaps), len(beats)))
        pump(0.5)
        played = anim.Time.Value - kept15
        check("the player waited for it and plays on from where it was",
              0.2 < played < 1.5, (kept15, anim.Time.Value))
        Gui.runCommand("Std_AnimationStop")
        pump(0.5)
        anim.End = 0.0
        anim.FrameRate = 20
        anim.removeTrack("Slow.Height")
        doc.removeObject("Slow")
        doc.recompute()
        pump(0.3)

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

        # --- an edit session while playing: the clip set aside, the host's
        # pose published, and the clip told again when it ends
        held = client.last("clip", t8)
        t16 = clock()
        Gui.ActiveDocument.setEdit(box, 1)
        pump(1.0)
        t17 = clock()
        pump(1.0)
        aside = client.last("clip", t16)
        tick = client.last("time", t17)
        check("an edit session while it plays tells the clients no clip",
              held is not None and held["id"] > 0 and aside is not None and aside["id"] == 0,
              (held and held["id"], aside and aside["id"]))
        check("the clock names none, and plays on",
              tick is not None and tick["clip"] == 0 and tick["playing"], tick)
        flowing = client.since(t17, "scene")
        check("and the host's pose is published as it moves", len(flowing) >= 3, len(flowing))
        t18 = clock()
        Gui.ActiveDocument.resetEdit()
        pump(1.0)
        back = client.last("clip", t18)
        check("the edit over, the same clip is told again",
              back is not None and held is not None and back["id"] == held["id"],
              (back and back["id"], held and held["id"]))
        t19 = clock()
        pump(1.0)
        check("and nothing is published while it plays",
              len(client.since(t19, "scene")) == 0, len(client.since(t19, "scene")))

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
