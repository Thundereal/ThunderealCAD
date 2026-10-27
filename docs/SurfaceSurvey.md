# Surface workbench survey: what CAD programs offer, and what the Curves workbench has

Date of survey: 2026-10-27. Companion to docs/Surface.md (the design), which cites
Part A's sources as [S n] and Part B's command rows as [C n].

- **Part A** surveys the surface tools of ten CAD programs and of upstream FreeCAD.
- **Part B** inventories the Curves workbench (tomate44/CurvesWB), read from its code.

Conventions (Part A):
- `[n]` is a numbered source in section A7.
- "(search summary)" on a source means only a search-engine summary was seen, not the page itself.
- "(gk)" marks general knowledge that no source here confirms. Treat it as a lead to check.
- "(analysis)" marks this survey's own inference from the cited facts.

# Part A. Surface tools in CAD programs

Access notes: the SOLIDWORKS help pages render only through JavaScript (fetches returned an empty shell, a direct download returned "Access Denied"), so SOLIDWORKS facts are search summaries. Siemens NX documentation redirects to a login portal, so NX facts are NXOpen API search summaries and forum posts. wiki.freecad.org and forum.freecad.org both answered with a bot-protection "Access Denied" page, so FreeCAD facts come from the GitHub API and from source code.

---------------------------------------------------------------------------

## A1. Summary table per CAD program

| Program | Surface creation | Surface editing | Continuity | Curves for surfaces | Analysis |
|---|---|---|---|---|---|
| SOLIDWORKS | Extruded, Revolved, Swept, Lofted, Boundary, Filled, Planar, Offset, Ruled, Radiate, Freeform [1][4][5] | Trim, Untrim, Extend, Knit, Thicken, Delete Face (Delete / Delete and Patch / Delete and Fill), Replace Face, Cut with Surface [1][5][6] | Fill: Contact / Tangent / Curvature per edge; Boundary: Tangency to Face, Curvature to Face per curve with tangent influence % [2][3] | Split Line, Projected, Composite, Intersection, Face Curves (iso-parametric), Offset on Surface (gk; partly [62]) | Zebra Stripes, Curvature display, Deviation Analysis, Draft Analysis, curvature combs (gk) [7][8] |
| Autodesk Fusion | Patch, Extrude, Revolve, Sweep, Loft, Ruled, Offset [9] | Trim, Untrim, Extend, Stitch, Unstitch, Merge, Reverse Normal, Split Face, Split Body, Thicken (gk), Replace Face (gk) [10] | Patch: Connected (G0) / Tangent (G1) / Curvature (G2) per edge, Tangency Weight; Loft: G0/G1/G2 at end profiles and on edge rails [11][12] | Intersection curve, projected curve, isoparametric extract (gk) | Zebra, Curvature Comb, Curvature Map, Isocurve, Draft, Environment Map, Accessibility, Minimum Radius [14] |
| Autodesk Inventor | Boundary Patch, Ruled Surface (Normal / Tangent / Vector), Loft, Sweep, Extrude, Revolve [15] | Stitch (with tolerance and gap analysis), Trim, Extend (boundary edges only), Sculpt, Replace Face, Delete Face with Heal, Thicken/Offset [15][16] | Boundary Patch: Contact (G0) / Tangent (G1) / Smooth (G2) per edge with weight 0..1; guide rails [15] | Project to Surface, Intersection, Silhouette, Curve on Face [62] | Zebra, Draft, Curvature (comb), Surface (Gaussian / Mean / Max), Cross Section [17] |
| Siemens NX | Through Curves, Through Curve Mesh, Studio Surface, Swept, Styled Sweep, N-Sided Surface, Fill Surface, Bridge Surface, Ruled, Styled Blend, Face Blend, Offset Surface (gk names; [19][20][21][22][23]) | Trim Sheet, Untrim, Sew, Thicken, Extension, Law Extension, Enlarge, Replace Edge, Delete Face, Replace Face, Match Edge, X-Form (gk; partly [22]) | G0..G3 named in the product; Through Curve Mesh sets first/last primary and first/last cross continuity separately; Bridge Curve G0..G3 [18][19][20] | Bridge Curve, Curve on Surface, Isoparametric, Projected, Intersection, Offset in Face (gk; [19][62]) | Reflection, face curvature, curve combs, deviation, highlight lines (gk) |
| PTC Creo | Boundary Blend (1 or 2 directions, approximate via influencing curves), Fill (planar), Sweep, Swept Blend, Variable Section Sweep, Offset, Style (ISDX) boundary / loft / blended surfaces [24][25][27] | Trim, Extend, Merge, Thicken, Solidify, Replace (gk) | Boundary Blend: Free / Tangent / Curvature / Normal per chain, stretch value; G3 only inside Style [24][25][26] | Style COS (curve on surface) curves, projected, intersection, wrap (gk; [62]) | Reflection, curvature, draft, dihedral, Gaussian, deviation (gk) |
| CATIA V5 GSD (+ FreeStyle) | Extrude, Revolve, Sphere, Cylinder, Offset, Variable/Rough offset (gk), Sweep (explicit, linear, circular, conic), Fill, Multi-sections (loft), Blend; FreeStyle Net Surface, Styling Fill, Blend Surface, Match Surface [28][31] | Join (assemble), Healing, Split, Trim, Boundary, Extract, Extrapolate, Untrim (gk), Thickness, Mid surface, Wrap, Bump, Develop [28] | Fill and Blend: Point (C0) / Tangency (C1) / Curvature (C2) per boundary with a support face; G1 judged at 0.5 deg [29][30] | Projection, Intersection, Combine, Reflect Line, Connect (blend curve), Parallel curve, 3D curve offset, Rolling offset, Spine, Isoparametric (gk) [28] | Connect Checker (gap / tangency / curvature), Porcupine, Surfacic curvature, draft analysis (gk) [31] |
| Onshape | Extrude, Revolve, Sweep, Loft, Boundary Surface, Fill, Ruled Surface, Offset Surface, Face Blend, Constrained Surface [32][34] | Move Boundary (extend/trim), Mutual Trim, Split, Enclose, Thicken, Delete Face, Move Face, Replace Face, Surface Flattening [32][34] | Boundary Surface per profile: None / Normal / Tangent to profile / Match tangent / Match curvature, magnitude; Fill per edge Position / Tangency / Curvature; G3 "Flow" on Bridging Curve [32][33][34] | Bridging Curve (to G3), Isoparametric curve, Projected curve, Offset curve, Intersection curve (gk) [34] | Zebra, Curvature combs, Dihedral, Reflection, Connection Analysis, Curve/Surface Analysis [34] |
| Rhino | Loft, Sweep1, Sweep2, NetworkSrf, EdgeSrf (2..4 curves), Patch, PlanarSrf, Revolve, RailRevolve, Ribbon, DevLoft, OffsetSrf, VariableOffsetSrf, BlendSrf, FilletSrf (gk-ish), Drape [35][36][37][39][40] | Trim, Split, Untrim, Join, ExtendSrf, MatchSrf, Rebuild, FitSrf, ShrinkTrimmedSrf, RefitTrim [38][42] | NetworkSrf Loose / Position / Tangency / Curvature; Sweep2 edge Position / Tangency / Curvature; BlendSrf Position..G4 per end; MatchSrf Position / Tangency / Curvature [35][36][37][38] | Project, Pull, Intersect, ExtractIsocurve, BlendCrv, InterpCrvOnSrf, OffsetCrvOnSrf, DupEdge (gk) | Zebra, CurvatureAnalysis (Gaussian / Mean / min / max radius), CurvatureGraph, DraftAngleAnalysis, EMap, EdgeContinuity, GCon, PointDeviation, CrvDeviation [41] |
| Plasticity | Bridge Surface, Constrained Surface, Extrude, Loft (with guides), Patch, Pipe, Revolve, Sweep, XNurbs (N-sided) [43][44][45] | Trim, Untrim, Extend Sheet, Join Sheets, Thicken, Delete Face, Match Face, Rebuild Face, Isoparam, Offset Face, Imprint [43] | Loft: G0/G1/G2 with per-edge tension; Patch: Planar / G0 / G1 / G2; XNurbs G0/G1/G2 [43][44][45] | Bridge Curve, Project Curve Body, Imprint Curve Body, Offset Curve [43] | Toggle Surface Curvature, Measure Continuity, Section Analysis [43] |
| Solid Edge | BlueSurf (sections + guides), Bounded, Swept, Ruled, Redefine (replace tangent faces by one surface), Offset, Extruded, Revolved [46][47][48] | Intersect (trim + extend + stitch), Trim, Extend, Stitch, Replace Face, Thicken (gk; [47][48][49]) | Bounded and Redefine: Natural / Tangent / Curvature per edge, no tangency weighting; BlueSurf ends: Natural / Normal to Section / Parallel to Section / Tangent Interior / Tangent Continuous [46][48] | Keypoint, Intersection, Projected, Contour, Derived curves (gk) | Zebra, curvature shading, curvature combs (gk) |
| FreeCAD (fork = upstream here) | Surface: Filling (N-sided GeomPlate), GeomFillSurface (2..4 boundaries), Sections (loft through curves), BlendCurve; Part: Loft, Sweep, Ruled, Extrude, Revolve, Offset [50][51] | Surface: ExtendFace, Cut; Part: Slice, BooleanFragments, Shape builder, Thickness, Defeaturing; PD Split (fork) [50] | Filling: per-boundary order G0/G1/G2 with support face; nothing else on surfaces [50] | Surface: CurveOnMesh, BlendCurve (continuity 0..25 per end); Part: ProjectionOnSurface, Section [50] | Zebra matcap in the fork renderer [50]; nothing else in core |

---------------------------------------------------------------------------

## A2. Per-program notes

### A2.1 SOLIDWORKS

Creation (Q1):
- The surface features folder covers Boundary, Planar, Extruded, Revolved, Swept, Lofted, Offset, Radiate and Surface Cut. Its Boundary entry says boundary surfaces "can be tangent or curvature continuous in both directions" and usually beat lofts for quality [4].
- Filled, Freeform and Ruled are confirmed only by a course outline [5]. Ruled types (Tangent to Surface, Normal to Surface, Tapered to Vector, Perpendicular to Vector, Sweep) are (gk).
- Boundary takes curves in Direction 1 and Direction 2; the directions are interchangeable [3].
- Swept and Lofted accept guide curves (gk). A sweep with guide curves is SOLIDWORKS' two-rail sweep (gk).

Editing (Q2):
- Toolbar includes Knit, Untrim, Trim, Thicken [1]. Trim uses a surface, plane or sketch to cut intersecting surfaces; Untrim extends a surface along its natural boundaries to patch holes or edges [1].
- Delete Face has three options. Delete leaves an open body; Delete and Patch extends the adjoining faces to close the gap; Delete and Fill puts one new face across the gap, with a Tangent fill checkbox [6].
- Replace Face and Cut with Surface appear in the course outline [5].

Continuity (Q3):
- Filled Surface: Contact, Tangent, Curvature per edge, mixable in one patch, with an "Apply to all edges" box. Sketch boundaries accept Contact only. Constraint curves (sketch points or splines) add interior slope control. "Optimize surface" with 2 or 4 sides gives a loft-like simplified patch [2].
- Boundary: per curve, Tangency to Face and Curvature to Face (only when attaching to existing geometry), each with a Tangent influence %; Normal to Profile and Direction Vector (with draft angle) when not attaching to geometry [3].
- A third-party blog notes Loft applies curvature continuity only at its first and last profiles, not to its sides; Boundary does all four sides [3] (search summary of engineersrule).

Curves (Q4): 3D-sketch Face Curves (iso-parametric), Intersection Curve, Offset on Surface, Spline on Surface [62]. Split Line, Projected Curve, Composite Curve, Curve Through XYZ Points (gk).

Analysis (Q5): Zebra Stripes (read stripe mismatch / kink / smooth as G0 / G1 / G2), Curvature display (View > Display > Curvature, coloured by radius), Deviation Analysis (min / max / average angle between faces along an edge) [7][8]. Draft analysis and curvature combs (gk).

### A2.2 Autodesk Fusion

Creation (Q1): Patch ("a planar or 3D surface within the boundary of a closed loop"), Extrude, Revolve, Sweep, Loft, Ruled ("at a specified distance and angle from selected edges"), Offset [9]. Form (T-Splines/SubD) is a separate workspace and outside this survey.

