# SPDX-License-Identifier: LGPL-2.1-or-later
# Unit tests of the PMI module (docs/GDandT.md)

import Part

# The PMI module calls the OCCT fork's GD&T extensions of XCAF by their C++ names: where the
# OCCT libraries are upstream's it does not load (docs/GDandT.md), and its tests are one
# skip saying so
if Part.hasOCCTFunction("TKXCAF", "XCAFDoc_DimTolTool_PMI"):
    from PMITests.TestModel import (  # noqa: F401
        TestAdvisor,
        TestApi,
        TestCheck,
        TestDatumTarget,
        TestEdges,
        TestModel,
        TestPresentation,
        TestQuery,
        TestRepair,
        TestView,
        TestViewObjects,
    )
    from PMITests.TestDrawing import TestImportModelAnnotations  # noqa: F401
    from PMITests.TestStep import (  # noqa: F401
        TestNistSet,
        TestStepExport,
        TestStepImport,
        TestStepRoundTrip,
        TestStepShapes,
    )
else:
    import unittest

    class TestPMIUnavailable(unittest.TestCase):
        def test_needs_the_occt_fork(self):
            self.skipTest(
                "the PMI module needs the OCCT fork's GD&T extensions (TKXCAF's"
                " XCAFDoc_DimTolTool_PMI), which these OCCT libraries do not have"
            )
