"""An Animation's frame cache, and the detail a frame is shown and saved in.

docs/Animation.md sec 4, "The frame cache". A plate whose thickness the
Animation drives, holes cut through it: every frame is a boolean. With
Cache on a frame's recompute is kept -- the shape with the mesh it was
shown with -- and put back the next time; and while frames are kept or
saved, a rebuilt shape is meshed exact at once rather than coarse first
(Render::ExactDetail). What is asserted:

  - exact detail is held while an Animation with Cache on is active, and
    by nothing before or after;
  - a first pass keeps every frame, a second recomputes none and takes a
    fraction of the time;
  - a frame put back is the picture it was: the same pixels, and the
    picture of the same frame recomputed with the cache off;
  - a frame is complete as soon as it is drawn -- nothing refines after;
  - the timeline marks the frames kept;
  - playing goes frame by frame, out of the cache;
  - an edit of the design drops the frames;
  - an export holds exact detail while it saves, and gives it back.

Skips the pictures when the view has no renderer.

  scripts/gui-test.sh tests/gui/animation-cache.py /tmp/anim-cache
"""
import os
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
W, H = 480, 360
N = 6
now = time.perf_counter


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = now() + seconds
    while now() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.005)


def differing(a, b, tolerance=6):
    a = a.convertToFormat(QtGui.QImage.Format_RGB32)
    b = b.convertToFormat(QtGui.QImage.Format_RGB32)
    if a.size() != b.size():
        return -1
    pa, pb = bytes(a.constBits()), bytes(b.constBits())
    count = 0
    for i in range(0, len(pa), 4):
        if (abs(pa[i] - pb[i]) > tolerance or abs(pa[i + 1] - pb[i + 1]) > tolerance
                or abs(pa[i + 2] - pb[i + 2]) > tolerance):
            count += 1
    return count


def shot(view, name):
    path = os.path.join(OUT, name)
    view.saveImage(path, W, H, "Current")
    return QtGui.QImage(path)


