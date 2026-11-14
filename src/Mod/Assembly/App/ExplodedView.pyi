# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import List, Optional, Tuple

from Base.Metadata import constmethod, export
from Base.Placement import Placement
from Base.Vector import Vector

from App.DocumentObject import DocumentObject

@export(
    Include="Mod/Assembly/App/Views.h",
    Namespace="Assembly",
)
class ExplodedView(DocumentObject):
    """
    An exploded view of an assembly: steps in Group, applied in order
    (docs/AssemblyObjectModel.md sec 7.2).

    Author: Zheng Lei (realthunder.dev@gmail.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def getAssembly(self) -> Optional[DocumentObject]:
        """The assembly whose parts the view moves."""
        ...

    @constmethod
    def getSteps(self) -> List[DocumentObject]:
        """The steps, in order."""
        ...

    @constmethod
    def getCenterAndSize(self) -> Tuple[Vector, float]:
        """
        The center of the assembly's bounding box at rest and its diagonal
        (100 when it has no shape), in the document's frame: a radial step
        moves a part away from the center, by the size.
        """
        ...

    @constmethod
    def explodedAt(
        self, progress: Optional[float] = None, /
    ) -> Tuple[List[Tuple[DocumentObject, Placement]], List[Tuple[int, Vector, Vector]]]:
        """
        Where the steps put the parts at `progress`, writing nothing: 0 is
        the assembly at rest, n the end of step n, a fraction part of the
        next step's move; the default is the end.

        Returns: the parts moved, each with its Placement (in its parent's
        frame), in the order first moved; and the trails, one per part per
        step started, each the step's index and where the center of the
        part's bounding box went from and to, in the document's frame.
        """
        ...
