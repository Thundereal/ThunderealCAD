# SPDX-License-Identifier: LGPL-2.1-or-later
"""An App::Animation as an animated glTF (docs/Animation.md sec 9), for the
web viewer, Blender and other tools.

    from freecad import animationgltf
    done = animationgltf.exportGltf(anim, "out.glb")

The document's shown objects are meshed as the view shows them, a node for
each as the document nests them (freecad.gltfscene). The Animation is then
stepped through every frame of its range at its FrameRate, and what each
node does there is written:

  - a placement that moves: the node's translation and rotation. Where the
    object's Placement has a track of its own whose keys are all Constant,
    all Linear, or Bezier (and Linear), the channel is the keys themselves:
    STEP, LINEAR or CUBICSPLINE on the key times, the CUBICSPLINE's
    tangents read off the curve at each key. The translation and the
    rotation each are, when that gives back every frame -- to a micrometre
    and a ten-thousandth of a radian; whatever else moves the object (a
    part track, easing, Turns, a strip, an expression reading Time, a
    Bezier turn too large for a CUBICSPLINE to follow: it blends on the
    sphere) leaves that one as samples at every frame, LINEAR between them;
  - a visibility that switches: the node's scale, 1 or 0, STEP, at the
    frames it switches at. glTF has no visibility to animate, and a node of
    no size is how its tools hide one. An object hidden at rest and shown
    at some frame is in the file;
  - a colour or a transparency that changes: the object's mesh has
    materials of its own, the faces whose colour went the same way through
    the frames sharing one, and each material's baseColorFactor is a
    channel of a sample a frame by KHR_animation_pointer (in
    extensionsUsed, not required: a reader without it shows the first
    frame's colours). One that turns transparent is blended throughout;
  - a shape the animation rebuilds: each shape it takes is a mesh, once
    however many frames have it, on a child node of the object's that is
    shown (scale 1, STEP) at the frames that shape is the object's and
    hidden (0) at the others. glTF does not blend one mesh into another
    of other topology, so the shape is stepped at the frame rate. Two
    shapes are one when their counts of faces, edges and vertices, their
    box, area and volume are;
  - a camera track: a glTF camera, perspective or orthographic as the
    track was keyed, on a node sampled at every frame where the view's
    camera stands -- turning about the point it looks at as the view
    does -- its angle or height a channel too when it changes
    (KHR_animation_pointer). An orthographic camera stands back as far as
    the scene needs, which changes nothing it shows;
  - lines an object draws that the animation changes -- an exploded
    view's trails, which grow as its parts move: an object whose Python
    proxy has gltfLines(obj) is asked at every frame for its lines, each
    [(name, (r, g, b), [(App.Vector, App.Vector), ...])], segments in the
    document's frame as they are on show. Each shape a set of lines takes
    is a mesh of LINES, unlit, on a node shown (scale 1, STEP) at the
    frames it is that shape.

What does not fit is left out and named in the result's "Skipped": a
camera track that cannot be read (no 3D view to say what a camera is, as
in FreeCADCmd) and a camera track of a strip's Animation.

The Animation is left as it was. Times in the file start at 0; the asset's
extras give the range's start.
"""

import os

import FreeCAD as App
import Part

from freecad import gltfscene

__all__ = ["exportGltf"]

SKIP_TYPES = ("App::Animation", "App::Origin", "App::Line", "App::Plane", "App::Point")

# How far either side of a key the curve is read for a CUBICSPLINE tangent
TANGENT_STEP = 1e-4
# What a channel on the keys may miss a frame by: metres, and 1 - |dot| of
# the quaternions (about 1e-4 rad)
MOVE_TOLERANCE = 1e-6
TURN_TOLERANCE = 1e-9


def _isGroup(obj):
    return obj.hasExtension("App::GroupExtension")


def _isHidden(obj):
    return not gltfscene.isShown(obj)


