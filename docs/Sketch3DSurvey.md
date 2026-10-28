# 3D sketch survey: how CAD programs do it, and which solvers we could use

Date of survey: 2026-10-26. Companion to docs/Sketch3D.md (the design), which cites it as [S n].

Conventions:
- `[n]` is a numbered source in section 6.
- "(search summary)" on a source means only a search-engine summary was seen, not the page itself.
- "(gk)" marks general knowledge that no source here confirms. Treat it as a lead to check.
- "(analysis)" marks this survey's own inference from the cited facts.

---------------------------------------------------------------------------

## 1. Summary table per CAD program

| Program | 3D geometry | Notable 3D constraints | How points are placed | Planar sub-sketch inside 3D sketch | Main uses |
|---|---|---|---|---|---|
| SOLIDWORKS 3D Sketch | points, lines, centerlines, all arc/circle/rectangle tools, splines, spline on surface (3D only), face curves, intersection curve, convert entities, fillet, chamfer. No patterns, no offset [3] | Along X/Y/Z (global), Horizontal/Vertical/Normal (local to a 3D sketch plane), Parallel line-plane, ParallelYZ, ParallelZX, AlongZ, On Plane, On Surface, Tangent to Face, Equal Curvature, Normal To, symmetric, midpoint, equal, concentric, Fix, Pierce [4][5][7] | Tab cycles XY/YZ/ZX; Ctrl+click on plane/face/UCS reorients; space handle; snap to model vertices/faces; sketcher triad [2][7][8][10] | Yes: 3D sketch planes, each with relations and a defined state; "3D Sketch on Plane" [7][9] | weldment frames, routing paths, mesh reverse engineering [1] |
| Autodesk Inventor 3D Sketch | lines, arcs, bends, circles, ellipses, elliptical arcs, interpolation and control-vertex splines, fixed splines, equation curves, points, helical curve, curve on face, silhouette, intersection, project-to-surface curves [20][22] | Coincident (also to surfaces), Collinear, Parallel, Perpendicular, Parallel to X/Y/Z axis, Parallel to XY/YZ/XZ plane, On Face, Equal, Smooth (G2), Tangent, Fix. Concentric, Horizontal/Vertical, Symmetric are 2D only [16][17] | 3D space indicator with planes; Shift+Space; Ortho mode along X/Y/Z; precise XYZ input; 3D Transform triad [18] | Not as such; 2D geometry is brought in by Include Geometry [21] | wiring, tubing, sweep paths, lofts, surface edges [19] |
| Autodesk Fusion (3D Sketch option) | line, arc, spline, rectangle, circle, ellipse, point, text, conic [27] | Horizontal/Vertical, Coincident, Tangent, Equal, Parallel, Perpendicular, Fix/Unfix, Midpoint, Concentric, Collinear [26] | 3D Sketch Manipulator: plane picks XY/YZ/ZX, axis restriction, rotation handles; Alt cycles planes, Up Arrow locks XY to change Z [28] | A 2D sketch with the 3D option turned on; the manipulator plane acts as a temporary sketch plane [25] | tubing, sweeps, lofts, surface edges [25] |
| Solid Edge 3D Sketch | draw commands "similar to 2D sketching", plus Routing Path (ST10) [32] | mostly the 2D set, plus Coaxial and On Plane [32] | triad; z cycles axis lock, x cycles plane lock, s locks to an edge; Lock Sketch Plane; "Relative to" coordinate system [32] | Lock to planes/faces; no separate sub-sketch object found | sweep, BlueSurf, sheet metal, assembly relations, tube/piping/frame routing [32] |
| Siemens NX | no 3D sketcher; associative lines/arcs/conics/splines in 3D; Studio Spline; Curve on Surface (spline, line, arc, circle) [33][34][35] | Per-feature: studio spline point/pole constraints with slope and curvature; curve-on-surface continuity [34][35] | Feature dialogs (gk) | n/a | curve networks, surfaces (gk) |
| PTC Creo | Style (ISDX): free, planar, curve-on-surface (COS) curves by interpolation or control points; datum curve through points [37] | Tangent directional constraints Fix Angle, Horizontal, Vertical, Normal, Align; G1/G2/G3 to surfaces; soft points [38][39] | Active datum plane; soft points on references [38] | Planar curve type (curve bound to a plane) [37] | freeform surfacing (gk) |
| CATIA V5 / 3DEXPERIENCE | GSD wireframe features (spline, points, lines, planes, ...); FreeStyle 3D Curve [42][43] | Spline per-point tangent (explicit line/plane or from curve), tension, curvature; geometry on support [42] | Points must exist or be picked; FreeStyle active plane [42][43] | n/a | surfacing / GSD (gk) |
| Onshape | no 3D sketch; 3D Fit Spline, Bridging Curve, Routing Curve features [45][46][47][48] | End conditions: match position/tangent/curvature/flow (G3); per-point XYZ offsets from references [45][46][47] | Routing Curve triad (3 axes, 3 rotations, 3 planes), CSV, sampled edges [47] | n/a | routing, wiring, sweep/loft guides [45][47] |
| SolveSpace | everything is 3D: points, lines, circles, cubic Beziers free in 3D; arcs only in a workplane [50] | every constraint has a 3D and a "projected into workplane" version; point on plane/face, same orientation, distance point-plane, angle between vectors [50] | active workplane or "Anywhere In 3d" [50] | Yes: workplanes inside a "Sketch in 3d" group [50] | whole model is constraint-driven (gk) |
| Rhino / Plasticity / Shapr3D | free 3D curves (Rhino, Plasticity); Shapr3D sketches are planar [51][52][53] | none persistent (Rhino gk); Plasticity 2026.2 Studio-only, not saved [52] | Rhino elevator mode, Planar mode, osnaps [51] | n/a | freeform modelling |

---------------------------------------------------------------------------

## 2. Per-program notes

### 2.1 SOLIDWORKS 3D Sketch

Geometry (Q1):
- Usable tools: all arc, circle and rectangle tools, lines, points, splines. "Spline on Surface is only available in 3D". Also centerline, Convert Entities, Face Curves (iso-parametric), sketch fillet and chamfer, Intersection Curve, trim, extend, construction geometry. Patterns and Offset are not available [3].
- Offset on Surface is a 3D-sketch tool that offsets model edges and faces onto surfaces [1].
- Virtual sharps work in 3D [1].

