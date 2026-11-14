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
    at some frame is in the file.

What does not fit is left out and named in the result's "Skipped": an
object whose shape the animation rebuilds (it is in the file as it rests),
a colour or transparency that changes, and camera tracks.

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


def _signature(obj):
    """What tells one shape of obj from another, cheaply: its faces and the
    box of it in its own coordinates"""
    try:
        shape = Part.getShape(obj, "", needSubElement=False, transform=False)
    except Exception:  # pylint: disable=broad-except
        return None
    if shape.isNull():
        return None
    box = shape.BoundBox
    if not box.isValid():
        return (len(shape.Faces),)
    return (len(shape.Faces),) + tuple(
        round(v, 6) for v in (box.XMin, box.YMin, box.ZMin, box.XMax, box.YMax, box.ZMax)
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


def exportGltf(anim, path):
    """Writes anim as an animated glTF at path: .glb binary, else JSON with
    a .bin beside it.

    Returns a dict: Frames, Nodes, Animated (the nodes that move or
    switch), Keyed (the nodes whose move is written on its keys, not on
    every frame), PartlyKeyed (those whose translation or rotation is, the
    other sampled), Meshes, Bytes (the file's, with its .bin) and Skipped (a
    line for each thing the animation does that the file does not hold)."""
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
                    item.looks.append(gltfscene.appearance(obj))
                    if rebuilds:
                        item.signatures.append(_signature(obj))
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
        channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": target}})

    for item in items:
        obj = item.obj
        ever = any(item.shown)
        if not ever and not item.group:
            continue
        mesh = None
        if not item.group:
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
        if moved:
            animated += 1
        if item.signatures and any(s != item.signatures[0] for s in item.signatures[1:]):
            skipped.append("%s: its shape changes; exported as it rests" % obj.Label)
        if item.looks and any(look != item.looks[0] for look in item.looks[1:]):
            skipped.append("%s: its colour or transparency changes" % obj.Label)

    for track in anim.Tracks:
        if track.get("Kind") == "Camera":
            skipped.append("the camera track %s" % track.get("Path", ""))

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
        "Meshes": meshes,
        "Bytes": size,
        "Skipped": skipped,
        "Path": os.path.abspath(path),
    }
