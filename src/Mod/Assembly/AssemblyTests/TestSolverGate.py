# SPDX-License-Identifier: LGPL-2.1-or-later
"""The identity gate of the solver interface (docs/AssemblySolverInterface.md sec 6.2).

Every case builds a document, hands it to the solver -- an ASMT export, a
static solve, a drag, a simulation -- and checks the result against the
references recorded in SolverGateReference.json:

- the solver input, as OndselSolver's ASMT text (the export, and the model
  of every solve, drag and simulation, which the preference SolverDumpFile
  writes out), by SHA-256;
- the placement of every object after the solve and after each drag step,
  each value within 1e-9;
- the placement of every object at each simulation frame (frames also played
  in reverse order), within 1e-9 mm in position and 1e-9 rad in rotation;
- the return codes, the solve status and the drag status, exactly.

Since sec 6.12 OndselSolver no longer parses or differentiates a motion's
formula: the adapter hands it the neutral evaluator's value and exact
derivatives as an ExternalFunction. The integrator's velocities and
accelerations then differ from its own by rounding, and so may the frames'
last bits; the ASMT text (which keeps the formula) does not change. The
records were not re-recorded: each simulation case reports the largest
deviation it saw (SIMULATION_DEVIATION, and the report view).

Since sec 6.3 step 7 the solver no longer writes the document: the builder
swaps frames I and J and orders limits in its model. That is the first step
allowed to change results (placements within 1e-9 of the record, statuses --
DoF, redundant and conflicting joints -- equal). The model reproduces the
order the document swap gave exactly, so the ASMT text is still compared
byte for byte; every case also checks that no joint property was written,
and TestSolverGateOrder builds the swapped cases in both orders.

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

Every other registered backend (AssemblyApp.getAvailableSolvers(); since sec
6.13 the Reference backend) runs the same cases through the assembly's
SolverBackend property, in a generated class named after the case's class and
the backend (TestSolverGateJointsReference, ...), and is compared with the
OndselSolver record by the oracle's rules instead (gate_backend): it must
converge wherever OndselSolver did; DoF, redundant, partially redundant,
conflicting and malformed joints equal, except the disagreements listed in
BACKEND_DIFFERENCES with their diagnosis (asserted as they are, so a fix on
either side shows); placements within 1e-7 when OndselSolver's DoF is 0, and
otherwise every joint satisfied to 1e-9 by the neutral checker
(getSolverResiduals) with the solution no farther from the start, in the
Reference's metric, than OndselSolver's; drag steps that OndselSolver took,
taken, each leaving every joint satisfied; simulation frames within 1e-7
(BACKEND_SIMULATION has the cases where OndselSolver's recorded frames are
wrong; there the frames are checked against OndselSolver run again with an
output step ten times finer). A backend may refuse a case it cannot express
(sec 6.15: Chrono has no link for some joint kinds); BACKEND_REFUSED lists
each such case with the reason, and the gate asserts the refusal as recorded:
the solve fails and moves nothing, a drag takes no step, a simulation fails,
while OndselSolver's record solved it. The OndselSolver runs and their
records are not touched by any of this. BACKEND_DEVIATION collects the
numbers.

To re-record after a change that is meant to alter the solver input or its
results: run with ASM_SOLVER_GATE_RECORD=1; the file in the source tree is
rewritten (SolverGateReference.json beside this file, or the path the
variable gives if it is not "1"). The references are this platform's: on
another platform the gate skips.
"""

import hashlib
import json
import math
import os
import sys
import tempfile

import FreeCAD as App
import Part
import unittest

import JointObject
import UtilsAssembly

from AssemblyTests.TestJoints import JointTestBase, plc, Z

PREFS = "User parameter:BaseApp/Preferences/Mod/Assembly"
TOL = 1e-9
REFERENCE_NAME = "SolverGateReference.json"

# The largest deviation of a simulation frame from its record, by case:
# {key: (mm, rad)} (sec 6.12).
SIMULATION_DEVIATION = {}

# What another backend's run of each case gave against the OndselSolver
# record (gate_backend): {(backend, key): {...}}.
BACKEND_DEVIATION = {}

# Where a backend disagrees with OndselSolver's record on a status, and why.
# {(backend, key): ({field: the backend's value}, diagnosis)}; the fields are
# getSolveStatus() keys and "ret". Asserted as recorded, so that a fix on
# either side fails here and gets looked at.
BACKEND_DIFFERENCES = {}

# Simulation cases whose recorded OndselSolver frames are wrong: {key:
# diagnosis}. The backend's frames are checked against OndselSolver run again
# with a tenth of the output step instead (check_simulation_finer). Empty
# since OndselSolver holds an angular motion's angle with its turns counted
# (sec 6.16); the four simulation cases were here while it held the sine.
BACKEND_SIMULATION = {}

# Cases a backend refuses because it cannot express a joint of them:
# {(backend, key): diagnosis}. Asserted as recorded (gate_backend), so that a
# backend that learns the joint fails here and the entry gets looked at.
BACKEND_REFUSED = {}

BACKEND_DIFFERENCES.update(
    {
        ("Reference", "TestSolverGateDistance.test_distance_subtypes[PointPoint d=0]"): (
            {"ret": 0},
            "the builder makes a Spherical joint of it; OndselSolver does not converge "
            "from this start (the box 50 mm from the point) and leaves the box where it "
            "was, the Reference puts the vertices together",
        ),
        ("Reference", "TestSolverGateRigid.test_drag_bundles_fixed"): (
            {"DoF": 1, "Conflicting": [], "Redundant": [], "PartiallyRedundant": []},
            "C starts 50 mm from where the Fixed joint puts it; OndselSolver sets "
            "equations of the Fixed and the Revolute aside as redundant (it removes "
            "them at the first singular matrix it meets on the way) and solves without "
            "them, leaving the Revolute 0.80 and the Fixed 0.92 off (the neutral "
            "checker) and both reported conflicting. Nothing is redundant in a "
            "revolute with a part fixed to its arm: the Reference satisfies both and "
            "counts DoF 1",
        ),
        ("Reference", "TestSolverGateRoleMappings.test_distance_subtypes[LineTorus]"): (
            {"DoF": 45, "Conflicting": [], "Redundant": [], "PartiallyRedundant": []},
            "OndselSolver leaves the box where it was, the distance 0.73 mm off (the "
            "neutral checker), and reports it conflicting; the Reference satisfies the "
            "LineLineDistance and counts DoF 45, as both count for the other "
            "LineLineDistance role mappings (LineCone, ConeCone, ConeTorus)",
        ),
    }
)


