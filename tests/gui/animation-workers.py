"""Frames of an Animation made by worker processes, in the GUI.

docs/Animation.md sec 4.2. A plate whose thickness the Animation drives,
64 holes cut through it and every edge filleted: a frame is a boolean and
a fillet, a quarter of a second here. With Cache on, worker processes --
each a ThunderealCmd holding a copy of the document -- make the frames
not kept yet and write them as the cache writes a frame to disk; the cache
takes them in as frames on disk. What is asserted:

  - the workers start, and their frames come in through the event loop,
    with nothing asking: no change of Time, no wait;
  - the timeline marks a frame a worker is making, in amber, and the
    frames they made as frames on disk, in blue;
  - every frame after the first is a worker's, none made here;
  - a worker's frame is the picture of the frame recomputed here;
  - an export of the Animation hands its frames to workers and saves the
    same pictures;
  - an edit of the design stops them, and leaves none of their files.

Skips the pictures when the view has no renderer.

  scripts/gui-test.sh tests/gui/animation-workers.py /tmp/anim-workers
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
N = 8
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


def colours(image):
    """Pixels of the cache line's three colours: in memory, on disk, being made"""
    green = blue = amber = 0
    for y in range(image.height()):
        for x in range(image.width()):
            c = image.pixelColor(x, y)
            r, g, b = c.red(), c.green(), c.blue()
            if abs(r - 0x3C) < 24 and abs(g - 0xA8) < 24 and abs(b - 0x54) < 24:
                green += 1
            elif abs(r - 0x40) < 24 and abs(g - 0x80) < 24 and abs(b - 0xD0) < 24:
                blue += 1
            elif abs(r - 0xE0) < 24 and abs(g - 0x9A) < 24 and abs(b - 0x20) < 24:
                amber += 1
    return green, blue, amber


