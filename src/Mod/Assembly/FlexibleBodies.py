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


# ---------------------------------------------------------------- stresses

_GRADIENTS = []


def _tet10Gradients():
    """The 10-node tetrahedron's shape functions' derivatives by its natural
    coordinates (xi, eta, zeta: its corners 1, 2 and 3's barycentric
    coordinates) at each of its own nodes: an array [node, function, 3], the
    nodes in _EDGE's order -- corners, then the edges (0 1) (1 2) (2 0)
    (0 3) (1 3) (2 3). A corner's function L (2 L - 1), an edge's 4 L_i L_j."""
    import numpy as np

    if _GRADIENTS:
        return _GRADIENTS[0]
    edges = [(0, 1), (1, 2), (2, 0), (0, 3), (1, 3), (2, 3)]
    corners = np.eye(4)
    points = list(corners) + [0.5 * (corners[i] + corners[j]) for i, j in edges]
    gradients = np.zeros((10, 10, 3))
    for p, L in enumerate(points):
        # By the barycentric coordinates, then by (xi, eta, zeta): L_0 is
        # 1 - xi - eta - zeta
        byL = np.zeros((10, 4))
        for i in range(4):
            byL[i, i] = 4.0 * L[i] - 1.0
        for e, (i, j) in enumerate(edges):
            byL[4 + e, i] = 4.0 * L[j]
            byL[4 + e, j] = 4.0 * L[i]
        gradients[p] = byL[:, 1:] - byL[:, [0]]
    _GRADIENTS.append(gradients)
    return gradients


def modalStresses(nodes, tetrahedra, shapes, youngs, poisson, wanted):
    """Each coordinate's stress at the nodes wanted, linear elastic: an
    array [len(wanted), 6, columns] of xx, yy, zz, xy, yz, zx in youngs'
    units per coordinate, each node's the mean of its tetrahedra's there.
    nodes: n x 3; tetrahedra: their nodes, ten each, _EDGE's order; shapes:
    the displacement per coordinate, n x 3 x columns, in the nodes' units --
    the run's mode shapes. Their strain is the small strain, which a rigid
    turn has none of."""
    import numpy as np

    points = np.asarray(nodes, dtype=float)
    tets = np.asarray(tetrahedra, dtype=np.int64).reshape(-1, 10)
    wanted = np.asarray(wanted, dtype=np.int64)
    at = np.zeros(len(points), dtype=bool)
    at[wanted] = True
    tets = tets[at[tets].any(axis=1)]
    gradients = _tet10Gradients()
    # dx_i / dxi_k at each node of each, then dN_a / dx_i
    jacobian = np.einsum("eai,pak->epik", points[tets], gradients)
    byX = np.einsum("epki,pak->epai", np.linalg.inv(jacobian), gradients)
    lam = youngs * poisson / ((1 + poisson) * (1 - 2 * poisson))
    mu = youngs / (2 * (1 + poisson))
    flat = tets.ravel()
    count = np.bincount(flat, minlength=len(points)).astype(float)[wanted]
    columns = shapes.shape[2]
    out = np.zeros((len(wanted), 6, columns))
    for c in range(columns):
        # du_j / dx_i
        h = np.einsum("eaj,epai->epji", shapes[:, :, c][tets], byX)
        trace = h[..., 0, 0] + h[..., 1, 1] + h[..., 2, 2]
        stress = (
            lam * trace + 2 * mu * h[..., 0, 0],
            lam * trace + 2 * mu * h[..., 1, 1],
            lam * trace + 2 * mu * h[..., 2, 2],
            mu * (h[..., 0, 1] + h[..., 1, 0]),
            mu * (h[..., 1, 2] + h[..., 2, 1]),
            mu * (h[..., 2, 0] + h[..., 0, 2]),
        )
        for k, s in enumerate(stress):
            summed = np.bincount(flat, weights=s.ravel(), minlength=len(points))
            out[:, k, c] = summed[wanted] / count
    return out


def vonMisesOf(stress):
    """The von Mises stress of stresses [..., 6], xx yy zz then the shears"""
    import numpy as np

    normal = stress[..., :3]
    shear = stress[..., 3:6]
    mean = normal.mean(axis=-1, keepdims=True)
    return np.sqrt(1.5 * ((normal - mean) ** 2).sum(axis=-1) + 3.0 * (shear**2).sum(axis=-1))


