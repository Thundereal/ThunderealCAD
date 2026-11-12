# SPDX-License-Identifier: LGPL-2.1-or-later
"""An App::Animation as an animated glTF (docs/Animation.md sec 9), for the
web viewer, Blender and other tools.

    from freecad import animationgltf
    done = animationgltf.exportGltf(anim, "out.glb")

The document's shown objects are meshed as the view shows them, a node for
each as the document nests them (freecad.gltfscene). The Animation is then
stepped through every frame of its range at its FrameRate, and what each
node does there is written as samples:

  - a placement that moves: the node's translation and rotation, LINEAR
    between the frames. Whatever moved it is in the samples -- keys of any
    interpolation, easing, Turns, strips, an expression reading Time;
  - a visibility that switches: the node's scale, 1 or 0, STEP. glTF has
    no visibility to animate, and a node of no size is how its tools hide
    one. An object hidden at rest and shown at some frame is in the file.

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
    switch), Meshes, Bytes (the file's, with its .bin) and Skipped (a line
    for each thing the animation does that the file does not hold)."""
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

    wasActive = bool(anim.Active)
    time = anim.Time
    try:
        for t in times:
            anim.Time = t
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

    def channel(node, target, values, kind, interpolation):
        if clock[0] is None:
            clock[0] = writer.accessor(stamps, "SCALAR", bounds=True)
        output = writer.accessor(values.astype(numpy.float32), kind)
        samplers.append({"input": clock[0], "output": output, "interpolation": interpolation})
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
            channel(item.node, "translation", translations, "VEC3", "LINEAR")
            channel(item.node, "rotation", rotations, "VEC4", "LINEAR")
            moved = True
        if any(s != item.shown[0] for s in item.shown[1:]):
            scales = numpy.array(
                [[1.0, 1.0, 1.0] if s else [0.0, 0.0, 0.0] for s in item.shown],
                dtype=numpy.float64,
            )
            channel(item.node, "scale", scales, "VEC3", "STEP")
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
        "Meshes": meshes,
        "Bytes": size,
        "Skipped": skipped,
        "Path": os.path.abspath(path),
    }
