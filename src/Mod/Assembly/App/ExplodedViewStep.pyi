# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import List, Tuple

from Base.Metadata import constmethod, export
from Base.Placement import Placement
from Base.Vector import Vector

from App.DocumentObject import DocumentObject

@export(
    Include="Mod/Assembly/App/Views.h",
    Namespace="Assembly",
)
class ExplodedViewStep(DocumentObject):
    """
    One step of an exploded view (docs/AssemblyObjectModel.md sec 7.2).

    Author: Zheng Lei (realthunder.dev@gmail.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def explodeFrom(
        self, center: Vector, size: float, fraction: float = 1.0, paths: bool = False, /
    ) -> Tuple[List[Tuple[DocumentObject, Placement]], List[Tuple]]:
        """
        Where the step puts the parts it moves, from where they are now, for
        `fraction` of its move, writing nothing. `center` and `size` are the
        assembly's, as ExplodedView.getCenterAndSize() gives them, which a
        radial step reads.

        Returns: the parts with their new Placement, and the trail of each,
        in the document's frame: (from, to), or with `paths` (from, to,
        points), a turn's arc sampled every 5 deg or less.
        """
        ...
