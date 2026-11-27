#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the sandbox GUI gates (docs/Sandbox.md 7.9) under the FreeCAD GUI.

The Gui binary has no -t mode, so the unittest modules that need a GUI
(SandboxGui) run from this script, which FreeCAD executes at startup:

    cd build/conda-relwithdebinfo-801
    env -u WAYLAND_DISPLAY QT_QPA_PLATFORM=xcb FREECAD_USER_HOME=/tmp/fchome \
      FCX_PYODIDE=$HOME/.local/share/FreeCAD/Pyodide/314.0.6 \
      xvfb-run -a -s "-screen 0 1280x800x24" timeout -k 5 600 \
      ~/works/sw/fcad/.conda/run.sh ./bin/FreeCAD ~/works/sw/fcad/scripts/sandbox-gui-gate.py

(FCX_PYODIDE because that user home has no runtime of its own; without
it every sandbox case skips and the verdict is still OK -- read the
case lines.)

The result goes to $SANDBOX_GUI_GATE_RESULT (default: sandbox-gui-gate.txt
in the user data directory) and the last line is `RESULT OK` or `RESULT
FAILED`; judge by that file, not by the exit code (the GUI's exit is not
clean on every box).  $SANDBOX_GUI_GATE_MODULES selects the modules
(comma-separated, default SandboxGui,SandboxWidgets,SandboxForms,
SandboxNative,SandboxPanels,SandboxSelection,SandboxSessionDoc,
SandboxHostFiles,SandboxProxyImport,ViewProviderHooks,
ViewProviderChain,PartGroupView,TreeEntries,PartDesignTests.TestFilletPanel,
SandboxToolBarMirror,SandboxPanelMirror; the two mirror gates run last
and need no guest).  SandboxMirrorBench is a
measurement (docs/Sandbox.md 8.4), not in the default list: name it
alone to run it.

It runs in a FRESH user home by default: the process started with the
script starts the same binary again, with this script, in an empty
temporary FREECAD_USER_HOME, and waits for it. A profile carries what
runs before left: the transient folders of a process killed at its
timeout open Document Recovery at the next start, and the gate's cases
then ran nested inside that modal's loop, where a case's own modal never
returned (SandboxPanelMirror.test_nested_messagebox hung on it,
2026-11-28). FCX_PYODIDE is set for the run to the newest runtime in the
starting profile when it is not set already, and the result goes where
it would have gone. SANDBOX_GUI_GATE_PROFILE=keep runs in the profile the
process was started with instead -- a profile named with
FREECAD_USER_HOME, to measure in it.

