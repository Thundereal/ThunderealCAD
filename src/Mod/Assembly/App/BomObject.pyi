# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import List, Tuple

from Base.Metadata import constmethod, export

from App.DocumentObject import DocumentObject
from Spreadsheet.Sheet import Sheet

@export(
    Include="Mod/Assembly/App/BomObject.h",
    FatherInclude="Mod/Spreadsheet/App/SheetPy.h",
    Namespace="Assembly",
)
class BomObject(Sheet):
    """
    This class is the BOM object of assemblies, it derives from Spreadsheet::Sheet.

    Author: Ondsel (development@ondsel.com)
    License: LGPL-2.1-or-later
    """

    @constmethod
    def getRows(self) -> List[Tuple[str, DocumentObject]]:
        """
        Not upstream: each row's index ("1", "1.2") and object, in order, as
        the last recompute made them.
        """
        ...

    @constmethod
    def getIndexOf(self, obj: DocumentObject, /) -> str:
        """
        Not upstream: the index of the row of obj, a link's being its linked
        object's as the BOM files it; empty if it has none.
        """
        ...
