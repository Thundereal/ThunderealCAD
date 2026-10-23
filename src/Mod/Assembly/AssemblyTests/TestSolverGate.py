# SPDX-License-Identifier: LGPL-2.1-or-later
"""The identity gate of the solver interface (docs/AssemblySolverInterface.md sec 6.2).

Every case builds a document, hands it to the solver -- an ASMT export, a
static solve, a drag, a simulation -- and checks the result against the
references recorded in SolverGateReference.json:

- the solver input, as OndselSolver's ASMT text (the export, and the model
  of every solve, drag and simulation, which the preference SolverDumpFile
  writes out), by SHA-256;
- the placement of every object after the solve, after each drag step and at
  each simulation frame, within 1e-9;
- the return codes, the solve status and the drag status, exactly.

The references were recorded on 2026-10-23 from the builder that wrote the
ASMT model directly, in the same process as the builder that emits the
solver-neutral model, and the two agreed byte for byte and bit for bit (sec
6.3 step 3; that in-process comparison is commit 09ce3133a3). So a change
that alters what the solver is given, or what it gives back, fails here.

Grounded parts come from an unordered_set of pointers, so their order -- in
the ASMT text, and in OndselSolver's columns -- differs from run to run. The
text is compared with the grounded entries sorted (assembly markers, the
leading grounded parts, the leading grounding joints), everything else in
order; placements with a tolerance, since the column order can move the last
bits.

To re-record after a change that is meant to alter the solver input or its
results: run with ASM_SOLVER_GATE_RECORD=1; the file in the source tree is
rewritten (SolverGateReference.json beside this file, or the path the
variable gives if it is not "1"). The references are this platform's: on
another platform the gate skips.
"""

import hashlib
import json
import os
import sys
import tempfile

import FreeCAD as App
import unittest

import JointObject
import UtilsAssembly

from AssemblyTests.TestJoints import JointTestBase, plc, Z

PREFS = "User parameter:BaseApp/Preferences/Mod/Assembly"
TOL = 1e-9
REFERENCE_NAME = "SolverGateReference.json"


def source_reference_path():
    """The reference file in the source tree (this file is copied into the build)."""
    here = os.path.dirname(os.path.abspath(__file__))
    env = os.environ.get("ASM_SOLVER_GATE_RECORD", "")
    if env and env != "1":
        return env
    # build/<tree>/Mod/Assembly/AssemblyTests -> src/Mod/Assembly/AssemblyTests
    src = os.path.join(App.getHomePath(), "..", "..", "src", "Mod", "Assembly", "AssemblyTests")
    src = os.path.normpath(src)
    if os.path.isdir(src):
        return os.path.join(src, REFERENCE_NAME)
    return os.path.join(here, REFERENCE_NAME)


RECORDING = bool(os.environ.get("ASM_SOLVER_GATE_RECORD"))
_references = None
_recorded = {}


def references():
    global _references
    if _references is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), REFERENCE_NAME)
        with open(path) as f:
            _references = json.load(f)
    return _references


def record(key, value):
    """Adds one case to the reference file (record mode)."""
    path = source_reference_path()
    data = {"platform": sys.platform, "cases": {}}
    if os.path.exists(path):
        with open(path) as f:
            data = json.load(f)
    data["platform"] = sys.platform
    data["cases"][key] = value
    data["cases"] = dict(sorted(data["cases"].items()))
    with open(path, "w", newline="\n") as f:
        json.dump(data, f, indent=1, sort_keys=False)
        f.write("\n")


# ---------------------------------------------------------------- ASMT text


def parse_tree(lines):
    """ASMT text as (line, children) nodes by tab indentation."""
    root = ("", [])
    stack = [(-1, root)]
    for line in lines:
        depth = len(line) - len(line.lstrip("\t"))
        node = (line, [])
        while stack[-1][0] >= depth:
            stack.pop()
        stack[-1][1][1].append(node)
        stack.append((depth, node))
    return root


