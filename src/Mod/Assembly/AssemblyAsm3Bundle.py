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

"""The Assembly3 code bundled for migrating its documents, loaded lazily.

Assembly3 objects are Python features whose proxies live in the package
freecad.asm3. A copy of it is kept in Assembly3/asm3 beside this file
(Assembly3/README.md), on no search path, so FreeCAD's startup scan of the
freecad namespace does not see it and nothing of it loads at startup.

install() appends a finder to the END of sys.meta_path that answers an
import of freecad.asm3 with that copy. An installed Assembly3 add-on is
found by the normal finders first and always wins. So the copy is imported
the first time a document restores an Assembly3 proxy, and only then.

The copy is under a Mod directory, which is what the proxy restore checks
before importing a module a document names (App/PropertyPythonObject.cpp).
"""

import importlib.abc
import importlib.util
import os
import sys

PACKAGE = "freecad.asm3"
BUNDLE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Assembly3", "asm3")


class Asm3Finder(importlib.abc.MetaPathFinder):
    """Finds freecad.asm3 in the bundle when nothing else has"""

    def find_spec(self, fullname, path=None, target=None):
        if fullname != PACKAGE:
            return None
        init = os.path.join(BUNDLE_DIR, "__init__.py")
        if not os.path.isfile(init):
            return None
        # The submodules are found through the package's __path__ as usual
        return importlib.util.spec_from_file_location(
            fullname, init, submodule_search_locations=[BUNDLE_DIR]
        )


def install():
    """Make the bundle importable, once"""
    if not any(isinstance(finder, Asm3Finder) for finder in sys.meta_path):
        sys.meta_path.append(Asm3Finder())


def isBundled():
    """Whether the loaded (or loadable) freecad.asm3 is the bundled copy"""
    module = sys.modules.get(PACKAGE)
    if module is not None:
        location = getattr(module, "__file__", None) or ""
    else:
        spec = importlib.util.find_spec(PACKAGE)
        location = spec.origin if spec is not None and spec.origin else ""
    if not location:
        return False
    return os.path.normcase(os.path.dirname(os.path.abspath(location))) == os.path.normcase(
        BUNDLE_DIR
    )
