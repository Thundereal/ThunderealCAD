"""Selectable false on a link, or on what it links, and the other's picks.

Asked by notes, 2026-11-28, beside 5e761e400b (finding B1), from reading
alone: does Selectable false on an App::Link switch off selection of the
linked object too, through selection nodes the two share? The link's
scene holds what it links, shared, and setSelectable() turned off every
SoFCSelection node it found below the link's root -- the linked object's
own nodes among them, so the two views wrote the same fields and the last
writer won.

Two pairs, each an original and a link to it beside it, all four shown:
a Part box (its view has no SoFCSelection node) and a mesh cube (its view
has them). For each pair, as a user would, the pointer moved over the
shape in the 3D view and clicked, and the pixel under it read:

  link false: the original is still preselected and selected, drawn so;
              the link is not;
  original false: neither is -- a link follows its target's Selectable,
              by design (LinkInfo::getSnapshot copies the target root's
              selectionStyle; getElementPicked refuses) -- and with the
              View preference OverrideSelectability on, the link is
              picked;

the same after a save and a reopen, from files saved in those states (B1
was about the restore path). And what the mesh original's own
SoFCSelection nodes say: on while it is selectable, whatever its link is
set to; off while it is not, also after its link is set back to true.
Before the change the link's false switched them off, and its true
switched an unselectable original's back on.
"""
import os
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui
from PySide6 import QtOpenGLWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
VIEW = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
STEPS = []
S = {}
V = FreeCAD.Vector
DOC = "LinkSelShared"
# where each shape's top face is, in the model: the original at x 0..10,
# its link 30 along x; the mesh pair 60 along y
PAIRS = {
    "part": ("Box", "BoxLink", V(5, 5, 10), V(35, 5, 10)),
    "mesh": ("Cube", "CubeLink", V(5, 65, 10), V(35, 65, 10)),
}


def note(msg):
    with open(RESULT, "a") as f:
        f.write(str(msg) + "\n")
        f.flush()


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def settle(ms=300):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(ms, loop.quit)
    loop.exec()


def preselected():
    pre = FreeCADGui.Selection.getPreselection()
    try:
        # nothing preselected is an empty SelectionObject, whose Object
        # raises "Object already deleted"
        return pre.Object.Name if pre and pre.Object else None
    except RuntimeError:
        return None


def selected():
    return sorted(set(s.Object.Name for s in FreeCADGui.Selection.getSelectionEx("", 0)))


def away():
    from pivy import coin
    manager = S["view"].getViewer().getSoEventManager()
    event = coin.SoLocation2Event()
    event.setPosition(coin.SbVec2s(3, 3))
    manager.processEvent(event)
    settle(300)


def frame():
    """What the 3D view shows: the render dump, or with no render engine
    the GL widget's own frame buffer."""
    view = S["view"]
    path = os.path.join(OUT, "frame.png")
    settle(300)
    try:
        view.saveRenderDump(path, metadata=False)
    except RuntimeError:
        gl = view.graphicsView().findChildren(QtOpenGLWidgets.QOpenGLWidget)
        if not gl and isinstance(view.graphicsView(), QtOpenGLWidgets.QOpenGLWidget):
            gl = [view.graphicsView()]
        img = gl[0].grabFramebuffer()
        img.reinterpretAsFormat(QtGui.QImage.Format_RGB32)
        img.save(path)
    return QtGui.QImage(path).convertToFormat(QtGui.QImage.Format_RGB32)


def colour_at(x, y):
    """The mean colour of a 5x5 patch around a viewport point (y up)."""
    img = frame()
    h = img.height()
    px, py = int(x), h - 1 - int(y)
    total, n = [0, 0, 0], 0
    for dx in range(-2, 3):
        for dy in range(-2, 3):
            if 0 <= px + dx < img.width() and 0 <= py + dy < h:
                c = QtGui.QColor(img.pixel(px + dx, py + dy))
                total = [total[0] + c.red(), total[1] + c.green(), total[2] + c.blue()]
                n += 1
    return tuple(int(v / max(n, 1)) for v in total)


def dist(a, b):
    return sum(abs(p - q) for p, q in zip(a, b))


