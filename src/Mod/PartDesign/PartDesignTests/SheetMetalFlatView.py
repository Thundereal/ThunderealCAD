# SPDX-License-Identifier: LGPL-2.1-or-later

# A feature's flat as the sheet metal suites read it: its element Flat
# (docs/SheetMetal.md sec 12), with the values the retired flat pattern
# object had, worked out from it as PartDesign::layFlat() works out the
# sheet's solid's: the size from the solid's bounds without tolerance, the
# area its volume over the thickness (from its two skins' planes: its bounds
# across Z are looser than 1e-6), one bend line a bend.

import FreeCAD
import Part

Q = FreeCAD.Units.Quantity


class Flat:
    def __init__(self, feature):
        self.Source = feature
        self.Shape = feature.getSubObject("Flat")
        # as the flat pattern's State said it
        self.State = ["Invalid"] if self.Shape is None else []
        if self.Shape is None:
            return
        solids = self.Shape.SubShapes[0].Solids
        if len(solids) != 1:
            self.State = ["Invalid"]
            return
        box = solids[0].optimalBoundingBox(False, False)
        skins = [
            f.Surface.Position.z
            for f in solids[0].Faces
            if isinstance(f.Surface, Part.Plane) and abs(abs(f.Surface.Axis.z) - 1) < 1e-9
        ]
        self.FlatLength = Q(max(box.XLength, box.YLength), FreeCAD.Units.Length)
        self.FlatWidth = Q(min(box.XLength, box.YLength), FreeCAD.Units.Length)
        self.FlatArea = Q(solids[0].Volume / (max(skins) - min(skins)), FreeCAD.Units.Area)
        self.BendCount = len(self.Shape.SubShapes[1].Edges)
