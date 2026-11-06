# SPDX-License-Identifier: LGPL-2.1-or-later
"""Flexible bodies (docs/AssemblyDynamics.md, D4b): a component that bends in
a dynamic run, an Assembly::FlexibleBody in the Physics group. Not upstream.

What the run takes from it is made here, before the run: the component's
mesh of 10-node tetrahedra (Netgen, through the Fem workbench, in a hidden
document of its own) and its interfaces -- for each frame of the model on
the component, the nodes on the faces that hold it there: a joint's faces
on the component, as D4a's loads take them (MotionLoads._facesOf()); a
force element's end, the nearest node, the run's default. Kept on the
object, remade when the shape, the mesh size or what holds the component
changes (MeshSignature)."""

import hashlib

import FreeCAD as App

import UtilsAssembly

__title__ = "Assembly Flexible Bodies"
__url__ = "https://www.freecad.org"


def isFlexibleBody(obj):
    return getattr(obj, "TypeId", "") == "Assembly::FlexibleBody"


def flexibleBodiesOf(sim):
    """The flexible bodies a study uses"""
    return [e for e in getattr(sim, "Elements", []) if isFlexibleBody(e)]


def makeFlexibleBody(assembly, component):
    """A flexible body of a component, in a transaction; used by every
    Dynamic study of the assembly"""
    doc = assembly.Document
    doc.openTransaction("Create flexible body")
    try:
        physics = assembly.getGroup("Physics", True)
        flexible = physics.newObject("Assembly::FlexibleBody", "FlexibleBody")
        flexible.Component = component
        flexible.Label = "Flexible %s" % component.Label
        sims = assembly.getGroup("Simulations")
        for sim in sims.Group if sims else []:
            if getattr(sim, "AnalysisType", None) == "Dynamic":
                sim.Elements = list(sim.Elements) + [flexible]
    finally:
        doc.commitTransaction()
    return flexible


def _assemblyOf(obj):
    for parent in obj.InList:
        if parent.TypeId == "Assembly::AssemblyObject":
            return parent
        found = _assemblyOf(parent)
        if found:
            return found
    return None


def _same(a, b):
    return a is not None and b is not None and a.FullName == b.FullName


def interfacesOf(assembly, component):
    """What holds the component: for each joint with a side on it, the label
    of the model frame it makes there (the joint's full name) and the
    element of its reference on the component"""
    found = []
    joints = assembly.getGroup("Joints", False)
    for joint in joints.Group if joints else []:
        for name in ("Reference1", "Reference2"):
            ref = getattr(joint, name, None)
            if ref and _same(UtilsAssembly.getMovingPart(ref), component):
                found.append((joint.FullName, ref))
                break
    return found


def _element(ref):
    import Part

    sub = ref[1][0] if ref[1] else ""
    element = Part.getShape(ref[0], sub, needSubElement=True, transform=False)
    return None if element.isNull() else element


def _signature(flexible, local, holds):
    """What the mesh is made of, as text: the shape's measures, the size,
    and each interface's frame and reference"""
    box = local.BoundBox
    parts = [
        "%.9g %.9g %d %d" % (local.Volume, local.Area, len(local.Faces), len(local.Edges)),
        " ".join("%.9g" % v for v in (box.XMin, box.YMin, box.ZMin, box.XMax, box.YMax, box.ZMax)),
        "%.9g" % flexible.MeshSize.Value,
    ]
    for label, ref in holds:
        parts.append("%s %s" % (label, ref[1]))
    return hashlib.sha1("\n".join(parts).encode("utf-8")).hexdigest()


def prepare(flexible, force=False):
    """The flexible body's mesh and interfaces made, unless they are of what
    the component is now. True when made."""
    import Part

    import MotionLoads

    component = flexible.Component
    assembly = _assemblyOf(flexible)
    if component is None or assembly is None:
        return False
    local = Part.getShape(component, "", transform=False)
    if local.isNull() or not local.Solids:
        raise ValueError("the flexible body %s is of a component with no solid" % flexible.Label)
    holds = interfacesOf(assembly, component)
    signature = _signature(flexible, local, holds)
    if not force and flexible.MeshSignature == signature and flexible.Nodes:
        return False

    import ObjectsFem

    work = App.newDocument("FlexibleMesh", hidden=True, temp=True)
    try:
        copy = work.addObject("Part::Feature", "Shape")
        copy.Shape = local
        mesh = ObjectsFem.makeMeshNetgenLegacy(work, "Mesh")
        mesh.Shape = copy
        size = flexible.MeshSize.Value
        mesh.MaxSize = size if size > 0 else local.BoundBox.DiagonalLength / 40.0
        mesh.SecondOrder = True
        mesh.Fineness = 2
        work.recompute()
        femmesh = mesh.FemMesh
        if not femmesh.Volumes:
            raise ValueError("the flexible body %s could not be meshed" % flexible.Label)
        # Taken once: each read of Nodes makes the whole dict again
        points = femmesh.Nodes
        ids = sorted(points.keys())
        index = {n: i for i, n in enumerate(ids)}
        rows, _ = MotionLoads._tet10Nodes(femmesh)
        tetrahedra = [index[int(n)] for row in rows for n in row]
        frames = []
        held = []
        faces = copy.Shape.Faces
        for label, ref in holds:
            element = _element(ref)
            names = MotionLoads._facesOf(copy.Shape, element) if element is not None else []
            on = set()
            for name in names:
                face = faces[int(name[4:]) - 1]
                on.update(index[n] for n in femmesh.getNodesByFace(face) if n in index)
            frames.append(label)
            held.extend(sorted(on))
            held.append(-1)
        flexible.Nodes = [points[n] for n in ids]
        flexible.Tetrahedra = tetrahedra
        flexible.InterfaceFrames = frames
        flexible.InterfaceNodes = held
        flexible.MeshSignature = signature
    finally:
        App.closeDocument(work.Name)
    return True


def prepareStudy(sim, force=False):
    """Each flexible body a study uses prepared: the names of those made"""
    made = []
    for flexible in flexibleBodiesOf(sim):
        if prepare(flexible, force):
            made.append(flexible.Label)
    return made