Relations (Q2):
- "Many relations available in 2D sketches are available in 3D sketches" [4]. Additions: perpendicular through a point on a surface; relations between entities on different sketch planes; spline-handle relations (along an axis, between handles); tangent or equal curvature between faces and splines; Normal To between a line and a plane, or two points and a plane; cone-to-line concentric and perpendicular [4].
- The global/local axis split is explicit. "Relations to the global axes are called AlongX, AlongY, and AlongZ. Relations that are local to a plane are called Horizontal, Vertical, and Normal" [5].
- Snapping while sketching creates these: snapping to X/Y/Z gives Along X/Y/Z; on a 3D sketch plane it gives Horizontal/Vertical [7].
- 3D-only or 3D-specific relations in the table: Parallel (line to plane or planar face), ParallelYZ, ParallelZX, AlongZ (normal to a selected plane), On Plane, On Surface, Tangent to Face, Torsion Continuity, Pierce [5].
- Note from the same page: relations to a line are to the infinite line, and relations to an arc are to the full circle [5].

Dimensions (Q3):
- 3D dimension types are Absolute, Along X, Along Y, Along Z. While placing a dimension, Tab once gives Along X, twice Along Y, three times Along Z, and further presses return to absolute [6].
- Dimensioning on a 3D sketch plane offers absolute, horizontal and vertical [1].

Placement (Q4):
- A 3D sketch starts on the Front plane. Tab switches between the default planes, and the origin of the current plane is shown. Ctrl+click on a plane, planar face or coordinate system rotates the 3D sketch planes to it [2][8].
- The space handle "appears when the first point of a sketch entity is defined"; you use it to select the axis to sketch along [8].
- Line snap to model surfaces, vertices and sketch points. It is disabled while sketching along a major coordinate direction. An off-plane snap shows a temporary 3D box [10].
- 3D splines: "Each time you release the left button, a new 3D origin is created", and Tab changes planes per point [11].
- Sketcher Triad limits drag of entities [7].
- Mirror Entities works in 3D, about any plane or planar face [7].

Planar sub-sketches (Q5):
- 3D sketch planes can be added inside a 3D sketch. They can be fully, over or under defined, carry relations, be dragged, and be activated by double-click. Entities not on the active plane are muted [7].
- "3D Sketch on Plane" starts a 3D sketch in a 2D mode. Inside it, more relations are inferred and snapping is limited to in-plane geometry, with stated exceptions [9].

Model references (Q6): Convert Entities, Intersection Curve, Face Curves, Spline on Surface, Offset on Surface, On Edge, Pierce [3][5].

Uses (Q7): weldment structural members along a 3D frame, routing paths, reverse-engineering meshes [1].

Splines (Q8):
- Editing works as in 2D plus a Z coordinate, with Add Tangency Control and Insert Spline Point [12].
- Tangent and Equal Curvature to adjacent faces [13]; Torsion Continuity to splines, arcs, conics and model edges [5].

Pitfalls (Q9):
- In 3D there is no projection, so parallel means truly parallel [14].
- Nested contours in a 3D sketch are not subtracted from extrusions [14].
- A forum thread quotes a SOLIDWORKS author: 3D sketches are harder to solve, and relations fail or flip more often [15] (search summary).

### 2.2 Autodesk Inventor 3D Sketch

Geometry (Q1):
- The API collections of a 3D sketch are: Lines3D, Arcs3D (arcs and bends), Circles3D, Ellipses3D, EllipticalArcs3D, Splines3D, ControlPointSplines3D, FixedSplines3D, EquationCurves3D, Points3D, HelicalCurves, OnFaceCurves, IntersectionCurves, SilhouetteCurves, ProjectToSurfaceCurves. There is also an Include method [22].
- Helical Curve: pitch and revolution variants, natural or flat ends, taper [20].
- Curve on Face: an interpolation spline whose points are coincident to the face [20].
- Silhouette Curve: from a body plus a pull direction [20].
- Intersection Curve [20].
- Control-vertex splines have a control frame and can be converted to interpolation. Picking a part vertex creates an adaptive work point [24] (search summary).

Constraints (Q2):
- API list: Coincident, Collinear, Custom, Equal, Ground, Midpoint, OnFace, Parallel, ParallelToX/Y/ZAxis, ParallelToXY/XZ/YZPlane, Perpendicular, Smooth (G2), Tangent (between entities sharing a point) [16].
- Help notes [17]:
  - Coincident can constrain points and lines to surfaces.
  - On Face takes a point, line, arc or spline onto a planar face, or a point onto a curved face.
  - Tangent's first pick must be a spline; it may then go to a model edge that shares the endpoint.
  - Perpendicular with a spline must be endpoint to endpoint.
- Concentric, Horizontal/Vertical and Symmetric are labelled 2D only [17]. Inference must be enabled (Ctrl+I) [17].
- On Face, Equal and the axis/plane-parallel constraints were added to 3D sketch in the 2017 enhancement set [18].

Dimensions (Q3):
- API dimension types: line length, point-to-plane distance, radius, spline length, two-line angle, two-point distance [23].
- Two-line angle "fails if the lines are not coplanar" [23].
- A dimension fails if driving it would over-constrain the sketch [23].
- Dynamic Dimension is toggled with Ctrl+D [18].

Placement (Q4) [18]:
- 3D space indicator: click one of its planes to draw on it; Shift+Space switches between the planes and drawing parallel to the screen.
- Ortho Mode (Ctrl+R) draws along X, Y or Z.
- Context menu: Align to Plane, Orient Z, Orient to World, Snap Intersection.
- 3D Transform with a triad relative to the World, View or Local frame.

Sub-sketches / references (Q5, Q6):
- Include Geometry associatively copies model edges, 2D sketch geometry and imported wireframe. The source 2D sketch must be above the 3D sketch in the browser [21].
- Project to Surface: along a vector, to the closest point, or wrap to surface (length-preserving, on developable faces) [21].

Uses (Q7):
- Paths for wiring, tubing, sweeps and lofts, and surface edges. IPT (part) files only [19].
- Example: a wire route in a part that references the placement of several assembly components [19].

### 2.3 Autodesk Fusion

- 3D sketching is an option on an ordinary sketch (Sketch Palette > 3D Sketch). It "removes the planar restriction" [25]. A preference "Allow 3D sketching of lines and splines" also exists, and the tech-tip warns it makes off-plane picks easy to make by accident [30] (search summary).
- Tools in 3D (Q1): Line, Arc, Spline, Rectangle, Circle, Ellipse, Point, Text, Conic Curve [27].
- Constraints in 3D (Q2): Horizontal/Vertical, Coincident, Tangent, Equal, Parallel, Perpendicular, Fix/Unfix, Midpoint, Concentric, Collinear [26]. Symmetric, curvature and polygon-type constraints are not listed (analysis, from the absence on that page).
- Placement (Q4) [28]:
  - The 3D Sketch Manipulator appears at 0,0,0. Click XY, YZ or ZX on it to change the active plane; drag the rotation handles to tilt it.
  - Hover along an axis to restrict a point to it.
  - Up Arrow locks the XY position so the line can end at a different Z.
  - Alt cycles planes; Tab cycles rotation handles.
  - The manipulator origin moves to the last placed point.
