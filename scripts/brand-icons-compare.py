#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Every drawing of the Thundereal mark, at one revision beside another.

The pictures src/Tools/thundereal-icons.py writes that show the mark -- the
application icon and its small sizes, the bare mark, the document and package
icons, the watermark, the TechDraw logo, the splash and the About picture --
each on the ground it sits on, rendered by Qt as the program draws them, the
base revision on the left and the other on the right. For reviewing a change
to the artwork before it is committed:

    QT_QPA_PLATFORM=offscreen .conda/run.sh python scripts/brand-icons-compare.py out.html
    ... --base HEAD~1 --other HEAD out.html

--base defaults to HEAD, --other to the working tree. The page is one
self-contained HTML file (the pictures are inline).
"""

import argparse
import base64
import html
import os
import subprocess
import sys

from PySide6 import QtCore, QtGui, QtSvg

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
I = "src/Gui/Icons/"
SH = "src/Gui/Stylesheets/images_dark-light/"

# (title, where it is used, path, width, height, grounds, zoom or "sizes")
ITEMS = [
    ("Application icon", "The window and task bar, the title bar's logo, Help > About, the .exe, installer, Dock and desktop icons, the browser tab",
     I + "thundereal.svg", 128, 128, ["#FFFFFF", "#EEEEEE", "#202020"], 1),
    ("Application icon, task bar sizes", "24 px at 100% display scale, 32 px at 125%, 48 px at 200%",
     I + "thundereal.svg", None, None, ["#EEEEEE", "#202020"], "sizes"),
    ("Application icon, 16 px", "Menus, the window's corner, file lists (drawn on the pixel grid)",
     I + "thundereal-16.svg", 16, 16, ["#EEEEEE", "#202020"], 6),
    ("The mark alone", "thundereal-mark.svg: the README, the GitHub organisation page",
     I + "thundereal-mark.svg", 128, 128, ["#FFFFFF", "#0D1117"], 1),
    ("Document icon", "The .TStd file's icon, 48 px and up", I + "thundereal-doc.svg", 128, 128, ["#FFFFFF", "#202020"], 1),
    ("Document icon, small", "24 and 32 px", I + "thundereal-doc-small.svg", 32, 32, ["#FFFFFF", "#202020"], 3),
    ("Document icon, 16 px", "File lists, the recent-files menu", I + "thundereal-doc-16.svg", 16, 16, ["#FFFFFF", "#202020"], 6),
    ("Package box", "Add-on packages, the workbench preferences", I + "thundereal-package.svg", 128, 128, ["#FFFFFF", "#2B2B2B"], 1),
    ("Watermark, light theme", "The empty main window", SH + "background_thundereal.svg", 200, 187, ["#F0F0F0"], 1),
    ("Watermark, dark theme", "The empty main window", SH + "background_thundereal_light.svg", 200, 187, ["#333333"], 1),
    ("TechDraw logo", "The title block of the drawing templates", "src/Mod/TechDraw/Branding/thundereal-logo.svg", 160, 150, ["#FFFFFF"], 1),
    ("Splash", "At start-up", I + "thunderealsplash.svg", 400, 245, [None], 1),
    ("About picture", "Help > About", I + "thunderealabout.svg", 400, 245, [None], 1),
]


def read(rev, path):
    if rev is None:
        with open(os.path.join(ROOT, path), "rb") as f:
            return f.read()
    return subprocess.run(["git", "-C", ROOT, "show", "%s:%s" % (rev, path)],
                          capture_output=True, check=True).stdout


def render(data, w, h, ground, zoom=1):
    r = QtSvg.QSvgRenderer(QtCore.QByteArray(data))
    img = QtGui.QImage(w, h, QtGui.QImage.Format_ARGB32)
    img.fill(QtGui.QColor(ground) if ground else QtGui.QColor(0, 0, 0, 0))
    p = QtGui.QPainter(img)
    p.setRenderHint(QtGui.QPainter.Antialiasing)
    r.render(p, QtCore.QRectF(0, 0, w, h))
    p.end()
    if zoom != 1:
        img = img.scaled(w * zoom, h * zoom, QtCore.Qt.IgnoreAspectRatio, QtCore.Qt.FastTransformation)
    buf = QtCore.QBuffer()
    buf.open(QtCore.QIODevice.WriteOnly)
    img.save(buf, "PNG")
    return "data:image/png;base64," + base64.b64encode(bytes(buf.data())).decode()


def cells(data, w, h, grounds, zoom):
    if zoom == "sizes":
        return "".join(
            '<div class="ground sizes" style="background:%s">%s</div>'
            % (g, "".join('<img src="%s" width="%d" height="%d" alt="">' % (render(data, s, s, g), s, s) for s in (24, 32, 48)))
            for g in grounds)
    return "".join(
        '<div class="ground" style="background:%s"><img src="%s" width="%d" height="%d" alt=""%s></div>'
        % (g or "transparent", render(data, w, h, g, zoom), w * zoom, h * zoom, ' class="px"' if zoom != 1 else "")
        for g in grounds)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", help="the HTML file to write")
    ap.add_argument("--base", default="HEAD", help="the revision on the left (default HEAD)")
    ap.add_argument("--other", default=None, help="the revision on the right (default: the working tree)")
    a = ap.parse_args()
    app = QtGui.QGuiApplication(sys.argv)
    left, right = a.base, a.other or "working tree"
    rows = []
    for title, use, path, w, h, grounds, zoom in ITEMS:
        try:
            lb, rb = read(a.base, path), read(a.other, path)
        except (subprocess.CalledProcessError, FileNotFoundError):
            continue
        same = " (the same)" if lb == rb else ""
        rows.append(
            '<section class="item"><h2>%s%s</h2><p>%s</p><div class="pair">'
            '<figure><figcaption>%s</figcaption><div class="grounds">%s</div></figure>'
            '<figure><figcaption>%s</figcaption><div class="grounds">%s</div></figure></div></section>'
            % (html.escape(title), same, html.escape(use), html.escape(left), cells(lb, w, h, grounds, zoom),
               html.escape(right), cells(rb, w, h, grounds, zoom)))
    with open(a.out, "w", encoding="ascii") as f:
        f.write(PAGE.replace("@@TITLE@@", html.escape("%s beside %s" % (left, right))).replace("@@ROWS@@", "".join(rows)))
    print("wrote", a.out)


PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Thundereal artwork: @@TITLE@@</title>
<style>
body { margin: 0; background: #f4f6fb; color: #17203a; font: 15px/1.5 "Segoe UI", system-ui, sans-serif; padding: 24px 16px 48px; }
main { max-width: 1180px; margin: 0 auto; display: grid; gap: 16px; }
h1 { font-size: 24px; margin: 0; }
.item { background: #fff; border: 1px solid #d9dfec; border-radius: 10px; padding: 14px; }
h2 { font-size: 16px; margin: 0; } p { margin: 2px 0 10px; color: #5a6683; font-size: 14px; }
.pair { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 14px; }
figure { margin: 0; min-width: 0; } figcaption { font: 600 12px ui-monospace, monospace; color: #2552c4; margin-bottom: 6px; }
.grounds { display: flex; flex-wrap: wrap; gap: 8px; }
.ground { border-radius: 8px; padding: 8px; line-height: 0; border: 1px solid #d9dfec; max-width: 100%; }
.ground img { max-width: 100%; height: auto; } .ground.sizes { display: flex; align-items: flex-end; gap: 14px; }
.px { image-rendering: pixelated; }
</style></head><body><main><h1>Thundereal artwork: @@TITLE@@</h1>@@ROWS@@</main></body></html>
"""

if __name__ == "__main__":
    main()
