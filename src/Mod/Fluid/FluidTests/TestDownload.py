# SPDX-License-Identifier: LGPL-2.1-or-later

# The offer to download OpenFOAM (docs/FluidSimulation.md sec 13, ruling 4):
# what the installer is told to do, and the package's parallel layer mended.
# Nothing is fetched here.

import os
import tempfile
import unittest

import FreeCAD as App

import FluidOpenFoam


class TestDownload(unittest.TestCase):
    def test_folder_is_the_workbench_own(self):
        folder = FluidOpenFoam.downloadFolder()
        self.assertTrue(folder.startswith(App.getUserAppDataDir()))
        self.assertTrue(folder.endswith(os.path.join("Fluid", "openfoam")))
        self.assertIn(folder, FluidOpenFoam.explanation())
        self.assertIn("General Public License", FluidOpenFoam.explanation())

    def test_script_with_a_tool_already_here(self):
        script = FluidOpenFoam.installScript("/data/of", "/usr/bin/mamba")
        lines = script.splitlines()
        self.assertEqual(lines[0], "set -e")
        self.assertNotIn("curl", script)
        self.assertIn(
            "'/usr/bin/mamba' create -y -p '/data/of' -c conda-forge --override-channels "
            "openfoam=2412",
            lines,
        )
        self.assertEqual(lines[-1], "echo DONE")

    def test_script_fetches_and_checks_the_installer(self):
        script = FluidOpenFoam.installScript("/data/of", None)
        lines = script.splitlines()
        tool = os.path.join(FluidOpenFoam.toolsFolder(), "micromamba")
        fetch = [k for k, line in enumerate(lines) if line.startswith("curl ")]
        check = [k for k, line in enumerate(lines) if "sha256sum -c" in line]
        install = [k for k, line in enumerate(lines) if " create -y -p " in line]
        self.assertEqual(len(fetch), 2)
        self.assertEqual(len(check), 1)
        self.assertEqual(len(install), 1)
        # fetched, checked against the published sum, and only then run
        self.assertLess(fetch[-1], check[0])
        self.assertLess(check[0], install[0])
        self.assertTrue(lines[install[0]].startswith("'%s' create" % tool))
        self.assertIn("micromamba-linux-64", lines[fetch[0]])
        self.assertIn("https://", lines[fetch[0]])

    def test_parallel_layer_mended(self):
        folder = tempfile.mkdtemp()
        lib = os.path.join(folder, "lib")
        os.makedirs(os.path.join(lib, "dummy"))
        # nothing shipped: nothing to mend
        self.assertFalse(FluidOpenFoam.fixParallel(folder))
        os.makedirs(os.path.join(lib, "mpich-3.3"))
        self.assertFalse(FluidOpenFoam.fixParallel(folder))
        open(os.path.join(lib, "mpich-3.3", "libPstream.so"), "w").close()
        self.assertTrue(FluidOpenFoam.fixParallel(folder))
        self.assertTrue(os.path.islink(os.path.join(lib, "sys-mpich")))
        self.assertTrue(os.path.exists(os.path.join(lib, "sys-mpich", "libPstream.so")))
        # once is enough
        self.assertFalse(FluidOpenFoam.fixParallel(folder))