Editing (Q2): Trim ("splits intersecting surfaces and removes one or more sections"), Untrim ("extends trimmed surfaces and fills gaps, holes, or empty regions"), Extend, Stitch, Unstitch, Merge, Reverse Normal, Scale, Split Face, Split Body [10]. Thicken is distinct from Offset (forum, search summary [9]); Thicken, Delete and Replace Face (gk).

Continuity (Q3):
- Patch: Connected (G0), Tangent (G1), Curvature (G2) on boundary edges, each with a Tangency Weight; Group Edges applies one setting to all edges, off = per edge; Interior Rails / Points make the patch pass through curves or points [11].
- Loft: G0/G1/G2 at the first and last profile when they are edges of faces; API enum LoftRailEdgeConditions gives G0/G1/G2 for rails that are edges (added Nov 2022). A 2019 forum post said rails had no G1/G2 then [12].
- Sweep types: Single Path, Path + Guide Rail (rail controls scale and orientation), Path + Guide Surface [13].

Analysis (Q5): Curvature Comb (body edges only), Curvature Map, Draft, Environment Map, Isocurve (UV isocurves with combs), Zebra, Accessibility, Minimum Radius [14].

### A2.3 Autodesk Inventor

Creation (Q1):
- Boundary Patch from a closed sketch or a closed chain of edges; several loops per operation; optional Guide Rails (curves or points) shape the interior [15].
- Ruled Surface: Normal, Tangent, Vector/Sweep, with extension distance and angle; sketch edges are not valid input [15].
- Loft with rails and centerline, Sweep with guide rail or guide surface (gk).

Editing (Q2):
- Stitch joins surfaces into a quilt or solid with a Maximum Tolerance; an Analyze tab shows stitched (black), unstitched (red) and near-tangent (magenta) edges, plus the smallest Max Gap [15].
- Trim (cutting tool: quilt, face, sketch, work plane), Extend ("You can extend only boundary edges"), Sculpt (add/remove material from a solid, or a solid from untrimmed surfaces) [16].
- Replace Face (faces replaced by a surface, quilt or work planes), Delete Face with Heal (extends adjacent faces until they meet), Thicken/Offset on the Modify panel [16].

Continuity (Q3): Boundary Patch per edge Contact (G0, default), Tangent (G1), Smooth (G2); G1 and G2 take a unitless Weight, typically 0..1 [15].

Curves (Q4): 3D sketch Project to Surface, Intersection Curve, Silhouette, Curve on Face [62].

Analysis (Q5): Zebra, Draft (colour by pull direction), Cross Section (simple / advanced with wall thickness), Curvature (comb on faces, surfaces, sketch curves, edges), Surface (Gaussian, Mean, Max colour map) [17].

### A2.4 Siemens NX

Creation (Q1):
- Through Curve Mesh: separate primary and cross curve lists, both required; continuity set separately for FirstPrimary, LastPrimary, FirstCross, LastCross; an Emphasis option [20]. This is NX's network (Gordon-type) surface (analysis).
- Bridge Surface between two edges, default G1 on both sides, with a G0 tolerance [19].
- N-Sided Surface (Insert > Mesh Surface); one user found it sagged and preferred Bridge Curve + Through Curve Mesh for control [21][22].
- Fill Surface with guide curves and continuity (course outline, search summary [22]).
- Studio Surface 2x2 for four-sided patches from edges or projected curves; a user notes Bridge Curve cannot be G3, so a surface bounded by one cannot be G3 in that direction [23].
- Styled Blend (per-side continuity, center curve as spine), Blend Corner (G1 default, G2 option) [23].
- Through Curves (loft), Swept with up to three guides, Ruled, Styled Sweep, Four Point Surface, Offset Surface, Variable Offset (gk).

Editing (Q2): Untrim (associative, works on sewn bodies; older Remove Trim), Replace Edge (replaces Edit Sheet Boundary), Sew with tolerance, Extension / Law Extension / Enlarge [22]; Trim Sheet [22]. Delete Face, Replace Face, Match Edge, X-Form, I-Form, Refit Face (gk).

Continuity (Q3): Siemens defines G0 position, G1 tangent, G2 curvature, G3 rate of change of curvature [18]. Bridge Curve end constraints G0..G3 with Cubic / Quintic / Advanced fit [19]. A forum user reports that a tight G1 asked on a multi-edge patch can loosen at the parent edge, so continuity has to be checked afterwards [21].

Curves (Q4): Bridge Curve [19]; Curve on Surface with continuity [62]. Isoparametric, Projected, Intersection, Offset in Face, Composite (gk).

Analysis (Q5): reflection, face curvature, highlight lines, curve combs, deviation gauge, draft (gk; no source fetched).

### A2.5 PTC Creo

Creation (Q1):
- Boundary Blend: 1st and 2nd direction collectors of curve or edge chains; Close blend for a one-direction closed loop; Control Points tab with fit methods Natural, Arclength, Point to Point, Piece to piece, Developable; Options tab "Influencing curves" make an approximate surface, with smoothness factor and patch counts [25].
- "Optimize surface shape" smooths a single four-boundary surface with G1 or G2 applied [25].
- Style (ISDX): boundary surface from 3 or 4 boundaries, blended surface (primary curve + cross curves), loft through sections [27].
- Fill (planar), Sweep, Swept Blend, Variable Section Sweep (several chains = multi-rail), Offset (gk).

Editing (Q2): Trim, Extend, Merge, Thicken, Solidify, Replace, Freestyle (gk).

Continuity (Q3):
- Boundary Blend Constraints tab: Free (default), Tangent, Curvature, Normal per chain; the system proposes reference surfaces and asks for one per segment otherwise; Stretch value sets transition intensity; "Add side curve influence" makes side edges tangent to the references [24][25].
- G3 ("acceleration continuous") exists only inside Style; outside Style the boundary blend stops at G1/G2 [26].
- ISDX default boundary conditions follow the underlying curve conditions: G0 for shared boundaries, G1 for tangent or normal, G2 for curvature [27].

Curves (Q4): Style free, planar, and COS (curve on surface) curves; G1/G2/G3 to surfaces; soft points [62].

Analysis (Q5): reflection, curvature, section, draft, Gaussian, dihedral angle, deviation (gk).

### A2.6 CATIA V5 Generative Shape Design (and FreeStyle)

Feature set, from the V5 Automation interface list [28]: assemble (Join), blend, boundary, bump, combine, connect (connect curve), curve_par (parallel curve), 3d_curve_offset, develop, extract / extract_multi, extrapol (Extrapolate), extrude, fill, healing, helix, intersection, inverse, loft (Multi-sections Surface), mid_surface, near, offset, polyline, project, reflect_line, revol, rolling_offset, sphere, cylinder, spine, spiral, spline, split, sweep / sweep_circle / sweep_conic / sweep_explicit / sweep_line, thickness, trim, wrap_curve, wrap_surface, plus points, lines, planes, circles and transforms.

Creation (Q1):
- Explicit sweep subtypes: with reference surface, with two guide curves, with pulling direction [28].
- Linear (ruled) sweep modes: two guides, guide and angle, guide and middle curve, guide and angle from reference surface, guide and tangency surface, guide and draft direction, two tangency surfaces [28].
- Multi-sections: sections, guides (also "guide with tangent"), spine, start/end section tangent surfaces, couplings with coupling points; coupling modes include ratio, tangency, tangency then curvature, vertices (gk for the full list; tangency and curvature coupling from [28]).
- Fill: bounds, a support face per bound, interior constraint curves or points ("passing" elements), a "plane only" mode, tolerant mode with maximum deviation [29].
- Blend: two limit curves with supports, continuity per limit, closing points, coupling [28].
- FreeStyle adds Net Surface (associative in Freestyle Profiler), Styling Fill, Blend Surface, Match Surface [31].

Editing (Q2): Join, Healing, Split (cut by elements, keep chosen side, can keep both), Trim (mutual), Untrim (gk), Disassemble (gk), Extrapolate, Thickness, Mid surface [28][55].

Continuity (Q3): Fill per boundary point (C0), tangency (C1), curvature (C2) [29]; Blend per limit point / tangency / curvature [28]. CATIA treats faces as G1 when the angular discontinuity is under 0.5 deg, and tangent Fill stops once it is under that [30].

