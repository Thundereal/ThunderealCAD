"""A frame shown for a picture holds back the geometry no view draws.

docs/WorkerProcess.md sec 16. A plate cut by a chain of holes, one cut
after another, the last cut shown and the plate and every cut before it
hidden, as the commands leave them; the Animation drives the plate's
thickness, so a frame writes every shape of the chain. Playing, scrubbing,
an export of pictures (`showAt`) put back of a kept frame only the shapes a
view draws, and hold back the rest until something may read it. What is
asserted:

  - the picture of a frame shown so is the picture of the frame set as
    Time, and the shown cut's shape is the frame's;
  - the hidden cuts and the plate are held back: their shapes are still
    the ones before, and `Owed` counts them;
  - `settle()` puts them in, each the frame's;
  - so do Time set (a frame put back whole), a recompute, an edit, a
    selection, Pause, Stop;
  - a frame read back from disk holds back the same, read no further, and
    stays on disk;
  - an object shown since is put back, not held back; showing it drops no
    frame, colouring it the frames holding what is made from it (sec 17);
  - the view does not build what it does not show: a hidden cut gets no
    visual while hidden, and one when shown.

  scripts/gui-test.sh tests/gui/animation-shown-holds-back.py /tmp/anim-shown
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
CHAIN = 6
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
    plate = doc.addObject("Part::Box", "Plate")
    plate.Length = plate.Width = 80
    plate.Height = 8
    base = plate
    cuts = []
    for k in range(CHAIN):
        hole = doc.addObject("Part::Cylinder", "Hole")
        hole.Radius = 5
        hole.Height = 80
        hole.Placement.Base = App.Vector(14 + 26 * (k % 3), 20 + 40 * (k // 3), -10)
        cut = doc.addObject("Part::Cut", "Cut")
        cut.Base = base
        cut.Tool = hole
        cuts.append(cut)
        base = cut
    doc.recompute()
    return plate, cuts


def top_of(obj):
    """The highest point of the object's visual, as the view last built it"""
    from pivy import coin
    sa = coin.SoSearchAction()
    sa.setType(coin.SoCoordinate3.getClassTypeId())
    sa.setInterest(coin.SoSearchAction.ALL)
    sa.apply(obj.ViewObject.RootNode)
    zs = [v[2] for p in sa.getPaths() for v in p.getTail().point.getValues()]
    return round(max(zs), 3) if zs else None


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetInt("RenderCache", 3)
        App.ParamGet("User parameter:BaseApp/Preferences/View/Render").SetString(
            "Type", "Default")
        workers = App.ParamGet("User parameter:BaseApp/Preferences/Workers")
        workers.SetInt("Workers", 0)  # every frame made here
        group = App.ParamGet("User parameter:BaseApp/Preferences/Animation")
        mw = Gui.getMainWindow()
        mw.resize(1300, 900)
        doc = App.newDocument("AnimationShown")
        plate, cuts = build(doc)
        top = cuts[-1]
        hidden = [plate] + cuts[:-1]
        check("the commands leave the chain hidden but its last cut",
              all(not o.ViewObject.Visibility for o in hidden) and top.ViewObject.Visibility,
              [o.Name for o in doc.Objects if o.ViewObject.Visibility])
        Gui.runCommand("Std_AnimationNew")
        pump(0.5)
        anim = next(o for o in doc.Objects if o.TypeId == "App::Animation")
        anim.FrameRate = 10
        anim.addKey("Plate.Height", 0.0, 8.0)
        anim.addKey("Plate.Height", 1.0, 20.0)
        anim.stop()
        doc.recompute()
        view = Gui.ActiveDocument.ActiveView
        view.viewIsometric()
        Gui.SendMsgToActiveView("ViewFit")
        pump(0.5)

        # The view does not build a hidden cut when the chain changes
        mid = cuts[2]
        plate.Height = 9
        doc.recompute()
        pump(0.5)
        # Never built, or built before the change: not at the new height
        check("a hidden cut is not built again when the chain changes", top_of(mid) != 9.0,
              "its visual's top at %s" % top_of(mid))
        check("the shown cut is", top_of(top) == 9.0, "its visual's top at %s" % top_of(top))
        mid.ViewObject.Visibility = True
        pump(0.3)
        check("a hidden cut shown is built", top_of(mid) == 9.0,
              "its visual's top at %s" % top_of(mid))
        mid.ViewObject.Visibility = False
        plate.Height = 8
        doc.recompute()
        pump(0.3)

        # Every frame set as Time: kept, and the volumes and pictures of each
        anim.Cache = True
        anim.activate()
        times = anim.frames()
        ref, pics = {}, {}
        for t in times:
            anim.Time = t
            pump(0.05)
            ref[t] = {o.Name: o.Shape.Volume for o in [plate] + cuts}
            pics[t] = shot(view, "ref-%02d.png" % times.index(t))
        info = anim.cacheInfo()
        check("each frame kept", len(info["Times"]) == len(times), len(info["Times"]))

        def same(t, objs):
            return all(abs(o.Shape.Volume - ref[t][o.Name]) <= 1e-6 * ref[t][o.Name]
                       for o in objs)

        t1, t3, t5, t7 = times[1], times[3], times[5], times[7]
        anim.Time = t1
        anim.showAt(t3)
        pump(0.05)
        info = anim.cacheInfo()
        check("shown: the plate and the hidden cuts are held back",
              info["Owed"] == len(hidden), "Owed %d of %d" % (info["Owed"], len(hidden)))
        check("shown: the shown cut is the frame's", same(t3, [top]))
        check("shown: a hidden cut is the one before", same(t1, [mid]),
              "%.3f, %.3f before" % (mid.Shape.Volume, ref[t1][mid.Name]))
        d = differing(shot(view, "shown-03.png"), pics[t3])
        check("shown: the picture is the frame's", 0 <= d < W * H // 500, "%d pixels differ" % d)
        settled = info["Settled"]
        anim.settle()
        info = anim.cacheInfo()
        check("settle(): nothing held back, each the frame's",
              info["Owed"] == 0 and same(t3, [plate] + cuts)
              and info["Settled"] - settled == len(hidden),
              "Owed %d, settled %d" % (info["Owed"], info["Settled"] - settled))
        check("settle(): nothing left touched", not any("Touched" in o.State for o in doc.Objects),
              [o.Name for o in doc.Objects if "Touched" in o.State])

        anim.showAt(t5)
        anim.Time = times[2]
        check("Time set: the frame put back whole",
              anim.cacheInfo()["Owed"] == 0 and same(times[2], [plate] + cuts))

        anim.showAt(t5)
        doc.recompute()
        check("a recompute: what was held back put in first",
              anim.cacheInfo()["Owed"] == 0 and same(t5, [plate] + cuts))

        anim.showAt(t7)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(doc.Name, mid.Name)
        check("a selection: what was held back put in",
              anim.cacheInfo()["Owed"] == 0 and same(t7, [plate] + cuts))
        Gui.Selection.clearSelection()

        # Shown since: put back, not held back. No result reads a step's
        # Visibility, nor a view property only the view has: every frame
        # stays (sec 17)
        kept = len(anim.cacheInfo()["Times"])
        mid.ViewObject.Visibility = True
        mid.ViewObject.LineWidth = 3
        pump(0.1)
        check("shown: every frame stays", len(anim.cacheInfo()["Times"]) == kept,
              "%d of %d" % (len(anim.cacheInfo()["Times"]), kept))
        # A colour is the object's look, which what is made from it takes
        # (ShapeAppearanceDesign.md sec 14.6.2): the frames holding the later
        # cuts' looks go
        mid.ViewObject.ShapeColor = (0.8, 0.2, 0.2)
        pump(0.1)
        check("coloured: the frames holding what follows go",
              len(anim.cacheInfo()["Times"]) < kept, anim.cacheInfo()["Times"])
        anim.Time = t3
        anim.Time = t5
        touched = [o.Name for o in doc.Objects if "Touched" in o.State]
        replayed = anim.cacheInfo()["Replayed"]
        anim.showAt(t3)
        check("a cut shown since is put back", same(t3, [mid]) and anim.cacheInfo()["Owed"]
              == len(hidden) - 1, "Owed %d, replayed %d, touched before %s" % (
                  anim.cacheInfo()["Owed"], anim.cacheInfo()["Replayed"] - replayed, touched))
        mid.ViewObject.Visibility = False
        anim.settle()

        # Played, then paused
        anim.Time = times[0]
        Gui.runCommand("Std_AnimationPlay")
        pump(0.6)
        Gui.runCommand("Std_AnimationPause")
        pump(0.05)
        at = anim.Time
        frame = min(times, key=lambda x: abs(x - at))
        check("paused: nothing held back, the frame's",
              anim.cacheInfo()["Owed"] == 0 and same(frame, [plate] + cuts),
              "Time %.3f" % at)
        check("played: held back on the way", anim.cacheInfo()["Deferred"] > 0,
              anim.cacheInfo()["Deferred"])

        # From disk: read no further than what is drawn
        group.SetInt("CacheLimitBytes", 1)
        anim.Time = t1
        loaded = anim.cacheInfo()["Loaded"]
        ondisk = len(anim.cacheInfo()["OnDisk"])
        anim.showAt(t5)
        info = anim.cacheInfo()
        check("from disk: read back, the shown cut the frame's",
              info["Loaded"] == loaded + 1 and same(t5, [top]), info["Loaded"] - loaded)
        check("from disk: the rest held back", info["Owed"] == len(hidden), info["Owed"])
        check("from disk: the frame stays on disk", len(info["OnDisk"]) >= ondisk,
              "%d, %d before" % (len(info["OnDisk"]), ondisk))
        d = differing(shot(view, "shown-disk-05.png"), pics[t5])
        check("from disk: the picture is the frame's", 0 <= d < W * H // 500, "%d pixels" % d)
        anim.settle()
        check("from disk: settled, each the frame's",
              anim.cacheInfo()["Owed"] == 0 and same(t5, [plate] + cuts))
        group.RemInt("CacheLimitBytes")

        # An edit: of the document as it is at Time
        anim.showAt(t3)
        doc.getObject("Hole").Radius = 4
        check("an edit: what was held back put in first", anim.cacheInfo()["Owed"] == 0)
        doc.recompute()

        # Stop: the rest values, and what they recompute to
        anim.showAt(t5)
        Gui.runCommand("Std_AnimationStop")
        pump(0.1)
        rest = {o.Name: o.Shape.Volume for o in [plate] + cuts}
        for o in [plate] + cuts:
            o.touch()
        doc.recompute()
        check("stopped: nothing held back, the design's shapes, as recomputed",
              anim.cacheInfo()["Owed"] == 0
              and all(abs(o.Shape.Volume - rest[o.Name]) <= 1e-6 * rest[o.Name]
                      for o in [plate] + cuts))
    except Exception:
        note("ABORT " + traceback.format_exc())
    finally:
        App.ParamGet("User parameter:BaseApp/Preferences/Workers").RemInt("Workers")
        App.ParamGet("User parameter:BaseApp/Preferences/Animation").RemInt("CacheLimitBytes")
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