def flatten(node, out):
    out.append(node[0])
    for child in node[1]:
        flatten(child, out)
    return out


def node_text(node):
    return "\n".join(flatten(node, []))


def child(node, name):
    for c in node[1]:
        if c[0].strip() == name:
            return c
    return None


def sort_leading(children, is_grounded):
    """The leading run of grounded entries sorted; the rest kept in order."""
    n = 0
    while n < len(children) and is_grounded(children[n]):
        n += 1
    return sorted(children[:n], key=node_text) + children[n:]


def canonical_asmt(data):
    """The ASMT text with the grounded entries, whose order varies by run, sorted."""
    text = data.decode("ascii").replace("\r\n", "\n")
    root = parse_tree([l for l in text.split("\n") if l != ""])
    asm = child(root, "Assembly")
    if asm is None:
        return text
    refpoints = child(asm, "RefPoints")
    if refpoints is not None:
        refpoints[1].sort(key=node_text)
    parts = child(asm, "Parts")
    if parts is not None:
        parts[1][:] = sort_leading(parts[1], lambda n: "FixingMarker" in node_text(n))
    sets = child(asm, "ConstraintSets")
    joints = child(sets, "Joints") if sets is not None else None
    if joints is not None:
        joints[1][:] = sort_leading(
            joints[1], lambda n: "/OndselAssembly/marker-" in node_text(n)
        )
    return node_text(root) + "\n"


def asmt_digest(data):
    if data is None:
        return None
    return hashlib.sha256(canonical_asmt(data).encode("ascii")).hexdigest()


def placement_values(p):
    return [p.Base.x, p.Base.y, p.Base.z] + list(p.Rotation.Q)