def _signature(shape):
    """What tells one shape from another, cheaply: its counts of faces,
    edges and vertices, and its box, area and volume in its own
    coordinates. A rebuild that gives the same geometry is the same."""
    if shape is None:
        return None
    counts = (len(shape.Faces), len(shape.Edges), len(shape.Vertexes))
    box = shape.BoundBox
    if not box.isValid():
        return counts
    try:
        measures = (shape.Area, shape.Volume if shape.Solids else 0.0)
    except Exception:  # pylint: disable=broad-except
        measures = ()
    return counts + tuple(
        round(v, 6)
        for v in (box.XMin, box.YMin, box.ZMin, box.XMax, box.YMax, box.ZMax) + measures
    )


class _Item:
    """An object of the scene and what it did over the frames"""

    def __init__(self, obj, parent, group):
        self.obj = obj
        self.parent = parent
        self.group = group
        self.placements = []
        self.shown = []
        self.signatures = []
        # Each shape it took, by its signature, in the order first met: the
        # shape and what appearance() gave at that frame
        self.variants = {}
        self.looks = []
        self.node = None
        # The keys of the object's own Placement track, when a channel may
        # be them (_keyPlan), and the placement at each time read for it
        self.plan = None
        self.keyed = {}


def _keyPlan(anim, obj, start, end):
    """The key times of obj's Placement track and the glTF interpolation
    they would be, or None when they cannot be one"""
    if not hasattr(obj, "Placement"):
        return None
    try:
        keys = anim.keys(obj.Name + ".Placement")
    except Exception:  # pylint: disable=broad-except
        return None
    if len(keys) < 2:
        return None
    times = [k[0] for k in keys]
    if times[0] < start - 1e-9 or times[-1] > end + 1e-9:
        return None
    spans = {k[2] for k in keys[:-1]}
    if spans == {"Constant"}:
        kind = "STEP"
    elif spans == {"Linear"}:
        kind = "LINEAR"
    elif spans <= {"Bezier", "Linear"}:
        kind = "CUBICSPLINE"
    else:
        return None
    reads = set(times)
    if kind == "CUBICSPLINE":
        for k, t in enumerate(times):
            for n in (1, 2):
                if k:
                    reads.add(t - n * TANGENT_STEP)
                if k + 1 < len(times):
                    reads.add(t + n * TANGENT_STEP)
    return {"times": times, "kind": kind, "reads": sorted(reads)}


def _aligned(q, to):
    """q on the side of to, so the two are interpolated the short way"""
    return -q if float((q * to).sum()) < 0.0 else q


def _keyChannels(item, numpy):
    """The translation and rotation on the keys: (times, translations,
    rotations), each value array as glTF's interpolation wants it"""
    plan = item.plan
    times = plan["times"]
    count = len(times)
    trs = [gltfscene.trs(item.keyed[t]) for t in times]
    moves = numpy.array([t for t, _r in trs], dtype=numpy.float64)
    turns = numpy.array([r for _t, r in trs], dtype=numpy.float64)
    for k in range(1, count):
        turns[k] = _aligned(turns[k], turns[k - 1])
    if plan["kind"] != "CUBICSPLINE":
        return times, moves, turns
    # [in-tangent, value, out-tangent] a key, the tangents per second, each
    # a one-sided difference of the second order off its own side of the key
    outMoves = numpy.zeros((count * 3, 3))
    outTurns = numpy.zeros((count * 3, 4))
    h = TANGENT_STEP
    for k, t in enumerate(times):
        outMoves[3 * k + 1] = moves[k]
        outTurns[3 * k + 1] = turns[k]
        for side, row in ((-1, 0), (1, 2)):
            near, far = t + side * h, t + side * 2 * h
            if near not in item.keyed or far not in item.keyed:
                continue
            m1, q1 = gltfscene.trs(item.keyed[near])
            m2, q2 = gltfscene.trs(item.keyed[far])
            q1 = _aligned(numpy.array(q1), turns[k])
            q2 = _aligned(numpy.array(q2), turns[k])
            outMoves[3 * k + row] = (
                side * (-3 * moves[k] + 4 * numpy.array(m1) - numpy.array(m2)) / (2 * h)
            )
            outTurns[3 * k + row] = side * (-3 * turns[k] + 4 * q1 - q2) / (2 * h)
    return times, outMoves, outTurns


