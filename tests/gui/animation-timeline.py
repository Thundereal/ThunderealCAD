"""The animation timeline, driven as a user drives it (docs/Animation.md 3.9, 6).

A document with a cylinder, its Height keyed 10 mm at 0 s and 40 mm at
2 s. Claims:
  - Std_AnimationNew makes an Animation and shows the timeline, its keys
    view among the dock's children;
  - a click on the ruler at 1 s moves the time there (to a frame) and the
    Height follows: 25 mm;
  - dragging the key at 2 s to 3 s moves it, one undo step; undo takes it
    back;
  - the key menu's Interpolation > Constant sets the key's interpolation;
  - a key clicked and Delete pressed is removed;
  - the property editor marks Height: green between keys, yellow on a key,
    orange typed and not keyed;
  - Play advances the time by the clock, Pause holds it, Stop puts the
    design back (the Height before the animation) and turns Active off;
  - Record keys a Height typed at the time shown, and a view's
    Transparency;
  - I over the property editor's Radius row keys the Radius; bound to an
    expression too, the row's name turns dark orange (written twice);
  - none of the scrubbing and playing marks the document modified;
  - Key the camera, isometric at 0 s and from the top at 1 s: a Camera
    track; at 0.5 s the view's orientation is halfway on the shortest arc
    between them; Stop puts the camera back as it was before.

Runs offscreen as well: nothing here reads the 3D view's pixels.
"""
import os
import tempfile
import time
import traceback

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets

Qt = QtCore.Qt
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


def mouse(widget, kind, pos, button=Qt.LeftButton, buttons=None, mods=Qt.NoModifier):
    if buttons is None:
        buttons = Qt.NoButton if kind == QtCore.QEvent.MouseButtonRelease else button
    glob = widget.mapToGlobal(pos)
    ev = QtGui.QMouseEvent(kind, QtCore.QPointF(pos), QtCore.QPointF(glob), button, buttons, mods)
    QtWidgets.QApplication.sendEvent(widget, ev)


def click(widget, pos, mods=Qt.NoModifier):
    mouse(widget, QtCore.QEvent.MouseButtonPress, pos, mods=mods)
    mouse(widget, QtCore.QEvent.MouseButtonRelease, pos, mods=mods)
    pump(0.1)


def drag(widget, a, b):
    mouse(widget, QtCore.QEvent.MouseButtonPress, a)
    for i in range(1, 9):
        p = a + (b - a) * (i / 8.0)
        mouse(widget, QtCore.QEvent.MouseMove, p, button=Qt.NoButton, buttons=Qt.LeftButton)
        pump(0.02)
    mouse(widget, QtCore.QEvent.MouseButtonRelease, b)
    pump(0.2)


