# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

from typing import Any

from Base.Metadata import constmethod, export

from App.DocumentObject import DocumentObject

@export(Include="Mod/Fluid/App/Study.h", Namespace="Fluid")
class Study(DocumentObject):
    """
    A fluid study: its walls, the fluid at the start, its monitors, the run
    and the results it keeps.

    License: LGPL-2.1-or-later
    """

    @constmethod
    def buildModel(self) -> dict:
        """
        The study as a solver is given it, summed up: Walls and Fluids (how
        many), Triangles, Monitors, Resolution (m), SpaceMin and SpaceMax (m),
        Gravity (m/s^2), TimeEnd and OutputInterval (s), Signature (str).
        """
        ...

    def run(self) -> int:
        """
        Runs the study on its backend and keeps the results: start() and
        finish(). 0 when it ran, else a negative code: -1 failed, -2 the
        backend cannot run this study, -3 the backend or what it needs was
        not found, -4 nothing to simulate, -7 cancelled. The Status property
        says why.
        """
        ...

    def start(self) -> int:
        """
        Starts a run in a thread of its own and returns at once; codes as
        run()'s. Follow it with getProgress() and end it with finish().
        """
        ...

    @constmethod
    def getProgress(self) -> dict:
        """
        Running (a run was started and not finished), Done (finish() will
        not wait), Time (of the last frame, s) and Frames (made so far).
        """
        ...

    def cancel(self) -> None:
        """
        Asks the run to stop; finish() then returns -7.
        """
        ...

    def finish(self) -> int:
        """
        Waits for the run started and keeps its results; codes as run()'s.
        """
        ...

    @constmethod
    def getResults(self) -> Any:
        """
        The kept results, or None: Backend, Version, Kind ('Particles',
        'Surface'), Times (the frames', s), Frames (how many), MonitorTimes
        (the monitors' own times, s: a solver reports them far oftener than
        it makes frames) and Monitors, each Object, Kind, Unit and Values
        (one per monitor time).
        """
        ...

    @constmethod
    def getFrame(self, index: int, /) -> dict:
        """
        One kept frame, read from the document when asked for: Time (s),
        Count (of points), Points (bytes: float32 x, y, z each, mm) and, for
        a surface, Triangles (bytes: int32, three per triangle).
        """
        ...

    @constmethod
    def getSurface(self, index: int, /) -> dict:
        """
        One kept frame as a closed surface to draw: Time (s), Count (of
        points), Points (bytes: float32 x, y, z each, mm), Normals (bytes:
        float32, one a point, outwards) and Triangles (bytes: int32, three
        per triangle, wound outwards). A frame of particles is wrapped in
        the surface around them, each a cube of the run's resolution,
        particles apart from the body as drops that run together where
        they are near; those with next to none about them (one alone, a
        pair, three in a row) are no part of it but its Spray (bytes:
        float32 x, y, z each, mm). A frame that is a surface has no
        Normals and no Spray and is given as it is kept.
        """
        ...

    @constmethod
    def isStale(self) -> bool:
        """
        Whether the kept results are of another model than the study is now.
        """
        ...