- References (Q6): Project, Intersect, Include 3D Geometry, Project To Surface, Intersection Curve (creates a 3D sketch curve) and Isoparametric Curve [29].
- Uses (Q7): paths for tubing, sweeps and lofts; surface edges [25]. The help warns that unconstrained sketch geometry referenced downstream "can cause unpredictable results" [25].
- Complaint (Q9): a user reports a spline drifting off-plane with the 3D options off, possibly through projected geometry [31] (search summary).

### 2.3a Fusion against the separate-object programs (added 2026-10-26)

Fusion has no 3D sketch object. Every Fusion sketch can already hold 3D geometry; the
"3D Sketch" checkbox changes only how points are placed. Evidence:
- The API's Sketch object has no 3D flag. Its only 3D-described property is the
  bounding box; entities live in "sketch space" with sketchToModelSpace /
  modelToSketchSpace [84].
- Sketch.include "creates new sketch curves and points that represent the specified
  entity as sketch geometry", placed where the entity is, not projected [84].
- Autodesk's own API sample (API dated August 2014, before the March 2020 checkbox) adds
  a fitted spline to an ordinary sketch through (6,4,3) and (7,6,6), off the sketch's
  XY plane [85][86].
- The Autodesk tech tip on the older preference "Allow 3D sketching of lines and
  splines" says "ALL sketches in Fusion 360 are also 3D in nature". It also says Move/Copy
  can drag sketch elements into 3D, which "does have an impact on dimensions" [30]
  (search summary; page 403).
- The March 2020 update added "Intuitive 3D Sketching: Create Sketch > Sketch Palette >
  3D Sketch" [87] (search summary). That is the manipulator of [28].
- A forum answer says off-plane snaps only work with 3D sketch enabled. That is
  consistent with the checkbox being an input and snapping mode [88] (search summary).

What follows from one sketch type (Fusion) [25][26]:
- A profile for Extrude/Revolve is any closed loop of planar elements. Geometry that
  drifts off the plane silently stops a profile forming. Forum cases: a projected spline
  went 3D with the 3D options off, and CAD imports carry a tiny Z. A moderator mentions a
  context-menu fix that "only appears when the Z value is not 0" [31][89] (search
  summary; pages 403).
- The 3D constraint list is the 2D list: Horizontal/Vertical, Coincident, Tangent,
  Equal, Parallel, Perpendicular, Fix, Midpoint, Concentric, Collinear [26]. It has no
  axis-alignment, on-face or normal-to constraint, and the help does not say which plane
  Horizontal/Vertical refers to in 3D.
- The 3D pages list no dimension types [26][27]. A forum consultant's advice for
  dimensioning was to "untick 3d sketch" [90] (search summary).
- Full definition is weak in 3D. A user reports the fully-constrained lock disappearing
  after geometry moves along the third axis [91] (search summary; post circa 2020, may
  be out of date).
- The gain: no mode or object switch. One sketch can carry a planar profile and the 3D
  path swept along it, and project/include/intersect work the same in both.

The separate-object programs (SOLIDWORKS, Inventor, Solid Edge) [1][16][32]:
- A 3D sketch is its own feature type, so planar intent is declared by the type. A 2D
  sketch cannot drift off its plane, and a 3D sketch is never offered as a profile.
- They have 3D-specific constraints: Along X/Y/Z or ParallelToX/Y/ZAxis, On Plane/On
  Face, Normal To, Pierce, Tangent to Face [5][16].
- They have 3D-specific dimensions: Along X/Y/Z by Tab in SOLIDWORKS [6], and
  point-to-plane distance in Inventor [23].
- They keep a defined/under-defined state that users route tubes and weldment frames
  against. It is harder to reach than in 2D [15].
- SOLIDWORKS adds 3D sketch planes inside the 3D sketch [7]. Inventor brings 2D
  geometry in by Include Geometry [21].
- The cost: two sketch kinds to learn, and a path plus its profile in two objects.

Placement compared (analysis from [8][18][28]):
- Fusion's manipulator is a free triad at the last point. Any plane can be rotated to,
  and Alt cycles.
- SOLIDWORKS cycles fixed XY/YZ/ZX by Tab, with Ctrl+click to align to a face.
- Inventor has a space indicator with Shift+Space and an ortho mode.
- All three snap to model geometry. Only SOLIDWORKS documents that snapping creates the
  3D relation (Along X/Y/Z).

### 2.4 Solid Edge

All from [32]:
- Introduced in ST7 for Part, Sheet Metal, Assembly and routing. On the synchronous side all elements go into the active 3D sketch.
- "Unlike 2D Sketching, 3D Sketching does not need a plane to draw".
- Elements can be locked to planes or faces, to keypoints, to axes, or drawn freely in space.
- Keys: z locks a triad axis (repeat to cycle), x locks a triad plane (repeat to cycle), s locks to a highlighted edge. Lock Sketch Plane covers other planes, and "Relative to" orients the triad to any coordinate system.
- IntelliSketch controls keypoints and inferred relations, including Project Keypoint.
- Relationships are mostly as in 2D, plus Coaxial and On Plane.
- True dimensions are the default; projected dimensions are an option.
- Routing Path (ST10) is a dedicated path command.
- Editing goes through the command bar, edit handles, or Move with the steering wheel, and "the 3D relationships will be honored". Editing is called tricky because true lengths are not always visible.
- Uses: extrude, sweep, BlueSurf, bounded surface; sheet metal tab, contour flange, dimple; assembly relations; Tube, Piping and Frame routing.

A 3D spline in Solid Edge 3D sketch could not be confirmed.

### 2.5 Siemens NX

- An eng-tips answer: NX "has never had a so-called '3D Sketcher'". Instead, lines, arcs, conics and splines are placed in 3D and can be associative, "but they are still NOT a sketch" [33] (search summary).
- Curve on Surface: a spline, line, arc or circle on faces [34]:
  - continuity options are set per spline point, and tangent magnitudes are editable;
  - lines have a Projection mode (along local normals) and a Geodesic mode;
  - arcs are parametric.
- Studio Spline is defined through points or by poles, with constraints (slope, curvature) on any point or pole [35] (search summary). A user reports G1/G2 continuity is better kept outside the sketcher [35] (search summary).
- Recent releases added freehand Draw Shape with snapping, Project Curve and Intersection Curve inputs [36].
- No evidence was found of a constrained 3D sketch in current NX.

### 2.6 PTC Creo

- Creo has no constrained 3D sketcher. Creo Sketcher is 2D (gk). 3D curves come from datum curves and from Style (ISDX).
- Style curve types [37]:
  - Free: "lies in three-dimensional space", tied to no entity;
  - Planar: on a specified plane;
  - COS (curve on surface): on a single surface.
  Curves are built from control points or interpolation points, with a degree, periodic or not, an optional symmetry plane, and proportional update through soft points.
