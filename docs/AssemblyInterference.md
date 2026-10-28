# Assembly: the interference check

Which components of an assembly overlap, and, when asked, which touch:
where they are, over the frames of the last simulation, or while a part is
dragged. The static check of the audit's phase 5 (`docs/AssemblyAudit.md`
sec 8.2), interference over time (`docs/AssemblyDynamics.md` sec 7.3, P14),
and collision while dragging (CATIA's "stop on collision"). Built
2026-10-27.

## Use

**Assembly > Check Interference** (`Assembly_CheckInterference`, active with
an assembly in edit) opens a task panel:

- **Mode.** *Exact* works on the solids: the volume two components have in
  common, the exact distance between them; as slow as a boolean. *Fast*
  works on meshes of the solids, and finds that two overlap, not by how
  much.
- **Tolerance** (mm). Two components whose surfaces are within this of each
  other touch: a contact, not an interference, unless they overlap. In
  Fast it is also what covers the meshes' error, and is taken at least twice
  the mesh deflection; the summary says what was used.
- **Mesh deflection** (Fast only; Automatic is 1/10000 of each component's
  size, at least 0.01 mm).
- **Report touching components.** Off by default: mated parts touch by
  design.
- **Positions.** *Where they are*, or *Over the simulation's frames*: each
  frame of the last simulation run (Simulation, then Generate), the
  components where the frame puts them; the document is not moved. Enabled
  once a simulation has been run.
- **Frame step** (over frames): every this many frames; the last is always
  checked.

**Check** lists the pairs: the two components, Interference or Contact, and
the volume in common (Exact) or the distance (contacts). Over frames, a row
per pair and kind says in how many frames, from which to which, and the
largest volume (Exact); the frames are under it, each with its time.
Picking a row selects both components; for an Exact interference the solid
in common is drawn in red until another row is picked or the panel is
closed. Picking a frame shows the assembly at it, as the simulation's
playback does; picking a pair shows its worst frame (Exact), or its first.
The parts are put back where they were when the panel closes or checks
again. The choices are kept in the Assembly preferences (`InterferenceMode`,
`InterferenceTolerance`, `InterferenceDeflection`, `InterferenceContacts`,
`InterferenceFrameStep`).

From Python:

    results = assembly.findInterferences(
        mode="Exact", tolerance=0.0, contacts=False, objects=None, deflection=0.0,
        frames=None)

gives a list of dicts: `Object1`, `Object2`, `Kind` ("Interference" or
"Contact"), `Volume` (Exact, mm^3; 0 in Fast), `Distance` (contacts, mm;
Fast gives the tolerance), `Tolerance` (the one the pair was judged with),
`Overlap` (Exact: the solid in common, in the assembly's coordinates; else
None). `frames` is a list of frame numbers of the last simulation
(`range(assembly.numberOfFrames())` for all); then each result is one pair
at one frame and also has `Frame` and `Time` (s). A frame past the last is
a ValueError, as is `frames` with no simulation run.

### Collision while dragging

Preferences > Assembly > **Collision while dragging**: *Off* (the default),
*Highlight* or *Stop* (the parameter `DragCollision`: 0, 1, 2; its tolerance
`DragCollisionTolerance`, mm, 0 by default and taken at least twice the
deflection, as Fast's). The Assembly tool bar has it too, as **Collision
While Dragging** (`Assembly_DragCollision`, `Gui/Commands.cpp`): the button
turns it on and off, in the mode it shows -- the one its menu chose last,
Stop at first -- and the menu has the three values, as radio buttons. Both
follow the parameter whoever sets it; from a script,
`Gui.runCommand("Assembly_DragCollision", n)` sets it to n. With either on, a
drag watches the assembly's components with the Fast check:

- *Highlight*: the drag goes on; the faces of the components it brings into
  collision that cross each other are drawn in red while they collide (the
  whole component when none cross, one inside the other: the triangles of
  the meshes that cross, `touchOf()` below, give the faces,
  `InterferenceResult::touchFaces1`/`2`).
- *Stop*: a step that would bring two components into collision goes only
  as far as it can without -- the solver and the document both -- and the
  pair's faces are drawn in red so, the status bar saying "Stopped: A would collide with
  B". The part stays against the other and follows the mouse again as soon
  as it can. How far: the move from the last good step to this one is
  halved towards the farthest fraction that does not collide
  (`AssemblyObject::stopAtDragCollision()`) until what is left of it moves
  the parts less than the watch's tolerance -- the first fraction posed
  tells how far the step moves them (`CollisionWatch::travel()`, the most a
  corner of a component's box went) -- and 8 times at most, so the part
  stops within the tolerance of the contact, or 1/256 of a long step.
  Pushed on from where the last step stopped -- most often against what
  stopped it -- the least fraction, 1/256, is tried first: colliding, the
  part stays, for one check instead of all of them. With the
  solver each fraction is a drag step solved from the last good poses
  towards the targets moved that fraction of the way (`Placement::sclerp`);
  without, the moved parts' placements are interpolated so.

Components interfering when the drag starts are left out (the log says how
many pairs), so a drag can move parts that overlap by design apart. Both
drags are watched: with the solver (`doDragStep()`) and without (the 3D view
moving a part itself, "Solve while dragging" off), and a drag of a part in a
rigid group, which the 3D view does by a whole solve per move: stopped, it
solves each fraction of the move too, the dragged parts at that fraction --
a rigid group stays together, whichever member meets the other part. That
costs a stopped move about 40-50 ms (a box in a group of two: 12 ms a move
otherwise). And the transform dragger's moves, for a part no joint holds:
watched from the dragger's start to its finish, a stopped move interpolated
as without the solver, and the dragger, let go past where the parts
stopped, put back on them. From Python: the preference, then `preDrag()`, `doDragStep()`,
`getDragCollisions()` (the pairs the last step brought into collision, or,
stopped, would have), `postDrag()`.

Where a drag brought two components into collision is marked with a red
arrow, its tip there, pointing into the component hit (the one that did not
move), drawn over everything -- the selection's highlight too -- and the same
size on screen at any zoom. One for each pair and drag, at the last check that
found them: stopped, where they meet; highlighting, where they last crossed.
**Check Interference** marks its interferences the same way, one for each
pair (over frames, at the frame its row shows), pointing into Object2; a
pair marked at the same place already is not marked again, so running the
check again where nothing moved adds nothing. With contacts asked for, the
pairs that only touch are marked too, in amber, at the middle of where they
touch.

The marks stay after the drag or the check, in the assembly they were made
in, until **Clear Collision Markers** (`Assembly_ClearCollisionMarkers`, on
the tool bar) removes them -- the edited assembly's, else those of every
assembly of the document. They are not saved. Where
(`InterferenceResult::touchCentre`/`touchNormal`):

- Fast (`touchOf()` in `App/Interference.cpp`): the points where an edge of
  one mesh passes through a triangle of the other make, for each pair of
  triangles, a segment of the curve along which the meshes cross; the centre
  is the middle of the segments, each weighted by its length. The direction
  is the normal of the hit component's triangles that edges of the other
  pass through, turned to point out of it: a step along it leaves the hit
  component, or, the centre deep in the overlap, it points from the hit
  one's box towards the other's. One inside the other, nothing crosses: the
  middle of the common part of their boxes, the direction between the boxes.
- Exact: the centre of mass of the solid in common, the direction from the
  centre of the box of the one hit to the other's.
- A contact, in both modes (`contactOf()`; Exact meshes the two for it, at a
  tolerance of at least twice the meshes' error): the middle of the vertices
  of each mesh within the tolerance of a triangle of the other's. The
  direction is the normal of the hit component's triangles that lie face to
  face with the other's -- parallel, within the tolerance, and overlapping
  by more than it -- not those of faces meeting only at an edge, as the
  sides of two blocks one on the other; turned out of the hit one as above.
  A pin in its hole is touched all round: the direction says little there.

From Python: `getCollisionMarkers()` (dicts of `First`, `Second`, `Centre`,
`Direction`), `addCollisionMarker(first, second, centre, direction, contact=False)`,
`clearCollisionMarkers(checksOnly=False)`; `findInterferences()` gives each result its
`Centre` and `Direction`.

### Rules

The assembly's **Interference** properties, which the check and collision
while dragging both follow (`Rules` in `App/Interference.cpp`;
`findInterferences(rules=False)` checks without them):

- `InterferenceExcluded`: components left out, or containers or plain
  groups of them -- a sub-assembly, a fasteners group -- whose components
  are then all left out.
- `InterferenceIgnoreHidden`: the hidden components left out (a component,
  or a container or group it is in, with its Visibility off).
- `InterferenceIgnoredPairs`: pairs never reported, `"Name1|Name2"` either
  way round, by the objects' names: a component's, or a container's or
  group's it is in (names, not links: removing a linked object from a list
  would shift the pairs after it; a pair naming a deleted object matches
  nothing).
- `InterferenceClearances`: `"Name1|Name2"` to mm. A pair that does not
  overlap but comes closer than its clearance is reported, as `Clearance`
  (Exact: its distance; Fast: its meshes within the clearance of each
  other); while dragging it is a collision, highlighted or stopped at, so a
  part stops the clearance away from the other. The largest of the
  clearances that name a pair.

In Check Interference, a result's context menu adds them -- Ignore This
Pair, Leave Out (either component), Require Clearance -- each an undoable
change of the assembly that runs the check again; "Leave out hidden
components" is the hidden one's box, and Clear Rules empties them all. Undo
changes the properties, but not the open panel until it checks again. Each
run replaces the markers of the last; a drag's stay until cleared.

A component in a plain group in the assembly was collected twice
(`getAssemblyComponents()`: the assembly's Group has it too) and found
interfering with itself; it is collected once now.

## What is checked

The assembly's components (`getAssemblyComponents()`: a rigid sub-assembly
is one, a flexible one is its components), each with its shape taken
through its path from the assembly, so that a component of a flexible
sub-assembly is where it is in the assembly. Only solids: a component with
none is left out. Pairs whose bounding boxes are farther apart than the
tolerance are not looked at.

**Exact** (`Assembly::findInterferences`, `App/Interference.{h,cpp}`):
`BRepAlgoAPI_Common` of the two, non-destructive (the solids are the
document's own, and a destructive boolean may raise their tolerances), the
solids as arguments and tools. A volume above 1e-9 of the smaller
component's (faces in contact give a common of no volume, or of rounding)
is an interference, and the common is kept for the drawing. Otherwise, with
contacts asked for, `BRepExtrema_DistShapeShape`: within the tolerance, a
contact.

**Fast** (`App/ContactGeometry.{h,cpp}`, `App/Interference.cpp`):

1. Each distinct solid (TShape) is meshed once, on a copy
   (`BRepBuilderAPI_Copy`: the display's triangulation is left alone), into
   a `ContactMesh`: the mesh, its triangles' tree and the solid's exact
   classifier, all in the solid's own frame. `ContactGeometryCache` keeps
   them for every caller (the check, its frames, a drag's watch, the
   dynamics later; `docs/AssemblyDynamics.md` sec 7.2) and drops those whose
   solid nobody else holds as it grows -- a fastener used 200 times is
   meshed once, and a second check meshes nothing. A solid in a component is
   an instance: its `ContactMesh` and its pose in the assembly.
2. Two solids overlap where their surfaces cross. An edge of a triangle of
   one crossing a triangle of the other, its ends on either side of the
   triangle's plane, one more than the tolerance from it and the other more
   than 1e-3 of it, is searched for over the two solids' bounding volume
   trees (`BRepExtrema_TriangleSet`'s BVH), in the first solid's frame: the
   second's tree is carried over by the pose of its solid relative to the
   first's, its boxes as boxes around them and its triangles moved as they
   are reached. Moving a part moves its pose: nothing is rebuilt. Not both ends beyond the
   tolerance: a sphere sunk 0.08 mm into a face had no such edge, its
   mesh's rings near the pole 0.06 deep and within 0.001 of the face. The
   1e-3 leaves out the ends of edges on a face in contact, on its plane to
   rounding.
3. A crossing is only a candidate: where faces touch, an edge of one meets
   the other's boundary, and meshes that touch (a pin in its hole) cross
   within their error. It is confirmed by a point near it inside both solids
   by more than the tolerance, by exact point classification
   (`BRepClass3d_SolidClassifier`) -- any such point is a witness, so no
   point tried can make a false positive. A point the mesh has outside is
   not classified: by the parity of a ray's crossings through the tree
   (another ray when one grazes an edge or a vertex, the classifier when all
   do). The tolerance is at least twice the deflection, so a point inside
   the solid by more is inside the mesh by more than the deflection. Faces
   in contact along tangent lines (two filleted plates turned on each
   other) make candidates that are never confirmed, each tried at up to
   eight points: the parity took such a pair from 57 ms to 15 ms. Tried: along the bisector of the
   two triangles' normals, either way and either sign (the region inside
   both is a wedge between the surfaces; the step is sized to be 1.5 times
   the tolerance deep, as far as the edge is long); then a little along the
   edge, either way, and off its triangle, either way (the overlap of two
   boxes is bounded by faces of theirs, and its crossings are on them).
   After 64 unconfirmed candidates a pair is taken to touch.
4. One inside the other crosses nowhere, nor do two copies of a part in one
   place, whose surfaces coincide. With no crossing, one is wholly inside
   the other or wholly outside it, and any point inside it tells which: one
   is found 2 tolerances off the middle of a triangle of its mesh, either
   way, and classified -- not a vertex, which can be on the other's surface.
   Only when its mesh's box is in the other's.
5. Overlaps bounded by coplanar faces and tangent blends cross nowhere
   either: two filleted plates of one thickness slid into each other edge
   to edge. The middle of the part the two meshes' boxes have in common,
   and the middles of its halves along its longest side, are classified;
   for faces in contact that part is a slab, its middle on them.
6. With contacts asked for, a pair with no interference whose meshes come
   within the tolerance (`BRepExtrema_ShapeProximity`) is a contact.

**Over frames** the components are built once. At each frame a component's
move is its placement in the assembly at the frame (the simulation's for
each of it and its containers that the simulation moves, the present one
for the rest) times the inverse of its placement now: its solids' poses
change (Fast), or its solids are moved (Exact). A pair neither of whose
components moved since the last frame checked is not checked again: it has
the last frame's finding. The work shows on the progress bar and can be
cancelled there.

**While dragging** (`CollisionWatch`) the same, with the document's
placements after each step for a frame: a component that moved less than
1e-9 (the solver's rounding) has not moved. A step taken back goes to the
solver as `Session::setPoses()`, which every backend and the two session
wrappers (the centred frames, the rigid groups) take.

## Measured (2026-10-27)

Over 102 frames, 20 sliders each driving a sphere (r 5) down into a box and
up through a disk, 60 components, defaults:

| | Fast | Exact |
|---|---|---|
| time | 0.48 s, the meshes cached (0.9 s to mesh them) | 2.8 s |
| findings | 963 | 965 |

Over 52 frames, 10 sliders each moving a filleted plate with 16 holes (42
faces) over and across another:

| | Fast | Exact |
|---|---|---|
| time | 0.28 s | 148 s |
| findings | 514 | 514 |

On analytic solids the booleans are cheap; Fast pays from a few dozen faces
on. One frame of the plates where they are: Fast 0.012 s, Exact 2.4 s.
Before the meshes stayed in their solids' frames (their trees rebuilt for
each move), Fast took 2.7 s and 1.3 s over these frames.

Dragging, a stack of 30 such plates in face contact on revolutes, the top
one turned 2 deg a step (3.5k triangles a plate, 594 on each face in
contact):

| Collision while dragging | a step (median) | the drag's start |
|---|---|---|
| Off | 4 ms | 0.008 s |
| Highlight | 16 ms | 0.2 s (every pair checked once) |
| Stop | 16 ms | 0.17 s |

A step that collides costs its halvings: a solve and a check each, as few
as reach the tolerance, 8 at most. The box of the tests
(`TestDragCollision`) dragged 2 mm, 1 mm of it into a plate: 11 to 31 ms
(5 halvings), against under 1 ms for a step that does not collide; it
stops 0.031 mm into the plate, within the tolerance, on all three backends,
where it stopped 1 mm short before.

The stack with a block beside its top plate, which the plate's corner meets
at 20 deg, turned on to 60 deg in steps of 2 (as a mouse moving on past the
block does, each step from the contact to further on):

| Stop, a step | median | max |
|---|---|---|
| free (to 20 deg) | 23 ms | |
| pushing on against the block, all halvings | 230 ms | 260 ms |
| the least fraction first | 87 ms | 315 ms (the first stop) |

Highlight's steps through the block: 28 ms. The rest of a stopped step is
the step's own check, its solve, and putting the parts back.

Timed with a WSL build sharing the box.

What Fast misses here, Exact found: two sphere caps of under 0.1 mm^3, some
0.08 mm deep, at a tolerance of 0.02 mm -- the mesh's rings, 0.6 mm apart at
the default deflection, straddle such a cap without an edge across it. At a
deflection of 0.005 mm they are found. Before the confirmation on the
bisector, Fast missed 231 interferences of the first case, up to 126 mm^3;
before the classification of an inner point and of the middle of the boxes'
common part, 10 of the second (two copies in one place) and 2 more (the
filleted plates edge to edge). Each of these is a test now
(`AssemblyTests/TestInterference.py`). The non-destructive boolean fixed
Exact, which failed (`IsDone()` false) where a sphere touched a disk's face
at a point.

## Tests

`AssemblyTests/TestInterference.py`:

- `TestInterference`, on an assembly whose answers are known: an overlap of
  200 mm^3, a box inside another, two bars crossing (8 mm^3 in common, no
  vertex of either inside the other), faces in contact, a pin snug in its
  hole, a gap of 0.05 mm, a Link, two copies of a box in one place, a box
  in another on five of its faces, two filleted plates edge to edge. Both
  modes find the same pairs; Exact gives the volumes to 1e-6.
- `TestInterferenceOverFrames`: a slider drives a box up through a plate,
  and two boxes that never move overlap. Every frame's volume against the
  playback's own common, the document left where it was, the unmoving pair
  at every frame, the frames' times in order, only the frames asked for,
  bad frames refused; Fast finds the same frames, but for overlaps under
  1 mm^3.

## Limits

- Dragging stops within the tolerance of the contact, or, a step more than
  256 times as long, within 1/256 of it: a fast mouse's very long step
  leaves a gap of that much. The halvings are fractions of the step, not of
  the distance to the contact.
- Stop's least-fraction-first and its halving both take the step's path to
  collide from some fraction on: a path that passes through a gap in the
  other part and collides only in the middle is stopped where a halving
  lands, not at the first contact.
- Fast finds that two components overlap, not by how much, and an overlap
  no deeper than a few times the tolerance can be taken for a contact; Exact
  is the authority. Fast's tolerance is at least twice the deflection, so
  checking a press fit of 0.01 mm takes Exact, or a deflection under
  0.005 mm.
- Over frames, an interference that begins and ends between two frames
  checked is not seen: the frames are the simulation's output steps (and
  the frame step's).

## Not done

- In Check Interference, the faces that cross or touch highlighted (a drag
  has them), the touching area measured.
- The solid two components have in common drawn over everything from the
  meshes, and Fast measuring it.
- The work in a worker or another process (`docs/ComputeBoundaries.md`), and
  the pairs of a frame in parallel: Stop pushing on against a part of the
  plates' stack costs 87 ms a step on the main thread.
- The transform dragger's collision handling was checked with Coin events
  sent to the dragger itself, not with a mouse on its arrows (synthetic Qt
  clicks there land as selection clicks); the markers were seen in the
  bgfx renderer's own frame (`saveRenderDump`) as in Coin's.
