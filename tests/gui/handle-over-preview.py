"""A handle draws over a dress-up's on-top preview (entry 185).

The reporter 2026-10-19 15:49: "the handle is displayed after the preview
shape. It should draw above it, i.e. with higher show-on-top priority". A
10 mm box, one edge filleted 3 mm, in edit. The handles' container is a 3D
annotation (So3DAnnotation) at priority -1; the preview's groups are path
annotations (SoFCPathAnnotation) at -2: the handles draw over the preview.

The scene is made to have the arrow over the preview: the view looks nearly
down the arrow's own axis at its foot, which lies on the fillet's face, at
20 px/mm whatever the window's height (the arrow itself is sized to the
screen, 1.5% of the view's height). Three render dumps, every pixel read:
  - the preview hidden: the arrow's own pixels, its mask;
  - the preview shown, the handles at 0 (under it): fewer of the mask's
    pixels are the arrow's colour -- where they are not, the preview covers
    them; if none is covered the scene no longer has them meet, and that is
    a FAIL of the scene by name;
  - the preview shown, the handles at -1 (as built): the whole mask shows.
HOP_HANDLE_PRIORITY replaces -1 in the last dump (a proof that the check
bites: -3, under the preview, must fail it). Pictures: alone.png,
priority0.png, priority-1.png.

Before: two dumps at the view fitAll left, counted every second pixel. On
the Mac mini and on Windows the arrow then stood beside the preview, not
over it -- two identical pictures, a FAIL that said nothing of the order.
"""
import os
import time
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
DOC = "HandleOverPreview"

state = {"done": False}


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def pump(turns=30):
    for _ in range(turns):
        QtWidgets.QApplication.processEvents()
        time.sleep(0.01)


def field_text(node, name):
    value = node.getField(name).get()
    return value.getString() if hasattr(value, "getString") else str(value)


def find_all(type_name):
    from pivy import coin

    view = FreeCADGui.getDocument(DOC).ActiveView
    root = view.getViewer().getSoRenderManager().getSceneGraph()
    search = coin.SoSearchAction()
    search.setType(coin.SoType.fromName(type_name))
    search.setInterest(coin.SoSearchAction.ALL)
    search.setSearchingAll(True)
    search.apply(root)
    return [p.getTail() for p in search.getPaths()]


def colour_mask(path, rgb):
    """Every pixel of the dump at `path` of the colour `rgb`"""
    img = QtGui.QImage(path)
    r, g, b = rgb
    mask = set()
    for y in range(img.height()):
        for x in range(img.width()):
            c = QtGui.QColor(img.pixel(x, y))
            if abs(c.red() - r) < 40 and abs(c.green() - g) < 40 and abs(c.blue() - b) < 40:
                mask.add((x, y))
    return mask


def pin_view(view, foot, axis, px_per_mm=20.0):
    """An orthographic view nearly down `axis` (35 degrees off it) at
    `foot`, at `px_per_mm` whatever the window's height"""
    import math

    view.setCameraType("Orthographic")
    axis = FreeCAD.Vector(axis)
    axis.normalize()
    side = axis.cross(FreeCAD.Vector(0, 0, 1))
    if side.Length < 1e-6:
        side = axis.cross(FreeCAD.Vector(1, 0, 0))
    side.normalize()
    eye = FreeCAD.Rotation(side, 35).multVec(axis)
    look = FreeCAD.Vector(-eye.x, -eye.y, -eye.z)
    view.setCameraOrientation(FreeCAD.Rotation(FreeCAD.Vector(0, 0, -1), look))
    pump(10)
    # a camera fitted to the scene (its clipping, its focal distance), then
    # moved onto the foot along the look it has
    view.fitAll()
    pump(20)
    cam = view.getCameraNode()
    distance = cam.focalDistance.getValue()
    pos = foot - look * distance
    cam.position.setValue(pos.x, pos.y, pos.z)
    cam.height.setValue(view.getSize()[1] / px_per_mm)
    pump(40)


def set_children(nodes, kept):
    """The preview's groups emptied (kept is None: their children taken
    and returned) or filled again from `kept`"""
    if kept is None:
        taken = []
        for n in nodes:
            children = [n.getChild(i) for i in range(n.getNumChildren())]
            for c in children:
                c.ref()
            n.removeAllChildren()
            taken.append(children)
        return taken
    for n, children in zip(nodes, kept):
        for c in children:
            n.addChild(c)
            c.unref()
    return None