- Soft point references and tangent constraints are optional. Unlink removes them; Convert changes the curve type [41] (search summary).
- Tangent attributes: direction, length, angle on the active datum plane, elevation, draft [38].
  - Directional constraints: Fix Angle, Horizontal, Vertical (to the active datum plane grid), Normal (to a reference plane), Align (to a location on another curve) [38].
  - Length: Natural or Free; fixed or proportional [40].
- Curve-to-surface connections are G1 Surface Tangent, G2 Surface Curvature and G3 Surface Acceleration. The endpoint must be a soft point on the surface, a boundary or a COS [39].
- A curve-to-curve tangent needs the follower's endpoint to be a soft point on the leader [39]. Periodic curve ends have no tangency control [40].

### 2.7 CATIA V5 / 3DEXPERIENCE

- GSD (Generative Shape Design) wireframe is history-based: each curve is a feature defined from explicit inputs, not from a simultaneous solve (gk).
- GSD spline [42]:
  - runs through 2 or more selected points;
  - per point, Tangent Dir. is either explicit (parallel to a line or plane) or "from curve" (tangent to a curve), with tension;
  - a curvature direction and radius can be added once tangency is set;
  - "Geometry on support" projects the spline onto a plane or surface;
  - Close Spline.
- An IBM APAR records a GSD spline that did not follow the selected tangent direction [44] (search summary).
- FreeStyle 3D Curve has creation types through points or control points. The working plane (XY/XZ/YZ) must be active, or points are picked onto a surface [43] (search summary).
- A true multi-entity constraint solve in 3D was not found in CATIA user-facing curve tools (analysis). The constraint technology does exist (Spatial CDS, 4.6).

### 2.8 Onshape

- No 3D sketch. In April 2025 an Onshape employee answered "3D sketch like SolidWorks?" with "Check out the new Routing Curve feature" [48].
- Routing Curve [47]:
  - a triad manipulator offers translation along 3 axes, rotation about 3 axes, and translation in 3 planes;
  - the curve is an interpolated cubic spline, or a polyline with a global or per-point bend radius;
  - an optional target length;
  - inputs: vertices or mate connectors, sampled edges, or a CSV table;
  - each point's XYZ offset is relative to the origin, a vertex, a curve, or the previous point;
  - per-point derivative (tangent) and magnitude;
  - an "orthogonal path" segment edit;
  - "Add point on axis drag" adds a point on each drag;
  - uses: pipe routing, wiring, and NURBS curves for surfacing.
- 3D Fit Spline [45]:
  - a curvature-continuous spline through sketch points, curve endpoints or part vertices, or through tangential edges;
  - start and end direction with magnitude, and Match curvature;
  - Approximate (degree, control-point count, tolerance);
  - uses: routing, bridging, sweep/loft paths and guides.
- Bridging Curve: between two points, vertices or mate connectors, each end set to match position, tangent, curvature or flow (G3); edited through control points or magnitude/bias [46].
- Solver: an Onshape employee referred to "Onshape's implementation of D-Cubed's DCM" (2D sketch solver context) [49].

### 2.9 SolveSpace

All from [50]:
- "SolveSpace represents all geometry in 3d." An active workplane forces new entities into it; Sketch > Anywhere In 3d drops it.
- Entities:
  - points: free in 3d when no workplane is active;
  - workplane: a point plus a normal;
  - line segment;
  - circle: center, diameter and a normal that can be dragged or constrained in 3D;
  - arc: "must be drawn in a workplane";
  - cubic Bezier spline: an interpolating C2 spline when it has more on-curve points;
  - text, image.
- "Most constraints are available in both 3d and projected versions." With a workplane active, a constraint applies to the projection into it, and a line drawn in workplane A can be constrained by its projection into workplane B.
- Constraints:
  - distance/diameter, including a signed point-plane distance;
  - angle between vectors (lines or normals), 3d and projected;
  - horizontal/vertical, which need a workplane;
  - on point/curve/plane: 3d coincidence removes 3 DOF, the projected version 2. A point "on a circle" off-plane lands on the cylinder through it;
  - equal length/radius/angle (four vectors), length ratio, length difference;
  - midpoint (also midpoint on a plane);
  - symmetric about a plane or a line in a workplane;
  - perpendicular (a 90 deg angle);
  - parallel/tangent: a 3D parallel removes 2 DOF; tangency needs a shared endpoint;
  - same orientation (normals as quaternions, fixes all three rotations);
  - lock point where dragged;
  - reference dimensions.
- Failure UX: the background turns red and the Property Browser lists constraints that could be removed to restore consistency. Non-convergence is a separate error.

### 2.10 Rhino, Plasticity, Shapr3D (unconstrained 3D curves)

- Rhino: elevator mode (Ctrl+click) picks a point above or below the construction plane along a tracking line, with typed heights and osnaps. Planar mode keeps successive points at one elevation [51] (search summary). Curves are unconstrained after creation (gk); Grasshopper is the parametric route (gk).
- Plasticity 2026.2: a "Constraint System" creates constraints per command that "stay enforced as geometry is transformed". It is Studio-only, and "constraints aren't saved with the file yet" [52].
- Shapr3D: sketches are 2D items on a sketch plane. Forum users report no 3D sketch and work around it with several planar sketches [53] (search summary).

---------------------------------------------------------------------------

## 3. Constraint catalog union (3D)

Legend: SW SOLIDWORKS, INV Inventor, FUS Fusion, SE Solid Edge, SS SolveSpace, DCM D-Cubed 3D DCM (component), CDS Spatial CDS (component). "Y" = documented in the cited source, "-" = not listed or explicitly absent, "2D" = documented as 2D only.

