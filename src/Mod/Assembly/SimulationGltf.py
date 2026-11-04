# SPDX-License-Identifier: LGPL-2.1-or-later
"""A simulation's frames as an animated glTF (docs/AssemblyDynamics.md sec 8.4):
the assembly's parts meshed, each one the run moves animated through its
placements, for Blender and web viewers.

The file is glTF 2.0, binary (.glb) or JSON with its buffer beside it (.gltf
and .bin). Metres and Y up, as glTF has them: the root node turns the
assembly's Z up to Y. A node per component, as the assembly nests them, each
named by its label (its document name in its extras); a part the run moves
has its placement at each frame, interpolated linearly between them, and its
time from the run's first frame. Each component's faces are coloured as its
view shows them. The asset's extras name the study, say whether its results
were stale, and give the run's start time."""

import json
import math
import os
import struct

import FreeCAD as App
import Part

__all__ = ["exportGltf"]

# glTF's constants
FLOAT = 5126
UNSIGNED_INT = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963

# Z up to Y up: a quarter turn about x, backwards
Z_UP_TO_Y_UP = [-math.sqrt(0.5), 0.0, 0.0, math.sqrt(0.5)]

# The assembly's groups that are not components
NOT_COMPONENTS = (
    "Assembly::JointGroup",
    "Assembly::SimulationGroup",
    "Assembly::ViewGroup",
    "Assembly::BomGroup",
    "Assembly::Simulation",
)

GREY = (0.8, 0.8, 0.8, 1.0)


def srgbToLinear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