def _evaluate(kind, times, values, t, rotation, numpy):
    """What a glTF sampler gives at t"""
    count = len(times)
    value = (lambda k: values[3 * k + 1]) if kind == "CUBICSPLINE" else (lambda k: values[k])
    if t <= times[0]:
        return numpy.array(value(0))
    if t >= times[-1]:
        return numpy.array(value(count - 1))
    k = max(i for i in range(count) if times[i] <= t)
    t0, t1 = times[k], times[k + 1]
    u = (t - t0) / (t1 - t0)
    if kind == "STEP":
        return numpy.array(value(k))
    if kind == "LINEAR":
        a, b = numpy.array(value(k)), numpy.array(value(k + 1))
        if not rotation:
            return a + (b - a) * u
        d = min(1.0, max(-1.0, float(numpy.dot(a, b))))
        if d > 0.9995:
            r = a + (b - a) * u
        else:
            angle = numpy.arccos(d)
            r = (numpy.sin((1 - u) * angle) * a + numpy.sin(u * angle) * b) / numpy.sin(angle)
        return r / numpy.linalg.norm(r)
    span = t1 - t0
    u2, u3 = u * u, u * u * u
    r = (
        (2 * u3 - 3 * u2 + 1) * values[3 * k + 1]
        + span * (u3 - 2 * u2 + u) * values[3 * k + 2]
        + (-2 * u3 + 3 * u2) * values[3 * (k + 1) + 1]
        + span * (u3 - u2) * values[3 * (k + 1)]
    )
    return r / numpy.linalg.norm(r) if rotation else r


def _fits(kind, times, values, stamps, baked, rotation, numpy):
    """Whether the channel gives back every baked frame"""
    for t, want in zip(stamps, baked):
        got = _evaluate(kind, times, values, float(t), rotation, numpy)
        if rotation:
            if 1.0 - abs(float(numpy.dot(got, want))) > TURN_TOLERANCE:
                return False
        elif float(numpy.max(numpy.abs(got - want))) > MOVE_TOLERANCE:
            return False
    return True


def _collect(objs, parent, items, seen):
    for obj in objs:
        if obj in seen or obj.TypeId in SKIP_TYPES:
            continue
        seen.add(obj)
        group = _isGroup(obj)
        if not group and not hasattr(obj, "Placement"):
            continue
        item = _Item(obj, parent, group)
        items.append(item)
        if group:
            _collect(obj.Group, item, items, seen)


def _roots(doc):
    """The objects no group holds, in the document's order"""
    held = set()
    for obj in doc.Objects:
        if _isGroup(obj):
            held.update(obj.Group)
    return [o for o in doc.Objects if o not in held]


def _sceneSphere(items):
    """The centre (mm) of the scene and a radius it stays in, moving as the
    frames move it: the box of the shapes now, and the farthest any object
    stood from where it is now"""
    box = App.BoundBox()
    drift = 0.0
    for item in items:
        if item.parent is not None:
            continue
        try:
            shape = Part.getShape(item.obj, "", needSubElement=False)
        except Exception:  # pylint: disable=broad-except
            shape = None
        if shape is not None and not shape.isNull() and shape.BoundBox.isValid():
            box.add(shape.BoundBox)
        now = getattr(item.obj, "Placement", None)
        if now is not None and item.placements:
            drift = max(drift, max((p.Base - now.Base).Length for p in item.placements))
    if not box.isValid():
        return App.Vector(), 1.0 + drift
    return box.Center, box.DiagonalLength / 2.0 + drift


def _viewAspect():
    """The width over the height of the active 3D view, 1 without one"""
    try:
        import FreeCADGui as Gui

        width, height = Gui.ActiveDocument.ActiveView.getSize()
        return float(width) / float(height) if height else 1.0
    except Exception:  # pylint: disable=broad-except
        return 1.0