def pick(point):
    """The pointer moved onto a point of the model, sent to the view as
    events of the scene, then a left click there. Returns what was
    preselected under the pointer and what the click selected."""
    from pivy import coin
    view = S["view"]
    manager = view.getViewer().getSoEventManager()
    FreeCADGui.Selection.clearSelection()
    away()
    x, y = view.getPointOnViewport(point)
    plain = colour_at(x, y)
    for dx, wait in ((40, 250), (4, 250), (1, 250), (0, 700)):
        event = coin.SoLocation2Event()
        event.setPosition(coin.SbVec2s(int(x) + dx, int(y) + dx))
        manager.processEvent(event)
        settle(wait)
    pre = preselected()
    hovered = colour_at(x, y)
    for state in (coin.SoButtonEvent.DOWN, coin.SoButtonEvent.UP):
        event = coin.SoMouseButtonEvent()
        event.setButton(coin.SoMouseButtonEvent.BUTTON1)
        event.setState(state)
        event.setPosition(coin.SbVec2s(int(x), int(y)))
        manager.processEvent(event)
        settle(200)
    settle(400)
    got = selected()
    away()
    chosen = colour_at(x, y)
    FreeCADGui.Selection.clearSelection()
    S["drawn"] = (dist(hovered, plain) > 60, dist(chosen, plain) > 60, plain, hovered, chosen)
    return pre, got


def view_of(doc):
    view = FreeCADGui.getDocument(doc.Name).mdiViewsOfType("Gui::View3DInventor")[0]
    S["view"] = view
    view.setCameraType("Orthographic")
    view.viewIsometric()
    settle(600)
    view.fitAll()
    settle(1500)


def judge(tag, off):
    """Off is 'link' or 'original': which view of each pair has Selectable
    false. Picks all four and says what each gave."""
    doc = FreeCAD.getDocument(DOC)
    for kind, (orig, link, p_orig, p_link) in PAIRS.items():
        state = (doc.getObject(orig).ViewObject.Selectable,
                 doc.getObject(link).ViewObject.Selectable)
        note("%s %s: Selectable original %s, link %s" % (tag, kind, state[0], state[1]))
        if kind == "mesh":
            note("NOTE %s mesh: the original's SoFCSelection nodes: %s" % (tag, mesh_nodes()))
        for who, name, point in (("original", orig, p_orig), ("link", link, p_link)):
            pre, got = pick(point)
            d = S["drawn"]
            note("NOTE %s %s: the %s drawn preselected %s, drawn selected %s"
                 " (plain %s, hovered %s, selected %s)"
                 % (tag, kind, who, d[0], d[1], d[2], d[3], d[4]))
            if who == off or off == "original":
                why = "Selectable false" if who == off else "its target's Selectable false"
                check("%s %s: the %s, %s, is not preselected" % (tag, kind, who, why),
                      pre is None, pre)
                check("%s %s: the %s, %s, is not selected" % (tag, kind, who, why),
                      got == [], got)
                check("%s %s: the %s, %s, is not drawn selected" % (tag, kind, who, why),
                      not d[1], d[2:])
            else:
                check("%s %s: the %s is preselected" % (tag, kind, who), pre == name, pre)
                check("%s %s: the %s is selected" % (tag, kind, who), got == [name], got)
                check("%s %s: the %s is drawn selected" % (tag, kind, who), d[1], d[2:])
    FreeCADGui.getMainWindow().grab().save(os.path.join(OUT, "%s.png" % tag.replace(" ", "-")))


def set_off(off):
    doc = FreeCAD.getDocument(DOC)
    for kind, (orig, link, _, _) in PAIRS.items():
        doc.getObject(orig).ViewObject.Selectable = off != "original"
        doc.getObject(link).ViewObject.Selectable = off != "link"
    settle(500)


