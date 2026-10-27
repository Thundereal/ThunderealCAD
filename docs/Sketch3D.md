# 3D sketch: design (2026-10-26, proposal -- nothing built)

Status: DESIGN FOR DISCUSSION. Asked for by the user on 2026-10-26 as one of
three things for the pending release (3D sketch, a fuller Surface
workbench, sheet metal). The agreed order is Surface first and sheet metal
later, both as native C++ workbenches, with this design before any 3D
sketch code. The survey it rests on is `docs/Sketch3DSurvey.md`, cited here
as [S n]; claims about our own code were read from the source on the same
day. The open questions are section 13.

## 1. What it is for

A 3D sketch is a set of points, lines, arcs and splines placed anywhere in
space, held by constraints and dimensions the way a 2D sketch is, and
published as edges. Every program that has one names the same jobs
[S 1][S 19][S 25][S 32]:

- **Paths**: sweep and pipe spines that do not lie in a plane -- tube and
  pipe routing, wire harnesses, handrails, frame and weldment skeletons.
- **Curve networks for surfaces**: the boundary and section curves a
  Filling, a Gordon surface or a loft with guides is built from. This is
  where it meets the Surface work, which goes first.
- **Skeletons**: layout lines that parts and assemblies are attached to.

The fork has none of this today. A non-planar path is a Draft wire or
B-spline (no constraints), a Part helix, or edges borrowed from a model.
Upstream has the same gap: FreeCAD issue #14010, "3D Constraint-driven
sketcher/wireframe workbench", is open with Priority: High and a GSoC label
(checked 2026-10-26), and a 2026-02 comment there records "We don't have
useful 3D sketching tools" from FreeCAD's complaint session [S 76].

## 2. What other CAD programs do

The full survey, with sources, is `docs/Sketch3DSurvey.md`. What matters for
the design:

- **Who has one.** A constrained 3D sketch: SOLIDWORKS, Inventor, Fusion (as
  an option on an ordinary sketch), Solid Edge, SolveSpace (whose whole model
  is one). No constrained 3D sketch, but history-based or freeform 3D curves
  instead: NX, Creo, CATIA, Onshape. Onshape's own answer to "3D sketch like
  SolidWorks?" was its Routing Curve feature [S 48]. Rhino and Plasticity
  have free 3D curves without lasting constraints [S 51][S 52].
- **The core constraint set** found in essentially every 3D sketcher:
  coincident (point-point and point on curve), parallel, perpendicular,
  tangent, equal, midpoint, fix, collinear, point-point distance, radius,
  angle [S sec 3]. On top of it, **every one aligns a line with an axis**:
  SOLIDWORKS has AlongX/Y/Z to the global axes and Horizontal / Vertical /
  Normal local to a 3D sketch plane [S 5]; Inventor has parallel to the X/Y/Z
  axis and to the XY/YZ/XZ plane [S 16]. **On plane / on face** appears
  wherever model geometry can be referenced (SOLIDWORKS, Inventor, Solid
  Edge, SolveSpace).
- **Left out in 3D on purpose.** Inventor marks Concentric, Horizontal /
  Vertical and Symmetric as 2D only [S 17], and its line-line angle fails
  unless the lines are coplanar [S 23]; SolveSpace needs a workplane for
  horizontal / vertical and for arcs [S 50]. Each of these is undefined
  without a reference plane. The two ways round it are planes inside the 3D
  sketch -- SOLIDWORKS' 3D sketch planes, each with its own relations and
  defined state [S 7], SolveSpace's workplanes with "projected" versions of
  every constraint [S 50] -- or explicit global-axis variants.
- **Placing points.** SOLIDWORKS: Tab cycles the XY / YZ / ZX plane, Ctrl+click
  on a plane, face or coordinate system reorients them, and a space handle
  appears at the first point [S 2][S 8]. Fusion and Onshape: a triad
  manipulator, with planes and axes to pick, that moves to the last point
  [S 28][S 47]. Solid Edge: keys lock a triad axis (z), a plane (x) or an
  edge (s) [S 32]. All of them snap to model vertices, edges and faces, and
  SOLIDWORKS creates the Along / Horizontal relations from those snaps
  [S 7][S 10].
- **Dimensions.** SOLIDWORKS offers absolute and along X / Y / Z, cycled with
  Tab while placing [S 6]; Solid Edge dimensions are true lengths, projected
  ones optional [S 32]; Inventor's 3D dimension set is line length,
  point-plane distance, radius, spline length, two-line angle, two-point
  distance [S 23].
- **Model references.** Include / convert model edges and 2D sketch geometry
  (Inventor requires the 2D sketch to be earlier in the tree [S 21]),
  intersection curves, curves on face, project to surface [S 3][S 20][S 29].
- **Splines.** Through-points and control-point kinds both exist (Inventor
  has both [S 22]); tangency and curvature to model faces and edges is where
  surface users need them [S 13][S 17].
- **Known pain.** 3D sketches are harder to solve, and relations fail or
  flip more often (a SOLIDWORKS author, quoted in [S 15], search summary);
  editing is tricky because true lengths are not always visible [S 32];
  Fusion warns its 3D option makes off-plane picks easy to make by accident
  [S 30]; SolveSpace's solver had to be patched for speed by Dune 3D [S 71].

## 3. What this fork has to build on

Read from the source, 2026-10-26:

- **PlaneGCS** (`src/Mod/Sketcher/App/planegcs`, LGPL-2.1+, 13.6k lines). Its
  core does not know it is 2D: a `GCS::Constraint` is a vector of parameter
  pointers with `errorgrad()` (`Constraints.h` 113-215), and `GCS::solve`,
  the DogLeg / LM / BFGS algorithms, the split into independent
  subsystems, the QR-based diagnosis (DOF count, redundant, conflicting,
  partially redundant, dependent parameter groups, `GCS.h` 665-690) and the
  temporary drag constraints all work on that parameter vector. The core
  special-cases one constraint type, `Equal`, which it reduces away
  (`GCS.cpp` 1818); the other switch on the type is a debug dump. What IS
  2D: `GCS::Point { double *x, *y; }` (`Geo.h` 39-51), `DeriVector2` ("3d
  vectors with null z", `Geo.h` 111), the curve classes, and the 90
  constraint classes. New 3D geometry and constraint classes can sit beside
  them and be handed to the solver through the public
  `GCS::addConstraint(Constraint*)` (`GCS.h` 303) without touching the
  solving code. It already treats a curve parameter as an unknown
  (`addConstraintCurveValue(Point&, Curve&, double* u)`, `GCS.h` 460) -- the
  pattern "point on face" needs with (u, v).
- **OndselSolver** (`src/3rdParty/OndselSolver`, LGPL-2.1, 28k lines): the
  Assembly workbench's multibody solver -- rigid parts with frames, joined by
  joints. A sketch line whose length is free is not a rigid body.
- **Part::Geometry is already 3D** (`GeomPoint`, `GeomLineSegment`,
  `GeomCircle`, `GeomArcOfCircle`, `GeomBSplineCurve`, `GeomPlane`), and
  `Part::PropertyGeometryList` stores it. The 2D sketch only keeps z = 0.
- **`Sketcher::Constraint` / `PropertyConstraintList`** (`App/Constraint.h`):
  a constraint by type, geometry ids and point positions, a value, a name,
  driving or reference, and the expression binding through
  `Constraints.<name>`. Its `ConstraintType` enum ends at `Text = 21`.
- **The 2D Sketcher's splines**: control-point and by-interpolation, open and
  periodic (`Sketcher_CreateBSpline`, `..ByInterpolation`, `..Periodic..`).
- **Attachment** (`Part::AttachExtension`), so a 3D sketch's own frame can be
  attached to a datum, a face or an LCS like any attachable object.
- **Edit-mode helpers**: `Gui::EditableDatumLabel`, which takes the
  `Base::Placement` of the plane its label lies in (`EditableDatumLabel.h`
  70), the on-view value entries (docs/OnViewValueEditing.md), and
  `Gui::SoFCCSysDragger`, the triad used for placement editing.
- **The 2D Sketcher's GUI is 2D throughout**: `DrawSketchHandler.h` alone uses
  `Base::Vector2d` 57 times, `ViewProviderSketch.cpp` 57. Tools for a 3D
  sketch need their own handler base; the tool widget, the on-view parameter
  machinery and the command plumbing can be shared.
- **Consumers take any edges**: PartDesign Pipe's `Spine` is a
  `PropertyLinkSub` to edges of any object (`FeaturePipe.cpp` 73); Part Sweep
  and the Surface features likewise.

## 4. Solver: extend PlaneGCS with a 3D layer

### 4.1 The choice

| Option | Verdict |
|---|---|
| SolveSpace's solver (libslvs) | Best feature fit, GPL-3.0-or-later (checked 2026-10-26). Linking it makes the distributed core GPL [S 54]. Assembly3 kept it out of the core by loading `py_slvs` at run time from a separately installed GPL workbench [S 56][S 57]; a native C++ sketch cannot do that. Its ideas (projected constraints, groups) are published and usable; its code is not. |
| OndselSolver | LGPL, in tree, but rigid bodies and joints, not sketch entities: every point would be a six-DOF body. No sketch-style diagnosis. |
| openDCM | LGPL-2.1, 3D primitives, but "pre-alpha ... unuseable" in its own README, last push 2018 [S 66]. |
| D-Cubed 3D DCM, Spatial CDS, C3D, LGS 3D | Commercial [S 59][S 61][S 63][S 64]. Not distributable with free source. |
| OCCT | No solver; `TDataXtd_Constraint` only stores constraints [S 65]. |
| A new solver on Eigen / Ceres / NLopt | Possible, but rebuilds what PlaneGCS already has: DogLeg / LM, sparse QR diagnosis, subsystem splitting, drag handling. |
| **Extend PlaneGCS** | **Recommended.** LGPL, in tree, the core is dimension-free (section 3), its diagnosis is what users of our 2D sketch already know, and the fork has measured and cut its costs before (a 92-spline sketch with no constraints took 5.46 s in its diagnosis, now 0.29 s). The work is a 3D geometry and constraint layer. No 3D fork exists to start from [S 68]. |

The literature backs a numeric least-squares solve with rank diagnosis over
graph decomposition for 3D: extending the 2D decomposition methods to 3D
"remains an open issue due to the lack of available rigidity theory in 3D"
[S 74] (search summary).

### 4.2 Where it goes

New files beside the 2D ones, in the same library (`SketcherExport`):

- `planegcs/Geo3D.{h,cpp}`: `Point3`, `Line3`, `Plane3`, `Circle3`, `Arc3`,
  `BSpline3`, and `DeriVector3` (value and derivative, like `DeriVector2`).
- `planegcs/Constraints3D.{h,cpp}`: the constraint classes of 4.4, each an
  ordinary `GCS::Constraint` subclass with `errorgrad()`, new values in
  `GCS::ConstraintType` after the 2D ones.
- `GCS` gains `addConstraint...3D` helpers in a separate section, or a thin
  builder class beside it; the solve and diagnosis calls are unchanged.

### 4.3 Two rules for the equations

1. **Every constraint is rank-exact.** The QR diagnosis finds redundant and
   conflicting constraints by the rank of the Jacobian, so a constraint that
   removes k DOF must contribute k independent equations. The naive 3D forms
   are not: `d1 x d2 = 0` (parallel) is three equations of rank two, and the
   diagnosis would call the constraint partly redundant with itself. So:
   - a parallel-type condition is `d2 - s d1 = 0` with the ratio s an extra
     unknown: three equations for one more parameter, removing exactly two DOF
     whatever the directions' orientation (s < 0 is antiparallel). Changed in
     S1 from two cross product components picked by the directions as the
     system was built: S1's arc bend has a line swing from Y to Z in one
     solve, the picked pair lost rank there, and the solver collapsed the
     line (test `parallelSwingsOntoAnotherAxis`);
   - a point-on-curve or point-on-face condition takes the curve or surface
     parameter as an extra unknown: `P - C(t) = 0` is three equations for
     one more parameter, removing exactly two DOF, with plain derivatives
     (`addConstraintCurveValue`'s pattern, section 3).
2. **Branch-keeping.** 3D solves flip more easily [S 15]. Angles are measured
   with `atan2(|d1 x d2|, d1 . d2)` on (0, pi); an arc's side is its normal's
   sign, kept from the current state; drags move the target in small steps
   from the current solution, as the 2D drag does.

### 4.4 Entities and constraints

Entities, in the sketch's local frame:

| Entity | Parameters | Internal equations | DOF |
|---|---|---|---|
| Point | x, y, z | -- | 3 |
| Line segment | two points | -- | 6 |
| Workplane | origin (3), orientation as a unit quaternion (4) | \|q\| = 1 | 6 (v2); 0 in v1, where it is always fixed (section 6) |
| Circle | centre (3), normal n (3), radius | \|n\| = 1 | 6 |
| Arc | centre c, normal n, start s, end e (12) | \|n\| = 1, \|s-c\| = \|e-c\|, (s-c).n = 0, (e-c).n = 0 | 8 |
| B-spline | poles (3 each), weights; degree and knots fixed | endpoint = first / last pole; through-points at their knots, as in 2D | 3 per pole + weights |

Relations to a line are to the infinite line and relations to an arc to its
full circle, as in SOLIDWORKS [S 5] and our 2D sketch.

Constraints, first set (v1) and second (v2). "DOF" is what each removes.

| Constraint | DOF | Equations | Set |
|---|---|---|---|
| Coincident point-point | 3 | P - Q = 0 | v1 |
| Fix (lock) | all | parameters frozen, as 2D Block | v1 |
| Point on line | 2 | P - (A + t (B - A)) = 0, t unknown | v1 |
| Point on circle / arc | 2 | \|P - c\| = r, (P - c).n = 0 | v1 |
| Point on spline / model edge | 2 | P - C(t) = 0, t unknown | v1 |
| Point on plane (workplane, datum, planar face) | 1 | (P - O).n = 0 | v1 |
| Point on face (any) | 1 | P - F(u, v) = 0, u and v unknown, F from OCCT | v2 |
| Along X / Y / Z (the sketch frame's axes) | 2 | the other two components of d = 0 | v1 |
| Horizontal / Vertical on a workplane | 1 | as seen on it: d.v = 0 (d.u = 0) -- see S3b | v2 |
| Normal to a workplane | 2 | d parallel to n | v2 |
| Parallel line-line | 2 | d2 - s d1 = 0, s unknown (4.3) | v1 |
| Parallel to plane | 1 | d.n = 0 | v1 |
| Perpendicular | 1 | d1 . d2 = 0 | v1 |
| Collinear | 4 | both ends of L2 on L1 | v1 |
| Equal length / radius | 1 | \|d1\| - \|d2\| = 0 | v1 |
| Midpoint | 3 | P - (A + B)/2 = 0 | v1 |
| Tangent line-arc at a shared end S | 2 | d.(S - c) = 0, d.n = 0 | v1 |
| Tangent arc-arc, spline-line, spline-arc at a shared end | 2 | the two tangents parallel (4.3) | v1 |
| Tangent (G1) to a model edge at a shared end | 2 | as above, the edge's tangent at t | v2 |
| Smooth (G2) at a shared end | 7 | coincident (3), tangent (2), the curvature vectors equal across the tangent (2) -- see S3e | v2 |
| Coaxial (3D concentric) | 5 | centres coincident, normals parallel | v2 |
| Symmetric about a plane | 3 | midpoint on the plane, P - Q parallel to n | v2 |
| Distance point-point | 1 | \|P - Q\| - D | v1 |
| Distance along X / Y / Z | 1 | (Q - P).x - D (and so on) | v1 |
| Distance point-plane (signed) | 1 | (P - O).n - D | v1 |
| Distance point-line | 1 | \|(P - A) x d\| / \|d\| - D | v1 |
| Angle line-line (coplanar or not) | 1 | atan2(\|d1 x d2\|, d1 . d2) - theta | v1 |
| Radius / diameter | 1 | r - R | v1 |
| Arc / circle in a plane | 3 | centre on the plane, n parallel to its normal | v2 |
| Spline length | 1 | numeric arc length - L | later |

Deliberately not in 3D without a plane (as Inventor and SolveSpace):
horizontal / vertical, concentric, symmetric about a line. They come back as
the workplane-local forms in v2.

## 5. The object

**A separate object, `Sketcher::Sketch3DObject`** (decided 2026-10-26), not a 3D
option on the 2D sketch as Fusion does it [S 25]. Points are placed Fusion's way,
though (section 7). Reasons for the separate object:

- the 2D sketch is a `Part::Part2DObject`, and Pad, Pocket, Revolution,
  holes and the sketch's own projection rules all rely on it being planar;
  a 3D option would put that rule in every consumer;
- Fusion's own help warns its option makes off-plane picks easy to make by
  accident [S 30];
- separately, nothing in the 2D sketch's tested code changes.

It lives in the Sketcher module, sharing the library with PlaneGCS:

- base `Part::Feature` + `Part::AttachExtension` (the 3D sketch's frame is
  attachable; its geometry is in that frame);
- `Geometry`: `Part::PropertyGeometryList`, 3D coordinates, workplanes as
  `Part::GeomPlane` entries (construction, not in the output shape);
- `Constraints`: `Sketcher::PropertyConstraintList` with the 3D types
  appended to `Sketcher::ConstraintType` after `Text = 21` (recommended,
  for the naming, the driving / reference flag, datum editing and the
  expression binding for free; the 2D sketch then refuses the new types) --
  or a 3D constraint class of its own, question 13 c;
- `ExternalGeometry`: links to model vertices, edges, faces and datum planes,
  brought in **without projection** -- a 3D sketch takes an edge as it is,
  which is simpler than the 2D sketch's projection -- with negative geometry
  ids as in 2D, and their names kept through the element map as the 2D
  sketch keeps its external references;
- **2D sketch geometry may be included** as references (Inventor's Include
  [S 21]), so a planar profile drawn in an ordinary sketch can be tied to;
- the output `Shape`: the non-construction edges as wires where they
  connect, each edge named from its geometry id the way the 2D sketch names
  its edges, so a Pipe that follows Edge3 keeps following it after a solve;
- a Python API mirroring `SketchObjectPy`: `addGeometry` with 3D
  `Part.LineSegment` / `Part.ArcOfCircle` / `Part.BSplineCurve`,
  `addConstraint(Sketcher.Constraint('AlongZ', 0))`, `solve()`,
  `movePoint`, `setDatum` / `getDatum`. Everything is usable from Python
  and the MCP console before any GUI exists.

A solver facade `Sketcher::Sketch3DSolver` maps between the object and
PlaneGCS, as `Sketcher::Sketch` does for 2D.

**In a PartDesign Body** (decided): allowed, as a 2D sketch is.
`Body::isAllowed` (src/Mod/PartDesign/App/Body.cpp) gains the type, so an
AdditivePipe can follow it without a binder. It never changes the solid, so the
Tip rules do not apply to it. It works outside a Body too.

## 6. Workplanes inside the sketch (decided 2026-10-26: fixed in v1, constrainable in v2)

A workplane is a geometry entry with a position and orientation
(`Part::GeomPlane`, construction, never in the output shape). Uses:

- the plane new points are placed on (section 7);
- "a planar part of a 3D sketch": geometry put on a workplane by one
  constraint per point, so a rectangle drawn on a tilted plane stays flat;
- in v2, the reference that makes horizontal / vertical / normal, arcs and
  symmetric-about-a-line meaningful, through workplane-local constraints --
  SolveSpace's "projected" idea [S 50].

**v1: a kept plane, fixed.** The "Keep plane" command stores the manipulator's
current plane as a workplane, locked where it is.
- Activating it (double-click, or Ctrl+click as for a face) puts the
  manipulator on it. Everything drawn while it is active gets a point on plane
  constraint per point (the v1 constraint, against constants).
- It is moved by editing its placement values, like a datum plane's
  attachment offset; the solver never moves it, so it adds no solver
  parameters and nothing to the diagnosis.
- Without it, the manipulator's plane is a placement aid only, and a
  rectangle drawn 40 above the frame on a plane tilted 30 deg is four joined
  lines that twist out of plane when a corner is dragged.

**v2: constrainable workplanes**, like SOLIDWORKS' 3D sketch planes [S 7]:
the plane's own 6 DOF are solved (origin, unit quaternion with |q| = 1), and
constraints place it -- on a face, through three points, parallel to a datum,
at a distance or angle -- carrying its geometry along when it moves. The
workplane-local constraints of 4.4 come with it.

**The stored form is the same in both**: v1's plane is a v2 plane carrying a
Fix. Going to v2 removes nothing and converts no files.

## 7. Editing

Edit mode follows the 2D sketch's pattern: the 3D sketch's tools come up
from whatever workbench is active and go on leaving (docs/CadNewcomerWorkflow.md
section 6 item 4) -- an edit is a mode entered on purpose, so the global
switch is right here, unlike a TechDraw page's.

**Placing points: Fusion's manipulator** (decided 2026-10-26) [S 28].
- A **manipulator** (`Gui::SoFCCSysDragger`) stands at the sketch frame's
  origin when editing starts and **moves to each point as it is placed**.
  Clicking an existing point without placing one moves it there, which is how a
  path branches off the middle of another.
- Its **active plane** (the sketch frame's XY at first) is where a click lands.
  Clicking one of its plane handles picks XY / YZ / ZX. **Its rotation handles
  tilt it to any orientation**, typed angles included. Ctrl+click on a face,
  datum plane, LCS or workplane takes that plane (SOLIDWORKS [S 8]). The
  plane is a placement aid only: geometry put on it gets no constraint keeping
  it there (as in Fusion). Keeping a plane is section 6.
- **Hovering along one of its axes** locks the next point to that axis, and a
  typed distance places it. That makes "go up 40" one move.
- The switches are **commands**, so Tools > Customize can rebind them. They are
  active only while a 3D sketch is in edit:

  | Command | Does | Default |
  |---|---|---|
  | Cycle working plane | XY -> YZ -> ZX of the manipulator | Shift+Space (Inventor's [S 18]; Fusion's Alt cannot be a Qt shortcut alone) |
  | Lock height | keep the in-plane position, move along the plane normal | Up (Fusion's [S 28]); a navigation style's Up-arrow pan is lost during the edit |
  | Reset manipulator | back to the sketch frame's orientation | none |
  | Manipulator rotation handles | show / hide | none |

  Tab stays with the on-view value entries, as in the 2D sketch
  (`DrawSketchKeyboardManager.cpp`).
- **Snapping** to sketch points, model vertices, edges and faces, with the
  constraint it implies created on release: coincident to a vertex, point on
  an edge, point on a face; and Along X / Y / Z when a line is drawn within a
  few degrees of an axis (SOLIDWORKS creates Along relations from snaps
  [S 7]). The snap shows its constraint icon before the click.
- **Typed values** through the on-view entries: length and the three
  coordinate offsets of the next point. A dimension's kind (absolute / along
  X / Y / Z) is picked in its on-view entry (SOLIDWORKS uses Tab [S 6], which
  is taken here).

**Commands** (decided): in the Sketcher workbench, with one new "Create 3D
sketch" command. A 3D sketch in edit brings up the Sketcher's own toolbars,
with the same icons (drawn in teal, below) and shortcuts (G, L for a line, and so on); each command
runs its 3D handler when the edited object is a 3D sketch and is greyed out
where it has no 3D meaning yet (slots, polygons, ...). The shared part is the
commands and toolbars, not the drawing handlers, which are 2D throughout.

**Tools, v1**: point, line, polyline, through-points spline, control-point
spline, construction toggle, Keep plane (a fixed workplane, section 6), and
the constraint commands of the v1 set.
**v2**: arc, circle, 3D corner fillet (an arc tangent to two lines in their
plane -- the routing bend), constrainable workplanes, spline on face, trim /
split.

**Display** (decided): the same as the 2D sketch's edit geometry -- the same
drawing, styles and colours. Dimension labels lie in a plane through the measured
geometry chosen to face the camera (`EditableDatumLabel` takes that
placement); the DOF count and the conflicting / redundant lists are shown as
the 2D sketch's task panel shows them, clickable.

**Icons** (decided 2026-10-26). A 3D sketch's colour is teal where the 2D
sketch's is red, shade for shade (`#ef2929` -> `#4dd0e1`, `#a40000` ->
`#00838f`, ...). Teal rather than a light blue: the 2D sketch already uses
blue for construction geometry and reference dimensions, and both keep that
meaning here.
- Its own icon is the 2D sketch's square and circle laid flat in isometric
  view, the circle floating over the square's far corner; New 3D Sketch puts
  them on a flat sheet. Its own commands are drawn in the same isometric
  view, axes as FreeCAD's isometric view shows them (X down-right, Y up-right,
  Z up) in the manipulator's red, green and blue.
- The SVGs are written by `Resources/icons/make_sketch3d_icons.py`; change
  the script, not the files.
- The shared tools keep their SVGs: while a 3D sketch is in edit
  `SketcherGui::setSketch3DToolIcons` sets a teal colour mapping on their icon
  names, `_Constr` and `_Driven` variants included
  (`BitmapFactory::setIconColorMapping`), so a construction or reference mode
  switch during the edit is teal too. The commands that make or pick a sketch
  stay red.

**Drag.** A dragged point moves in the working plane, or along a triad axis
when one is held, through the solver's temporary drag constraints.

## 8. Output and its users

- PartDesign Pipe and additive / subtractive pipe spines, Part Sweep -- any
  edges already (section 3).
- The Surface workbench: Filling boundaries, Sections, GeomFillSurface today;
  the curve-network surfaces (Gordon) of the native Surface port next. The
  port goes first and takes any edges, so it does not wait for this; a 3D
  sketch then gives it parametric, constrained inputs.
- Routing: a polyline with corner fillets (v2) is a pipe or tube route,
  Onshape's Routing Curve's polyline-with-bend-radius case [S 47].
- Inside a PartDesign Body as a non-solid feature (decided, section 5).

## 9. The browser and the agent

The object, its solve and its Python API are App-side, so the served
document and the MCP console get them unchanged. The edit mode in the
browser follows when the 2D sketch's served edit does (docs/ThinClient.md);
not in the first phases.

## 10. Phases and tests

| Phase | What | Tests | Size |
|---|---|---|---|
| S0 | PlaneGCS 3D layer: entities and the v1 constraints | GoogleTest under `tests/src/Mod/Sketcher/App/planegcs/`: each constraint's error is zero at a satisfying configuration; every gradient against finite differences; DOF counts of canonical cases (a free point 3, a line 6, a line Along Z 4, a closed rectangle in space ...); redundant and conflicting sets reported; convergence from perturbed starts | M |
| S1 | `Sketch3DObject`, the solver facade, external references, the output shape, the Python API, save / restore | Python tests: a Pipe along a constrained 3D path; edge names stable across a solve and a dimension change; an expression driving a dimension; a document saved and reopened; a reference to a model edge followed through a TNP change | M to L |
| S2 | GUI v1: edit mode, the v1 tools, the manipulator and its switching commands, fixed workplanes, snapping with implied constraints, on-view values, drag, diagnosis display | GUI tests through `scripts/gui-test.sh`, as the 2D sketch's | L |
| S3 | v2: arcs and circles, constrainable workplanes and their local constraints, corner fillet, spline and point on face, G1 / G2 to model edges, coaxial, symmetric | per feature, as S0 to S2 | L |
| S4 | the served edit in the browser | the web gate | M |

S0 and S1 deliver something usable on their own (scripted 3D paths with
constraints, which the Surface port and the agent can use) before any GUI.

**S0 done 2026-10-26.** `planegcs/Geo3D.{h,cpp}` (Point3, DeriVector3, Plane3,
Circle3, Arc3, BSpline3, FixedCurve3 for model edges) and
`planegcs/Constraints3D.{h,cpp}` (13 one-equation classes, the ConstraintType
values from 100, and one `addConstraint*3` function per sketch constraint
adding as many equations as the DOF it removes; coincident, along an axis,
distance along an axis and circle radius reuse the 2D Equal and Difference, so
the solver's Equal reduction applies). Tests:
`tests/src/Mod/Sketcher/App/planegcs/Constraints3D.cpp`, 11 cases:
- every class's gradient by every parameter against central differences;
- the B-spline tangent against differences of its value;
- the DOF of a constrained point pair, a line along Z, a circle and an arc;
- every v1 constraint's DOF drop against the table, with nothing reported
  partly redundant;
- a rectangle on a tilted workplane (5 DOF, and the fourth right angle
  reported redundant);
- parallel staying rank-exact along each axis and a diagonal;
- redundant and conflicting sets reported;
- convergence from perturbed starts: a routed path, keeping the branch it
  started on (20 starts); curves, planes and dimensions (10 starts); an arc
  tangent to two lines;
- the timing case below.
All 141 Sketcher C++ tests pass.

Not in S0: workplane-local constraints, point on face, G1 / G2 to model edges,
coaxial and symmetric (all v2); periodic splines.

**S1 done 2026-10-26.**
- `Sketcher::Sketch3DObject` (`App/Sketch3DObject.{h,cpp}`):
  - It is a `Part::Feature` with `AttachExtension` (the 3D attach engine).
  - Properties: `Geometry`, `Constraints` (the 2D sketch's
    `PropertyConstraintList`), `ExternalGeometry` (links), and a hidden
    `ExternalGeo` holding the references as geometry in the sketch's frame, kept
    in the file so a document opens with valid constraints before a recompute.
  - Geometry ids: -1, -2, -3 are the frame's X, Y, Z axes (-1 start is the
    origin), -4, -5, -6 its XY, YZ, ZX planes, and -7 down the external
    references.
  - The output shape names each edge `g<geometry id>` and its ends
    `g<id>v1` / `v2`, as the 2D sketch does.
  - The shape always takes `Placement` (`shouldApplyPlacement`). Outside a
    recompute, `Part::Feature` otherwise takes the placement from the shape,
    and a Python solve reset the sketch's frame.
  - A solution whose geometry cannot be built (a collapsed line) is not
    applied, as the 2D sketch undoes an invalid one.
- `Sketcher::Sketch3DSolver` (`App/Sketch3DSolver.{h,cpp}`) maps the
  constraints. Coincident, PointOnObject (line, circle, arc, spline, plane,
  external curve), AlongX / Y / Z (new types), Parallel and Perpendicular
  (lines, line and plane), Tangent (two lines: collinear; end to end), Equal,
  Symmetric about a point (midpoint), Distance (line, two points, point to
  line or plane), DistanceX / Y / Z (DistanceZ new), Angle, Radius, Diameter,
  Block, and the B-spline control and knot point alignments. Anything else is
  reported in `Malformed`, and the solve returns -5. Spline weights and knots
  are kept, poles solved; periodic splines are not solvable yet.
- The 2D sketch refuses the four new types: `addConstraint` raises
  ValueError, and the 2D solver treats one from a file as malformed.
- The Python API is `Sketch3DObjectPy`:
  - methods: `solve`, `addGeometry`, `delGeometry`, `addConstraint`,
    `delConstraint`, `setDatum` / `getDatum` (index or name), `setDriving`,
    `setConstruction`, `addExternal`, `delExternal`, `movePoint`;
  - attributes: `DoF`, `Conflicting`, `Redundant`, `PartiallyRedundant`,
    `Malformed` (0-based constraint indices).
- A PartDesign Body accepts the type (by name; PartDesign does not link
  Sketcher).
- Tests: `SketcherTests/TestSketch3D.py`, 15 cases:
  - a constrained path solves to 0 DOF;
  - the diagnosis (DOF, redundant, conflicting, malformed);
  - a rectangle on a tilted workplane;
  - an arc tangent bend;
  - a Part Sweep, and a PartDesign AdditivePipe in a Body, along the path;
  - an edge reference following its geometry through a dimension change and
    through a lead-in that comes first in the wire;
  - an expression driving a named dimension;
  - a reference dimension;
  - save and reopen;
  - an external edge followed through a cut that renumbers the edges;
  - a datum plane and the frame's plane with the sketch moved;
  - a drag;
  - a spline through its knot points;
  - the 2D sketch refusing the new types.
- Regression: Sketcher C++ 142/142 (one more, the parallel swing),
  TestSketcherApp 161 OK, TestPartDesignApp 481 OK.

**S2 done 2026-10-26** (the GUI's first version), in five steps:
`ViewProviderSketch3D` and its Python binding, `CommandSketch3D.cpp`, and a
one-line branch in each Sketcher creation and constraint command.
- S2a, edit mode. The Sketcher workbench and its edit toolbars come up through
  TempoVis, as for a 2D sketch. Geometry is drawn in the 2D sketch's edit colours
  (the same View preferences). The task panel shows the solver's state. There is
  a Create 3D Sketch command, into the active Body.
- S2b, the manipulator:
  - axes, three plane handles, and the CSys dragger's rotation rings, about
    70 px;
  - a click lands on the active plane, on an axis when the pointer runs along
    it, or on the normal with the height locked. Those points are computed in
    doubles: the viewer's getPointOnLine was 0.16 mm off its line;
  - commands on a Sketcher 3D toolbar shown in this edit only: Cycle Working
    Plane (Shift+Space), Lock Height (Up), Reset, Rotation Handles, Keep Plane,
    Along X/Y/Z, Distance Z.
- S2c, tools. Point, Line, Polyline, and the B-spline by poles and through
  points, started by the Sketcher's own commands; Toggle Construction.
  - Snapping implies a constraint: sketch points first, by screen distance;
    sketch curves; and the model element the view preselected, made an
    external reference.
  - Along X/Y/Z goes on a segment drawn within 2 deg of an axis.
  - Escape or a right click finishes what is placed, and every addition is one
    undo step.
- S2d, constraints.
  - Selection in the edit (points, curves, labels); the Sketcher's constraint
    commands make their 3D constraint of it; Delete removes it.
  - Dimension labels are SoDatumLabel in a plane through what each measures.
  - Values are edited in place on Gui::DatumValueEditor, as the 2D sketch does:
    Enter, Tab, a press elsewhere, expressions, reference, name. A new
    dimension and its value are one undo step.
  - Found and fixed on the way: a bare number typed into that editor gave
    -2147483647, in the 2D sketch too (1d761d7459).
- S2e, drag and planes:
  - points and curves dragged through the solver;
  - Keep Plane, a fixed workplane (section 6, v1) that holds points placed
    while it is active;
  - Ctrl+press on a kept plane, a planar face or a datum plane;
  - the panel's constraint numbers select the constraints.
- Tests: five GUI tests, `tests/gui/sketch3d-{edit,manipulator,tools,constraints,
  drag-plane}.py`, 112 checks. They drive the view provider's input entry
  points (press, dragTo, pointAt, toView, startTool), which the mouse handlers
  call, so no synthetic Qt event is needed.

Not in S2, left for later (all closed since, below S3e):
- typed values while placing (length, coordinate offsets) and a dimension's kind
  picked in its entry;
- typed angles for the manipulator;
- icons for the non-dimensional constraints;
- labels re-facing the camera when it turns (they face it as it was at the draw);
- the browser (S4).

**S3 is done in steps.** S3a, circles and arcs (2026-10-27):
- Four tools, started by the Sketcher's own commands:
  - Circle From Center: the centre, then the rim, in the working plane;
  - Arc From Center: the centre and the start in the working plane, then the
    end. The arc runs the way the pointer swept round from the start, as the 2D
    arc does;
  - Circle From 3 Points, Arc From 3 Points (start, end, a point on it): in the
    plane the three points span, so a tilted circle needs no working plane.
- Their snaps tie them as a line's ends are tied: a centre, start or end
  clicked on a sketch point is coincident with it, and a point a curve was
  drawn through lies on it.
- On a kept plane, a circle or arc held by no snap is put in the plane: its
  centre on the plane and the curve parallel to it, 3 of its DOF.
- While a rim, a start or an end is picked, the manipulator's axis along the
  curve's normal does not lock. That point is taken into the curve's plane, and
  the lock put it back on the centre: in the isometric view, an end straight
  below the start on screen is on the Z axis.
- A circle or arc stands for its plane in Parallel and Perpendicular, as a
  workplane does:
  - a line parallel to it lies along that plane;
  - a line perpendicular to it runs along its axis;
  - two of them compare normals.
- Coincident of two circles or arcs makes them coaxial (4.4): the centres
  coincident and the two parallel.
- Parallel's ratio s starts at |d2| / |d1|, signed by d1 . d2. It started at
  the projection d1 . d2 / |d1|^2, which is 0 for directions square to each
  other. There d2 - s d1 has no gradient to turn d2, and a unit normal cannot
  shrink, so making two square circles coaxial stalled.
- Tests:
  - `tests/gui/sketch3d-arcs.py`, 48 checks;
  - `Constraints3DSolve.parallelFromPerpendicular`, which fails with the old
    start.

Found on the way: a click on the working plane went through the viewer's float
ray-plane hit and landed up to 1e-6 off the plane, so a drag on XY now and then
left z = 1.7e-6 (the drag-plane test failed one run in three). It is computed
in doubles now and projected onto the plane, as the axis lock was.

S3b, workplanes solved for (2026-10-27):
- A workplane is a frame of 6 DOF: origin, and orientation as a unit
  quaternion. It is solved for unless it carries a Block, which is the v1
  plane: Keep Plane now adds that Block (section 6, "v1's plane is a v2 plane
  carrying a Fix"). Its X and Y axes are what Horizontal and Vertical on it
  mean.
- What places a free plane: its origin (geometry position 3) coincident with
  a point; point on it; Distance from a point; Parallel or Perpendicular to a
  line, circle, arc or another plane; Angle to another plane, circle or arc
  (between their normals).
- Being a frame, a plane through three points keeps 3 DOF -- its slide in
  itself and its spin -- until its origin and one axis are tied: the origin
  coincident with a point, and Horizontal of a fixed line on it. That follows
  from the ruled 6-DOF frame; a plane as a bare surface (3 DOF) would have no
  axes for Horizontal and Vertical.
- Horizontal / Vertical on a workplane are one equation, as seen on the
  plane (SolveSpace's projected form [S 50]): the line has no run along the
  plane's Y (X). The table had two (d.v = 0 and d.n = 0), but a line whose
  ends are held on the plane already has d.n = 0, so every Horizontal on a
  rectangle drawn on a kept plane would have been redundant. Stored as
  `Constraint('Horizontal', line, plane)`; the 2D sketch refuses that form.
- In the edit:
  - Horizontal / Vertical take the plane selected with the lines, else the
    active kept plane; with neither they say so;
  - Block on blocked geometry frees it, so a kept plane is made
    constrainable by selecting it and pressing Block;
  - Symmetric of two points and a plane mirrors them in it;
  - Angle of two planes, circles or arcs is a dimension of their normals,
    with its label;
  - a free plane drags; an active kept plane moved by a solve keeps the
    manipulator on it.
- The dot products these use have no gradient where the answer is a quarter
  turn away with nothing else free to move (a blocked line, a plane that must
  spin 90 deg). There the local diagnosis reads it as conflicting. A plane
  started near its answer, as one drawn is, solves.
- Tests:
  - `tests/gui/sketch3d-workplanes.py`, 22 checks;
  - four Python cases: a free plane following its constraints, symmetric
    about a plane, a rectangle horizontal and vertical on a tilted plane with
    nothing redundant, two planes at an angle;
  - the 2D sketch refusing the workplane Horizontal.

S3c, corner fillet (2026-10-27):
- `Sketch3DObject.fillet(geo1, pos1, geo2, pos2, radius)`: two lines whose
  given ends meet are rounded by an arc in their plane, tangent to both. The
  lines are trimmed back to where it touches, and the corner's coincidence
  becomes the arc's tangency at both ends (end to end, so coincident too). No
  plane constraint is needed: tangency at both ends keeps the arc in the
  lines' plane. Like the 2D fillet, it adds one DOF, the radius.
- In the edit, the Sketcher's Fillet command takes either selection:
  - two lines that meet;
  - the corner point;
  - with nothing selected it starts a tool, and a click on a corner fillets
    it.
- The radius starts with the tangent points a third of the shorter line from
  the corner, and a Radius dimension is added, its value typed next. The
  fillet and its value are one undo step. Unlike the 2D fillet, which is
  sized by the pointer, a selection has no drag to size it by. The dimension
  can be deleted for a free radius.
- Tests:
  - `tests/gui/sketch3d-fillet.py`, 16 checks;
  - two Python cases: one corner, and the pipe route with both bends filleted
    and fully constrained again, swept.

S3d, faces (2026-10-27). The curve on a face is a new geometry, as decision f
asked: a 2D curve in the face's (u, v), so it lies on the face exactly.
- Part (`Part.CurveOnSurface(curve2d, surface)`, `GeomCurveOnSurface`):
  - it is a curve like the others: evaluated as S(c(t)) with derivatives by
    the chain rule to the third, so projection, approximation, transform,
    copy and reverse work through it;
  - `toShape()` makes the edge from the 2D curve on the surface, carrying it
    as the edge's curve on that surface, with a 3D curve built to match;
  - it saves in OCCT's own text form, which holds any surface and 2D curve.
  Part's surfaces could not be saved at all ("Not yet implemented"); they now
  are, in the same form, so a curved face as a reference keeps in the file.
- A curved face as a reference is its surface. Point on object puts a point on
  it (P - S(u, v), u and v unknown, 1 DOF), and a snap onto a curved face in
  the edit now does that. It used to take the position only.
- A reference that cannot be built (its element gone) keeps its geometry as it
  was. It used to be dropped, which moved every reference after it down one
  id.
- In the sketch, the curve on a face is solved in (u, v):
  - its poles' u and v are the unknowns;
  - put on a face with PointOnObject(curve, none, face), it is solved on that
    face's surface and follows it; without, on its own surface;
  - its ends, point on it, its knot points and Tangent work as on any curve.
- Two curves on the same face join in (u, v): a coincidence is their end poles
  equal (2 equations), and a tangency adds their end pole pairs parallel (1
  more). In 3D the same coincidence removed 3 DOF away from the solution and
  left one equation implied at it.
- The edit has Spline on Face (Sketcher 3D toolbar):
  - points clicked on one model face, then Escape: a spline through them in
    the face's (u, v), with u carried across a periodic surface's seam the
    short way;
  - the face becomes a reference and the spline is put on it;
  - its points are sketch points at its knots, so dragging one reshapes it on
    the face.
- A limit shared with every 3D tangency: Parallel is d2 = s d1, which s = 0
  meets when a curve's end tangent collapses. A line started square to a face
  and made tangent to a curve on it was met that way, with the curve's first
  two (u, v) poles merged. Drawn roughly along the curve, as a G1 line is, it
  solves as meant.
- Tests:
  - Part: `parttests/CurveOnSurfaceTest.py` (6) and `GeometrySaveTest.py`;
  - planegcs gtests for the new constraint's gradients, the tangent's value
    and a solve on a saddle;
  - seven Python cases;
  - `tests/gui/sketch3d-face-spline.py`, 20 checks.

S3e, continuity to model edges (2026-10-27), which completes S3:
- Tangent of a curve's end and a model edge (`Constraint('Tangent', geo, pos,
  edge)`) puts the end on the edge at a solved parameter t and makes the
  tangents parallel there: 4 DOF, G1 to the edge. It takes any edge, including
  one the solver knows only through OCCT; the edge's tangent at t comes from
  OCCT with its derivative by t.
- Tangent end to end with a model edge's end works for every kind of edge.
- Smooth (`Constraint('Smooth', g1, p1, g2, p2)`, a new type the 2D sketch
  refuses; a command on the Sketcher 3D toolbar) is G2 at a shared end. It
  is 7 equations: joined (3), tangent (2), and the two curvature vectors
  k = (C'' - (C'' . u) u) / |C'|^2 equal along two directions across the
  tangent (2). The table had 3; in 3D a curvature vector has two components
  across the tangent, not one as in 2D. It takes lines, arcs and splines of
  the sketch and any model edge. B-splines gained a second derivative, from
  the basis functions' derivatives (The NURBS Book, A2.3). The two curvature
  equations take their gradient numerically: written out by every pole, the
  second derivative's own derivatives are not worth it for two equations.
- Tests:
  - gtests for the spline's second derivative against differences, the
    edge tangent's gradient, and a spline made smooth to a helix (curvature
    r / (r^2 + pitch^2), nothing redundant at the solution);
  - five Python cases: a line tangent to a model arc along it and to a 3D
    spline edge; a spline smooth to a model edge's end; a spline smooth to an
    arc (curvature 1 / r); the 2D sketch refusing Smooth;
  - `tests/gui/sketch3d-continuity.py`, 9 checks.

S3 left nothing of its list.

The S2 leftovers, closed (2026-10-27):
- Icons for the constraints without a value, drawn as the 2D sketch draws
  them, by what they bind; a press on one selects it, Delete removes it.
  Coincidences show none, as in 2D. The task panel lists every constraint in
  step with the view's selection, so a coincidence is selected and deleted
  there.
- The dimensions and the icons are in the 3D sketch's teal, with colours of
  its own on the Sketcher appearance page. They apply at once.
- Colours that read on every background (2026-10-27). No one colour has more
  than about 1.7:1 contrast against all three built-in themes' backgrounds
  (Classic's blue-grey gradient, Dark #3c3c3c, Light #c3c3c3), so each label
  and icon has a 1 px outline (ConstraintOutlineWidth, 0 for none), and there
  are two colour sets, picked from the view's background: a light fill with a
  dark outline on a dark background, a dark fill with a light halo on a light
  one. The background is light when the average relative luminance of its
  colours (the flat colour, or the gradient's two or three) is above 0.35
  (`SketcherGui::viewBackgroundIsLight`); a change to it re-colours an open
  edit at once.
  - 3D: Sketch3DDimColor and Sketch3DIcoColor #4dd0e1 with
    Sketch3DOutlineColor #00292d; on a light background the `...Light` keys,
    #00575e on #eef9fa.
  - 2D, the same outline on its dimensions, symbols and the tools' on-view
    parameters: ConstrainedDimColor and ConstrainedIcoColor as before with
    ConstrainedOutlineColor #280000; on a light background #a01800 on
    #fff4f2.
  - The outline is `SoDatumLabel::outlineColor`/`outlineWidth`: the text
    stroked under its fill, and the leaders drawn once wider in the outline
    colour (in the render-cache path, a companion leader node); the icons'
    silhouette is dilated by the width.
  - In the render-cache path (mode 3) a highlight recolours a draw by its
    material, which a REPLACE texture ignores. So the two-colour glyph and
    icon images are drawn BLEND instead: the texel is each pixel's place
    from the fill (0) to the border (1), the material is the fill and the
    texture's blendColor the border. A preselected label or icon is then
    the highlight colour inside its own border. The label knows its two
    colours; a captured SoImage's are found from its pixels
    (`twoToneImageColors`, beside the one-colour `flatImageColor`), and the
    border is the colour its outer rim leans to.
  - Bordered labels make a fit's bounds a pixel or two larger, which moves
    where a fit puts a line on the pixel grid; a test that reads one pixel
    of a line reads the best within one pixel of it.
- Labels and icons are placed again when the camera moves.
- Typed values while placing. A number typed into the view while a tool
  places points opens an entry; the edit takes the key before the standard
  views' digit shortcuts do. Then:
  - one value places the next point that far from the manipulator toward the
    pointer; a line drawn so gets that length as a Distance, in the line's own
    undo step;
  - three values (dx, dy, dz) place it at that offset in the manipulator's
    frame, with DistanceX/Y/Z for the non-zero ones while that frame is the
    sketch's.
- A dimension's kind picked in its entry: "x 20", "y 20", "z 20" (or "d 20"
  back) makes a line's or two points' distance that kind, the value signed as
  the geometry measures it now.
- Typed angles: after a rotation ring turned the manipulator, an entry takes
  the angle about that ring's axis, exactly.
- Found on the way: a pick on the working plane seen edge on threw "No
  intersection found" from the viewer's fallback; it now gives no point.
- Tests:
  - `tests/gui/sketch3d-constraint-icons.py`, 16 checks;
  - `tests/gui/sketch3d-typed-values.py`, 24 checks.

Open: S4, the browser.

## 11. Risks

- **Solve stability in 3D** [S 15]: answered by rank-exact equations,
  branch-keeping (4.3), and the S0 convergence tests from perturbed starts.
- **Speed** on large 3D sketches: PlaneGCS evaluates gradients per
  parameter; Dune 3D had to patch SolveSpace's solver for speed [S 71]. The
  S0 tests include a 1000-entity case timed against the 2D sketch of the
  same size. Measured 2026-10-26, a chain of lines each joined to the last
  with an axis direction and a length (`FC_S0_CHAIN=1000`; the suite runs 300):

  | Chain | 3D diagnose + solve | 2D diagnose + solve |
  |---|---|---|
  | 300 lines | 449 + 45 ms | 104 + 42 ms |
  | 1000 lines | 33.2 + 2.3 s | 10.2 + 2.0 s |

  **Solving costs about the same; the diagnosis is the difference**, and it is
  slow in 2D already at this size. So `errorgrad` is not the next step. The
  next step is the diagnosis's sparse QR, where the 2D sketch's big cost
  already is. Its time grows faster than the cube of the size here (300 ->
  1000 lines: 74 times in 3D, 98 times in 2D), and 3D has 1.5 times the
  unknowns per entity.
- **The diagnosis is local.** The QR diagnosis is taken at the current
  shape, before the solve, as in 2D. Constraints added all at once to a shape
  far from their solution can be reported conflicting there: S1's arc bend,
  added in one call to an arc drawn a quarter turn from where it ends, is;
  added one at a time, as the editor adds them, it solves to 0 DOF. The 2D
  sketch shares this. Related: AlongX / Y / Z equate two of a line's end
  coordinates, so a line drawn across the axis starts the solve at zero
  length, where a length has no gradient.
- **Shared enum**: appending 3D types to `Sketcher::ConstraintType` touches
  code that switches over it, in a file shared with upstream. A separate
  enum avoids it at the cost of re-doing the naming and datum plumbing
  (decided: the shared enum, 13 c).
- **Scope pull** toward a CATIA-GSD-style wireframe workbench, which #14010's
  commenters ask for [S 76]: the 3D sketch is the constrained-curve piece;
  intersection curves, projections and curves on surfaces as features
  belong to the Surface workbench.

## 12. Not proposed

- A plugin interface for an optional GPL SolveSpace backend (the Assembly3
  pattern [S 57]): two solvers means two diagnosis behaviours to support;
  one solver, ours.
- Turning the 2D sketch 3D (section 5).
- A whole-model constraint system like SolveSpace's: the 3D sketch is one
  feature in the parametric loop, like the 2D one.

## 13. Questions and decisions

Decided 2026-10-26:

a. A separate `Sketch3DObject`, with Fusion's way of placing points (section 7).
b. Allowed inside a PartDesign Body as a sketch is; no Tip rules (section 5).
c. `Sketcher::Constraint` and `PropertyConstraintList` reused, the 3D types
   appended to the enum.
e. Through-points spline first, control-point right after.
f. Curves pinned to a face ("point / spline on face") are 3D sketch geometry,
   v2, built in the face's (u, v) so they lie exactly on it. Projection and
   wrap of a flat sketch onto faces are Surface workbench features.
g. Commands in the Sketcher workbench, sharing its toolbars and shortcuts
   (section 7).
h. Display the same as the 2D sketch.
i. The manipulator plane tilts freely; Ctrl+click takes a face's plane.
j. Plane switching and height lock are customisable commands, active only in
   3D sketch edit (section 7).
k. The manipulator moves to the last placed point, or to a point clicked.
d. Workplanes: fixed ("Keep plane") in v1, constrainable in v2, the same
   stored form (section 6).

Nothing is open.