def _cameras(anim, doc, writer, root, items, times, channel, skipped, numpy):
    """A glTF camera a camera track of anim, on a node under root sampled
    at every frame. -> how many"""
    import math

    for track in anim.Tracks:
        if track.get("Kind") != "Strip":
            continue
        child = doc.getObject(track.get("Path", "").split(":", 1)[-1])
        if child is None or not child.isDerivedFrom("App::Animation"):
            continue
        for inner in child.Tracks:
            if inner.get("Kind") == "Camera":
                skipped.append(
                    "the camera track %s of the strip %s" % (inner.get("Path", ""), child.Label)
                )
    tracks = [t for t in anim.Tracks if t.get("Kind") == "Camera" and not t.get("Mute")]
    if not tracks:
        return 0
    centre, radius = _sceneSphere(items)
    aspect = _viewAspect()
    count = len(times)
    made = 0
    for track in tracks:
        path = track.get("Path", "")
        try:
            values = [anim.value(path, t) for t in times]
        except Exception:  # pylint: disable=broad-except
            values = None
        if not values or any(v is None or len(v) != 9 for v in values):
            skipped.append("the camera track %s: no 3D view to read it by" % path)
            continue
        # A key is the point looked at, the orientation, the distance to the
        # point and the size: an orthographic height, a perspective angle in
        # degrees (Gui/AnimationCamera.cpp)
        perspective = path.endswith("/perspective")
        focals = [App.Vector(*v[0:3]) for v in values]
        turns = [App.Rotation(*v[3:7]) for v in values]
        distances = [float(v[7]) for v in values]
        sizes = [float(v[8]) for v in values]
        # From a point looked at, the farthest the scene reaches, mm
        reach = radius + max((f - centre).Length for f in focals)
        translations = numpy.empty((count, 3), dtype=numpy.float64)
        rotations = numpy.empty((count, 4), dtype=numpy.float64)
        for k in range(count):
            # An orthographic picture is the same from any distance back:
            # back far enough that nothing is behind the camera
            back = distances[k] if perspective else max(distances[k], reach)
            looking = turns[k].multVec(App.Vector(0, 0, -1))
            stand = App.Placement(focals[k] - looking * back, turns[k])
            translations[k], rotations[k] = gltfscene.trs(stand)
            if k and numpy.dot(rotations[k], rotations[k - 1]) < 0.0:
                rotations[k] = -rotations[k]
        if perspective:
            near = max(1e-6, min(distances) * 1e-5)
            far = (max(distances) + reach) * 1.01e-3
            camera = {
                "type": "perspective",
                "perspective": {"yfov": math.radians(sizes[0]), "znear": near, "zfar": far},
            }
        else:
            far = (max(max(distances), reach) + reach) * 1.01e-3
            ymag = sizes[0] * 0.5e-3
            camera = {
                "type": "orthographic",
                "orthographic": {"xmag": ymag * aspect, "ymag": ymag, "znear": 0.0, "zfar": far},
            }
        cameras = writer.gltf.setdefault("cameras", [])
        cameras.append(camera)
        index = len(cameras) - 1
        name = track.get("Label") or "Camera (%s)" % path.split(":", 1)[-1].split("/")[0]
        node = writer.node(
            {
                "name": name,
                "camera": index,
                "translation": [float(v) for v in translations[0]],
                "rotation": [float(v) for v in rotations[0]],
                "extras": {"track": path},
            },
            root,
        )
        if float(numpy.ptp(translations, axis=0).max()) > 0.0 or float(
            numpy.ptp(rotations, axis=0).max()
        ) > 0.0:
            channel(node, "translation", translations, "VEC3", "LINEAR")
            channel(node, "rotation", rotations, "VEC4", "LINEAR")
        if max(sizes) != min(sizes):
            if perspective:
                channel(
                    None,
                    "/cameras/%d/perspective/yfov" % index,
                    numpy.radians(numpy.array(sizes, dtype=numpy.float64)),
                    "SCALAR",
                    "LINEAR",
                )
            else:
                heights = numpy.array(sizes, dtype=numpy.float64) * 0.5e-3
                channel(None, "/cameras/%d/orthographic/ymag" % index, heights, "SCALAR", "LINEAR")
                channel(
                    None,
                    "/cameras/%d/orthographic/xmag" % index,
                    heights * aspect,
                    "SCALAR",
                    "LINEAR",
                )
        made += 1
    return made


def _liners(doc):
    """The objects that draw lines an animation changes: their proxy's
    gltfLines(obj) says what is on show"""
    out = []
    for obj in doc.Objects:
        proxy = getattr(obj, "Proxy", None)
        if callable(getattr(proxy, "gltfLines", None)):
            out.append(obj)
    return out


