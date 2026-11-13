"""An export's frames have the renderer's effects at the frame's time.

docs/Animation.md sec 9: water, fire, particles and a user shader's
u_fcTime read one clock, the wall's. An export steps time itself, and a
frame takes as long to save as it takes -- so the exporter pins that clock
to each frame's time (Gui.setRenderClock, Render::AnimationClock), and the
fire in frame 12 is the fire at 0.4 s whatever the machine.

A flame (Render_Fire) over a box, drawn by the renderer. What is asserted:

  - the clock is the wall's until it is pinned, and again when released;
  - pinned to the same time, two images of the view are the same;
  - pinned to another time, the flame is drawn differently;
  - an export of the Animation pins it for each frame and gives it back;
  - the same export made twice gives the same frames, one by one, and the
    frames differ from each other;
  - with renderClock=False the exporter leaves the clock alone.

Skips when the view has no renderer (RenderCache is not 3, or no backend).

  scripts/gui-test.sh tests/gui/animation-export-clock.py /tmp/anim-clock
"""
import os
import platform
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
W, H = 480, 360


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds):
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.005)


def differing(a, b, tolerance=6):
    """How many pixels of two images differ by more than `tolerance` in a
    channel; -1 when they are not of one size"""
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


def run():
    try:
        from freecad import videoexport

        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
        prefs = App.ParamGet("User parameter:BaseApp/Preferences/View")
        prefs.SetInt("RenderCache", 3)
        prefs.SetBool("ShowNaviCube", False)
        prefs.SetBool("CornerCoordSystem", False)
        render = App.ParamGet("User parameter:BaseApp/Preferences/View/Render")
        render.SetString("Type", "bgfx - Metal" if platform.system() == "Darwin"
                         else "bgfx - OpenGL")
        render.SetBool("Volumetric", True)
        Gui.getMainWindow().resize(1300, 900)
        doc = App.newDocument("AnimationExportClock")
        box = doc.addObject("Part::Box", "Box")
        flame = doc.addObject("Part::Cone", "Flame")
        flame.Radius1, flame.Radius2, flame.Height = 4, 1, 14
        flame.Placement.Base = App.Vector(5, 5, 10)
        fvo = flame.ViewObject
        fvo.ShapeColor = (1.0, 0.55, 0.1)
        fvo.addProperty("App::PropertyBool", "Render_Fire").Render_Fire = True
        fvo.addProperty("App::PropertyFloat", "Render_FireIntensity").Render_FireIntensity = 0.0
        fvo.addProperty("App::PropertyFloat", "Render_FireDetail").Render_FireDetail = 0.0
        fvo.addProperty("App::PropertyFloat", "Render_FireSpeed").Render_FireSpeed = 1.0
        doc.recompute()
        anim = doc.addObject("App::Animation", "Anim")
        anim.FrameRate = 5
        anim.addKey("Box.Placement", 0.0)
        anim.addKey("Box.Placement", 1.0, App.Placement(App.Vector(0, 0, 0.5), App.Rotation()))
        doc.recompute()
        view = Gui.ActiveDocument.ActiveView
        view.viewIsometric()
        view.fitAll()
        pump(1.5)
        # The flame is a volumetric medium, lit through the scene light's
        # shadow map: the view's own switches for both
        for name in ("Render_Light", "Render_Volumetric"):
            if name in view.PropertiesList:
                setattr(view, name, True)
        pump(1.5)

        if not hasattr(Gui, "setRenderClock"):
            check("Gui.setRenderClock is there", False)
            return finish()
        check("the clock is the wall's until it is pinned", Gui.renderClock() is None,
              Gui.renderClock())

        Gui.setRenderClock(0.25)
        check("pinned, it reads the time it was given", Gui.renderClock() == 0.25,
              Gui.renderClock())
        a1 = shot(view, "pin-a1.png")
        pump(0.7)
        a2 = shot(view, "pin-a2.png")
        Gui.setRenderClock(1.75)
        b = shot(view, "pin-b.png")
        Gui.setRenderClock(0.25)
        a3 = shot(view, "pin-a3.png")
        Gui.setRenderClock(None)
        same, moved, back = differing(a1, a2), differing(a1, b), differing(a1, a3)
        if moved == 0:
            # No flame drawn at all: nothing here reads the clock
            note("SKIP the view draws no time-animated effect (no renderer behind it?) | %s"
                 % ((same, moved, back),))
            return finish()
        check("at the same time two images are the same", 0 <= same <= 20, same)
        check("at another time the flame is drawn differently", moved > 200, moved)
        check("and back at the first time as it was", 0 <= back <= 20, back)
        check("released, the clock is the wall's again", Gui.renderClock() is None)

        seen = []

        def progress(i, count):
            seen.append(Gui.renderClock())
            return True

        one = os.path.join(OUT, "one.png")
        two = os.path.join(OUT, "two.png")
        done = videoexport.exportAnimation(anim, one, size=(W, H), progress=progress)
        check("an export pins the clock for each frame",
              done["Frames"] == 6 and len(seen) == 7
              and all(abs(seen[i + 1] - i * 0.2) < 1e-9 for i in range(6)), seen)
        check("and gives it back", Gui.renderClock() is None, Gui.renderClock())
        pump(0.9)
        videoexport.exportAnimation(anim, two, size=(W, H))
        pairs = [differing(QtGui.QImage(videoexport.frameName(one, i)),
                           QtGui.QImage(videoexport.frameName(two, i))) for i in range(6)]
        check("the same export made twice gives the same frames", all(0 <= d <= 20
                                                                      for d in pairs), pairs)
        steps = [differing(QtGui.QImage(videoexport.frameName(one, i)),
                           QtGui.QImage(videoexport.frameName(one, i + 1))) for i in range(5)]
        check("and the frames differ from each other", all(d > 200 for d in steps), steps)

        # A caller that pinned it itself gets its own time back
        Gui.setRenderClock(3.5)
        videoexport.exportAnimation(anim, os.path.join(OUT, "three.png"), size=(W, H))
        check("an export leaves a clock somebody else pinned as it found it",
              Gui.renderClock() == 3.5, Gui.renderClock())
        seen[:] = []
        times = list(anim.frames())

        def show(i, t):
            seen.append(Gui.renderClock())

        videoexport.export(os.path.join(OUT, "four.png"), times, show, size=(W, H),
                           renderClock=False)
        check("with renderClock off the exporter leaves the clock alone",
              seen and all(s == 3.5 for s in seen), seen)
        Gui.setRenderClock(None)
    except Exception:
        note("FAIL the script:\n" + traceback.format_exc())
    finish()


def finish():
    try:
        Gui.setRenderClock(None)
    except Exception:
        pass
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, QtCore.QCoreApplication.quit)


QtCore.QTimer.singleShot(1500, run)