def elasticityOf(flexible):
    """The flexible body's Young's modulus (MPa) and Poisson's ratio, as the
    run takes them; None without them"""
    assembly = _assemblyOf(flexible)
    component = flexible.Component
    if assembly is None or component is None:
        return None
    for entry in assembly.getMassProperties()["Components"]:
        if _same(entry["Object"], component):
            youngs = entry.get("YoungsModulus", 0.0) * 1e-3  # kPa
            if youngs > 0:
                return youngs, entry.get("PoissonRatio", 0.0)
    return None


# The stresses per coordinate last recovered, by what they are of: the
# player's deformations and the Results box's channel take the same
_STRESSES = {}


def _stressesOf(flexible, rest, psi, used, elasticity):
    import hashlib

    key = (
        flexible.FullName,
        hashlib.sha1(psi.tobytes()).hexdigest(),
        len(used),
        elasticity,
    )
    if key not in _STRESSES:
        _STRESSES.clear()
        youngs, poisson = elasticity
        _STRESSES[key] = modalStresses(
            rest, list(flexible.Tetrahedra), psi, youngs, poisson, used
        )
    return _STRESSES[key]


# The colours from no stress to the largest, as the Fem workbench's results
_STOPS = ((0.0, 0.0, 1.0), (0.0, 1.0, 1.0), (0.0, 1.0, 0.0), (1.0, 1.0, 0.0), (1.0, 0.0, 0.0))


def colorsOf(fractions):
    """Each fraction's colour on _STOPS, 0 to 1: n x 3"""
    import numpy as np

    stops = np.array(_STOPS)
    x = np.clip(np.asarray(fractions, dtype=float), 0.0, 1.0) * (len(stops) - 1)
    i = np.minimum(np.floor(x).astype(int), len(stops) - 2)
    f = (x - i)[:, None]
    return stops[i] * (1 - f) + stops[i + 1] * f


class Deformation:
    """A flexible body's deformation as its last run had it: its mesh's
    surface moved by its mode shapes and the run's coordinates at a time,
    scaled up to be seen: its largest over the run a tenth of its size,
    unless show() is given a scale. And its stress there, each mode's
    (modalStresses()) weighed by the coordinates, coloured over it, the
    largest over the run red."""

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
        self.root = None
        self._stresses = None
        self._largest = None
        if not self.valid:
            return
        triangles = surfaceOf(list(flexible.Tetrahedra), nodes)
        used = sorted({n for tri in triangles for n in tri})
        index = {n: i for i, n in enumerate(used)}
        self.triangles = [tuple(index[n] for n in tri) for tri in triangles]
        self.used = used
        self.nodes = np.array([tuple(p) for p in nodes])
        self.rest = self.nodes[used]
        # Every node's, for the stresses; the surface's moved
        self.shapes = np.array(shapes).reshape(len(nodes), 3, columns)
        self.psi = self.shapes[used]  # surface nodes x 3 x columns
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
        self.elasticity = elasticityOf(flexible)

    def stresses(self):
        """Each coordinate's stress at the surface's nodes, MPa
        (modalStresses()); None without the part's elasticity"""
        if self._stresses is None and self.valid and self.elasticity:
            self._stresses = _stressesOf(
                self.flexible, self.nodes, self.shapes, self.used, self.elasticity
            )
        return self._stresses

    def stressAt(self, time):
        """The stress at the surface's nodes at a time, MPa: n x 6, xx yy zz
        xy yz zx in the component's axes; None without it"""
        import numpy as np

        modal = self.stresses()
        return None if modal is None else np.einsum("nkc,c->nk", modal, self.coordinatesAt(time))

    def vonMisesAt(self, time):
        stress = self.stressAt(time)
        return None if stress is None else vonMisesOf(stress)

    def largestStresses(self):
        """Each frame's largest von Mises stress on the surface, MPa; None
        without the stresses"""
        import numpy as np

        modal = self.stresses()
        if modal is None:
            return None
        if self._largest is None:
            largest = []
            # A few frames at a time: all of them at once is the surface's
            # nodes times six times the frames
            for start in range(0, len(self.coordinates), 32):
                block = np.einsum("nkc,fc->fnk", modal, self.coordinates[start : start + 32])
                largest.extend(float(v) for v in vonMisesOf(block).max(axis=1))
            self._largest = largest
        return self._largest

    def peakStress(self):
        """The largest von Mises stress over the run, MPa; 0 without it"""
        largest = self.largestStresses()
        return max(largest) if largest else 0.0

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

    def show(self, time, scale=None, peak=None):
        """Drawn at a time, over the component, which hides; coloured by its
        von Mises stress, red at peak (MPa; its own largest over the run by
        default), where it has its stresses"""
        import FreeCADGui as Gui
        from pivy import coin

        if not self.valid:
            return
        points = self.pointsAt(time, scale)
        vonMises = self.vonMisesAt(time)
        if self.root is None:
            self.root = coin.SoSeparator()
            self.transform = coin.SoTransform()
            # Outward, counterclockwise (surfaceOf()): lit on its outer side
            hints = coin.SoShapeHints()
            hints.vertexOrdering = coin.SoShapeHints.COUNTERCLOCKWISE
            hints.shapeType = coin.SoShapeHints.SOLID
            hints.creaseAngle = 0.5
            self.material = coin.SoMaterial()
            self.material.diffuseColor = (0.45, 0.62, 0.81)
            binding = coin.SoMaterialBinding()
            self.coords = coin.SoCoordinate3()
            faces = coin.SoIndexedFaceSet()
            index = []
            for tri in self.triangles:
                index += [tri[0], tri[1], tri[2], -1]
            faces.coordIndex.setValues(0, len(index), index)
            if vonMises is not None:
                # A colour a node, by its stress
                binding.value = coin.SoMaterialBinding.PER_VERTEX_INDEXED
                faces.materialIndex.setValues(0, len(index), index)
            for node in (self.transform, hints, self.material, binding, self.coords, faces):
                self.root.addChild(node)
            view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
            if view is None:
                self.root = None
                return
            view.getSceneGraph().addChild(self.root)
            self.wasVisible = self.component.ViewObject.Visibility
            self.component.ViewObject.Visibility = False
        self.coords.point.setValues(0, len(points), [tuple(p) for p in points])
        if vonMises is not None:
            peak = self.peakStress() if peak is None else peak
            fractions = vonMises / peak if peak > 0 else vonMises * 0.0
            colors = colorsOf(fractions)
            self.material.diffuseColor.setValues(0, len(colors), [tuple(c) for c in colors])
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