def _lineMesh(writer, name, color, segments):
    """A mesh of LINES, unlit in `color`, of segments (pairs of points, mm)"""
    import numpy

    positions = numpy.array(
        [(p.x, p.y, p.z) for segment in segments for p in segment], dtype=numpy.float64
    )
    positions = (positions * 1e-3).astype(numpy.float32)
    writer.extension("KHR_materials_unlit")
    writer.gltf["materials"].append(
        {
            "pbrMetallicRoughness": {
                "baseColorFactor": gltfscene.baseColor(color, 0.0),
                "metallicFactor": 0.0,
                "roughnessFactor": 1.0,
            },
            "extensions": {"KHR_materials_unlit": {}},
        }
    )
    material = len(writer.gltf["materials"]) - 1
    accessor = writer.accessor(positions, "VEC3", target=gltfscene.ARRAY_BUFFER, bounds=True)
    writer.gltf["meshes"].append(
        {
            "name": name,
            "primitives": [{"attributes": {"POSITION": accessor}, "mode": 1, "material": material}],
        }
    )
    return len(writer.gltf["meshes"]) - 1


def _lines(writer, root, liners, lines, times, channel, numpy):
    """Each shape each liner's set of lines takes, a node under root shown
    at the frames it is that shape. -> how many nodes"""
    made = 0
    for obj in liners:
        names, variants = [], {}
        for k, frame in enumerate(lines[obj.Name]):
            for name, color, segments in frame or []:
                if not segments:
                    continue
                if name not in names:
                    names.append(name)
                key = (
                    name,
                    tuple(
                        (round(p.x, 6), round(p.y, 6), round(p.z, 6))
                        for segment in segments
                        for p in segment
                    ),
                )
                if key not in variants:
                    variants[key] = (color, segments, [])
                variants[key][2].append(k)
        for name in names:
            for key, (color, segments, frames) in variants.items():
                if key[0] != name:
                    continue
                label = "%s %s" % (obj.Label, name)
                node = {"name": label, "mesh": _lineMesh(writer, label, color, segments)}
                always = len(frames) == len(times)
                if not always and frames[0] != 0:
                    node["scale"] = [0.0, 0.0, 0.0]
                index = writer.node(node, root)
                made += 1
                if always:
                    continue
                at = set(frames)
                switches = [
                    k for k in range(len(times)) if not k or ((k in at) != (k - 1 in at))
                ]
                scales = numpy.array(
                    [[1.0, 1.0, 1.0] if k in at else [0.0, 0.0, 0.0] for k in switches],
                    dtype=numpy.float64,
                )
                channel(index, "scale", scales, "VEC3", "STEP", [times[k] for k in switches])
    return made


