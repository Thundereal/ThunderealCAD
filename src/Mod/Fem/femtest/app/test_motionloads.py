# SPDX-License-Identifier: LGPL-2.1-or-later
"""Not upstream: the loads a part in motion takes into a static analysis
(the Assembly workbench's loads to FEM, docs/AssemblyDynamics.md D4a) --
a rigid body constraint's distributing coupling, and the body motion
load -- on a steel bar 100 x 10 x 10 of second order tetrahedra, held at
x = 0, solved by CalculiX against the closed forms. The solves need ccx:
the Ccx preferences' binary, or one on PATH; without it they are
skipped."""

import os
import re
import shutil
import tempfile
import unittest

import FreeCAD
import ObjectsFem

E = 210000.0  # MPa
NU = 0.3
RHO = 7.85e-9  # t/mm^3
L = 100.0
A = 10.0
INERTIA = A**4 / 12.0


def ccx_binary():
    binary = (
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Fem/Ccx")
        .GetString("ccxBinaryPath", "")
        .strip()
    )
    return shutil.which(binary) if binary else shutil.which("ccx")


class TestMotionLoads(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument(self.__class__.__name__)
        self.dir = tempfile.mkdtemp(prefix="fem_motionloads_")

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)
        shutil.rmtree(self.dir, ignore_errors=True)

    def bar(self):
        """The bar, its material, mesh, solver and the fixed face at x = 0"""
        box = self.doc.addObject("Part::Box", "Bar")
        box.Length, box.Width, box.Height = L, A, A
        self.doc.recompute()
        self.box = box
        analysis = ObjectsFem.makeAnalysis(self.doc, "Analysis")
        solver = ObjectsFem.makeSolverCalculiXCcxTools(self.doc, "Solver")
        solver.WorkingDir = self.dir
        solver.SplitInputWriter = False
        solver.AnalysisType = "static"
        analysis.addObject(solver)
        material = ObjectsFem.makeMaterialSolid(self.doc, "Steel")
        card = material.Material
        card["Name"] = "Steel"
        card["YoungsModulus"] = "%g MPa" % E
        card["PoissonRatio"] = "%g" % NU
        card["Density"] = "7850 kg/m^3"
        material.Material = card
        analysis.addObject(material)
        fixed = ObjectsFem.makeConstraintFixed(self.doc, "Fixed")
        fixed.References = [(box, "Face1")]
        analysis.addObject(fixed)
        mesh = ObjectsFem.makeMeshNetgenLegacy(self.doc, "Mesh")
        mesh.Shape = box
        mesh.MaxSize = 2.5
        mesh.SecondOrder = True
        mesh.Fineness = 2
        analysis.addObject(mesh)
        self.doc.recompute()
        self.assertGreater(mesh.FemMesh.VolumeCount, 0)
        self.analysis = analysis
        self.solver = solver
        return analysis

    def write(self):
        """The analysis' input; the problems its checks find, the input
        file's text"""
        from femtools import ccxtools

        fea = ccxtools.FemToolsCcx(self.analysis, self.solver, test_mode=True)
        fea.update_objects()
        fea.setup_working_dir(self.dir)
        self.fea = fea
        problems = fea.check_prerequisites()
        if problems:
            return problems, ""
        fea.write_inp_file()
        with open(fea.inp_file_name) as f:
            return "", f.read()

    def solve(self):
        """Solves the input written: the .dat file's text"""
        binary = ccx_binary()
        if not binary:
            self.skipTest("no CalculiX binary")
        self.fea.setup_ccx(binary)
        self.fea.start_ccx()
        dat = os.path.splitext(self.fea.inp_file_name)[0] + ".dat"
        self.assertTrue(os.path.exists(dat), "ccx wrote no results")
        with open(dat) as f:
            return f.read()

    @staticmethod
    def block(text, title):
        """The rows of numbers under the .dat block whose title starts so"""
        rows = []
        found = False
        for line in text.splitlines():
            s = line.strip()
            if s.startswith(title):
                found = True
                rows = []
                continue
            if found:
                parts = s.split()
                if not parts:
                    if rows:
                        break
                    continue
                try:
                    rows.append([float(p) for p in parts])
                except ValueError:
                    break
        return rows

    def coupling(self, **loads):
        rb = ObjectsFem.makeConstraintRigidBody(self.doc, "Tip")
        rb.References = [(self.box, "Face2")]
        rb.ReferenceNode = FreeCAD.Vector(L, A / 2, A / 2)
        rb.Coupling = "Distributing"
        for name, value in loads.items():
            setattr(rb, name, value)
            axis = name[-1]
            kind = "Translational" if name.startswith("Force") else "Rotational"
            setattr(rb, kind + "Mode" + axis, "Load")
        self.analysis.addObject(rb)
        self.doc.recompute()
        return rb

    def tip(self, text):
        """The tip face's mean displacement: a distributing coupling's
        reference node is reported unmoved"""
        rows = self.block(text, "displacements (vx,vy,vz) for set TIP")
        self.assertGreater(len(rows), 10, text[-2000:])
        mean = FreeCAD.Vector()
        for row in rows:
            mean += FreeCAD.Vector(*row[1:4])
        return mean * (1.0 / len(rows))

    def test_distributing_force(self):
        """A force of 100 N across the bar through a distributing coupling
        on its far face: its tip bends F L^3 / (3 E I) + F L / (k G A)
        within 1%, the clamp taking the 100 N; the input has the coupling."""
        self.bar()
        self.coupling(ForceY="100 N")
        problems, inp = self.write()
        self.assertEqual(problems, "")
        self.assertIn("*DISTRIBUTING", inp)
        self.assertRegex(inp, r"\*COUPLING, REF NODE=\d+, SURFACE=Tip_Surface")
        self.assertNotIn("*RIGID BODY", inp)
        text = self.solve()
        shear = 100 * L / (5.0 / 6.0 * E / (2 * (1 + NU)) * A * A)
        want = 100 * L**3 / (3 * E * INERTIA) + shear
        self.assertAlmostEqual(self.tip(text).y, want, delta=0.01 * want)
        total = self.block(text, "total force (fx,fy,fz) for set FIXED")
        self.assertEqual(len(total), 1, text[-2000:])
        self.assertAlmostEqual(total[0][1], -100.0, delta=1e-6)

    def test_distributing_moment(self):
        """A moment of 1e4 N mm about z through the coupling: its tip turns
        up M L^2 / (2 E I) within 1%."""
        self.bar()
        self.coupling(MomentZ="10000 N*mm")
        problems, _ = self.write()
        self.assertEqual(problems, "")
        text = self.solve()
        want = 1e4 * L**2 / (2 * E * INERTIA)
        self.assertAlmostEqual(self.tip(text).y, want, delta=0.01 * want)

    def test_reference_node_fields(self):
        """A coupling's reference node off by round-off, as a point from a
        motion's run is: its coordinates written in fields of at most 20
        characters, CalculiX's, where a float's repr took 23 and the card
        was refused; the bar bends as test_distributing_force's."""
        import re

        self.bar()
        rb = self.coupling(ForceY="100 N")
        rb.ReferenceNode = FreeCAD.Vector(
            L + 1.1102230246251565e-14, A / 2, -1.1050188541972261e-14
        )
        self.doc.recompute()
        problems, inp = self.write()
        self.assertEqual(problems, "")
        node = re.search(r"\*NODE\n(\d+,[^\n]+)\n\*NSET,NSET=Tip_RefNode", inp)
        self.assertIsNotNone(node, inp[-2000:])
        fields = node.group(1).split(",")
        self.assertEqual(len(fields), 4)
        self.assertLessEqual(max(len(f) for f in fields), 20, fields)
        text = self.solve()
        shear = 100 * L / (5.0 / 6.0 * E / (2 * (1 + NU)) * A * A)
        want = 100 * L**3 / (3 * E * INERTIA) + shear
        self.assertAlmostEqual(self.tip(text).y, want, delta=0.01 * want)

    def test_distributing_takes_no_constraint(self):
        """A distributing coupling held in place: the checks refuse it."""
        self.bar()
        rb = self.coupling()
        rb.TranslationalModeX = "Constraint"
        self.doc.recompute()
        problems, _ = self.write()
        self.assertIn("distributing coupling takes loads only", problems)

    def test_body_motion(self):
        """The bar turning about the z axis through its held end's centre,
        at 100 rad/s and 1e4 rad/s^2, with gravity and an acceleration that
        cancel it: the clamp takes rho A L^2 / 2 times omega^2 along -x and
        alpha along +y, and nothing along z."""
        self.bar()
        motion = ObjectsFem.makeConstraintBodyMotion(self.doc, "Motion")
        motion.ReferencePoint = FreeCAD.Vector(0, A / 2, A / 2)
        motion.Gravity = FreeCAD.Vector(0, 0, -9806.65)
        motion.LinearAcceleration = FreeCAD.Vector(0, 0, -9806.65)
        motion.AngularVelocity = FreeCAD.Vector(0, 0, 100)
        motion.AngularAcceleration = FreeCAD.Vector(0, 0, 1e4)
        self.analysis.addObject(motion)
        self.doc.recompute()
        problems, inp = self.write()
        self.assertEqual(problems, "")
        self.assertIn("CENTRIF", inp)
        self.assertNotIn("Eall,GRAV", inp)
        text = self.solve()
        total = self.block(text, "total force (fx,fy,fz) for set FIXED")
        self.assertEqual(len(total), 1, text[-2000:])
        fx, fy, fz = total[0]
        # The share on the clamped nodes themselves is O(h^2): r is small there
        want = RHO * A * A * L * L / 2 * 1e4
        self.assertAlmostEqual(fx, -want, delta=0.01 * want)
        self.assertAlmostEqual(fy, want, delta=0.01 * want)
        self.assertAlmostEqual(fz, 0.0, delta=1e-9 * want)
