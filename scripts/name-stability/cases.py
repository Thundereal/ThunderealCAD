# The corpus of the name stability harness: models, and the upstream changes
# made to each (docs/TopoNamingEnhance.md sec 9.5).
#
# A case is a function building a model in the document it is given and
# returning whatever its changes need.  @case names the changes: each is a
# label and a function of that return value.  A change should leave most of
# the model's geometry where it was -- a hole more, a feature more, one tool
# moved -- because an element is only known to be the same one by its geometry.
import types

import FreeCAD as App
import Part

V = App.Vector
CASES = []


def case(*changes):
    def deco(fn):
        fn.changes = changes
        CASES.append(fn)
        return fn

    return deco


def ns(**kw):
    return types.SimpleNamespace(**kw)


# ---------------------------------------------------------------- helpers


def new_body(doc):
    return doc.addObject("PartDesign::Body", "Body")


def sketch(body, support=None, name="Sketch"):
    """A sketch on the body's XY plane, or on (object, [sub])."""
    s = body.newObject("Sketcher::SketchObject", name)
    if support is None:
        support = (body.Origin.OriginFeatures[3], [""])
    elif isinstance(support, int):
        support = (body.Origin.OriginFeatures[support], [""])
    s.AttachmentSupport = support
    s.MapMode = "FlatFace"
    return s


def poly(s, pts):
    n = len(pts)
    for i in range(n):
        a, b = pts[i], pts[(i + 1) % n]
        s.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(b[0], b[1], 0)))


def rect(s, x0, y0, x1, y1):
    poly(s, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)])


def circle(s, x, y, r):
    s.addGeometry(Part.Circle(V(x, y, 0), V(0, 0, 1), r))


def feature(body, kind, name, **props):
    f = body.newObject("PartDesign::" + kind, name)
    for k, v in props.items():
        setattr(f, k, v)
    f.Document.recompute()
    if not f.isValid():
        raise RuntimeError("%s is invalid: %s" % (name, f.getStatusString()))
    return f


def edges(obj, pred):
    return ["Edge%d" % (i + 1) for i, e in enumerate(obj.Shape.Edges) if pred(e)]


def faces(obj, pred):
    return ["Face%d" % (i + 1) for i, f in enumerate(obj.Shape.Faces) if pred(f)]


def mid(e):
    return e.valueAt(0.5 * (e.FirstParameter + e.LastParameter))


def vertical(e, x=None, y=None):
    """A straight edge along Z, optionally at (x, y)."""
    if len(e.Vertexes) != 2:
        return False
    p, q = e.Vertexes[0].Point, e.Vertexes[1].Point
    if abs(p.x - q.x) > 1e-6 or abs(p.y - q.y) > 1e-6 or abs(p.z - q.z) < 1e-6:
        return False
    if abs(e.Length - abs(p.z - q.z)) > 1e-6:
        return False
    return (x is None or abs(p.x - x) < 1e-6) and (y is None or abs(p.y - y) < 1e-6)


def round_edge(e, r):
    """A full circle of radius r."""
    return len(e.Vertexes) == 1 and abs(e.Length - 2 * 3.141592653589793 * r) < 1e-6


def flat_at(f, z):
    """A face lying in the plane Z = z."""
    bb = f.BoundBox
    return abs(bb.ZMin - z) < 1e-6 and abs(bb.ZMax - z) < 1e-6


def at_z(e, z):
    bb = e.BoundBox
    return abs(bb.ZMin - z) < 1e-6 and abs(bb.ZMax - z) < 1e-6


# ------------------------------------------------------------------ cases


