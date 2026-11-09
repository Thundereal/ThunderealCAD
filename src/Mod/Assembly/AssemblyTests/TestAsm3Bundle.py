# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
# *                                                                         *
# *   This file is part of FreeCAD.                                         *
# *                                                                         *
# *   FreeCAD is free software: you can redistribute it and/or modify it    *
# *   under the terms of the GNU Lesser General Public License as           *
# *   published by the Free Software Foundation, either version 2.1 of the  *
# *   License, or (at your option) any later version.                       *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful, but        *
# *   WITHOUT ANY WARRANTY; without even the implied warranty of            *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
# *   Lesser General Public License for more details.                       *
# *                                                                         *
# *   You should have received a copy of the GNU Lesser General Public      *
# *   License along with FreeCAD. If not, see                               *
# *   <https://www.gnu.org/licenses/>.                                      *
# *                                                                         *
# ***************************************************************************

"""The Assembly3 code bundled for migration, and its lazy loading.

See AssemblyAsm3Bundle.py and docs/AssemblyObjectModel.md sec 8.
"""

import importlib.machinery
import os
import subprocess
import sys
import tempfile
import unittest

import FreeCAD as App

import AssemblyAsm3Bundle

PACKAGE = AssemblyAsm3Bundle.PACKAGE


def asm3Modules():
    return [name for name in sys.modules if name == PACKAGE or name.startswith(PACKAGE + ".")]


def unloadAsm3():
    for name in asm3Modules():
        del sys.modules[name]


class TestAsm3Bundle(unittest.TestCase):
    def setUp(self):
        if not AssemblyAsm3Bundle.isBundled():
            self.skipTest("an installed Assembly3 add-on takes precedence")

    def tearDown(self):
        for name in list(App.listDocuments()):
            if name.startswith("Asm3Bundle"):
                App.closeDocument(name)

    def testFinderIsInstalledLast(self):
        finders = [f for f in sys.meta_path if isinstance(f, AssemblyAsm3Bundle.Asm3Finder)]
        self.assertEqual(len(finders), 1)
        # After the normal finders, so that an installed add-on wins
        self.assertGreater(
            sys.meta_path.index(finders[0]), sys.meta_path.index(importlib.machinery.PathFinder)
        )

    def testNotLoadedAtStartup(self):
        exe = os.path.basename(sys.executable).lower()
        if "cmd" not in exe:
            self.skipTest("needs the console executable to start a fresh session")
        script = os.path.join(tempfile.mkdtemp(), "probe.py")
        with open(script, "w") as f:
            f.write(
                "import sys\n"
                "print('ASM3=%d' % len([m for m in sys.modules if 'asm3' in m]))\n"
            )
        out = subprocess.run(
            [sys.executable, script], capture_output=True, text=True, timeout=300
        ).stdout
        self.assertIn("ASM3=0", out)

    def testLoadedWhenADocumentNeedsIt(self):
        from freecad.asm3.assembly import AsmPlainGroup

        doc = App.newDocument("Asm3Bundle")
        group = doc.addObject("App::DocumentObjectGroupPython", "Group")
        AsmPlainGroup(group, None)
        path = os.path.join(tempfile.mkdtemp(), "Asm3Bundle.FCStd")
        doc.saveAs(path)
        App.closeDocument(doc.Name)

        unloadAsm3()
        self.assertEqual(asm3Modules(), [])
        doc = App.openDocument(path)
        self.assertIn(PACKAGE + ".assembly", asm3Modules())
        self.assertTrue(AssemblyAsm3Bundle.isBundled())
        proxy = doc.getObject("Group").Proxy
        self.assertEqual(type(proxy).__name__, "AsmPlainGroup")
        self.assertEqual(type(proxy).__module__, PACKAGE + ".assembly")