class SolverGateBase(JointTestBase):
    def setUp(self):
        super().setUp()
        self.p = App.ParamGet(PREFS)
        self._saved = (
            self.p.GetString("SolverDumpFile", ""),
            self.p.GetBool("SolveOnRecompute", True),
        )
        # The gate decides when to solve.
        self.p.SetBool("SolveOnRecompute", False)
        self.tmp = tempfile.mkdtemp(prefix="fc_solver_gate_")
        self.dump_file = os.path.join(self.tmp, "dump.asmt")
        self.p.SetString("SolverDumpFile", self.dump_file)
        self.reset_hooks = []
        if not RECORDING:
            ref = references()
            if ref.get("platform") != sys.platform:
                self.skipTest("references recorded on %s" % ref.get("platform"))

    def tearDown(self):
        dump, solve_on_recompute = self._saved
        if dump:
            self.p.SetString("SolverDumpFile", dump)
        else:
            self.p.RemString("SolverDumpFile")
        self.p.SetBool("SolveOnRecompute", solve_on_recompute)
        for f in os.listdir(self.tmp):
            os.remove(os.path.join(self.tmp, f))
        os.rmdir(self.tmp)
        super().tearDown()

    # ------------------------------------------------------------ plumbing

    def read(self, path):
        with open(path, "rb") as f:
            return f.read()

    def take_dump(self):
        """The ASMT text of the last model handed to the solver, then forget it."""
        if not os.path.exists(self.dump_file):
            return None
        data = self.read(self.dump_file)
        os.remove(self.dump_file)
        return data

    def placed(self):
        """Every object with a Placement, by name."""
        return {
            o.Name: o
            for o in self.doc.Objects
            if "Placement" in o.PropertiesList and o.TypeId != "Assembly::AssemblyObject"
        }

    def snapshot(self):
        return {n: App.Placement(o.Placement) for n, o in self.placed().items()}

    def values(self):
        return {n: placement_values(o.Placement) for n, o in sorted(self.placed().items())}

    def reset(self):
        """Clears what a run leaves behind for the next (see TestSolverGateRigid)."""
        for hook in self.reset_hooks:
            hook()
        self.take_dump()

    def restore(self, snap):
        for n, o in self.placed().items():
            if n in snap and placement_values(o.Placement) != placement_values(snap[n]):
                o.Placement = snap[n]

    @staticmethod
    def status(st):
        return {k: (sorted(v) if isinstance(v, list) else v) for k, v in st.items()}

    # ------------------------------------------------------------ the runs

    def export(self):
        path = os.path.join(self.tmp, "export.asmt")
        self.assembly.exportAsASMT(path)
        text = self.read(path)
        os.remove(path)
        self.take_dump()
        return text

    def solve_once(self):
        r = self.assembly.solve()
        dump = self.take_dump()
        return dict(
            ret=r,
            dump=asmt_digest(dump),
            status=self.status(self.assembly.getSolveStatus()),
            placements=self.values(),
        )

    def drag_once(self, part, targets):
        self.assembly.preDrag([part], 10.0)
        dump = asmt_digest(self.take_dump())
        steps = []
        for t in targets:
            part.Placement = t
            ok = self.assembly.doDragStep()
            steps.append(dict(ok=ok, placements=self.values()))
        self.assembly.postDrag()
        steps.append(dict(ok="post", placements=self.values()))
        return dict(dump=dump, steps=steps, status=self.assembly.getDragStatus())

    def simulate_once(self, sim):
        r = self.assembly.generateSimulation(sim)
        dump = asmt_digest(self.take_dump())
        frames = []
        for i in range(self.assembly.numberOfFrames()):
            self.assembly.updateForFrame(i)
            frames.append(self.values())
        return dict(ret=r, dump=dump, frames=frames)

    # ------------------------------------------------------------ the gate

    def gate(self, drag=None, sim=None, label=""):
        """Hands this document to the solver every way it can be, and checks the record."""
        self.doc.recompute()
        start = self.snapshot()
        result = {}

        text = self.export()
        self.assertGreater(len(text), 500)
        result["export"] = asmt_digest(text)

        if sim is not None:
            self.reset()
            self.restore(start)
            result["simulation"] = self.simulate_once(sim)
            self.assertEqual(result["simulation"]["ret"], 0)
            self.assertGreater(len(result["simulation"]["frames"]), 2)

        self.reset()
        self.restore(start)
        result["solve"] = self.solve_once()
        self.assertIsNotNone(result["solve"]["dump"])
        solved = self.snapshot()

        if drag is not None:
            part, targets = drag
            self.reset()
            self.restore(solved)
            result["drag"] = self.drag_once(part, targets)
            self.assertIsNotNone(result["drag"]["dump"])

        key = self.id().split(".", 2)[-1] + ("[%s]" % label if label else "")
        if RECORDING:
            record(key, result)
            return
        expected = references()["cases"].get(key)
        self.assertIsNotNone(expected, "no reference recorded for %s" % key)
        self.check(expected, json.loads(json.dumps(result)), key, text)

    def check(self, expected, actual, where, export_text):
        """Digests and statuses exactly, placements within TOL."""
        if isinstance(expected, dict):
            self.assertIsInstance(actual, dict, where)
            self.assertEqual(sorted(expected), sorted(actual), "%s: keys" % where)
            for k in expected:
                self.check(expected[k], actual[k], "%s/%s" % (where, k), export_text)
        elif isinstance(expected, list) and expected and isinstance(expected[0], float):
            self.assertEqual(len(expected), len(actual), where)
            for e, a in zip(expected, actual):
                self.assertLessEqual(abs(e - a), TOL, "%s: %r != %r" % (where, actual, expected))
        elif isinstance(expected, list):
            self.assertEqual(len(expected), len(actual), where)
            for i, (e, a) in enumerate(zip(expected, actual)):
                self.check(e, a, "%s[%d]" % (where, i), export_text)
        elif expected != actual and where.endswith("/export"):
            # Keep the text a failure was about, for a diff against a re-record.
            path = os.path.join(tempfile.gettempdir(), "solver_gate_failed.asmt")
            with open(path, "w", newline="\n") as f:
                f.write(canonical_asmt(export_text))
            self.fail("%s: the ASMT export changed (canonical text in %s)" % (where, path))
        else:
            self.assertEqual(expected, actual, where)