| Constraint | SW [5][4] | INV [16][17] | FUS [26] | SE [32] | SS [50] | DCM [59] | CDS [64] |
|---|---|---|---|---|---|---|---|
| Coincident (point-point, point-on-curve) | Y | Y | Y | Y (gk detail) | Y | Y (blog [60]) | Y |
| Point on plane / on face / on surface | Y (On Plane, On Surface) | Y (On Face; coincident to surface) | - | Y (On Plane) | Y (pt in plane, pt on face) | - | - |
| Parallel (line-line) | Y | Y | Y | Y (gk) | Y | Y | Y |
| Parallel line-plane | Y | Y (to XY/YZ/XZ, to face) | - | - | via angle/normal (analysis) | - | - |
| Along / parallel to global X, Y, Z | Y (AlongX/Y/Z) | Y (ParallelToX/Y/ZAxis) | Y (Horiz/Vert in 3D) | Y (axis lock, gk as relation) | via workplane horiz/vert | - | - |
| Perpendicular | Y | Y | Y | Y (gk) | Y | Y | Y |
| Normal to plane/face | Y (Normal, AlongZ) | via axis/plane forms | - | - | via parallel to normal (analysis) | Y (normal) | - |
| Tangent | Y | Y (spline first) | Y | Y (gk) | Y (shared endpoint) | Y | Y |
| Tangent to face | Y | - | - | - | - | - | - |
| Curvature / G2 (smooth, equal curvature) | Y | Y (Smooth) | - | - | - | Y (free-form shape control) | Y (curvature dim) |
| Equal (length/radius) | Y | Y | Y | Y (gk) | Y | - | - |
| Midpoint | Y | Y | Y | Y (gk) | Y | Y | Y |
| Collinear | Y | Y | Y | Y (gk) | via pt on line (analysis) | - | - |
| Concentric / coaxial | Y | 2D | Y | Y (Coaxial) | via coincident centers (analysis) | Y | Y |
| Symmetric | Y (about centerline, same plane) | 2D | - | Y (gk) | Y (about plane) | Y | Y |
| Fix / ground | Y | Y | Y | Y (gk) | Y (where dragged) | - | - |
| Pierce / point at curve-plane intersection | Y | - | - | - | - | - | - |
| Same orientation (frame) | - | - | - | - | Y | - | - |
| Pattern | - | - | - | - | - | Y | - |
| Distance point-point (absolute) | Y | Y | (dim) | Y (true dim) | Y | Y | Y |
| Distance along X/Y/Z | Y (Tab) | - | - | projected dim option | projected versions | - | Y (coordinates) |
| Point-plane distance | via plane | Y | - | - | Y (signed) | Y | Y |
| Angle line-line | Y | Y (coplanar only) | (dim) | Y | Y (3d or projected) | Y | Y |
| Radius / diameter | Y | Y | (dim) | Y | Y | Y | Y |
| Curve/spline length | Y (fixed length, [13] page links) | Y (spline length) | - | - | equal line-arc length | Y (curve length) | Y (segment length) |

Core set (analysis):
- Constraints that appear in essentially every 3D implementation: coincident (point-point and point-on-curve), parallel, perpendicular, tangent, equal, midpoint, fix, collinear, point-point distance, radius, and angle.
- Every interactive 3D sketcher also gives a way to align a line with a global or local axis. SOLIDWORKS splits it into Along X/Y/Z (global) and Horizontal/Vertical/Normal (local to a sketch plane); Inventor has ParallelToX/Y/ZAxis and ParallelToXY/YZ/XZPlane.
- Point/curve on plane or face is present wherever model references are first-class (SW, INV, SE, SS).
- Second tier: symmetric, concentric/coaxial, curvature (G2) on splines, spline length, and dimensions along an axis.

Deliberately absent in 3D:
- Inventor marks Concentric, Horizontal/Vertical and Symmetric as 2D only [17].
- SolveSpace requires a workplane for horizontal/vertical, rectangles and arcs [50].
- Inventor's two-line angle fails if the lines are not coplanar [23].

Reason for these exclusions (analysis):
- Each one is undefined without a reference plane.
- A 3D design therefore needs either per-plane variants (SOLIDWORKS 3D sketch planes, SolveSpace workplanes) or explicit global-axis variants.

---------------------------------------------------------------------------

## 4. Solvers usable for a native C++ 3D sketch

### 4.1 Table

