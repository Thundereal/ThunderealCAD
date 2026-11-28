# SPDX-License-Identifier: LGPL-2.1-or-later

"""Temporary folders for the PartDesign tests, removed when a test module is done.

The tests saved their documents straight into the system's temporary folder
or into tempfile.mkdtemp() folders nothing removed: 86 entries left there
by every TestPartDesignApp run (the temp census, 2026-11-28). A test module
makes one of these and names its remove() its tearDownModule:

    _temp = TempFolders()
    tearDownModule = _temp.remove

then _temp.mkdtemp() where it called tempfile.mkdtemp(), and _temp.folder()
-- one folder of the module's own, made on first use -- where it saved into
tempfile.gettempdir(). unittest calls tearDownModule after the module's
classes, their documents closed; a module's classes run apart from each
other (TestPartDesignApp lists them by name) call it more than once, so
folder() makes a new one after a remove()."""

import shutil
import tempfile


class TempFolders:
    def __init__(self, prefix="fc-pdtest-"):
        self.prefix = prefix
        self.made = []
        self.own = None

    def mkdtemp(self, *args, **kwargs):
        path = tempfile.mkdtemp(*args, **kwargs)
        self.made.append(path)
        return path

    def folder(self):
        if self.own is None:
            self.own = self.mkdtemp(prefix=self.prefix)
        return self.own

    def remove(self):
        for path in self.made:
            shutil.rmtree(path, ignore_errors=True)
        self.made = []
        self.own = None