class Layout:
    """Where the keys view draws: its own layout (AnimationTimeline.cpp)"""

    def __init__(self, view, anim):
        self.view = view
        self.anim = anim

    def gutter(self):
        return min(240, max(140, self.view.width() // 4))

    def range(self):
        start = min(0.0, self.anim.Start.Value)
        ends = [k[0] for t in self.anim.Tracks for k in [(kk[1],) for kk in t["Keys"]]]
        end = self.anim.End.Value or max(ends + [0.0])
        end = max(end, start + 1.0)
        margin = (end - start) * 0.04
        return start - margin, end + margin

    def x(self, t):
        v0, v1 = self.range()
        g = self.gutter()
        return int(round(g + (t - v0) * (self.view.width() - g - 8) / (v1 - v0)))

    def at(self, row, t):
        return QtCore.QPoint(self.x(t), 22 + row * 20 + 10)


def rowIndex(editor, name):
    model = editor.model()

    def walk(parent):
        for r in range(model.rowCount(parent)):
            idx = model.index(r, 0, parent)
            if model.data(idx) == name:
                return idx
            found = walk(idx)
            if found is not None:
                return found
        return None

    return walk(QtCore.QModelIndex())


def rowColor(editor, name, role=Qt.BackgroundRole):
    idx = rowIndex(editor, name)
    if idx is None:
        return None
    c = editor.model().data(idx, role)
    return c.name() if isinstance(c, QtGui.QColor) else c


def run():
    try:
        App.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool(
            "AutoSaveEnabled", False)
        # A view's turn to a standard view is animated otherwise, and goes on
        # moving the camera after the animation's write
        App.ParamGet("User parameter:BaseApp/Preferences/View").SetBool(
            "UseNavigationAnimations", False)
        mw = Gui.getMainWindow()
        mw.resize(1300, 900)
        doc = App.newDocument("AnimationTimeline")
        doc.UndoMode = 1
        post = doc.addObject("Part::Cylinder", "Post")
        post.Height = 10
        doc.recompute()
        pump(0.5)

        Gui.runCommand("Std_AnimationNew")
        pump(0.5)
        anims = [o for o in doc.Objects if o.TypeId == "App::Animation"]
        check("New animation makes one", len(anims) == 1, [o.Name for o in doc.Objects])
        anim = anims[0]
        dock = next((d for d in mw.findChildren(QtWidgets.QDockWidget)
                     if d.objectName() == "Animation"), None)
        check("the timeline is shown", dock is not None and dock.isVisible())
        view = dock.findChild(QtWidgets.QWidget, "AnimationKeys")
        check("its keys view is there", view is not None and view.isVisible())

        anim.addKey("Post.Height", 0.0, 10.0)
        anim.addKey("Post.Height", 2.0, 40.0)
        pump(0.5)  # the keys view rebuilds, its range fitted to the keys
        layout = Layout(view, anim)

        # Scrub on the ruler
        click(view, QtCore.QPoint(layout.x(1.0), 10))
        check("a click on the ruler moves the time", abs(anim.Time.Value - 1.0) < 1.0 / 30 + 1e-9,
              anim.Time)
        check("the Height follows", abs(post.Height.Value - (10 + 30 * anim.Time.Value / 2)) < 1e-6,
              post.Height)
        check("the time snaps to a frame", abs(anim.Time.Value * 30 - round(anim.Time.Value * 30)) < 1e-6,
              anim.Time)

        # The property editor's marks: the post selected
        Gui.Selection.addSelection(post)
        pump(1.0)
        editor = [e for e in mw.findChildren(QtWidgets.QTreeView)
                  if e.metaObject().className().endswith("PropertyEditor") and e.isVisible()]
        editor = editor[0] if editor else None
        check("between keys: green", editor and rowColor(editor, "Height") == "#a6d88a",
              editor and rowColor(editor, "Height"))
        anim.Time = 2.0
        pump(0.3)
        check("on a key: yellow", editor and rowColor(editor, "Height") == "#f2d04a",
              editor and rowColor(editor, "Height"))
        anim.Time = 1.0
        doc.openTransaction("Height")
        post.Height = 33
        doc.commitTransaction()
        pump(0.3)
        check("typed, not keyed: orange", editor and rowColor(editor, "Height") == "#f29c46",
              editor and rowColor(editor, "Height"))
        doc.undo()
        pump(0.3)
        # I over the Radius row: Insert key
        idx = rowIndex(editor, "Radius")
        editor.setCurrentIndex(idx)
        editor.setFocus()
        QtWidgets.QApplication.sendEvent(editor, QtGui.QKeyEvent(
            QtCore.QEvent.KeyPress, Qt.Key_I, Qt.NoModifier, "i"))
        pump(0.3)
        radius = [t["Path"] for t in anim.Tracks]
        check("I over a row keys it", "Post.Radius" in radius, radius)
        # Keyed and bound to an expression: written twice, the name dark orange
        post.setExpression("Radius", "3 mm")
        doc.recompute()
        pump(0.3)
        check("keyed and driven: the name dark orange",
              rowColor(editor, "Radius", Qt.ForegroundRole) == "#b03800",
              rowColor(editor, "Radius", Qt.ForegroundRole))
        post.setExpression("Radius", None)
        anim.removeTrack("Post.Radius")
        doc.recompute()
        Gui.Selection.clearSelection()

        # Drag the key at 2 s to 3 s
        undo = doc.UndoCount
        drag(view, layout.at(0, 2.0), QtCore.QPoint(layout.x(3.0), layout.at(0, 2.0).y()))
        times = [k[0] for k in anim.keys("Post.Height")]
        check("a drag moves the key", times == [0.0, 3.0], times)
        check("one undo step", doc.UndoCount == undo + 1, (undo, doc.UndoCount))
        doc.undo()
        pump(0.3)
        times = [k[0] for k in anim.keys("Post.Height")]
        check("undo takes it back", times == [0.0, 2.0], times)

        # The key menu: Interpolation > Constant
        layout = Layout(view, anim)
        chosen = []

        def pick():
            menu = QtWidgets.QApplication.activePopupWidget()
            if menu is None:
                QtCore.QTimer.singleShot(100, pick)
                return
            for a in menu.actions():
                if a.text() == "Interpolation" and a.menu() is not None:
                    for b in a.menu().actions():
                        if b.text() == "Constant":
                            chosen.append(b.text())
                            b.trigger()
            menu.close()

        QtCore.QTimer.singleShot(300, pick)
        pos = layout.at(0, 0.0)
        QtWidgets.QApplication.sendEvent(view, QtGui.QContextMenuEvent(
            QtGui.QContextMenuEvent.Mouse, pos, view.mapToGlobal(pos)))
        pump(0.3)
        interp = anim.track("Post.Height")["Keys"][0]["Interp"]
        check("the key menu sets the interpolation", chosen and interp == "Constant", interp)

        # Delete a key
        anim.addKey("Post.Height", 1.0, 20.0)
        pump(0.3)
        layout = Layout(view, anim)
        click(view, layout.at(0, 1.0))
        view.setFocus()
        QtWidgets.QApplication.sendEvent(view, QtGui.QKeyEvent(
            QtCore.QEvent.KeyPress, Qt.Key_Delete, Qt.NoModifier))
        pump(0.3)
        times = [k[0] for k in anim.keys("Post.Height")]
        check("Delete removes the key selected", times == [0.0, 2.0], times)

        # Play, Pause, Stop: the edits above saved first, so what follows
        # is all that could mark the document modified
        anim.setInterp("Post.Height", 0.0, "Linear")
        # Saved through the Gui document: App's save leaves its flag alone
        doc.saveAs(os.path.join(tempfile.mkdtemp(), "timeline.FCStd"))
        Gui.ActiveDocument.save()
        pump(0.5)
        check("saved: not modified", not Gui.ActiveDocument.Modified)
        anim.Time = 0.3
        anim.Time = 0.0
        Gui.runCommand("Std_AnimationPlay")
        pump(0.6)
        t1 = anim.Time.Value
        check("Play advances the time by the clock", 0.3 < t1 < 1.2, t1)
        Gui.runCommand("Std_AnimationPause")
        t2 = anim.Time.Value
        pump(0.4)
        check("Pause holds it", abs(anim.Time.Value - t2) < 1e-9, (t2, anim.Time.Value))
        check("the pose stays", abs(post.Height.Value - (10 + 15 * t2)) < 1e-6, post.Height)
        Gui.runCommand("Std_AnimationStop")
        pump(0.3)
        check("Stop puts the design back", abs(post.Height.Value - 10) < 1e-9, post.Height)
        check("Stop turns Active off", not anim.Active)
        check("scrubbing and playing are no edits", not Gui.ActiveDocument.Modified)

        # Record
        anim.Time = 1.5
        Gui.runCommand("Std_AnimationRecord")
        pump(0.2)
        check("Record turns Recording on", anim.Recording)
        doc.openTransaction("Height")
        post.Height = 50
        doc.commitTransaction()
        pump(0.3)
        keys = [(k[0], k[1].Value) for k in anim.keys("Post.Height")]
        check("Record keys the Height at the time shown", (1.5, 50.0) in keys, keys)
        doc.openTransaction("Transparency")
        post.ViewObject.Transparency = 50
        doc.commitTransaction()
        pump(0.3)
        paths = [t["Path"] for t in anim.Tracks]
        check("Record keys a view's Transparency", "Post.ViewObject.Transparency" in paths
              and anim.value("Post.ViewObject.Transparency", 1.5) == 50, paths)
        Gui.runCommand("Std_AnimationRecord")
        check("Record off", not anim.Recording)
        Gui.runCommand("Std_AnimationStop")
        pump(0.3)
        check("a recorded pose is no design edit", abs(post.Height.Value - 10) < 1e-9, post.Height)
        check("nor a recorded view", post.ViewObject.Transparency == 0, post.ViewObject.Transparency)

        # The camera
        view3d = Gui.ActiveDocument.ActiveView
        anim.Time = 0.0
        Gui.runCommand("Std_AnimationStop")
        view3d.viewIsometric()
        pump(0.3)
        iso = view3d.getCameraOrientation()
        Gui.runCommand("Std_AnimationKeyCamera")
        anim.Time = 1.0
        view3d.viewTop()
        pump(0.3)
        top = view3d.getCameraOrientation()
        Gui.runCommand("Std_AnimationKeyCamera")
        cams = [t for t in anim.Tracks if t["Kind"] == "Camera"]
        check("Key the camera makes a Camera track",
              len(cams) == 1 and len(cams[0]["Keys"]) == 2, cams)
        view3d.viewFront()
        pump(0.2)
        front = view3d.getCameraOrientation()
        anim.Time = 0.5
        pump(0.3)
        half = view3d.getCameraOrientation()
        want = iso.slerp(top, 0.5)
        check("at 0.5 s the camera is halfway", half.isSame(want, 1e-4)
              or half.isSame(App.Rotation(*[-c for c in want.Q]), 1e-4), (half.Q, want.Q))
        Gui.runCommand("Std_AnimationStop")
        pump(0.3)
        back = view3d.getCameraOrientation()
        check("Stop puts the camera back", back.isSame(front, 1e-4), (back.Q, front.Q))
        App.closeDocument(doc.Name)
    except Exception:
        note("ABORT\n" + traceback.format_exc())
    note("DONE")
    QtCore.QTimer.singleShot(200, QtWidgets.QApplication.quit)


QtCore.QTimer.singleShot(1000, run)
