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

import FreeCAD as App  # noqa: F401

from freecad.gltfscene import (  # noqa: F401 -- some are for those who import them from here
    Writer,
    Z_UP_TO_Y_UP,
    appearance,
    deflection,
    isContainer,
    isShown,
    meshOf,
    save,
    srgbToLinear,
    trs,
)

__all__ = ["exportGltf"]

# The assembly's groups that are not components
NOT_COMPONENTS = (
    "Assembly::JointGroup",
    "Assembly::SimulationGroup",
    "Assembly::ViewGroup",
    "Assembly::BomGroup",
    "Assembly::Simulation",
)


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
    writer = Writer("ThunderealCAD Assembly")
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
    size = save(writer, path)
    return {
        "Frames": len(exporter.frames),
        "Nodes": len(writer.gltf["nodes"]),
        "Animated": exporter.animated,
        "Meshes": len(writer.gltf.get("meshes", [])),
        "Bytes": size,
    }