| Solver | License | Language | 3D points / lines / planes / circles / splines | Constraint coverage | Maintenance | Notable users | Usable in an LGPL-2.1+ C++ core module? |
|---|---|---|---|---|---|---|---|
| SolveSpace solver (libslvs, `include/slvs.h`) | GPL-3.0-or-later [54] | C++ (C API) | Y / Y / Y (workplane) / Y (circle, arc) / cubic Bezier only [54] | 37 constraint types incl. pt-in-plane, pt-on-face, pt-face distance, same orientation, projected distance, curve-curve tangent [54] | active; upstream pushed 2026-10-21 [54] | SolveSpace; Dune 3D (patched it for speed) [71]; Assembly3 via py_slvs [57] | No: GPL-3.0 code linked into the LGPL core makes the combined work GPL (analysis). OK only as a separately licensed, optional GPL add-on. |
| `slvs` Python wheel (PyPI 3.3) | metadata says MIT, but the package is built from the GPL-3.0 SolveSpace tree [55][54] | Cython | as above | as above | released 2026-10-21 [55] | -- | No (analysis: the MIT classifier does not relicense the GPL solver it wraps) |
| py_slvs (realthunder/slvs_py) | GPL-3.0 [56] | SWIG over SolveSpace 2.3 fork [56] | as above | as above | pushed 2026-04 [56] | Assembly3 primary solver [56][57] | No (same reason) |
| OndselSolver (FreeCAD Assembly) | LGPL-2.1 [58] | C++ | rigid bodies and markers, not sketch entities (analysis from class list) | joints: fixed, revolute, cylindrical, spherical, planar, point-in-plane, point-in-line, line-in-plane, parallel axes, perpendicular, angle, gear, rack-pinion, screw, ... [58] | FreeCAD/OndselSolver pushed 2026-09-19; the Ondsel repo is archived [58] | FreeCAD 1.x Assembly | Yes on license. Poor fit: there are no point/line/spline primitives or sketch-style diagnostics (analysis). |
| PlaneGCS (FreeCAD Sketcher) | LGPL-2.1-or-later [65b] | C++ (Eigen) | 2D only. Geo.h: "DeriVector2 are considered as 3d vectors with null z" [65b] | rich 2D set; BFGS, Levenberg-Marquardt, DogLeg; dense/sparse QR diagnosis [65b] | active (in-tree) | FreeCAD Sketcher; Python and WASM ports [68] (search summary) | Yes on license. Needs a 3D entity/constraint layer; the solve core (DogLeg/LM, QR rank diagnosis, subsystem split) could be reused (analysis). No 3D fork was found [68] (search summary). |
| openDCM (ickby) | LGPL-2.1 [66] | C++ header-only (Eigen, Boost) | Point3, Line3, Plane, Sphere, Cylinder [66] | distance, angle, parallel, coincident, alignment; clusters (rigid parts) [66] | README: "pre-alpha ... unuseable"; last push 2018-04 [66] | written for the old FreeCAD Assembly by Stefan Troeger (gk for the attribution) | License yes; maturity no. Possible source of ideas (analysis). |
| Siemens D-Cubed 3D DCM | commercial [59] | C/C++ component (gk) | points, lines, planes, circles, ellipses, cylinders, tori, parametric curves/surfaces [59] | distance, angle, radius, parallel, perpendicular, tangent, concentric, symmetric, normal, midpoint, pattern; bounded dims; curve shape by tangency/curvature/length; under/over/well-defined diagnostics [59] | active (v61 blog [60]) | vendors generally [82] (search summary); "3D pipe/wire routes", 3D sketches named as a target [59] | No: proprietary; not distributable with GPL/LGPL source as a free component (analysis). |
| LEDAS LGS 3D | commercial; IP sold to Bricsys (now Hexagon) in 2011 [61] | C++ (gk) | (not detailed) | non-linear solver plus decomposition heuristics [61] | last release found: v9, 2014 [62] (search summary) | BricsCAD; Cimatron, CD-adapco licensed LGS [61][62] | No (proprietary; unclear if still licensable) |
| C3D Solver (C3D Labs / ASCON) | commercial [63] (search summary) | C++ (gk) | 2D and 3D solvers [63] | assembly mates for 3D objects [63] | active vendor [63] | nanoCAD, GeoS Center [63] (search summary) | No (proprietary) |
| Spatial CDS (Dassault) | commercial [64] | native C++, .NET 6, WebAssembly path [64] | points, lines, planes, circles, ellipses, parabolas, hyperbolas, cylinders, cones, spheres, tori, NURBS curves and surfaces [64] | distance, angle, radius, curvature, segment length; coincidence, tangency, parallel, perpendicular, concentric, midpoint, symmetry; joints; update, move-under-constraints, and simulation modes [64] | active [64] | "Built on Dassault Systemes V5/V6 constraint technology" [64] | No (proprietary) |
| OCCT | LGPL-2.1 + exception (gk) | C++ | no general solver | `TDataXtd_Constraint` only stores constraints: Verified() stays false "until the solver sets it back to true" [65]. GccAna/Geom2dGcc are 2D construction (tangent circles/lines), not a solver [65] | -- | -- | n/a: nothing to reuse beyond 2D construction helpers |
| GeoSolver (H. van der Meiden) | GPL-3.0 (SourceForge metadata) [67] | Python | points, with distance and angle constraints; lines, planes and spheres mapped onto points [67] (search summary) | cluster rewriting, chirality, incremental [67] (search summary) | research code (2008-2010 papers) [67] (search summary) | Geometric Constraint Workbench demo [67] | No (GPL, Python); its algorithms are published and can be studied (analysis) |
| CadQuery solvers | Apache-2.0 [69] | Python on casadi + ipopt (assembly), nlopt (sketch) [69] | assembly: shapes as rigid bodies; sketch: 2D lines/arcs [69] | assembly: Fixed, FixedPoint, FixedAxis, FixedRotation, Plane, Point, Axis, PointInPlane, PointOnLine; sketch: Fixed, FixedPoint, Coincident, Angle, Length, Distance, Radius, Orientation, ArcAngle [69] | active [69] | CadQuery | License yes. Python; shows that a plain least-squares formulation works for small problems (analysis). |
| build123d | Apache-2.0 (gh metadata) | Python | its "constraints" tutorial is about construction methods (gk) | -- | active | -- | Not a solver |
| DIY base: Eigen | MPL-2.0 [70] | C++ | -- | linear algebra, sparse QR (already used by PlaneGCS) | active | FreeCAD | Yes (already a FreeCAD dependency) |
| DIY base: Ceres Solver | BSD-3-Clause style (LICENSE text) [70] | C++ | -- | non-linear least squares, autodiff, sparse | active | -- | Yes |
| DIY base: CasADi | LGPL-3.0 [70] | C++ / Python | -- | symbolic AD + NLP front end | active | CadQuery | LGPL-3.0 code with an LGPL-2.1+ host: the combined distribution falls under LGPL-3.0 terms (analysis). Usable as a dynamic library. |
| DIY base: NLopt | LGPL-2.1+ by default (luksan part), else MIT-style [70] | C | -- | NLP algorithms | active | CadQuery sketch | Yes |
| DIY base: Ipopt | EPL-2.0 [70] | C++ | -- | interior-point NLP | active | CadQuery | EPL-2.0 is weak-copyleft. Dynamic linking from LGPL code is usual, but EPL-2.0 is not GPL-compatible unless a secondary license is declared (gk). |

Not found:
- No "noether" or GeoGebra-style 3D constraint solver turned up.
- GeoGebra 3D does dependency-driven construction, not simultaneous solving (gk).

### 4.2 Notes

- SolveSpace's library model maps cleanly onto a CAD sketch [54]:
  - entities are defined from parameters;
  - entities and constraints belong to groups;
  - "only the entities within that group will be modified by the solver", while constraints may reference outside entities (the model-reference case);
  - the solver needs an initial guess, which also picks the solution branch.
  This is also the right shape for a 3D sketch that references model geometry (analysis).
- SolveSpace's "projected" constraints are a cheap way to offer 2D-style constraints inside 3D [50]. A 3D sketch with SOLIDWORKS-style planes could implement Horizontal/Vertical as constraints projected into the plane (analysis).
- Dune 3D (GPL-3.0) [71]:
  - combines OCCT with the SolveSpace solver;
  - calls the solver directly instead of through the exposed wrapper, because the wrapper "didn't expose all of the features I needed";
  - had to "patch the solver to make it sufficiently fast ... by symbolically solving equations where applicable".
  This is a warning on SolveSpace solver performance for larger 3D problems (analysis).
- PlaneGCS today [65b]:
  - SPDX LGPL-2.1-or-later in GCS.cpp;
  - algorithms BFGS, LevenbergMarquardt, DogLeg;
  - QR via Eigen dense or sparse.
  The license fits a core module. The work is a 3D geometry and constraint layer plus diagnostics for 3D DOF (analysis). Fork history on this box: SparseQR on zero columns is quadratic (project memory; not a web source).
- Academic background:
  - "Modern solvers" since the 1990s decompose with graph-based structural analysis, then solve the subproblems algebraically [74].
  - A 2022 review splits the field into decomposing well-constrained systems (late 1980s to mid-2000s) and classifying general systems with under- and over-constrained parts (late 2000s on) [73].
  - The witness configuration method (Michelucci and Foufou, CAD 2006) analyzes dependencies by computing infinitesimal motions of a known witness. A 2019 paper argues it has limitations for core tasks [72] (search summary).
  - A survey statement that extending 2D methods to 3D "remains an open issue due to the lack of available rigidity theory in 3D" [74] (search summary). This supports numeric least squares plus rank diagnosis over pure graph decomposition for 3D (analysis).

### 4.3 Assembly3 and slvs: GPL implications

- Assembly3 is GPL-3.0 [57]. Its solver backends are `sys_slvs.py` ("SolveSpace", imported as `slvs` or `py_slvs`) and `sys_sympy.py` ("SymPy + SciPy", using `scipy.optimize`) [57].
- The README tells users to `pip install py_slvs` [57].
- py_slvs is a SWIG binding of a SolveSpace 2.3 fork, GPL-3.0 [56].
- Implications (analysis):
  - Assembly3 kept the GPL code out of the LGPL FreeCAD core: it is a separately installed GPL workbench, loaded at runtime through Python.
  - A native C++ 3D sketch in an LGPL-2.1+ core module cannot link SolveSpace's solver without making the distributed combination GPL-3.0.
  - Relicensing is not possible without all SolveSpace copyright holders (gk).
  - Workable patterns: (a) an LGPL-compatible solver (extend PlaneGCS, or build on Eigen/Ceres/NLopt) in core; or (b) a solver plugin interface in core, with an optional GPL SolveSpace backend shipped as a separate GPL add-on, the Assembly3 pattern.