@case(
    ("a hole added to the sketch", lambda c: circle(c.s, 30, 10, 3)),
    ("a hole taken out of the sketch", lambda c: c.s.delGeometry(5)),
    ("a corner edge more in the fillet", lambda c: setattr(c.fillet, "Base", (c.pad, c.corners))),
)
def pad_fillet_chamfer(doc):
    """A plate with two holes, three corners rounded, the holes' rims chamfered."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, 0, 0, 60, 40)
    circle(s, 15, 20, 4)
    circle(s, 45, 20, 4)
    pad = feature(b, "Pad", "Pad", Profile=s, Length=10)
    corners = edges(pad, vertical)
    corners = [n for n in corners if pad.Shape.getElement(n).Length > 9]
    fillet = feature(b, "Fillet", "Fillet", Base=(pad, corners[:3]), Radius=3)
    rims = edges(fillet, lambda e: at_z(e, 10) and round_edge(e, 4) and mid(e).x < 30)
    feature(b, "Chamfer", "Chamfer", Base=(fillet, rims), Size=1)
    return ns(s=s, pad=pad, fillet=fillet, corners=corners)


@case(
    ("a third slot in the pocket", lambda c: rect(c.s2, 25, 28, 35, 34)),
    ("a hole added to the pad", lambda c: circle(c.s, 52, 8, 3)),
)
def pocket_on_face(doc):
    """Two slots cut into a plate's top, the first slot's corners rounded."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, 0, 0, 60, 40)
    pad = feature(b, "Pad", "Pad", Profile=s, Length=10)
    top = faces(pad, lambda f: flat_at(f, 10))
    s2 = sketch(b, (pad, top), "SlotSketch")
    rect(s2, 8, 8, 20, 16)
    rect(s2, 30, 8, 50, 20)
    pocket = feature(b, "Pocket", "Pocket", Profile=s2, Length=5)
    inner = edges(pocket, lambda e: vertical(e) and mid(e).x < 25 and abs(e.Length - 5) < 1e-6)
    feature(b, "Fillet", "Fillet", Base=(pocket, inner), Radius=1)
    return ns(s=s, s2=s2)


def occurrences(pattern, n, **more):
    for k, v in more.items():
        setattr(pattern, k, v)
    pattern.Occurrences = n


# The spacing is kept, so the holes that stay do not move.
@case(
    ("three holes become four", lambda c: occurrences(c.pattern, 4, Length=60)),
    ("three holes become two", lambda c: occurrences(c.pattern, 2, Length=20)),
)
def linear_pattern(doc):
    """A hole repeated along a plate, the plate's top rim rounded after."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, 0, 0, 80, 30)
    pad = feature(b, "Pad", "Pad", Profile=s, Length=8)
    s2 = sketch(b, (pad, faces(pad, lambda f: flat_at(f, 8))), "HoleSketch")
    circle(s2, 10, 15, 4)
    pocket = feature(b, "Pocket", "Pocket", Profile=s2, Length=8)
    pattern = feature(
        b,
        "LinearPattern",
        "Pattern",
        Originals=[pocket],
        Direction=(s, ["H_Axis"]),
        Length=40,
        Occurrences=3,
    )
    rim = edges(pattern, lambda e: at_z(e, 8) and len(e.Vertexes) == 2 and e.Length > 20)
    feature(b, "Fillet", "Fillet", Base=(pattern, rim), Radius=1.5)
    return ns(pattern=pattern)


@case(
    ("three lugs become four", lambda c: occurrences(c.pattern, 4, Angle=270)),
    ("a hole added to the disc", lambda c: circle(c.s, 0, 0, 5)),
)
def polar_pattern(doc):
    """A lug repeated around a disc, mirrored through the disc's plane."""
    b = new_body(doc)
    s = sketch(b)
    circle(s, 0, 0, 30)
    pad = feature(b, "Pad", "Pad", Profile=s, Length=6)
    s2 = sketch(b, None, "LugSketch")
    rect(s2, 26, -4, 38, 4)
    lug = feature(b, "Pad", "Lug", Profile=s2, Length=10)
    pattern = feature(
        b,
        "PolarPattern",
        "Pattern",
        Originals=[lug],
        Axis=(s, ["N_Axis"]),
        Angle=180,
        Occurrences=3,
    )
    feature(
        b,
        "Mirrored",
        "Mirrored",
        Originals=[pattern],
        MirrorPlane=(b.Origin.OriginFeatures[3], [""]),
    )
    return ns(s=s, pattern=pattern)