BACKEND_DIFFERENCES.update(
    {
        ("Chrono", "TestSolverGateDistance.test_distance_subtypes[PointPoint d=0]"): (
            {"ret": 0},
            "the builder makes a Spherical joint of it; OndselSolver does not converge "
            "from this start (the box 50 mm from the point) and leaves the box where it "
            "was, Chrono's ChLinkMateSpherical puts the vertices together",
        ),
        ("Chrono", "TestSolverGateRigid.test_drag_bundles_fixed"): (
            {"DoF": 1, "Conflicting": [], "Redundant": [], "PartiallyRedundant": []},
            "as the Reference: C starts 50 mm from where the Fixed joint puts it; "
            "OndselSolver sets equations of the Fixed and the Revolute aside on the way "
            "and leaves both unsatisfied and conflicting; Chrono's Newton iteration "
            "satisfies both (ChLinkMateFix, ChLinkMateRevolute) and its Jacobian's rank "
            "gives DoF 1, nothing redundant",
        ),
    }
)

# The Chrono backend (sec 6.15) has no link for some joint kinds; a model with
# one is refused as a whole (Chrono::refusal() gives each reason).
_CHRONO_PLD = (
    "the builder maps it to a PointLineDistance, and Chrono has no link for the "
    "distance of a point from a line"
)
_CHRONO_LLD = (
    "the builder maps it to a LineLineDistance, and Chrono has no link that holds "
    "two lines parallel at a distance"
)
BACKEND_REFUSED.update(
    {("Chrono", "TestSolverGateDistance.test_distance_subtypes[%s]" % k): why
     for k, why in (
         ("LineLine", _CHRONO_LLD),
         ("LineCircle", _CHRONO_LLD),
         ("CircleCircle", _CHRONO_LLD),
         ("CylinderCylinder", _CHRONO_LLD),
         ("CylinderTorus", _CHRONO_LLD),
         ("CylinderSphere", _CHRONO_PLD),
         ("TorusSphere", _CHRONO_PLD),
         ("PointCylinder", _CHRONO_PLD),
         ("PointLine", _CHRONO_PLD),
     )}
)  # fmt: skip
BACKEND_REFUSED.update(
    {("Chrono", "TestSolverGateDistanceMapped.test_distance_subtypes[%s]" % k): why
     for k, why in (
         ("LineCylinder", _CHRONO_LLD),
         ("LineSphere", _CHRONO_PLD),
         ("PointTorus", _CHRONO_PLD),
     )}
)  # fmt: skip
BACKEND_REFUSED.update(
    {("Chrono", "TestSolverGateRoleMappings.test_distance_subtypes[%s]" % k): why
     for k, why in (
         ("CylinderCone", _CHRONO_LLD),
         ("ConeCone", _CHRONO_LLD),
         ("ConeTorus", _CHRONO_LLD),
         ("LineCone", _CHRONO_LLD),
         ("LineTorus", _CHRONO_LLD),
         ("CurveCylinder", _CHRONO_LLD),
         ("CurveCone", _CHRONO_LLD),
         ("CurveTorus", _CHRONO_LLD),
         ("CylinderCylinder B-spline", _CHRONO_LLD),
         ("LineCircle B-spline", _CHRONO_LLD),
         ("ConeSphere", _CHRONO_PLD),
         ("PointCone", _CHRONO_PLD),
         ("PointTorus", _CHRONO_PLD),
         ("CurveSphere", _CHRONO_PLD),
     )}
)  # fmt: skip
BACKEND_REFUSED.update(
    {
        ("Chrono", "TestSolverGateJoints.test_lower_pairs_and_orientation[Angle{'Angle': 30.0}]"): (
            "an AngleZ of 30 degrees: Chrono has no link for an angle between two axes "
            "other than a right angle (ChLinkMateOrthogonal) or none (ChLinkMateParallel)"
        ),
        ("Chrono", "TestSolverGateJoints.test_gears_and_belt[Belt]"): (
            "a belt: ChLinkLockPulley couples its pulleys at velocity level only (its "
            "phase correction is unfinished in Chrono 10), and ChLinkLockGear's "
            "epicyclic form divides by zero for equal radii"
        ),
        ("Chrono", "TestSolverGateJoints.test_coaxial_gear_with_carrier"): (
            "a CoaxialGear: ChLinkLockGear measures each gear's turn from the line "
            "between the two shafts and has no carrier frame"
        ),
        ("Chrono", "TestSolverGateJoints.test_rack_pinion"): (
            "ChLinkMateRackPinion recomputes its contact frames at every update and "
            "holds the rack against the pinion at velocity level only"
        ),
        ("Chrono", "TestSolverGateJoints.test_screw"): (
            "ChLinkLockScrew is a whole screw pair (five equations, the axes held "
            "together too); the neutral Screw is the coupling alone"
        ),
    }
)


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


def placement_deviation(expected, actual):
    """The distance (mm) and the rotation angle (rad) between two placement_values().

    The angle from the vector part of the relative quaternion, not its scalar:
    acos near 1 cannot see 1e-9 rad.
    """
    e = App.Placement(App.Vector(*expected[:3]), App.Rotation(*expected[3:]))
    a = App.Placement(App.Vector(*actual[:3]), App.Rotation(*actual[3:]))
    distance = (a.Base - e.Base).Length
    x, y, z, w = (a.Rotation.inverted() * e.Rotation).Q
    angle = 2.0 * math.atan2(math.sqrt(x * x + y * y + z * z), abs(w))
    return distance, angle


