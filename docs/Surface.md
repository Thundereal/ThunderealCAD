# Surface workbench: design (2026-10-27, proposal -- nothing built)

Status: DESIGN FOR DISCUSSION. The user asked on 2026-10-26 for three things in
the pending (end of 2026) release: a 3D sketch, a fuller Surface workbench and
sheet metal. The order ruled the same day is Surface first, sheet metal later,
both as native C++ workbenches rather than bundled Python addons. The 3D sketch
is done (docs/Sketch3D.md, S0 to S4). This is the Surface design, written
before any code, as the 3D sketch's was.

The survey it rests on is `docs/SurfaceSurvey.md`: Part A covers ten CAD
programs and upstream FreeCAD, cited here as [S n]; Part B inventories the
Curves workbench command by command, cited as [C n]. Claims about our own
code and about OCCT were read from the source on 2026-10-27, and section 3.4's
numbers were measured that day (`scripts/occt-gordon-probe.cpp`). The open
questions are section 12.

## 1. What it is for

Surface modelling builds shapes from curves rather than from solids:

1. draw a network of curves;
2. span it with surfaces;
3. trim them against each other;
4. sew the pieces;
5. close them into a solid.

It is how consumer products, ducts, hulls, car and aircraft skins, ergonomic
handles and moulds are made. It is also how a solid model gets a face that no
pad, pocket or fillet can make.

Every program in the survey has this workflow, and its core set is the same
everywhere [S, A5a]:
- **Create:** loft with guides, a two-direction network surface, one- and two-rail sweep, and an N-sided fill. Each offers G0/G1/G2 continuity per edge.
- **Edit:** offset, extend, trim and split, knit, and thicken.
- **Curves that feed surfaces:** projected, intersection and isoparametric curves.
- **Analysis:** zebra, curvature comb and curvature map.

FreeCAD has fragments of this:
- **Upstream** issue #31597, open since 2026-07-27, calls the Surface workbench "very limited", with surface functions "split across external Curve and Part wb" [S 52].
- **The oldest request** is the ability to trim a surface by another and keep the side you click. That is #5587, open since 2014 at Priority High [S 55].
- **The Curves workbench** does much of the rest. Its author calls it "EXPERIMENTAL and should NOT be used for any serious work" [C, B0].

The goal: a surfacing user can do the core workflow in this fork without an addon, and the result is a parametric feature tree like the rest of FreeCAD.

## 2. What other CAD programs do

The full tables are [S, A1-A5]. What shapes this design:

- **Per-edge continuity everywhere.** Every N-sided fill and network surface offers G0/G1/G2 per edge [S, A3]. A tool that offers only G0 is not competitive. Ours: only Filling has it. GeomFillSurface and Sections are G0 only [S, A5b].
- **Two ways to say which face to match.**
  - Implicit: the boundary is an edge of an existing face (SOLIDWORKS, Fusion, Inventor, Rhino).
  - Explicit: a support face is picked per boundary (CATIA, Creo, Onshape).
  - Explicit is needed when the boundary is a free curve lying on a face. The usual answer is implicit by default, with an explicit override [S, A3].
- **A weight per edge.** Every program has a scalar per edge that shapes the transition: tangent influence, Tangency Weight, Weight 0..1, Stretch, Magnitude, tension or bulge. Solid Edge's lack of one is criticised as giving "deflated" results [S 48].
- **G3 is a Class-A feature** (NX Studio, Creo Style, Onshape Flow, Rhino BlendSrf). The mainstream mechanical tools stop at G2 [S, A3].
- **The network surface is the main shape tool** in SOLIDWORKS (recommended over Loft [S 4]), NX, Creo, Onshape, Rhino and Solid Edge [S, A5c item 1].
- **Trim and split, with a side to keep, are in all ten programs** [S, A5a].
- **Analysis ships with surfacing:**
  - zebra in 8 of 10 programs;
  - curvature comb in 10 of 10;
  - curvature map in 10 of 10;
  - an edge-continuity checker in 7 [S, A5a].
  - Upstream asks that these work on any face, not only inside the Surface workbench [S 52][S 56].