def exportGltf(anim, path):
    """Writes anim as an animated glTF at path: .glb binary, else JSON with
    a .bin beside it.

    Returns a dict: Frames, Nodes, Animated (the nodes that move or
    switch), Keyed (the nodes whose move is written on its keys, not on
    every frame), PartlyKeyed (those whose translation or rotation is, the
    other sampled), Recoloured (the nodes whose colours are animated),
    Rebuilt (the nodes whose shape changes), Shapes (the meshes of their
    shapes, together), Cameras, Lines (the nodes of lines, one for each
    shape a set of lines takes), Meshes, Bytes (the file's, with its .bin)
    and Skipped (a line for each thing the animation does that the file
    does not hold)."""
    import numpy

    doc = anim.Document
    times = list(anim.frames())
    if not times:
        raise ValueError("the animation has no frames")
    items = []
    _collect(_roots(doc), None, items, set())
    if not items:
        raise ValueError("the document has nothing to export")
    rebuilds = anim.Recompute != "Never"
    for item in items:
        item.plan = _keyPlan(anim, item.obj, times[0], times[-1])
    reads = sorted({t for item in items if item.plan for t in item.plan["reads"]})
    liners = _liners(doc)
    lines = {obj.Name: [] for obj in liners}

    wasActive = bool(anim.Active)
    time = anim.Time
    def at(t):
        anim.Time = t
        # A Time already at t is no change, and activates nothing: the
        # frame would be the design as it rests
        if not anim.Active:
            anim.activate()

    try:
        for t in times:
            at(t)
            for item in items:
                obj = item.obj
                item.shown.append(not _isHidden(obj))
                item.placements.append(
                    obj.Placement if hasattr(obj, "Placement") else App.Placement()
                )
                if not item.group:
                    look = gltfscene.appearance(obj)
                    item.looks.append(look)
                    if rebuilds:
                        shape = gltfscene.shapeOf(obj)
                        signature = _signature(shape)
                        item.signatures.append(signature)
                        if signature not in item.variants:
                            item.variants[signature] = (shape, look)
            for obj in liners:
                lines[obj.Name].append(obj.Proxy.gltfLines(obj))
        # Where the keys are, and either side of them for a tangent
        for t in reads:
            at(t)
            for item in items:
                if item.plan and t in item.plan["reads"]:
                    item.keyed[t] = item.obj.Placement
    finally:
        if wasActive:
            anim.Time = time
        else:
            anim.stop()

    writer = gltfscene.Writer("ThunderealCAD Animation")
    writer.gltf["asset"]["extras"] = {
        "units": "m",
        "timeStart": times[0],
        "frameRate": anim.FrameRate,
        "animation": anim.Name,
    }
    root = writer.node({"name": doc.Name, "rotation": gltfscene.Z_UP_TO_Y_UP})
    stamps = numpy.array([t - times[0] for t in times], dtype=numpy.float32)
    channels, samplers, skipped = [], [], []
    clock = [None]
    animated = 0
    keyed = 0
    partly = 0
    looked = 0

    def channel(node, target, values, kind, interpolation, at=None):
        if at is None:
            if clock[0] is None:
                clock[0] = writer.accessor(stamps, "SCALAR", bounds=True)
            source = clock[0]
        else:
            source = writer.accessor(
                numpy.array([t - times[0] for t in at], dtype=numpy.float32),
                "SCALAR",
                bounds=True,
            )
        output = writer.accessor(values.astype(numpy.float32), kind)
        samplers.append({"input": source, "output": output, "interpolation": interpolation})
        if node is None:
            # Not a node's: a property anywhere in the file, by its JSON
            # pointer (KHR_animation_pointer)
            writer.extension("KHR_animation_pointer")
            to = {"path": "pointer", "extensions": {"KHR_animation_pointer": {"pointer": target}}}
        else:
            to = {"node": node, "path": target}
        channels.append({"sampler": len(samplers) - 1, "target": to})

    rebuiltNodes = 0
    shapes = 0
    for item in items:
        obj = item.obj
        ever = any(item.shown)
        if not ever and not item.group:
            continue
        mesh = None
        recoloured = []
        rebuilt = not item.group and len(item.variants) > 1
        relooked = bool(item.looks) and any(look != item.looks[0] for look in item.looks[1:])
        if not item.group and not rebuilt:
            if relooked:
                mesh, recoloured = gltfscene.animatedMeshOf(writer, obj, item.looks)
            else:
                mesh = gltfscene.meshOf(writer, obj)
            if mesh is None:
                continue
        node = {"name": obj.Label, "extras": {"name": obj.Name}}
        if mesh is not None:
            node["mesh"] = mesh
        node["translation"], node["rotation"] = gltfscene.trs(item.placements[0])
        if not item.shown[0]:
            node["scale"] = [0.0, 0.0, 0.0]
        parent = item.parent.node if item.parent is not None else root
        if parent is None:
            parent = root
        item.node = writer.node(node, parent)

        moved = False
        first = item.placements[0]
        if any(not p.isSame(first, 0.0) for p in item.placements[1:]):
            translations = numpy.empty((len(times), 3), dtype=numpy.float64)
            rotations = numpy.empty((len(times), 4), dtype=numpy.float64)
            for k, placement in enumerate(item.placements):
                translations[k], rotations[k] = gltfscene.trs(placement)
                # Each quaternion on the side of the one before: interpolated
                # the short way
                if k and numpy.dot(rotations[k], rotations[k - 1]) < 0.0:
                    rotations[k] = -rotations[k]
            # Each of the two on the keys where that gives back every frame:
            # a Bezier span turns along a curve on the sphere, which a
            # CUBICSPLINE follows for a small turn and not for a large one,
            # while the translation beside it fits all the same
            moveKeys = turnKeys = False
            if item.plan:
                kind = item.plan["kind"]
                at, moves, turns = _keyChannels(item, numpy)
                moveKeys = _fits(kind, at, moves, times, translations, False, numpy)
                turnKeys = _fits(kind, at, turns, times, rotations, True, numpy)
            if moveKeys:
                channel(item.node, "translation", moves, "VEC3", kind, at)
            else:
                channel(item.node, "translation", translations, "VEC3", "LINEAR")
            if turnKeys:
                channel(item.node, "rotation", turns, "VEC4", kind, at)
            else:
                channel(item.node, "rotation", rotations, "VEC4", "LINEAR")
            keyed += 1 if moveKeys and turnKeys else 0
            partly += 1 if moveKeys != turnKeys else 0
            moved = True
        if any(s != item.shown[0] for s in item.shown[1:]):
            # Only where it switches: STEP holds the value till the next
            switches = [k for k, s in enumerate(item.shown) if not k or s != item.shown[k - 1]]
            scales = numpy.array(
                [[1.0, 1.0, 1.0] if item.shown[k] else [0.0, 0.0, 0.0] for k in switches],
                dtype=numpy.float64,
            )
            channel(item.node, "scale", scales, "VEC3", "STEP", [times[k] for k in switches])
            moved = True
        if rebuilt:
            # A child a shape, shown at the frames it is the object's
            made = 0
            for k, (signature, (shape, look)) in enumerate(item.variants.items()):
                if relooked:
                    piece, colours = gltfscene.animatedMeshOf(writer, obj, item.looks, shape)
                else:
                    piece, colours = gltfscene.meshOf(writer, obj, shape, look), []
                if piece is None:
                    continue
                on = [s == signature for s in item.signatures]
                child = {
                    "name": "%s.%d" % (obj.Label, k),
                    "mesh": piece,
                    "extras": {"name": obj.Name, "shape": k},
                }
                if not on[0]:
                    child["scale"] = [0.0, 0.0, 0.0]
                index = writer.node(child, item.node)
                switches = [f for f, s in enumerate(on) if f and s != on[f - 1]]
                if switches:
                    switches = [0] + switches
                    scales = numpy.array(
                        [[1.0, 1.0, 1.0] if on[f] else [0.0, 0.0, 0.0] for f in switches],
                        dtype=numpy.float64,
                    )
                    channel(index, "scale", scales, "VEC3", "STEP", [times[f] for f in switches])
                recoloured += colours
                made += 1
            shapes += made
            rebuiltNodes += 1
            moved = True
        # A colour or a transparency that changes: its materials' base
        # colour at every frame, by KHR_animation_pointer
        for material, history in recoloured:
            factors = numpy.array(
                [gltfscene.baseColor(c, t) for c, t in history], dtype=numpy.float64
            )
            channel(
                None,
                "/materials/%d/pbrMetallicRoughness/baseColorFactor" % material,
                factors,
                "VEC4",
                "LINEAR",
            )
            moved = True
        if recoloured:
            looked += 1
        if moved:
            animated += 1

    cameras = _cameras(anim, doc, writer, root, items, times, channel, skipped, numpy)
    lineNodes = _lines(writer, root, liners, lines, times, channel, numpy)

    if channels:
        writer.gltf["animations"] = [
            {"name": anim.Label, "channels": channels, "samplers": samplers}
        ]
    nodes = len(writer.gltf["nodes"])
    meshes = len(writer.gltf["meshes"])
    size = gltfscene.save(writer, path)
    return {
        "Frames": len(times),
        "Nodes": nodes,
        "Animated": animated,
        "Keyed": keyed,
        "PartlyKeyed": partly,
        "Recoloured": looked,
        "Rebuilt": rebuiltNodes,
        "Shapes": shapes,
        "Cameras": cameras,
        "Lines": lineNodes,
        "Meshes": meshes,
        "Bytes": size,
        "Skipped": skipped,
        "Path": os.path.abspath(path),
    }