def start():
    import Mesh
    FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Document").SetInt("AutoSaveTimeout", 0)
    VIEW.SetBool("EnablePreselection", True)
    if os.environ.get("GT_RENDER_CACHE"):
        VIEW.SetInt("RenderCache", int(os.environ["GT_RENDER_CACHE"]))
    note("NOTE render cache mode %d" % VIEW.GetInt("RenderCache", -1))
    doc = FreeCAD.newDocument(DOC)
    box = doc.addObject("Part::Box", "Box")
    box.Length = box.Width = box.Height = 10
    cube = doc.addObject("Mesh::Feature", "Cube")
    mesh = Mesh.createBox(10, 10, 10)
    mesh.translate(5, 65, 5)
    cube.Mesh = mesh
    for name, target, y in (("BoxLink", box, 0), ("CubeLink", cube, 0)):
        link = doc.addObject("App::Link", name)
        link.LinkedObject = target
        link.Placement.Base = V(30, y, 0)
    doc.recompute()
    view_of(doc)
    FreeCADGui.Selection.clearSelection()
    judge("baseline", None)


def saved(state):
    """The file for a state, named for the document so a reopen keeps its
    name."""
    folder = os.path.join(OUT, state)
    os.makedirs(folder, exist_ok=True)
    return os.path.join(folder, DOC + ".FCStd")


def mesh_nodes():
    """What the SoFCSelection nodes under the mesh original's own root say:
    the nodes a link to it shares."""
    from pivy import coin
    vp = FreeCAD.getDocument(DOC).getObject("Cube").ViewObject
    sa = coin.SoSearchAction()
    sa.setType(coin.SoType.fromName("SoFCSelection"))
    sa.setInterest(coin.SoSearchAction.ALL)
    sa.setSearchingAll(True)
    sa.apply(vp.RootNode)
    out = []
    for i in range(sa.getPaths().getLength()):
        node = sa.getPaths()[i].getTail()
        out.append("selectionMode=%s highlightMode=%s" % (
            node.getField("selectionMode").getValue(),
            node.getField("highlightMode").getValue()))
    return out


def nodes_are(tag, on):
    nodes = mesh_nodes()
    want = "selectionMode=0 highlightMode=0" if on else "selectionMode=1 highlightMode=2"
    check("%s: the mesh original's own SoFCSelection nodes are %s"
          % (tag, "on" if on else "off"), nodes and all(n == want for n in nodes), nodes)


def live_link_off():
    set_off("link")
    nodes_are("live link-off, the link set false", True)
    judge("live link-off", "link")
    FreeCAD.getDocument(DOC).saveAs(saved("link-off"))


def live_original_off():
    # set_off sets the original false first, then the link back to true
    set_off("original")
    nodes_are("live original-off, the link set back to true", False)
    judge("live original-off", "original")
    FreeCAD.getDocument(DOC).saveAs(saved("original-off"))


def reopen(path):
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    settle(500)
    doc = FreeCAD.openDocument(path)
    settle(1500)
    check("reopened as %s" % DOC, doc.Name == DOC, doc.Name)
    view_of(doc)


def reopen_link_off():
    reopen(saved("link-off"))
    nodes_are("reopen link-off", True)
    judge("reopen link-off", "link")


def reopen_original_off():
    reopen(saved("original-off"))
    nodes_are("reopen original-off", False)
    judge("reopen original-off", "original")


def override():
    """OverrideSelectability on: the link of an unselectable original is
    picked."""
    VIEW.SetBool("OverrideSelectability", True)
    settle(500)
    try:
        for kind, (orig, link, p_orig, p_link) in PAIRS.items():
            pre, got = pick(p_link)
            check("override %s: the link of an unselectable original is preselected"
                  % kind, pre == link, pre)
            check("override %s: the link of an unselectable original is selected"
                  % kind, got == [link], got)
            pre, got = pick(p_orig)
            note("NOTE override %s: the unselectable original: preselected %s, selected %s"
                 % (kind, pre, got))
    finally:
        VIEW.SetBool("OverrideSelectability", False)


def finish():
    for name in list(FreeCAD.listDocuments()):
        FreeCAD.closeDocument(name)
    note("DONE")
    QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


def advance():
    if not STEPS:
        finish()
        return
    step = STEPS.pop(0)
    try:
        step()
    except Exception:
        note("ABORT in %s: %s" % (step.__name__, traceback.format_exc().replace("\n", " | ")))
        STEPS[:] = []
    QtCore.QTimer.singleShot(800, advance)


STEPS[:] = [start, live_link_off, live_original_off, reopen_link_off, reopen_original_off,
            override]
QtCore.QTimer.singleShot(2500, advance)
