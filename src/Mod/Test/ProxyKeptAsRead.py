# SPDX-License-Identifier: LGPL-2.1-or-later
"""A Proxy whose class cannot be made is saved back as it was read.

The smoke corpus, 2026-11-27 (finding A1): a document opened without the
addon its objects need -- Fasteners' FastenersCmd, not installed -- and
saved, lost the addon's objects for good. The restore left the Proxy None
and the save wrote that None as the state, `json="yes"` and null, in place
of the module, the class and the state the file had; installing the addon
afterwards found nothing to restore. Upstream does the same.

PropertyPythonObject now keeps what the file said of such a Proxy and
writes it back unchanged, in whatever form it was read (a file entry of
its own, the encoded value attribute an older file has); a value set on
the property drops it. The log says which class could not be made, not
"unsupported serialisation".

No addon is needed: a saved document's Proxy is renamed to a module that
does not exist, the way a missing addon's is.
"""

import base64
import json
import os
import re
import shutil
import tempfile
import unittest
import zipfile

import FreeCAD

MISSING = "NoSuchAddonForProxyKeptAsRead"
STATE = {"type": "ISO4762", "diameter": "M3", "length": 5.0}


class Keeper:
    """An addon's Proxy: its state is what its objects are"""

    def __init__(self, obj=None, state=None):
        self.state = dict(state or {})
        if obj is not None:
            obj.Proxy = self

    def execute(self, obj):
        pass

    def dumps(self):
        return self.state

    def loads(self, state):
        self.state = state


def proxy_element(path, name):
    """The <Python .../> of `name`'s Proxy in a saved document, and its state"""
    with zipfile.ZipFile(path) as z:
        xml = z.read("Document.xml").decode("utf-8")
        start = xml.index('<Object name="%s"' % name, xml.index("<ObjectData"))
        block = xml[start : xml.index("</Object>", start)]
        prop = block[block.index('<Property name="Proxy"') :]
        element = re.search(r"<Python [^>]*?/>|<Python [^>]*?>", prop).group(0)
        state = None
        value = re.search(r'value="([^"]*)"', element)
        entry = re.search(r'file="([^"]*)"', element)
        if value and 'encoded="yes"' in element:
            state = base64.b64decode(value.group(1)).decode("utf-8")
        elif value:
            state = value.group(1)
        elif entry:
            state = z.read(entry.group(1)).decode("utf-8")
    return element, state


def rewrite(source, target, edit):
    """`source` copied to `target` with Document.xml passed through edit()"""
    with zipfile.ZipFile(source) as zin, zipfile.ZipFile(target, "w") as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "Document.xml":
                data = edit(data.decode("utf-8")).encode("utf-8")
            zout.writestr(item, data)


class Log:
    """The console's errors and warnings while it is attached"""

    def __init__(self):
        self.lines = []

    def __call__(self, notifier, message, level):
        self.lines.append(message)


class ProxyKeptAsReadCases(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="proxykept")
        doc = FreeCAD.newDocument("ProxyKept")
        # what Python's zipfile reads, which zstd it does not
        doc.BlobCompression = "Deflate"
        obj = doc.addObject("App::FeaturePython", "Screw")
        Keeper(obj, STATE)
        doc.recompute()
        self.saved = os.path.join(self.dir, "saved.FCStd")
        doc.saveAs(self.saved)
        FreeCAD.closeDocument(doc.Name)

    def tearDown(self):
        for name in list(FreeCAD.listDocuments()):
            FreeCAD.closeDocument(name)
        shutil.rmtree(self.dir, ignore_errors=True)

    def missing(self, name, edit=None):
        """The saved document with its Proxy's module gone, as an addon's is"""
        path = os.path.join(self.dir, name)

        def rename(xml):
            xml = xml.replace('module="%s"' % __name__, 'module="%s"' % MISSING)
            return edit(xml) if edit else xml

        rewrite(self.saved, path, rename)
        return path

    def openAndResave(self, path, name):
        log = Log()
        FreeCAD.Console.AttachObserver(log)
        try:
            doc = FreeCAD.openDocument(path)
        finally:
            FreeCAD.Console.DetachObserver(log)
        self.assertIsNone(doc.Screw.Proxy, "the module is missing: no Proxy is made")
        resaved = os.path.join(self.dir, name)
        doc.saveAs(resaved)
        FreeCAD.closeDocument(doc.Name)
        return resaved, log

    def assertKept(self, resaved):
        element, state = proxy_element(resaved, "Screw")
        self.assertIn('module="%s"' % MISSING, element)
        self.assertIn('class="Keeper"', element)
        self.assertNotIn('json="yes"', element)
        self.assertEqual(json.loads(state), STATE, element)

    def testTheProxyOfAMissingModuleIsSavedBackAsRead(self):
        self.assertIn('module="%s"' % __name__, proxy_element(self.saved, "Screw")[0])
        resaved, log = self.openAndResave(self.missing("missing.FCStd"), "resaved.FCStd")
        self.assertKept(resaved)
        # The addon back: its object is whole again
        back = os.path.join(self.dir, "back.FCStd")
        rewrite(resaved, back, lambda xml: xml.replace(MISSING, __name__))
        doc = FreeCAD.openDocument(back)
        self.assertIsInstance(doc.Screw.Proxy, Keeper)
        self.assertEqual(doc.Screw.Proxy.state, STATE)

    def testAnOlderFilesEncodedValueIsSavedBackAsRead(self):
        # The form a 0.19 file has (carriage_assembly.FCStd): the state in the
        # element's value attribute, not an archive entry of its own
        encoded = base64.b64encode(json.dumps(STATE).encode("utf-8")).decode("ascii")

        def inline(xml):
            return re.sub(
                r'(<Python [^>]*module="%s" class="Keeper")[^>]*?/>' % MISSING,
                r'\1 value="%s" encoded="yes"/>' % encoded,
                xml,
            )

        older = self.missing("older.FCStd", inline)
        self.assertIn('value="%s"' % encoded, proxy_element(older, "Screw")[0])
        resaved, _ = self.openAndResave(older, "older-resaved.FCStd")
        self.assertKept(resaved)

    def testTheLogNamesTheClassNotTheSerialisation(self):
        _, log = self.openAndResave(self.missing("missing.FCStd"), "resaved.FCStd")
        text = "".join(log.lines)
        self.assertNotIn("unsupported serialisation", text)
        self.assertIn("%s.Keeper could not be made" % MISSING, text)

    def testAValueSetDropsWhatWasKept(self):
        doc = FreeCAD.openDocument(self.missing("missing.FCStd"))
        Keeper(doc.Screw, {"type": "new"})
        resaved = os.path.join(self.dir, "set.FCStd")
        doc.saveAs(resaved)
        element, state = proxy_element(resaved, "Screw")
        self.assertIn('module="%s"' % __name__, element)
        self.assertEqual(json.loads(state), {"type": "new"})

    def testUndoKeepsWhatWasKept(self):
        doc = FreeCAD.openDocument(self.missing("missing.FCStd"))
        doc.UndoMode = 1
        doc.openTransaction("set")
        Keeper(doc.Screw, {"type": "new"})
        doc.commitTransaction()
        doc.undo()
        self.assertIsNone(doc.Screw.Proxy)
        resaved = os.path.join(self.dir, "undone.FCStd")
        doc.saveAs(resaved)
        self.assertKept(resaved)