@case(
    ("a hole added to the base", lambda c: circle(c.s, 12, 12, 4)),
    ("a second boss", lambda c: circle(c.s2, 45, 25, 6)),
    ("the wall made thicker", lambda c: setattr(c.thick, "Value", 2)),
)
def thickness_boss(doc):
    """A plate with a round boss, hollowed from below."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, 0, 0, 60, 40)
    pad = feature(b, "Pad", "Pad", Profile=s, Length=10)
    s2 = sketch(b, (pad, faces(pad, lambda f: flat_at(f, 10))), "BossSketch")
    circle(s2, 20, 20, 8)
    boss = feature(b, "Pad", "Boss", Profile=s2, Length=6)
    bottom = faces(boss, lambda f: flat_at(f, 0))
    thick = feature(b, "Thickness", "Thickness", Base=(boss, bottom), Value=1.5, Reversed=True)
    return ns(s=s, s2=s2, thick=thick)


PLATE = "/home/thunder/works/sw/occt/tests/fork/thickness/models/issue360_plate.brep"


@case(
    ("the wall made thinner", lambda c: setattr(c.thick, "Value", 3)),
    ("the wall made a little thicker", lambda c: setattr(c.thick, "Value", 4.5)),
)
def plate360_thickness(doc):
    """realthunder/FreeCAD#360's plate, hollowed: a model with no names to start from."""
    base = doc.addObject("Part::Feature", "Plate")
    base.Shape = Part.read(PLATE)
    b = new_body(doc)
    feat = b.newObject("PartDesign::FeatureBase", "Base")
    feat.BaseFeature = base
    doc.recompute()
    thick = feature(
        b,
        "Thickness",
        "Thickness",
        Base=(feat, ["Face5"]),
        Value=4,
        Reversed=True,
        Join="Arc",
        Intersection=True,
    )
    return ns(thick=thick, base=feat)


@case(
    (
        "the second drill moved through a side",
        lambda c: setattr(c.drill2, "Placement", App.Placement(V(38, 15, -5), App.Rotation())),
    ),
    ("the first drill made wider", lambda c: setattr(c.drill1, "Radius", 7)),
    ("a third drill", lambda c: c.add()),
)
def part_cut_fillet(doc):
    """Part workbench: a box drilled twice, four of its edges rounded."""
    box = doc.addObject("Part::Box", "Box")
    box.Length, box.Width, box.Height = 40, 30, 20
    drill1 = doc.addObject("Part::Cylinder", "Drill1")
    drill1.Radius, drill1.Height = 5, 30
    drill1.Placement = App.Placement(V(10, 10, -5), App.Rotation())
    drill2 = doc.addObject("Part::Cylinder", "Drill2")
    drill2.Radius, drill2.Height = 4, 30
    drill2.Placement = App.Placement(V(30, 15, -5), App.Rotation())
    tools = doc.addObject("Part::MultiFuse", "Tools")
    tools.Shapes = [drill1, drill2]
    cut = doc.addObject("Part::Cut", "Cut")
    cut.Base, cut.Tool = box, tools
    doc.recompute()
    fillet = doc.addObject("Part::Fillet", "Fillet")
    fillet.Base = cut
    fillet.Edges = [
        (i + 1, 2.0, 2.0)
        for i, e in enumerate(cut.Shape.Edges)
        if vertical(e) and abs(e.Length - 20) < 1e-6 and mid(e).y < 1
    ]
    doc.recompute()

    def add():
        d = doc.addObject("Part::Cylinder", "Drill3")
        d.Radius, d.Height = 3, 30
        d.Placement = App.Placement(V(20, 22, -5), App.Rotation())
        tools.Shapes = [drill1, drill2, d]

    return ns(drill1=drill1, drill2=drill2, add=add)


@case(
    ("a step cut into the profile", lambda c: c.step()),
    ("a second groove", lambda c: rect(c.s2, 14, 20, 20, 24)),
)
def revolve_groove(doc):
    """A turned part with one groove, its top rim chamfered."""
    b = new_body(doc)
    s = sketch(b, 4)  # XZ
    poly(s, [(0, 0), (20, 0), (20, 30), (10, 30), (10, 40), (0, 40)])
    rev = feature(
        b, "Revolution", "Revolution", Profile=s, ReferenceAxis=(s, ["V_Axis"]), Angle=360
    )
    s2 = sketch(b, 4, "GrooveSketch")
    rect(s2, 17, 8, 22, 12)
    groove = feature(b, "Groove", "Groove", Profile=s2, ReferenceAxis=(s2, ["V_Axis"]), Angle=360)
    # the top's outer circle, whichever of Y and Z the sketch plane turns up
    rim = edges(
        groove,
        lambda e: round_edge(e, 10)
        and (abs(abs(mid(e).y) - 40) < 1e-6 or abs(abs(mid(e).z) - 40) < 1e-6),
    )
    if rim:
        feature(b, "Chamfer", "Chamfer", Base=(groove, rim[:1]), Size=1)

    def step():
        # the line (20,30)-(10,30) becomes a step down to 26
        s.delGeometry(2)
        s.delGeometry(1)
        for a, c in (
            ((20, 0), (20, 26)),
            ((20, 26), (15, 26)),
            ((15, 26), (15, 30)),
            ((15, 30), (10, 30)),
        ):
            s.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(c[0], c[1], 0)))

    return ns(s=s, s2=s2, step=step)


