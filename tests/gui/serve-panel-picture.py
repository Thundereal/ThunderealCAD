"""A served user's own panel shows its pictures (docs/TaskPanelPerView.md
sec 16, sec 13.5).

A custom-painted widget has nothing to name to a browser, and the panel
mirror sends it as a picture: a grab of the widget, by image id. A served
user's dialog under PerViewEdit is never the page the desktop's Tasks tab
shows, so it is hidden there -- and a hidden widget used to give no
picture at all, nor ever a new one, since what told the mirror to grab
again was the widget being painted.

alice, served with PerViewEdit on, puts Pad1 in edit and has her panel;
a painted leaf (red) is put into its task box. Claims:
  1. the leaf is on a page the desktop does not show (the case);
  2. alice's panel has the leaf with a picture, the leaf's size, red;
  3. the leaf repainted blue: a new picture, blue, with no paint to
     announce it;
  4. a leaf nothing changes keeps its picture: no new id each poll;
  5. and those looks rebuild nothing.
"""
import base64
import os
import tempfile
import traceback

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

import serve_pd_multi_user as m
from serve_pd_multi_user import ALICE, User, check, does, settle, field_under, own_panel

QUIT = {"done": False}


class Painted(QtWidgets.QWidget):
    """A leaf only its paint says anything about"""

    def __init__(self):
        super().__init__()
        self.setObjectName("paintedLeaf")
        self.setMinimumSize(48, 24)
        self.color = QtGui.QColor(255, 0, 0)

    def paintEvent(self, event):
        p = QtGui.QPainter(self)
        p.fillRect(self.rect(), self.color)
        p.end()


def picture_of(user, field):
    """The leaf's picture as the user would fetch it: (id, QImage)"""
    snap = FreeCADGui.FormWidgets.snapshot(field)
    pid = snap["state"].get("q_pixmap") or ""
    if not pid.startswith("img:"):
        return pid, None
    reply = does(user, "widgets.image", {"name": pid}) or {}
    if not reply.get("ok"):
        return pid, None
    return pid, QtGui.QImage.fromData(base64.b64decode(reply["data"]), "PNG")


def centre(image):
    c = image.pixelColor(image.width() // 2, image.height() // 2)
    return c.red(), c.green(), c.blue()


def run():
    users = []
    try:
        m.state["log"] = m.DOCP.GetInt("TransactionLog", 2)
        m.VIEW.SetBool("PerViewEdit", True)
        m.PART.SetBool("PreviewOnEdit", True)
        doc = FreeCAD.newDocument("PanelPicture")
        body = doc.addObject("PartDesign::Body", "Body")
        pad = body.newObject("PartDesign::Pad", "Pad1")
        pad.Profile = m.rectangle(body, "Sketch1", 0, 0, 10, 0)
        pad.Length = 15
        doc.recompute()
        doc.saveAs(os.path.join(tempfile.mkdtemp(prefix="fc-panel-pic-"), "p.FCStd"))
        settle(1000)

        port = m.free_port()
        if not check("the document is served", FreeCADGui.serveDocument(doc, port)):
            return
        FreeCADGui.serveSetGrants([{"identity": ALICE, "access": 0}, {"access": 1}])
        alice = User(port, "alice", ALICE)
        users = [alice]
        alice.start()
        does(alice, "join")

        entered = does(alice, "edit", {"obj": "Pad1"})
        root = own_panel(alice)
        edits = [w for w in QtWidgets.QApplication.allWidgets()
                 if w.objectName() == "lengthEdit"]
        if not check("alice's Pad1 in edit, her own panel", (entered or {}).get("ok")
                     and root and len(edits) == 1, (entered, root, len(edits))):
            return
        # into the form that holds the length field
        leaf = Painted()
        form = edits[0].parentWidget()
        lay = form.layout()
        lay.addWidget(leaf)
        settle(1500)

        field = field_under(root, "paintedLeaf")
        check("1: the leaf is in alice's panel, on a page the desktop does not show",
              field and not leaf.isVisible(), (field, leaf.isVisible()))
        if not field:
            return
        pid, image = picture_of(alice, field)
        check("2: it has a picture of its size, red",
              image is not None and image.width() == leaf.width() and leaf.width() > 0
              and image.height() == leaf.height() and centre(image) == (255, 0, 0),
              (pid, image and (image.width(), image.height(), centre(image)),
               (leaf.width(), leaf.height())))

        leaf.color = QtGui.QColor(0, 0, 255)
        leaf.update()
        settle(2000)
        pid2, image2 = picture_of(alice, field)
        check("3: repainted blue, a new picture, blue",
              image2 is not None and pid2 != pid and centre(image2) == (0, 0, 255),
              (pid, pid2, image2 and centre(image2)))

        settle(2000)
        pid3, _ = picture_of(alice, field)
        check("4: nothing changed, the same picture",
              pid3 == pid2 and pid3.startswith("img:"), (pid2, pid3))
        # the look lays the hidden page out to render it: that is not a
        # change of the panel's structure, and costs no walk of it
        before = FreeCADGui.FormWidgets.panelStats()
        settle(3000)
        after = FreeCADGui.FormWidgets.panelStats()
        check("5: the polls look, and rebuild nothing",
              after["polls"] > before["polls"] and after["grabs"] > before["grabs"]
              and after["rebuilds"] == before["rebuilds"],
              {k: (before[k], after[k]) for k in ("polls", "grabs", "rebuilds")})

        does(alice, "widgets.custom", {"target": root, "content": {"event": "reject"}})
        settle(1500)
    except Exception:
        m.note("FAIL run:\n" + traceback.format_exc())
    finally:
        for u in users:
            u.jobs.put(("stop", None))
        m.finish()


QtCore.QTimer.singleShot(1500, run)