def run():
    try:
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt(
            "AutoSaveTimeout", 0)
        doc = FreeCAD.newDocument(DOC)
        body = doc.addObject("PartDesign::Body", "Body")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = box.Height = 10
        doc.recompute()
        fillet = body.newObject("PartDesign::Fillet", "Fillet")
        fillet.Base = (box, ["Edge1"])
        fillet.Radius = 3
        doc.recompute()
        view = FreeCADGui.getDocument(DOC).ActiveView
        view.viewIsometric()
        gdoc = FreeCADGui.getDocument(DOC)
        gdoc.setEdit(fillet, 0)
        pump(120)
        FreeCADGui.SendMsgToActiveView("ViewFit")
        pump(60)
        check("the fillet is in edit", gdoc.getInEdit() is not None)

        # registered under its full name (the editing root's path reads
        # "SoFCEditingRoot > So3DAnnotation"), unlike Coin's own types
        annotations = find_all("So3DAnnotation")
        paths = find_all("SoFCPathAnnotation")
        note("INFO the scene's 3D annotations (the handles): %d, priorities %s; its path"
             " annotations: %d, priorities %s"
             % (len(annotations), [field_text(n, "priority") for n in annotations],
                len(paths), [field_text(n, "priority") for n in paths]))
        if not check("the gizmo container's annotation is there", len(annotations) >= 1):
            return
        check("... its priority is -1 as built",
              any(field_text(n, "priority") == "-1" for n in annotations),
              [field_text(n, "priority") for n in annotations])
        preview = [n for n in paths if field_text(n, "priority") == "-2"]
        if not check("the preview's groups are there, at -2", bool(preview), len(paths)):
            return
        arrows = [a for a in find_all("SoLinearDraggerContainer")
                  if field_text(a, "visible") in ("TRUE", "1")]
        if not check("an arrow is shown", bool(arrows)):
            return
        arrow = arrows[0]
        rgb = tuple(int(float(v) * 255) for v in field_text(arrow, "color").split()[:3])
        foot = FreeCAD.Vector(*[float(v) for v in field_text(arrow, "translation").split()[:3]])
        x, y, z, angle = [float(v) for v in field_text(arrow, "rotation").split()[:4]]
        import math
        axis = FreeCAD.Rotation(FreeCAD.Vector(x, y, z), math.degrees(angle)).multVec(
            FreeCAD.Vector(0, 1, 0))
        note("INFO the arrow: colour %s, foot %s, axis %s" % (rgb, tuple(foot), tuple(axis)))
        pin_view(view, foot, axis)

        def dump(name, priority):
            for n in annotations:
                n.getField("priority").set(priority)
            view.redraw() if hasattr(view, "redraw") else None
            pump(60)
            path = os.path.join(OUT, name)
            view.saveRenderDump(path, metadata=False)
            return colour_mask(path, rgb)

        kept = set_children(preview, None)
        try:
            alone = dump("alone.png", "-1")
        finally:
            set_children(preview, kept)
        pump(30)
        under = dump("priority0.png", "0")
        over_priority = os.environ.get("HOP_HANDLE_PRIORITY", "-1")
        over = dump("priority%s.png" % over_priority, over_priority)
        for n in annotations:
            n.getField("priority").set("-1")
        shown_under = len(alone & under)
        shown_over = len(alone & over)
        note("INFO the arrow's own pixels %d; shown with the handles at 0: %d, at %s: %d"
             % (len(alone), shown_under, over_priority, shown_over))
        if not check("the arrow is drawn", len(alone) > 20, len(alone)):
            return
        check("the scene has the arrow and the preview meet (some of the arrow covered at 0)",
              len(alone) - shown_under >= 10, (len(alone), shown_under))
        check("over the preview: the whole arrow shows with the handles at %s" % over_priority,
              shown_over >= 0.95 * len(alone), (len(alone), shown_over))
        check("... and more of it than at 0", shown_over > shown_under + 5,
              (shown_under, shown_over))
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        finish()


def finish():
    if state["done"]:
        return
    state["done"] = True
    try:
        FreeCADGui.getDocument(DOC).resetEdit()
        pump()
    except Exception:
        pass
    for name in list(FreeCAD.listDocuments().keys()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
