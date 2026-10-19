"""The application icon on light and dark grounds (entry 175).

x16's tile-less icon (thundereal/main 094770130e): one drawing meant to read
on both themes. This draws, from the running program, the window icon (what
the task bar shows for the running window), Gui.getIcon("freecad") and the
title-bar logo button, each at 16, 24, 32 and 48 px, on white, Windows'
light grey, Windows' dark grey and black, into icons.png for a look:
  - the window icon is set and not null at every size;
  - it is the same picture as Gui.getIcon("freecad") at 32 px;
  - its corners are clear (no ground of its own): the alpha at the four
    corners of the 48 px pixmap is 0;
  - the title-bar logo button, rendered by itself, is saved beside them.
"""
import os
import traceback

import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets

OUT = os.environ["GT_OUT"]
RESULT = os.environ.get("GT_RESULT", os.path.join(OUT, "result.txt"))
GROUNDS = [("white", "#ffffff"), ("light", "#f3f3f3"), ("dark", "#202020"), ("black", "#000000")]
SIZES = [16, 24, 32, 48]


def note(msg):
    with open(RESULT, "a", encoding="utf-8") as f:
        f.write(str(msg) + "\n")


def check(name, cond, detail=""):
    note(("PASS " if cond else "FAIL ") + name + (" | " + str(detail) if detail else ""))
    return cond


def run():
    try:
        app = QtWidgets.QApplication.instance()
        win = app.windowIcon()
        mine = FreeCADGui.getIcon("freecad")
        check("the window icon is set", not win.isNull())
        sizes_ok = all(not win.pixmap(s, s).isNull() for s in SIZES)
        check("... a pixmap at 16, 24, 32 and 48 px", sizes_ok)
        a = win.pixmap(32, 32).toImage().convertToFormat(QtGui.QImage.Format_ARGB32)
        b = mine.pixmap(32, 32).toImage().convertToFormat(QtGui.QImage.Format_ARGB32)
        same = a.size() == b.size() and sum(
            1 for y in range(a.height()) for x in range(a.width()) if a.pixel(x, y) != b.pixel(x, y)
        ) <= a.width() * a.height() // 50
        check("the window icon is Gui.getIcon('freecad') at 32 px", same)
        big = win.pixmap(48, 48).toImage().convertToFormat(QtGui.QImage.Format_ARGB32)
        w, h = big.width(), big.height()
        corners = [QtGui.QColor.fromRgba(big.pixel(x, y)).alpha()
                   for x, y in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))]
        check("no ground of its own: the 48 px corners are clear", max(corners) == 0, corners)

        # the sheet: a row per ground, the four sizes side by side
        cell = 64
        sheet = QtGui.QImage(cell * len(SIZES), cell * len(GROUNDS), QtGui.QImage.Format_ARGB32)
        p = QtGui.QPainter(sheet)
        for row, (_, colour) in enumerate(GROUNDS):
            p.fillRect(0, row * cell, sheet.width(), cell, QtGui.QColor(colour))
            for col, s in enumerate(SIZES):
                pm = win.pixmap(s, s)
                p.drawPixmap(col * cell + (cell - s) // 2, row * cell + (cell - s) // 2, pm)
        p.end()
        sheet.save(os.path.join(OUT, "icons.png"))
        note("INFO rows: %s; columns: %s px" % ([g for g, _ in GROUNDS], SIZES))

        btn = FreeCADGui.getMainWindow().findChild(QtWidgets.QWidget, "titleBarLogo")
        if btn is not None and btn.width() > 0:
            for name, colour in (("light", "#f3f3f3"), ("dark", "#202020")):
                img = QtGui.QImage(btn.size(), QtGui.QImage.Format_ARGB32)
                img.fill(QtGui.QColor(colour))
                btn.render(img, QtCore.QPoint(), QtGui.QRegion(),
                           QtWidgets.QWidget.RenderFlag.DrawChildren)
                img.scaled(img.width() * 3, img.height() * 3).save(
                    os.path.join(OUT, "titlebar-%s.png" % name))
            note("INFO title-bar button rendered, %dx%d" % (btn.width(), btn.height()))
        else:
            note("INFO no title-bar logo button (title bar off)")
    except Exception:
        note("ABORT " + traceback.format_exc().replace("\n", " | "))
    finally:
        note("DONE")
        QtCore.QTimer.singleShot(300, FreeCADGui.getMainWindow().close)


QtCore.QTimer.singleShot(3000, run)