class Writer:
    """The file's JSON and its one buffer, filled as the scene is walked"""

    def __init__(self):
        self.gltf = {
            "asset": {"version": "2.0", "generator": "ThunderealCAD Assembly"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [],
            "meshes": [],
            "materials": [],
            "accessors": [],
            "bufferViews": [],
            "buffers": [],
        }
        self.blob = bytearray()
        self.materials = {}
        self.meshes = {}

    def view(self, data, target=None):
        while len(self.blob) % 4:
            self.blob.append(0)
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.blob += data
        self.gltf["bufferViews"].append(view)
        return len(self.gltf["bufferViews"]) - 1

    def accessor(self, array, kind, componentType=FLOAT, target=None, bounds=False):
        """array: numpy, one row per element"""
        accessor = {
            "bufferView": self.view(array.tobytes(), target),
            "componentType": componentType,
            "count": len(array),
            "type": kind,
        }
        if bounds:
            shaped = array.reshape(len(array), -1)
            accessor["min"] = [float(v) for v in shaped.min(axis=0)]
            accessor["max"] = [float(v) for v in shaped.max(axis=0)]
        self.gltf["accessors"].append(accessor)
        return len(self.gltf["accessors"]) - 1

    def material(self, color, transparency):
        key = (tuple(round(c, 6) for c in color[:3]), round(transparency, 6))
        if key in self.materials:
            return self.materials[key]
        alpha = 1.0 - transparency
        material = {
            "pbrMetallicRoughness": {
                "baseColorFactor": [srgbToLinear(c) for c in key[0]] + [alpha],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.5,
            },
            "doubleSided": True,
        }
        if alpha < 1.0:
            material["alphaMode"] = "BLEND"
        self.gltf["materials"].append(material)
        self.materials[key] = len(self.gltf["materials"]) - 1
        return self.materials[key]

    def node(self, node, parent=None):
        self.gltf["nodes"].append(node)
        index = len(self.gltf["nodes"]) - 1
        if parent is not None:
            self.gltf["nodes"][parent].setdefault("children", []).append(index)
        return index


def appearance(obj):
    """Each face's colour and transparency as obj's view shows them; one for
    all when the view gives one, None without a view"""
    for owner in (obj, obj.getLinkedObject(True)):
        vobj = getattr(owner, "ViewObject", None)
        materials = getattr(vobj, "ShapeAppearance", None) if vobj else None
        if materials:
            return [(tuple(m.DiffuseColor), m.Transparency) for m in materials]
    return None


def deflection(obj, shape):
    """The view's meshing tolerance, mm: its Deviation, a per cent of the
    shape's size"""
    vobj = getattr(obj.getLinkedObject(True), "ViewObject", None)
    deviation = getattr(vobj, "Deviation", 0.5) if vobj else 0.5
    size = shape.BoundBox.DiagonalLength if shape.BoundBox.isValid() else 1.0
    return max(size * deviation / 100.0, 1e-3)


def meshOf(writer, obj):
    """A glTF mesh of obj's shape in its own coordinates, in metres; None when
    it has no faces. Two components showing one shape the same way share it."""
    import numpy

    try:
        shape = Part.getShape(obj, "", needSubElement=False, transform=False)
    except Exception:
        return None
    if shape.isNull() or not shape.Faces:
        return None
    faces = shape.Faces
    colors = appearance(obj)
    if colors is None:
        colors = [(GREY, 0.0)]
    if len(colors) != len(faces):
        colors = colors[:1]
    key = (shape.hashCode(), tuple(colors))
    if key in writer.meshes:
        return writer.meshes[key]

    tolerance = deflection(obj, shape)
    groups = {}
    for i, face in enumerate(faces):
        points, triangles = face.tessellate(tolerance)
        if not points or not triangles:
            continue
        p = numpy.array([(v.x, v.y, v.z) for v in points], dtype=numpy.float64)
        t = numpy.array(triangles, dtype=numpy.int64)
        # Each vertex's normal: the face's triangles around it, by their area
        n = numpy.cross(p[t[:, 1]] - p[t[:, 0]], p[t[:, 2]] - p[t[:, 0]])
        normals = numpy.zeros_like(p)
        for k in range(3):
            numpy.add.at(normals, t[:, k], n)
        length = numpy.linalg.norm(normals, axis=1)
        length[length == 0.0] = 1.0
        normals /= length[:, None]
        color, transparency = colors[i if len(colors) > 1 else 0]
        groups.setdefault(writer.material(color, transparency), []).append((p, normals, t))
    if not groups:
        writer.meshes[key] = None
        return None

    primitives = []
    for material, parts in groups.items():
        positions, normals, indices = [], [], []
        base = 0
        for p, n, t in parts:
            positions.append(p)
            normals.append(n)
            indices.append(t + base)
            base += len(p)
        positions = (numpy.concatenate(positions) * 1e-3).astype(numpy.float32)
        normals = numpy.concatenate(normals).astype(numpy.float32)
        indices = numpy.concatenate(indices).astype(numpy.uint32).reshape(-1)
        primitives.append(
            {
                "attributes": {
                    "POSITION": writer.accessor(
                        positions, "VEC3", target=ARRAY_BUFFER, bounds=True
                    ),
                    "NORMAL": writer.accessor(normals, "VEC3", target=ARRAY_BUFFER),
                },
                "indices": writer.accessor(
                    indices, "SCALAR", UNSIGNED_INT, target=ELEMENT_ARRAY_BUFFER
                ),
                "material": material,
            }
        )
    writer.gltf["meshes"].append({"name": obj.Label, "primitives": primitives})
    writer.meshes[key] = len(writer.gltf["meshes"]) - 1
    return writer.meshes[key]


def isContainer(obj):
    return obj.hasExtension("App::GeoFeatureGroupExtension") or obj.isDerivedFrom(
        "Assembly::AssemblyLink"
    )


def isShown(obj):
    vobj = getattr(obj, "ViewObject", None)
    if vobj is not None:
        return vobj.Visibility
    return obj.Visibility


def trs(placement):
    """A node's translation (m) and rotation of a Placement"""
    q = placement.Rotation.Q
    base = placement.Base
    return [base.x * 1e-3, base.y * 1e-3, base.z * 1e-3], [q[0], q[1], q[2], q[3]]


class Exporter:
    def __init__(self, assembly, writer):
        import numpy

        self.assembly = assembly
        self.writer = writer
        results = assembly.getSimulationResults()
        times = results["Times"]
        # glTF's key frames go strictly forward
        self.frames = [0]
        for i in range(1, len(times)):
            if times[i] > times[self.frames[-1]]:
                self.frames.append(i)
        self.timeStart = times[0] if times else 0.0
        self.times = numpy.array(
            [times[i] - self.timeStart for i in self.frames], dtype=numpy.float32
        )
        self.simulation = results.get("Simulation")
        self.input = None
        self.channels = []
        self.samplers = []
        self.animated = 0

    def walk(self, objs, parent):
        for obj in objs:
            if obj.TypeId in NOT_COMPONENTS or not isShown(obj):
                continue
            if isContainer(obj):
                children = obj.Group if hasattr(obj, "Group") else []
                node = self.place(obj, parent)
                self.walk(children, node)
            elif obj.TypeId == "App::DocumentObjectGroup":
                node = self.writer.node({"name": obj.Label, "extras": {"name": obj.Name}}, parent)
                self.walk(obj.Group, node)
            elif hasattr(obj, "Placement"):
                mesh = meshOf(self.writer, obj)
                if mesh is not None:
                    self.place(obj, parent, mesh)

    def place(self, obj, parent, mesh=None):
        """obj's node: where it stands, or where it goes, frame by frame"""
        import numpy

        node = {"name": obj.Label, "extras": {"name": obj.Name}}
        if mesh is not None:
            node["mesh"] = mesh
        placements = self.assembly.getSimulationPlacements(obj)
        if placements:
            placements = [placements[i] for i in self.frames]
            moves = any(not p.isSame(placements[0], 0.0) for p in placements[1:])
        else:
            placements = [obj.Placement]
            moves = False
        t, r = trs(placements[0])
        node["translation"], node["rotation"] = t, r
        index = self.writer.node(node, parent)
        if not moves:
            return index

        translations = numpy.empty((len(placements), 3), dtype=numpy.float64)
        rotations = numpy.empty((len(placements), 4), dtype=numpy.float64)
        for k, placement in enumerate(placements):
            translations[k], rotations[k] = trs(placement)
            # Each quaternion on the side of the one before: interpolated the
            # short way
            if k and numpy.dot(rotations[k], rotations[k - 1]) < 0.0:
                rotations[k] = -rotations[k]
        if self.input is None:
            self.input = self.writer.accessor(self.times, "SCALAR", bounds=True)
        for path, values, kind in (
            ("translation", translations, "VEC3"),
            ("rotation", rotations, "VEC4"),
        ):
            output = self.writer.accessor(values.astype(numpy.float32), kind)
            self.samplers.append({"input": self.input, "output": output, "interpolation": "LINEAR"})
            self.channels.append(
                {"sampler": len(self.samplers) - 1, "target": {"node": index, "path": path}}
            )
        self.animated += 1
        return index


def exportGltf(assembly, path):
    """Writes the frames assembly holds -- its last run, or the results
    loadSimulationResults() read -- as an animated glTF at path: .glb
    binary, else JSON with a .bin beside it.

    Returns: a dict: Frames, Nodes, Animated (the nodes that move), Meshes
    and Bytes (the file's, with its .bin)."""
    import numpy  # noqa: F401 -- fails here, not halfway

    if assembly.numberOfFrames() < 1:
        raise ValueError("the assembly holds no simulation frames")
    writer = Writer()
    exporter = Exporter(assembly, writer)
    sim = exporter.simulation
    extras = {"units": "m", "timeStart": exporter.timeStart}
    if sim is not None:
        extras["study"] = sim.Label
        extras["stale"] = bool(getattr(sim, "ResultsStale", False))
    writer.gltf["asset"]["extras"] = extras

    root = writer.node({"name": assembly.Document.Name, "rotation": Z_UP_TO_Y_UP})
    top = writer.node({"name": assembly.Label, "extras": {"name": assembly.Name}}, root)
    t, r = trs(assembly.getGlobalPlacement())
    writer.gltf["nodes"][top]["translation"] = t
    writer.gltf["nodes"][top]["rotation"] = r
    exporter.walk(assembly.Group, top)
    if exporter.channels:
        writer.gltf["animations"] = [
            {
                "name": sim.Label if sim is not None else "Simulation",
                "channels": exporter.channels,
                "samplers": exporter.samplers,
            }
        ]
    for key in ("meshes", "materials", "animations"):
        if key in writer.gltf and not writer.gltf[key]:
            del writer.gltf[key]

    while len(writer.blob) % 4:
        writer.blob.append(0)
    buffer = {"byteLength": len(writer.blob)}
    writer.gltf["buffers"] = [buffer]
    binary = os.path.splitext(path)[1].lower() == ".glb"
    if binary:
        text = json.dumps(writer.gltf, separators=(",", ":")).encode("utf-8")
        text += b" " * (-len(text) % 4)
        total = 12 + 8 + len(text) + 8 + len(writer.blob)
        with open(path, "wb") as f:
            f.write(struct.pack("<4sII", b"glTF", 2, total))
            f.write(struct.pack("<I4s", len(text), b"JSON"))
            f.write(text)
            f.write(struct.pack("<I4s", len(writer.blob), b"BIN\0"))
            f.write(writer.blob)
        size = total
    else:
        bin_path = os.path.splitext(path)[0] + ".bin"
        buffer["uri"] = os.path.basename(bin_path)
        with open(bin_path, "wb") as f:
            f.write(writer.blob)
        text = json.dumps(writer.gltf, indent=1).encode("utf-8")
        with open(path, "wb") as f:
            f.write(text)
        size = len(text) + len(writer.blob)
    return {
        "Frames": len(exporter.frames),
        "Nodes": len(writer.gltf["nodes"]),
        "Animated": exporter.animated,
        "Meshes": len(writer.gltf.get("meshes", [])),
        "Bytes": size,
    }