class SolverGateBase(JointTestBase):
    # The backend the cases run with; the generated classes at the end of
    # this file set another, and GATE_CLASS to the class whose records they
    # are compared with.
    BACKEND = "Ondsel"
    GATE_CLASS = None

    def setUp(self):
        super().setUp()
        if self.BACKEND != "Ondsel":
            self.assembly.SolverBackend = self.BACKEND
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
        self.doc_alias = None
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

    def digest(self, data):
        """asmt_digest(), with this document's name read as the one of the case checked against.

        The ASMT text carries the document's name (each test class has its
        own document), so a case built in another class to check against a
        recorded one is renamed to it first.
        """
        if data is not None and self.doc_alias:
            data = data.replace(
                (self.doc.Name + "#").encode("ascii"), (self.doc_alias + "#").encode("ascii")
            )
        return asmt_digest(data)

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
            dump=self.digest(dump),
            status=self.status(self.assembly.getSolveStatus()),
            placements=self.values(),
        )

    def drag_once(self, part, targets):
        self.assembly.preDrag([part], 10.0)
        dump = self.digest(self.take_dump())
        steps = []
        for t in targets:
            part.Placement = t
            ok = self.assembly.doDragStep()
            steps.append(dict(ok=ok, placements=self.values()))
        self.assembly.postDrag()
        steps.append(dict(ok="post", placements=self.values()))
        return dict(dump=dump, steps=steps, status=self.assembly.getDragStatus())

    def simulate_once(self, sim, frame_order=None):
        r = self.assembly.generateSimulation(sim)
        dump = self.digest(self.take_dump())
        frames = []
        n = self.assembly.numberOfFrames()
        for i in frame_order(n) if frame_order else range(n):
            self.assembly.updateForFrame(i)
            frames.append(self.values())
        return dict(ret=r, dump=dump, frames=frames)

    # ------------------------------------------------------------ the gate

    JOINT_PROPS = (
        "Reference1", "Reference2", "Placement1", "Placement2", "Offset1", "Offset2",
        "Detach1", "Detach2", "LengthMin", "LengthMax", "AngleMin", "AngleMax",
        "EnableLengthMin", "EnableLengthMax", "EnableAngleMin", "EnableAngleMax",
    )  # fmt: skip

    def joint_state(self):
        """What a solve must leave alone on every joint (sec 6.3 step 7)."""

        def value(v):
            if isinstance(v, App.Placement):
                return placement_values(v)
            if hasattr(v, "Value"):
                return v.Value
            if isinstance(v, tuple) and len(v) == 2 and hasattr(v[0], "Name"):
                return (v[0].Name, list(v[1]))
            return v

        state = {}
        for j in self.jointgroup.Group:
            for p in self.JOINT_PROPS:
                if p in j.PropertiesList:
                    state["%s.%s" % (j.Name, p)] = value(getattr(j, p))
        return state

    def gate(self, drag=None, sim=None, label="", frame_order=None, key=None):
        """Hands this document to the solver every way it can be, and checks the record.

        key names the recorded case to check against (by default this test's
        own id and label): a case built in the other order checks against the
        record of the first.
        """
        if key:
            self.doc_alias = key.split(".")[0]
        if self.BACKEND != "Ondsel":
            key = key or "%s.%s%s" % (
                self.GATE_CLASS,
                self._testMethodName,
                "[%s]" % label if label else "",
            )
            return self.gate_backend(key, drag, sim, frame_order)
        result, text = self.run_all(drag, sim, frame_order)
        key = key or self.id().split(".", 2)[-1] + ("[%s]" % label if label else "")
        if RECORDING:
            record(key, result)
            return
        expected = references()["cases"].get(key)
        self.assertIsNotNone(expected, "no reference recorded for %s" % key)
        self.deviation_key = key
        self.check(expected, json.loads(json.dumps(result)), key, text)
        if key in SIMULATION_DEVIATION:
            mm, rad = SIMULATION_DEVIATION[key]
            App.Console.PrintMessage(
                "Solver gate: %s: simulation frames within %.3g mm, %.3g rad of the record\n"
                % (key, mm, rad)
            )

    def run_all(self, drag=None, sim=None, frame_order=None):
        """The export, simulation, solve and drag of gate(); the result and the export text.

        None of them may write the joints: their references, placements and
        limits are the same after as before.
        """
        if self.BACKEND != "Ondsel":
            return self.run_all_backend(drag, sim, frame_order), b""
        self.doc.recompute()
        start = self.snapshot()
        joints_before = self.joint_state()
        result = {}

        text = self.export()
        self.assertGreater(len(text), 500)
        result["export"] = self.digest(text)

        if sim is not None:
            self.reset()
            self.restore(start)
            result["simulation"] = self.simulate_once(sim, frame_order)
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

        self.assertEqual(joints_before, self.joint_state(), "the solver wrote a joint")
        return result, text

    # ------------------------------------------------------------ other backends

    def assert_satisfied(self, where):
        """Every joint satisfied to TOL at the solver's poses (the neutral checker)."""
        residuals = self.assembly.getSolverResiduals()
        worst = max(residuals.values(), default=0.0)
        bad = {k: v for k, v in residuals.items() if not v <= TOL}
        self.assertFalse(bad, "%s: joints not satisfied: %r" % (where, bad))
        return worst

    def run_all_backend(self, drag=None, sim=None, frame_order=None):
        """run_all() for a backend other than OndselSolver: no export, no dumps.

        The result has the record's shape without the digests. Every joint
        is checked satisfied after the solve and after each drag step taken.
        """
        self.doc.recompute()
        start = self.snapshot()
        self.backend_start = self.values()
        self.backend_start_snapshot = start
        joints_before = self.joint_state()
        result = {}
        worst = 0.0

        if sim is not None:
            self.reset()
            self.restore(start)
            r = self.assembly.generateSimulation(sim)
            frames = []
            n = self.assembly.numberOfFrames()
            for i in frame_order(n) if frame_order else range(n):
                self.assembly.updateForFrame(i)
                frames.append(self.values())
            result["simulation"] = dict(ret=r, frames=frames)

        self.reset()
        self.restore(start)
        r = self.assembly.solve()
        result["solve"] = dict(
            ret=r,
            status=self.status(self.assembly.getSolveStatus()),
            placements=self.values(),
        )
        if r == 0:
            worst = max(worst, self.assert_satisfied("solve"))
        solved = self.snapshot()

        if drag is not None:
            part, targets = drag
            self.reset()
            self.restore(solved)
            self.assembly.preDrag([part], 10.0)
            steps = []
            for i, t in enumerate(targets):
                part.Placement = t
                ok = self.assembly.doDragStep()
                steps.append(dict(ok=ok, placements=self.values()))
                if ok:
                    worst = max(worst, self.assert_satisfied("drag step %d" % i))
            self.assembly.postDrag()
            steps.append(dict(ok="post", placements=self.values()))
            worst = max(worst, self.assert_satisfied("after the drag"))
            result["drag"] = dict(steps=steps, status=self.assembly.getDragStatus())

        self.backend_residual = worst
        self.assertEqual(joints_before, self.joint_state(), "the solver wrote a joint")
        return json.loads(json.dumps(result))

    @staticmethod
    def moves(start, placements):
        """sum |dx|^2 + |dtheta|^2 / 4 over the objects: the Reference's metric."""
        total = 0.0
        for name, before in start.items():
            if name not in placements:
                continue
            distance, angle = placement_deviation(before, placements[name])
            total += distance * distance + 0.25 * angle * angle
        return total

    def gate_backend(self, key, drag=None, sim=None, frame_order=None):
        """The case run with this class's backend, against OndselSolver's record."""
        expected = references()["cases"].get(key)
        self.assertIsNotNone(expected, "no reference recorded for %s" % key)
        actual = self.run_all_backend(drag, sim, frame_order)
        where = "%s with %s" % (key, self.BACKEND)
        stats = BACKEND_DEVIATION.setdefault((self.BACKEND, key), {})
        stats["residual"] = self.backend_residual

        refused = BACKEND_REFUSED.get((self.BACKEND, key))
        if refused:
            self.check_refused(where, refused, expected, actual)
            stats["refused"] = refused
            return

        # Statuses: equal, or the recorded disagreement.
        exp_solve, act_solve = expected["solve"], actual["solve"]
        got = dict(act_solve["status"], ret=act_solve["ret"])
        want = dict(exp_solve["status"], ret=exp_solve["ret"])
        known, diagnosis = BACKEND_DIFFERENCES.get((self.BACKEND, key), ({}, ""))
        for field in sorted(set(want) | set(got)):
            if field == "SolverStatus":
                continue
            if field in known:
                self.assertEqual(
                    got.get(field), known[field], "%s: %s (%s)" % (where, field, diagnosis)
                )
                self.assertNotEqual(
                    got.get(field), want.get(field), "%s: %s agrees now" % (where, field)
                )
                stats.setdefault("differs", []).append(field)
            else:
                self.assertEqual(got.get(field), want.get(field), "%s: %s" % (where, field))

        # Placements: equal when nothing is free; otherwise as close to the
        # start as OndselSolver's, in the Reference's metric.
        if exp_solve["ret"] == 0 and act_solve["ret"] == 0:
            if exp_solve["status"].get("DoF") == 0:
                dev = 0.0
                for name, values in exp_solve["placements"].items():
                    distance, angle = placement_deviation(values, act_solve["placements"][name])
                    dev = max(dev, distance, angle)
                stats["placement"] = dev
                self.assertLessEqual(dev, 1e-7, "%s: placements" % where)
            else:
                mine = self.moves(self.backend_start, act_solve["placements"])
                theirs = self.moves(self.backend_start, exp_solve["placements"])
                stats["moves"] = (mine, theirs)
                # A solution OndselSolver left with a conflict is no bound.
                if not exp_solve["status"].get("Conflicting"):
                    self.assertLessEqual(
                        mine, theirs * (1.0 + 1e-6) + 1e-9, "%s: farther from the start" % where
                    )

        # Drag: the steps OndselSolver took, taken; the counts equal.
        if "drag" in expected:
            exp_steps = [s["ok"] for s in expected["drag"]["steps"]]
            act_steps = [s["ok"] for s in actual["drag"]["steps"]]
            self.assertEqual(act_steps, exp_steps, "%s: drag steps" % where)
            self.assertEqual(
                actual["drag"]["status"], expected["drag"]["status"], "%s: drag status" % where
            )

        # Simulation: the frames within 1e-7, or the formulas.
        if "simulation" in expected:
            self.assertEqual(actual["simulation"]["ret"], expected["simulation"]["ret"], where)
            exp_frames = expected["simulation"]["frames"]
            act_frames = actual["simulation"]["frames"]
            # OndselSolver reports the start twice (sec 6.9); the first, or in
            # reverse order the last, is the extra one.
            if len(act_frames) == len(exp_frames) - 1:
                reverse = "reverse" in key
                exp_frames = exp_frames[:-1] if reverse else exp_frames[1:]
            self.assertEqual(len(act_frames), len(exp_frames), "%s: frames" % where)
            dev = 0.0
            for e, a in zip(exp_frames, act_frames):
                for name in e:
                    distance, angle = placement_deviation(e[name], a[name])
                    dev = max(dev, distance, angle)
            stats["simulation"] = dev
            if key in BACKEND_SIMULATION:
                self.assertGreater(dev, 1e-3, "%s: OndselSolver's frames agree now" % where)
                stats["finer"] = self.check_simulation_finer(where, sim, frame_order, act_frames)
            else:
                self.assertLessEqual(dev, 1e-7, "%s: simulation frames" % where)

    def check_refused(self, where, refused, expected, actual):
        """A case the backend refuses (BACKEND_REFUSED): nothing solved, nothing moved.

        OndselSolver's record solved it, so it is a disagreement; the
        backend's solve returns -1 and leaves every object where it was, a
        drag takes none of its steps, a simulation fails.
        """
        self.assertEqual(expected["solve"]["ret"], 0, "%s: OndselSolver's record" % where)
        self.assertEqual(
            actual["solve"]["ret"], -1, "%s: solved, though refused (%s)" % (where, refused)
        )
        self.assertEqual(actual["solve"]["placements"], self.backend_start, where)
        if "drag" in actual:
            steps = [s["ok"] for s in actual["drag"]["steps"]]
            self.assertEqual(steps, [False] * (len(steps) - 1) + ["post"], where)
        if "simulation" in actual:
            self.assertNotEqual(actual["simulation"]["ret"], 0, where)

    def check_simulation_finer(self, where, sim, frame_order, frames):
        """The frames against OndselSolver's with an output step ten times finer.

        A driver that turns less than a quarter turn between two output
        times keeps OndselSolver on the right root (BACKEND_SIMULATION says
        why it matters); every tenth of those frames is an output time of the
        case. The largest deviation.
        """
        step = sim.cTimeStepOutput.Value
        backend = self.assembly.SolverBackend
        self.reset()
        self.restore(self.backend_start_snapshot)
        try:
            sim.cTimeStepOutput = step / 10.0
            self.assembly.SolverBackend = "Ondsel"
            self.assertEqual(self.assembly.generateSimulation(sim), 0, where)
            finer = []
            for i in range(self.assembly.numberOfFrames()):
                self.assembly.updateForFrame(i)
                finer.append(self.values())
        finally:
            sim.cTimeStepOutput = step
            self.assembly.SolverBackend = backend
        # OndselSolver reports the start twice; then every tenth frame.
        finer = finer[1::10]
        if frame_order:
            finer = [finer[i] for i in frame_order(len(finer))]
        self.assertEqual(len(finer), len(frames), where)
        dev = 0.0
        for e, a in zip(finer, frames):
            for name in e:
                distance, angle = placement_deviation(e[name], a[name])
                dev = max(dev, distance, angle)
        self.assertLessEqual(
            dev, 1e-7, "%s: frames against OndselSolver at a tenth of the step" % where
        )
        return dev

    def check(self, expected, actual, where, export_text):
        """Digests and statuses exactly, placements within TOL."""
        if isinstance(expected, dict):
            self.assertIsInstance(actual, dict, where)
            self.assertEqual(sorted(expected), sorted(actual), "%s: keys" % where)
            for k in expected:
                self.check(expected[k], actual[k], "%s/%s" % (where, k), export_text)
        elif isinstance(expected, list) and expected and isinstance(expected[0], float):
            self.assertEqual(len(expected), len(actual), where)
            if "/simulation/frames[" in where:
                # sec 6.12: position and rotation, within TOL mm and TOL rad.
                distance, angle = placement_deviation(expected, actual)
                key = self.deviation_key
                mm, rad = SIMULATION_DEVIATION.get(key, (0.0, 0.0))
                SIMULATION_DEVIATION[key] = (max(mm, distance), max(rad, angle))
                self.assertLessEqual(distance, TOL, "%s: %r != %r" % (where, actual, expected))
                self.assertLessEqual(angle, TOL, "%s: %r != %r" % (where, actual, expected))
                return
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
        if self.BACKEND == "Ondsel":
            self.assertIn(b"MarkerK", self.export())
        self.gate()

    def test_screw(self):
        g = self.box("G")
        shaft = self.cylinder("Shaft", 3.0, 40.0, plc(0, 0, 10))
        nut = self.box("Nut", plc(10, 10, 10), size=6.0)
        self.ground(g)
        self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 10), (shaft, ["Face3"]), plc())
        self.joint("Slider", (g, ["Face6"]), plc(10, 10, 10), (nut, ["Face5"]), plc())
        # Shaft first: the builder swaps the sides in the model so the sliding one is I.
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
        # Reversed picks. The joint task's creation puts the connected side
        # first (ensureUnconnectedIsSecondRef), so this is PlaneSphere again,
        # recorded as such; the builder's own swaps are TestSolverGateOrder's.
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