Curves (Q4): Projection, Intersection, Combine (curve from two sketches' projections), Reflect Line, Connect curve (blend curve with continuity), Parallel curve (offset on surface), 3D curve offset, Rolling offset, Spine, Isoparametric curve (gk) [28].

Analysis (Q5): Connect Checker (surface-surface and surface-edge gap, tangency, curvature), Porcupine curvature, Surfacic curvature analysis [31]; feature draft analysis (gk).

### A2.7 Onshape

Creation (Q1):
- Boundary Surface: four-sided; U and V profiles; per-profile conditions None, Normal to profile, Tangent to profile, Match tangent (pick faces), Match curvature (pick faces), Normal direction, Tangent direction, each with a signed Magnitude [32].
- Fill: any number of boundary segments forming one closed chain; guides are vertices (pass-through) or curves (Sampled or Precise) [33].
- Ruled surface, Offset surface, Face blend, Constrained surface (fit) [32][34]. Sweep, Loft (with guides and path), Extrude, Revolve (gk for options).

Editing (Q2): Move boundary (extend or trim an edge), Mutual trim, Enclose (surfaces to solid), Thicken, Delete face, Move face, Replace face, Surface Flattening [32][34]. Split (gk).

Continuity (Q3): Fill per edge Position (G0), Tangency (G1; sketch selections; optional adjacent faces), Curvature (G2; optional adjacent face) [33]. The product names G0 Position, G1 Tangent, G2 Curvature, G3 Flow; Bridging Curve can make "Match Flow (G3)" [34].

Curves (Q4): Bridging Curve, Offset Curve, single-span Bezier curves [34]; isoparametric curve and projected curve exist (search summary [34]). Intersection and composite curves (gk).

Analysis (Q5): Zebra Stripes, Curvature Combs, Dihedral Analysis, Reflection Analysis, Connection Analysis, Curve/Surface Analysis [34].

### A2.8 Rhino

Creation (Q1):
- Surface from curves: DevLoft, ExtrudeCrv, EdgeSrf (two to four curves), Fin, Loft, NetworkSrf, Patch, PlanarSrf, RailRevolve, Revolve, Ribbon, Sweep1, Sweep2 [40]. From surfaces: OffsetSrf, VariableOffsetSrf, Drape, UnrollSrf [40].
- NetworkSrf: curves in two directions, each crossing all of the other; edge matching Loose / Position / Tangency / Curvature; separate edge, interior and angle tolerances; the result is always smoothed [35].
- Sweep2: two rails, ordered sections, Point sections at ends, Maintain height, rebuild/refit options, Add Slash alignments, edge continuity Position / Tangency / Curvature when rails are surface edges [36].
- BlendSrf: between two edges or chains; continuity per end Position (G0), Tangency (G1), Curvature (G2), G3, G4; bulge sliders, Add Shapes, Straighten, Planar sections, Same height [37].
- Patch: points, curves, edges, meshes, point clouds; span counts, Stiffness, Adjust tangency (edges of existing surfaces), Automatic trim, Starting surface [39]. No G2 option on the page (analysis: it is a fitted surface, not a constrained one).

Editing (Q2): MatchSrf changes an untrimmed edge to meet another edge with Position / Tangency / Curvature, Preserve other end, Refine match (adds knots), Average surfaces, MultipleMatches [38]. ExtendSrf, FilletSrf, Rebuild, RebuildUV, FitSrf (fewer control points), Untrim, ShrinkTrimmedSrf, RefitTrim [42]. Trim, Split, Join (gk).

Curves (Q4): Project, Pull, Intersect, ExtractIsocurve, BlendCrv (to G4), InterpCrvOnSrf, OffsetCrvOnSrf, DupEdge, Silhouette (gk).

Analysis (Q5): Zebra, CurvatureAnalysis (Gaussian, mean, min/max radius), CurvatureGraph, DraftAngleAnalysis, EMap, EdgeContinuity (distance, tangency, curvature differences along edges), GCon, PointDeviation, CrvDeviation, ThicknessAnalysis [41].

### A2.9 Plasticity

Creation (Q1): Bridge Surface, Constrained Surface, Extrude, Loft, Patch, Pipe, PolySplines, Revolve, Sweep, XNurbs [43]. Loft takes guides that must touch the profiles, trims overlap, loops, and lofts from edges or faces [43]. XNurbs makes N-sided and quad-sided fills, "Tools like Patch may struggle with complex surfaces" [44]. Patch closes curves or holes, all holes of a sheet at once, with guide curves [45].

Editing (Q2): Delete Face, Remove Fillets from Shell, Extend Sheet, Join Sheets, Thicken, Hollow, Imprint, Isoparam, Match Face, Rebuild Face, Raise Surface Degree, Untrim, Unwrap Face, Offset Face [43].

Continuity (Q3): Loft G0/G1/G2 set per edge by clicking markers, tension per edge [43]; Patch Planar / G0 / G1 / G2 [45]; XNurbs G0/G1/G2 as one setting [44].

Curves (Q4): Bridge Curve, Project Curve Body, Imprint Curve Body, Offset Curve [43].

Analysis (Q5): Toggle Surface Curvature, Toggle Curve Curvature, Measure Continuity, Section Analysis [43]. No zebra page was found (analysis: it may live in the shader/view mode settings).

### A2.10 Solid Edge

Creation (Q1):
- BlueSurf: "Cross Section" and "Guide Curve" directions, treated equally; at least two sections, or one section plus one guide; open and closed elements may mix; ends can be capped [46].
- Bounded surface (N-sided fill) [48]; Ruled surface with start options tangent, normal to face, tapered to plane, natural, along an axis [48]; Redefine Surface replaces contiguous tangent faces with one editable surface [48]. Swept, Offset, Extruded, Revolved (gk).

Editing (Q2): Intersect = trim + extend + stitch in one command, with Linear or Curvature Continuous extension [49]; Replace Face swaps a solid face for a surface [47]. Trim, Extend, Stitch, Thicken, Split (gk).

Continuity (Q3): Bounded and Redefine: Natural / Tangent / Curvature, applied to the whole loop or per edge, no tangency weighting ("results look deflated") [48]. BlueSurf end conditions Natural, Normal to Section, Parallel to Section, Tangent Interior, Tangent Continuous [46].

Curves (Q4): Keypoint, Intersection, Projected, Contour, Derived, Split curves (gk). BlueDot connects curves (gk).

Analysis (Q5): Zebra stripes, curvature shading, curvature combs (gk).

### A2.11 FreeCAD (upstream and this fork)

What the Surface workbench has (Q1-Q5) [50][51]:
- The toolbar is identical in the fork and in upstream main: Filling, GeomFillSurface ("Fill boundary curves"), Sections, ExtendFace, CurveOnMesh, BlendCurve, and Cut. Upstream also has a `Measure.cpp` integration; the fork does not.
- Filling (`Surface::Filling`, `BRepFill_Filling` / GeomPlate): boundary edges with an optional support face and an order G0/G1/G2 per boundary; unbound (interior) edges with their own order; free faces; constraint points; an initial face; degree, points on curve, iterations, anisotropy, 2D/3D/G1/G2 tolerances, maximum degree and segments.
- GeomFillSurface: 2 to 4 boundary curves, FillType Stretched / Coons / Curved (`GeomFill_*`), a reversed flag per boundary. G0 only.
- Sections: a single list of section curves through `GeomFill_NSections`. No guides, no spine, no end continuity.
- ExtendFace: extends a whole face by fractions in U-/U+/V-/V+ (symmetric options), refit by sampling 32x32. Not per edge, not by length.
- BlendCurve: a curve between two edges, continuity 0..25 per end, parameter and size per end.
- Cut: cuts one shape by a list of shapes. `Surface::Sewing` exists as a feature with no command.

Part workbench supplies the rest: Loft (sections, ruled, closed, no guides), Sweep (one spine, Frenet, Transition; no second rail), Ruled Surface (two edges), Extrude, Revolve, Offset, Thickness, Shape builder (shell from faces), Slice / BooleanFragments, Section (intersection curve), ProjectionOnSurface, Defeaturing. PartDesign Pipe has an Auxiliary mode with an auxiliary spine that controls orientation; the fork's PartDesign has Split [50].

Analysis: the fork renderer has a Zebra matcap preset (`src/Gui/ShadingOptions.cpp`) [50]. Sketcher has a 2D curvature comb. Nothing else in core; the Curves workbench has ZebraTool, ParametricComb, DraftAnalysis and SurfaceAnalysis [60].

Upstream plans and open threads:
- Issue #31597 (open, 2026-07-27): "Surface: Workbench is very limited and surface functions split across external Curve and Part wb." A commenter (pierreporte) asks that zebra and other analysis be generic (work on solid faces, not only in Surface WB) and that the comparison be with CATIA and NX. Ping to tomate44 (Chris_G, Curves author) [52].
- PR #31089 (open draft, 2026-07-01, Shkolik): Gordon surface feature in Surface WB, "twin-engine": OCCT 8.0's Gordon or the occ_gordon single-header library (by rainman110), with automatic seam alignment. Both engines approximate when reparametrizing, causing deviation. Forum t=95441 [53].
- OCCT 8.0.0 added `GeomFill_Gordon` (high level: arbitrary curves, finds intersections, sorts, reparametrizes) and `GeomFill_NetworkSurface` (low level) -- commits 1cc42af8d6 (2026-03-03) and 02530f0217 (2026-06-04), present in tags V8_0_0 and V8.0.1 and therefore in our `LinkVibe-801` kernel [54].
- PR #32572 "Surface: Intersection curves" and PR #32609 "Surface: Freehand B-spline" (PaddleStroke, Sept 2026, open); issue #32619 asks for a CATIA-style polyline with per-knot fillets [59].
- Issue #5587 (open since 2014, Priority High, GSoC): trim a surface by another and click the part to keep. wwmayer: "currently not or not easily doable in Part or Surface wb". A 2025 comment describes CATIA Split vs Trim. Comments point at this fork's split-by-surface demo [55].
- Issue #15028 (open): curvature map and zebra analysis tools; tomate44 points at his shader-based Draft Analysis in Curves WB [56].
- Issue #12699 (open): "Add surfacing capabilities in PartDesign", sub-issues #16103 split bodies by surface / sketch, #5587 trim, #8754 loft guide curves, #6010 loft improvement, #16114 replace a face by a referenced surface. It cites this fork as already having open-sketch pads and split by surfaces [57].
- Issue #6255 "Integration of Curves workbench" is CLOSED; #6254 (3D parametric curve) open; tomate44 there: the Surface WB should group the tools of a "bottom-up workflow starting from points / curves then building surfaces, and possibly shells / solids" [58].
- Issue #30312 (open): curvature-driven fairness tools [61].
- The Curves workbench (LGPL-2.1, pushed 2026-10-12) calls itself "EXPERIMENTAL and should NOT be used for any serious work" and "my personal playground". Its modules include Gordon, blendSurface, multiLoft, pipeshell (sweep with auxiliary), TrimFace, isocurves, curveOnSurface, curveExtend, joinSurfaces, segmentSurface, PlateSurface, HQRuledSurface, RotationSweep, ReflectLines, ZebraTool, ParametricComb, DraftAnalysis, SurfaceAnalysis, Flatten [60]. It now also targets "xwzCAD" on Codeberg [60].

---------------------------------------------------------------------------

## A3. Continuity: who offers what, and how it is specified

| Program | Max G on fills / patches | Max G on network / loft / blend | Where G3+ exists | How the adjacent face is given |
|---|---|---|---|---|
| SOLIDWORKS | G2 (Curvature) [2] | G2 on Boundary in both directions; Loft G2 at ends only [3] | none found | implicit when the boundary is a model edge; tangent influence % [3] |
| Fusion | G2 [11] | G2 at loft ends and edge rails [12] | none found | implicit from the selected edge; Tangency Weight [11] |
| Inventor | G2 (Smooth) [15] | G2 (gk for loft) | none found | implicit from edge; Weight 0..1 [15] |
| NX | G2 (gk) | G2 on Through Curve Mesh per boundary; G3 on Studio / Bridge Curve [18][19][20] | Bridge Curve G3, Studio (gk) | face per boundary (gk) |
| Creo | G2 [24] | G2 Boundary Blend; G3 only in Style [26] | Style (ISDX) | reference surface per chain segment, Stretch value [24] |
| CATIA | C2 [29] | C2 on Blend, Multi-sections tangent surfaces [28] | FreeStyle (gk) | explicit support face per bound [29] |
| Onshape | G2 [33] | G2 Match curvature on Boundary Surface [32] | Bridging Curve G3 Flow [34] | explicit "Faces" per profile, Magnitude [32] |
| Rhino | G1 (Patch Adjust tangency) [39] | G2 NetworkSrf / Sweep2; G4 BlendSrf [35][36][37] | BlendSrf G3/G4, BlendCrv (gk) | implicit when the input is a surface edge [35][36] |
| Plasticity | G2 [44][45] | G2 Loft with tension [43] | none found | click markers per edge [43] |
| Solid Edge | G2 (Curvature) [48] | Tangent Continuous on BlueSurf ends [46] | none found | implicit; no weighting on Bounded [48] |
| FreeCAD | G2 on Filling [50] | none (Sections, GeomFillSurface are G0) [50] | BlendCurve 0..25 (curves only) [50] | explicit support face per boundary on Filling [50] |

Patterns (analysis):
- G0/G1/G2 per edge is the norm on every N-sided fill and every network surface. A tool that offers only G0 is not competitive.
- Two styles of saying which face to match: implicit (the boundary is an edge of an existing face, so that face is the reference -- SOLIDWORKS, Fusion, Inventor, Rhino) and explicit (pick a support face per boundary -- CATIA, Creo, Onshape). Explicit is needed when the boundary is a free curve lying on a face. A design can default to implicit and allow an explicit override (analysis).
- Every program adds a scalar per edge to shape the transition: tangent influence %, Tangency Weight, Weight 0..1, Stretch, Magnitude, tension, bulge. Solid Edge's lack of one is called out as a defect [48].
- G3 is a Class-A feature (NX Studio, Creo Style, Onshape Flow, Rhino BlendSrf) and is not in the mainstream mechanical tools.

---------------------------------------------------------------------------

## A4. Tool catalogue: union across programs

Creation:
- Loft / multi-section through sections, with guides, spine, couplings, end tangency to faces.
- Network / boundary (two curve families; Gordon or Coons-Gordon): SW Boundary, NX Through Curve Mesh, Creo Boundary Blend (2 directions), Onshape Boundary Surface, Rhino NetworkSrf, SE BlueSurf, CATIA FreeStyle Net Surface.
- 2..4-sided edge surface (Coons): Rhino EdgeSrf, NX Studio 2x2, Creo Style boundary, FreeCAD GeomFillSurface.
- Sweep: one rail; two rails (scaling: SW guide curves, Fusion guide rail, CATIA two guides, Rhino Sweep2); guide surface (Fusion, CATIA reference surface); rail revolve.
- Ruled / linear sweep variants: normal, tangent, angle, draft direction, two tangency surfaces (CATIA has seven modes [28]).
- N-sided fill / patch with interior constraints.
- Blend / bridge surface between two edges (NX Bridge, CATIA Blend, Rhino BlendSrf, Plasticity Bridge Surface; others use loft/boundary between edges).
- Offset, variable offset; extrude, revolve; planar fill; fit surface to points/mesh (Onshape Constrained Surface, Plasticity Constrained Surface, Rhino Patch/Drape).

Editing:
- Trim (pick side to keep), split (keep all), mutual trim, untrim.
- Knit / sew / stitch / join; enclose to solid; thicken.
- Extend (per edge, linear or natural/same-surface, by distance or up to).
- Delete face (+ heal by extending neighbours, or + fill); replace face.
- Match surface (deform an existing surface to an edge with G0..G3).
- Rebuild / refit / reduce control points; raise degree; shrink to trims.

Curves feeding surfaces: projected, intersection, isoparametric, curve on surface (spline/arc on a face), offset on surface (parallel curve), blend / bridge / connect curve, composite / join curves, combine (curve from two planar projections), reflect / silhouette lines, split line.

Analysis: zebra / reflection / environment map, curvature comb (curves, edges, isocurves), curvature map (Gaussian / mean / min / max radius), draft analysis, continuity / connection checker along an edge (gap, angle, curvature difference), deviation (curve-curve, point-surface).

---------------------------------------------------------------------------

## A5. Synthesis

### A5a. The core set: tools nearly every program has

"y" = a dedicated tool; "~" = available through another tool or only partly; "-" = not found; "?" = no source and no confident gk.

| Tool | SW | Fus | Inv | NX | Creo | CATIA | Onsh | Rhino | Plast | SE | Count (y) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Loft through sections | y | y | y | y | y | y | y | y | y | y | 10 |
| Loft guides / end G1-G2 to faces | y | y | y | y | y | y | y | ~ | y | y | 9 |
| Network / boundary (2 curve families) | y | ~ | ~ | y | y | ~ | y | y | ~ | y | 6 (+4 ~) |
| Sweep, 1 rail | y | y | y | y | y | y | y | y | y | y | 10 |
| Sweep, 2 rails (scaling) | y | y | y | y | y | y | ~ | y | ? | y | 8 |
| N-sided fill G0-G2 per edge | y | y | y | y | ~ | y | y | ~ | y | y | 8 (+2 ~) |
| Blend surface between edges | ~ | ~ | ~ | y | ~ | y | ~ | y | y | ~ | 4 (+6 ~) |
| Ruled surface | y | y | y | y | ~ | y | y | ~ | ~ | y | 7 |
| Offset surface | y | y | y | y | y | y | y | y | y | y | 10 |
| Extend (per edge) | y | y | y | y | y | y | y | y | y | y | 10 |
| Trim (keep a side) / split | y | y | y | y | y | y | y | y | y | y | 10 |
| Untrim | y | y | - | y | ? | ? | ~ | y | y | ? | 5 |
| Knit / sew / stitch | y | y | y | y | y | y | y | y | y | y | 10 |
| Thicken | y | y | y | y | y | y | y | ? | y | y | 9 |
| Delete face (+ patch) | y | y | y | y | ? | - | y | ~ | y | ? | 6 |
| Replace face | y | ? | y | y | ? | - | y | - | - | y | 5 |
| Match surface (deform to edge) | - | - | - | y | ~ | y | - | y | y | - | 4 |
| Rebuild / refit | - | - | - | ~ | - | ~ | - | y | y | ~ | 2 |
| Projected / intersection curve | y | y | y | y | y | y | y | y | y | y | 10 |
| Isoparametric / curve on surface | y | y | y | y | y | y | y | y | y | ? | 9 |
| Blend / bridge curve | ? | ? | ? | y | ~ | y | y | y | y | ? | 5 |
| Zebra / reflection | y | y | y | y | y | ~ | y | y | ? | y | 8 |
| Curvature comb | y | y | y | y | y | y | y | y | y | y | 10 |
| Curvature map | y | y | y | y | y | y | y | y | y | y | 10 |
| Draft analysis | y | y | y | y | y | y | ? | y | - | ? | 7 |
| Edge continuity / deviation check | y | ~ | ~ | y | y | y | y | y | y | ? | 7 |

Several cells rest on (gk) rather than a fetched page; they are marked as such in section 2. The NX and Creo columns lean on (gk) the most. The ranking below is robust to a few flips in those columns (analysis).

Core set (analysis): loft with guides and edge continuity, a two-direction network surface, 1-rail and 2-rail sweep, N-sided fill with per-edge G0-G2, offset, extend, trim/split, knit/sew, thicken, projected/intersection/isoparametric curves, and zebra + curvature comb + curvature map. Blend surface is universal as a capability even where it is not a separate command.

### A5b. What our Surface workbench (plus Part) already covers

| Core tool | Fork today | Gap |
|---|---|---|
| N-sided fill with G0-G2 | Surface Filling (GeomPlate) with per-boundary order and support face [50] | Has the substance. Missing a per-edge weight; no "apply to all"; plate output is approximate (G1/G2 by tolerance) |
| 2..4-sided edge surface | GeomFillSurface (Stretched / Coons / Curved) [50] | G0 only; no per-edge continuity (OCCT `GeomFill_ConstrainedFilling` and `GeomFill_BoundWithSurf` exist for G1 with a surface [54]) |
| Loft through sections | Surface Sections (`GeomFill_NSections`), Part Loft [50] | No guides, no spine, no G1/G2 end conditions to faces (upstream #8754, #6010) |
| Network / boundary (Gordon) | none (Curves WB has one; upstream PR #31089 in draft) [53][60] | Missing. OCCT 8.0 `GeomFill_Gordon` is already in our kernel [54] |
| Sweep 1 rail | Part Sweep, PD Pipe [50] | fine |
| Sweep 2 rails | PD Pipe Auxiliary (orientation only) [50] | No scaling second rail; no guide surface |
| Blend surface between edges | none; BlendCurve + Filling or GeomFillSurface by hand [50] | Missing as a feature |
| Ruled | Part Ruled Surface [50] | Two-edge only; no normal / tangent / angle / draft modes |
| Offset | Part Offset [50] | fine (variable offset missing) |
| Extend | Surface ExtendFace (whole face, fractional, sampled refit) [50] | No per-edge extend, no length, no "natural vs linear" choice |
| Trim / split | Surface Cut, Part Slice / BooleanFragments, PD Split (fork) [50] | No pick-to-keep trim, no mutual trim (upstream #5587) |
| Untrim | none | Missing |
| Knit / sew | Part Shape builder, `Surface::Sewing` (no command) [50] | Sewing has no UI; no gap analysis like Inventor's Stitch [15] |
| Thicken | Part Thickness / Offset with fill [50] | fine |
| Delete face + heal | Part Defeaturing [50] | No "delete and fill" |
| Replace face | none (upstream #16114) [57] | Missing |
| Match surface | none | Missing |
| Projected / intersection curve | Part ProjectionOnSurface, Part Section [50]; upstream PR #32572 [59] | Exists but not surface-oriented (no curve-on-surface result object) |
| Isoparametric curve | none as a feature (Curves WB has one [60]) | Missing |
| Blend curve | Surface BlendCurve [50] | Exists (continuity up to 25) |
| Zebra | fork renderer matcap preset [50] | Exists in the fork only; upstream #15028 open |
| Curvature comb / map / draft / continuity check | none in core (Curves WB has them [60]) | Missing |

### A5c. Gaps, ranked

Ranking weighs (1) how many programs have the tool (section 5a), and (2) how central it is to a curve-network -> surface -> trim -> sew workflow. All ranking is (analysis).

1. Network / boundary surface (Gordon) with per-edge G0/G1/G2 to adjacent faces. In every program as a capability (6 dedicated, 4 through loft-with-guides). It is the main shape tool in SOLIDWORKS (recommended over Loft [4]), NX, Creo, Onshape, Rhino, Solid Edge. The kernel piece is already in hand (`GeomFill_Gordon`, OCCT 8.0 [54]); what OCCT does not give is the G1/G2 boundary conditions, which is where the work is. Upstream is converging here too (PR #31089).
2. Trim with pick-to-keep, split, mutual trim, and untrim. 10/10 for trim/split; the oldest open upstream request (#5587, 2014, Priority High). Without it no surfacing workflow can finish. The fork's PD Split is a partial base.
3. Loft with guides, spine and end continuity to faces. 9/10. Our Sections and Part Loft both lack guides and end conditions (#8754, #6010). With item 1 this covers most "through curves" shapes; the end-continuity code is shared with item 1 and item 5.
4. Sweep with two rails (scaling) and with a guide surface. 8/10. `BRepOffsetAPI_MakePipeShell` has an auxiliary spine for orientation only; a scaling 2-rail sweep needs its own construction (or a Gordon network built from swept sections).
5. Blend surface between two edges with G0..G2 (G3 later) and per-side weight. Dedicated in 4/10 but present as a capability in all 10. It is the workhorse for joining primary surfaces. Once items 1 and 3 support edge continuity, a blend surface is mostly a convenience front end (a 2-section loft between edges with G1/G2 ends), so it should come right after them.
6. Analysis: curvature comb on edges and isocurves, curvature map, draft analysis, edge-continuity checker. 7-10/10. The fork has only zebra. Per the #31597 comment these should be generic view tools, not Surface-only. Surfacing without a continuity checker leaves the user unable to verify items 1-5.
7. Extend per edge (by length or up to, natural or linear) and untrim. 10/10 for extend; our ExtendFace is face-wide and fractional.
8. Knit/sew UI with gap analysis. 10/10; we have the feature (`Surface::Sewing`) but no command.
9. Curve tools that feed surfaces: isoparametric curve, curve on surface / offset on surface, and a curve-on-surface result for projection and intersection. 9-10/10. Upstream is working on intersection curves (PR #32572).
10. Replace face and delete-and-fill. 5-6/10. Ties surfacing to solids (upstream #16114).
11. Match surface. 4/10, the Class-A tier (NX, CATIA FreeStyle, Rhino, Plasticity).
12. Rebuild / refit. 2/10 dedicated; useful as a hidden step inside the other tools.

Order suggested by the ranking (analysis): edge-continuity conditions as one shared module, then Gordon network (1) and guided loft (3) on top of it, then trim/split (2) and the analysis tools (6) in parallel, then two-rail sweep (4) and blend surface (5).

---------------------------------------------------------------------------

## A6. Open questions for later

- What weight or tension parameter to expose per edge, and whether it maps to the same thing in Gordon, loft and fill (analysis).
- Whether to wrap OCCT 8.0 `GeomFill_Gordon` or occ_gordon (PR #31089 found flaws in both) -- needs a measurement, not a survey.
- Whether analysis belongs in the renderer (shader-based, like the fork's zebra matcap and Curves WB's draft analysis) or as document objects.

---------------------------------------------------------------------------

## A7. Sources

1. https://help.solidworks.com/2021/english/SolidWorks/sldworks/r_surfaces_toolbar.htm (search summary; the fetched page was empty)
2. https://help.solidworks.com/2022/English/SolidWorks/sldworks/HIDD_FEAT_FILL_SURFACE.htm (search summary, with the 2025 fill tutorial)
3. https://help.solidworks.com/2019/English/SolidWorks/Sldworks/r_boundary_pm.htm and https://www.engineersrule.com/the-solidworks-boundary-surface-feature/ (search summary)
4. https://help.solidworks.com/2020/English/solidworks/sldworks/c_Surface_Features_Folder.htm (search summary)
5. https://coursetakers.teachable.com/courses/master-surfacing-in-solidworks-with-real-life-examples/lectures/53594461 (search summary; course outline, not official)
6. https://help.solidworks.com/2019/english/SolidWorks/sldworks/t_Deleting_and_Patching_Faces.htm and https://blogs.solidworks.com/tech/2020/04/solidworks-command-delete-face-easily-remove-faces-from-a-solid-or-surface-body.html (search summary)
7. https://hawkridgesys.com/blog/solidworks-continuity-curvature-part-ii (search summary)
8. https://www.goengineer.com/blog/investigating-continuity-and-curvature-in-solidworks (search summary)
9. https://help.autodesk.com/cloudhelp/ENU/Fusion-Patch/files/SFC-CREATE-SURFACE-FROM-SKETCH.htm (fetched: Patch, Extrude, Revolve, Sweep, Loft, Ruled, Offset)
10. https://help.autodesk.com/cloudhelp/ENU/Fusion-Patch/files/SFC-MODIFY-SURFACE.htm (fetched: 13 modify tools)
11. https://help.autodesk.com/cloudhelp/ENU/Fusion-Patch/files/SFC-PATCH.htm and https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/PatchFeatureInput.htm (search summary)
12. https://help.autodesk.com/cloudhelp/ENU/Fusion-Patch/files/SFC-REF-LOFT-DLG.htm and https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/fusion_LoftRailEdgeConditions.htm (search summary)
13. https://help.autodesk.com/cloudhelp/ENU/Fusion-Patch/files/SFC-SWEEP.htm (search summary)
14. https://help.autodesk.com/cloudhelp/ENU/Fusion-Model/files/SLD-INSPECT-TOOLS.htm (fetched: analysis tool list)
15. https://help.autodesk.com/cloudhelp/2020/ENU/Inventor-Help/files/GUID-5A8568DF-5BDA-47B9-97FC-2722B8A78A89.htm (fetched: Boundary Patch, Stitch, Ruled Surface)
16. https://help.autodesk.com/cloudhelp/2022/ENU/Inventor-Help/files/GUID-7661F5F8-2C6C-42E9-8B7F-E7B62F2C7422.htm, https://help.autodesk.com/cloudhelp/2021/ENU/Inventor-Help/files/GUID-A9A4FF6E-9569-4BED-8AC7-EB245E6456F8.htm, https://help.autodesk.com/cloudhelp/2020/ENU/Inventor-Help/files/GUID-E554EB68-DE29-4D8E-A2ED-75BC0A218225.htm, https://help.autodesk.com/cloudhelp/2014/ENU/Inventor/files/GUID-02A26058-D239-4FAF-8C3A-77B06E66E99D.htm (search summary: trim, extend, sculpt, thicken/offset, delete face, replace face)
17. https://help.autodesk.com/cloudhelp/2027/ENU/Inventor-Help/files/GUID-C67F8283-DFDF-498F-89BE-91E131CE6171.htm (fetched: Zebra, Draft, Cross section, Curvature, Surface)
18. https://support.sw.siemens.com/en-US/okba/PL7009817/What-does-Geometric-Continuity-in-NX-mean,-and-how-are-they-defined/index.html (search summary)
19. https://docs.plm.automation.siemens.com/data_services/resources/nx/12/nx_api/custom/en_US/nxopen_net/class_n_x_open_1_1_features_1_1_bridge_surface_builder.html and the Bridge Curve builder pages (search summary)
20. https://docs.plm.automation.siemens.com/data_services/resources/nx/10/nx_api/en_US/custom/nxopen_python_ref/NXOpen.Features.ThroughCurveMeshBuilder.html (search summary; direct fetch redirects to a login portal)
21. https://eng-tips.com/threads/patch.337624 (search summary; forum)
22. https://eng-tips.com/threads/nx5-untrim-vs-enlarge.214532, https://support.sw.siemens.com/en-US/okba/PL8010675/Where-is-Edit-greater-Surface-greater-Boundary-in-NX/index.html, https://mechanical-engineering.com/nx-tutorial-surface-n-sided-through-curves-commands/, https://www.udemy.com/course/siemens-nx-surface-training/, https://community.wvu.edu/~bpbettig/MAE455/Lecture_16_freeform_surface_modeling.pdf (search summary)
23. https://docs.plm.automation.siemens.com/data_services/resources/nx/11/nx_api/custom/en_US/nxopen_python_ref/NXOpen.Features.StyledBlendBuilder.html, BlendCornerBuilder, and an eng-tips Studio Surface thread (search summary)
24. https://support.ptc.com/help/creo/creo_pma/usascii/surfacing/surface/To_Define_Boundary_Conditions.html (search summary)
25. https://support.ptc.com/help/creo/creo_pma/r12/usascii/surfacing/surface/About_the_Boundary_Blend_User_Interface.html (fetched: collectors, constraints, control points, influencing curves)
26. https://support.ptc.com/help/creo/creo_plus/usascii/surfacing/style/Glossary_for_Style.html and https://community.ptc.com/3d-part-assembly-design-327/does-boundary-blend-in-creo-4-have-the-option-for-g3-curvature-continuity-28512 (search summary)
27. https://learn.lboro.ac.uk/ludata/cd/cad/mod_level3/isdx.htm (search summary)
28. https://github.com/evereux/pycatia/tree/master/pycatia/hybrid_shape_interfaces (read via GitHub API: module list; hybrid_shape_sweep_explicit.py, hybrid_shape_sweep_line.py, hybrid_shape_loft.py, hybrid_shape_blend.py read for subtypes, modes, couplings, continuity)
29. https://pycatia.readthedocs.io/en/latest/api/pycatia/hybrid_shape_interfaces/hybrid_shape_fill.html (read as hybrid_shape_fill.py source: bounds, supports, constraints, C0/C1/C2 per boundary)
30. https://www.ibm.com/support/pages/node/5488983 (search summary: 0.5 deg G1 tolerance)
31. https://3dswym.3dexperience.3ds.com/wiki/catia-user-community/catia-freestyle-shaper-2-fss_lir3l79TSwCVSY6C61qf5g, the Freestyle Profiler 2 page, and https://www.udemy.com/course/class-a-surfacing-in-catia/ (search summary)
32. https://cad.onshape.com/help/Content/PartStudio/boundary_surface.htm (fetched)
33. https://cad.onshape.com/help/Content/PartStudio/fill.htm (fetched)
34. https://www.onshape.com/en/features/surfacing (fetched: features, curve tools, analysis, G0..G3 names)
35. https://docs.mcneel.com/rhino/8/help/en-us/commands/networksrf.htm (fetched)
36. https://docs.mcneel.com/rhino/8/help/en-us/commands/sweep2.htm (fetched)
37. https://docs.mcneel.com/rhino/8/help/en-us/commands/blendsrf.htm (fetched)
38. https://docs.mcneel.com/rhino/8/help/en-us/commands/matchsrf.htm (fetched)
39. https://docs.mcneel.com/rhino/8/help/en-us/commands/patch.htm (fetched)
40. https://docs.mcneel.com/rhino/8/help/en-us/seealso/sak_surface.htm (fetched: surface creation index)
41. https://docs.mcneel.com/rhino/8/help/en-us/seealso/sak_analysis.htm (fetched: analysis index)
42. https://docs.mcneel.com/rhino/8/help/en-us/toolbarmap/surface_tools_toolbar.htm, refittrim.htm, fitsrf.htm, shrinktrimmedsrf.htm (search summary)
43. https://doc.plasticity.xyz/solid/loft (fetched: Loft, and the site navigation listing every command)
44. https://doc.plasticity.xyz/solid/xnurbs (fetched)
45. https://doc.plasticity.xyz/solid/patch (fetched)
46. https://blogs.sw.siemens.com/solidedge/special-traits-of-the-bluesurf/ and https://www.soliddna.com/SEHelp/ST5/EN/a_h/consur17d.htm (search summary)
47. https://blogs.sw.siemens.com/solidedge/surfacing-techniques-from-the-trenches/ (fetched)
48. https://blogs.sw.siemens.com/solidedge/st6-four-new-or-improved-surface-features/ (fetched: Redefine, Intersect, Ruled, Bounded)
49. https://soliddna.com/SEHelp/ST6/EN/feature_modeling/xid614996.htm (search summary: Intersect)
50. This repository: src/Mod/Surface/App (FeatureFilling.cpp, FeatureGeomFillSurface.cpp, FeatureSections.cpp, FeatureExtend.cpp, FeatureCut.cpp, FeatureSewing.cpp, Blending/FeatureBlendCurve.cpp), src/Mod/Surface/Gui/Workbench.cpp, src/Mod/Part/Gui/Workbench.cpp, src/Mod/Part/App/PartFeatures.h, src/Mod/PartDesign/App/FeaturePipe.cpp, src/Gui/ShadingOptions.cpp (read)
51. https://github.com/FreeCAD/FreeCAD/tree/main/src/Mod/Surface (read via GitHub API: App file list, Gui/Workbench.cpp)
52. https://github.com/FreeCAD/FreeCAD/issues/31597 (read, with 10 comments)
53. https://github.com/FreeCAD/FreeCAD/pull/31089 (read); forum https://forum.freecad.org/viewtopic.php?t=95441 (not reachable: bot protection)
54. ~/works/sw/occt branch LinkVibe-801: src/ModelingAlgorithms/TKGeomAlgo/GeomFill/GeomFill_Gordon.hxx, GeomFill_NetworkSurface.hxx, git log (commits 1cc42af8d6, 02530f0217; tags V8_0_0, V8.0.1); presence of GeomFill_ConstrainedFilling, GeomFill_BoundWithSurf, BRepOffsetAPI_MakePipeShell (read)
55. https://github.com/FreeCAD/FreeCAD/issues/5587 (read, with 14 comments)
56. https://github.com/FreeCAD/FreeCAD/issues/15028 (read)
57. https://github.com/FreeCAD/FreeCAD/issues/12699 and its sub-issues #16103, #5587, #8754, #6010, #16114 (read)
58. https://github.com/FreeCAD/FreeCAD/issues/6255 (closed) and https://github.com/FreeCAD/FreeCAD/issues/6254 (read)
59. https://github.com/FreeCAD/FreeCAD/pull/32572, https://github.com/FreeCAD/FreeCAD/issues/32571, https://github.com/FreeCAD/FreeCAD/pull/32609, https://github.com/FreeCAD/FreeCAD/issues/32619 (read)
60. https://github.com/tomate44/CurvesWB (read via GitHub API: README, freecad/Curves module list)
61. https://github.com/FreeCAD/FreeCAD/issues/30312 (read)
62. docs/Sketch3DSurvey.md in this repository, section 1 and its sources [3], [20], [22], [35], [37] (curve tools in SOLIDWORKS, Inventor, NX, Creo 3D sketching)

---------------------------------------------------------------------------

# Part B. The Curves workbench, command by command

## B0. Source

Written by reading the clone; one correction made when this was filed: the
inventory assumed OCCT has no Gordon builder, which is true of the OCCT 7.x the
Curves workbench targets but not of our 8.0.1 kernel (docs/Surface.md sec 3.4).

- Repo: https://github.com/tomate44/CurvesWB, cloned to
  `/tmp/claude-1002/-home-thunder-works-sw-fcad/141730f2-39e3-4d28-a97d-9fa11c7a3f79/scratchpad/surf/CurvesWB`
  (first shallow, then unshallowed to get per-file history; the code was read, never run).
- HEAD: `c08e21ba16b8d6c6846cee3c9a6b1ea950987f36`, 2026-10-12 07:31:32 +0200, "ShapeMapper fix".
  package.xml version 0.6.82 (2026-10-08). 1047 commits since 2016-08-06.
- Licences: LGPL-2.1-or-later (LICENSE-CODE). Apache-2.0 (LICENSE-TIGL) applies to 4 files
  that are Python ports of DLR TiGL C++ code (see "Shared library modules").
- The README describes the workbench as "EXPERIMENTAL and should NOT be used for any serious
  work", and as "my personal playground for experimenting with geometric algorithms". Pull
  requests are off; issues go to the FreeCAD forum.
- Size: 120 .py modules, 32,996 lines. Static import closure from `init_gui.py`:
  **79 live modules, 24,618 lines**; **41 dead modules, 8,379 lines** (nothing imports them; the
  list is below).
- Open GitHub issues: I listed these with `gh issue list -R tomate44/CurvesWB` (the GitHub
  issues API, not the clone). There are 32 open issues. The ones that name a tool are cited in
  its row.

How the commands were found: `init_gui.py` `Initialize()` imports 46 modules, and each module
registers its command with `FreeCADGui.addCommand` at import time. The toolbars and menus are
three lists: "Curves" (12 entries), "Surfaces" (23) and "Misc." (7). That makes 42 toolbar
commands. Two more are registered but appear on no toolbar: `blendSurface` and
`SublinkEditor`. **44 registered commands in total.** The Tree context menu repeats
join/split/Discretize/Approximate/Interpolate, and the View context menu repeats
Curves_adjacent_faces/Curves_bspline_to_console.

Eight more files carry an `addCommand` call, but nothing imports them, so those commands are
never registered (listed at the end). `"Curves_ProfileSupport"`, `"Curves_Sweep2Rails"`,
`HQRuledSurface` and `HelicalSweep` are commented out in `init_gui.py`.

Legend:
- **FP** = parametric FeaturePython object (properties, recomputes). **Macro** = one-shot.
  **Overlay** = a view-only object or mode that makes no geometry.
- **OWN** in the engine column means the real geometry maths is the workbench's own Python. A
  C++ port has to reimplement it. **OCCT** means it is a thin wrapper over an OCCT or
  FreeCAD-Part call.
- "commits" counts the git commits on the main file. Most files were last touched on
  2025-10-20, the date of a tree-wide SPDX/cleanup commit, so the commit count is the better
  signal of activity.

## B1. Command inventory (44 registered)

### Curves toolbar (12)

| # | Command | Files (lines) | What it does | Category | Object | Geometry engine | Deps | Maturity |
|---|---|---|---|---|---|---|---|---|
| 1 | `Curves_line` "Parametric line" | lineFP.py (99) | Makes a line segment between two selected vertexes, and keeps it linked to them. | curve creation | FP (Part::FeaturePython) | OCCT: `Part.LineSegment` (GC_MakeSegment) | -- | Trivial. 13 commits. |
| 2 | `gordon_profile` "Freehand BSpline" | gordon_profile_FP.py (363), profile_editor.py (539), graphics.py (536) | Interactive interpolating BSpline. The user drags points in the 3D view; points snap onto shapes (stored as LinkSubList); tangent, linear-segment and periodic flags are keyboard toggles. Made to draw Gordon networks. | curve creation | FP + Coin editor | OCCT: `BSplineCurve.interpolate` with tangents/flags (GeomAPI_Interpolate). OWN: parametrisation (chord^alpha) and the whole pivy/Coin editor. | pivy | 39 commits. Issues #104 (cannot edit), #70. |
| 3 | `mixed_curve` "Mixed curve" | mixed_curve.py (231), approximate_extension.py (176) | 3D curve from two 2D curves seen from two directions: each is extruded along its view direction and the result is the intersection of the two extrusions. Can also build ruled faces back to the sources. | curve creation | FP | OCCT: `Shape.extrude` (BRepPrimAPI_MakePrism), `Surface.intersectSS` (GeomAPI_IntSS) on RectangularTrimmedSurface, optional `BSplineCurve.approximate` | -- | 27 commits, active 2026. |
| 4 | `extend` "Curve extend" | curveExtendFP.py (155), curveExtend.py (115) | Extends an edge at each end by a length, as a straight (G1) or curved (G2) extension. Output is a single joined edge or a wire. | curve modification | FP | OWN: the G2 extension is a hand-built degree-2 Bezier (control point from curvature radius and a circle/plane intersection, then trimmed to length). Join uses `BSplineCurve.join` (GeomConvert_CompCurveToBSplineCurve). OCCT has `GeomLib::ExtendCurveToPoint` that could replace it. | -- | 13 commits. Source comment: "TODO use basisSpline". |
| 5 | `join` "joinCurves" | JoinCurves.py (280), approximate_extension.py (176) | Joins selected edges (or every edge of an object) into one BSpline. Options: tolerance, break at sharp corners, force contact (snap poles), force closed, reverse, and origin shift for periodic results. Optional approximation extension. | curve modification | FP (+ ApproximateExtension property group) | OCCT: `toNurbs`/`toBSpline`, `BSplineCurve.join`, `removeKnot` (raise continuity), `setPeriodic`/`setOrigin`, `approximate` (GeomAPI_PointsToBSpline). OWN glue (forceJoin, forceC1Continuity) is small. | -- | 40 commits. Issue #134 (no update after sketch change). |
| 6 | `split` "Split curve" | splitCurves_2.py (694), graphics.py | Splits an edge or wire at given parameters, distances, percentages or N equal parts, or where cutting objects touch it. Includes a freehand Coin marker editor. Also feeds `segment_surface` (exposes NormalizedParameters). | curve modification | FP + Coin editor | OCCT: `Edge.split`, `distToShape` (BRepExtrema_DistShapeShape), `discretize` | pivy | 32 commits. Issues #105 (wrong place), #100 (crash moving point). |
| 7 | `Discretize` | Discretize.py (304) | Makes points along an edge or wire by Number / QuasiNumber / Distance / Deflection / QuasiDeflection / Angular-Curvature. Output is vertexes plus a Points list, which feeds Approximate/Interpolate. | utility | FP | OCCT: `Edge.discretize` (GCPnts_UniformAbscissa / QuasiUniform* / UniformDeflection / TangentialDeflection) | -- | 38 commits, stable. |
| 8 | `Approximate` | approximate.py (422) | Approximates a point set into a BSpline curve, or a point grid (Group of point objects) into a BSpline surface. Options: degree range, tolerance, continuity, parametrisation, smoothing weights (length/curvature/torsion), index range, closed. | curve creation (also surface creation) | FP | OCCT: `BSplineCurve.approximate` (GeomAPI_PointsToBSpline, incl. the variational smoothing form), `BSplineSurface.approximate` (GeomAPI_PointsToBSplineSurface) | -- | 39 commits, stable. |
| 9 | `Interpolate` | interpolate.py (386) | Interpolates points (from objects or vertex links) with optional custom tangents, periodic, polygonal, or wire output (split at points). A "Spiral" mode interpolates in the UV space of a periodic support face with N turns. | curve creation | FP | OCCT: `BSplineCurve.interpolate` (GeomAPI_Interpolate), `Geom2d.BSplineCurve2d.interpolate` + `toShape(surface)` for the spiral | -- | 31 commits, stable. |
| 10 | `ParametricBlendCurve` "Blend curve" | ParametricBlendCurve.py (750), blend_curve.py (972), manipulators.py (401), graphics.py, CoinNodes.py (471) | Bezier blend between points on two edges with C0/G1/G2/G3/G4 per end, a scale per end, and reverse flags. AutoScale minimises curvature variation. Double-click enables freehand editing. Output is a single edge, a wire, or a joined wire. | curve creation | FP + Coin editor | Mixed. The derivative-constrained Bezier solve is `Part.BezierCurve.interpolate([[P,D1,D2..],[..]])`, which is **FreeCAD C++ (Part, math_Gauss)**. OWN: PointOnEdge derivative scaling, auto-orient, and **AutoScale = scipy `minimize` (Nelder-Mead) over the two scales against its own curvature-regularity objective**. | numpy; scipy optional (AutoScale is hidden without it) | 77 commits, the most-worked curve tool. Already ported once to C++ (see sec. 3). |
| 11 | `ParametricComb` "Comb plot" | ParametricComb.py (608) | Curvature comb on edges, and on face isocurves (tangent or normal curvature), with scale, samples and colours. Clicking prints values. | analysis | FP (App::FeaturePython + Coin display) | OCCT: `curvatureAt`/`normalAt` (BRepLProp/GeomLProp). OWN trivial comb assembly and Coin drawing. | pivy | 27 commits. Issue #84 (FR: curve analysis reports). |
| 12 | `cos` "CurveOnSurface" | curveOnSurfaceFP.py (151), curveOnSurface.py (586) | Takes an edge lying on a face and outputs either that edge as a true curve-on-surface, or a ribbon face normal or binormal to the surface along it (width, symmetric, closed). The ribbon is the support face used to impose G1 on Filling/Gordon/blends. | curve creation (support-surface helper) | FP | OCCT: `Face.curveOnSurface` (BRep_Tool::CurveOnSurface pcurve), fallback `Face.project` (BRepAlgo_NormalProjection) + 2D BSpline join, `BSplineCurve.approximate` of sampled offset points, `Part.makeRuledSurface` | -- | 26 commits, stable. |

### Surfaces toolbar (23)

| # | Command | Files (lines) | What it does | Category | Object | Geometry engine | Deps | Maturity |
|---|---|---|---|---|---|---|---|---|
| 13 | `ZebraTool` | ZebraTool.py (153), Gui/Zebra_Gui.py (71) | Task panel that puts a zebra-stripe environment texture on the whole scene (stripe width, scale, rotation, colours). | analysis | Overlay (no object) | Coin: `SoTexture2` + `SoTextureCoordinateEnvironment`. No geometry. | pivy, PySide | 13 commits. Issue #64. Superseded by #32. |
| 14 | `Trim` "Trim face" | TrimFace.py (241) | Trims a face with curves projected along a direction (a vector or a linked edge), and keeps the piece under the picked UV point. | surface modification | FP | OCCT: `Wire.extrude` into cutting sheets, then `BOPTools.SplitAPI.slice` (BOPAlgo_Splitter), `Face.isPartOfDomain`. | -- | 14 commits. Issue #51 "Fix trim surface". |
| 15 | `IsoCurve` | IsoCurve.py (258), isocurves.py (221) | One or N x M isoparametric curves of a face, trimmed to its boundary. | curve creation | FP | OCCT: `Surface.uIso/vIso`, `Geom2d.Line2dSegment.intersectCC` against the face pcurves (Geom2dAPI_InterCurveCurve), `Line2d.toShape(face)`. OWN: small trimming logic. | -- | 23 commits, stable. |
| 16 | `SoS` "Sketch on surface" | Sketch_On_Surface.py (607) | Wraps a sketch attached to a face onto that face: the sketch's construction box is mapped to the face's UV range. Optional face filling, offset, thickness (extruded solid), and U/V reverse and swap. | surface modification | FP (+ creates a Sketcher::SketchObject) | Mostly OWN logic over OCCT primitives: a bilinear "quad" `BSplineSurface` built by `buildFromPolesMultsKnots`, `quad.project` (normal projection), `curveOnSurface` then `c2d.toShape(targetSurface)` (pcurve transfer), an OWN BoundarySorter to rebuild faces with holes, `Part.OffsetSurface`, ruled surfaces + `sewShape` for thickness. | Sketcher | 70 commits, very active (fixes 2026-10). Many issues: #161, #156, #144 (cones), #140 (segfault), #121, #111. |
| 17 | `Curves_MapOnFace` "MapOnFace" | MapOnFaceFP.py (243), ShapeMapper.py (721) | Generalised SoS: maps any source shapes onto a target face through a "flat map" object (usually from FlattenFace). Fill faces, offset, thickness, extrusion to solids. Inside a PartDesign Body it becomes an additive/subtractive feature. | surface modification | FP (Part or PartDesign::FeaturePython) | Same family as SoS: OWN ShapeMapper (Quad/TransferSurface param mapping, BoundarySorter, pcurve joining), OCCT `project`, `curveOnSurface`, OffsetSurface, ruled surfaces, `sewShape`, booleans (`fuse`/`cut`/`removeSplitter`) in PD mode. | -- | Docstring: "WORK IN PROGRESS. USE AT YOUR OWN RISKS. This will replace SketchOnSurface." Added 2025-06; ShapeMapper fixed 2026-10-12. |
| 18 | `sw2r` "Sweep2Rails" | Sweep2Rails.py (230), libS2R.py (422), CoinNodes.py | Sweeps profiles between two rails given as a ruled surface (the "birail"), with Average/Blend/Rail1/Rail2 mixing. Output is a **point cloud** that then needs Approximate. | surface creation | FP (App::FeaturePython with a Shape) | OWN: libS2R builds local frames on the birail, local profile transforms, interpolation curves between profiles and mixing; OCCT interpolate/approximate only for the pieces. | pivy | Tooltip says "DEPRECATED : Use Gordon surface." Still on the toolbar. |
| 19 | `profileSupportCmd` "Profile support plane" | ProfileSketch.py (163) | Planar face spanned by the line between points on two edges and an axis. A sketch support for drawing profiles between two rails. | utility | FP | OCCT trivial: LineSegment + `extrude`. | -- | 14 commits. Its sibling ProfileSupportFP is marked DEPRECATED ("Sketcher now has intersection geometry"). |
| 20 | `profile` "Pipeshell profile" | pipeshellProfileFP.py (177) | Profile holder for pipeshell: profile shape, location vertex on the spine, and contact/correction flags. | utility (pipeshell input) | FP | none (data holder) | -- | 13 commits. |
| 21 | `pipeshell` "Pipeshell" | pipeshellFP.py (390) | Full sweep of multiple profiles along a spine with every trihedron mode: Frenet (corrected), DiscreteTrihedron, FixedTrihedron, Binormal, ShapeSupport, AuxiliarySpine (contact, equal curvilinear). Max degree/segments and tolerances; outputs sections, lofted sections, surface or solid. | surface creation | FP | OCCT: `Part.BRepOffsetAPI.MakePipeShell` (all setters), `simulate` for previews, `makeLoft` | -- | 21 commits, stable. |
| 22 | `gordon` "Gordon surface" | gordonFP.py (226), gordon.py (596), BSplineAlgorithms.py (648), BSplineApproxInterp.py (540), curve_network_sorter.py (226) | Skins a network of profile and guide curves (a grid of crossing curves) into one BSpline surface. The network is auto-sorted, made compatible and intersected. Output is a surface or a wireframe preview. | surface creation | FP | **OWN (TiGL port, Apache-2.0)**: network sort, intersection parameters (seeded by `intersectCC`), curve reparametrisation through BSplineApproxInterp (its own least-squares fit with interpolation and kink constraints, **numpy `linalg.solve`**), skinning in both directions (BSplineAlgorithms.curvesToSurface, using GeomAPI_Interpolate pole-wise), tensor-product interpolation of the intersection grid, and the Boolean sum S_prof + S_guide - S_tensor done on poles. OCCT has no Gordon builder [correction: OCCT 8.0 has one, `GeomFill_Gordon`; see docs/Surface.md sec 3.4]. | numpy | 24 + 35 commits. Issues #185 (2026-08: "Attempt to create Gordon freezes FreeCAD"), #138 (recompute error 0.21 -> 1.0), #101. |
| 23 | `segment_surface` "Segment surface" | segmentSurfaceFP.py (222), nurbs_tools.KnotVector | Converts a face to BSpline and cuts it into patches: automatically at the highest-multiplicity knots, or at custom parameters, N equal parts, or parameters from a Discretize/Split object. | surface modification | FP | OCCT: RectangularTrimmedSurface `toBSpline` (GeomConvert), `BSplineSurface.segment` | -- | 19 commits, stable. |
| 24 | `Curves_JoinSurface` "Join Surfaces" | joinSurfacesFP.py (140), lib/bspline_surface.py (478), lib/geometry.py (138), BaseProxy.py (39) | Merges several adjacent faces into one BSpline surface. A gap is bridged with a ruled strip. Then tries to raise continuity across the seam and reports U/V continuity. | surface modification | FP | **OWN**: match orientation, reparametrise by average iso length, elevate degrees and insert knots (OCCT calls), then **concatenate the pole/weight grids by hand** (seam row averaged) and `buildFromPolesMultsKnots`. raise_continuity uses `removeKnot`. | -- | **New: added 2026-08-23 (0.6.8), 4 commits**, fixes 2026-09-25. |
| 25 | `comp_spring` "Compression Spring" | comp_spring.py (188) | Parametric compression spring (length, turns, wire diameter, diameter, end flatness, handedness). Output is the path wire or a solid. | other (parametric part) | FP | OWN: a 2D rational BSpline in the cylinder's UV space (end weights flatten the closed ends) mapped by `toShape(cylinder)`. OCCT MakePipeShell (Frenet) for the solid. | -- | 14 commits, stable, niche. |
| 26 | `ReflectLines` "Reflect Lines" | ReflectLinesFP.py (214), nurbs_tools.remove_subsegments | Silhouette/outline, smooth (G1), sewn (CN), sharp and iso lines of a shape for a view direction, visible or hidden, on the shape (3D) or projected to XY. | curve creation (from surfaces) | FP | OCCT: `Shape.reflectLines` (HLRBRep_Algo + HLRBRep_HLRToShape). OWN small duplicate-subsegment removal. | -- | 18 commits, stable. |
| 27 | `MultiLoft` | multiLoftFP.py (132) | Lofts objects made of several faces "in parallel": face i of each source is lofted to face i of the next, wire by wire, then closed into solids. | surface creation | FP | OCCT: `Part.makeLoft` (BRepOffsetAPI_ThruSections), Shell/Solid | -- | 9 commits. |
| 28 | `Curves_BlendSurf2` "BlendSurface" | blendSurfaceFP_new.py (162), blend_curve.py (972), curves_to_surface.py (719) | Blend surface between two edges, each on its own support face, with per-side continuity (G0..G3+) and scale lists along each edge. AutoScale modes: RegularPoles, MinimizeCurvature, Manual. | surface creation | FP | **OWN**: EdgeOnFace takes cross-derivatives from the support faces; a BlendCurve (constrained Bezier) is built at N samples; OWN skinning (CurvesToSurface: pole-wise interpolation of compatible curves, rational weights handled by hand); ruled rail surface; **OWN Gordon Boolean sum S1 + S2 - S3** on matched knots and degrees. MinimizeCurvature uses scipy. | numpy; scipy (MinimizeCurvature) | 9 commits (added 2021). Issue #115 (BlendSurface not working as expected). |
| 29 | `Curves_BlendSolid` "BlendSolid" | blendSolidFP.py (251), blendSolids.py (400), match_wires.py (166), blend_curve.py | Smooth solid bridging one face on each of two solids. Wires and vertices of the two faces are matched, one BlendSurface is built per edge pair, the result is sewn to a solid and optionally fused with the inputs. | surface creation (solid) | FP | OWN on top of #28 (MatchFaces/MatchWires correspondence, offsets); OCCT Shell/Solid/`fuse`. | numpy, scipy (optional) | 11 commits (added 2021). |
| 30 | `Curves_FlattenFace` "Flatten face" | FlattenFP.py (513) | Exact development (unroll) of a cylindrical, conical or extrusion face, holes included. Output in place (tangent at the seam) or on XY. Usable inside PartDesign (Part2DObjectPython). Its output is the "flat map" input for MapOnFace. | surface creation | FP | **OWN**: builds a flat BSpline surface whose parametrisation is isometric to the source (cylinder: scaled bilinear patch; cone: polar construction; extrusion: arc-length approximation of the directrix), then transfers each boundary pcurve with `c2d.toShape(flatSurface)` and rebuilds the face (`cutHoles`, `validate`). | -- | 16 commits. Issue #181 (fails on one face). The same code is duplicated in dead lib/flatten_face.py (460, 2026 WIP). |
| 31 | `Curves_RotationSweep` "Rotation Sweep" | RotationSweepFP.py (218), SweepPath.py (677), curves_to_surface.py | Sweeps profiles along a path and around a centre point (fan-like surfaces such as propeller or turbine-blade style shapes). Optional extra profiles, trim or extrapolate the path, smooth top. | surface creation | FP | **OWN**: Sweep/RotationSweep compute S1 (loft of profiles), S2 (rotational sweep of the path) and S3, combined with the OWN Gordon Boolean sum; SweepInterpolator makes local frames. OCCT only for interpolate/approximate/segment. | -- | 9 + 5 commits (added 2022). |
| 32 | `Curves_SurfaceAnalysis` "Surface Analysis" | SurfaceAnalysisFP.py (223), Zebra_shaders/Zebra_shader.py (209) | Shader overlay on selected objects: Zebra, Rainbow or Isophote, in a fixed or view-relative direction, with stripe count/ratio, colours, iso angles and shading. | analysis | Overlay (App::FeaturePython, settings on the ViewObject) | Coin GLSL: `SoShaderProgram`/`SoVertexShader`/`SoFragmentShader` | pivy | Docstring: "Experimental GLSL shader version of the Zebra tool." 8 commits. |
| 33 | `Curves_DraftAnalysis` "Draft Analysis" | DraftAnalysisFP.py (201), DraftAnalysis_shaders/DraftAnalysis_shader.py (192) | Colours faces by draft angle against a direction: two angle bands, each with in-draft / in-tolerance / out-of-draft colours. Clicking prints the measured angle. | analysis | Overlay on a Part::FeaturePython copy | Coin GLSL shader | pivy | 13 commits. |
| 34 | `Curve_TruncateExtendCmd` "Truncate Extend" | Truncate_Extend_FP.py (145), Truncate_Extend.py (52) | Cuts a shape with a plane and moves everything beyond it by a distance: positive stretches the shape (extrudes the cross-section), negative shortens it. PartDesign-aware. | surface modification (solid) | FP | OCCT: `extrude` half-space, `cut`/`common`/`fuse` (BRepAlgoAPI), `removeSplitter` | -- | 5 commits (added 2023). |
| 35 | `Curves_WaterlineCurves` "WaterLine" | WaterLineFP.py (160) | N plane sections (along an axis) of the selected faces. | curve creation (from surfaces) | FP | OCCT: `Plane.intersectSS(face.Surface)` (GeomAPI_IntSS). **Intersects the untrimmed surface, so results are not clipped to the face boundary.** | -- | 4 commits (added 2024-12). Waterline_shaders/ is unused. |

### Misc. toolbar (7) and non-toolbar commands (2)

| # | Command | Files (lines) | What it does | Category | Object | Geometry engine | Deps | Maturity |
|---|---|---|---|---|---|---|---|---|
| 36 | `GeomInfo` | GeomInfo.py (703), CoinNodes.py, lib/trimmed_surface.py (179) | Toggle mode: a HUD plus 3D markers showing degree, poles, knots, weights, multiplicities and continuity of the BSpline/Bezier under selection. | analysis | Overlay (toggle) | Reads Geom data; Coin display | pivy | 69 commits (refactored 2026-02; crash fix 2026-08). |
| 37 | `extract` "Extract subshape" | ExtractShapes.py (39) | Non-parametric copy of selected subshapes into Part::Feature objects. Same as Part_ElementCopy. | utility | Macro | -- | -- | Trivial. |
| 38 | `solid` "Parametric solid" | parametricSolid.py (209) | Sews faces into a shell and a solid, falling back to a compound. ShapeStatus property and icon colour report which; can show open edges. | utility | FP | OCCT: Shell + `sewShape` (BRepBuilderAPI_Sewing), Solid | PySide | 26 commits. |
| 39 | `pasteSVG` | pasteSVG.py (53) | Imports the SVG on the clipboard (for example copied from Inkscape). | utility | Macro | Draft importSVG | importSVG (Draft) | Trivial. |
| 40 | `to_console` "Objects to Console" | toConsole.py (96) | Pushes the selection into Python console variables. | utility | Macro | -- | -- | Trivial. Std_SendToPythonConsole does the same. |
| 41 | `Curves_adjacent_faces` | adjacent_faces.py (81) | Adds every face touching the selected face or edge to the selection. | utility | Macro | edge hashCode set intersection | -- | Trivial. |
| 42 | `Curves_bspline_to_console` "BSpline to Console" | curve_to_script.py (115) | Writes Python that rebuilds the selected BSpline/Bezier curves and surfaces. | utility | Macro | -- | -- | Trivial. |
| 43 | `blendSurface` (old; no toolbar) | blendSurfaceFP.py (333), blendSurface.py (236), curveOnSurface.py (586), property_editor.py (208) | Old blend surface between two CurveOnSurface objects. Builds cross blend curves and outputs the curve network (meant to be fed to Gordon). | surface creation | FP | OWN cross-curve + Bezier blend construction (its `buildCurves` is marked DEPRECATED in code) | PySide | Replaced by #28. 42 commits, historical. |
| 44 | `SublinkEditor` (no toolbar) | sublink_edit.py (296) | Qt widget to edit an object's LinkSub/LinkSubList properties. | utility | Macro/dialog | -- | PySide | 12 commits. |

### Commands in files that are never registered (dead)

| Command id | File (lines) | Content | Engine |
|---|---|---|---|
| `Plate` "Plate Surface" | PlateSurface.py (178) | Plate through points/objects | OCCT `Part.PlateSurface` (GeomPlate). Already covered by Surface_Filling. |
| `hq_ruled_surface` | HQRuledSurfaceFP.py (192) + reparametrize.py (205) | Ruled surface after reparametrising both rails to matched parameters | OWN reparametrisation (uses BSplineAlgorithms) + makeRuledSurface |
| `HelicalSweep` | HelicalSweepFP.py (297) | Sweep an open wire along a helical path | OWN + makeFace/sewing |
| `Curves_ContinuityCheckerCmd` | continuity_check.py (205) | Seam continuity sampler (title still "Title", doc "Doc": a template) | OCCT normals |
| `outline` | Outline_FP.py (169) | Outline curve of a shape | approximate on distToShape samples |
| `Curves_ProfileSupport` | ProfileSupportFP.py (183) + SweepObject.py (234) | DEPRECATED profile support | ruled surface |
| `Curves_Sweep2Rails` | Sweep2RailsFP.py (167) + SweepObject(_2).py | Newer Sweep2Rails (commented out in init_gui) | curves_to_surface |
| `tool_name` x2 | FaceMapFP.py (155), TemplateFP.py (96) | Template and "Face Map" stubs | -- |

Other dead modules (no command): Blending/smooth_objects.py (1473, scipy, a WIP smooth-surface
rewrite: "smooth_surface WIP" 2026-09-14), lib/flatten_face.py (460), lib/curve_fitting.py (376,
scipy `make_interp_spline`), lib/point_grid.py (272), lib/point_list.py (236),
lib/point_array.py (156), lib/surface_identifier.py (330, recognises canonical surfaces, WIP
2026-02..04), lib/curve_identifier.py (122), lib/face_builder.py, lib/surface_builder.py,
lib/logger.py, lib/timer.py, lib/math_util.py, nurbs_surface_match.py (307), ribbon.py (309),
grid.py (493), grid2.py (357), HUD.py (154), fix_wire.py, fuzzy_wire.py, helix_on_face.py,
import3DM.py, PointInterpolat*.py, PointParameters.py, selFilter.py. Most of the 2026 work is
in this lib/ tree and is not yet wired to any command.

## B2. Shared library modules

| Module | Lines | Licence header | Provides | Used by (live commands) |
|---|---|---|---|---|
| gordon.py | 596 | **Apache-2.0, (c) 2018 DLR**. Python port of TiGL `CTiglGordonSurfaceBuilder.cpp` + `CTiglInterpolateCurveNetwork.cpp` | GordonSurfaceBuilder (Boolean sum of skinned profiles and guides minus tensor surface), InterpolateCurveNetwork (sort -> intersect -> make compatible -> remove intersection inaccuracies -> build) | gordon (#22); imported by blend_curve.py but not called on its live path |
| BSplineAlgorithms.py | 648 | **Apache-2.0, DLR**. Port of TiGL `CTiglBSplineAlgorithms.cpp` | makeGeometryCompatible, common knot vectors, reparametrizeBSpline(ContinuouslyApprox), curve-curve intersections, curvesToSurface (skinning), pointsToSurface | gordon; dead HQRuledSurface via reparametrize.py |
| BSplineApproxInterp.py | 540 | **Apache-2.0, DLR**. Port of TiGL `CTiglBSplineApproxInterp.cpp` | Mixed least-squares approximation with exact interpolation points, kink support and closed-curve C0/C1/C2 conditions. Basis matrix built by hand; **numpy `linalg.solve` replaces math_Gauss**; iterative parameter optimisation by projection | BSplineAlgorithms -> gordon |
| curve_network_sorter.py | 226 | **Apache-2.0** (no copyright line). Port of TiGL `CTiglCurveNetworkSorter.cpp` | Orders and reverses profiles and guides into a consistent grid from the intersection-parameter matrices | gordon |
| blend_curve.py | 972 | LGPL-2.1+ (Chris_G) | PointOnEdge (point + scaled derivatives up to G4), BlendCurve (constrained Bezier; auto_orient; scipy minimisers: curvature regularity, pole regularity, angular variation), ValueOnEdge (scale law along an edge), EdgeOnFace (cross-derivative field off a support face, offset pcurve), BlendSurface (blend curves -> skin -> Boolean sum) | ParametricBlendCurve (#10), BlendSurf2 (#28), BlendSolid (#29) |
| curves_to_surface.py | 719 | LGPL-2.1+ | SurfaceAdapter, match_knots, ruled_surface with auto-twist/orient, CurvesToSurface (skinning by pole-wise interpolation incl. rational weights), **Gordon (S1 + S2 - S3 Boolean sum on matched surfaces)**, U_linear_surface, CurvesOn2Rails | blend_curve (#28/#29), SweepPath (#31); dead Sweep2RailsFP/SweepObject |
| nurbs_tools.py | 875 | LGPL-2.1+ | BSpline equality/duplicate/subsegment detection, BsplineBasis (own basis-function evaluation), KnotVector (normalise/transpose), parameterization, knot creation from parameters, curvematch, an older blendCurve class, join/reparametrize helpers, nurbs_quad, EdgeInterpolator | Discretize, FlattenFP, ReflectLines, segment_surface, splitCurves_2, blendSolids, BSplineApproxInterp, curve_network_sorter |
| curveOnSurface.py | 586 | LGPL-2.1+ | curveOnSurface class (pcurve with projection fallback, tangent/normal/binormal frames, cross curves, offset pcurves, normal/binormal ribbon faces) | cos (#12), old blendSurface (#43) |
| lib/bspline_surface.py | 478 | LGPL-2.1+ (2026) | BSplineFacade, continuity query, raise_continuity, smooth_surface, reverseU/V, match_orientation, isocurves, join_surfaces / join_multiple_surfaces | JoinSurface (#24) |
| SweepPath.py | 677 | LGPL-2.1+ | Sweep / RotationSweep (S1/S2/S3 construction), SweepInterpolator (frames), knot/degree sync facade | RotationSweep (#31) |
| ShapeMapper.py / map_on_face.py | 721 / 457 | LGPL-2.1+ | Param-space mapping (Quad, TransferSurface, ReversibleSurface), BoundarySorter, pcurve joining, face rebuild, offset/extrusion/solids | MapOnFace (#17); FlattenFP imports map_on_face but never calls it |
| libS2R.py | 422 | LGPL-2.1+ | Birail frames, local profiles, mixing | sw2r (#18) |
| match_wires.py | 166 | LGPL-2.1+ | Wire/vertex correspondence between two faces | BlendSolid (#29) |
| approximate_extension.py | 176 | LGPL-2.1+ | Reusable "ShapeApproximation" property group + approximate() | join, mixed_curve; dead Outline |
| graphics.py / manipulators.py / CoinNodes.py / profile_editor.py | 536 / 401 / 471 / 539 | LGPL-2.1+ | pivy interaction library (markers, drag, snapping), Coin node helpers, the freehand BSpline editor | Freehand BSpline, Split, BlendCurve, GeomInfo, Sweep2Rails |
| _utils.py | 366 | LGPL-2.1+ | getShape from LinkSub, pcurve helpers, ruled_surface, same_direction, geom_equal | ~25 modules |

TiGL note: all four Apache-2.0 files are ports of C++ that already exists upstream in DLR TiGL
(`src/geometry/CTigl*.cpp`, `src/guide_curves/CTiglCurveNetworkSorter.cpp`), written against
OCCT. A native Gordon could adapt the original C++, not re-port the Python; that is also the
route that avoids the Python's numpy-only solver. Licensing: Apache-2.0 is compatible with
(L)GPLv3 but not with GPLv2-only. FreeCAD is LGPL-2.1-or-later, so this matters only if
someone needs a v2-only combination. The Apache NOTICE/attribution has to be carried.

## B3. What src/Mod/Surface already covers

Our tree (read-only; currently on branch `Surface`). App: FeatureFilling (437), FeatureGeomFillSurface
(454), FeatureSections (96), FeatureExtend (210), FeatureCut (97), FeatureSewing (112),
Blending/{BlendCurve, BlendPoint, FeatureBlendCurve} (~760 incl. Py). The Gui registers
**6 commands**: Filling, GeomFillSurface, Sections, ExtendFace, CurveOnMesh, BlendCurve.
`Surface_Cut` is **commented out** in both `CreateSurfaceCommands()` and `Workbench.cpp`, so
it is not 7 commands.

| Surface command | Engine | Curves tool it covers |
|---|---|---|
| Surface_BlendCurve | `Surface::BlendCurve`: C++ port of the Curves blend curve by Matteo Grellier (2022). Builds the derivative-constrained Bezier with math_Gauss. Properties: Start/End Edge, Continuity (int), Parameter, Size. | **Covers the geometry of `ParametricBlendCurve` (#10).** Missing: AutoScale (the scipy curvature minimisation), Reverse flags, Wire/Joined output, freehand drag editing. |
| Surface_Sections | `GeomFill_NSections` through a list of section edges | Partial overlap with skinning (`curves_to_surface.CurvesToSurface`, MultiLoft #27), but profiles only: **no guide curves, so no Gordon network**. |
| Surface_GeomFillSurface | `GeomFill_BSplineCurves` / `GeomFill_BezierCurves` (Stretch / Coons / Curved) from 2-4 boundary curves | A 2-4-boundary patch with no inner curves. **Gordon (#22) is the generalisation to an N x M network.** |
| Surface_Filling | `BRepFill_Filling` (N-sided; G0/G1/G2 against boundary faces; free faces; unbound edges; points; initial face) | Covers the dead `Plate` and part of what Curves users do with blends. It needs support faces for G1, which is the job of `cos` (#12) ribbons. Blend surface (#28) is the two-edge, controllable-continuity case Filling does not give. |
| Surface_ExtendFace | Samples the face beyond its UV range and refits with `GeomAPI_PointsToBSplineSurface` | No Curves counterpart. Curves' `extend` (#4) is for curves; `Truncate/Extend` (#34) is a solid plane-cut stretch. |
| Surface_CurveOnMesh | MeshPart task (polyline on mesh -> BSpline) | No Curves counterpart. |
| Surface_Cut (unregistered) | `TopoShape::cut` | Not a face trim. Curves' `Trim` (#14) is the real trim-by-projected-curve. |

Elsewhere in our tree:
- Part: Loft (ThruSections) ~ MultiLoft. Part::Sweep (MakePipeShell, Frenet bool + transition
  only) and PartDesign::Pipe (Standard/Fixed/Frenet/Auxiliary/Binormal) cover part of
  `pipeshell` (#21), but neither has ShapeSupport mode, per-profile contact/correction,
  DiscreteTrihedron, or a surface-only feature in Part. Also RuledSurface, Offset2D,
  Part_ElementCopy (= `extract`), Part_ProjectionOnSurface (projection only; no UV mapping, so
  no SoS), Part CrossSections (one-shot, clipped; compare Waterline #35), Part shape builder
  (~ `solid`).
- Std_SendToPythonConsole = `to_console`. Sketcher's BSpline comb covers combs inside
  sketches only.
- **Renderer: a zebra matcap** (`MatcapConfig` preset 4, `RenderParams` MatcapPreset /
  MatcapStripes) covers `ZebraTool` (#13) and the Zebra mode of SurfaceAnalysis (#32). Rainbow,
  Isophote and Draft analysis (#32/#33) have no counterpart; they would belong in the bgfx
  renderer, not Coin GLSL.
- No Gordon, blend surface, curve-on-surface ribbon, sketch-on-surface, flatten, join-surfaces,
  isocurve feature, reflect-lines feature, or mixed curve anywhere in src/Mod.

## B4. Counts and assessment

Per category (44 registered):
- curve creation: 10 (line, Freehand BSpline, mixed curve, Approximate, Interpolate, BlendCurve,
  CurveOnSurface, IsoCurve, ReflectLines, Waterline)
- curve modification: 3 (extend, join, split)
- surface creation: 9 (sw2r, pipeshell, Gordon, MultiLoft, BlendSurf2, old blendSurface,
  BlendSolid, FlattenFace, RotationSweep)
- surface modification: 6 (Trim, SoS, MapOnFace, segment_surface, JoinSurface, TruncateExtend)
- analysis: 5 (Comb, ZebraTool, SurfaceAnalysis, DraftAnalysis, GeomInfo)
- utility: 10 (Discretize, profile support, pipeshell profile, extract, solid, pasteSVG,
  to_console, adjacent_faces, bspline_to_console, SublinkEditor)
- other: 1 (comp_spring)
- Object kind: 34 parametric FeaturePython features (Comb included), 4 view overlays/modes
  (Zebra, SurfaceAnalysis, DraftAnalysis, GeomInfo), 6 one-shot macros/dialogs.

Tools whose real geometry maths is the workbench's own (a C++ port must reimplement it):
- Gordon surface (#22): TiGL port, numpy least squares; C++ original available.
- BlendSurf2 (#28) and BlendSolid (#29): EdgeOnFace derivatives, own skinning, own Boolean sum.
- BlendCurve auto-scale (#10): scipy Nelder-Mead objective. The Bezier core is already C++.
- RotationSweep (#31): SweepPath S1/S2/S3 + Boolean sum.
- Sweep2Rails (#18, deprecated): libS2R.
- JoinSurface (#24): pole-grid concatenation.
- FlattenFace (#30): isometric flat parametrisation of cylinder/cone/extrusion.
- Sketch on Surface (#16) / MapOnFace (#17): UV mapping, boundary sorting and face rebuild (logic
  more than maths; the projection and pcurves are OCCT).
- curve extend G2 (#4), comp_spring path (#25), old blendSurface (#43): small own constructions.
- Analysis shaders (#32, #33): own GLSL.

Everything else is a thin OCCT wrapper and would be a cheap port: line, Interpolate,
Approximate, Discretize, join, split, mixed curve, CurveOnSurface, IsoCurve, ReflectLines,
Waterline, pipeshell, MultiLoft, segment, Trim, TruncateExtend, solid.

Ten most valuable for a surfacing user (port priority):
1. **Gordon surface** -- the network-of-curves skin (OCCT 8.0 now has `GeomFill_Gordon`: docs/Surface.md sec 3.4) that GeomFillSurface
   only does for 4 boundaries. Port from TiGL C++; the Python has a freeze bug (#185).
2. **Blend Surface (BlendSurf2)** -- G1/G2/G3 bridge between two face edges. It is the core
   class-A operation Surface lacks, and BlendSolid follows from it almost for free.
3. **Curve on Surface ribbon (cos)** -- makes the support face that Filling and other features
   need for tangency. A small OCCT port with high leverage.
4. **Pipeshell (+ profile)** -- every MakePipeShell mode (aux spine with contact, binormal,
   support shape, per-profile correction) as a surface feature. All OCCT.
5. **Join Curves** -- one clean C1/G1 BSpline from edge chains: the input hygiene that Sections,
   Gordon and GeomFill need. Small.
6. **Sketch on Surface / MapOnFace** -- wraps sketches onto curved faces (labels, grooves,
   embossing). The most-used and most-reported Curves tool. Moderate own logic.
7. **Trim face** -- trims a surface by a projected curve and keeps the picked side. The basic
   surfacing edit that Surface lacks (Surface_Cut is a dead boolean).
8. **Curvature comb + Rainbow/Isophote/Draft analysis** -- continuity inspection to go with the
   renderer's zebra matcap. Better redone in bgfx than ported from Coin.
9. **Flatten face** -- exact unroll of cylinder/cone/extrusion faces. Small own maths, ties into
   the sheet-metal wishlist, and supplies the flat map for MapOnFace.
10. **Curve extend + Isocurve + Mixed curve** (one bundle) -- cheap helpers for building curve
    networks: G2 extension so curves cross, iso-curves as network members, and 3D curves from
    two views.

Next tier: JoinSurface (valuable, but 2 months old), RotationSweep (niche), segment_surface,
ReflectLines/Waterline (Waterline needs face clipping), Interpolate/Approximate as parametric
features (the Part Python API already exposes them).