def sm_edge_at(f, x=None, y=None):
    """The longest straight edge of the top skin lying at x or at y."""
    best = None
    for i, e in enumerate(f.Shape.Edges):
        p = [v.Point for v in e.Vertexes]
        if len(p) != 2 or abs(p[0].z - 1) > 1e-6 or abs(p[1].z - 1) > 1e-6:
            continue
        if x is not None and (abs(p[0].x - x) > 1e-6 or abs(p[1].x - x) > 1e-6):
            continue
        if y is not None and (abs(p[0].y - y) > 1e-6 or abs(p[1].y - y) > 1e-6):
            continue
        if best is None or e.Length > best[1]:
            best = ("Edge%d" % (i + 1), e.Length)
    if best is None:
        raise RuntimeError("no edge at x=%r y=%r" % (x, y))
    return best[0]


@case(
    ("a third flange", lambda c: c.flange("Flange3", x=0)),
    ("a hole in the wall", lambda c: circle(c.s, 40, 30, 6)),
)
def sheet_metal(doc):
    """A sheet metal wall with a flange on two of its edges."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, 0, 0, 80, 60)
    wall = feature(b, "SheetMetalBaseWall", "Wall", Profile=s)

    def flange(name, **at):
        base = b.Tip
        return feature(b, "SheetMetalFlange", name, Edges=(base, [sm_edge_at(base, **at)]))

    flange("Flange1", y=60)
    flange("Flange2", x=80)
    return ns(s=s, flange=flange)


@case(
    (
        "a bend more in the path",
        lambda c: c.s2.addGeometry(Part.LineSegment(V(20, 50, 0), V(40, 50, 0))),
    ),
    ("the profile made hollow", lambda c: circle(c.s, 0, 0, 2)),
)
def pipe(doc):
    """A square bar swept along a bent path."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, -4, -4, 4, 4)
    s2 = sketch(b, 4, "Path")  # XZ
    s2.addGeometry(Part.LineSegment(V(0, 0, 0), V(0, 30, 0)))
    s2.addGeometry(Part.LineSegment(V(0, 30, 0), V(20, 50, 0)))
    feature(b, "AdditivePipe", "Pipe", Profile=s, Spine=s2, Transition="Right corner")
    return ns(s=s, s2=s2)


@case(
    ("a hole added to the sketch", lambda c: circle(c.s, 20, 15, 4)),
    ("a notch cut into the sketch", lambda c: c.notch()),
)
def draft_fillet(doc):
    """A block with two drafted sides, its top rim rounded."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, 0, 0, 50, 30)
    pad = feature(b, "Pad", "Pad", Profile=s, Length=20)
    sides = faces(pad, lambda f: abs(f.BoundBox.YMax - f.BoundBox.YMin) < 1e-6)
    bottom = faces(pad, lambda f: flat_at(f, 0))
    draft = feature(b, "Draft", "Draft", Base=(pad, sides), NeutralPlane=(pad, bottom), Angle=5)
    rim = edges(draft, lambda e: at_z(e, 20) and len(e.Vertexes) == 2)
    feature(b, "Fillet", "Fillet", Base=(draft, rim), Radius=2)

    def notch():
        # the line (50,0)-(50,30) gets a notch
        s.delGeometry(1)
        for a, c in (
            ((50, 0), (50, 10)),
            ((50, 10), (44, 10)),
            ((44, 10), (44, 20)),
            ((44, 20), (50, 20)),
            ((50, 20), (50, 30)),
        ):
            s.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(c[0], c[1], 0)))

    return ns(s=s, notch=notch)


@case(
    ("a third hole", lambda c: circle(c.s2, 30, 30, 3)),
    ("a slot added to the plate", lambda c: rect(c.s, 40, 5, 55, 10)),
)
def hole(doc):
    """Two counterbored holes in a plate."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, 0, 0, 60, 40)
    pad = feature(b, "Pad", "Pad", Profile=s, Length=12)
    s2 = sketch(b, (pad, faces(pad, lambda f: flat_at(f, 12))), "HoleSketch")
    circle(s2, 15, 15, 3)
    circle(s2, 45, 15, 3)
    doc.recompute()
    h = doc.addObject("PartDesign::Hole", "Hole")
    h.Profile = s2
    b.addObject(h)
    doc.recompute()
    h.Diameter = 6
    h.Depth = 8
    h.DepthType = 0
    h.HoleCutType = "Counterbore"
    h.HoleCutDiameter = 10
    h.HoleCutDepth = 3
    doc.recompute()
    if not h.isValid():
        raise RuntimeError("Hole is invalid: %s" % h.getStatusString())
    return ns(s=s, s2=s2)


