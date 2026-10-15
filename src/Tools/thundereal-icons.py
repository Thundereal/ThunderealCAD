#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Draw the Thundereal icon set from one description of the mark.

The mark is the T-strike: a bar seen a little from below, struck through by
a two-pointed bolt. Everything that shows it is written from the geometry
here, so a change to the mark is made once and the files follow:

    src/Gui/Icons/thundereal.svg            the application icon
    src/Gui/Icons/thundereal-16.svg         the same, fitted to 16 px
    src/Gui/Icons/thundereal-mark.svg       the mark alone, for other grounds
    src/Gui/Icons/thundereal-doc*.svg       the .TStd document icon
    src/Gui/Icons/*.png, thundereal.xpm     rasters for XDG and Qt resources
    src/Main/thundereal.ico, the installer's Thundereal.ico
                                            Windows
    src/MacAppBundle/.../thundereal*.icns   macOS
    src/Gui/Stylesheets/images_*/background_thundereal*.svg, .png
                                            the empty-window watermark
    src/Gui/Icons/thundereal-package.svg    the package box, wearing the icon
    src/Gui/Renderer/wasm/thundereal-favicon.html
                                            the browser viewer's tab icon

FreeCAD's own artwork stays beside each of these under its own name, and the
build switch THUNDEREAL_IDENTITY picks one set; this script only reads
upstream's package box, to dress it.

Run it with the conda environment's Python, which has PySide6 and Pillow:

    QT_QPA_PLATFORM=offscreen .conda/run.sh python src/Tools/thundereal-icons.py