def build(doc):
    box = doc.addObject("Part::Box", "Plate")
    box.Length = box.Width = 20.0 * N
    box.Height = 10
    holes = []
    for i in range(N):
        for j in range(N):
            c = doc.addObject("Part::Cylinder", "Hole")
            c.Radius = 5
            c.Height = 60
            c.Placement.Base = App.Vector(10 + 20 * i, 10 + 20 * j, -10)
            holes.append(c)
    tool = doc.addObject("Part::Compound", "Tool")
    tool.Links = holes
    cut = doc.addObject("Part::Cut", "Cut")
    cut.Base = box
    cut.Tool = tool
    doc.recompute()
    top = doc.addObject("Part::Fillet", "Fillet")
    top.Base = cut
    top.Edges = [(i + 1, 1.0, 1.0) for i in range(len(cut.Shape.Edges))]
    doc.recompute()
    cut.ViewObject.Visibility = False
    return box, top


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        App.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", "Default")
        group = App.ParamGet("User parameter:BaseApp/Preferences/Animation")
        group.SetInt("Workers", 2)  # a test's share of the machine
        mw = Gui.getMainWindow()
        mw.resize(1300, 900)
        doc = App.newDocument("AnimationWorkers")
        box, top = build(doc)
        Gui.runCommand("Std_AnimationNew")
        pump(0.5)
        anim = next(o for o in doc.Objects if o.TypeId == "App::Animation")
        anim.FrameRate = 10
        anim.addKey("Plate.Height", 0.0, 10.0)
        anim.addKey("Plate.Height", 2.0, 20.0)
        anim.stop()
        doc.recompute()
        view = Gui.ActiveDocument.ActiveView
        view.viewIsometric()
        Gui.SendMsgToActiveView("ViewFit")
        pump(1.0)
        times = anim.frames()
        rendered = view.waitFrameComplete(20000)
        note("renderer complete at rest: %s, %d faces" % (rendered, len(top.Shape.Faces)))

        # --- pictures of frames recomputed here, the cache off
        plain = {}
        anim.activate()
        for k in (5, 15):
            anim.Time = times[k]
            view.waitFrameComplete(20000)
            plain[k] = shot(view, "plain-%d.png" % k)
        anim.Time = times[0]
        view.waitFrameComplete(20000)

        # --- the cache on, the workers on the rest
        anim.Cache = True
        anim.Time = times[0]
        stored = anim.cacheInfo()["Stored"]
        started = anim.prepareFrames(force=True)
        info = anim.workerInfo()
        check("the workers start", started > 0 and info["Running"], info)
        dock = next((d for d in mw.findChildren(QtWidgets.QDockWidget)
                     if d.objectName() == "Animation"), None)
        keys = dock.findChild(QtWidgets.QWidget, "AnimationKeys") if dock else None
        check("the timeline is shown", keys is not None and keys.isVisible())
        # Nothing asks: the frames come in through the event loop
        amber = 0
        arrivals = []
        peak = 0
        end = now() + 180
        while now() < end and anim.workerInfo()["Running"]:
            pump(0.1)
            arrivals.append(anim.cacheInfo()["Prepared"])
            peak = max(peak, anim.workerInfo()["Workers"])
            if keys is not None and not amber and anim.workerInfo()["Busy"]:
                image = keys.grab().toImage()
                amber = colours(image)[2]
                if amber:
                    image.save(os.path.join(OUT, "timeline-busy.png"))
        info = anim.workerInfo()
        cache = anim.cacheInfo()
        check("they finish", not info["Running"] and info["Failed"] == 0, info)
        check("as many as were let", peak == 2, peak)
        check("their frames come in with nothing asking, one by one",
              cache["Prepared"] == len(times) - 1 and len(set(arrivals)) > 3,
              (cache["Prepared"], sorted(set(arrivals))))
        check("none of them made here", cache["Stored"] - stored <= 1,
              (stored, cache["Stored"]))
        check("the timeline marks a frame being made", amber > 0, amber)
        if keys is not None:
            keys.update()
            pump(0.3)
            image = keys.grab().toImage()
            image.save(os.path.join(OUT, "timeline-prepared.png"))
            green, blue, amber = colours(image)
            check("and the frames made, as frames on disk", blue > 50 and amber == 0,
                  (green, blue, amber))

        # --- a worker's frame is the frame recomputed here
        if plain[5].width() == W:
            for k in (5, 15):
                anim.Time = times[k]
                view.waitFrameComplete(20000)
                d = differing(shot(view, "worker-%d.png" % k), plain[k])
                check("frame %d of a worker is the picture of the frame made here" % k,
                      0 <= d <= W * H // 500, d)
        else:
            note("SKIP the pictures: the view gives none")
        check("put back, not made again", anim.cacheInfo()["Stored"] - stored <= 1,
              anim.cacheInfo()["Stored"])

        # --- an export hands its frames to workers
        from freecad import videoexport

        anim.clearCache()
        anim.stop()
        before = anim.cacheInfo()
        done = videoexport.exportAnimation(anim, os.path.join(OUT, "export.png"),
                                           view=view, size=(W, H))
        after = anim.cacheInfo()
        check("an export saves every frame", done["Frames"] == len(times), done)
        check("with frames the workers made",
              after["Prepared"] > before["Prepared"],
              (after["Prepared"] - before["Prepared"], after["Stored"] - before["Stored"]))
        check("and leaves none running", not anim.workerInfo()["Running"], anim.workerInfo())
        if plain[5].width() == W:
            for k in (5, 15):
                image = QtGui.QImage(videoexport.frameName(os.path.join(OUT, "export.png"), k))
                d = differing(image, plain[k])
                check("its frame %d is the picture of the frame made here" % k,
                      0 <= d <= W * H // 500, d)

        # --- an edit stops them
        anim.clearCache()
        anim.activate()
        anim.Time = times[0]
        check("started again", anim.prepareFrames(force=True) > 0, anim.workerInfo())
        pump(0.5)
        box.Length = 20.0 * N + 5
        doc.recompute()
        info = anim.workerInfo()
        check("an edit of the design stops them",
              not info["Running"] and info["Workers"] == 0
              and info["Reason"] == "the design changed", info)
        frames = os.path.join(doc.TransientDir, "AnimationCache", anim.Name)
        left = [n for n in os.listdir(frames)] if os.path.isdir(frames) else []
        check("and leaves none of their files", not [n for n in left if "wframe" in n], left)
        group.RemInt("Workers")
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
