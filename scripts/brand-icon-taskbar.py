#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The application icon in a mock Windows 11 task bar.

How the icon sits beside other programs' -- which mostly fill their square --
at the sizes the task bar draws it: 24 px (100% display scale) and 32 px
(125%), on the light and the dark task bar, between generic full-square
placeholders (no real brands), and large on white and dark grey. Each
revision given is a row, so a change to the icon can be judged where it is
seen:

    QT_QPA_PLATFORM=offscreen .conda/run.sh python scripts/brand-icon-taskbar.py out.html
    ... --rev HEAD~1 --rev HEAD --rev worktree out.html

The default is the working tree only. The icon is src/Gui/Icons/thundereal.svg
(and thundereal-16.svg for nothing here: the task bar never draws 16 px).
"""

import argparse
import base64
import html
import os
import subprocess
import sys

from PySide6 import QtCore, QtGui, QtSvg

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ICON = "src/Gui/Icons/thundereal.svg"


def read(rev):
    if rev in (None, "worktree"):
        with open(os.path.join(ROOT, ICON), "rb") as f:
            return f.read()
    return subprocess.run(["git", "-C", ROOT, "show", "%s:%s" % (rev, ICON)], capture_output=True, check=True).stdout


def render(data, size):
    r = QtSvg.QSvgRenderer(QtCore.QByteArray(data))
    img = QtGui.QImage(size, size, QtGui.QImage.Format_ARGB32_Premultiplied)
    img.fill(0)
    p = QtGui.QPainter(img)
    p.setRenderHint(QtGui.QPainter.Antialiasing)
    r.render(p, QtCore.QRectF(0, 0, size, size))
    p.end()
    buf = QtCore.QBuffer()
    buf.open(QtCore.QIODevice.WriteOnly)
    img.save(buf, "PNG")
    return "data:image/png;base64," + base64.b64encode(bytes(buf.data())).decode()


def taskbar(data, size, theme):
    nb = lambda cls: '<span class="nb %s" style="width:%dpx;height:%dpx"></span>' % (cls, size, size)
    ours = '<img src="%s" width="%d" height="%d" alt="">' % (render(data, size), size, size)
    return '<div class="tb %s" style="height:%dpx">%s%s%s%s%s</div>' % (
        theme, size * 2, nb("sq a"), nb("circ b"), ours, nb("sq c"), nb("sq d"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", help="the HTML file to write")
    ap.add_argument("--rev", action="append", help="a revision to show, or 'worktree' (repeatable)")
    a = ap.parse_args()
    app = QtGui.QGuiApplication(sys.argv)
    rows = []
    for rev in a.rev or ["worktree"]:
        data = read(rev)
        bars = "".join(taskbar(data, s, t) for s in (24, 32) for t in ("light", "dark"))
        big = render(data, 192)
        rows.append('<section class="item"><h2>%s</h2><div class="bars">%s</div><div class="large">'
                    '<div class="g light"><img src="%s" width="192" height="192" alt=""></div>'
                    '<div class="g dark"><img src="%s" width="192" height="192" alt=""></div></div></section>'
                    % (html.escape(rev), bars, big, big))
    with open(a.out, "w", encoding="ascii") as f:
        f.write(PAGE.replace("@@ROWS@@", "".join(rows)))
    print("wrote", a.out)


PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Thundereal icon in the task bar</title>
<style>
body { margin: 0; background: #f4f6fb; color: #17203a; font: 15px/1.5 "Segoe UI", system-ui, sans-serif; padding: 24px 16px 48px; }
main { max-width: 1100px; margin: 0 auto; display: grid; gap: 16px; }
.item { background: #fff; border: 1px solid #d9dfec; border-radius: 10px; padding: 14px; display: grid; gap: 12px; }
h2 { font: 600 15px ui-monospace, monospace; color: #2552c4; margin: 0; }
.bars { display: grid; grid-template-columns: repeat(auto-fill, minmax(240px, 1fr)); gap: 8px; }
.tb { display: flex; align-items: center; justify-content: center; gap: 14px; border-radius: 6px; }
.tb.light { background: #eeeeee; } .tb.dark { background: #202020; } .tb img, .nb { display: block; flex: none; }
.nb.sq { border-radius: 22%; } .nb.circ { border-radius: 50%; }
.nb.a { background: linear-gradient(135deg, #3b82f6, #1d4ed8); } .nb.b { background: radial-gradient(circle at 35% 35%, #fbbf24, #ea580c); }
.nb.c { background: linear-gradient(160deg, #22c55e, #15803d); } .nb.d { background: linear-gradient(135deg, #a855f7, #6d28d9); }
.large { display: flex; gap: 10px; flex-wrap: wrap; } .g { border-radius: 8px; padding: 6px; line-height: 0; }
.g.light { background: #fff; border: 1px solid #d9dfec; } .g.dark { background: #202020; }
</style></head><body><main><h1 style="font-size:24px;margin:0">Thundereal icon in the task bar</h1>@@ROWS@@</main></body></html>
"""

if __name__ == "__main__":
    main()