And a modal already up when the cases are to start -- that recovery
dialog, a migration question -- is rejected (a rejected Document
Recovery keeps its data) and written into the result as a FAIL line:
the run says it, rather than passing over it.
"""

import faulthandler
import io
import os
import shutil
import subprocess
import sys
import tempfile
import traceback
import unittest

# FreeCAD's sys.stderr is its console (no fileno): the real one for a crash
try:
    faulthandler.enable(file=sys.__stderr__)
except Exception:
    pass


class _Tee(io.StringIO):
    """The runner's stream: kept for the result file, echoed to stderr
    as it goes, so a crash mid-suite still shows which case ran."""

    def write(self, s):
        sys.stderr.write(s)
        sys.stderr.flush()
        return io.StringIO.write(self, s)

import FreeCAD
import FreeCADGui


class _EagerResult(unittest.TextTestResult):
    """A failure's traceback printed as it happens, not at the end: a
    crash later in the run would otherwise take it along."""

    def addError(self, test, err):
        unittest.TextTestResult.addError(self, test, err)
        self.stream.writeln(self._exc_info_to_string(err, test))
        self.stream.flush()

    def addFailure(self, test, err):
        unittest.TextTestResult.addFailure(self, test, err)
        self.stream.writeln(self._exc_info_to_string(err, test))
        self.stream.flush()


def main():
    default_modules = ("SandboxGui,SandboxWidgets,SandboxForms,SandboxNative,SandboxPanels,"
                       "SandboxSelection,SandboxSessionDoc,SandboxHostFiles,"
                       "SandboxProxyImport,"
                       "ViewProviderHooks,ViewProviderChain,PartGroupView,TreeEntries,"
                       "PartDesignTests.TestFilletPanel,"
                       "SandboxToolBarMirror,"
                       "SandboxPanelMirror")
    modules = os.environ.get("SANDBOX_GUI_GATE_MODULES", default_modules).split(",")
    out = os.environ.get("SANDBOX_GUI_GATE_RESULT") or os.path.join(
        FreeCAD.getUserAppDataDir(), "sandbox-gui-gate.txt"
    )
    stream = _Tee()
    verdict = "FAILED"
    for line in state["modals"]:
        stream.write("FAIL: a modal was up as the cases were to start: %s\n" % line)
    try:
        suite = unittest.TestSuite()
        for name in modules:
            suite.addTests(unittest.defaultTestLoader.loadTestsFromName(name.strip()))
        result = unittest.TextTestRunner(stream=stream, verbosity=2,
                                         resultclass=_EagerResult).run(suite)
        if result.wasSuccessful() and not state["modals"]:
            verdict = "OK"
    except Exception:
        stream.write(traceback.format_exc())
    text = stream.getvalue()
    with open(out, "w") as f:
        f.write(text)
        f.write("\nRESULT %s\n" % verdict)
    sys.stderr.write(text)
    sys.stderr.write("RESULT %s (%s)\n" % (verdict, out))


state = {"modals": [], "rounds": 0}


def modal_up():
    """A modal that is up now, rejected and said: its class and title"""
    from PySide import QtWidgets

    w = QtWidgets.QApplication.activeModalWidget()
    if w is None or not w.isVisible():
        return None
    said = "%s %r" % (w.metaObject().className(), w.windowTitle())
    sys.stderr.write("gate: rejecting a modal up at the start: %s\n" % said)
    w.reject()
    return said


def relaunch():
    """The gate again, in a fresh user home (see the module's text); the
    result is the run's. Closes this process's window when it is over"""
    from PySide import QtCore

    home = tempfile.mkdtemp(prefix="sandbox-gui-gate-home-")
    env = dict(os.environ)
    env["FREECAD_USER_HOME"] = home
    env["SANDBOX_GUI_GATE_FRESH"] = "1"
    if not env.get("FCX_PYODIDE"):
        # the newest of the runtimes, by version: "314.0.6", not the
        # "packages" folder beside them
        runtimes = os.path.join(FreeCAD.getUserAppDataDir(), "Pyodide")
        names = os.listdir(runtimes) if os.path.isdir(runtimes) else []
        found = sorted((tuple(int(p) for p in d.split(".")), d) for d in names
                       if d.replace(".", "").isdigit() and os.path.isdir(os.path.join(runtimes, d)))
        if found:
            env["FCX_PYODIDE"] = os.path.join(runtimes, found[-1][1])
    if not env.get("SANDBOX_GUI_GATE_RESULT"):
        env["SANDBOX_GUI_GATE_RESULT"] = os.path.join(
            FreeCAD.getUserAppDataDir(), "sandbox-gui-gate.txt")
    exe = QtCore.QCoreApplication.applicationFilePath()
    sys.stderr.write("gate: in a fresh user home %s (FCX_PYODIDE %s)\n"
                     % (home, env.get("FCX_PYODIDE", "unset")))
    try:
        code = subprocess.call([exe, os.path.abspath(__file__)], env=env)
        sys.stderr.write("gate: the run in the fresh home exited %s; result in %s\n"
                         % (code, env["SANDBOX_GUI_GATE_RESULT"]))
    finally:
        shutil.rmtree(home, ignore_errors=True)
        FreeCADGui.getMainWindow().close()


def finish():
    # A modal up now -- the startup's Document Recovery, whose loop this
    # timer fires inside -- is rejected, said, and the start goes round
    # once more, so that the cases run with that loop unwound
    said = modal_up()
    if said is not None and state["rounds"] < 5:
        state["modals"].append(said)
        state["rounds"] += 1
        QtCore.QTimer.singleShot(200, finish)
        return
    if (not os.environ.get("SANDBOX_GUI_GATE_FRESH")
            and os.environ.get("SANDBOX_GUI_GATE_PROFILE") != "keep"):
        # what this process's own profile put up is not the run's: the
        # run is the fresh one's
        state["modals"] = []
        relaunch()
        return
    try:
        main()
    finally:
        # a document a module left open would make the close ask (a
        # modal, a hang under Xvfb)
        for name in list(FreeCAD.listDocuments()):
            try:
                FreeCAD.closeDocument(name)
            except Exception:
                pass
        # a close that does not end the process shows up as this stack
        # on stderr (Python's view of it) two minutes later
        try:
            faulthandler.dump_traceback_later(120, repeat=False, file=2)
        except Exception:
            pass
        sys.stderr.write("gate: closing the main window\n")
        FreeCADGui.getMainWindow().close()
        sys.stderr.write("gate: main window closed\n")
        # The event loop ends with the window and the process leaves
        # through the normal C++ teardown -- the exit() hang this once
        # skipped with os._exit (MeshLevelSource's level workers parked
        # on a static condition variable) is fixed at the source:
        # PartGui::shutdownMeshLevelWorkers() joins them on aboutToQuit.
        sys.stdout.flush()
        sys.stderr.flush()


# run inside the event loop, after the main window is up
from PySide import QtCore  # noqa: E402

QtCore.QTimer.singleShot(0, finish)