class Legend:
    """The stress colours' scale: from no stress to the largest, in MPa, and
    how many times the bending is drawn, on a light backing of its own so it
    reads on any background. Painted by Qt (paint()), not drawn in the
    scene: over the 3D view's right side as a widget of its own, and into
    Save Animation's frames at the same place (the panel's videoFrame()).
    The render backend's overlay feed, where a view provider's front root
    goes, reaches neither its frames nor a saved picture (2026-11-06)."""

    WIDTH, HEIGHT, MARGIN = 170, 290, 16

    def __init__(self):
        self.widget = None
        self.peak = 0.0
        self.scale = 1.0

    @staticmethod
    def lines(peak, scale):
        """Its words: the title, the bending's scale, then the bar's top,
        middle and bottom"""
        return [
            "von Mises stress, MPa",
            "bending drawn x%.4g" % scale,
            "%.4g" % peak,
            "%.4g" % (peak / 2),
            "0",
        ]

    @classmethod
    def place(cls, width, height):
        """Where it goes in a view width x height: its rectangle, (left,
        top, width, height), at the right, half way down, as tall as the
        view leaves it"""
        tall = max(120, min(cls.HEIGHT, height - 2 * cls.MARGIN))
        left = width - cls.MARGIN - cls.WIDTH
        return left, max(0, (height - tall) // 2), cls.WIDTH, tall

    @classmethod
    def paint(cls, painter, rect, peak, scale):
        """Painted into rect, (left, top, width, height)"""
        from PySide import QtCore, QtGui

        left, top, width, height = rect
        painter.save()
        try:
            painter.setRenderHint(QtGui.QPainter.Antialiasing)
            painter.setRenderHint(QtGui.QPainter.TextAntialiasing)
            painter.setPen(QtGui.QPen(QtGui.QColor(180, 180, 180), 1))
            painter.setBrush(QtGui.QColor(247, 247, 247, 235))
            painter.drawRoundedRect(QtCore.QRectF(left, top, width, height), 4, 4)
            barTop, barBottom = top + 52, top + height - 14
            barLeft = left + 12
            gradient = QtGui.QLinearGradient(0, barBottom, 0, barTop)
            for k, rgb in enumerate(_STOPS):
                gradient.setColorAt(
                    k / (len(_STOPS) - 1), QtGui.QColor.fromRgbF(rgb[0], rgb[1], rgb[2])
                )
            painter.setPen(QtCore.Qt.NoPen)
            painter.setBrush(QtGui.QBrush(gradient))
            painter.drawRect(QtCore.QRectF(barLeft, barTop, 18, barBottom - barTop))
            font = QtGui.QFont("Arial")
            font.setPixelSize(13)
            painter.setFont(font)
            painter.setPen(QtGui.QColor(30, 30, 30))
            words = cls.lines(peak, scale)
            places = [
                (left + 10, top + 20),
                (left + 10, top + 38),
                (barLeft + 26, barTop + 5),
                (barLeft + 26, (barTop + barBottom) / 2 + 5),
                (barLeft + 26, barBottom + 5),
            ]
            for (x, y), text in zip(places, words):
                painter.drawText(QtCore.QPointF(x, y), text)
        finally:
            painter.restore()

    def show(self, peak, scale):
        """Over the active 3D view, for the scale to peak (MPa), the bending
        drawn scale times"""
        import FreeCADGui as Gui

        self.peak, self.scale = peak, scale
        if not self._alive():
            view = Gui.ActiveDocument.ActiveView if Gui.ActiveDocument else None
            if view is None or not hasattr(view, "graphicsView"):
                return
            self.widget = _LegendWidget(view.graphicsView(), self)
        parent = self.widget.parentWidget()
        self.widget.setGeometry(*self.place(parent.width(), parent.height()))
        self.widget.show()
        self.widget.raise_()
        self.widget.update()

    def _alive(self):
        if self.widget is None:
            return False
        try:
            self.widget.isVisible()
            return True
        except RuntimeError:
            # The view it was over closed
            self.widget = None
            return False

    def remove(self):
        if self._alive():
            self.widget.hide()
            self.widget.setParent(None)
            self.widget.deleteLater()
        self.widget = None


def _LegendWidget(parent, legend):
    """The legend over a view: a widget painting it, which the mouse goes
    through, kept at the view's right as the view is resized"""
    from PySide import QtCore, QtGui, QtWidgets

    class Widget(QtWidgets.QWidget):
        def __init__(self):
            super().__init__(parent)
            self.setAttribute(QtCore.Qt.WA_TransparentForMouseEvents)
            self.setAttribute(QtCore.Qt.WA_TranslucentBackground)
            self.setObjectName("FlexibleStressLegend")
            parent.installEventFilter(self)

        def eventFilter(self, watched, event):
            if watched is parent and event.type() == QtCore.QEvent.Resize:
                self.setGeometry(*Legend.place(parent.width(), parent.height()))
            return False

        def paintEvent(self, event):
            painter = QtGui.QPainter(self)
            try:
                Legend.paint(
                    painter, (0, 0, self.width(), self.height()), legend.peak, legend.scale
                )
            finally:
                painter.end()

    return Widget()


class Deformations(list):
    """The deformations of a study's flexible bodies, drawn together: their
    stresses on one scale, the largest of any over the run red, its legend
    beside them"""

    def __init__(self, found=()):
        super().__init__(found)
        self.legend = Legend()

    def peakStress(self):
        return max((d.peakStress() for d in self), default=0.0)

    def legendOf(self):
        """What the legend shows, (peak, scale), or None without stresses"""
        stressed = [d for d in self if d.stresses() is not None]
        if not stressed:
            return None
        return self.peakStress(), min(d.scale for d in stressed)

    def show(self, time):
        peak = self.peakStress()
        for deformation in self:
            deformation.show(time, peak=peak)
        shown = self.legendOf()
        if shown is not None:
            self.legend.show(*shown)

    def remove(self):
        for deformation in self:
            deformation.remove()
        self.legend.remove()
        self.clear()


def deformationsOf(sim, results):
    """The deformations of a study's flexible bodies its last run has"""
    found = Deformations()
    for flexible in flexibleBodiesOf(sim):
        deformation = Deformation(flexible, results)
        if deformation.valid:
            found.append(deformation)
    return found


def stressChannels(assembly, results):
    """The flexible bodies' stresses as channels of the run's results, for
    the Results box: Quantity "FlexibleVonMises", each frame's largest von
    Mises stress on the body's surface (MPa), Object the flexible body"""
    channels = []
    sim = results.get("Simulation")
    if sim is None or not results.get("Times"):
        return channels
    for flexible in flexibleBodiesOf(sim):
        if _assemblyOf(flexible) != assembly:
            continue
        deformation = Deformation(flexible, results)
        largest = deformation.largestStresses() if deformation.valid else None
        if largest is None:
            continue
        channels.append(
            {
                "Name": flexible.Name,
                "Quantity": "FlexibleVonMises",
                "Object": flexible,
                "Values": largest,
                "Unique": True,
            }
        )
    return channels