class TestSolverGateJoints(SolverGateBase):
    """One assembly per joint type."""

    def test_lower_pairs_and_orientation(self):
        for jtype, props in (
            ("Fixed", {}),
            ("Revolute", {}),
            ("Cylindrical", {}),
            ("Slider", {}),
            ("Ball", {}),
            ("Parallel", {}),
            ("Perpendicular", {}),
            ("Angle", {"Angle": 30.0}),
            ("Angle", {"Angle": 0.0}),  # becomes a parallel joint
        ):
            with self.subTest(joint=jtype, **props):
                self.tearDown()
                self.setUp()
                self.pair_joint(jtype, **props)
                self.gate(label="%s%s" % (jtype, props))

    def gear_pair(self, jtype):
        g = self.box("G")
        a = self.cylinder("A", 10.0, 5.0, plc(0, 0, 10))
        b = self.cylinder("B", 20.0, 5.0, plc(30, 0, 10))
        self.ground(g)
        self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 10), (a, ["Face3"]), plc())
        self.joint("Revolute", (g, ["Face6"]), plc(30, 0, 10), (b, ["Face3"]), plc())
        self.joint(jtype, (a, ["Face1"]), plc(), (b, ["Face1"]), plc(), Distance=10.0, Distance2=20.0)
        return a

    def test_gears_and_belt(self):
        for jtype in ("Gears", "Belt"):
            with self.subTest(joint=jtype):
                self.tearDown()
                self.setUp()
                a = self.gear_pair(jtype)
                self.doc.recompute()
                target = App.Placement(App.Vector(), App.Rotation(Z, 20), App.Vector(0, 0, 10)).multiply(
                    a.Placement
                )
                self.gate(drag=(a, [target]), label=jtype)

    def test_coaxial_gear_with_carrier(self):
        """Two gears on one axis of a common carrier: the gear joint gets marker K."""
        g = self.box("G", size=20.0)
        a = self.cylinder("A", 10.0, 5.0, plc(0, 0, 20))
        b = self.cylinder("B", 5.0, 5.0, plc(0, 0, 25))
        self.ground(g)
        self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 20), (a, ["Face5"]), plc())
        self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 25), (b, ["Face5"]), plc())
        self.joint("Gears", (a, ["Face1"]), plc(), (b, ["Face1"]), plc(), Distance=10.0, Distance2=5.0)
        self.doc.recompute()
        self.assertIn(b"MarkerK", self.export())
        self.gate()

    def test_screw(self):
        g = self.box("G")
        shaft = self.cylinder("Shaft", 3.0, 40.0, plc(0, 0, 10))
        nut = self.box("Nut", plc(10, 10, 10), size=6.0)
        self.ground(g)
        self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 10), (shaft, ["Face3"]), plc())
        self.joint("Slider", (g, ["Face6"]), plc(10, 10, 10), (nut, ["Face5"]), plc())
        # Shaft first: the builder swaps the sides so the sliding one is I.
        self.joint("Screw", (shaft, ["Face1"]), plc(), (nut, ["Face5"]), plc(), Distance=8.0)
        self.gate()

    def test_rack_pinion(self):
        g = self.box("G", size=10.0)
        rack = self.box("Rack", plc(0, 0, 10), size=6.0)
        pinion = self.cylinder("Pinion", 10.0, 5.0, plc(0, 20, 10))
        self.ground(g)
        along_x = App.Rotation(App.Vector(0, 1, 0), 90)
        self.joint(
            "Slider",
            (g, ["Face6"]),
            App.Placement(App.Vector(0, 0, 10), along_x),
            (rack, ["Face5"]),
            App.Placement(App.Vector(), along_x),
        )
        self.joint("Revolute", (g, ["Face6"]), plc(0, 20, 10), (pinion, ["Face3"]), plc())
        self.joint(
            "RackPinion",
            (pinion, ["Face1"]),
            plc(),
            (rack, ["Face5"]),
            App.Placement(App.Vector(), along_x),
            Distance=10.0,
        )
        self.gate()