def build(doc):
    box = doc.addObject("Part::Box", "Plate")
    box.Length = box.Width = 20.0 * N
    box.Height = 10
    holes = []
    for i in range(N):
        for j in range(N):
            c = doc.addObject("Part::Cylinder", "Hole")
            c.Radius = 6
            c.Height = 60
            c.Placement.Base = App.Vector(10 + 20 * i, 10 + 20 * j, -10)
            holes.append(c)
    tool = doc.addObject("Part::Compound", "Tool")
    tool.Links = holes
    cut = doc.addObject("Part::Cut", "Cut")
    cut.Base = box
    cut.Tool = tool
    doc.recompute()
    return box, cut


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        # The platform's backend, as a user gets it (RenderParams::
        # engineType): on macOS not OpenGL, whose 2.1 context there the
        # bgfx OpenGL backend declines (docs/Testing.md).
        App.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", "Default")
        mw = Gui.getMainWindow()
        mw.resize(1300, 900)
        doc = App.newDocument("AnimationCache")
        box, cut = build(doc)
        Gui.runCommand("Std_AnimationNew")
        pump(0.5)
        anim = next(o for o in doc.Objects if o.TypeId == "App::Animation")
        anim.FrameRate = 10
        anim.Loop = "Loop"
        anim.addKey("Plate.Height", 0.0, 10.0)
        anim.addKey("Plate.Height", 1.0, 30.0)
        anim.stop()
        doc.recompute()
        view = Gui.ActiveDocument.ActiveView
        view.viewIsometric()
        Gui.SendMsgToActiveView("ViewFit")
        pump(1.0)
        times = anim.frames()
        rendered = view.waitFrameComplete(20000)
        note("renderer complete at rest: %s" % rendered)

        check("nothing holds exact detail before", not Gui.exactDetail())

        # --- the cache off: every frame recomputed, each pass
        off = []
        anim.activate()
        for t in times:
            a = now()
            anim.Time = t
            off.append(now() - a)
        plain = {}
        for k in (3, 7):
            anim.Time = times[k]
            plain[k] = shot(view, "plain-%d.png" % k)
        check("with the cache off nothing is kept", anim.cacheInfo()["Times"] == [])
        check("and nothing holds exact detail", not Gui.exactDetail())
        anim.stop()

        # --- the cache on
        anim.Cache = True
        check("turned on, an Animation at rest holds nothing", not Gui.exactDetail())
        first = []
        anim.activate()
        for t in times:
            a = now()
            anim.Time = t
            first.append(now() - a)
        check("active, it holds exact detail", Gui.exactDetail())
        info = anim.cacheInfo()
        check("a first pass keeps every frame",
              len(info["Times"]) == len(times) and info["Stored"] == len(times),
              (len(info["Times"]), info["Stored"], info["Replayed"]))
        check("and says what they hold", info["Bytes"] > 0, info["Bytes"])
        volumes = []
        second = []
        for t in times:
            a = now()
            anim.Time = t
            second.append(now() - a)
            volumes.append(cut.Shape.Volume)
        again = anim.cacheInfo()
        check("a second pass recomputes none",
              again["Replayed"] - info["Replayed"] == len(times)
              and again["Stored"] == info["Stored"],
              (again["Replayed"] - info["Replayed"], again["Stored"]))
        ms = lambda v: round(1000 * sum(v) / len(v), 1)
        check("and takes a fraction of the time",
              sum(second) < 0.5 * sum(first), "off %s, first %s, second %s ms a frame"
              % (ms(off), ms(first), ms(second)))
        check("the frames put back are the plate growing",
              all(b > a for a, b in zip(volumes, volumes[1:])), [round(v) for v in volumes[::3]])

        # --- the pictures
        if plain[3].width() == W:
            for k in (3, 7):
                anim.Time = times[k]
                if rendered:
                    a = now()
                    done = view.waitFrameComplete(20000)
                    took = now() - a
                    check("frame %d put back is complete at once" % k, done and took < 2.0,
                          "%.2f s" % took)
                a = now()
                one = shot(view, "kept-%d.png" % k)
                note("a picture of frame %d took %.2f s" % (k, now() - a))
                anim.Time = times[(k + 4) % len(times)]
                pump(0.1)
                anim.Time = times[k]
                two = shot(view, "kept-%d-again.png" % k)
                d = differing(one, two)
                check("frame %d is the same picture each time" % k, d == 0, d)
                d = differing(one, plain[k])
                check("and the picture of the frame recomputed", 0 <= d <= W * H // 500, d)
            moved = differing(shot(view, "kept-a.png"), plain[3])
            check("which is not another frame's", moved > W * H // 200, moved)
        else:
            note("SKIP the pictures: the view gives none")

        # --- the timeline
        dock = next((d for d in mw.findChildren(QtWidgets.QDockWidget)
                     if d.objectName() == "Animation"), None)
        keys = dock.findChild(QtWidgets.QWidget, "AnimationKeys") if dock else None
        if check("the timeline is shown", keys is not None and keys.isVisible()):
            keys.update()
            pump(0.3)
            image = keys.grab().toImage()
            image.save(os.path.join(OUT, "timeline-kept.png"))
            green = 0
            for y in range(image.height()):
                for x in range(image.width()):
                    c = image.pixelColor(x, y)
                    if c.green() > 140 and c.red() < 110 and c.blue() < 130:
                        green += 1
            check("and marks the frames kept", green > 50, green)

        # --- playing
        before = anim.cacheInfo()
        seen = set()
        Gui.runCommand("Std_AnimationPlay")
        end = now() + 1.5
        while now() < end:
            pump(0.02)
            seen.add(round(anim.Time.Value, 6))
        Gui.runCommand("Std_AnimationPause")
        pump(0.2)
        after = anim.cacheInfo()
        grid = all(abs(t * 10 - round(t * 10)) < 1e-4 for t in seen)
        check("playing goes frame by frame", len(seen) >= 5 and grid, sorted(seen))
        check("out of the cache", after["Replayed"] > before["Replayed"]
              and after["Stored"] == before["Stored"],
              (before["Replayed"], after["Replayed"], after["Stored"]))

        # --- an edit
        box.Length = 20.0 * N + 5
        doc.recompute()
        check("an edit of the design drops the frames", anim.cacheInfo()["Times"] == [],
              len(anim.cacheInfo()["Times"]))
        anim.Time = times[6]
        anim.Time = times[2]
        check("and the next frames are recomputed and kept",
              anim.cacheInfo()["Times"] == [times[2], times[6]], anim.cacheInfo()["Times"])
        Gui.runCommand("Std_AnimationStop")
        pump(0.3)
        check("stopped, nothing holds exact detail", not Gui.exactDetail())

        # --- an export
        from freecad import videoexport
        anim.Cache = False
        held = []

        def show(_i, t):
            anim.Time = t
            held.append(Gui.exactDetail())

        done = videoexport.export(os.path.join(OUT, "frame.png"), times[:4], show, view=view,
                                  size=(W, H), fps=10)
        anim.stop()
        check("an export holds exact detail for each frame",
              done["Frames"] == 4 and held == [True] * 4, held)
        check("and gives it back", not Gui.exactDetail())
        held.clear()
        videoexport.export(os.path.join(OUT, "loose.png"), times[:2], show, view=view,
                           size=(W, H), fps=10, exactDetail=False)
        anim.stop()
        check("unless told not to", held == [False] * 2, held)
    except Exception:
        note("FAIL the script:\n" + traceback.format_exc())
    finish()


def finish():
    try:
        Gui.runCommand("Std_AnimationStop")
    except Exception:
        pass
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
