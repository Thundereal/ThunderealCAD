# Skips for the tests of what the realthunder OCCT fork fixes or adds, when
# the OCCT loaded at run time is another one -- upstream's, as FreeCAD can
# link it (FCAD_DEV_OCCT_BIN on Windows). Such a test fails or hangs there for
# want of the fork, not for a fault of FreeCAD's.
#
# The probe is the one FreeCAD itself uses to find the fork at run time: a C
# function of the fork's toolkits, looked up by name (Part.hasOCCTFunction).
# A test of one of the fork's functions asks for that function; a test of one
# of its fixes to an algorithm, which has no function of its own, asks for
# the fork (TKBRep's SetFuncShowTopoShape), and says which fix it needs.

import unittest

import Part

# The function that marks the fork, in every version of it
FORK_MARK = ("TKBRep", "SetFuncShowTopoShape")


def hasOCCTFork():
    """Whether the OCCT loaded is the realthunder fork"""
    return Part.hasOCCTFunction(*FORK_MARK)


def needsOCCTFork(fix):
    """Skips a test of a fix of the fork's (fix: which, as the skip says)
    on any other OCCT"""
    return unittest.skipUnless(hasOCCTFork(), "needs the OCCT fork: " + fix)


def needsOCCTFunction(toolkit, name):
    """Skips a test of the fork's C function name of toolkit on an OCCT
    without it"""
    return unittest.skipUnless(
        Part.hasOCCTFunction(toolkit, name), "needs the OCCT fork's %s (%s)" % (name, toolkit)
    )


_raised = None


def refusals():
    """How many times the fork has refused to change a frozen shape so far
    (TopoDS_LockedShape raised, TKBRep's TopoDS_LockedShape_Raised): one
    caught inside an algorithm counts too. Compare before and after."""
    global _raised
    if _raised is None:
        import ctypes
        import sys

        if sys.platform == "darwin":
            lib = ctypes.CDLL("RTLD_DEFAULT", handle=ctypes.c_void_p(-2).value)
        elif sys.platform == "win32":
            lib = ctypes.CDLL("TKBRep.dll")
        else:
            # TKBRep by the path it is loaded from: Part.so is loaded
            # RTLD_LOCAL, and there may be no unversioned libTKBRep.so
            with open("/proc/self/maps") as maps:
                paths = sorted({l.split()[-1] for l in maps if "/libTKBRep." in l})
            lib = ctypes.CDLL(paths[0]) if paths else ctypes.CDLL(None)
        _raised = lib.TopoDS_LockedShape_Raised
        _raised.argtypes = []
        _raised.restype = ctypes.c_uint
    return _raised()


class frozen:
    """Shape values frozen (Part's ImmutableShapeValues) -- or not, with
    frozen(False) -- while the block runs, the user's own value back after."""

    GROUP = "User parameter:BaseApp/Preferences/Mod/Part"
    NAME = "ImmutableShapeValues"

    def __init__(self, on=True):
        self.on = on

    def __enter__(self):
        import FreeCAD

        self.grp = FreeCAD.ParamGet(self.GROUP)
        self.had = self.NAME in self.grp.GetBools()
        self.old = self.grp.GetBool(self.NAME)
        self.grp.SetBool(self.NAME, self.on)
        return self

    def __exit__(self, *exc):
        if self.had:
            self.grp.SetBool(self.NAME, self.old)
        else:
            self.grp.RemBool(self.NAME)
        return False