class TestSolverGateDistance(SolverGateBase):
    """One assembly per Distance sub-type that has a mapping (TestJoints' cases)."""

    D = 3.0
    R_CYL = 4.0
    R_SPH = 5.0

    fixtures = __import__("AssemblyTests.TestJoints", fromlist=["x"]).TestDistanceJoint.fixtures
    distance_joint = __import__("AssemblyTests.TestJoints", fromlist=["x"]).TestDistanceJoint.distance_joint

    CASES = [
        ("PointPoint", ("g", "Vertex8"), ("box_m", "Vertex1"), 3.0),
        ("PointPoint d=0", ("g", "Vertex8"), ("box_m", "Vertex1"), 0.0),
        ("LineLine", ("g", "Edge1"), ("box_m", "Edge1"), 3.0),
        ("LineCircle", ("g", "Edge1"), ("cyl_m", "Edge2"), 3.0),
        ("CircleCircle", ("cyl_g", "Edge2"), ("cyl_m", "Edge2"), 3.0),
        ("PlanePlane", ("g", "Face6"), ("box_m", "Face5"), 3.0),
        ("PlaneCylinder", ("g", "Face6"), ("cyl_m", "Face1"), 3.0),
        ("PlaneSphere", ("g", "Face6"), ("sph_m", "Face1"), 3.0),
        ("PlaneTorus", ("g", "Face6"), ("tor_m", "Face1"), 3.0),
        ("CylinderCylinder", ("cyl_g", "Face1"), ("cyl_m", "Face1"), 3.0),
        ("CylinderSphere", ("cyl_g", "Face1"), ("sph_m", "Face1"), 3.0),
        ("CylinderTorus", ("cyl_g", "Face1"), ("tor_m", "Face1"), 3.0),
        ("TorusTorus", ("tor_g", "Face1"), ("tor_m", "Face1"), 3.0),
        ("TorusSphere", ("tor_g", "Face1"), ("sph_m", "Face1"), 3.0),
        ("SphereSphere", ("sph_g", "Face1"), ("sph_m", "Face1"), 3.0),
        ("PointPlane", ("g", "Face6"), ("box_m", "Vertex1"), 3.0),
        ("PointCylinder", ("cyl_g", "Face1"), ("box_m", "Vertex1"), 3.0),
        ("PointSphere", ("sph_g", "Face1"), ("box_m", "Vertex1"), 3.0),
        ("LinePlane", ("g", "Face6"), ("box_m", "Edge1"), 3.0),
        ("PointLine", ("g", "Edge1"), ("box_m", "Vertex1"), 3.0),
        ("PointCurve", ("cyl_g", "Edge2"), ("box_m", "Vertex1"), 3.0),
        # Reversed picks: the classification swaps the sides in the document.
        ("SpherePlane (swapped)", ("sph_m", "Face1"), ("g", "Face6"), 3.0),
    ]

    def test_distance_subtypes(self):
        for name, ref1, ref2, d in self.CASES:
            with self.subTest(subtype=name):
                self.tearDown()
                self.setUp()
                f = self.fixtures()
                self.distance_joint((f[ref1[0]], ref1[1]), (f[ref2[0]], ref2[1]), d)
                self.gate(label=name)