@case(
    ("two rows become three", lambda c: occurrences(c.rows, 3, Length=24)),
    ("the slot made a round hole", lambda c: c.swap()),
)
def multi_transform(doc):
    """A slot mirrored, then repeated in rows; the plate's corners rounded after."""
    b = new_body(doc)
    s = sketch(b)
    rect(s, -40, 0, 40, 50)
    pad = feature(b, "Pad", "Pad", Profile=s, Length=6)
    s2 = sketch(b, (pad, faces(pad, lambda f: flat_at(f, 6))), "SlotSketch")
    rect(s2, 10, 6, 30, 12)
    pocket = feature(b, "Pocket", "Pocket", Profile=s2, Length=6)
    multi = doc.addObject("PartDesign::MultiTransform", "Multi")
    multi.Originals = [pocket]
    b.addObject(multi)
    doc.recompute()
    mirror = doc.addObject("PartDesign::Mirrored", "Mirror")
    mirror.MirrorPlane = (s, ["V_Axis"])
    b.addObject(mirror)
    rows = doc.addObject("PartDesign::LinearPattern", "Rows")
    rows.Direction = (s, ["V_Axis"])
    rows.Length = 12
    rows.Occurrences = 2
    b.addObject(rows)
    multi.Transformations = [mirror, rows]
    b.Tip = multi
    doc.recompute()
    if not multi.isValid():
        raise RuntimeError("Multi is invalid: %s" % multi.getStatusString())
    corners = edges(
        multi, lambda e: vertical(e) and abs(e.Length - 6) < 1e-6 and abs(abs(mid(e).x) - 40) < 1e-6
    )
    feature(b, "Fillet", "Fillet", Base=(multi, corners), Radius=4)

    def swap():
        for i in (3, 2, 1, 0):
            s2.delGeometry(i)
        circle(s2, 20, 9, 3)

    return ns(rows=rows, swap=swap)


def spots(face, n):
    """n points well inside a face, far from each other: its biggest triangles' middles."""
    pts, tris = face.tessellate(0.1)
    mids = sorted(
        (
            (
                (pts[b] - pts[a]).cross(pts[c] - pts[a]).Length,
                (pts[a] + pts[b] + pts[c]) * (1.0 / 3.0),
            )
            for a, b, c in tris
        ),
        key=lambda t: t[0],
    )
    res = []
    for _, m in reversed(mids):
        if all((m - r).Length > 10 for r in res):
            res.append(m)
        if len(res) == n:
            break
    return res


@case(
    ("a pocket more through the plate", lambda c: circle(c.s, c.at.x, c.at.y, 0.5)),
)
def brep_pocket(doc):
    """A pocket on a shape that came with no names (a .brep)."""
    base = doc.addObject("Part::Feature", "Plate")
    base.Shape = Part.read(PLATE)
    b = new_body(doc)
    feat = b.newObject("PartDesign::FeatureBase", "Base")
    feat.BaseFeature = base
    doc.recompute()
    first, second = spots(feat.Shape.Face5, 2)
    s = sketch(b, None, "PocketSketch")
    circle(s, first.x, first.y, 0.5)
    feature(b, "Pocket", "Pocket", Profile=s, Type="ThroughAll", Midplane=True)
    return ns(s=s, at=second)