The document icon's "TStd" is set in DejaVu Sans Bold and written as outlines,
so the font is needed here and nowhere the icon is shown.
"""

import io
import os
import re
import sys

from PIL import Image
from PySide6 import QtCore, QtGui, QtSvg

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ICONS = os.path.join(ROOT, "src", "Gui", "Icons")
SHEETS = os.path.join(ROOT, "src", "Gui", "Stylesheets")
MAC = os.path.join(ROOT, "src", "MacAppBundle", "FreeCAD.app", "Contents", "Resources")

K = dict(
    tile="#1B2744",
    # The bar: its bottom face is the lit one, it catches the strike's light.
    barBottom="#9CC0FF",
    barFront="#4F86F0",
    barSide="#2552C4",
    hole="#0F1730",
    strike="#FFB21E",
    strikeSide="#C26F00",
    page="#F5F7FC",
    pageEdge="#8E9AB8",
    fold="#D6DDEC",
    ink="#16203A",
)

# All in a 64-unit square. The bar is a box whose depth runs along (5, 3): a
# front face, and the bottom and right side that a view from a little below
# shows.
BAR_FRONT = [(10, 7), (48, 7), (48, 19), (10, 19)]
BAR_BOTTOM = [(10, 19), (48, 19), (53, 22), (15, 22)]
BAR_SIDE = [(48, 7), (53, 10), (53, 22), (48, 19)]
# One strike with two points, drawn as if there were no bar; the bar is put
# over it, and the strike comes out of a hole in the bar's bottom face.
STRIKE = [(37, 0.5), (33, 33), (44, 33), (24, 59), (28, 43), (15, 43)]
STRIKE_DEPTH = (2, 1.2)
HOLE = [(25.5, 19.6), (36.2, 19.6), (39.2, 21.4), (28.5, 21.4)]
EXIT = 19.9

DESC = (
    "  <desc>Thundereal application mark: the T-strike. A bar seen a little from below,\n"
    "  struck through by a two-pointed bolt. Written by src/Tools/thundereal-icons.py.</desc>\n"
)


def poly(pts):
    p = QtGui.QPainterPath()
    p.addPolygon(QtGui.QPolygonF([QtCore.QPointF(*q) for q in pts]))
    p.closeSubpath()
    return p


def extrude_side(path, d):
    """The faces a prism shows behind its front: the back cap and each edge
    swept along d, as one outline."""
    out = path.translated(*d)
    for sub in path.toSubpathPolygons():
        pts = [(q.x(), q.y()) for q in sub]
        for a, b in zip(pts, pts[1:] + pts[:1]):
            if a != b:
                out = out.united(
                    poly([a, b, (b[0] + d[0], b[1] + d[1]), (a[0] + d[0], a[1] + d[1])])
                )
    return out.simplified()


def below(path, y):
    r = QtGui.QPainterPath()
    r.addRect(-100, y, 300, 300)
    return path.intersected(r).simplified()


def mark_layers():
    """The mark in paint order, as (name, outline, colour)."""
    s = poly(STRIKE)
    low = below(s, EXIT)
    return [
        ("strikeSide", extrude_side(s, STRIKE_DEPTH), K["strikeSide"]),
        ("strike", s, K["strike"]),
        ("barBottom", poly(BAR_BOTTOM), K["barBottom"]),
        ("barSide", poly(BAR_SIDE), K["barSide"]),
        ("barFront", poly(BAR_FRONT), K["barFront"]),
        ("hole", poly(HOLE), K["hole"]),
        ("lowSide", extrude_side(low, STRIKE_DEPTH), K["strikeSide"]),
        ("lowStrike", low, K["strike"]),
    ]


def partition(layers):
    """What of each layer is seen: the layer less everything painted over it.
    The pieces do not overlap, so they can be drawn translucent."""
    out = []
    for i, (n, p, c) in enumerate(layers):
        for _, q, _ in layers[i + 1 :]:
            p = p.subtracted(q)
        out.append((n, p.simplified(), c))
    return out


# The 16 px drawings. There the 64-unit mark is a smudge: the bar's three
# faces and the hole fall between pixels. So the mark is redrawn on the pixel
# grid -- larger than its inset on the tile, the bar's edges on whole pixels,
# the hole left out, and the bar's bottom face one lit row.
def strike16(cx, top, s):
    return [(cx + (x - 31.5) * s, top + (y - 0.5) * s) for x, y in STRIKE]


def mark16(bar, side, bottom, strike, exit_y, depth=None):
    s = poly(strike)
    low = below(s, exit_y)

    def edge(name, q):
        return [(name, extrude_side(q, depth), K["strikeSide"])] if depth else []

    return (
        edge("strikeSide", s)
        + [("strike", s, K["strike"])]
        + ([("barBottom", poly(bottom), K["barBottom"])] if bottom else [])
        + ([("barSide", poly(side), K["barSide"])] if side else [])
        + [("barFront", poly(bar), K["barFront"])]
        + edge("lowSide", low)
        + [("lowStrike", low, K["strike"])]
    )


def num(v):
    s = ("%.2f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def path_data(path, xf=None):
    if xf:
        path = xf.map(path)
    parts = []
    i, n = 0, path.elementCount()
    E = QtGui.QPainterPath.ElementType
    while i < n:
        e = path.elementAt(i)
        if e.type == E.MoveToElement:
            if parts:
                parts.append("Z")
            parts.append("M%s %s" % (num(e.x), num(e.y)))
            i += 1
        elif e.type == E.LineToElement:
            parts.append("L%s %s" % (num(e.x), num(e.y)))
            i += 1
        else:
            c1, c2 = path.elementAt(i + 1), path.elementAt(i + 2)
            parts.append(
                "C%s %s %s %s %s %s"
                % (num(e.x), num(e.y), num(c1.x), num(c1.y), num(c2.x), num(c2.y))
            )
            i += 3
    if parts:
        parts.append("Z")
    return "".join(parts)


def paths(layers, xf=None, indent="  ", fill=None, opacity=None):
    out = []
    for n, p, c in layers:
        d = path_data(p, xf)
        if not d:
            continue
        attrs = 'fill="%s"' % (fill or c)
        if opacity is not None:
            o = opacity[n] if isinstance(opacity, dict) else opacity
            if not o:
                continue
            attrs += ' fill-opacity="%s"' % o
        out.append('%s<path id="%s" %s d="%s"/>\n' % (indent, n, attrs, d))
    return "".join(out)


def svg(w, h, body, title):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" width="%s" height="%s" viewBox="0 0 %s %s">\n'
        "  <title>%s</title>\n%s</svg>\n" % (w, h, w, h, title, body)
    )


def inset(s, tx=32, ty=32):
    t = QtGui.QTransform()
    t.translate(tx, ty)
    t.scale(s, s)
    t.translate(-32, -32)
    return t


def text_path(text, px, cx, baseline):
    f = QtGui.QFont("DejaVu Sans")
    f.setBold(True)
    f.setPixelSize(100)
    if not QtGui.QFontInfo(f).family().startswith("DejaVu Sans"):
        sys.exit("DejaVu Sans Bold is needed for the document icon's label")
    p = QtGui.QPainterPath()
    p.addText(0, 0, f, text)
    s = px / 100.0
    t = QtGui.QTransform()
    t.translate(cx - p.boundingRect().center().x() * s, baseline)
    t.scale(s, s)
    return t.map(p)


def write(path, text):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(text)
    print("wrote", os.path.relpath(path, ROOT))


PAGE = (
    '  <path d="M13 3H40L53 16V61H13Z" fill="%s" stroke="%s" stroke-width="1.5"'
    ' stroke-linejoin="round"/>\n'
    '  <path d="M40 3V16H53" fill="%s" stroke="%s" stroke-width="1.5" stroke-linejoin="round"/>\n'
    % (K["page"], K["pageEdge"], K["fold"], K["pageEdge"])
)
PAGE16 = (
    '  <path d="M3.5 0.5H10L13.5 4V15.5H3.5Z" fill="%s" stroke="%s" stroke-linejoin="round"/>\n'
    '  <path d="M10 0.5V4H13.5" fill="%s" stroke="%s" stroke-linejoin="round"/>\n'
    % (K["page"], K["pageEdge"], K["fold"], K["pageEdge"])
)


def write_svgs():
    tile = '  <rect width="64" height="64" rx="14" fill="%s"/>\n' % K["tile"]
    write(
        os.path.join(ICONS, "thundereal.svg"),
        svg(64, 64, DESC + tile + paths(mark_layers(), inset(0.74)), "Thundereal"),
    )
    write(
        os.path.join(ICONS, "thundereal-mark.svg"),
        svg(64, 64, DESC + paths(mark_layers()), "Thundereal mark"),
    )
    app16 = mark16(
        [(3, 3), (11, 3), (11, 5), (3, 5)],
        [(11, 3), (12, 3.6), (12, 6), (11, 5)],
        [(3, 5), (11, 5), (12, 6), (4, 6)],
        strike16(8, 1.5, 13 / 58.5),
        6,
    )
    write(
        os.path.join(ICONS, "thundereal-16.svg"),
        svg(
            16,
            16,
            '  <rect width="16" height="16" rx="3.5" fill="%s"/>\n' % K["tile"] + paths(app16),
            "Thundereal",
        ),
    )

    # The .TStd document: a page with its corner folded, the mark, and a band
    # naming the extension, which reads from 48 px up. Below that the band is
    # a smudge, so the smaller drawings carry the page and a larger mark only.
    title = "Thundereal document (.TStd)"
    band = (
        '  <rect x="13" y="45" width="40" height="12" fill="%s"/>\n' % K["ink"]
        + '  <path fill="#FFFFFF" d="%s"/>\n' % path_data(text_path("TStd", 8.5, 33, 54))
    )
    write(
        os.path.join(ICONS, "thundereal-doc.svg"),
        svg(64, 64, PAGE + paths(mark_layers(), inset(0.42, 33, 27)) + band, title),
    )
    write(
        os.path.join(ICONS, "thundereal-doc-small.svg"),
        svg(64, 64, PAGE + paths(mark_layers(), inset(0.56, 33, 37)), title),
    )
    doc16 = mark16(
        [(5, 5), (11, 5), (11, 7), (5, 7)],
        [(11, 5), (12, 5.6), (12, 7.6), (11, 7)],
        None,
        strike16(8.2, 3.5, 10 / 58.5),
        7,
        # On the white page the strike needs its dark side to hold.
        (0.8, 0.5),
    )
    write(os.path.join(ICONS, "thundereal-doc-16.svg"), svg(16, 16, PAGE16 + paths(doc16), title))

    # The empty-window watermark, in the old one's 200 x 187 frame: in colour
    # for the light theme, and as a black or white silhouette for the others.
    # The pieces are cut apart so translucent faces do not darken where they
    # would overlap.
    parts = partition(mark_layers())
    wm = inset(2.6, 100, 93.5)
    shade = {
        "strike": 0.12,
        "lowStrike": 0.12,
        "barFront": 0.12,
        "barBottom": 0.06,
        "barSide": 0.09,
        "strikeSide": 0.09,
        "lowSide": 0.09,
        "hole": 0,
    }
    for folder in ("images_dark-light", "images_classic"):
        d = os.path.join(SHEETS, folder)
        write(
            os.path.join(d, "background_thundereal.svg"),
            svg(200, 187, paths(parts, wm, opacity=0.22), "Thundereal"),
        )
        for name, ink in (("dark", "#000000"), ("light", "#FFFFFF")):
            write(
                os.path.join(d, "background_thundereal_%s.svg" % name),
                svg(200, 187, paths(parts, wm, fill=ink, opacity=shade), "Thundereal"),
            )

    # The package box wears the application icon as its label: the tile
    # keeps the mark apart from the box's own amber. The label group sits in
    # a 64-unit frame its parents shear onto the box's front face.
    label = re.findall(r"<(?:path|rect) [^>]*/>", tile + paths(mark_layers(), inset(0.74)))
    pad = "\n" + " " * 12
    group = "<g" + pad + '   id="thundereal-mark">' + pad + "  "
    group += (pad + "  ").join(label) + pad + "</g>"
    # Upstream's box, with its logo group swapped for the label.
    with open(os.path.join(ICONS, "document-package.svg"), encoding="utf-8", newline="") as f:
        text = replace_group(f.read(), "g3813-3", group)
    if not text:
        sys.exit("no logo group in document-package.svg")
    p = os.path.join(ICONS, "thundereal-package.svg")
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("wrote", os.path.relpath(p, ROOT))


def replace_group(text, gid, new):
    m = re.search(r'<g\b[^>]*\bid="%s"[^>]*>' % re.escape(gid), text)
    if not m:
        return None
    depth, pos = 1, m.end()
    for t in re.finditer(r"<g\b|</g>", text[pos:]):
        depth += 1 if t.group(0) == "<g" else -1
        if depth == 0:
            return text[: m.start()] + new + text[pos + t.end() :]
    return None


app = None


def render(name, size, scale=1.0):
    """The SVG at size x size px, the drawing scaled about the centre."""
    r = QtSvg.QSvgRenderer(os.path.join(ICONS, name))
    img = QtGui.QImage(size, size, QtGui.QImage.Format_ARGB32)
    img.fill(0)
    p = QtGui.QPainter(img)
    p.setRenderHint(QtGui.QPainter.Antialiasing)
    w = size * scale
    r.render(p, QtCore.QRectF((size - w) / 2, (size - w) / 2, w, w))
    p.end()
    buf = QtCore.QBuffer()
    buf.open(QtCore.QIODevice.WriteOnly)
    img.save(buf, "PNG")
    return Image.open(io.BytesIO(bytes(buf.data()))).convert("RGBA")


def app_image(size, scale=1.0):
    return render("thundereal-16.svg" if size == 16 else "thundereal.svg", size, scale)


def doc_image(size):
    if size == 16:
        return render("thundereal-doc-16.svg", 16)
    return render("thundereal-doc.svg" if size >= 48 else "thundereal-doc-small.svg", size)


def save_png(img, path):
    img.save(path, optimize=True)
    print("wrote", os.path.relpath(path, ROOT))


def save_multi(path, fmt, images):
    big = images[-1]
    big.save(path, fmt, sizes=[i.size for i in images], append_images=images[:-1])
    print("wrote", os.path.relpath(path, ROOT))


# The ICNS entries, each a PNG, by pixel size and scale. Pillow's writer leaves
# out the 1x 16 and 32 px entries, so a 1x screen would shrink the 32 px
# drawing in place of the one fitted to 16 px.
ICNS_TYPES = [
    (b"icp4", 16, 1),
    (b"icp5", 32, 1),
    (b"icp6", 64, 1),
    (b"ic07", 128, 1),
    (b"ic08", 256, 1),
    (b"ic09", 512, 1),
    (b"ic11", 16, 2),
    (b"ic12", 32, 2),
    (b"ic13", 128, 2),
    (b"ic14", 256, 2),
    (b"ic10", 512, 2),
]


def save_icns(path, image_of):
    chunks = b""
    for tag, size, scale in ICNS_TYPES:
        buf = io.BytesIO()
        image_of(size * scale, scale).save(buf, "PNG", optimize=True)
        data = buf.getvalue()
        chunks += tag + (len(data) + 8).to_bytes(4, "big") + data
    with open(path, "wb") as f:
        f.write(b"icns" + (len(chunks) + 8).to_bytes(4, "big") + chunks)
    print("wrote", os.path.relpath(path, ROOT))


def save_xpm(img, path, name):
    """XPM keeps one transparent colour: a pixel is in or out."""
    px = img.load()
    w, h = img.size
    colours, rows = {}, []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = px[x, y]
            key = None if a < 128 else "#%02X%02X%02X" % (r, g, b)
            row.append(colours.setdefault(key, len(colours)))
        rows.append(row)
    chars = "".join(chr(c) for c in range(35, 127) if chr(c) not in '"\\')
    code = lambda i: chars[i // len(chars)] + chars[i % len(chars)]
    keys = sorted(colours, key=colours.get)
    lines = ['"%d %d %d 2",' % (w, h, len(keys))]
    lines += ['"%s\tc %s",' % (code(colours[k]), k or "None") for k in keys]
    lines += ['"%s",' % "".join(code(i) for i in row) for row in rows]
    lines[-1] = lines[-1].rstrip(",")
    write(path, "/* XPM */\nstatic char * %s_xpm[] = {\n%s};\n" % (name, "\n".join(lines)))


def write_rasters():
    for s in (16, 32, 48, 64):
        save_png(app_image(s), os.path.join(ICONS, "thundereal-icon-%d.png" % s))
    for s in (16, 24, 32):
        save_png(doc_image(s), os.path.join(ICONS, "thundereal-doc-%d.png" % s))
    save_png(doc_image(64), os.path.join(ICONS, "thundereal-doc.png"))
    save_xpm(app_image(32), os.path.join(ICONS, "thundereal.xpm"), "thundereal")

    ico = [app_image(s) for s in (16, 24, 32, 48, 64, 128, 256)]
    save_multi(os.path.join(ROOT, "src", "Main", "thundereal.ico"), "ICO", ico)
    save_multi(
        os.path.join(ROOT, "tools", "build", "WindowsInstaller", "icons", "Thundereal.ico"),
        "ICO",
        ico,
    )

    # macOS draws an application icon inside the grid's 824 of 1024 units;
    # a tile to the edge stands larger than every other icon in the Dock.
    # The 16 px drawing is fitted to the pixel grid and keeps its full tile:
    # shrunk into the grid it would land between pixels again.
    def mac_app(px, scale):
        return app_image(16) if px == 16 and scale == 1 else app_image(px, 824 / 1024)

    save_icns(os.path.join(MAC, "thundereal.icns"), mac_app)
    # A @2x entry takes the drawing for the size it stands for: 16 pt at 32 px
    # is the page without its band.
    def mac_doc(px, scale):
        if px == 16:
            return render("thundereal-doc-16.svg", 16)
        return render("thundereal-doc.svg" if px // scale >= 48 else "thundereal-doc-small.svg", px)

    save_icns(os.path.join(MAC, "thundereal-doc.icns"), mac_doc)

    for folder in ("images_classic",):
        for variant in ("", "_dark", "_light"):
            name = "background_thundereal" + variant
            r = QtSvg.QSvgRenderer(os.path.join(SHEETS, folder, name + ".svg"))
            img = QtGui.QImage(320, 299, QtGui.QImage.Format_ARGB32)
            img.fill(0)
            p = QtGui.QPainter(img)
            p.setRenderHint(QtGui.QPainter.Antialiasing)
            r.render(p)
            p.end()
            out = os.path.join(SHEETS, folder, name + ".png")
            img.save(out)
            print("wrote", os.path.relpath(out, ROOT))


def write_favicon():
    """The browser viewer's tab icon, inline in its page: the page is served
    alone, with no folder of assets beside it. The viewer's build writes the
    link into its shell when THUNDEREAL_IDENTITY is on."""
    out = os.path.join(ROOT, "src", "Gui", "Renderer", "wasm", "thundereal-favicon.html")
    with open(os.path.join(ICONS, "thundereal.svg"), encoding="ascii") as f:
        body = f.read()
    drop = r"<\?xml[^>]*>\s*|<title>.*?</title>\s*|<desc>.*?</desc>\s*"
    body = re.sub(drop, "", body, flags=re.S)
    body = re.sub(r' id="[^"]*"', "", re.sub(r"\s*\n\s*", "", body)).replace('"', "'")
    data = body.replace("%", "%25").replace("#", "%23").replace("<", "%3C").replace(">", "%3E")
    link = '<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%s">' % data
    write(out, link + "\n")


if __name__ == "__main__":
    app = QtGui.QGuiApplication(sys.argv)
    write_svgs()
    write_rasters()
    write_favicon()