class TestSolverGateLimits(SolverGateBase):
    """Limits on Slider, Revolute and Cylindrical joints, with a drag that hits them."""

    def test_slider_limits(self):
        g = self.box("G")
        b = self.box("B", plc(0, 0, 10))
        self.ground(g)
        self.joint(
            "Slider", (g, ["Face6"]), plc(5, 5, 10), (b, ["Face5"]), plc(5, 5, 0),
            EnableLengthMin=True, LengthMin=0.0, EnableLengthMax=True, LengthMax=20.0,
        )  # fmt: skip
        targets = [App.Placement(Z * dz, App.Rotation()).multiply(b.Placement) for dz in (10, 30)]
        self.gate(drag=(b, targets))

    def test_revolute_limits(self):
        g = self.box("G")
        b = self.box("B", plc(0, 0, 10))
        self.ground(g)
        self.joint(
            "Revolute", (g, ["Face6"]), plc(5, 5, 10), (b, ["Face5"]), plc(5, 5, 0),
            EnableAngleMin=True, AngleMin=-15.5, EnableAngleMax=True, AngleMax=30.0,
        )  # fmt: skip
        center = App.Vector(5, 5, 10)
        targets = [
            App.Placement(App.Vector(), App.Rotation(Z, a), center).multiply(b.Placement)
            for a in (20, 45)
        ]
        self.gate(drag=(b, targets))

    def test_cylindrical_limits_reversed(self):
        """Min above max: the builder swaps them in the document, then limits."""
        g = self.box("G")
        b = self.box("B", plc(0, 0, 10))
        self.ground(g)
        self.joint(
            "Cylindrical", (g, ["Face6"]), plc(5, 5, 10), (b, ["Face5"]), plc(5, 5, 0),
            EnableLengthMin=True, LengthMin=25.0, EnableLengthMax=True, LengthMax=-5.0,
            EnableAngleMin=True, AngleMin=40.0, EnableAngleMax=True, AngleMax=-10.0,
        )  # fmt: skip
        targets = [App.Placement(Z * 12, App.Rotation(Z, 25)).multiply(b.Placement)]
        self.gate(drag=(b, targets))


class TestSolverGateSimulation(SolverGateBase):
    """One simulation: Angular, Linear, and a Cylindrical driven both ways."""

    def make_simulation(self, motions):
        """A simulation as CommandCreateSimulation makes one, without its GUI module."""
        sim_group = UtilsAssembly.getSimulationGroup(self.assembly)
        sim = sim_group.newObject("App::FeaturePython", "Simulation")
        sim.addExtension("App::GroupExtensionPython")
        for prop, ptype in (
            ("aTimeStart", "App::PropertyTime"),
            ("bTimeEnd", "App::PropertyTime"),
            ("cTimeStepOutput", "App::PropertyTime"),
            ("fGlobalErrorTolerance", "App::PropertyFloat"),
        ):
            sim.addProperty(ptype, prop, "Simulation")
        sim.aTimeStart = 0.0
        sim.bTimeEnd = 1.0
        sim.cTimeStepOutput = 0.1
        sim.fGlobalErrorTolerance = 1.0e-6
        objs = []
        for joint, mtype, formula in motions:
            m = self.assembly.newObject("App::FeaturePython", "Motion")
            m.addProperty("App::PropertyXLinkSub", "Joint", "Motion")
            m.addProperty("App::PropertyString", "Formula", "Motion")
            m.addProperty("App::PropertyEnumeration", "MotionType", "Motion")
            m.MotionType = ["Angular", "Linear"]
            m.MotionType = mtype
            m.Joint = joint
            m.Formula = formula
            objs.append(m)
        sim.Group = objs
        return sim

    def test_simulation(self):
        g = self.box("G", size=20.0)
        a = self.box("A", plc(0, 0, 20))
        b = self.box("B", plc(30, 0, 20))
        c = self.box("C", plc(-30, 0, 20))
        self.ground(g)
        rev = self.joint("Revolute", (g, ["Face6"]), plc(5, 5, 20), (a, ["Face5"]), plc(5, 5, 0))
        sli = self.joint("Slider", (g, ["Face6"]), plc(15, 5, 20), (b, ["Face5"]), plc(5, 5, 0))
        cyl = self.joint("Cylindrical", (g, ["Face6"]), plc(2, 15, 20), (c, ["Face5"]), plc(5, 5, 0))
        self.doc.recompute()

        sim = self.make_simulation(
            [
                (rev, "Angular", "36*time"),
                (sli, "Linear", "5*time + initialValue"),
                (cyl, "Angular", "90*time"),
                (cyl, "Linear", "2*sin(time)"),
            ]
        )
        self.doc.recompute()
        self.gate(sim=sim)