class TestSolverGateDistanceMapped(SolverGateBase):
    """Audit sec 8.1 decision 7: the sub-types that used to fall to Planar(d).

    LineCylinder, LineSphere and CurvePlane have a mapping of their own
    (docs/AssemblySolverInterface.md sec 6.8). CurvePlane on a circle was
    recorded before the mapping went in and is what the default case gave;
    LineCylinder, LineSphere and CurvePlane on an ellipse were recorded
    after it (TestJoints.TestDistanceMapped checks them on the geometry).
    PlaneCone and PointTorus stood for the refused sub-types, solved as
    Planar(d); since the role mappings (sec 6.11) they have one, and were
    recorded again from it (TestJoints.TestDistanceRoleMappings checks them
    on the geometry).
    """

    D = TestSolverGateDistance.D
    R_CYL = TestSolverGateDistance.R_CYL
    R_SPH = TestSolverGateDistance.R_SPH
    fixtures = TestSolverGateDistance.fixtures
    distance_joint = TestSolverGateDistance.distance_joint

    def mapped_fixtures(self):
        """TestDistanceJoint's fixtures, a cone and an ellipse on the moving side."""
        f = self.fixtures()
        start = f["box_m"].Placement
        cone_m = self.assembly.newObject("Part::Cone", "ConeM")
        cone_m.Radius1 = 5.0
        cone_m.Radius2 = 2.0
        cone_m.Height = 8.0
        cone_m.Placement = start
        ell_m = self.assembly.newObject("Part::Ellipse", "EllM")
        ell_m.MajorRadius = 6.0
        ell_m.MinorRadius = 3.0
        ell_m.Placement = start
        self.doc.recompute()
        f.update(cone_m=cone_m, ell_m=ell_m)
        return f

    CASES = [
        ("LineCylinder", ("cyl_g", "Face1"), ("box_m", "Edge1"), 3.0),
        ("LineSphere", ("sph_g", "Face1"), ("box_m", "Edge1"), 3.0),
        ("CurvePlane", ("g", "Face6"), ("cyl_m", "Edge2"), 3.0),
        ("CurvePlane ellipse", ("g", "Face6"), ("ell_m", "Edge1"), 3.0),
        ("PlaneCone", ("g", "Face6"), ("cone_m", "Face1"), 3.0),
        ("PointTorus", ("tor_g", "Face1"), ("box_m", "Vertex1"), 3.0),
    ]

    def test_distance_subtypes(self):
        for name, ref1, ref2, d in self.CASES:
            with self.subTest(subtype=name):
                self.tearDown()
                self.setUp()
                f = self.mapped_fixtures()
                self.distance_joint((f[ref1[0]], ref1[1]), (f[ref2[0]], ref2[1]), d)
                self.gate(label=name)


