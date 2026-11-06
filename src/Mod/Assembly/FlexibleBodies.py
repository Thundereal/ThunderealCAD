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


# ---------------------------------------------------------------- display

# A tetrahedron's faces by its corners, and the nodes of its edges by the
# corners they join: _tet10Mass()'s order
_FACES = ((0, 1, 2), (0, 1, 3), (1, 2, 3), (0, 2, 3))
_EDGE = {(0, 1): 4, (1, 2): 5, (0, 2): 6, (0, 3): 7, (1, 3): 8, (2, 3): 9}


def surfaceOf(tetrahedra, nodes):
    """The mesh's outer faces, each a 6-node triangle as four: a list of
    node triples, each turning counterclockwise seen from outside -- away
    from its tetrahedron's fourth corner. A face is outer when one
    tetrahedron has it."""
    count = {}
    for t in range(0, len(tetrahedra), 10):
        tet = tetrahedra[t : t + 10]
        for face in _FACES:
            key = tuple(sorted(tet[i] for i in face))
            if key in count:
                count[key] = None
            else:
                count[key] = (tet, face)
    triangles = []
    for found in count.values():
        if found is None:
            continue
        tet, (a, b, c) = found
        (other,) = {0, 1, 2, 3} - {a, b, c}
        pa, pb, pc, po = (nodes[tet[i]] for i in (a, b, c, other))
        if (pb - pa).cross(pc - pa).dot(po - pa) > 0:
            b, c = c, b

        def mid(i, j):
            return tet[_EDGE[(min(i, j), max(i, j))]]

        A, B, C = tet[a], tet[b], tet[c]
        ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
        triangles += [(A, ab, ca), (ab, B, bc), (ca, bc, C), (ab, bc, ca)]
    return triangles


class Deformation:
    """A flexible body's deformation as its last run had it: its mesh's
    surface moved by its mode shapes and the run's coordinates at a time,
    scaled up to be seen: its largest over the run a tenth of its size,
    unless show() is given a scale."""

    def __init__(self, flexible, results):
        import numpy as np

        self.flexible = flexible
        self.component = flexible.Component
        columns = flexible.ModeShapeColumns
        shapes = flexible.ModeShapes
        nodes = flexible.Nodes
        self.valid = bool(
            self.component and columns > 0 and nodes and len(shapes) == 3 * len(nodes) * columns
        )
        if not self.valid:
            return
        triangles = surfaceOf(list(flexible.Tetrahedra), nodes)
        used = sorted({n for tri in triangles for n in tri})
        index = {n: i for i, n in enumerate(used)}
        self.triangles = [tuple(index[n] for n in tri) for tri in triangles]
        self.rest = np.array([tuple(nodes[n]) for n in used])
        psi = np.array(shapes).reshape(len(nodes), 3, columns)
        self.psi = psi[used]  # surface nodes x 3 x columns
        # The run's coordinates: its interfaces' deformation, then its modes'
        name = flexible.FullName
        found = {}
        for channel in results.get("Channels", []):
            obj = channel.get("Object")
            if (obj is not None and obj.FullName == name) or channel.get("Name") == name:
                found[channel["Quantity"]] = channel["Values"]
        self.times = list(results.get("Times", []))

        def rows(values):
            return [v if isinstance(v, (tuple, list)) else (v,) for v in values]

        interfaces = rows(found.get("InterfaceDeformation", []))
        modes = rows(found.get("ModalCoordinates", []))
        if len(interfaces) != len(self.times) or len(modes) != len(self.times):
            self.valid = False
            return
        self.coordinates = np.array([tuple(a) + tuple(b) for a, b in zip(interfaces, modes)])
        if self.coordinates.shape[1] != columns:
            self.valid = False
            return
        largest = 0.0
        for row in self.coordinates:
            moved = self.psi @ row
            largest = max(largest, float(np.max(np.linalg.norm(moved, axis=1))))
        size = float(np.max(np.ptp(self.rest, axis=0)))
        self.scale = 0.1 * size / largest if largest > 0 else 1.0
        self.root = None

    def coordinatesAt(self, time):
        import bisect

        times = self.times
        if time <= times[0]:
            return self.coordinates[0]
        if time >= times[-1]:
            return self.coordinates[-1]
        i = bisect.bisect_right(times, time) - 1
        span = times[i + 1] - times[i]
        f = (time - times[i]) / span if span > 0 else 0.0
        return self.coordinates[i] * (1 - f) + self.coordinates[i + 1] * f

    def pointsAt(self, time, scale=None):
        """The surface's nodes at a time, in the component's coordinates"""
        moved = self.psi @ self.coordinatesAt(time)
        return self.rest + (self.scale if scale is None else scale) * moved

    def show(self, time, scale=None):
        """Drawn at a time, over the component, which hides"""
        import FreeCADGui as Gui
        from pivy import coin

        if not self.valid:
            return
        points = self.pointsAt(time, scale)
        if self.root is None:
            self.root = coin.SoSeparator()
            self.transform = coin.SoTransform()
            # Outward, counterclockwise (surfaceOf()): lit on its outer side
            hints = coin.SoShapeHints()
            hints.vertexOrdering = coin.SoShapeHints.COUNTERCLOCKWISE
            hints.shapeType = coin.SoShapeHints.SOLID
            hints.creaseAngle = 0.5
            material = coin.SoMaterial()
            material.diffuseColor = (0.45, 0.62, 0.81)
            self.coords = coin.SoCoordinate3()
            faces = coin.SoIndexedFaceSet()
            index = []
            for tri in self.triangles:
                index += [tri[0], tri[1], tri[2], -1]
            faces.coordIndex.setValues(0, len(index), index)
            for node in (self.transform, hints, material, self.coords, faces):
                self.root.addChild(node)
            view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
            if view is None:
                self.root = None
                return
            view.getSceneGraph().addChild(self.root)
            self.wasVisible = self.component.ViewObject.Visibility
            self.component.ViewObject.Visibility = False
        self.coords.point.setValues(0, len(points), [tuple(p) for p in points])
        at = self.component.getGlobalPlacement()
        self.transform.translation = tuple(at.Base)
        self.transform.rotation = at.Rotation.Q

    def remove(self):
        import FreeCADGui as Gui

        if self.root is None:
            return
        view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
        if view is not None:
            view.getSceneGraph().removeChild(self.root)
        self.root = None
        try:
            self.component.ViewObject.Visibility = self.wasVisible
        except Exception:
            pass


def deformationsOf(sim, results):
    """The deformations of a study's flexible bodies its last run has"""
    found = []
    for flexible in flexibleBodiesOf(sim):
        deformation = Deformation(flexible, results)
        if deformation.valid:
            found.append(deformation)
    return found