class TestSolverGateBench(SolverGateBase):
    """The bench's chain and star (scripts/assembly-bench/bench.py) at N = 10 and 50."""

    def bench_assembly(self, topo, n):
        box = self.doc.addObject("Part::Box", "Src")
        box.Length = box.Width = box.Height = 10
        self.doc.recompute()
        links = []
        for i in range(n):
            lk = self.assembly.newObject("App::Link", "L%d" % i)
            lk.LinkedObject = box
            lk.Placement = App.Placement(
                App.Vector(12.0 * i + (i % 3), 1.5 * (i % 4), 0.7 * (i % 5)),
                App.Rotation(App.Vector(0, 0, 1), (7 * i) % 20),
            )
            links.append(lk)
        self.ground(links[0])
        for i in range(1, n):
            other = links[i - 1] if topo == "chain" else links[0]
            j = self.jointgroup.newObject("App::FeaturePython", "Joint")
            JointObject.Joint(j, 1 if (i % 2) else 0)  # Revolute / Fixed alternate
            j.Proxy.setJointConnectors(j, [[other, ["Face2", "Face2"]], [links[i], ["Face1", "Face1"]]])
        self.doc.recompute()
        return links

    def run_bench(self, topo, n):
        links = self.bench_assembly(topo, n)
        # The bench drags the last link; at even N it is Fixed-bundled, so
        # drag the last Revolute-joined one (odd index).
        k = n - 1 if (n - 1) % 2 else n - 2
        part = links[k]
        targets = [
            part.Placement.multiply(App.Placement(App.Vector(), App.Rotation(App.Vector(1, 0, 0), 3 * s)))
            for s in (1, 2, 3)
        ]
        self.gate(drag=(part, targets))

    def test_chain_10(self):
        self.run_bench("chain", 10)

    def test_star_10(self):
        self.run_bench("star", 10)

    def test_chain_50(self):
        self.run_bench("chain", 50)

    def test_star_50(self):
        self.run_bench("star", 50)


class TestSolverGateRigid(SolverGateBase):
    """A rigid group, and a drag that bundles Fixed joints."""

    def forget_rigid_cache(self, rg):
        """Empties AssemblyObject's rigid-group placement cache.

        A solve syncs the members of a rigid group to the one that moved since
        the last solve, against placements cached then; a member put back on
        its start counts as moved, so every run would start from a slightly
        different state. A solve with the group suppressed drops the cache, and
        the next solve refills it without moving anything.
        """
        rg.Suppressed = True
        self.assembly.solve()
        rg.Suppressed = False

    def test_rigid_group(self):
        g, b = self.ground_and_mover()
        c = self.box("C", plc(40, 5, 3))
        d = self.box("D", plc(-20, 7, 9))
        self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        rg = self.jointgroup.newObject("App::FeaturePython", "RigidGroupJoint")
        JointObject.RigidGroupJoint(rg, [b, c])
        self.reset_hooks.append(lambda: self.forget_rigid_cache(rg))
        self.joint("Ball", (c, ["Face6"]), plc(5, 5, 10), (d, ["Face5"]), plc(5, 5, 0))
        self.doc.recompute()
        target = App.Placement(App.Vector(), App.Rotation(Z, 15), App.Vector(4, 3, 10)).multiply(
            b.Placement
        )
        self.gate(drag=(b, [target]))

    def test_drag_bundles_fixed(self):
        g, b = self.ground_and_mover()
        c = self.box("C", plc(50, 50, 50))
        j = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        self.joint("Fixed", (b, ["Face6"]), plc(0, 0, 10), (c, ["Face5"]), plc())
        self.doc.recompute()
        self.assembly.solve()
        axis = self.jcs(j, 1)
        targets = [
            App.Placement(App.Vector(), App.Rotation(axis.Rotation.multVec(Z), a), axis.Base).multiply(
                b.Placement
            )
            for a in (20, 40, 60)
        ]
        self.gate(drag=(b, targets))