---------------------------------------------------------------------------

## 5. FreeCAD community history

- Issue #5794 is not about 3D sketching [75]. It is "[Feature] [Sketcher] Adding centripetal Catmull-Rom spline", imported from tracker id 3500 (2018) and still open. It is relevant to spline UX: users want curves controlled by way points rather than knots [75].
- Issue #14010, "[Feature] 3D Constraint-driven sketcher/wireframe workbench" (open, since 2024-05-14; labels Priority: High, GSoC, Mod: Part, Mod: Surface) [76]:
  - Motivation: non-planar wire routing, plumbing, ducting and tube frames; "very difficult to relate/constrain the dimensions of wires drawn in one sketch to those in another".
  - The reporter suggests the Assembly solver ("MbDCode") makes this feasible and says it "would not be practical to do this in Sketcher".
  - A commenter argues for something like CATIA GSD (wireframe plus surfacing), not just 3D sketches.
  - Others point to the Curves workbench (Join Curve, 3D B-splines) and the LGPL Parametric Curve FP macro.
  - A 2026-02-07 comment records from the FreeCAD complaint session: "We don't have useful 3D sketching tools".
  - A Discord 3D-sketch thread is linked.
- Issue #19091, "Improve 3D sketch experience in Draft workbench" (closed 2025-01-16 as a duplicate of #14010): it proposed SOLIDWORKS-style plane switching in Draft [77].
- Issue #12699 groups the FreeCAD Day 2024 complaint "no way to do surfacing in part design". It notes the realthunder Link branch already has some of this (pad of open sketches, splitting by surfaces), and a commenter again proposes a GSD-like wireframe workbench [78].
- Existing workarounds named by users [79] (search summary):
  - attach several 2D sketches to each other;
  - Curves workbench "Mixed Curve" from two sketch projections;
  - Dodo DWire;
  - JoinCurves.
  Draft can take 3D points and constrain movement with the X/Y/Z keys (FreeCAD 0.13 release notes, search summary [83]).
- Roadmaps:
  - No FreeCAD or Ondsel roadmap item for 3D sketching was found.
  - Ondsel's blog archive lists "We are shutting down Ondsel" on October 30, with conflicting year labels (2024 heading, "2025" date) [80]. Ondsel's other late posts are about the 2D sketcher (floating input widgets, faster constraining) [80].

---------------------------------------------------------------------------

## 6. Sources

1. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_3d_sketching_top.htm
2. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_Coordinate_Systems_in_3D_Sketches.htm
3. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_3D_Sketching_Tools.htm
4. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_Sketch_Relations_in_3D_Sketches.htm
5. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_Description_of_Sketch_Relations.htm
6. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_3D_dimension_types.htm
7. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_3D_Sketching_Capabilities.htm
8. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_Space_Handles.htm
9. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_2D_Sketching_in_3D.htm
10. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_Line_Snap.htm
11. https://help.solidworks.com/2024/english/SolidWorks/sldworks/t_Creating_3D_Splines.htm
12. https://help.solidworks.com/2024/english/SolidWorks/sldworks/t_Editing_3D_Splines.htm
13. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_Tangent_to_Face_Relations_with_Splines.htm
14. https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_Differences_Between_2D_and_3D_Sketching.htm
15. https://forum.solidworks.com/thread/59875 (search summary)
16. https://help.autodesk.com/cloudhelp/2025/ENU/Inventor-API/files/GeometricConstraints3D.htm
17. https://help.autodesk.com/cloudhelp/2025/ENU/Inventor-Help/files/GUID-A85A7A30-7D81-4C75-8769-CAD034EEA930.htm
18. https://help.autodesk.com/cloudhelp/2021/ENU/Inventor-WhatsNew/files/GUID-DCE06C79-C8B7-45FF-A42C-91A2F167D548.htm (3D Sketch Enhancements, Inventor 2017)
19. https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-Help/files/GUID-97C8CDC5-AAAD-4A6F-BB84-24DB5C90C7A3.htm
20. https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-Help/files/GUID-37BA1DCE-6BC4-41CD-A36D-577F31B304FC.htm
21. https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-Help/files/GUID-7D3E9482-CBF9-410E-8758-E650712CD049.htm
22. https://help.autodesk.com/cloudhelp/2025/ENU/Inventor-API/files/Sketch3D.htm
23. https://help.autodesk.com/cloudhelp/2025/ENU/Inventor-API/files/DimensionConstraints3D.htm
24. https://help.autodesk.com/cloudhelp/2015/ENU/Inventor-Help/files/GUID-9EC93A07-913A-45B1-B054-EE2338D45FB2.htm (search summary)
25. https://help.autodesk.com/cloudhelp/ENU/Fusion-Sketch/files/SKT-3D-SKETCH.htm
26. https://help.autodesk.com/cloudhelp/ENU/Fusion-Sketch/files/SKT-REF-3D-SKETCH-SUPPORTED-CONSTRAINT.htm
27. https://help.autodesk.com/cloudhelp/ENU/Fusion-Sketch/files/SKT-REF-3D-SKETCH-SUPPORTED-CMD.htm
28. https://help.autodesk.com/cloudhelp/ENU/Fusion-Sketch/files/SKT-CREATE-3D-SKETCH.htm
29. https://help.autodesk.com/cloudhelp/ENU/Fusion-Sketch/files/SKT-SKETCH-CREATE-PROJECT-INCLUDE.htm
30. https://www.autodesk.com/products/fusion-360/blog/fusion-360-tech-tip-sketching/ (search summary; page returned 403)
31. https://forums.autodesk.com/t5/fusion-support/spline-not-staying-in-2d-plane/m-p/12700702 (search summary)
32. https://blogs.sw.siemens.com/solidedge/3d-sketching-the-future-of-sketching-in-cad/
33. https://www.eng-tips.com/threads/3d-sketch.339380/post-7729229 (search summary)
34. https://blogs.sw.siemens.com/designcenter/nx-tips-and-tricks-curve-on-surface/
35. https://medesign.seas.upenn.edu/index.php/NX/SketchingSplines and https://eng-tips.com/threads/parametric-spline.215234 (search summary)
36. https://blogs.sw.siemens.com/nx-design/whats-new-in-nx-june-2023-core-design/
37. https://support.ptc.com/help/creo/creo_plus/usascii/surfacing/style/To_Create_a_New_Curve.html
38. https://support.ptc.com/help/creo/creo_plus/usascii/surfacing/style/Defining_Curve_Tangents.html
39. https://support.ptc.com/help/creo/creo_plus/usascii/surfacing/style/Endpoints_for_Curve_Connections.html
40. https://support.ptc.com/help/creo/creo_plus/usascii/surfacing/style/About_Curve_Tangents.html
41. https://support.ptc.com/help/creo/creo_plus/usascii/surfacing/style/About_Unlinking_and_Converting_Curves.html (search summary)
42. https://www.staff.city.ac.uk/~ra600/ME2105/Catia%20course/CATIA%20Tutorials/wfsug_C2/wfsugbt0102.htm (CATIA V5 GSD user guide copy)
43. https://eng-tips.com/viewthread.cfm?qid=248592 (search summary)
44. https://www.ibm.com/support/pages/apar/HD76410 (search summary)
45. https://cad.onshape.com/help/Content/PartStudio/3d_fit_spline.htm
46. https://cad.onshape.com/help/Content/PartStudio/bridging_curve.htm
47. https://cad.onshape.com/help/Content/PartStudio/routing_curve.htm
48. https://forum.onshape.com/discussion/27453/3d-sketch-like-solid-works
49. https://forum.onshape.com/discussion/2172/what-is-missing-to-fully-define-sketch
50. https://solvespace.com/ref.pl
51. https://docs.mcneel.com/rhino/8/help/en-us/user_interface/accurate_modeling.htm (search summary)
52. https://80.lv/articles/3d-modeling-cad-software-plasticity-2026-2-has-been-released/ (2026-10-20)
53. https://support.shapr3d.com/hc/en-us/articles/18816009328284-Sketching-in-Shapr3D and https://discourse.shapr3d.com/t/moving-spline-control-points-perpendicular-to-start-sketch-plane/40717 (search summary)
54. https://github.com/solvespace/solvespace (license GPL-3.0, README "GPL v3 or later"; include/slvs.h; exposed/DOC.txt; read via GitHub API 2026-10-26)
55. https://pypi.org/project/slvs/ (v3.3, 2026-10-21; pyproject classifier MIT)
56. https://github.com/realthunder/slvs_py and https://pypi.org/project/py-slvs/ (1.0.6, GPL-3.0)
57. https://github.com/realthunder/FreeCAD_assembly3 (README; freecad/asm3/sys_slvs.py, sys_sympy.py)
58. https://github.com/FreeCAD/OndselSolver (LGPL-2.1; OndselSolver/ASMT*Joint sources) and https://github.com/Ondsel-Development/OndselSolver (archived)
59. https://www.siemens.com/en-us/products/plm-components/d-cubed/3d-dcm/
60. https://blogs.sw.siemens.com/plm-components/d-cubed-3d-components-release-highlights/
61. https://ledas.com/en/expertise/3d-modeling/ (redirect target of lgs.ledas.com)
62. https://ledas.com/news/201-bricsys-released-version-9-of-lgs-technology-constraint-driven-parametric-design-and-optimization (search summary)
63. https://c3dlabs.com/blog/customer-stories/geos-center-licenses-c3d-solver-from-c3d-labs/ and https://c3dlabs.com/upload/documents/2026/C3D_Labs_brochure_EN.pdf (search summary)
64. https://www.spatial.com/products/cds-constraint-design-solver
65. OCCT source, ~/works/sw/occt (branch LinkVibe-801): src/ApplicationFramework/TKCAF/TDataXtd/TDataXtd_Constraint.hxx; src/ModelingAlgorithms/TKGeomAlgo/{GccAna,GccEnt,GccInt,Geom2dGcc}. Online: https://dev.opencascade.org/doc/occt-7.8.0/refman/html/classTDataXtd__Constraint.html (search summary)
65b. FreeCAD source in this repo: src/Mod/Sketcher/App/planegcs/GCS.h (enum Algorithm), Geo.h (line 112 comment), GCS.cpp (SPDX header)
66. https://github.com/ickby/openDCM (README; opendcm/module3d/*.hpp)
67. https://geosolver.sourceforge.net/ (search summary) and https://sourceforge.net/rest/p/geosolver (license GPLv3, created 2009-09-04)
68. https://planegcs.readthedocs.io and https://cdn.jsdelivr.net/npm/@salusoft89/planegcs@1.1.7/README.md (search summary)
69. https://github.com/CadQuery/cadquery (LICENSE Apache 2.0; cadquery/occ_impl/solver.py, sketch_solver.py)
70. https://github.com/ceres-solver/ceres-solver (LICENSE), https://gitlab.com/libeigen/eigen (MPL-2.0), https://github.com/casadi/casadi (LGPL-3.0), https://github.com/stevengj/nlopt (COPYING), https://github.com/coin-or/Ipopt (EPL-2.0)
71. https://github.com/dune3d/dune3d (GPL-3.0; README)
72. https://arxiv.org/pdf/1904.00526 and https://jga2006.projet.liris.cnrs.fr/PAPIERS_RESUMES_PRESENTATIONS/neutral.pdf (search summary)
73. https://arxiv.org/abs/2202.13795
74. https://arxiv.org/abs/1608.05205 (Fudos, Hoffmann, Joan-Arinyo); 3D-rigidity remark from https://ar5iv.labs.arxiv.org/html/1608.05205 (search summary)
75. https://github.com/FreeCAD/FreeCAD/issues/5794
76. https://github.com/FreeCAD/FreeCAD/issues/14010
77. https://github.com/FreeCAD/FreeCAD/issues/19091
78. https://github.com/FreeCAD/FreeCAD/issues/12699
79. https://grabcad.com/questions/how-to-create-a-model-with-freecad-wiring (search summary)
80. https://www.ondsel.com/blog/archive/
82. https://www.digitalengineering247.com/article/vero-licenses-siemens-d-cubed-3d-dcm/products and related 2007-2013 D-Cubed licensee articles (search summary)
83. https://wiki.freecad.org/Translations:Release_notes_0.13/22/en (search summary)
84. https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/fusion_Sketch.htm
85. https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/SketchFittedSplines_add_Sample.htm
86. https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/SketchFittedSplines_add.htm (search summary: "available since August 2014")
87. https://www.autodesk.com/products/fusion-360/blog/march-2020-product-update-whats-new/ (search summary)
88. https://forums.autodesk.com/t5/fusion-design-validate-document/annoying-snapping-to-geometry-in-sketches/td-p/8815275 (search summary)
89. https://forums.autodesk.com/t5/fusion-support/spline-not-staying-in-2d-plane/m-p/12703625 (search summary)
90. https://forums.autodesk.com/t5/fusion-design-validate-document/dimension-diagonal-line/m-p/10328986 (search summary)
91. https://forums.autodesk.com/t5/fusion-design-validate-document/constraining-3d-vs-2d-sketches/td-p/9944404 (search summary)
