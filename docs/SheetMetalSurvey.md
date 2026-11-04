# Sheet metal survey: what CAD programs offer, and what the SheetMetal addon has

Date of survey: 2026-10-30. The user's rule, the same day: survey sheet metal against other CAD
programs, and if the FreeCAD SheetMetal addon is slow or limited in features, this fork builds a
native one. The order was ruled on 2026-10-26: Surface first (docs/Surface.md, built), sheet
metal after it, both as native C++ workbenches rather than bundled Python addons; Surface's sec
5.4 deferred Flatten to this work.

- **Part A** surveys the sheet metal tools of nine CAD programs (and notes two more) and of
  upstream FreeCAD, cited as [n] from its section A8.
- **Part B** inventories the SheetMetal addon (shaise/FreeCAD_SheetMetal 0.8.26, commit
  cbf6595b68), read from its code and its issue tracker; issues are cited as #n.

## Verdict

**Limited in features: yes.** Against the core set every commercial program has (Part A, A1 and
the synthesis in A7), the addon covers base walls, flanges with miters and reliefs, hems, sketched
folds, cutouts, corner reliefs, convert-from-solid, user forming tools and a K-factor unfold with
DXF/SVG export (B1, B6). It lacks what the second tier takes for granted: refold and features in
the flat state (#296), lofted and conical bends (#603), cross breaks, bend notches (#622), a
forming library, bend allowance and deduction tables and per-bend overrides, multi-body and
assembly flat export (#462), and an unfold that keeps both skins on its default path (#493,
#563). Its correctness debt: 35 open construction bugs, 11 open unfold failures, a K-factor table
interpolation that is wrong past a table's second row (B2), and a test suite that checks no
geometry.

**Slow: likely on large parts, by its structure; not yet measured.** Every feature copies the
whole incoming solid and re-runs booleans on it, uses booleans as a geometric search, and a chain
of N features is O(N^2) work that re-runs entirely on an upstream edit; the default unfolder
rebuilds an ancestor map per edge (B2, "Complexity"). The tracker has one slow case (#623, a
minute for one part) and Unfold was made manual-update because it was slow (#412). The timing
script (`~/.cache/lv-merge/sheetmetal/bench.py`: N = 1, 4, 16, 64 flanges in one feature and in a
chain of features, then unfold) is written but **has not been run**: running the addon's
downloaded code needs the user's permission (2026-10-30).

**Upstream:** core FreeCAD has no sheet metal module; the PR that would have moved the addon into
core (#31913 on FreeCAD/FreeCAD) was closed unmerged on 2026-09-21, the maintainers asking for an
FEP first (A5). The addon is LGPL-2.1-or-later, so its algorithms, or its code translated,
can be reused in this fork's core with the notices kept (B0).

**So, by the rule: a native workbench.** Part B's B6 lists the day-one core it must match and
what it can do better -- features that know the bends they make (element-mapped, no
boolean-as-search), a flat pattern from the feature history with a geometry-only unfolder kept for
imported parts, refold, bend tables, and unfolding out of process. The design comes next, as
docs/Surface.md did: written before any code, with its questions for the user.

# Part A. Sheet metal tools in CAD programs

Date of survey: 2026-10-30. Input to the decision whether this fork builds a native C++ sheet
metal workbench.

Conventions:
- `[n]` is a numbered source in section A8.
- "(search summary)" on a source means only a search-engine summary was seen, not the page itself.
- "(gk)" marks general knowledge that no source here confirms. Treat it as a lead to check.
- "(analysis)" marks this survey's own inference from the cited facts.

Access notes: the SOLIDWORKS help pages render only through JavaScript, so every SOLIDWORKS fact
is a search summary of help.solidworks.com or of reseller blogs. Siemens NX documentation
(docs.plm.automation.siemens.com) redirects through scredirect.docs.sws.siemens.com to the
support.sw.siemens.com login portal, so NX facts are NXOpen API search summaries, Siemens blog
posts (fetched) and course outlines. Solid Edge help was read on the soliddna.com mirror (one
page fetched) and through Siemens blogs (fetched); one white-paper PDF returned 404. CATIA V5 and
3DEXPERIENCE help is not public; CATIA facts are training outlines (one fetched) and search
summaries. Autodesk Inventor and Fusion help pages on help.autodesk.com fetched, except one
guessed Inventor URL (404). Onshape help (cad.onshape.com/help) and PTC Creo help
(support.ptc.com) fetched. BricsCAD help (help.bricsys.com), ZW3D (zwsoft.com) and Plasticity
(doc.plasticity.xyz) fetched. wiki.freecad.org and forum.freecad.org were not tried (known
bot-protection pages); FreeCAD facts come from the GitHub search API (until it hit its
unauthenticated rate limit), github.com pages fetched directly, and raw.githubusercontent.com.

---------------------------------------------------------------------------

## A1. Summary tables per CAD program

### A1a. Modeling features

| Program | Base features | Edge / miter flange | Hem | Jog | Sketched bend | Corners and reliefs | Forming tools | Convert solid / import | Rip |
|---|---|---|---|---|---|---|---|---|---|
| SOLIDWORKS | Base Flange/Tab, Lofted Bend, Swept Flange [1][2] | Edge Flange, Miter Flange [1] | Hem [1] | Jog [1] | Sketched Bend, Cross-Break [1][5] | Closed Corner, Welded Corner, Corner Relief (Rectangular, Circular, Tear, Obround, Constant Width, Full Round, Suitcase), Break-Corner/Corner-Trim, auto bend relief (Rectangular, Tear, Obround) [2][4] | Forming Tool (louvers, lances, ribs from die parts), Sheet Metal Gusset, Vent (gk) [2][5] | Convert to Sheet Metal, Insert Bends [2] | Rip [2] |
| Autodesk Inventor | Face, Contour Flange, Contour Roll, Lofted Flange [8] | Flange, Contour Flange on edges; Corner Seam (manual or automatic) [8] | Hem [8] | ~ via Contour Flange / Fold (gk) | Fold (about a sketched line), Bend (joins faces) [8] | Corner Seam, Corner Round, Corner Chamfer; bend relief Straight / Round / Tear; corner relief Round / Square / Trim to Bend / Linear weld / Arc weld / Laser weld (gk for the weld names in Inventor; [11]) [8][11] | Punch Tool (iFeature library) [8][9] | Convert to Sheet Metal (pick base face, thickness measured) [13] | Rip [8] |
| Autodesk Fusion | Flange (base, edge, contour, lofted chosen by input), Convert to Sheet Metal [14][16][19] | Flange; Corner Closure (miter two flanges) [14] | Hem: flat, open, rolled, rope, teardrop, double [14] | - (gk) | Fold (explicit bend line) [14] | Bend relief Round / Straight / Tear; 2-bend corner Round, Square, Tear, Trim to bend, Linear weld, Arc weld, Laser weld; 3-bend corner No replacement / Intersection / Full round / Round with radius [15] | none: no punch or form tool (forum) [18] | Convert to Sheet Metal (one way) [16] | Rip [14] |
| Onshape | Sheet Metal Model: Extrude, Thicken, Convert; Loft [20] | Flange; Make Joint / Modify Joint (bend or rip) [20][21] | Hem [21] | Jog (S/Z offset, two opposite bends) [25] | Bend (along a reference line) [21] | Corner (Square-Sized, Rectangle-Scaled, Round-Sized, Round-Scaled, Closed, Simple); Bend Relief (Square-Sized, Rectangle-Scaled, Obround-Scaled, Obround-Sized, Tear); Corner Break (fillet/chamfer) [21][22] | Form (library parts: standard, company, personal) [23] | Sheet Metal Model "Convert" encloses existing parts [20] | Make Joint as rip; table converts rips to bends and back [21][24] |
| PTC Creo | Planar, Extrude, Revolve, Blend, Rotational Blend, Swept Blend, Sweep, Helical Sweep, Boundary Blend, Twist walls [27] | Flat, Flange (attached), Extend, Merge, Join [27] | Flange shapes Open, Flushed, Joggle, Duck, C, Z (hem family) plus I, Arc, S [30] | Joggle as a flange shape [30] | Bend, Planar Bend, Edge Bend [27] | Corner Relief (No relief, V Notch, Normal, Circular, Square, Rectangular, Obround); bend relief No relief, Rip, Stretch, Rectangular, Obround [31] | Punch Form, Die Form, Sketched Form, Quilt Form, Flatten Form; Punch and Notch templates [27][32] | Conversion feature [27] | Edge Rip, Surface Rip, Sketched Rip, Rip Connect [27] |
| Siemens NX | Tab, Contour Flange, Lofted Flange [34][37] | Flange, Contour Flange along edges; Advanced Flange on non-linear edges [37][38] | Hem Flange [37] | Jog; Joggle (advanced) [34][38] | Bend [37] | Closed Corner, Three-Bend Corner, Break Corner, Bend Taper, Edge Rip [35][37] | Dimple, Louver, Drawn Cutout, Bead, Gusset, Solid Punch, Lightening Cutout (advanced) [34][37][38] | Sheet Metal from Solid, Convert to Sheet Metal [35][37] | Edge Rip, Rip Corner (gk name) [37] |
| Solid Edge | Tab, Contour Flange, Lofted Flange [40][44] | Flange (partial, material side, measure point) [39] | Hem (search summary) [44] | Jog [44] | Bend (gk) | Close 2-Bend Corner (close / overlap, gap), Close 3-Bend Corner (ordered only); corner relief Bend only / Bend and face / Bend and face chain; bend relief Square / Round [39][40][41] | Louver, Drawn Cutout, Dimple, Bead, Gusset, Emboss, Etch, Cross Brake [41][44] | Thin Part to Synchronous sheet metal; Rip Corner for converted parts [39][43] | Rip Corner [39] |
| CATIA GSM (V5 / 3DX) | Wall, Extrusion, Rolled Wall, Hopper (lofted) [45][46] | Wall on Edge, Swept walls: Flange, Swept Flange / User Flange [46] | Hem, Tear Drop [46] | ? | Bend, Conical Bend, Bend From Flat [46] | Corner Relief, Corner, Chamfer [46] | Stamps: Point/Surface/Curve stamp, Bead, Flanged cutout, Louver, Bridge, Flanged hole, Circular stamp, Stiffening rib, Dowel, User stamp [46] | Recognize (convert a solid to GSM) [46] | ? (gk: done by Recognize) |
| BricsCAD Mechanical | SMFLANGEBASE, SMEXTRUDE, SMLOFT [49] | SMFLANGEEDGE, SMFLANGEBEND; Miter feature [49] | ~ (Form / bead library, gk) | - (gk) | SMBENDCREATE [49] | Bend relief, Corner relief (3+ flanges), Junction / T-junction, Bevel [49] | Form and Bead features (BMINSERT, SMBEAD) [49] | SMCONVERT (recognizes any solid) [49] | Junction (SMJUNCTIONCREATE) [49] |
| ZW3D | Extrude, Flange with Profile, Twist Flange [52] | Flange, Full Flange, Joint Flange, Merge Flange [52] | Hem Flange [52] | Jog (search summary) [52] | Fold by Line [52] | ? | Louver, Dimple, Slot [52] | Convert to Sheet Metal [52] | ? |
| Plasticity | none: no sheet metal command in the manual [53] | - | - | - | - | - | - | - | - |
| FreeCAD upstream core | none: no sheet metal module in src/Mod; the SheetMetal addon (Python) is external [54][60] | (addon) | (addon) | (addon) | (addon) | (addon) | (addon) | (addon) | (addon) |

### A1b. Unfolding, flat pattern and manufacturing

| Program | Unfold / refold | Flat pattern | Flat export | Multi-body | Cross break | Gusset / rib | Welded corner |
|---|---|---|---|---|---|---|---|
| SOLIDWORKS | Unfold / Fold features, No Bends, Flatten toggle [2] | Flat-Pattern feature in the tree, live [6] | DXF/DWG straight from the part (right-click Flat-Pattern); bend lines, forming tools, up/down bends on separate layers; one file per body or one for all [6] | yes; per-body parameters can override the parent [6][7] | yes [5] | Sheet Metal Gusset [2] | Welded Corner [2] |
| Inventor | Unfold / Refold feature pairs [8] | Flat Pattern browser node, separate state, live; features can be added in the flat [12] | DWG/DXF/SAT with layer control; punch shown as formed, 2D sketch or centre mark; bend order annotation into a drawing bend table [12] | yes (top-down, Make Components) [13] | - (gk) | - (gk) | weld corner relief shapes (gk; [11]) |
| Fusion | Unfold / Refold group in the timeline [19] | Create Flat Pattern; K-factor and thickness editable there [19] | DXF: outer profile, interior profiles, bend centre lines, bend extent lines, text on separate layers; bend lines always included; splines to polylines [17] | yes; Join by Bend joins two bodies [14] | - | - | Linear / Arc / Laser weld corner reliefs [15] |
| Onshape | none as a feature; flat view is always live (gk) | Flat view panel beside the folded model and a Rip/Bend table, all updated live [20][24] | DXF/DWG of flat pattern: bend centre lines, bend tangent lines, form outlines and centre marks optional [23][24][26] | yes (several parts per model) (gk) | - | - | - |
| Creo | Unbend, Transitional Unbend, Cross Section Driven Unbend, Bend Back [27] | Flat Pattern feature; Flat Pattern Preview window; Flat Pattern Manager for many bodies (Creo 11) with Flat / Initial / Removed state per body; legacy flat-state family instances [27][28][32] | via drawing to DXF (community) [32] | yes (Creo 9 multibody, Creo 11 flat patterns per body) [28][33] | - | - | ? |
| NX | Unbend / Rebend; Form-Unbend-Rebend [37] | Flat Pattern (associative, shown split-screen beside 3D) and Flat Solid feature [34][37] | Export Flat Pattern (DXF), PMI on flat pattern [34][36] | yes (Unite, Mirror bodies) [34] | - | Gusset (not shown in flat) [37] | ? |
| Solid Edge | Unbend / Rebend for features across bends [41] | Flat Pattern in the part; flatten of dimples on bends [41][42] | Save As Flat to DXF / PAR / PSM; bend table on the draft [42] | yes (design bodies, gk) [43] | Cross Brake [41] | Gusset (not shown in flat) [41] | ~ Close 2-Bend Corner with gap [39] |
| CATIA GSM | Fold/Unfold toggle; Point/Curve Mapping (gk) | Unfolded view in drawing with bend axes and bend limits [48] | Save As DXF [48] | yes ("multi-body design" lesson) [45] | - | Stiffening rib stamp [46] | - |
| BricsCAD | SMUNFOLD makes a flat solid [51] | separate unfolded solid [51] | SMEXPORT2D to DXF/DWG; SMASSEMBLYEXPORT [51] | yes (assembly export) [51] | - | - | - |
| ZW3D | Unfold / Fold, Flatten [52] | Flatten [52] | ? | Joint / Merge Flange combine bodies [52] | - | - | - |

---------------------------------------------------------------------------

## A2. Bend allowance models

### A2.1 The formulas the programs use

All of them put the flat length of a bend on a neutral fibre inside the sheet. The common form:

- Bend allowance (arc length of the neutral fibre): `BA = pi * (R + K*T) * A / 180`, with R the
  inside radius, T the thickness, K the K-factor (neutral fibre offset from the inside face over
  T), A the bend angle in deg. SOLIDWORKS states it this way [3]; Solid Edge names it PZL
  (plastic zone length) with NF (neutral factor) in place of K [42]; BricsCAD `BA = angle * (R + kT)`
  in radians [50]; CATIA `W = pi * (R + k*T)` per unit angle [47].
- Creo's Y-factor: `L = (pi/2 * R + Y*T) * A/90`, with `Y = K * pi/2`; default Y 0.50 [29].
  Creo is the only program found exposing a Y-factor.
- Bend deduction: flat length = sum of outside mold-line lengths minus BD per bend. Offered as a
  method in SOLIDWORKS, Inventor (custom equation), Onshape, NX, BricsCAD (tangential bend
  deduction table) [3][10][20][36][50].
- K by formula from R/T:
  - CATIA's default K follows DIN: `K = (0.65 + log(R/T)/2) / 2`, giving 0..0.5 [47].
  - BricsCAD interpolates K from R/T: 0 to 0.27324 for R/T < 1, 0.27324 to 0.5 for 1 < R/T < 4,
    0.5 above [50].
  - NX offers a DIN6935 formula as a bend definition method [36].
  - The FreeCAD SheetMetal addon distinguishes "K-factor (ANSI)" and "K-factor (DIN)" (the DIN
    factor is measured on half the thickness, gk) [60].

### A2.2 Per program: methods offered and where they are set

| Program | Methods | Where set |
|---|---|---|
| SOLIDWORKS | K-Factor, Bend Allowance, Bend Deduction, Bend Calculation (equation table), bend table (txt or Excel, interpolated in R, T, angle) [3] | Sheet-Metal feature per body; gauge table per material (thickness, default radius, K per gauge); per feature "Override default parameters"; per bend sub-feature "Custom Bend Allowance" [3][7] |
| Inventor | Linear (K-factor uniform over angles), Bend Table (empirical by angle, backup K), Custom Equation (solves bend compensation, bend deduction, bend allowance or K, with bounding conditions); Spline Factor for non-cylindrical bends [10] | Sheet Metal Rule (style library) -> Unfold Rule; Sheet Metal Defaults per part; per feature "Unfold Options" tab overrides the rule [11][13] |
| Fusion | K-factor (linear unfold) only in the rule [15] | Sheet metal rule (library and per document); thickness and K editable on the flat pattern [15][19] |
| Onshape | K-factor (default 0.45; 0.5 for rolled walls), Bend allowance, Bend deduction [20] | Sheet Metal Model feature; per bend in the sheet metal table (radius, K / BA / BD per cell) [20][24] |
| Creo | Y-factor or K-factor equation; bend tables (three Machinery's Handbook 90 deg tables shipped); developed length overridable [29] | Part parameter SMT_PART_BEND_ALLOWANCE_FACTOR; material drives Y, K and bend table; per wall a feature-specific method; per bend an overridden developed length [29] |
| NX | Neutral Factor Value, Bend Table, Bend Allowance Formula, Material Table, Tool Table, Bend Allowance Table, Bend Deduction Table, Bend Deduction Formula, DIN6935 Formula [36] | Sheet Metal Preferences (part); material standard tables; neutral factor per flange [36] |
| Solid Edge | Neutral factor in the standard PZL formula; custom bend equation (Machinery's Handbook formula as an example); bend table [42] | Material Table, Gage tab (thickness from an external gage spreadsheet, default radius, relief depth and width, neutral factor) [40][42] |
| CATIA GSM | K-factor, default by the DIN formula, editable; bend allowance value [47] | Sheet Metal Parameters dialog (Parameters, Bend Extremities, Bend Allowance tabs); standards files (gk) [47] |
| BricsCAD | K-factor (R/T interpolation by default), bend table of tangential bend deductions [50][51] | Root node of the part in the Mechanical Browser; SMDEFAULTKFACTOR [50] |
| ZW3D | K-factor [52] | Global sheet metal attributes of the part (thickness, default radius, K) [52] |
| FreeCAD SheetMetal addon | K-factor (ANSI or DIN); Material Definition Sheet: a spreadsheet or CSV mapping R/T to K, per bend lookup [60] | Unfold task panel; MDS spreadsheet in the document or a CSV library folder; "Engineering mode" forces the user to enter K [60] |

Rules a flat pattern follows (analysis, from [3][29][42][47][50]):
- Flat faces keep their size; each cylindrical bend becomes a strip of width BA, measured on the
  neutral fibre at R + K*T.
- K (or BA, BD) is chosen per bend from the most specific source: per-bend override, then
  feature override, then a table keyed on (material, T, R, angle), then a part default.
- Non-cylindrical bends (lofted flanges, contour rolls, conical bends) need their own rule:
  Inventor's Spline Factor is the only named one found [10].
- Forming features are either flattened (shown as outline or centre mark) or kept formed in the
  flat; gussets, louvers and drawn cutouts are not flattened in Solid Edge and NX [12][23][37][41].

---------------------------------------------------------------------------

## A3. Sheet metal rules / styles

| Program | Rule object | Contents |
|---|---|---|
| SOLIDWORKS | Sheet-Metal feature (one per body) + gauge tables [3][7] | thickness, bend radius, bend allowance type and value, auto relief type and ratio; gauge table per material lists gauges with thickness, radius, K [3] |
| Inventor | Sheet Metal Rule style (Style and Standard Editor) + Unfold Rule [10][11] | material, thickness, flat pattern options (punch representation, bend angle reporting), bend (radius, relief shape, width, depth, minimum remnant, transition), corner (relief shape, size, 3-bend relief), unfold rule [11][12] |
| Fusion | Sheet metal rule (Sheet Metal Rules Library) [15] | thickness, material label, units, minimum gap (miter, rip, seam), bend radius, K-factor, unfold method, single-bend relief (Round, Straight, Tear; width, depth, remnant), 2-bend corner relief (7 shapes, size, placement), 3-bend corner (4 options, radius) [15] |
| Onshape | Sheet Metal Model feature parameters [20] | thickness, bend radius, K / BA / BD, minimal gap, corner relief type, bend relief type, clearance from input; no gauge table found (gk) |
| Creo | Sheetmetal parameters + material file + bend tables [29] | SMT_* parameters for defaults of every tool; material assigns Y, K, bend table; relief defaults on a Relief tab [29][31] |
| NX | Sheet Metal Preferences + material standards table [36] | thickness, bend radius, relief depth/width, neutral factor or bend definition method, material table columns (search summary only) [36] |
| Solid Edge | Material Table, Gage tab; gage table spreadsheet [40][42] | thickness, bend radius, relief depth and width, neutral factor, bend equation [40][42] |
| CATIA GSM | Sheet Metal Parameters [47] | thickness, default bend radius, bend extremities (relief type), K-factor or bend allowance [47] |
| BricsCAD | Part root parameters [50] | thickness, K-factor or bend table [50][51] |

Pattern (analysis): every program has one part-level rule object holding thickness, radius, K
(or method), gap and relief defaults; the mature ones (SOLIDWORKS, Inventor, Creo, NX, Solid
Edge) tie it to a material and a gauge table, and let a feature or a single bend override it.

---------------------------------------------------------------------------

## A4. Flat pattern: live state, display, manufacturing outputs

- SOLIDWORKS: Flat-Pattern is a feature at the end of the tree, kept suppressed in the folded
  state and unsuppressed to flatten (gk for the suppression mechanics); DXF/DWG export straight
  from that feature with geometry, bend lines, forming tools, sketches; up and down bend lines
  can map to separate layers; a multibody part goes to one file or one per body; bend lines not
  shown in the drawing template are not exported [6].
- Inventor: a Flat Pattern browser node; the model shows flattened when it is active. The Edit
  Flat Pattern Definition dialog sets orientation, punch representation (formed, 2D sketch,
  centre mark only) and bend angle reporting. Bend order is defined in the flat state and
  extracted to a bend table annotation on drawings. Export to SAT, DWG, DXF with layer control
  [12].
- Fusion: Create Flat Pattern; DXF export puts outer profile, interior profiles, bend centre
  lines, bend extent lines and text on separate layers; bend lines cannot be excluded; optional
  spline-to-polyline conversion with tolerance [17][19].
- Onshape: the flat view is not a feature but a live panel next to the folded model, with a
  table of bends and rips that is editable (radius, K/BA/BD, rip type) and cross-highlights;
  DXF/DWG export with optional bend centre lines and tangent lines, form outlines and centre
  marks [20][23][24][26]. Drawings take a flat pattern view with form annotations [23].
- Creo: Flat Pattern feature for one body, Flat Pattern Manager for all bodies (Flat / Initial
  / Removed per body, Creo 11), Flat Pattern Preview window, Create Instance (family-table flat
  instance), Create Representation, Bend Order table, bend notes and corner relief notes on the
  graphics toolbar [27][28][32]. DXF goes through a drawing [32].
- NX: Flat Pattern is associative and shown split-screen with the 3D; Flat Solid adds a flat body
  feature; Export Flat Pattern writes DXF; PMI on drawings [34][36][37].
- Solid Edge: Save As Flat writes DXF or a flat PAR/PSM; bend tables are placed on the draft of
  a flat pattern drawing for fabrication sequences [42].
- CATIA: the part toggles between folded and unfolded (Fold/Unfold); Unfolded View in Drafting
  shows bend axes and dashed bend limits plus hole centre lines; Save As DXF [48].
- BricsCAD: SMUNFOLD creates a solid of the flat sheet; SMEXPORT2D writes the unfolded profile to
  DXF/DWG; SMASSEMBLYEXPORT handles assemblies [51].

Summary (analysis): two display models exist. (1) A flat state of the same part, live and
associative (SOLIDWORKS, Inventor, Fusion, Creo, NX, Solid Edge, CATIA). (2) A permanent side
view kept in sync (Onshape) or a derived flat solid (BricsCAD, NX Flat Solid). DXF export with
bend lines on their own layer is universal among the mechanical CADs; bend order / bend sequence
is a second-tier output (Inventor, Creo, Solid Edge drafts).

---------------------------------------------------------------------------

## A5. Upstream FreeCAD

- No sheet metal module in core. Upstream `src/Mod` could not be listed through the API (rate
  limit), but every sheet-metal issue found through the search API is labelled or redirected as
  the third-party SheetMetal addon (e.g. #30107 "3rd party: Addon/Macro", SheetMetal 0.8.11)
  [55][58]. This fork's `src/Mod` has no sheet metal module either [61].
- PR #31913 "SheetMetal: Move the external workbench in main" (PaddleStroke, opened 2026-08-12,
  969 commits of the addon history) was CLOSED UNMERGED on 2026-09-21 [54]. In the thread:
  - chennes (08-12): the need could be met on the packaging side by shipping addons in releases.
  - shaise, the addon author (08-13): prefers a separate workbench included automatically.
  - pierreporte (08-14): "the workbench is not mature enough to be in main"; (08-26) build a
    new one from scratch with a correct representation.
  - kadet1090 (08-17): the right path is an FEP (FreeCAD Enhancement Proposal).
  - shaise (08-26): "sheetmetal works on 'solids' not sheets ... the system takes the solid,
    'reverse engineer' it to guess the sheet it represents, then perform the operation"; a
    proper workbench should start from a sheet representation, the way Assembly was built.
  No FEP on sheet metal was found in FreeCAD/FreeCAD-Enhancement-Proposals (search API, 0 hits)
  [55].
- Ondsel's "SheetMetal strategy" post (Aleksandr Prokudin and Joe Sardos; dated 2025-04-04 by
  the fetch) planned: UI/UX revamp and translation, the new materials system, an unfolder
  rewritten in C++ for FreeCAD core, then SheetMetal rewritten on that core unfolder. It notes no
  parametric link between folded and unfolded shapes and a "complex and computationally
  expensive" Python unfolder [59]. No sign it was carried out in core [55] (analysis).
- Issue #12638 (FreeCAD Day 2024, critical functionality only in addons) named SheetMetal and
  Fasteners as integration candidates; closed 2026-04-25 as non-actionable [56].
- Issue #13361 asks for a PartDesign tool to bend existing parts; commenters point to the addon's
  AddFoldWall as a starting point [57].
- Issue #18251 (open) asks TechDraw to treat sheet metal bend edges as smooth [55].
- The addon (shaise/FreeCAD_SheetMetal, v0.8.26 on 2026-10-15) is Python; recent releases added
  weld-type corner relief (0.8.23), folding marks (0.8.22), external Material Definition Sheets
  (0.8.25), an unfolder speedup (0.8.24) and a wave-cut unfold option (0.8.26) [60]. Its full
  inventory belongs in Part B.
- FreeCAD 1.1 (released 2026-03-24) has no sheet metal item in the summaries seen (search
  summary) [59].

---------------------------------------------------------------------------

## A6. Per-program notes

### A6.1 SOLIDWORKS
- Toolbar: Base-Flange/Tab, Forming Tool, Convert to Sheet Metal, Unfold, Lofted-Bend, Fold,
  Edge Flange, Flatten, Miter Flange, No Bends, Hem, Rip, Jog, Insert Bends, Sketched Bend,
  Swept Flange, Cross-Break, Sheet Metal Gusset, Closed Corner, Corner Relief, Welded Corner,
  Tab [2]. Each of miter flange, edge flange, sketched bend, hem and jog asks for a bend
  position (material inside / outside / bend outside / bend from virtual sharp) [1].
- Forming tools are parts that act as dies (louvers, lances, flanges, ribs) [5]. Cross-Break is
  graphical (HVAC stiffening), flattens, and edge/miter/swept flanges can sit on its face [5].
- Bend tables (txt or Excel) carry bend deduction, bend allowance or K by R, T, angle, and are
  interpolated [3].
- Two old base modes: Insert Bends (legacy, rips and bends on an imported shell) and Convert to
  Sheet Metal (pick fixed face and bend edges) (gk for the distinction).

### A6.2 Autodesk Inventor
- Features: Face, Contour Flange, Contour Roll, Lofted Flange, Flange, Fold, Bend, Hem, Corner
  Seam, Corner Round, Corner Chamfer, Cut (across bends), Punch Tool, Rip, Unfold/Refold, Pattern
  [8]. 2026: property panels on sheet metal commands, Contour Flange as robust first feature,
  live punch preview [9].
- Unfold rules are a separate style from the sheet metal rule; Custom Equation is the most
  general model found anywhere (any of BC, BD, BA, K, with bounds and a backup K) [10].
- The flat pattern is where the CAM side lives: punch representation, bend angle reporting,
  bend order, layered DWG/DXF [12].

### A6.3 Autodesk Fusion
- One Flange tool decides base / edge / contour / lofted from what is selected [19][14].
- Corner treatments live in the rule, including three weld styles [15]. Join by Bend, sheet
  metal fillets/chamfers and overlap corners are excluded from the free licence [14].
- No forming tools; users model them and keep only the 2D outline in the flat (forum) [18].

### A6.4 Onshape
- Workflow is different: a Sheet Metal Model feature opens an "active" model whose joints are
  edited (Make Joint, Modify Joint), and the flat view and table update live; Finish Sheet Metal
  Model closes it [20][21].
- Convert encloses existing solids with sheet, with a clearance, choosing edges to become bends;
  this is the main route from a solid design [20].
- Form library documents must carry a `thickness` configuration variable [23].

### A6.5 PTC Creo
- The widest wall set (sweep, helical sweep, boundary blend, twist), because Creo builds sheet
  metal walls from its general shape tools [27].
- Four rip types and three unbend types (regular, transitional, cross-section driven) [27].
- Material-driven factors; Y-factor; Machinery's Handbook bend tables [29].
- Multibody sheet metal since Creo 9, flat patterns per body since Creo 11 [28][33].

### A6.6 Siemens NX
- Core: Tab, Flange, Contour Flange, Lofted Flange, Hem Flange, Jog, Bend, Closed Corner,
  Three-Bend Corner, Break Corner, Bend Taper, Dimple, Louver, Drawn Cutout, Bead, Gusset, Normal
  Cutout, Solid Punch, Edge Rip, Unbend/Rebend, Flat Pattern, Flat Solid, Sheet Metal from Solid,
  Convert to Sheet Metal [37].
- Advanced Sheet Metal (aerospace, merged in NX 11): Advanced Flange along non-linear edges and
  surfaces, Joggle, Lightening Cutout, flat pattern compensation, One-step formability analysis
  [38].
- The richest bend definition list: nine methods including DIN6935 and tool tables [36].

### A6.7 Solid Edge (ordered and synchronous)
- Both modes share the commands; synchronous edits flanges by moving faces with the steering
  wheel, governed by Live Rules plus a sheet-metal-only rule "Maintain Thickness Chain" [40][43].
- Thin Part to Synchronous sheet metal recognizes flanges of an imported thin part [43].
- Deformation features can be added across bends (unbend, add, rebend) and dimples on bends
  flatten [41]. Close 3-Bend Corner is ordered-only [41].
- Corner relief model is distinctive: "Bend only", "Bend and face", "Bend and face chain" [39].

### A6.8 CATIA V5 Generative Sheetmetal Design / 3DEXPERIENCE Sheet Metal Design
- Walls (profile, extruded, rolled, hopper), walls on edge, swept walls (flange, hem, tear drop,
  user flange), bends (bend, conical, from flat), corner relief, stamps (the largest stamp
  catalogue found, ten kinds plus user stamps), Recognize for imported solids [45][46].
- Course outline adds mapping (point/curve mapping onto the unfolded part), DXF output, imported
  sheet metal parameters and multi-body design [45].
- An Aerospace Sheetmetal Design product exists for hydroformed / flanged aerospace parts (search
  summary, title only) [38].

### A6.9 BricsCAD Mechanical, ZW3D, Plasticity
- BricsCAD recognizes sheet metal in any solid (SMCONVERT) and keeps bends and junctions as
  separate features; reliefs, tabs, beads, forms; K by R/T or a bend table [49][50][51].
  This is the closest existing analogue to the FreeCAD addon's "reverse engineer the solid"
  approach (analysis).
- ZW3D: flanges (full, profile, twist, joint, merge), hem, jog, fold by line, louver, dimple,
  slot, unfold/fold/flatten, convert to sheet metal, part-level K [52].
- Plasticity has no sheet metal tools [53].

---------------------------------------------------------------------------

## A7. Union tool catalogue and synthesis

### A7a. Tool catalogue

"y" = dedicated tool; "~" = through another tool or partly; "-" = not found; "?" = no source and
no confident gk.

| Tool | SW | Inv | Fus | Onsh | Creo | NX | SE | CATIA | Brics | ZW3D | y |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Base flange / tab from closed sketch | y | y | y | y | y | y | y | y | y | y | 10 |
| Contour (open profile) flange | y | y | y | y | y | y | y | y | y | y | 10 |
| Edge flange | y | y | y | y | y | y | y | y | y | y | 10 |
| Lofted flange / hopper | y | y | y | y | y | y | y | y | y | ? | 9 |
| Rolled / revolved / twisted wall | ~ | y | ~ | ~ | y | ? | ? | y | ? | y | 4 |
| Miter flange (chain of edges) | y | ~ | ~ | ~ | ~ | ~ | ~ | ~ | y | ? | 2 |
| Hem (several shapes) | y | y | y | y | y | y | y | y | ~ | y | 9 |
| Jog / joggle | y | ~ | - | y | y | y | y | ? | - | y | 6 |
| Sketched bend / fold on line | y | y | y | y | y | y | y | y | y | y | 10 |
| Closed corner / corner closure | y | y | y | y | ? | y | y | y | ~ | ? | 7 |
| Corner relief (several shapes) | y | y | y | y | y | y | y | y | y | ? | 9 |
| Auto bend relief (rect / round / obround / tear) | y | y | y | y | y | y | y | y | y | ? | 9 |
| Welded corner / weld relief | y | ? | y | - | - | - | ~ | - | - | - | 2 |
| Break corner (fillet / chamfer of flat corners) | y | y | y | y | ~ | y | ? | y | y | ~ | 8 |
| Forming tool library (punch / die parts) | y | y | - | y | y | y | ~ | y | y | ~ | 7 |
| Built-in forms (louver, dimple, bead, drawn cut) | ~ | ~ | - | ~ | ~ | y | y | y | y | y | 5 |
| Gusset / stiffening rib | y | - | - | - | ? | y | y | y | - | - | 4 |
| Cross break | y | - | - | - | - | - | y | - | - | - | 2 |
| Convert solid / recognize imported part | y | y | y | y | y | y | y | y | y | y | 10 |
| Rip | y | y | y | y | y | y | y | ? | y | ? | 8 |
| Unfold / refold features | y | y | y | - | y | y | y | ~ | ~ | y | 8 |
| Live flat pattern | y | y | y | y | y | y | y | y | ~ | y | 9 |
| Flat DXF/DWG with bend lines | y | y | y | y | ~ | y | y | y | y | ? | 8 |
| Bend order / bend table annotation | ~ | y | ? | ? | y | ? | y | ? | - | ? | 3 |
| Bend table / gauge table by material | y | y | - | - | y | y | y | ~ | y | - | 6 |
| Per-bend override of K / BA | y | y | ~ | y | y | y | ? | ? | - | - | 5 |
| Multi-body sheet metal | y | y | y | y | y | y | y | y | y | y | 10 |
| Formability / one-step analysis | - | - | - | - | - | y | - | ? | - | - | 1 |

Several cells rest on (gk) or on a feature's absence from a list that may not be complete; the
NX, CATIA, ZW3D and BricsCAD columns lean on outlines and search summaries the most.

### A7b. Synthesis (analysis)

Core set, in essentially every program:
- One rule object per part (thickness, radius, K, gap, relief defaults), with a K-factor bend
  allowance on a neutral fibre.
- Base flange from a closed sketch, contour flange from an open sketch, edge flange on one or
  more edges, sketched bend / fold on a line, lofted flange.
- Hem; auto bend relief; corner relief and corner closure where two flanges meet.
- Convert a solid or imported thin part to sheet metal, with rips.
- A live, associative flat pattern and DXF/DWG export of it with bend lines on their own layer.
- Multi-body parts.

Second tier (most of the mature desktop CADs, missing in one or two cloud tools):
- Jog; miter flange along a chain of edges; break corner.
- Forming tool libraries (punch / die parts) and their flat-pattern representation (outline or
  centre mark).
- Unfold / refold to place cuts across bends.
- Bend tables and gauge tables tied to material; bend allowance and bend deduction methods
  beside K; per-feature and per-bend overrides.
- Built-in parametric forms (louver, dimple, bead, drawn cutout).

Rare or premium:
- Cross break (SOLIDWORKS, Solid Edge), gusset (SOLIDWORKS, NX, Solid Edge, CATIA rib stamp),
  welded corner (SOLIDWORKS, Fusion weld reliefs).
- Bend order / bend sequence tables (Inventor, Creo, Solid Edge drafts).
- Equation-driven bend models (Inventor Custom Equation, NX formulas and DIN6935, CATIA DIN K),
  Creo's Y-factor, Inventor's spline factor for non-cylindrical bends.
- Non-linear-edge flanges and joggles for aerospace (NX Advanced Sheet Metal, CATIA Aerospace),
  one-step formability.
- Synchronous / direct editing of flanges (Solid Edge; Onshape's joint editing is a lighter form).

Two architectural styles are visible: (1) feature-based sheet metal where every feature knows
its bends (SOLIDWORKS, Inventor, Creo, NX, Solid Edge, CATIA, Fusion), and (2) recognition from
a solid (BricsCAD SMCONVERT, Onshape Convert, Solid Edge Thin Part, and the FreeCAD addon,
whose author called this its main limitation in PR #31913 [54]). Every feature-based program
still offers recognition as an import path, so a native design needs both (analysis).

---------------------------------------------------------------------------

## A8. Sources

1. https://help.solidworks.com/2020/English/SolidWorks/sldworks/c_Using_Sheet_Metal_Tools.htm, https://help.solidworks.com/2012/English/SolidWorks/Sldworks/Bend_Positions.htm (search summary)
2. https://help.solidworks.com/2022/English/SolidWorks/sldworks/r_Sheet_Metal_Toolbar.htm (search summary: toolbar command list)
3. https://help.solidworks.com/2017/English/SolidWorks/sldworks/c_sheet_metal_gauge_bend_table.htm, https://help.solidworks.com/2018/English/SolidWorks/sldworks/c_Sheet_Metal_Gauge_Tables.htm, https://help.solidworks.com/2017/English/SolidWorks/sldworks/c_K_Factor.htm, https://help.solidworks.com/2017/English/SolidWorks/sldworks/c_Bend_Table_Overview.htm, https://www.javelin-tech.com/blog/2017/06/solidworks-sheet-metal-bend-table/ (search summary)
4. https://help.solidworks.com/2025/English/SolidWorks/Sldworks/HIDD_SHEET_METAL_CORNER_RELIEF.htm, https://help.solidworks.com/2017/English/SolidWorks/sldworks/c_Auto_Reliefs.htm (search summary)
5. https://help.solidworks.com/2023/english/SolidWorks/sldworks/c_forming_tools.htm, https://help.solidworks.com/2017/English/SolidWorks/sldworks/c_cross_break.htm, https://help.solidworks.com/2017/English/SolidWorks/sldworks/t_Example_of_Creating_a_Louver.xls.htm, Packt "Mastering SOLIDWORKS Sheet Metal" ch. 6 (search summary)
6. https://help.solidworks.com/2019/english/SolidWorks/sldworks/c_exporting_to_dxf_or_dwg_files.htm, https://help.solidworks.com/2020/English/SolidWorks/sldworks/t_Saving_Sheet_Metal_Flat_Patterns_DXF_Files.htm, https://www.goengineer.com/en-us/blog/missing-solidworks-bend-lines-in-flat-pattern-dxf (search summary)
7. https://help.solidworks.com/2019/English/SolidWorks/Sldworks/t_overriding_parent_sheet_metal_params.htm, https://help.solidworks.com/2017/English/SolidWorks/sldworks/t_Edit_Bends.htm, https://www.cati.com/?p=120860 (search summary)
8. https://help.autodesk.com/cloudhelp/2015/ENU/Inventor-Help/files/GUID-78F9188C-1B19-4DD2-9FC2-27B8A4B06E0D.htm (fetched: feature list)
9. https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-WhatsNew/files/INV2026-SHEET-METAL-ENHANCEMENTS.htm (fetched)
10. https://help.autodesk.com/cloudhelp/2025/ENU/Inventor-Help/files/GUID-4141CA32-0DCE-4806-AE73-EF9CD97B0206.htm (fetched: unfold rule types)
11. https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-Help/files/GUID-D1EBE010-86D3-49CC-88B3-522F9A0CCA3B.htm, https://help.autodesk.com/cloudhelp/2015/ENU/Inventor-Help/files/GUID-5030984D-D3FB-46E6-AAE0-72EDD8AB3913.htm, https://help.autodesk.com/cloudhelp/2025/ENU/Inventor-Help/files/GUID-3439BC10-23B6-4CFC-8C5B-0DE02416EB6A.htm (search summary: rules, defaults, bend tab, relief shapes)
12. https://help.autodesk.com/cloudhelp/2015/ENU/Inventor-Help/files/GUID-F89A8FD9-DA80-4191-8637-8AA82833A606.htm, https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-Help/files/GUID-CA143C9B-B8DF-4340-9070-3C07F972A6EA.htm, https://help.autodesk.com/cloudhelp/2014/ENU/InventorLT/files/GUID-0E1421BF-C9ED-4912-90E9-30EF5F1C5B55.htm (search summary: flat pattern, punch representation, bend order, export)
13. https://help.autodesk.com/cloudhelp/2016/ENU/Inventor-WhatsNew/files/GUID-B0691F9D-46AF-40CB-93D5-977658927A83.htm, https://help.autodesk.com/cloudhelp/2019/ENU/Inventor-WhatsNew/files/GUID-B0691F9D-46AF-40CB-93D5-977658927A83.htm (search summary: multibody, convert, unfold options override)
14. https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/SheetMetalFeatures_UM.htm (fetched)
15. https://help.autodesk.com/cloudhelp/ENU/Fusion-Sheet-Metal/files/SM-RULES-REF.htm (fetched)
16. https://help.autodesk.com/cloudhelp/ENU/Fusion-Sheet-Metal/files/SM-TO-CONVERT-TO-SM.htm (fetched)
17. https://help.autodesk.com/cloudhelp/ENU/Fusion-Sheet-Metal/files/GUID-9C211F95-7739-460F-BCA4-100AF6A444A2.htm, https://sendcutsend.com/blog/how-to-export-a-dxf-in-fusion-360/ (search summary)
18. https://forums.autodesk.com/t5/fusion-design-validate-document/sheetmetal-features/m-p/8323562, https://forums.autodesk.com/t5/fusion-360-ideastation-request-a/sheet-metal-die-punch-louvers-and-dimple-die-holes/idi-p/7361936 (search summary; forum posts, may be dated)
19. https://www.autodesk.com/products/fusion-360/blog/?p=4648, https://forums.autodesk.com/t5/fusion-design-validate-document/sheet-metal-unfold-flat-pattern-isnt-the-same-irl-settings/m-p/8679495 (search summary)
20. https://cad.onshape.com/help/Content/PartStudio/sheet_metal_model.htm (fetched)
21. https://cad.onshape.com/help/Content/PartStudio/sheet_metal_bend_relief.htm (fetched) and the Onshape sheet metal feature index in search results
22. https://cad.onshape.com/help/Content/PartStudio/sheet_metal_corner.htm (fetched)
23. https://cad.onshape.com/help/Content/PartStudio/sheet_metal_form.htm (fetched)
24. https://cad.onshape.com/help/Content/PartStudio/sheet_metal_table.htm (fetched)
25. https://cad.onshape.com/help/Content/PartStudio/sheet_metal_jog.htm (fetched)
26. https://sendcutsend.com/blog/how-to-export-a-dxf-from-onshape/, https://forum.onshape.com/discussion/28529/option-to-export-bend-lines-as-continuous-lines-in-flat-pattern-dxf-exports (search summary)
27. https://support.ptc.com/help/creo/creo_pma/r12/usascii/sheetmetal/sheetmetaldesign/About_the_Sheetmetal_Design_User_Interface.html (fetched: ribbon command list)
28. https://support.ptc.com/help/creo/creo_pma/r11.0/usascii/whats_new_pma/smt_unbending_and_flat_pattern.html (fetched)
29. https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/sheetmetal/sheetmetaldesign/About_Bend_Allowance_and_Developed_Length.html (fetched); https://support.ptc.com/help/creo/creo_pma/r12/usascii/sheetmetal/sheetmetaldesign/About_Y_factor_and_K_factor.html, https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/sheetmetal/sheetmetaldesign/About_Bend_Tables.html, https://support.ptc.com/help/creo/creo_pma/r13/usascii/sheetmetal/sheetmetaldesign/About_Sheet_Metal_Parameters.html (search summary)
30. https://support.ptc.com/help/creo/creo_plus/usascii/sheetmetal/sheetmetaldesign/About_Standard_Flange_Wall_Shapes.html (search summary)
31. https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/sheetmetal/sheetmetaldesign/About_Bend_Relief.html, https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/sheetmetal/sheetmetaldesign/About_the_Corner_Relief_User_Interface.html, https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/sheetmetal/sheetmetaldesign/To_Change_Relief_Settings.html (search summary)
32. https://support.ptc.com/help/creo/creo_pma/r11.0/usascii/sheetmetal/sheetmetaldesign/Configuration_Options_for_Sheetmetal.html, https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/sheetmetal/sheetmetaldesign/About_Creating_Flat_Pattern_Instances.html, https://community.ptc.com/3d-part-assembly-design-327/generate-sheet-metal-dxf-flat-pattern-25138 (search summary)
33. https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/sheetmetal/sheetmetaldesign/About_Multiple_Bodies_in_Sheetmetal.html (search summary, title only)
34. https://blogs.sw.siemens.com/nx-design/nx-how-to-series-sheet-metal/ (fetched)
35. https://blogs.sw.siemens.com/designcenter/designcenter-x-nx-essentials-how-to-sheet-metal-best-in-class-commands/ (fetched)
36. https://docs.plm.automation.siemens.com/data_services/resources/nx/12/nx_api/custom/en_US/nxopen_python_ref/NXOpen.Preferences.PartSheetmetalBendDefinitionMethodOptions.html, https://support.sw.siemens.com/en-US/okba/PL7007689/Why-is-the-Flange-command-using-a-Neutral-Factor/index.html, https://support.sw.siemens.com/en-US/okba/PL8750659/Explain-the-details-of-columns-used-in-sheetmetal-material-table/index.html (search summary; direct fetch redirects to a login portal)
37. https://www.cgeo.uliege.be/CAO/NX/62_Tutorial_metal2_en.pdf, https://www.scribd.com/document/442523880/sheet-metal-nx9-pdf, skill-lync NX sheet metal project pages (search summary: command lists)
38. https://blogs.sw.siemens.com/designcenter/NX-11-Advanced-Sheet-Metal-for-Automotive-and-Aerospace-Parts/, NXOpen AeroJoggleBuilder / AeroFlangeBuilder pages, https://enterprise.trimech.com/?p=73773 (CATIA Aerospace Sheet Metal course) (search summary)
39. https://blogs.sw.siemens.com/solidedge/sheet-metal-reliefs-and-corners/ (fetched)
40. https://blogs.sw.siemens.com/solidedge/sheet-metal-intro/ (fetched)
41. https://www.soliddna.com/SEHelp/ST6/EN/create_sheet_metal_part/shtpls1a.htm (fetched: deformation features, corner closures, gussets)
42. https://soliddna.com/SEHelp/ST5/EN/a_h/creatingbendtables1p.htm, https://www.soliddna.com/SEHelp/ST5/EN/i_v/material6d.htm, https://designfusion.com/gated-content/customer-resources/solid-edge---machinery-handbook-bend-formula-and-solid-edge, https://support.sw.siemens.com/en-US/okba/PL8520720/Using-Bend-Tables/index.html (search summary)
43. https://soliddna.com/SEHelp/ST5/EN/i_v/liverules_sheetmetal1a.htm, https://support.sw.siemens.com/en-US/okba/PL8008635/Cannot-convert-Synchronous-Thin-Part-to-Sheet-Metal/index.html, https://soliddna.com/SEHelp/ST5/EN/w_z/xid387464.htm (search summary)
44. https://support.sw.siemens.com/en-US/okba/PL8008308/What-features-in-the-Sheetmetal-enviroment-are-valid-base-features/index.html, https://support.sw.siemens.com/en-US/okba/PL8808426/Extend-profile-option-behavior-in-the-Sheetmetal-Jog-command/index.html, https://solidedge.siemens.com/en/?p=509 (search summary)
45. https://www.goengineer.com/training/catia-v5-generative-sheetmetal-design (fetched: course outline)
46. https://ascented.com/courseware/product/catia-v5-6r2023-sheet-metal-design, https://www.scribd.com/document/329549987/GSD-pdf, https://wichita.edu/industry_and_defense/NIAR/Documents/Sheet-Metal-Design-R30.pdf (search summary; the 2022x PDF returned 404)
47. https://www.slideshare.net/Anubhav5/catia-sheetmetal-parameters, https://www.scribd.com/doc/234734882/Catia-v5-Sheet-Metal-Design, https://eng-tips.com/threads/incorrect-k-factor.126032 (search summary: DIN K formula, parameter tabs)
48. https://www.staff.city.ac.uk/~ra600/ME2105/Catia course/CATIA Tutorials/draug_C2/draugbt0412.htm, https://3dswym.3dexperience.3ds.com/wiki/catia-user-community/catia-sheetmetal-design-1-sm1_IUknWOluSS6nOdchWlXrgQ (search summary)
49. https://help.bricsys.com/en-us/document/bricscad-mechanical/sheet-metal/sheet-metal-features (fetched)
50. https://help.bricsys.com/en-us/document/bricscad-mechanical/sheet-metal/k-factor (fetched)
51. https://help.bricsys.com/en-us/document/bricscad-mechanical/sheet-metal/unfolding-sheet-metal-parts-and-exporting-to-cam, https://help.bricsys.com/en-us/document/command-reference/s/smexport2d-command, SMASSEMBLYEXPORT command page (search summary)
52. https://www.zwsoft.com/user-guide/zw3d-sheet-metal-tutorial (fetched); https://www.zwsoft.com/blog/sheet-metal-design-tips (search summary: K-factor, jog)
53. https://doc.plasticity.xyz/solid/loft (fetched: full manual navigation, no sheet metal commands)
54. https://github.com/FreeCAD/FreeCAD/pull/31913 (read: description and comment thread)
55. GitHub search API, 2026-10-30: `repo:FreeCAD/FreeCAD "sheet metal"` (85 hits, first 60 read), `repo:FreeCAD/FreeCAD SheetMetal in:title` (10 hits), `repo:FreeCAD/FreeCAD-Enhancement-Proposals sheet metal` (0 hits); issue titles #18251, #29131, #32911, #33813 (read)
56. https://github.com/FreeCAD/FreeCAD/issues/12638 (read)
57. https://github.com/FreeCAD/FreeCAD/issues/13361 (read)
58. https://github.com/FreeCAD/FreeCAD/issues/30107 (read)
59. https://www.ondsel.com/blog/sheetmetal-strategy (fetched); FreeCAD 1.1 release summaries at medium.com and industryinsider.eu (search summary)
60. https://raw.githubusercontent.com/shaise/FreeCAD_SheetMetal/master/README.md (read: material definition sheets, K-factor ANSI/DIN, engineering mode, release notes to 0.8.26)
61. This repository: `ls src/Mod`, `git grep -il "sheet.?metal" -- src` (read: only an AddonManager resource matches)

# Part B. The SheetMetal addon, command by command

## B0. Source

- Repo: https://github.com/shaise/FreeCAD_SheetMetal, cloned (depth 50) to
  `/home/thunder/.cache/lv-merge/sheetmetal/addon`. The code was read, never run.
- HEAD: `cbf6595b68ad015266923749c160c3555a72e1fd`, 2026-10-17 09:05 +0300, "Fix broken links,
  licence and typos in README (#628)". package.xml version **0.8.26** (2026-10-15).
- Licence: **LGPL-2.1-or-later** (LICENSE is the LGPL 2.1 text; package.xml says
  `LGPL-2.1-or-later`; every file header says "version 2 of the License, or (at your option) any
  later version"). Copyright holders in headers: Shai Seger (2015-), Ulrich Brammer (2014, the V1
  unfolder), Ondsel Inc. (2023), Alex Neufeld (2025, the V2 unfolder). **Compatible with this
  fork's LGPL-2.1+ core**: the algorithms (or the code itself, translated to C++) can be reused,
  keeping the copyright notices. No Apache/GPL-only pieces. The only third-party runtime
  dependency is **networkx** (BSD-3), used by the V2 unfolder for a graph and a spanning tree; a C++
  port would replace it with a few dozen lines (BFS over a face adjacency list).
- Size: 37 .py files, 17,854 lines in total; the 33 top-level modules are 17,191 lines. Biggest:
  SheetMetalUnfolder.py 3,214 (V1 unfolder), SheetMetalCmd.py 2,377 (Make Wall),
  SheetMetalNewUnfolder.py 1,610 (V2 unfolder), SheetMetalTools.py 1,365 (shared helpers and the
  task-panel library), SheetMetalUnfoldCmd.py 1,104 (Unfold feature + panel). Plus 19 .ui panels,
  56 icons, 26 translation files. Dead code: `current.py` (214, a stray copy of the V2
  `TangentFaces` class added by #518, imported by nothing).
- Activity: **984 commits** since 2015-06-12 (GitHub API). 350 stars, 96 forks, 116 open issues
  + 3 open PRs. Last 50 commits span 2026-02-02 .. 2026-10-17: shaise 37 of them, the rest from 9
  occasional contributors (Merlin 3, Alex Neufeld 2, single PRs from others). All-time
  contributors: shaise 515, jaisekjames 125, luzpaz 46, Syres916 28, hasecilu 27, alexneufeld 21,
  sheetmetalman 17, ..., realthunder 9. **Effectively one maintainer** (Shai Seger) merging small
  community PRs; the two unfolders are each one outside author's work.
- Release cadence: version bump in README + package.xml, about **two per month** in 2026 (22
  release-note entries dated 2026, 0.7.60 2026-01-04 .. 0.8.26 2026-10-15; GitHub tags
  `V0.8.22-beta` .. `V0.8.26-beta`; the GitHub "Releases" page has only one 2018 entry). The Addon Manager
  installs master.
- Install route: Addon Manager (package.xml, `<depend>networkx</depend>`; the AM pip-installs it).
  networkx install failures are a recurring support topic (#461 open, 20 comments; #627 open;
  #509, #443 closed). Without networkx the Unfold feature silently falls back to V1
  (SheetMetalUnfoldCmd.py:44-69). **Note for the bench: `.conda/freecad` has no networkx today.**
- FreeCAD versions: no `freecadmin` in package.xml; code carries 0.20/0.21 branches
  (SheetMetalNewUnfolder.py:620, :1493-1503) and LinkStage3 branches (SheetMetalUnfolder.py:460
  `get_surface` uses LinkStage3's `findPlane`). shaise (#611, 2026-08-09): "I always check that my
  changes work on several versions of freecad (basically from V0.21) ... I'm also committed to
  LinkStage users". V2 unfolder needs Python >= 3.10.
- Merge into core: shaise in #611 (2026-08-09): it "was migrated with Ondsel version, not freecad
  core"; "In principle I completely agree [it belongs in core]. The thing is, I am a very bad coder,
  and I think sheetmetal does not meet FreeCAD's code quality standards", and prefers an "essential
  external WB" route. **No one in the tracker proposes a C++ rewrite.** #227 (2022, open): the
  unfolder could not be used headless then; the object classes are now GUI-free (see bench notes).

How the commands were found: `InitGui.py` `Initialize()` imports 17 modules; each registers its
command(s) with `Gui.addCommand` inside an `if SheetMetalTools.isGuiLoaded():` block. One
toolbar/menu list ("Sheet Metal", 18 entries) plus `SheetMetal_UnattendedUnfold`, which is always
registered but is put on the toolbar only when the preference "Engineering UX Mode" is on.
**19 registered commands.** The tree context menu repeats the list (plus PartDesign_MoveTip).

Object model: every geometric command makes a FeaturePython (`Part::FeaturePython`, or
`PartDesign::FeaturePython` when the input is in a Body, SheetMetalTools.smCreateNewObject:864),
whose `execute()` takes `baseObject[0].Shape.copy()` and returns a **whole new solid** built by
Part Python calls. There is no feature tree inside the part: each feature re-derives thickness,
bend axes and faces from the incoming B-rep by geometric search.

## B1. Command inventory (19 registered)

Legend: "Engine" lists the Part/OCCT calls doing the work. `cut/fuse/common` = BRepAlgoAPI
booleans, `extrude` = BRepPrimAPI_MakePrism, `revolve` = BRepPrimAPI_MakeRevol,
`makeOffsetShape` = BRepOffsetAPI_MakeOffsetShape/MakeThickSolid (fill=True makes a thick solid
from a face or shell), `makeFillet` = BRepFilletAPI_MakeFillet, `slice` = BOPAlgo_Splitter.
"k" is a K-factor.

### Sheet Metal toolbar (18) + UnattendedUnfold

| # | Command | File (lines) | What it does | Engine | Issues / notes |
|---|---|---|---|---|---|
| 1 | `SheetMetal_BaseShape` "Add base shape" | SheetMetalBaseShapeCmd.py (443) | Parametric starter part: Flat, L-Shape, U-Shape, Tub, Hat, Box with thickness, radius, width, length, height, flange width, origin, fill gaps. | `Part.makeBox`, then `smBend()` (the Make Wall core, #5) on the chosen box faces; Hat/Box run `smBend()` a second time for the rim flanges (smCreateBaseShape:60). | #298 (Tub/Hat/Box size wrong). |
| 2 | `SheetMetal_NewSketch` "New Sketch" | SheetMetalSketch.py (92) | Creates a sketch in the active Body so the user need not switch workbench. | none | Added 0.7.61. |
| 3 | `SheetMetal_AddBase` "Make Base Wall" | SheetMetalBaseCmd.py (278) | From a sketch: closed -> flat plate of thickness t (mid-plane / reverse); open -> a profile wall extruded by Length with bends of Radius at the corners, material Inside/Outside/Middle. | Closed: `makeFace(Bullseye)` + `extrude`. Open: wire `extrude`, `makeOffsetShape(t/2)`, `makeFillet(r + t/2)` on all edges, offset again, `slice` to get the trajectory, re-`extrude`, `makeOffsetShape(t, fill)` per face, `multiFuse` (smBase:60). | Accepts Draft clones (#575). |
| 4 | `SheetMetal_FromSolid` "Convert to Sheet Metal" | SheetMetalFromSolid.py (799) | Turns a solid or shell (e.g. a box) into sheet metal: selected faces are removed (open side), selected edges are ripped (seams), every other sharp edge becomes a bend of radius r; result offset to thickness t, inside or outside. | Own face/edge classification (smfsShapeInfo:470), flat-connected face groups, rips by cutting with circular `makePipeShell` lofts, bends as arc `extrude`s, `Part.makeShell`, then `makeOffsetShape(t, fill=True)` per shell (fallback per face + `fuse`) (smMakeSheetMetalFromSolid:647). **Only straight and circular edges** (raises otherwise). | New 2026-03 (0.8.00-0.8.03, issue #385). |
| 5 | `SheetMetal_AddWall` "Make Wall" | SheetMetalCmd.py (2,377) | Flange(s) on selected edges or thickness faces, all in one feature. Bend type Material Outside / Inside / Thickness Outside / Offset; length spec Leg / Tangential / Inner Sharp / Outer Sharp; angle, or angle and offset from a reference face; gap1/gap2, extend1/extend2, miter angles + AutoMiter with min gap / max extend; bend relief Rectangle/Round (w, d, or factor of t); perforated bend (slots); open sketch = multi-segment profile flange (LengthList/bendAList); closed sketch = wall shape; `unfold` flag builds the flange flat with a bend-allowance strip of (r + k t) * angle. | Per selected edge (smBend:1241): relief solids by `extrude` and `cut`, leg by `extrude`, bend by `revolve` of the thickness face, `fuse` into the growing solid, perforation by `revolve`/`extrude` + `cut`, `multiFuse` + `removeSplitter`. Geometric search by booleans: `smModifiedFace` (:587) and `smGetIntersectingEdge` (SheetMetalTools:1265) run a `common` per face/edge of the current solid until one overlaps; `smGetFace` (:631) is faces x faces `common`; `InsideEdge` (:941) is all-pairs `section`. | The most-reported tool: #614 (MAJOR BUG material inside / thickness inside, 12 comments), #561, #557, #631, #616, #615, #536, #576, #420. |
| 6 | `SheetMetal_AddHem` "Make Hem" | SheetMetalHem.py (712) | Hem on edges: Flat, Open, Teardrop, Rolled (width, radius, opening, roll angle, include bend). | Builds a length/angle list per type (`generateTeardropHem` solves with a bisection, :47) and feeds it through `smBend()` as a profile flange. | New 2026-03 (#562). |
| 7 | `SheetMetal_Extrude` "Extend Face" | SheetMetalExtendCmd.py (461) | Lengthens a wall by pushing a thickness face out (gaps, optional clearance against walls it would hit). | `extrude` of the side face, `fuse`, `cut`/`common` for clearance, `removeSplitter`. | Refactored 2026-02 (#482). #543 (negative lengths removed, 38 comments, open), #608, #585. |
| 8 | `SheetMetal_ExtendBySketch` "Extend by Sketch" | SheetMetalExtendCmd.py | Tab from a closed sketch on a wall face, fused to the sheet; optional intersection clearance. | `makeFace` + `extrude(t)`, `fuse`, `cut`. | #559, #558. |
| 9 | `SheetMetal_AddFoldWall` "Fold a Wall" | SheetMetalFoldCmd.py (400) | Folds an existing flat wall along a sketch line; bend line position forward / middle / backward / "intersection of planes"; radius, angle, k, invert. | `BOPTools.SplitAPI.slice` of the solid by the extruded line, `cut` of a strip of width (r + k t) * angle, rotation of the moving part, the strip re-bent by `bend_solid` (wrap onto a cylinder at the neutral radius, then `makeOffsetShape(t, fill)`, SheetMetalBendSolid.py:183), `multiFuse` (smFold:40). | #633 (crash unfolding two consecutive sketch folds), #573, #507, #306, #266, #262. |
| 10 | `SheetMetal_Unfold` "Unfold" | SheetMetalUnfoldCmd.py (1,104), SheetMetalNewUnfolder.py (1,610), SheetMetalUnfolder.py (3,214), SheetMetalBendCuts.py (369), SheetMetalKfactor.py (208), SheetMetalLookup.py (151), SheetMetalText.py (119) | Flat pattern of the part from a selected planar face: flat solid (in place) + optional sketches (outline, internal lines, holes, bend lines, bend-angle labels, laser "hinge" bend-relief cuts: straight / tiled custom sketch / wave), DXF or SVG export. K-factor manual (ANSI or DIN) or from a material table. | See B2. | #623 (unfold took a minute; fixed by #624), #607, #571, #563, #547, #540, #537, #533, #513. |
| 11 | `SheetMetal_UnattendedUnfold` (toolbar only in engineering mode) | SheetMetalUnfoldCmd.py | Unfold with the last-used options, no task panel. | as #10 | -- |
| 12 | `SheetMetal_UnfoldUpdate` "Unfold Update" | SheetMetalUnfoldCmd.py | Recomputes unfold objects queued for manual recompute. | -- | **Effectively dead**: `IsActive` needs `smObjectsToRecompute` non-empty, and nothing calls `smAddToRecompute` (SheetMetalTools.py:986 is defined, never called), so the button is never enabled. |
| 13 | `SheetMetal_AddCornerRelief` "Add Corner Relief" | SheetMetalCornerReliefCmd.py (897) | Relief where two bends meet, from 2 edges: Circle, Circle-Scaled, Square, Square-Scaled, Weld, Weld-Scaled, or a user sketch; size, ratio, offsets, k. | `getBendDetail` computes the neutral radius; the relief sketch face is cut flat, wrapped back by `bend_solid`; `makeOffset2D`, `makeOffsetShape`, `cut`, `multiFuse`, `removeSplitter` (smCornerR:401). | Weld types 2026-09 (#617/#619/#620). #609, #605. |
| 14 | `SheetMetal_AddRelief` "Make Relief" | SheetMetalRelief.py (249) | Relief notch at a corner vertex of a solid-modelled part. | Per adjacent face a relief face `extrude`d, `multiFuse`, `removeSplitter`, `cut` (smRelief:92). | -- |
| 15 | `SheetMetal_AddJunction` "Make Junction" | SheetMetalJunction.py (205) | Rip (gap) along an edge where two walls of a solid meet; the "model a box, rip it, bend it" workflow. | `fuse` of the two faces, `makeFillet(gap)`, `cut`, `makeOffsetShape(-gap, fill)`, `cut` (smJunction:40). | #602, #516. |
| 16 | `SheetMetal_AddBend` "Make Bend" | SheetMetalBend.py (244) | Rounds a ripped sharp corner of a solid into a bend: inner edge radius r, outer r + t. | `makeFillet` twice (smSolidBend:80). | #516. |
| 17 | `SheetMetal_SketchOnSheet` "Wrap Cutout" | SketchOnSheetMetalCmd.py (297) | Cuts a closed sketch that runs from a flat face across bends onto the next walls (the cut is unrolled over each bend). | Flat part `common` + `extrude(t)`; the leftover is walked bend by bend: find the cylinder via `ancestorsOfType`, wrap with `bend_solid`, continue on the next plane; `multiFuse`, `cut` (smSketchOnSheetMetal:38). | #621 ("Wrapped cutout is not wrapping", 10 comments). |
| 18 | `SheetMetal_AddCutout` "Extruded Cutout" | ExtrudedCutout.py (542) | Cut from a closed sketch (two lengths / symmetric / through all), made normal to the sheet on every wall it crosses, bends included. | `extrude`, `common` with the sheet, the touched faces offset through t (`makeOffsetShape`) and closed with `Part.Shell` from parallel face pairs, `fuse`, `cut`, `removeSplitter`; "ImproveCut / ImproveLevel" options (#403). | #493 (countersunk hole lost in unfold). |
| 19 | `SheetMetal_Forming` "Make Forming in Wall" | SheetMetalFormingCmd.py (380) | Stamps a user-modelled tool solid (dimple, louver, emboss) into a wall: tool face(s) choose the opening; placed on the selected face, optionally at every point of a sketch (pattern), with angle and offsets; `SuppressFeature` so it can be turned off before unfolding. | Tool shell minus the selected faces offset by t (`makeOffsetShape(fill)`, fallback `makeThickness`), placed by rotate/translate, then `base.cut(tool).fuse(offsetShell.cut(base))` (makeforming:79). | #594 (hang), #593, #305 ("too complicated for most cases"), #283, #259, #563 (unfold with embosses), #515. |

### Dead / unregistered

- No command file is unregistered; every `addCommand` call is reached from `Initialize()`.
- `SheetMetal_UnfoldUpdate` is registered but can never be active (row 12).
- `current.py` (214 lines) is an orphan module.
- `Macros/SheetMetalUnfoldUpdater.FCMacro` (255): batch re-unfold + TechDraw refresh; its header
  says it "depends on LinkStage3 toponaming feature" and needs the GUI.
- Tests (SMTests/, 355 lines): `testKfactor` checks `get_val_from_range`; `testBendCuts` checks the
  hinge-cut pattern; `testFolder.test_get_point_on_cylinder` assigns `d["expected"] = result`
  before `assertEqual(result, d["expected"])`, so it can never fail. **No test builds a part or
  unfolds one**; #108 ("Unit tests to catch regressions", 2020) is still open.

## B2. The unfolder in detail

`SMUnfold.execute` (SheetMetalUnfoldCmd.py:560) picks **V2** (`newUnfolder`, :371 ->
SheetMetalNewUnfolder.getUnfold) when networkx imports and the preference `UseOldUnfolder` is off,
else **V1** (`oldUnfolder`, :528 -> SheetMetalUnfolder.getUnfold). The input is any solid plus one
selected planar face; neither unfolder reads the SM feature history. The unfold is a feature, so
it re-runs on every recompute of its input (there is a ManualRecompute property, but it only stops
the task panel's live refresh, SheetMetalTools.py:678-689; #412 lists "Parametrize unfolding (using
manual update button, because it is slow)").

### V2 (Alex Neufeld, 2025; SheetMetalNewUnfolder.py) -- the default

1. **Thickness** `EstimateThickness.using_best_method` (:180): modal length of straight edges at
   the root face's vertices that are parallel to its normal (:89); else the nearest parallel
   plane's distance (:160); else modal difference of coaxial cylinder radii (:117).
2. **Face graph** `build_graph_of_tangent_faces` (:1088): for every edge of the solid,
   `shp.ancestorsOfType(e, Part.Face)` (:1096-1098); for edges with exactly two faces,
   `TangentFaces.compare` (:431) tests tangency by surface-type pair (plane/cylinder/torus/sphere/
   cone/extrusion; non plane-or-cylinder pairs warn). Tangent pairs become graph edges labelled
   with the edge index; the connected component containing the root face is kept (:1130) -- that is
   **one skin of the sheet** (top or bottom), since sharp thickness-face edges are not tangent.
3. **Tree** `unfold` (:1350): `nx.minimum_spanning_tree(weight="label")` (:1371), then
   `nx.all_pairs_shortest_path_length` on that tree (:1377) **only to read the distances from
   the root**, then a DiGraph pointing away from the root.
4. **Bends**: for each tree edge whose child face is a `Part::GeomCylinder` (only cylinders are
   unrolled; a non-line parent edge raises "bends across non-straight edges", :1400),
   `compute_unbend_transform` (:1240) builds a local frame at the right UV corner and the 4x4
   unbend = rotate back by the bend angle about the bend axis + translate by the bend allowance;
   `unroll_cylinder` (:1161) maps each non-seam edge's pcurve (Line2d or BSplineCurve2d poles)
   from (u, v) to (v, (u - umin) * BA/angle) -- an exact development of the cylinder skin -- and
   records a `BendInfo` (centre line, angle, radius) (:1140).
5. **Placement**: `nx.shortest_path(dg, root)` (:1437); each face's edges are transformed by the
   product of the unbend matrices along its path (`reduce(mul)`, :1450). Planar faces keep their
   edges (moved), cylinders contribute their unrolled edges.
6. **2D cleanup and solid** `getUnfold` (:1488): `Edge2DCleanup.clean_and_structure_geometry`
   (:1080) = B-splines to lines/arcs (`toBiArcs`, tol 0.1 mm) + `Part.sortEdges(fuzz 1e-3)` and
   endpoint snapping (:953) + split circles merged; `Part.makeFace(..., Bullseye)`; `extrude` by
   -t; bend lines = `common` of the bend-line compound with the flat solid (:1522).
7. **Sketches** `getUnfoldSketches` (:1529): outline / inner wires / holes taken from the top face
   of the flat solid (`extract_manually`, :638), bend lines, bend-angle labels drawn as line text
   (SheetMetalText), moved to the XY origin, written into `Sketcher::SketchObject`s (unconstrained).
   Laser hinge cuts are a post-process of the BendInfo list (SheetMetalBendCuts.build_relief_cuts).

What V2 therefore does NOT do: it never builds the opposite skin or the thickness faces of the flat
part (the flat solid is the root-side outline extruded by t), so anything that differs between the
two skins (countersinks #493, embosses/forms #563, chamfers) is lost or breaks it; only cylindrical
bends unroll (cones, i.e. tapered bends, do not); faces outside the root's tangent component are
silently dropped (other bodies; plausibly also #607 "removes material where sheets touch", whose
body I did not read). The issue titles #493 and #563 match the one-skin design; their bodies were
not read either.

### V1 (Ulrich Brammer 2014-2018, "sheet_ufo"; SheetMetalUnfolder.py) -- fallback

`SheetTree.__init__` (:499) measures thickness with a line `common` the solid (:622).
`Bend_analysis` (:1563) recurses from the root face; `make_new_face_node` (:1154) classifies each
face Flat/Bend, and **searches the counter face by scanning every remaining face's vertices**
(:1173-1260, optionally `distToShape` per candidate), cuts/divides side faces (`cutEdgeFace` :916
uses `cut`), and collects child faces through `ancestorsOfType`/`isSame` loops (21 such calls).
`unfold_tree2` (:2858) walks leaves-first: every face below a bend is rotated about the bend axis
and translated by the arc length; bend faces are rebuilt vertex by vertex (`unbendFace` :2165,
using the node's k), curved edges via `makePipeShell` (:2493). All faces -> `Part.Shell` ->
`Part.Solid` -> `removeSplitter` (getUnfold :2943). On failure it may **add a sewn copy of the
input to the document from inside execute** (:3016-3036). Sketches come from TechDraw `projectEx`
of the flat solid (:3118). V1 unfolds both skins and the side faces, so it keeps through-thickness
detail, at a high cost.

### Bend allowance / K-factor

- Model: BA = (r + k t) * angle with r the inner radius, i.e. the K-factor neutral-axis model
  only. No bend deduction tables, no bend-allowance tables, no per-bend override at unfold time.
  K is ANSI (0..1) or DIN (= 2 x ANSI); DIN is halved internally (:811).
- Sources: one manual k per Unfold feature; or a "material definition sheet" spreadsheet named
  `material_*` with rows (Radius/Thickness, K-factor) -> k per bend by r/t; or (V2 only, 0.8.25,
  2026-10) an external CSV library keyed by **thickness** (one k for the whole part;
  SheetMetalLookup.get_k_factor_from_csv:34, thickness guessed by `get_sheet_thickness` :136 as
  the distance to the first face between 0.1 and 30 mm).
- V1 lookup is a step function (`get_val_from_range`, SheetMetalLookup.py:89, first key >= r/t).
  V2 interpolates, but **the interpolation is wrong past the table's second row**:
  `get_k_factor` (:725) loops `while r_over_t <= self.radius_thickness_values[i]`, which is false
  on entry, so i stays 0 and every r/t is (extra)polated on the first segment. With the README's
  table (1: 0.38, 3: 0.43, 99: 0.5), r/t = 50 gives k = 1.605 (single-value k and two-row tables
  are unaffected). Also for DOWN bends V2 looks up r/t with the root skin's radius, which is the
  outer one, (r + t)/t (:1174 + :728). A native unfolder fixing these changes flat
  sizes, so it must be compared against the addon on purpose, not assumed equal.
- Each folding feature carries its **own** k (Make Wall `kfactor` for its `unfold` mode, Fold a
  Wall, Corner Relief, Wrap Cutout), independent of the Unfold feature's k; nothing checks they
  agree.

### Export

`smGuiExportSketch` (SheetMetalTools.py:822, GUI-only) writes the generated sketches with Draft's
`importDXF.export` / `importSVG.export`. DXF content is lines, arcs and circles only (V2 converts
B-splines to bi-arcs). No layers beyond separate sketches, no bend-direction/angle annotation in
DXF other than the stroked text labels.

### Complexity: where the time goes

- **V2 face graph is quadratic in the solid's size.** `ancestorsOfType(e, Part.Face)` with a type
  object runs `TopExp::MapShapesAndAncestors` over the whole shape on **every call**, then
  element-maps the result (this fork: src/Mod/Part/App/TopoShapePyImp.cpp:1349-1370; upstream's
  method builds the map per call as well as far as I recall -- not checked against a clone; the
  string-type overload, which uses a cached ancestor map in this fork, is not the one the addon
  calls), and it is called once per edge (:1096). E edges -> E full ancestor maps. The
  2026-09-30 speed-up #624 (issue #623: a part took "a full minute" to unfold) only removed a
  second quadratic term (`shp.Edges[i]`/`shp.Faces[i]` rebuilt the whole list per index).
- `all_pairs_shortest_path_length` (:1377) is O(F^2) in tree nodes to get one row.
- The rest of V2 is linear (one transform per face; matrices multiplied along each path, so
  O(F * depth)).
- V1: counter-face search is O(F) per face (O(F^2) vertex-plane tests, optionally
  `distToShape`); plus face cutting booleans and a final Shell/Solid/removeSplitter on all faces.
- **Feature side (the bigger cost on large parts):** every SM feature copies the whole incoming
  solid and runs several booleans on it; Make Wall runs per flange 3-6 booleans plus
  `smModifiedFace`, which is up to one `common` per face of the current solid, and all-pairs edge
  `section`s for miters. A chain of N features is therefore O(N) booleans on solids of size O(N)
  -> O(N^2) work, and an edit near the root re-runs all of it, then the unfold. The bench
  (`bench.py`) measures both the "fan" (N flanges in one feature) and "chain" (N features) shapes.

## B3. References, robustness, and the special workflows

- **References (TNP).** Every feature stores `baseObject` as `App::PropertyLinkSub` with short
  names ("Face12", "Edge7") taken from the selection; the code strips any mapped prefix
  (`getElementFromTNP`, SheetMetalTools.py:1117, which warns "Tnp Name still visible") and then
  does `shape.getElement(...)`. All features declare `getElementMapVersion` returning
  "sm1." + version so element maps are rebuilt when the modelling logic changes. The addon has no
  naming logic of its own: it **relies on the core to remap PropertyLinkSub when the input
  changes**. shaise (#342, 2025-07-20): "on realthunder's FreeCad version, everything works fine.
  no TNP issues with SheetMetal"; and "the code was taken from LinkStage3 version". On upstream
  1.x it is still the release blocker: #412 milestone "Make the WB TNP-aware?" unchecked, shaise
  2025-08-25 "out of all the issues, the TNP is the most important", 2026-03-08 "thats my main
  blocker". Open TNP issues: #342 (34 comments), #343. Robustness is thus as good as the core's
  element map **and** as stable as the Python construction (shapes made with `Part.Face(...)`
  rather than `makeFace` carry no history, per the #342 thread).
- Inside the unfolders, faces and edges are addressed by index and `hashCode()` within one
  execute only; the root face is a stored "FaceN" link like any other.
- Recompute surprises: #389 (open) -- a file saved where the addon is not installed loses the
  Proxy classes, after which edits silently stop propagating.
- **Forming tools**: user-supplied tool solid, placed and stamped by offset + booleans (row 19);
  patterns by sketch points; no library of standard forms; the unfold does not flatten forms --
  the tooltip tells the user to set `SuppressFeature` before unfolding; #563 (unfold with
  embosses) open.
- **Corner relief**: per-corner feature from two edges (row 13); bend relief inside Make Wall
  (rectangle/round); vertex relief (row 14). No automatic corner treatment for a closed box
  (AutoMiter only gaps/miters the flanges). Open: #618 "Discussion about corners", #605, #609.
- **Convert solid to sheet metal**: two routes. (a) "Convert to Sheet Metal" (row 4, 2026-03):
  choose open faces and rip edges, bends generated, offset to t. Straight/circular edges only.
  (b) the classic route: PartDesign Thickness (or a hollow solid) -> Make Junction rips (row 15)
  -> Make Bend fillets (row 16) -> Unfold, which works on any solid of constant thickness with
  cylindrical bends because the unfolder is history-free. "Auto-unfold STEP files" #95 open since
  2019.
- **Multi-body**: features live in a PartDesign Body or as free Part features; the unfold takes one
  connected sheet per Unfold object; a multi-solid input is truncated to the root's component
  (V2). #462 (all cut files from an assembly) and #469 (unfold as a separate body) open.
- **Refold**: none. #296 "[feature] refold unfolded sheet" open since 2024-01; the flat solid is a
  dead end (features cannot be added in the flat state and folded back). The only "flat" modelling
  is Make Wall's `unfold` flag and Fold a Wall on a flat face.
- **Headless**: object classes, unfolders and the K-factor code are GUI-free; export, task panels
  and the Unfold sketches' view styling are GUI-only (bench.py header lists what bites).

## B4. Issue tracker: counts and themes

Open issues on 2026-10-30: **116** (+ 3 PRs, oldest 2025-01). Classified by title (and body for
the borderline ones):

| Theme | Count | Representative |
|---|---|---|
| Feature construction gives wrong geometry or errors (Wall, Fold, Extend, Junction, Relief, Forming, Base shape) | 35 | #614 "MAJOR BUG with material inside and thickness inside" (12 comments); #561 "AddWall leg disappears if length goes below bend radius"; #543 negative extend lengths (38 comments); #631, #616, #615, #557, #427 inner bends on curved surfaces; #594 FreeCAD hangs with wall forming |
| Unfold fails or gives a wrong flat | 11 | #633 crash unfolding two consecutive sketch folds; #607 "Unfolding wrongly removes material where sheets touch"; #571 "Bigger gap when unfolding"; #563 embosses; #547 holes inside the bend; #540 circle cutout not a circle (V2); #537 "Unfold broken when part contains splines"; #533; #513; #493 countersink lost; #122 fold length |
| References / recompute / TNP | 5 | #342 "Bend: Invalid subelement name (due to Topoaming)" (34 comments); #343 errors recomputing 0.20-0.21 files; #389 "operations do not update on complex model"; #276; #573 |
| Dependency (networkx) | 2 | #461 (20 comments), #627 |
| UI / UX only | 19 | #582 cannot remove geometry with Delete; #480 commands need prior selection; #566 use upstream feature preview; #544 unfold value as expression |
| Feature requests | 38 | #296 refold; #622 bend position notches; #603 ducts/elbows/transitions; #535 A-fold/B-fold (foamboard); #488 perforation on base walls; #462 cut files from an assembly; #297 fold along cylindrical wires; #95 auto-unfold STEP; #96 detect welded hardware; #86 trim; #357 auto-miter on the bend; #227 headless use; #186 non-metal materials |
| Project / meta | 6 | #611 "migrating sheetmetal to freecad?"; #412 v1.0 milestones; #108 unit tests; #43 API docs |

- **Performance**: no open issue is about speed. Closed: #623 "Unfolding takes a lot of time"
  (2026-09-29: one part took "a full minute"; moving the tip back made it fast) fixed in a day by
  #624; PR #403 (2024) added "Improve cut" options to Extruded Cutout for speed; #412 made Unfold
  manual-update "because it is slow". The tracker's users mostly model small parts; there is no
  report on parts with dozens of bends either way -- **the bench has to answer the "slow" half.**
- **Missing features visible in the tracker**: refold, lofted bends / transitions (#603), conical
  (tapered) bends, cross breaks, bend notches, forming library (forming is user-modelled), hem
  variants beyond 4 types (rolled exists), A/B folds, assembly-level flat export, STEP auto
  recognition, bend tables / bend deduction, per-bend k.
- **Rewrite / core**: nobody proposes C++. Merge into core was discussed in #611 (2026-08): the
  maintainer agrees in principle, cites code quality and multi-version/LinkStage support as reasons
  to stay external, and suggests an "essential addon" mechanism instead.

## B5. What this fork already has that a native workbench would build on

- **Part / OCCT** (all already wrapped as element-mapped `TopoShape::makE*` builders,
  src/Mod/Part/App/TopoShape.h): `makEPrism`, `makERevolve` (a bend is a revolved thickness
  face), `makEBoolean`/`makECut`/`makEFuse`, `makEThickSolid` / `makEThicken` / `makEOffset` (plus
  `BRepOffsetAPI_MakeOffsetFix`) for shell -> sheet, `makEFillet` for rip-and-bend, `makESlice` /
  `makEGeneralFuse` for folds, `makEPipeShell`/`makELoft` for transitions. The **OCCT fork's TKOffset work** (thickness/offset fixes with
  opt-in fix levels, LinkVibe-801 branch: e.g. d572352e2e, b68a0320fa) is exactly the kernel path
  "Convert to Sheet Metal" and forming tools lean on (`makeOffsetShape(fill=True)`,
  `makeThickness`).
- **PartDesign**: `FeatureThickness` (hollow a solid; the classic convert route), `FeatureThicken`
  (surface -> solid, PartDesign/App/FeatureThicken.h: Faces, Value, Reversed, Midplane, Join), the
  `FeatureAddSub`/`FeatureDressUp` bases a Wall/Hem/Relief feature would derive from, `Hole`
  (countersinks that a two-skin unfolder must keep).
- **Element naming**: the addon's TNP story already depends on this fork's PropertyLinkSub
  remapping; a native feature gets it directly, with op codes per created face (bend, leg, relief)
  instead of anonymous booleans -- the base for stable flat-pattern references (bend lines, labels)
  and for refold.
- **Surface workbench**: docs/Surface.md 5.4 defers "Flatten [C 30]" to sheet metal; the Curves
  FlattenFace development of cylinder/cone/extrusion faces (docs/SurfaceSurvey.md B1 #30) is the
  same maths a tapered-bend or lofted-transition unfold needs.
- Nothing sheet-metal specific exists in src/ today.

## B6. Assessment

**Limited in features: yes, against any commercial sheet-metal module.** The addon covers the
everyday core well -- base plate, flanges (one feature, many edges, auto-miter, reliefs, length
specs, reference faces), hems, sketched folds, tabs, cutouts normal to the sheet, wrap cutouts,
corner reliefs, convert-from-solid, user forming tools, unfold with K-factor tables and
DXF/SVG with bend lines and hinge cuts. It lacks: refold / flat-state editing (#296), lofted and
conical bends / transitions (#603), cross breaks, bend notches (#622), a forming-tool library,
bend deduction or allowance tables and per-bend k at unfold, multi-body/assembly flat export
(#462), and an unfold that keeps both skins on the default path (V2 drops countersinks #493,
embosses #563).

**Likely slow on large parts: the structure says yes; the tracker cannot confirm it.** Evidence
from the code: every feature copies and re-booleans the whole solid; Make Wall uses booleans as
geometric search (one `common` per face until a match) and all-pairs edge sections; a chain of N
features is O(N^2) and fully re-runs on any upstream edit; the V2 face graph rebuilds an ancestor
map per edge (quadratic in edges) and calls networkx's all-pairs path lengths for one row. #623 is
the one recorded case (a minute for one part, fixed by removing a different quadratic term), and
#412 made unfolding manual because it was slow. The bench quantifies N = 1..64.

**Correctness debt worth knowing before matching it**: the V2 K-factor interpolation bug and the
DOWN-bend r/t lookup (B2), per-feature k disconnected from the unfold k, a test suite that tests
nothing of the geometry, 35 open construction bugs (material-inside offsets the worst, #614).

**Day-one core a native workbench must match** (what users of the addon do daily):
1. Base wall from a closed sketch (plate) and from an open sketch (profile with bends).
2. Make Wall on many edges at once: angle, radius, length spec (leg / tangential / inner / outer
   sharp), bend position (material inside / outside / thickness outside / offset), gaps/extends,
   auto-miter, bend relief (rectangle / round), reference-face angle/offset.
3. Hem (flat, open, teardrop, rolled).
4. Fold along a sketch line; tab / extend by sketch; extend face.
5. Cutout normal to the sheet (extruded cutout) and wrap cutout across bends.
6. Corner relief (circle/square/weld/sketch) and rip + bend of a solid (junction/bend), and
   convert solid/shell to sheet metal.
7. Forming with a user tool (pattern by sketch points).
8. Unfold: flat solid + outline/inner/hole/bend-line/label geometry, K-factor ANSI/DIN, material
   table by r/t, DXF/SVG export, hinge (laser bend-relief) cuts.
9. Read the addon's documents: the feature parameters above (proxies `SheetMetalCmd.SMBendWall`,
   `SheetMetalBaseCmd.SMBaseBend`, `SheetMetalUnfoldCmd.SMUnfold`, ...) so existing files keep
   working, or a converter.

**What it could do better:**
- Features that know their topology: create bends and legs with element-mapped op codes, no
  boolean-as-search, and update incrementally instead of re-booleaning the whole part per feature.
- A feature-aware flat pattern (the history knows every bend: axis, radius, angle, k) with a
  geometry-only unfolder kept for imported STEP; both skins and thickness faces kept (holes,
  countersinks, forms marked), cylinders and cones unrolled, multiple bodies and assembly export.
- Refold and flat-state features (cut in flat, fold back), bend notches, cross breaks, lofted
  transitions, a forming library, bend tables (allowance / deduction / k by material, thickness,
  radius, angle), per-bend overrides, and checks (relief too small, flange shorter than minimum,
  collisions on folding).
- Live and out-of-process: unfold in a worker (docs/ComputeBoundaries.md), so editing a 64-bend
  part does not block the GUI; headless and browser-tier friendly (no Draft/PySide imports in the
  geometry path).
