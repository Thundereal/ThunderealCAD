# SPDX-License-Identifier: LGPL-2.1-or-later
# M1's speed target (docs/SheetMetal.md sec 4.4): a chain of N flanges, one
# feature each, along the edge of a plate 2 thick, each a 90 deg bend of inner
# radius 2 and a 20-high leg, as M0's probe (scripts/sheetmetal-join-probe.cpp)
# measured the joins; rebuilt whole, and one more added. Run:
#   FreeCADCmd -c "exec(open('scripts/sheetmetal-chain-probe.py').read())"
# SM_N sets N (64).
import os
import time
import FreeCAD as App, Part
V = App.Vector
N = int(os.environ.get("SM_N", "64"))
P, W = 15.0, 10.0          # pitch and width of the flanges
doc = App.newDocument("chain")
body = doc.addObject("PartDesign::Body", "Body")
L = N * P + 3 * P
sk = body.newObject("Sketcher::SketchObject", "Sketch")
pts = [(0, 0), (L, 0), (L, 30), (0, 30)]
for i in range(4):
    a, b = pts[i], pts[(i + 1) % 4]
    sk.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))
plate = body.newObject("PartDesign::SheetMetalBaseWall", "Plate")
plate.Profile = sk
doc.recompute()
plate.Rule.Thickness = 2
plate.Rule.BendRadius = 2
doc.recompute()

def segment(feat, x0):
    """the far top edge from x0 on"""
    for i, e in enumerate(feat.Shape.Edges):
        vs = [v.Point for v in e.Vertexes]
        if len(vs) == 2 and all(abs(p.y - 30) < 1e-7 and abs(p.z - 2) < 1e-7 for p in vs):
            xs = sorted(p.x for p in vs)
            if abs(xs[0] - x0) < 1e-7 and abs(xs[1] - L) < 1e-7:
                return "Edge%d" % (i + 1)
    raise RuntimeError("no segment from %g on %s" % (x0, feat.Name))

def add(base, k, start):
    f = body.newObject("PartDesign::SheetMetalFlange", "Flange%03d" % k)
    f.Edges = (base, [segment(base, start)])
    f.BendPosition = "BendOutside"
    f.LengthType = "Leg"
    f.Length = 20 - 2 - 2   # a 20-high leg: R + T up to the leg, then the straight part
    name = f.Edges[1][0]
    first = base.Shape.getElement(name).Vertexes[0].Point.x
    g1, g2 = k * P + P - start, L - (k * P + P + W)
    # Gap1 is at the edge's first vertex
    if abs(first - start) > 1e-7:
        g1, g2 = g2, g1
    f.Gap1, f.Gap2 = g1, g2
    return f

base, start = plate, 0.0
t0 = time.perf_counter()
for k in range(N):
    base = add(base, k, start)
    doc.recompute()
    start = k * P + P + W
build = time.perf_counter() - t0
bad = [o.Name for o in body.Group if "Invalid" in o.State]

# rebuilt whole: the plate touched, all of it recomputed
plate.touch()
t0 = time.perf_counter()
doc.recompute()
rebuild = time.perf_counter() - t0

# one more added at the end
t0 = time.perf_counter()
last = add(base, N, start)
doc.recompute()
one = time.perf_counter() - t0

print("RESULT N=%d faces=%d valid=%s invalid=%s build=%.2fs rebuild=%.2fs one=%.0fms" % (
    N, len(last.Shape.Faces), last.Shape.isValid(), bad, build, rebuild, one * 1000))