@case(
    ("a ball more in the fuse", lambda c: c.add()),
    (
        "the stud moved off the edge",
        lambda c: setattr(c.stud, "Placement", App.Placement(V(15, 15, 10), App.Rotation())),
    ),
)
def part_fuse_chamfer(doc):
    """Part workbench: a box, a stud and a ball fused, the box's top edges chamfered."""
    box = doc.addObject("Part::Box", "Box")
    box.Length, box.Width, box.Height = 40, 30, 15
    stud = doc.addObject("Part::Cylinder", "Stud")
    stud.Radius, stud.Height = 5, 12
    stud.Placement = App.Placement(V(15, 28, 10), App.Rotation())
    ball = doc.addObject("Part::Sphere", "Ball")
    ball.Radius = 6
    ball.Placement = App.Placement(V(40, 15, 8), App.Rotation())
    fuse = doc.addObject("Part::MultiFuse", "Fuse")
    fuse.Shapes = [box, stud, ball]
    doc.recompute()
    chamfer = doc.addObject("Part::Chamfer", "Chamfer")
    chamfer.Base = fuse
    chamfer.Edges = [
        (i + 1, 1.0, 1.0)
        for i, e in enumerate(fuse.Shape.Edges)
        if at_z(e, 15) and len(e.Vertexes) == 2 and mid(e).y < 1
    ]
    doc.recompute()

    def add():
        d = doc.addObject("Part::Sphere", "Ball2")
        d.Radius = 5
        d.Placement = App.Placement(V(0, 0, 8), App.Rotation())
        fuse.Shapes = [box, stud, ball, d]

    return ns(stud=stud, add=add)


@case(
    (
        "the drill moved",
        lambda c: setattr(c.drill, "Placement", App.Placement(V(28, 12, -5), App.Rotation())),
    ),
    ("a second drill", lambda c: c.add()),
)
def part_thickness(doc):
    """Part workbench: a drilled box hollowed from its top."""
    box = doc.addObject("Part::Box", "Box")
    box.Length, box.Width, box.Height = 40, 30, 20
    drill = doc.addObject("Part::Cylinder", "Drill")
    drill.Radius, drill.Height = 4, 30
    drill.Placement = App.Placement(V(12, 12, -5), App.Rotation())
    tools = doc.addObject("Part::MultiFuse", "Tools")
    tools.Shapes = [drill]
    cut = doc.addObject("Part::Cut", "Cut")
    cut.Base, cut.Tool = box, drill
    doc.recompute()
    thick = doc.addObject("Part::Thickness", "Thickness")
    thick.Faces = (cut, faces(cut, lambda f: flat_at(f, 20)))
    thick.Value = -1.5
    doc.recompute()

    def add():
        d = doc.addObject("Part::Cylinder", "Drill2")
        d.Radius, d.Height = 3, 30
        d.Placement = App.Placement(V(30, 20, -5), App.Rotation())
        tools.Shapes = [drill, d]
        cut.Tool = tools

    return ns(drill=drill, add=add)


@case(
    ("a notch cut into the sketch", lambda c: c.notch()),
    (
        "one edge less in the first fillet",
        lambda c: setattr(c.fillet, "Base", (c.pad, c.corners[1:])),
    ),
)
def dressup_chain(doc):
    """An L-shaped block: corners rounded, then the top rim rounded, then the bottom chamfered."""
    b = new_body(doc)
    s = sketch(b)
    poly(s, [(0, 0), (60, 0), (60, 20), (25, 20), (25, 45), (0, 45)])
    pad = feature(b, "Pad", "Pad", Profile=s, Length=15)
    corners = edges(pad, lambda e: vertical(e) and abs(e.Length - 15) < 1e-6)
    fillet = feature(b, "Fillet", "Fillet", Base=(pad, corners), Radius=3)
    top = faces(fillet, lambda f: flat_at(f, 15))
    feature(b, "Fillet", "TopFillet", Base=(fillet, top), Radius=1.5)
    bottom = faces(b.Tip, lambda f: flat_at(f, 0))
    feature(b, "Chamfer", "Chamfer", Base=(b.Tip, bottom), Size=1)

    def notch():
        # the line (0,45)-(0,0) gets a notch
        s.delGeometry(5)
        for a, c in (
            ((0, 45), (0, 30)),
            ((0, 30), (8, 30)),
            ((8, 30), (8, 15)),
            ((8, 15), (0, 15)),
            ((0, 15), (0, 0)),
        ):
            s.addGeometry(Part.LineSegment(V(a[0], a[1], 0), V(c[0], c[1], 0)))

    return ns(notch=notch, fillet=fillet, pad=pad, corners=corners)
