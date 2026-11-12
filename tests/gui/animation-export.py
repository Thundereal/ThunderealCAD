"""An Animation exported (docs/Animation.md sec 9), by the exporter the
Assembly simulation shares (freecad.videoexport).

A document with a box, its Placement keyed up 6 mm over 1 s at 10 frames
a second, which keeps it in the view. Claims:
  - an image sequence: a PNG for each of the 11 frames, numbered, of the
    size asked for, the first, the middle and the last three pictures;
  - a GIF of 11 frames (when there is an encoder for one);
  - a video of the size asked for (when there is an encoder for one);
  - a progress callback saying no stops the export, and leaves no file;
  - after each, the animation is not active and the box is where the
    design has it; exported while active at 0.5 s, it is back at 0.5 s;
  - none of it marks the document modified;
  - Std_AnimationExport, its file dialog answered with a .glb, writes a
    binary glTF.

Needs the 3D view's pixels: run in a real window or under xvfb.
"""
import os
import time
import traceback
from unittest import mock

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(seconds=0.2):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def run():
    try:
        from freecad import videoexport

        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "AutoSaveEnabled", False)
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool(
            "UseNavigationAnimations", False)
        Gui.getMainWindow().resize(1300, 900)
        doc = App.newDocument("AnimationExport")
        doc.UndoMode = 1
        box = doc.addObject("Part::Box", "Box")
        doc.recompute()
        anim = doc.addObject("App::Animation", "Anim")
        anim.FrameRate = 10
        anim.addKey("Box.Placement", 0.0)
        anim.addKey("Box.Placement", 1.0, App.Placement(App.Vector(0, 0, 6), App.Rotation()))
        doc.recompute()
        view = Gui.ActiveDocument.ActiveView
        view.viewFront()
        view.fitAll()
        pump(0.5)
        saved = os.path.join(OUT, "export.FCStd")
        doc.saveAs(saved)
        # An App save leaves the Gui's flag alone
        Gui.ActiveDocument.save()
        pump(0.2)

        seq = os.path.join(OUT, "seq.png")
        done = videoexport.exportAnimation(anim, seq, size=(321, 240))
        files = [videoexport.frameName(seq, i) for i in range(11)]
        check("an image sequence: a PNG for each frame",
              done["Frames"] == 11 and all(os.path.isfile(f) for f in files)
              and not os.path.exists(videoexport.frameName(seq, 11)), done)
        first, last = QtGui.QImage(files[0]), QtGui.QImage(files[-1])
        check("of the size asked for, made even",
              (first.width(), first.height()) == (320, 240) and done["Size"] == (320, 240),
              (first.width(), first.height()))
        middle = QtGui.QImage(files[5])
        check("the first, the middle and the last frame differ",
              first != last and first != middle and middle != last)
        check("the animation is stopped and the box back",
              not anim.Active and abs(box.Placement.Base.z) < 1e-9, box.Placement.Base.z)

        known = dict((ext, label) for label, ext in videoexport.formats())
        if ".gif" in known:
            gif = os.path.join(OUT, "anim.gif")
            videoexport.exportAnimation(anim, gif, size=(160, 120))
            reader = QtGui.QImageReader(gif)
            count = 0
            while not reader.read().isNull():
                count += 1
            check("a GIF of 11 frames", count == 11, count)
        else:
            note("SKIP no GIF encoder here")
        video = next((e for e in (".mp4", ".webm", ".avi") if e in known), None)
        if video:
            path = os.path.join(OUT, "anim" + video)
            done = videoexport.exportAnimation(anim, path, size=(320, 240))
            check("a video is written",
                  os.path.isfile(path) and os.path.getsize(path) > 500
                  and done["Encoder"] in ("av", "ffmpeg"),
                  (video, done["Encoder"], os.path.getsize(path) if os.path.isfile(path) else 0))
        else:
            note("SKIP no video encoder here")

        stopped = os.path.join(OUT, "stopped.gif" if ".gif" in known else "stopped.png")
        asked = []

        def progress(index, count):
            asked.append((index, count))
            return index < 3

        try:
            videoexport.exportAnimation(anim, stopped, size=(160, 120), progress=progress)
            check("a progress callback saying no stops the export", False)
        except videoexport.Cancelled:
            check("a progress callback saying no stops the export",
                  asked[-1] == (3, 11) and not os.path.exists(stopped), asked)
        check("and leaves the animation stopped", not anim.Active)

        anim.Time = 0.5
        pump(0.2)
        videoexport.exportAnimation(anim, os.path.join(OUT, "active.png"), size=(160, 120))
        check("exported while active at 0.5 s, it is back at 0.5 s",
              anim.Active and abs(anim.Time - 0.5) < 1e-9
              and abs(box.Placement.Base.z - 3.0) < 1e-6,
              (anim.Active, anim.Time, box.Placement.Base.z))
        anim.stop()
        pump(0.2)
        check("none of it marks the document modified", not Gui.ActiveDocument.Modified)

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(anim)
        glb = os.path.join(OUT, "anim.glb")
        with mock.patch.object(QtWidgets.QFileDialog, "getSaveFileName",
                               return_value=(glb, "Animated glTF, binary (*.glb)")):
            Gui.runCommand("Std_AnimationExport")
        pump(0.3)
        head = b""
        if os.path.isfile(glb):
            with open(glb, "rb") as f:
                head = f.read(4)
        check("Std_AnimationExport writes a binary glTF", head == b"glTF", head)
        check("still not modified", not Gui.ActiveDocument.Modified)
        App.closeDocument(doc.Name)
    except Exception:
        note("ABORT\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(200, QtWidgets.QApplication.quit)


QtCore.QTimer.singleShot(1000, run)
