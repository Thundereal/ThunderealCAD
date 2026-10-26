#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Writes the 3D sketch's icons (docs/Sketch3D.md section 7) next to this script.

The 2D sketch's square and circle laid flat in isometric view, in the teal a 3D
sketch's tools are drawn in; the Sketcher's own tools are recoloured to it while
a 3D sketch is in edit (SketcherGui::setSketch3DToolIcons). Axes run as FreeCAD's
isometric view shows them: X down-right, Y up-right, Z up, in the manipulator's
red, green and blue.

Run it after changing a shape here; the SVGs it writes are what is committed:

    python3 make_sketch3d_icons.py
"""
import math
import os

X = (0.866, 0.5)
Y = (0.866, -0.5)
Z = (0.0, -1.0)
AXIS = {"X": "#cc0000", "Y": "#4e9a06", "Z": "#204a87"}
# geometry: the 2D sketch icon's strokes (#a40000 under #ef2929), in teal
DARK, LIGHT = "#00838f", "#4dd0e1"
# constraints: the 2D constraint bars (#280000 / #cc0000 / #ef2929), in teal
C_OUT, C_MID, C_HI = "#00292d", "#00acc1", "#4dd0e1"


def add(p, d, s=1.0):
    return (p[0] + d[0] * s, p[1] + d[1] * s)


def pts(ps):
    return " ".join("%.2f,%.2f" % p for p in ps)


def path(ps, close=True):
    return "M " + " L ".join("%.2f,%.2f" % p for p in ps) + (" Z" if close else "")


def unit(a, b):
    d = (b[0] - a[0], b[1] - a[1])
    n = math.hypot(*d)
    return (d[0] / n, d[1] / n)


def stroke(c, w):
    return (
        'fill="none" stroke="%s" stroke-width="%g" stroke-linejoin="round" stroke-linecap="round"'
        % (
            c,
            w,
        )
    )


def line(ps, c, w, closed=False):
    tag = "polygon" if closed else "polyline"
    return '<%s points="%s" %s/>' % (tag, pts(ps), stroke(c, w))


def rhombus(cx, cy, s):
    """A square of side s lying flat, centred at (cx, cy)."""
    a, b = 0.866 * s, 0.5 * s
    return [(cx - a, cy), (cx, cy - b), (cx + a, cy), (cx, cy + b)]


def flat_circle(cx, cy, r, st):
    """A circle of radius r lying flat."""
    return '<ellipse cx="%.2f" cy="%.2f" rx="%.2f" ry="%.2f" %s/>' % (
        cx,
        cy,
        r * 1.2247,
        r * 0.7071,
        st,
    )


def square_and_circle(sq, el):
    """The square whole, then the circle whole: its outline runs over the square."""
    out = []
    for c, w in ((DARK, 6), (LIGHT, 2)):
        out.append('<path d="%s" %s/>' % (path(rhombus(*sq)), stroke(c, w)))
    for c, w in ((DARK, 6), (LIGHT, 2)):
        out.append(flat_circle(*el, stroke(c, w)))
    return out


def centred(sq, el):
    """Shifts the square and circle so their outline box is centred in the icon."""
    xs = [p[0] for p in rhombus(*sq)] + [el[0] - el[2] * 1.2247, el[0] + el[2] * 1.2247]
    ys = [p[1] for p in rhombus(*sq)] + [el[1] - el[2] * 0.7071, el[1] + el[2] * 0.7071]
    dx = 32 - (min(xs) + max(xs)) / 2
    dy = 32 - (min(ys) + max(ys)) / 2
    return (sq[0] + dx, sq[1] + dy, sq[2]), (el[0] + dx, el[1] + dy, el[2]), dx, dy


def clip_polygon(ps, lo=0.0, hi=64.0):
    """Sutherland-Hodgman against the icon's box: Qt's SVG renderer has no clipPath."""

    def clip(ps, inside, cut):
        out = []
        for i, cur in enumerate(ps):
            prev = ps[i - 1]
            if inside(cur):
                if not inside(prev):
                    out.append(cut(prev, cur))
                out.append(cur)
            elif inside(prev):
                out.append(cut(prev, cur))
        return out

    def at_x(x):
        return lambda a, b: (x, a[1] + (b[1] - a[1]) * (x - a[0]) / (b[0] - a[0]))

    def at_y(y):
        return lambda a, b: (a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]), y)

    ps = clip(ps, lambda p: p[0] >= lo, at_x(lo))
    ps = clip(ps, lambda p: p[0] <= hi, at_x(hi))
    ps = clip(ps, lambda p: p[1] >= lo, at_y(lo))
    ps = clip(ps, lambda p: p[1] <= hi, at_y(hi))
    return ps


def clip_segment(a, b, lo=0.0, hi=64.0):
    t0, t1 = 0.0, 1.0
    for p, d in ((a[0], b[0] - a[0]), (a[1], b[1] - a[1])):
        for q, r in ((-d, p - lo), (d, hi - p)):
            if q == 0:
                if r < 0:
                    return None
                continue
            t = r / q
            if q < 0:
                t0 = max(t0, t)
            else:
                t1 = min(t1, t)
    if t0 > t1:
        return None
    return [add(a, (b[0] - a[0], b[1] - a[1]), t0), add(a, (b[0] - a[0], b[1] - a[1]), t1)]


def bar(a, b):
    """A constraint bar, as the 2D Horizontal one looks."""
    return [line([a, b], C_OUT, 11), line([a, b], C_MID, 7), line([a, b], C_HI, 2.5)]


def head(tip, d, size=8):
    n = (-d[1], d[0])
    base = add(tip, d, -size)
    return (
        '<polygon points="%s" fill="%s" stroke="%s" stroke-width="2" stroke-linejoin="round"/>'
        % (pts([tip, add(base, n, size * 0.62), add(base, n, -size * 0.62)]), C_HI, C_OUT)
    )


def axes(o, s, bold=None, w=2.5):
    """The axis cross; the bold axis is the one meant."""
    out = []
    for k, d in (("X", X), ("Y", Y), ("Z", Z)):
        e = add(o, d, s)
        out.append(
            '<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="%s" stroke-width="%g" '
            'stroke-linecap="round"/>'
            % (o[0], o[1], e[0], e[1], AXIS[k], w * 1.8 if k == bold else w)
        )
    return out


def quad(o, a, b, sa, sb):
    p1 = add(o, a, sa)
    return [o, p1, add(p1, b, sb), add(o, b, sb)]


def face(ps, fill, opacity, edge, w=2):
    return (
        '<polygon points="%s" fill="%s" fill-opacity="%g" stroke="%s" stroke-width="%g" '
        'stroke-linejoin="round"/>' % (pts(ps), fill, opacity, edge, w)
    )


def ring(c, a, b, r, t0=0.0, t1=2 * math.pi, n=48):
    """A circle of radius r in the plane of unit axes a and b, as points."""
    return [
        add(add(c, a, r * math.cos(t)), b, r * math.sin(t))
        for t in [t0 + (t1 - t0) * i / n for i in range(n + 1)]
    ]


def arrow_arc(arc, at_end=True):
    tip, before = (arc[-1], arc[-2]) if at_end else (arc[0], arc[1])
    return [line(arc, C_OUT, 6), line(arc, C_HI, 2.5), head(tip, unit(before, tip))]


def padlock(x, y):
    return [
        '<path d="M%.1f,%.1f v-5 a4.5,4.5 0 0 1 9,0 v5" fill="none" stroke="%s" stroke-width="3.2"/>'
        % (x + 3.5, y, C_OUT),
        '<rect x="%.1f" y="%.1f" width="16" height="13" rx="1.5" fill="%s" stroke="%s" '
        'stroke-width="2"/>' % (x, y, C_MID, C_OUT),
        '<rect x="%.1f" y="%.1f" width="10" height="2" fill="%s"/>' % (x + 3, y + 4, C_HI),
    ]


ICONS = {}

# The 3D sketch, in the tree: the circle floating over the square's far corner
sq, el, _, _ = centred((30, 42, 31), (39, 27, 18))
ICONS["Sketcher_Sketch3D"] = ("3D sketch", square_and_circle(sq, el))

# New 3D Sketch: the square on a flat sheet that runs past the icon's left and
# bottom edges, the circle floating over it
sq, el, dx, dy = centred((24, 42, 26), (36, 26.5, 19))
L, T, R, B = rhombus(20 + dx, 45 + dy, 50)
far = (-0.866 * 100, 0.5 * 100)
L, B = add(T, far), add(R, far)
sheet = [
    '<defs><linearGradient id="sheet" x1="0" y1="64" x2="0" y2="%.2f" '
    'gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#e2e4df"/>'
    '<stop offset="1" stop-color="#ffffff"/></linearGradient></defs>' % T[1],
    '<path d="%s" fill="url(#sheet)" stroke="none"/>' % path(clip_polygon([L, T, R, B])),
]
for a, b in ((L, T), (T, R), (R, B)):
    seg = clip_segment(a, b)
    if seg:
        sheet.append('<path d="%s" %s/>' % (path(seg, False), stroke("#3f3f3f", 2)))
ICONS["Sketcher_NewSketch3D"] = ("New 3D sketch", sheet + square_and_circle(sq, el))

# Along X / Y / Z: the Horizontal bar laid along the axis
corner = (10, 54)
ICONS["Constraint_AlongX"] = (
    "Along X",
    bar(add((36, 29), X, -20), add((36, 29), X, 20)) + axes(corner, 15, "X"),
)
ICONS["Constraint_AlongY"] = (
    "Along Y",
    bar(add((38, 33), Y, -20), add((38, 33), Y, 20)) + axes(corner, 15, "Y"),
)
ICONS["Constraint_AlongZ"] = ("Along Z", bar((40, 8), (40, 52)) + axes(corner, 15, "Z"))

# Distance Z: the vertical distance, its end marks lying flat
top, bottom = (40, 9), (40, 55)
ICONS["Constraint_DistanceZ"] = (
    "Distance Z",
    bar(add(top, X, -10), add(top, X, 10))
    + bar(add(bottom, X, -10), add(bottom, X, 10))
    + [line([(40, 18), (40, 46)], C_OUT, 6), line([(40, 18), (40, 46)], C_HI, 2.5)]
    + [head((40, 15), (0, -1)), head((40, 49), (0, 1))]
    + axes(corner, 15, "Z"),
)

# Cycle Working Plane: three planes at a corner, the working one teal, an arrow round them
o = (30, 40)
ICONS["Sketcher_Sketch3DCyclePlane"] = (
    "Cycle working plane",
    [
        face(quad(o, Y, Z, 20, 20), "#ffffff", 1, "#555753"),
        face(quad(o, Z, X, 20, 20), "#ffffff", 1, "#555753"),
        face(quad(o, X, Y, 20, 20), LIGHT, 0.9, DARK, 2.5),
    ]
    + arrow_arc(ring((32, 30), (1, 0), (0, 1), 27, math.radians(200), math.radians(330), 24)),
)

# Lock Height: the height up from the working plane, held by a lock
ICONS["Sketcher_Sketch3DLockHeight"] = (
    "Lock height",
    [
        face(quad((4, 48), X, Y, 22, 24), LIGHT, 0.85, DARK, 2.5),
        line([(26, 47), (26, 14)], C_OUT, 6),
        line([(26, 47), (26, 14)], AXIS["Z"], 2.5),
        head((26, 8), (0, -1), 9),
    ]
    + padlock(38, 26),
)

# Keep Plane: a tilted working plane, pinned
ICONS["Sketcher_Sketch3DKeepPlane"] = (
    "Keep plane",
    [
        face([(6, 40), (34, 56), (58, 30), (30, 14)], LIGHT, 0.85, DARK, 2.5),
        '<circle cx="33" cy="35" r="2.5" fill="#2e3436"/>',
        '<line x1="33" y1="35" x2="42" y2="16" stroke="#2e3436" stroke-width="3" '
        'stroke-linecap="round"/>',
        '<circle cx="44" cy="12" r="7.5" fill="%s" stroke="%s" stroke-width="2"/>' % (C_MID, C_OUT),
        '<circle cx="42" cy="10" r="2.2" fill="#ffffff" fill-opacity="0.8"/>',
    ],
)

# Reset Manipulator: the manipulator's axes, an arrow turning back
ICONS["Sketcher_Sketch3DResetManipulator"] = (
    "Reset manipulator",
    arrow_arc(ring((32, 32), (1, 0), (0, 1), 25, math.radians(-160), math.radians(110), 30), False)
    + axes((30, 38), 20, None, 4),
)

# Rotation Handles: the manipulator's axes inside its three rings
c = (32, 34)
rings = []
for (a, b), k in (((X, Y), "Z"), ((Y, Z), "X"), ((Z, X), "Y")):
    ps = ring(c, a, b, 25)
    rings += [line(ps, "#ffffff", 4, True), line(ps, AXIS[k], 2, True)]
ICONS["Sketcher_Sketch3DRotationHandles"] = (
    "Manipulator rotation handles",
    rings + axes(c, 12, None, 3),
)

FOLDERS = {"Sketcher_Sketch3D": "."}


def write(name, title, body, here):
    folder = FOLDERS.get(name, "constraints" if name.startswith("Constraint_") else "general")
    text = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        "<!-- Written by make_sketch3d_icons.py: change the script, not this file. -->\n"
        '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">\n'
        "  <title>%s</title>\n" % title + "".join("  %s\n" % e for e in body) + "</svg>\n"
    )
    with open(os.path.join(here, folder, name + ".svg"), "w", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    for name, (title, body) in ICONS.items():
        write(name, title, body, here)
