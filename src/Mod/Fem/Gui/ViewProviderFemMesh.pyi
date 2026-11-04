# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Any, Final

from Base.Metadata import export

from Gui.ViewProviderDocumentObject import ViewProviderDocumentObject

@export(
    Include="Mod/Fem/Gui/ViewProviderFemMesh.h",
    Namespace="FemGui",
)
class ViewProviderFemMesh(ViewProviderDocumentObject):
    """
    ViewProviderFemMesh class

    Author: Juergen Riegel (Juergen.Riegel@web.de)
    License: LGPL-2.1-or-later
    """

    def applyDisplacement(self) -> Any:
        """"""
        ...

    def resetNodeColor(self) -> Any:
        """Reset color set by method setNodeColorByScalars."""
        ...

    def resetNodeDisplacement(self) -> Any:
        """Reset displacements set by method setNodeDisplacementByVectors."""
        ...

    def resetHighlightedNodes(self) -> Any:
        """Reset highlighted nodes."""
        ...

    def setNodeColorByScalars(self) -> Any:
        """setNodeColorByScalars(nodeIds, values, [min, max])

        Sets mesh node colors using node list and value list. The colours run
        from blue at min through green at zero to red at max; min and max are
        the values' own unless given (min is never above zero, max never below).
        Nodes not listed take the colour of zero."""
        ...

    def setNodeDisplacementByVectors(self) -> Any:
        """"""
        ...
    NodeColor: dict
    """Postprocessing color of the nodes. The faces between the nodes get interpolated."""

    ElementColor: dict
    """Postprocessing color of the elements. All faces of the element get the same color."""

    NodeDisplacement: dict
    """Postprocessing color of the nodes. The faces between the nodes get interpolated."""

    HighlightedNodes: list
    """List of nodes which get highlighted."""

    VisibleElementFaces: Final[list]
    """List of elements and faces which are actually shown. These are all surface faces of the mesh."""