## 3. What this fork has to build on

### 3.1 The Surface module today

The module is about 8,200 lines of C++. It registers 6 commands; `Surface_Cut` is commented out in `Command.cpp` and `Workbench.cpp`.

| Feature | Engine | State on OCCT 8.0.1 (smoke run 2026-10-27) |
|---|---|---|
| Filling | `BRepFill_Filling` (GeomPlate): boundary, unbound and free constraints, G0/G1/G2 per constraint with a support face, points, an initial face | builds |
| GeomFillSurface | `GeomFill_BSplineCurves` / `BezierCurves`, 2 to 4 boundaries, Stretched / Coons / Curved | Stretched and Curved build. **Coons fails** with "invalid filling style" when a curve has fewer than 4 poles (an OCCT input rule, `GeomFill_BSplineCurves.cxx:321`). Raising the curves to degree 3 makes it build, so the feature can do that itself. |
| Sections | `GeomFill_NSections` through section curves; no guides, no end conditions | builds |
| Extend | samples the face beyond its range and refits (`GeomAPI_PointsToBSplineSurface`), by fractions of the whole face | builds |
| Cut | a boolean cut, no command | builds |
| Sewing | `BRepBuilderAPI_Sewing`, no command | builds |
| BlendCurve | a C++ port of the Curves blend curve (2022): continuity 0..25 per end, parameter and size | builds; tests exist (`TestBlendCurve.py`, the module's only tests) |

Since our last upstream merge, upstream has added a Blend Curve task panel, `Gui/Blending/TaskBlendCurve.*` (about 460 lines plus a .ui). Our Blend Curve has none.

### 3.2 Part, PartDesign and the 3D sketch

- **Part** already has loft (`ThruSections`, no guides), sweep (`MakePipeShell`: Frenet, a transition mode, no second rail), ruled surface (two edges), offset, thickness, extrude, revolve, filled faces (`makEFilledFace`), projection on surface and section [S, A2.11].
- **PartDesign** has Pipe with an auxiliary spine, but it sets orientation only. It also has the fork's Split, which splits solids by tools.
- **The 3D sketch** is done. Its curves are network curves by design (docs/Sketch3D.md sec 1, "curve networks for surfaces"). Since S3d, Part has `Part.CurveOnSurface`: a 2D curve in a face's (u, v) space plus the surface. Its edge carries the pcurve, so it is exact on the face. It is the natural result type for isocurves, intersections and trim curves (section 6).
- **Topological naming:** a feature's output carries an element map when it is built through `TopoShape::makE*`. A surface's edges then keep their names across recomputes, which matters here because the next surface's G1 condition refers to them.

### 3.3 The renderer

The bgfx renderer has a zebra matcap (`Render_MatcapPreset` Zebra, `Render_MatcapStripes`). It is view-space, so its stripes move with the camera. That is good for a quick look. A surfacing check usually wants stripes fixed to the model, so they stay put while you orbit and a kink shows as a stripe that steps. Curvature map, draft analysis and combs on faces do not exist. Sketcher has a 2D curvature comb inside sketches only.

### 3.4 OCCT 8.0.1

Our kernel (`LinkVibe-801`) has the classic set:
- `BRepOffsetAPI_ThruSections`, `MakePipeShell` (every trihedron mode, auxiliary spine, contact), `MakeFilling`, `MakeOffsetShape`, `MakeThickSolid`, `MiddlePath`;
- `GeomFill_NSections`, `Coons`, `Curved`, `Stretch`, `ConstrainedFilling` and `BoundWithSurf` (a 4-sided fill with G1 to a surface along a boundary);
- `GeomPlate`, `GeomLib::ExtendSurfByLength`;
- `BRepAlgoAPI_Splitter`, `BRepFeat_SplitShape`;
- `LocalAnalysis_SurfaceContinuity`.

**New in OCCT 8.0: a Gordon builder.** `GeomFill_Gordon` and the lower-level `GeomFill_NetworkSurface` were added upstream between 2026-03-03 and 2026-07-11 (commits 1cc42af8d6, 02530f0217, d3042539c7), with 64 GTest cases.
- `GeomFill_Gordon` takes arbitrary curves. It finds their crossings, orders the network, reparametrizes the curves so they cross at shared parameters, and builds the surface.
- `GeomFill_NetworkSurface` does only the last step, on a network that is already compatible.
- Upstream FreeCAD's draft PR #31089 wraps it, or the occ_gordon library, as a Gordon feature [S 53].

The Curves workbench's Gordon is a Python port of DLR TiGL (Apache-2.0, about 2,000 lines with a numpy solve) [C 22]. We would not need to port it.

**Measured** (`scripts/occt-gordon-probe.cpp`, 2026-10-27). Each network was built so its curves cross exactly. "Measured" is the worst distance from points sampled on the input curves to the result, computed independently of the builder's own report.

| Network | Result | Time | Surface |
|---|---|---|---|
| saddle 3 x 3, curves spaced evenly | exact (4e-15) | 3-4 ms | degree 3, 9 x 9 poles, C2 inside |
| wave 5 x 4, uneven spacing | exact (report 1e-13) | 126-141 ms | **degree 9, 217 x 212 poles, interior knots of multiplicity 9** |
| duct 4 x 6, rational arc profiles | exact (9e-14) | 12-15 ms | degree 9, 33 x 45 poles |
| tube 5 x 4, closed | exact (2e-13) | 33-40 ms | degree 9, 86 x 87 poles |
| wave 12 x 12 | exact (report 2e-13) | **13.8-19.9 s** | **degree 9, 2004 x 2004 poles (4 million)** |
| saddle with one profile 0.01 off, tol 1e-4 / 0.02 | fails (IntersectionFailed) / builds with a 0.01 gap | 1-22 ms | -- |

So OCCT's Gordon is exact and robust, but:
- **Uneven crossings bloat it.** When the crossings are not already at shared parameters, the result is bloated and, inside, only C0 in its parametrization.
  - The exact reparametrization (`reparametrizeExactly`, a piecewise-linear parameter law) inserts full-multiplicity knots at every crossing.
  - The result came out degree 9 and flagged rational, even for polynomial cubic input with all weights 1. Where the degree 9 comes from is not traced yet.
- **A C0 parametrization is a trap for what comes next.** Offset and thicken generally need C1 or better (gk, OCCT's offset algorithm), and a face that size is costly to tessellate, evaluate and save. An evenly spaced network, like the first row, has none of this.
- **It is G0 only.** It has no tangency or curvature conditions to adjacent faces.

**What follows (analysis).**
- Use `GeomFill_NetworkSurface` for the final step.
- Do the reparametrization ourselves, by approximation within a tolerance that keeps degree 3 and C2. TiGL works that way (`BSplineApproxInterp`) [C, B2].
- Add the edge conditions ourselves (section 4.3).
- The fix could live in our OCCT fork and be offered upstream, as a mode beside OCCT's `AllowApproximateFallback`. Question b.

### 3.5 The Curves workbench

LGPL-2.1+. Of its 32,996 lines, 24,618 are reachable from its command registration; about half of the author's 2026 work sits in the unreachable `lib/` tree: recognising faces that are really planes, cylinders or cones, and fitting curves through points [C, B4].
- **44 registered commands** [C, B4].
- **11 carry maths of their own** that a port would have to rewrite:
  - Gordon;
  - BlendSurf2 and BlendSolid;
  - BlendCurve's auto-scale;
  - RotationSweep, Sweep2Rails, JoinSurface;
  - FlattenFace;
  - Sketch on Surface / MapOnFace;
  - small curve constructions.
- **The rest are thin OCCT wrappers** and cheap to redo in C++.

The design takes its tool list and ideas from Curves, not its code. It is a reference, not a source to translate (section 11).

## 4. Architecture

### 4.1 Two layers

- **`Surface/App/Geom/`: geometry, no document.**
  - Functions over `Geom_*` and `TopoDS_*`: network preparation, edge conditions, blend construction, trimming helpers.
  - Each is tested by GoogleTest in `tests/src/Mod/Surface/` with known geometry, exactly as S0 tested the 3D sketch's solver layer.
  - Exposed to Python under `Surface.Geom` (for example `Surface.Geom.network(profiles, guides, tol, ...)`), so scripts and the agent can build surfaces without features.
- **`Surface/App/Feature*`: parametric features.**
  - Each reads links, calls the geometry layer, and publishes a `TopoShape` built through the `makE*` path, so its faces and edges are named (section 4.4).
  - Tested from Python (`SurfaceTests/`), like Sketch3D's S1 tests: build, recompute after an input changes, save and reopen, and a downstream reference that survives.

Gui commands and task panels sit on top, as in the existing module.

### 4.2 Inputs

- **Curves** are `App::PropertyLinkSubList` edges from any source: sketch edges, 3D sketch edges, model edges, and whole wires.
- **A chain of edges** that is G1 is treated as one curve: joined, then refitted where needed. This is Curves' "Join Curves" [C 5] applied quietly as input hygiene, which Curves leaves to the user.
- **A support face** comes in two ways:
  - **implicit:** the curve is an edge of a face in its source shape, the default;
  - **explicit:** a `Face` link per edge, which overrides it.
  - This is the survey's recommended pattern [S, A3].

### 4.3 Edge conditions: one module for every surface tool

```
struct EdgeCondition {
    int order;            // 0 = G0, 1 = G1 (tangent), 2 = G2 (curvature)
    TopoDS_Face support;  // implicit or explicit; null = free
    double weight;        // >0, scales the cross-boundary derivative; 1 = natural
};
```

The operation, applied to a B-spline surface and one of its four boundaries, is called **match boundary**:
1. Sample the support face's tangent plane (and, for G2, its curvature) along the shared edge.
2. Fit the target cross-boundary derivative fields in the surface's own parameter.
3. Refine the surface's knots in the boundary direction until the fit is within tolerance.
4. Set the second row of poles (G1), and the third (G2), from those fields.
5. Report what was achieved: the worst angle and the worst curvature difference along the edge.

Every surface tool uses the same module:
- the network surface (each of its four outer boundaries);
- Sections and loft ends;
- GeomFillSurface, which goes from G0 to G2;
- blend surface (both sides);
- Extend's "natural" mode (G2 to itself).

One implementation to test, and one meaning of "weight" across tools (survey question [S, A6]).

Two things to say plainly:
- **It is an approximation within tolerance, as in every program surveyed.** CATIA calls an edge G1 under 0.5 deg [S 30], and NX users report checking continuity afterwards [S 21]. The achieved values are reported on the feature, and the analysis tools of section 7 check them in the view.
- **Corners are the hard part.** Where two G1 boundaries meet, the twist at the corner has to agree with both supports. Where it cannot, the feature says so on the corner, rather than silently loosening one edge (risk 1).

### 4.4 Names

A surface feature's output names its edges after what made them. A boundary edge is generated from the input curve it lies on, with the op code of the feature. A network surface's face names its profiles and guides.

This is what lets the next feature hold "this surface's left edge" as its G1 boundary across a change upstream. It is tested in every phase with a reference that survives a recompute after an input curve moves.

## 5. The tools

"Engine" is where the geometry comes from. "Ref" is the Curves command that does something similar, consulted for ideas, not ported.

### 5.1 Creation

| Tool | What it does | Engine | Ref |
|---|---|---|---|
| **Network surface** | Spans profiles x guides, with an edge condition and a weight per outer boundary. Inner curves are interpolated. | own preparation (sort, crossings, approximate reparametrization keeping degree 3 / C2) + `GeomFill_NetworkSurface` + match boundary | C 22 |
| **Sections / loft with guides** | The existing Sections gains end conditions (G0..G2 to a face at the first and last section) and guides. Sections plus guides is a network. Sections alone stays `GeomFill_NSections`. | as above | C 27 |
| **GeomFillSurface** | Unchanged inputs; gains edge conditions; Coons raises low-degree curves itself. | `GeomFill_*` + match boundary | -- |
| **Blend surface** | A bridge between two edges (or chains) on two faces. G0..G2 per side, a weight per side (along each edge, later). | blend curves across the gap from both supports' derivatives, then a network built from them (G3 later) | C 28, 29 |
| **Two-rail sweep** | Sections swept between two rails that scale them. | sections placed along the rails, then a network | C 18 |
| **Pipeshell** | Part Sweep exposes `MakePipeShell` only partly. A Surface feature exposes every mode: auxiliary spine with contact, binormal, support face, and per-profile correction. | `BRepOffsetAPI_MakePipeShell` | C 21 |
| **Filling** | Unchanged; gains a weight per edge if GeomPlate's constraint order and tolerance can carry one (to measure). | `BRepFill_Filling` | -- |

### 5.2 Editing

| Tool | What it does | Engine | Ref |
|---|---|---|---|
| **Trim** | Cuts a face or shell by curves, surfaces or a sketch, and keeps the side you click. The kept side is stored as a 3D point and re-found as the nearest piece on recompute (question e). Mutual trim of two surfaces is the same with both as tools. | `BRepAlgoAPI_Splitter` (+ projection of curves onto the face as `CurveOnSurface`) | C 14 |
| **Split** | As Trim but keeps every piece. | as above | -- |
| **Untrim** | Restores a face to its natural boundary, or to the boundary it had before a chosen trim. | the face's underlying surface | -- |
| **Extend** | Per edge (not whole face), by a length or up to a face. Modes: natural (the same surface continued, `GeomLib::ExtendSurfByLength`, G2 with itself) or linear (tangent). The existing whole-face mode stays for files that use it. | OCCT | -- |
| **Sewing** | The existing feature gets a command, plus a gap readout of free edges and the largest gap, like Inventor's Stitch analysis [S 15]. | `BRepBuilderAPI_Sewing` | C 38 |

### 5.3 Curves for surfaces

| Tool | What it does | Engine | Ref |
|---|---|---|---|
| **Isocurve** | One or N x M isoparametric curves of a face, clipped to its boundary, as `CurveOnSurface` edges. | OCCT | C 15 |
| **Curve on surface** | Projects a curve onto a face (along a direction or the normal) and yields a `CurveOnSurface`. Optionally a ribbon face along it, as a support for a G1 condition where no face exists. | `BRepOffsetAPI_NormalProjection`, `BRepProj` | C 12 |
| **Intersection curve** | Where two surfaces meet, as edges on both. | `GeomInt_IntSS` / `BRepAlgoAPI_Section` | -- |
| **Join curves** | One smooth B-spline from a chain, with tolerance, a break at corners, and closing. | OCCT | C 5 |
| **Curve extend** | An end extended straight (G1) or curved (G2), so a curve reaches across the network. | `GeomLib::ExtendCurveToPoint` | C 4 |

### 5.4 Not in this release (section 11 says why)

- Sketch on Surface / MapOnFace [C 16, 17].
- Flatten [C 30]: with sheet metal.
- Match surface.
- Replace face and delete-and-fill.
- Rebuild / refit.
- Rotation sweep and join surfaces [C 31, 24].
- G3.

## 6. The 3D sketch and the surface

The 3D sketch was designed partly to draw curve networks (docs/Sketch3D.md sec 1). The two meet in three places:
- **A network drawn in one 3D sketch.** Profiles and guides are sketch edges, and their crossings are coincident constraints. Crossings that meet exactly are what the network needs (section 3.4: a profile 0.01 off fails at the default tolerance). The network surface accepts a whole 3D sketch and sorts its edges into two families, which the user can correct.
- **Curves on faces.** The 3D sketch's spline on face (S3d) and the curve tools of 5.3 all produce `Part.CurveOnSurface`. That is the exact type Trim and the edge conditions want.
- **G1 to a model edge** in the 3D sketch (S3e) uses the same tangent field as match boundary does, so the two share code.

## 7. Analysis

The analysis tools belong in the view, not in the Surface workbench. Upstream asks for exactly that [S 52][S 56], and in this fork they work in the browser too, because the renderer is shared.

| Tool | How | Where |
|---|---|---|
| **Zebra, fixed to the model** | The existing matcap zebra's shader with a model-space direction in place of the view-space one; a toggle beside it. | renderer shader |
| **Draft analysis** | Colour by the angle between the normal and a pull direction: two bands, with in-draft, in-tolerance and out-of-draft colours [C 33]. A shader of the normal alone. | renderer shader |
| **Curvature map** | Gaussian, mean, min or max curvature, coloured. Needs curvature per vertex, evaluated from the OCCT surface at the tessellation's (u, v) nodes and carried as a vertex attribute (also in the scene stream). | tessellation + renderer |
| **Curvature comb** | On edges and on face isocurves. Overlay geometry, per object, like a dimension. | Gui overlay, any object |
| **Edge continuity** | Along an edge between two faces: the gap, the angle and the curvature difference, sampled, with the worst point marked. The readout behind every "achieved" number of section 4.3. | `LocalAnalysis_SurfaceContinuity`, Gui readout |

## 8. Editing in the view

- **Edge conditions in the view.** Each outer boundary of a network, blend or fill shows a marker at its middle. A click cycles G0/G1/G2, as Plasticity's loft does [S 43]. Dragging the marker changes the weight. The task panel has the same per edge, with "apply to all".
- **Preview while editing.** The preview recomputes as inputs change, as the existing task panels do. A network too large to preview at full fidelity is previewed at reduced sampling and labelled so.
- **The browser.** The task panels mirror as the 3D sketch's did in S4. The new commands go on the browser allowlist; the three copies are listed in docs/Sketch3D.md S4.

## 9. Phases and tests

| Phase | Content | Tests | Size |
|---|---|---|---|
| U0 | **Foundation and repairs.** The `Geom/` layer and its test target. Coons raises low-degree curves. A Sewing command with a gap readout. A ruling on Cut (question h). Upstream's Blend Curve task panel taken over. Check that every existing feature's output carries an element map. | gtest target `Surface_tests_run`; one Python test per existing feature (only BlendCurve has one today) | S |
| U1 | **Network surface and edge conditions.** Approximate reparametrization; `GeomFill_NetworkSurface`; match boundary G1/G2 with a weight; corners. Sections and GeomFillSurface gain edge conditions through the same module. | gtests on known geometry: a network of iso-curves of a known surface reproduces it; G1/G2 achieved within the stated tolerance against a known neighbour; pole counts bounded (the 12 x 12 network of 3.4 under 100 x 100 poles, under a second). Python: a network from a 3D sketch, its edge named, a G1 neighbour that survives an input change, save and reopen. | L |
| U2 | **Trim, split, untrim, extend per edge.** | Python: trim keeps the clicked side across an input change; mutual trim; untrim restores; extend natural is G2 with itself (measured) | M |
| U3 | **Blend surface, two-rail sweep, loft guides, Pipeshell.** | per tool, as U1; blend's achieved continuity on both sides | L |
| U4 | **Analysis**: model-fixed zebra, draft, curvature map, comb, edge continuity. | render tests in tests/render (a known surface's curvature map against computed values); a browser test as in S4 | M |
| U5 | **Curve tools**: isocurve, curve on surface, intersection curve, join, extend. | Python per tool; `CurveOnSurface` results lie on their face exactly | M |

**Test rule, from the 3D sketch:** each phase's tests are written first and scored on the code before the change, so a pass means something.

## 10. Risks

1. **Corner compatibility.** G1 or G2 on two boundaries that meet needs the corner's twist to agree with both supports. Some inputs cannot satisfy both. The plan is to report it per corner, not to hide it (4.3).
2. **Size.** As section 3.4 shows, an exact construction can be 4 million poles. Every surface tool gets a size bound in its tests.
3. **Booleans on trimmed B-spline faces.** Trim and sew lean on OCCT's boolean and sewing code with faces it finds hardest. Expect kernel cases. Improving the kernel is a project focus, and our OCCT fork is where they get fixed.
4. **Fillet ownership.** Rolling-ball blends between faces (`ChFi3d`) belong to the `mac` session's fillet work. The blend surface here is curve-network based and does not touch ChFi3d. If a "variable rolling-ball blend surface" is wanted, it goes to that session.
5. **Upstream overlap.** Upstream has a draft Gordon feature (#31089) and open intersection-curve and freehand-B-spline PRs (#32572, #32609) [S 53][S 59]. Ours will differ (edge conditions, approximate reparametrization). Merges from upstream in `src/Mod/Surface` will conflict and need resolving by hand.

## 11. Not proposed

- **Porting the Curves Python.** Its maths is mostly in OCCT now, or is better redone against our geometry layer. Its own author does not stand behind it for serious work [C, B0].
- **Porting TiGL's C++ (Apache-2.0).** OCCT 8.0 has Gordon. We need the reparametrization idea, not the code.
- **T-splines / SubD** (Fusion's Form) and **Class-A G3** in this release.
- **Bundling Curves as an addon.** Ruled out on 2026-10-26: native C++.

## 12. Questions

a. **Scope for the end-2026 release.** Proposed: U0 to U3, then U4 (analysis), then U5 if there is time. Or a cut: U0, U1, U2 (network, edge conditions, trim) as the release's surfacing, and the rest after.

b. **Where the network fix lives.**
   - (1) In Surface's own geometry layer: our reparametrization, then OCCT's `GeomFill_NetworkSurface`.
   - (2) In our OCCT fork, as a mode of `GeomFill_Gordon` that approximates the reparametrization, offered upstream.
   - (2) helps every OCCT user and keeps one engine. But a new mode is a new member in an exported class, which is an ABI change to ask about first.
   - Recommendation: (1) now, with the same code proposed to OCCT later.

c. **Edge conditions approximate within tolerance**, with the achieved values reported, as every program surveyed does. Or exact-only constructions where OCCT has them (the 4-sided `GeomFill_ConstrainedFilling`) and nothing elsewhere. Recommendation: approximate, reported.

d. **The support face**: implicit from the edge's own face, with an explicit link per edge to override. Recommendation: both.

e. **Trim's kept side** is stored as a 3D point and re-found as the nearest piece after a recompute. The alternative is a region named through the element map, which is sharper but breaks when the trim's tool changes the piece count. Recommendation: the point, with the element name kept as a tie-break.

f. **Analysis in the renderer and Gui core** (any face, any workbench, the browser), not as Surface objects. Recommendation: yes; comb as a per-object overlay.

g. **The existing features stay** and grow: Sections gets guides and end conditions, and GeomFillSurface gets edge conditions; old files open unchanged. The network surface is a new feature. The alternative is new features replacing them, with a migration.

h. **Cut**: register it as Split / Trim's predecessor, or drop it in favour of Trim. Recommendation: drop it from the toolbar for good, keep loading old files.

i. **Upstream's Blend Curve task panel**: take it into U0 as is. Recommendation: yes.