class TestSolverGateRoleMappings(SolverGateBase):
    """Sec 6.11: the role mappings, the geometry recognition, and what stays refused.

    Recorded from the build that made them (TestJoints.TestDistanceRoleMappings
    and TestElementRecognition check them on the geometry). The refused cases
    are joints already in a document: Planar(d), as the default case always
    gave.
    """

    D = TestSolverGateDistance.D
    R_CYL = TestSolverGateDistance.R_CYL
    R_SPH = TestSolverGateDistance.R_SPH
    distance_joint = TestSolverGateDistance.distance_joint

    def fixtures(self):
        from AssemblyTests.TestJoints import TestDistanceRoleMappings, freeform, nurbs_copy

        f = TestDistanceRoleMappings.fixtures(self)
        start = f["box_m"].Placement
        ell_m = self.assembly.newObject("Part::Ellipse", "EllM")
        ell_m.MajorRadius = 6.0
        ell_m.MinorRadius = 3.0
        ell_m.Placement = start
        f["ell_m"] = ell_m
        f.update(freeform(self.assembly, placement=start))
        f["ncyl_m"] = nurbs_copy(
            self.assembly, "NCylM", Part.makeCylinder(TestSolverGateDistance.R_CYL, 10.0), start
        )
        self.doc.recompute()
        return f

    CASES = [
        ("PlaneCone", ("g", "Face6"), ("cone_m", "Face1"), 3.0),
        ("CylinderCone", ("cyl_g", "Face1"), ("cone_m", "Face1"), 3.0),
        ("ConeCone", ("cone_g", "Face1"), ("cone_m", "Face1"), 3.0),
        ("ConeTorus", ("cone_g", "Face1"), ("tor_m", "Face1"), 3.0),
        ("ConeSphere", ("cone_g", "Face1"), ("sph_m", "Face1"), 3.0),
        ("PointCone", ("cone_g", "Face1"), ("box_m", "Vertex1"), 3.0),
        ("PointTorus", ("tor_g", "Face1"), ("box_m", "Vertex1"), 3.0),
        ("LineCone", ("cone_g", "Face1"), ("box_m", "Edge1"), 3.0),
        ("LineTorus", ("tor_g", "Face1"), ("box_m", "Edge1"), 3.0),
        ("CurveCylinder", ("cyl_g", "Face1"), ("cyl_m", "Edge2"), 3.0),
        ("CurveSphere", ("sph_g", "Face1"), ("cyl_m", "Edge2"), 3.0),
        ("CurveCone", ("cone_g", "Face1"), ("cyl_m", "Edge2"), 3.0),
        ("CurveTorus", ("tor_g", "Face1"), ("cyl_m", "Edge2"), 3.0),
        ("CylinderCylinder B-spline", ("cyl_g", "Face1"), ("ncyl_m", "Face1"), 3.0),
        ("LineCircle B-spline", ("g", "Edge1"), ("ncyl_m", "Edge1"), 3.0),
        ("CurveCylinder ellipse (refused, planar)", ("cyl_g", "Face1"), ("ell_m", "Edge1"), 3.0),
        ("Other freeform (refused, planar)", ("g", "Face6"), ("free", "Face1"), 3.0),
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
        """Min above max: the builder orders them in the model; the document keeps them."""
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


class TestSolverGateOrder(SolverGateBase):
    """The builder's swaps live in the model (sec 6.3 step 7).

    Screw and RackPinion want the sliding side as frame I, the Distance
    sub-types want their sides in a set order, and a limit wants min below
    max. Upstream writes that order into the document during the solve; here
    the builder swaps its frames and orders its limits in the model, and the
    document keeps what the user picked. Each case picks both orders: the one
    the recorded reference was made from and the other, and both give the
    same solver input (byte for byte) and the same placements. run_all checks
    that the joints are not written.
    """

    def test_screw_slider_first(self):
        """test_screw with the nut (the sliding side) as Reference1: no swap needed."""
        g = self.box("G")
        shaft = self.cylinder("Shaft", 3.0, 40.0, plc(0, 0, 10))
        nut = self.box("Nut", plc(10, 10, 10), size=6.0)
        self.ground(g)
        self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 10), (shaft, ["Face3"]), plc())
        self.joint("Slider", (g, ["Face6"]), plc(10, 10, 10), (nut, ["Face5"]), plc())
        self.joint("Screw", (nut, ["Face5"]), plc(), (shaft, ["Face1"]), plc(), Distance=8.0)
        self.gate(key="TestSolverGateJoints.test_screw")

    def test_rack_pinion_rack_first(self):
        """test_rack_pinion with the rack as Reference1: no swap needed."""
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
            (rack, ["Face5"]),
            App.Placement(App.Vector(), along_x),
            (pinion, ["Face1"]),
            plc(),
            Distance=10.0,
        )
        self.gate(key="TestSolverGateJoints.test_rack_pinion")

    def test_cylindrical_limits_ordered(self):
        """test_cylindrical_limits_reversed with min below max: the same limits."""
        g = self.box("G")
        b = self.box("B", plc(0, 0, 10))
        self.ground(g)
        self.joint(
            "Cylindrical", (g, ["Face6"]), plc(5, 5, 10), (b, ["Face5"]), plc(5, 5, 0),
            EnableLengthMin=True, LengthMin=-5.0, EnableLengthMax=True, LengthMax=25.0,
            EnableAngleMin=True, AngleMin=-10.0, EnableAngleMax=True, AngleMax=40.0,
        )  # fmt: skip
        targets = [App.Placement(Z * 12, App.Rotation(Z, 25)).multiply(b.Placement)]
        self.gate(drag=(b, targets), key="TestSolverGateLimits.test_cylindrical_limits_reversed")

    # ------------------------------------------------------------ Distance

    R_CYL = TestSolverGateDistance.R_CYL
    R_SPH = TestSolverGateDistance.R_SPH
    fixtures = TestSolverGateDistance.fixtures

    def raw_distance_joint(self, ref1, ref2, d, offset1=None, offset2=None):
        """A Distance joint with its references set as given.

        Not through setJointConnectors, which puts the connected side first
        (and solves); the JCS are still placed on the elements and offset as
        the joint's own recompute does it.
        """
        j = self.jointgroup.newObject("App::FeaturePython", "Distance")
        JointObject.Joint(j, JointObject.JointTypes.index("Distance"))
        j.Distance = d
        # Offsets before references: with both references set, an offset
        # change solves the assembly.
        if offset1 is not None:
            j.Offset1 = offset1
        if offset2 is not None:
            j.Offset2 = offset2
        j.Detach1 = True
        j.Detach2 = True
        j.Reference1 = [ref1[0], [ref1[1], ref1[1]]]
        j.Reference2 = [ref2[0], [ref2[1], ref2[1]]]
        j.Detach1 = False
        j.Detach2 = False
        j.recompute()
        return j

    def both_orders(self, name, ref1, ref2, d=3.0, offset=None, fixtures=None):
        """The case with ref1 first, then with ref2 first; the second checked against the first.

        offset, when given, goes on ref1's side in both: Offset1 the first
        time, Offset2 the second.
        """
        results = []
        for swapped in (False, True):
            self.tearDown()
            self.setUp()
            f = (fixtures or type(self).fixtures)(self)
            r1 = (f[ref1[0]], ref1[1])
            r2 = (f[ref2[0]], ref2[1])
            if swapped:
                self.raw_distance_joint(r2, r1, d, offset2=offset)
            else:
                self.raw_distance_joint(r1, r2, d, offset1=offset)
            result, text = self.run_all()
            if self.BACKEND != "Ondsel" and self.refused_subtype(name):
                # Refused in both orders (BACKEND_REFUSED has the reason).
                self.assertEqual(result["solve"]["ret"], -1, name)
                continue
            self.assertEqual(result["solve"]["ret"], 0, name)
            results.append((json.loads(json.dumps(result)), text))
        if not results:
            return
        (first, _), (second, text) = results
        self.check(first, second, "%s, other order" % name, text)

    def refused_subtype(self, name):
        """Whether this class's backend refuses the Distance sub-type 'name' (BACKEND_REFUSED)."""
        suffix = "[%s]" % name
        return any(
            backend == self.BACKEND and key.endswith(suffix) for backend, key in BACKEND_REFUSED
        )

    # (sub-type, the side it wants first, the other); the builder swaps when
    # the other comes first.
    SWAPPED_DISTANCE = [
        ("LineCircle", ("g", "Edge1"), ("cyl_m", "Edge2")),
        ("PlaneCylinder", ("g", "Face6"), ("cyl_m", "Face1")),
        ("PlaneSphere", ("g", "Face6"), ("sph_m", "Face1")),
        ("PlaneTorus", ("g", "Face6"), ("tor_m", "Face1")),
        ("CylinderSphere", ("cyl_g", "Face1"), ("sph_m", "Face1")),
        ("CylinderTorus", ("cyl_g", "Face1"), ("tor_m", "Face1")),
        ("TorusSphere", ("tor_g", "Face1"), ("sph_m", "Face1")),
        ("PointPlane", ("g", "Face6"), ("box_m", "Vertex1")),
        ("PointCylinder", ("cyl_g", "Face1"), ("box_m", "Vertex1")),
        ("PointSphere", ("sph_g", "Face1"), ("box_m", "Vertex1")),
        ("LinePlane", ("g", "Face6"), ("box_m", "Edge1")),
        ("PointLine", ("g", "Edge1"), ("box_m", "Vertex1")),
        ("PointCurve", ("cyl_g", "Edge2"), ("box_m", "Vertex1")),
        # Decision 7's mapped sub-types (LineSphere also swaps in the case).
        ("LineCylinder", ("cyl_g", "Face1"), ("box_m", "Edge1")),
        ("LineSphere", ("sph_g", "Face1"), ("box_m", "Edge1")),
        ("CurvePlane", ("g", "Face6"), ("cyl_m", "Edge2")),
    ]

    def test_distance_both_orders(self):
        for name, ref1, ref2 in self.SWAPPED_DISTANCE:
            with self.subTest(subtype=name):
                self.both_orders(name, ref1, ref2)

    # Sec 6.11's role mappings, the side each wants first (CurveSphere also
    # swaps in the case), on TestSolverGateRoleMappings' fixtures.
    SWAPPED_ROLES = [
        ("PlaneCone", ("g", "Face6"), ("cone_m", "Face1")),
        ("CylinderCone", ("cyl_g", "Face1"), ("cone_m", "Face1")),
        ("ConeTorus", ("cone_g", "Face1"), ("tor_m", "Face1")),
        ("ConeSphere", ("cone_g", "Face1"), ("sph_m", "Face1")),
        ("PointCone", ("cone_g", "Face1"), ("box_m", "Vertex1")),
        ("PointTorus", ("tor_g", "Face1"), ("box_m", "Vertex1")),
        ("LineTorus", ("tor_g", "Face1"), ("box_m", "Edge1")),
        ("CurveSphere", ("sph_g", "Face1"), ("cyl_m", "Edge2")),
        ("CurveTorus", ("tor_g", "Face1"), ("cyl_m", "Edge2")),
    ]

    def test_role_mappings_both_orders(self):
        for name, ref1, ref2 in self.SWAPPED_ROLES:
            with self.subTest(subtype=name):
                self.both_orders(
                    name, ref1, ref2, fixtures=TestSolverGateRoleMappings.fixtures
                )

    def test_offset_stays_with_its_reference(self):
        """An offset moves the JCS of the side it belongs to, whichever side is frame I.

        Upstream's swap in the document moved the references and placements
        but not Offset1/Offset2, so the joint's next recompute put each offset
        on the other side.
        """
        offset = App.Placement(App.Vector(2, 1, 1.5), App.Rotation(Z, 15))
        self.both_orders("PointPlane, offset on the plane", ("g", "Face6"), ("box_m", "Vertex1"), offset=offset)


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

    def simulation_doc(self):
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
        return sim

    def test_simulation(self):
        self.gate(sim=self.simulation_doc())

    # The frames below were recorded on 2026-10-23 from playback of the stored
    # trajectory (sec 6.3 step 6), after the same process had played every
    # frame of these cases through the ASMT model as well and found them equal
    # bit for bit.

    def test_simulation_reverse_playback(self):
        """Frames played last to first: playback is random access."""
        sim = self.simulation_doc()
        self.gate(sim=sim, frame_order=lambda n: list(reversed(range(n))), label="reverse")

    def test_simulation_gears(self):
        """A driven gear turns the other: a body moved by a joint, not a driver."""
        g = self.box("G")
        a = self.cylinder("A", 10.0, 5.0, plc(0, 0, 10))
        b = self.cylinder("B", 20.0, 5.0, plc(30, 0, 10))
        self.ground(g)
        rev = self.joint("Revolute", (g, ["Face6"]), plc(0, 0, 10), (a, ["Face3"]), plc())
        self.joint("Revolute", (g, ["Face6"]), plc(30, 0, 10), (b, ["Face3"]), plc())
        self.joint("Gears", (a, ["Face1"]), plc(), (b, ["Face1"]), plc(), Distance=10.0, Distance2=20.0)
        self.doc.recompute()
        sim = self.make_simulation([(rev, "Angular", "90*time")])
        self.doc.recompute()
        self.gate(sim=sim)

    def test_simulation_rigid_group(self):
        """A driven part in a rigid group: playback places the other member by its offset."""
        g, b = self.ground_and_mover()
        c = self.box("C", plc(40, 5, 3))
        rev = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        rg = self.jointgroup.newObject("App::FeaturePython", "RigidGroupJoint")
        JointObject.RigidGroupJoint(rg, [b, c])

        def reset_rigid():
            # A simulation uses the rigid groups the last solve or export found
            # and does not look again; the suppressed solve that empties the
            # placement cache also drops them, so an export finds them again
            # (it touches neither the cache nor a placement).
            TestSolverGateRigid.forget_rigid_cache(self, rg)
            self.export()

        self.reset_hooks.append(reset_rigid)
        self.doc.recompute()
        sim = self.make_simulation([(rev, "Angular", "45*time")])
        self.doc.recompute()
        self.gate(sim=sim)

    def test_playback_survives_a_solve(self):
        """The trajectory is kept: a solve between two playbacks changes nothing.

        Not a recorded case -- the ASMT model played whatever the last solve
        left in it, so there is no old result to compare with.
        """
        sim = self.simulation_doc()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        n = self.assembly.numberOfFrames()
        self.assertGreater(n, 2)

        def play():
            frames = []
            for i in range(n):
                self.assembly.updateForFrame(i)
                frames.append(self.values())
            return frames

        first = play()
        self.assertEqual(self.assembly.solve(), 0)
        self.assertEqual(self.assembly.numberOfFrames(), n)
        self.assertEqual(play(), first)


    def test_bad_formula_refused(self):
        """A formula outside the grammar (sec 4.6.1) stops the run before any solve.

        Not a recorded case. OndselSolver ran the first one as 36*time (it
        dropped the text it could not read) and failed inside the solver on
        the other two.
        """
        g, b = self.ground_and_mover()
        rev = self.joint("Revolute", (g, ["Face6"]), self.JCS_G, (b, ["Face5"]), self.JCS_B)
        self.doc.recompute()
        for formula in ("36*time)", "log(time)", "2*-time"):
            sim = self.make_simulation([(rev, "Angular", formula)])
            self.doc.recompute()
            self.assertEqual(self.assembly.generateSimulation(sim), -1, formula)
            self.assertEqual(self.assembly.numberOfFrames(), 0, formula)
        sim = self.make_simulation([(rev, "Angular", "36*time")])
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assertGreater(self.assembly.numberOfFrames(), 2)

    def test_exp_motion(self):
        """An exp motion (sec 4.6.1) slides the part as the formula says.

        Not a recorded case: before exp, ln, sqrt and tan were added the
        builder refused it. The help text's C*exp(time/TIMEC), C = 2 mm and
        TIMEC = 0.8 s, on a slider whose JCS start coincident.
        """
        import math

        g = self.box("G", size=20.0)
        b = self.box("B", plc(30, 0, 20))
        self.ground(g)
        sli = self.joint("Slider", (g, ["Face6"]), plc(15, 5, 20), (b, ["Face5"]), plc(5, 5, 0))
        self.doc.recompute()
        sim = self.make_simulation([(sli, "Linear", "2*exp(time/0.8)")])
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        n = self.assembly.numberOfFrames()
        frames = []
        for i in range(n):
            self.assembly.updateForFrame(i)
            frames.append(App.Vector(b.Placement.Base))
        # The last frame is bTimeEnd, one per cTimeStepOutput before it.
        self.assertGreaterEqual(n, 11, frames)
        for k in (0, 3, 5, 10):
            t = 1.0 - 0.1 * k
            p = frames[n - 1 - k]
            self.assertAlmostEqual(p.z - 20.0, 2.0 * math.exp(t / 0.8), 6, (t, frames))
            self.assertAlmostEqual(p.x, 10.0, 6, (t, frames))
            self.assertAlmostEqual(p.y, 0.0, 6, (t, frames))

    # sec 6.12: formulas whose derivatives OndselSolver did not have (abs, asin,
    # arctan) or got wrong (a power of time to a power of time, a piecewise of
    # 2*time). Not recorded cases; each runs and follows its formula.

    def follow(self, mtype, formula, f):
        """Runs one motion; the driven part at every frame against f(time).

        Linear: a slider whose JCS start coincident, the part's z rises by
        f(t) mm. Angular: a revolute about z through (5, 5), the part turns
        by f(t) rad.
        """
        g = self.box("G", size=20.0)
        self.ground(g)
        if mtype == "Linear":
            b = self.box("B", plc(30, 0, 20))
            j = self.joint("Slider", (g, ["Face6"]), plc(15, 5, 20), (b, ["Face5"]), plc(5, 5, 0))
        else:
            b = self.box("A", plc(0, 0, 20))
            j = self.joint("Revolute", (g, ["Face6"]), plc(5, 5, 20), (b, ["Face5"]), plc(5, 5, 0))
        self.doc.recompute()
        sim = self.make_simulation([(j, mtype, formula)])
        self.doc.recompute()
        self.assertEqual(self.assembly.generateSimulation(sim), 0, formula)
        n = self.assembly.numberOfFrames()
        frames = []
        for i in range(n):
            self.assembly.updateForFrame(i)
            frames.append(App.Placement(b.Placement))
        # The last frame is bTimeEnd, one per cTimeStepOutput before it.
        self.assertGreaterEqual(n, 11, formula)
        for k in range(11):
            t = 1.0 - 0.1 * k
            p = frames[n - 1 - k]
            where = (formula, t, p)
            if mtype == "Linear":
                self.assertAlmostEqual(p.Base.z - 20.0, f(t), 6, where)
                self.assertAlmostEqual(p.Base.x, 10.0, 6, where)
                self.assertAlmostEqual(p.Base.y, 0.0, 6, where)
            else:
                x, y, z, w = p.Rotation.Q
                self.assertAlmostEqual(x, 0.0, 9, where)
                self.assertAlmostEqual(y, 0.0, 9, where)
                angle = 2.0 * math.atan2(z, w)
                turn = math.remainder(angle - f(t), 2.0 * math.pi)
                self.assertAlmostEqual(turn, 0.0, 6, where)
                # The JCS stays on the axis: (5, 5, 20) is fixed.
                axis = p.multVec(App.Vector(5, 5, 0))
                self.assertAlmostEqual((axis - App.Vector(5, 5, 20)).Length, 0.0, 6, where)

    def test_abs_motion(self):
        self.follow("Linear", "4*abs(time - 0.45)", lambda t: 4.0 * abs(t - 0.45))

    def test_asin_motion(self):
        self.follow("Linear", "10*asin(time/2)", lambda t: 10.0 * math.asin(t / 2.0))

    def test_arcsin_angular_motion(self):
        self.follow("Angular", "arcsin(0.9*time)", lambda t: math.asin(0.9 * t))

    def test_arctan_motion(self):
        # The help text's smooth step, 10 + (20 - 10)*(1/2 + arctan(50*(time - 0.5))/pi),
        # as an angle: a turn of 1 rad around t = 0.5.
        self.follow(
            "Angular",
            "(1/2) + (1/pi)*arctan(10*(time - 0.5))",
            lambda t: 0.5 + math.atan(10.0 * (t - 0.5)) / math.pi,
        )

    def test_power_of_time_motion(self):
        self.follow("Linear", "(time + 1)^time", lambda t: (t + 1.0) ** t)

    def test_piecewise_of_2time_motion(self):
        # x = 2*time selects the piece; the pieces are functions of time.
        def f(t):
            x = 2.0 * t
            if x < 0.6:
                return t
            if x < 1.4:
                return 2.0 * t - 0.3
            return 0.75 + 0.5 * t

        self.follow(
            "Angular",
            "piecewise(2*time, functions(time, 2*time - 0.3, 0.75 + 0.5*time), transitions(0.6, 1.4))",
            f,
        )

    def test_rampstep_of_2time_motion(self):
        def ramp(t):
            x = 2.0 * t
            if x < 0.4:
                return 1.0
            if x >= 1.6:
                return 3.0
            return 1.0 + (x - 0.4) * (3.0 - 1.0) / (1.6 - 0.4)

        self.follow("Linear", "rampstep(2*time, 0.4, 1, 1.6, 3)", ramp)

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


# ---------------------------------------------------------------- other backends

GATE_CLASSES = (
    TestSolverGateJoints,
    TestSolverGateDistance,
    TestSolverGateDistanceMapped,
    TestSolverGateRoleMappings,
    TestSolverGateLimits,
    TestSolverGateOrder,
    TestSolverGateSimulation,
    TestSolverGateBench,
    TestSolverGateRigid,
)


def backend_gate_classes():
    """Every gate class once per registered backend other than OndselSolver.

    {name: class}; TestAssemblyWorkbench puts them in its namespace, so the
    suite runs them.
    """
    import AssemblyApp

    classes = {}
    for backend in AssemblyApp.getAvailableSolvers():
        if backend == "Ondsel":
            continue
        for cls in GATE_CLASSES:
            name = cls.__name__ + backend
            classes[name] = type(
                name,
                (cls,),
                {
                    "BACKEND": backend,
                    "GATE_CLASS": cls.__name__,
                    "__module__": __name__,
                    "__doc__": "%s with the %s backend, against the OndselSolver records."
                    % (cls.__name__, backend),
                },
            )
    return classes


BACKEND_GATE_CLASSES = backend_gate_classes()
globals().update(BACKEND_GATE_CLASSES)
