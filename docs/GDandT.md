# GD&T / PMI: design (2026-11-19, proposed)

Status: APPROVED 2026-11-19 (section 11 decided); P0 in progress (section 10). Branch `pmi`. The user asked on 2026-11-19 for GD&T on
PartDesign parts, imported from and exported to STEP, and linked into TechDraw, with a survey of
other CAD programs first. The survey is docs/GDandTSurvey.md, cited as [A n] (other programs,
the standards) and [B n] (our code). Section 11 lists the decisions that are the user's.

## 1. What it is for

A drawing's dimensions and feature control frames are the contract with the shop and the
inspector: which surfaces matter, by how much they may vary, and from what they are measured.
Model-based definition (MBD) puts that contract in the 3D model, where a CAM program, a CMM
program and the next CAD program can read it, instead of in a picture of the model.

Three things are wanted:

1. **Author** GD&T on a part in 3D: datums, size and location dimensions with tolerances, and
   geometric tolerances, attached to the part's faces and surviving its edits.
2. **Exchange** it through STEP AP242 as *semantic* PMI (machine-readable) with a matching
   graphic, and read what other programs write.
3. **Drawings**: TechDraw shows the same annotations in its views, linked to the 3D ones, not
   copied.

Not in scope here: tolerance stack-up analysis, CMM program output (QIF), surface texture
(ISO 1302) and weld symbols in 3D -- each fits the model below later (section 10).

## 2. What the survey settles

These are the points every program agrees on, or the standard fixes, so they are not open:

1. **Semantics and presentation are separate** [A2, A7, A9.2]. A tolerance is data (what,
   how much, from what); where its frame is drawn is presentation. STEP links them n:m; the
   graphic is regenerated from the data, never the other way.
2. **The toleranced feature is an object of its own** [A1, A7]: a set of faces (or a derived
   axis/centre plane/point) that every annotation on those faces shares. STEP's
   `shape_aspect` is exactly this, and SolidWorks's DimXpert feature.
3. **Datum feature and datum are different things** [A6]: the faces, and the exact geometry
   simulated from them.
4. **One datum system per distinct reference frame**, reused [A7].
5. **References break, and the break is shown** [A9.1]: kept, flagged, reattached by the user.
   Never silently deleted.
6. **Annotations sit on planes** with a reading direction; drawings take those parallel to the
   sheet [A1, A3, A9.3].
7. **The graphic is ours to make** [A8]: OCCT stores and writes presentation shapes but draws
   none.

And what we have makes three things cheap [B1-B4]:

- OCCT already reads the GD&T of every STEP file we import, and its AP242 semantic writer is
  complete; we drop the first and never feed the second.
- The element map gives TNP-safe face references, and TechDraw already names each projected
  edge and face after its 3D source -- the 3D-to-2D link is built.
- Import Edge Labels is a working pattern for "3D annotation info -> editable 2D objects that
  keep the user's moves".

## 3. Where it lives

A module of its own, **`src/Mod/PMI`** (App + Gui, Python `PMI`), built the way the Surface
module is -- which lives outside PartDesign and still works inside a Body [B4]:

- **App** depends on Part only and owns the objects (section 4), the layout (6.1) and the check
  (8). Import (STEP) and TechDraw depend on it, not on PartDesign. Not in PartDesign, because an
  imported STEP part is a `Part::Feature`, not a Body, and must carry its PMI too; Assembly will
  want tolerances between parts later.
- **In the Body, referring to its solids** (revised 2026-11-19, the user's question). The
  annotation set is one of the Body's members, as its Elements group is
  (docs/AssemblyObjectModel.md sec 5.3), and its annotations refer to faces through the solid's
  `PartDesign::SolidBody`: `(Solid, "Face6")`. Measured, the three ways a reference could go:
  - `(Body, "Face6")` is wrong: a Body may hold several solids, and its own shape is its **tip's**
    solid. With a second solid added (and the tip in it), the same reference resolved to the
    *other* box's top face, silently -- nothing went missing, so nothing was flagged.
  - The set *inside* the Body linking the Body is a cycle ("The graph must be a DAG").
  - The set inside the Body linking a solid body is neither: the Body depends on the set through
    its group, the set's annotations on the solid body, never on the Body. The set names the Body
    by a hidden link (no dependency).
  So: a Body holding a set gives even a lone solid a solid body (`Part::BodyBase::
  requireSolidBody`, as sheet metal already gets one), annotations travel with the Body (copy,
  link into an assembly, export), each solid is a part with its own datum letters, and the tree
  can show each solid's annotations under it, as it shows its elements.
- **Editing an annotation rebuilds nothing downstream.** A member of a Body that changes makes
  the Body recompute; measured, that set the Body's shape again and rebuilt everything depending
  on the Body (a refine stood in for a drawing). `Body::execute` now keeps its shape when the
  tip's is the one it took last time (same TShape, transform, element map), so an annotation --
  or a VarSet, which had the same cost -- changes the Body's revision no more.
- **Beside the shape** where there is no Body: an imported `Part::Feature`, a Split piece. The
  set sits next to it, `Part` names it, references are `(Feature, "FaceN")`.
- **Gui** owns the commands and task panels, once, and offers them in two places:
  - its own **PMI workbench**, for an imported part or an assembly (and the name upstream's
    proposal and its "Mod: PMI" label use, [A8]);
  - **inside PartDesign** (and later Part and Assembly) through a `Gui::ToolContext`, as
    Surface's tool bars are offered in PartDesign while a surface group is active
    (`Surface/Gui/AppSurfaceGui.cpp:102-111`): the PMI tool bars appear while a Body's
    annotation set is active or a PMI object is in edit, so annotating a Body never means
    leaving PartDesign. The way in is sheet metal's (docs/SheetMetal.md sec 8.1): the Part
    Design menu's **PMI** submenu always holds Datum, Geometric tolerance, Dimension and
    Note. An annotation made in PartDesign with no set active makes its set active, so the
    first one -- which makes the Body's set -- brings the tool bar.

Names follow the upstream proposal (decision 1): its `PMI_AnnotationSet` is our
`PMI::AnnotationSet`, and so on; section 12 maps every part of it.

## 4. The object model

```
PMI::AnnotationSet  (in a Body, its solids' faces by solid body; beside any other shape)
 |-- Standard, Units, GeneralTolerance (ISO 2768-m, ...), Planes, Views
 |
 |-- PMI::TolerancedFeature  "toleranced feature" (= shape_aspect)        <-- shared by everything below
 |      References (faces of the part, mapped names), Kind, derived geometry
 |
 |-- PMI::Datum 'A'   -> PMI::TolerancedFeature, targets
 |-- PMI::DatumReferenceFrame  |A|B(M)|C|  -> datum features, modifiers
 |-- PMI::Dimension    size / location / basic   -> PMI::TolerancedFeature(s)
 |-- PMI::GeometricTolerance    characteristic, zone, modifiers  -> PMI::TolerancedFeature, PMI::DatumReferenceFrame
 |-- PMI::Note         free text, flag notes
 |
 |-- presentation on each item: plane, position, leaders, style (section 6)
 |-- PMI::View         camera + member list + clipping (section 6.3)
```

All are `App::DocumentObject`s (C++, App side, Python bindings by the usual XML pattern), so a
headless FreeCADCmd reads, edits, checks and exports them; the Gui only draws and edits.

### 4.1 PMI::AnnotationSet

The container and the part's settings: `Part` (a hidden link: the Body it is in, or the shape it sits beside),
`Standard` (ASME Y14.5-2018, ASME Y14.5-2009, ISO GPS -- it changes the default principle,
envelope or independency, and which symbols are legal [A6]), `GeneralTolerance` (ISO 2768 class
or a +/- default, for the title block and for dimensions without their own), `Units`,
`DecimalPlaces`. A Body has at most one, in it; another shape at most one, beside it. It is
created with the first annotation. Its group takes only PMI objects. Datum letters are each
solid's own (sec 8): a Body's two solids are two parts.

### 4.2 PMI::TolerancedFeature -- the toleranced feature

The heart of the model, because it is what survives edits.

- `References`: `PropertyLinkSubList` to the **solid's result shape** -- its
  `PartDesign::SolidBody`, not a feature inside it and not the Body (sec 3) -- with mapped
  element names: faces, or edges and vertices (a line element, a hole's rim, a corner; 10.14). A
  face a later Pad modifies is still found through the element map; pointing at the Pad that
  made it would freeze it at that step.
- `Kind`: `Integral` (the faces themselves), `Axis`, `CentrePlane`, `CentrePoint`, `Apex`,
  `Group` (several faces as one: "2X", a pattern), `AllAround`, `Between` (from X to Y). The
  derived kinds compute their geometry at recompute from the faces (a cylinder's axis, two
  opposed planes' median plane) and hold it in `Shape`, so a dimension can measure to an axis
  and STEP gets its `derived_shape_aspect` + constructive geometry [A7].
- `Size`: whether it is a feature of size (a hole, a boss, a slot width) -- computed from the
  faces, it decides whether M/L modifiers are legal on tolerances that reference it.
- `State`: `Ok`, `Remapped` (found, but by history or geometry, not by name -- the user should
  look), `Broken`. `SavedGeometry` keeps the last faces it resolved to, so a broken feature is
  still drawn where it was, in red, and `Reattach` is one command (section 7).

Every annotation on the same faces shares one PMI::TolerancedFeature; the tools look for an existing one
before making another (STEP's uniqueness rule, [A7]).

### 4.3 PMI::Datum and PMI::DatumReferenceFrame

- `PMI::Datum`: `Name` ('A'), `Feature` (a PMI::TolerancedFeature), `Targets` (optional: point, line,
  circle, rectangle, area, each with position and size, `Movable`), and the simulated datum
  (plane/axis/point) computed into `Shape` for display and for datum-system orientation.
- `PMI::DatumTarget` (built 2026-11-20, 10.11): each target is its own callout, not an entry of a
  list -- in STEP each is its own shape aspect with its own presentation, and a drawing gives each
  its own symbol and leader. `Number`, `TargetType` (point, line, rectangle, circle, area),
  `Target` (a placement: its centre, z out of the part, x along a line or a rectangle's length),
  `Length` (a circle's diameter), `Width`, `Movable`, `Feature` (the face it lies on; an area
  target is that face). The datum lists its targets in `Targets` and gives them their letter; a
  datum may have targets and no feature. Its `Shape` is then its targets, until the simulation of
  a datum from targets is built.
- `PMI::DatumReferenceFrame`: `Compartments`, up to three, each a datum or a common datum with its
  modifiers. Stored as a string list in the frame's own notation -- `["A", "B(M)", "C-D"]` --
  parsed and validated by the object: readable in the file (the save-as-directory/VCS intent),
  editable from Python, and expressions work on it. One system per distinct frame: the tools
  reuse a matching one.

### 4.4 PMI::Dimension

- `Type`: size (linear, diameter, radius, spherical, angular, thickness), location (linear
  between two features, angular, with path), or `Basic` (a TED: exact, in a box, no tolerance).
- `Features`: one PMI::TolerancedFeature for a size, two for a location.
- `Nominal`: **measured from the geometry** at recompute -- a dimension cannot disagree with
  the model. `Driver` (optional): an expression to the parameter that makes it (a Hole's
  `Diameter`, a Pad's `Length`); when set, the check (section 8) reports when the two part.
- Tolerance: `Upper`/`Lower` (+/-, unequal allowed), or `Limits`, or `Fit` (ISO 286, "H7"); the
  ISO 14405 size modifiers (`LP`, `GG`, `CT`, ...) as a list; `DecimalPlaces`.
- TechDraw's `DrawViewDimension` already has +/-, basic and format specs [B3]; section 9 makes a
  drawing dimension a view of one of these.

### 4.5 PMI::GeometricTolerance -- the feature control frame

- `Characteristic`: the 14 (12 under Y14.5-2018; the two are refused there).
- `Feature`: the toleranced PMI::TolerancedFeature.
- `Zone`: `Shape` (none, DIA, S-DIA), `Value` (a quantity, expressions allowed),
  `Modifiers` (M, L, F, P + `ProjectedLength`, T, U + `UnequalValue`, ST, CZ, ACS, LE, NC,
  LD/MD/PD, ...), `MaxValue`, `UnitBasis` (per length / area).
- `DatumSystem`: a PMI::DatumReferenceFrame, or none (form tolerances).
- `Composite`: a link to the upper frame this one is a lower segment of; `Stacked` is the
  default (independent). OCCT does not carry composite frames today [B2] -- section 5.3.

The enumerations follow OCCT's `XCAFDimTolObjects_*` where they exist (so import/export is a
table, not a translation) and CAx-IF Tables 13-15 where OCCT has gaps [A6, B2].

### 4.6 What PartDesign features contribute

A Hole already knows its thread class and fit [B4]; a Pad its length. Two ways in, both on top
of the objects above rather than instead of them:

1. **Recognise** (later, section 10): "Tolerance from features" creates PMI::TolerancedFeatures and
   dimensions from the Body's features -- holes become size + position, patterns become groups
   -- the way DimXpert recognises model features before topology [A1].
2. **Drive**: a dimension's `Driver` binds it to the parameter, so editing the Hole's diameter
   in the task panel updates the dimension, and the check flags a dimension whose nominal and
   driver disagree.

## 5. STEP

### 5.1 Import

OCCT already fills `XCAFDoc_DimTolTool` [B1]. After `ImportOCAF2` has made the shapes, a new
pass walks it:

1. For each referenced shape label, find the sub-shape in the imported object and its element
   name -- the `findShape()` mapping `scanElementColors` uses [B1].
2. Create the PMI::AnnotationSet, PMI::TolerancedFeatures (sharing per face set), datum features, datum systems,
   dimensions and tolerances from the `XCAFDimTolObjects_*` objects.
3. Presentation: OCCT gives each item a plane, a text point and a presentation shape. Keep the
   imported shape as the item's graphic, flagged `UseImported`, until the user edits the item;
   then the graphic is regenerated from the semantics (section 6). Graphic-only PMI (AP203/AP214,
   or AP242 without semantics) becomes `PMI::Note`s that carry only the shape.
4. Saved views (`XCAFDoc_ViewTool`) become PMI::Views.

Things to check before trusting it: the streamed import running OCCT's GD&T pass once per batch
may duplicate labels [B1]; OCCT skips datum targets on read [B2]; the reported null tolerance
values from NX/NIST files in 7.8/7.9 [A8]. The NIST CTC/FTC/STC files are the test set, and SFA's
"PMI Representation Summary" the expected counts [A7].

### 5.2 Export

1. `ExportOCAF2` adds sub-shape labels for every face a PMI::TolerancedFeature references (it already does
   for per-face colours [B1]).
2. Datums, dimensions and tolerances go in with `AddDatum`/`SetDatum`, `AddDimension`/
   `SetDimension`, `AddGeomTolerance`/`SetGeomTolerance`/`SetDatumToGeomTol`.
3. Presentation: our generated graphic (section 6), tessellated, on its annotation plane --
   what the CAx-IF practice and OCCT's writer both want [A7, B2].
4. **The schema is AP242 whenever the document has PMI**: OCCT writes semantic GD&T only with
   `write.step.schema` = AP242DIS, and the legacy AP214 form otherwise [B2]. The export says so
   rather than quietly writing AP214 GD&T. (Decision 3.)
5. Validated by round-trip (export, re-import, compare objects) and by SFA on the output.

### 5.3 OCCT fork work (occt, LinkVibe-801)

The survey found five gaps [B2], in the order the phases need them:

1. Datum targets skipped on read.
2. The streamed-import duplicate risk (fork code: our root-range transfer).
3. Composite frames (`geometric_tolerance_relationship`) neither read nor written, no field in
   `XCAFDimTolObjects_GeomToleranceObject`.
4. Saved views not written to STEP (`model_geometric_view` + `camera_model_d3`).
5. The presentation curve colour hard-coded white; `writeDGTsAP242` ignoring its label list.
   The colour is settable on the fork (`SetGDTPresentationColor`, 2026-11-21, sec 10.17); the
   label list is not needed while an export's XCAF document holds only what it exports.

Found reading the NIST set (sec 10.9), and fixed on the fork (2026-11-20):

6. A typed `SET_REPRESENTATION_ITEM((#a,#b))` read as one null item (`RWStepAP242`): a datum
   feature on a hole in two half cylinders lost its faces.
7. A datum's faces found only through a `geometric_item_specific_usage` on its datum feature
   (`STEPCAFControl_Reader`): not through the plain `item_identified_representation_usage`, not
   on a shape aspect the datum feature relates to, not on the datum itself. Of the set's 754
   datum labels 224 had no shapes after 6, 16 after 7.

Still open: `dimensional_size_with_datum_feature` is not an entity the reader knows (STC-07's
datum features). Datum targets are read and made (item 1, sec 10.11).

Found writing (sec 10.17), and fixed on the fork (2026-11-21):

8. A graphic-only item -- a `CommonLabel` or a `DimensionPresentation` -- written without its
   annotation plane: `writeDGTsAP242` passed `HasPlane()` as "has semantics" and `false` as "has
   a plane".

Worked around, not fixed: `GenerateTessellatedCurveSet` writes a curve that is not a line as
its B-spline's poles -- the control polygon, not the curve -- and reads an edge's curve without
its location. The exporter hands OCCT polylines and meshes instead (sec 10.17).

## 6. Presentation

### 6.1 One layout, many outputs

A `PMI::Layout` in PMI/App (no Qt, no Coin) turns an item into 2D strokes and fills in its
annotation plane's coordinates: the frame's boxes, the characteristic glyph, DIA, the value,
modifier circles, datum letters, the datum triangle, leaders and arrowheads. Its text uses a
built-in **stroke font** (ASCII plus the GD&T symbols, ISO 3098 lettering), not a system font,
because:

- STEP needs text as strokes or triangles, and the export must work headless [A7];
- the browser viewer and the bgfx renderer get the same lines as the desktop, through the
  served scene, with nothing font-specific to port (browser/mobile portability);
- the drawing and the model then agree by construction -- the NIST mismatch class [A9.2].

Consumers: the 3D view (Coin lines, which the render cache already feeds to bgfx), the STEP
exporter (tessellated curve sets), and TechDraw, which can draw from the same layout or with its
own Qt text -- the frame's structure, not its pixels, is what must match.

### 6.2 Annotation planes and placement

Each item has `Plane` (a placement: x = reading direction, z = toward the reader [A7]),
`Position` (in the plane), and `Leaders` (each an anchor -- the element and a parameter on it --
and optional bends). A plane is one of the set's `Planes`, which are driven (by a face, a datum
plane, or one of the six standard views) or frozen [A2]. The defaults: the plane of the
toleranced face if planar, else the standard view that looks most squarely at it. The text
always reads from the front: seen from behind, it is drawn mirrored back.

### 6.3 Views, and annotating in one

`PMI::View`: a camera (incl. zoom), a member list (which annotations show), which objects show,
and optional clipping -- what SolidWorks calls a 3D view, CATIA a capture, STEP a saved view [A1,
A4, A7]. Annotations are owned by the set and referenced by views, so one datum shows in several.

The working style is upstream's [A8], and every commercial program's: the set offers the
standard views (Front, Top, Right, Isometric) and the user adds their own (Section A-A).
Double-clicking a view **activates** it -- camera set, clipping applied, objects and annotations
filtered, its plane shown -- and an annotation made while a view is active joins it and takes its
plane by default. Deactivating (or leaving the workbench) restores the camera and clipping; the
annotations stay visible, with a global toggle in the View menu. With no view active everything
shows and new annotations take the default plane of 6.2.

A view is active in one 3D view, not in the document: each 3D view keeps its own, so two show two
PMI views side by side, and **Show in new 3D view** (the view's context menu, `PMI_ViewNewWindow`,
or a double click with the `DoubleClickNewView` preference) opens one beside the others. A
browser's view of a served document is a view too: a callout's **PMI views** submenu makes one
active there, changing nothing of the document, so any connection may (10.16).

### 6.4 Several annotations on one feature

A size dimension, its position tolerance and a datum on one hole are one stack, not three
floating labels [A8, its 5.18]: an item may be `AttachedTo` another, drawn below or beside it
with the parent's leader, and moved with it.

### 6.5 In the 3D view

A view provider per item draws the layout through a `So3DAnnotation`-style node (screen-facing
size option, depth-tested or always on top), with pre-selection and selection highlighting both
the frame and its faces (cross-highlighting is what makes PMI checkable by eye). States colour
it: broken red, remapped amber, check failures with a mark. Visibility follows the PMI::View
active in the 3D view drawing it, or all. The served scene carries the layout's strokes like edges. A frame, a
datum's box, a dimension's value turn per frame to read upright from wherever they are seen,
as a sketch's datum label does; leaders and extension lines stay (10.15). Each fill is rimmed
by a one-pixel line (2026-11-21): a glyph's strokes, a third of a millimetre at a 3.5 mm
height, are thinner than a pixel once the view is out a little, and a fill that thin is
rasterized in dots -- the lettering was unreadable in every screenshot of a whole part, where
a STEP file's graphic, all lines, read fine. `pmi-workbench.py` checks every fill has its rim.

## 7. Surviving edits

On every recompute of the part, each PMI::TolerancedFeature resolves its references in order:

1. by mapped name -- the normal case, the element map doing its job;
2. by history (`getElementHistory`), when the name changed but the element descends from one it
   had: a split face resolves to its pieces, which the feature takes as a `Group` and marks
   `Remapped`;
3. by geometry against `SavedGeometry` (same surface type and position within tolerance), as
   TechDraw's `fixByName` does [B3], marked `Remapped`;
4. otherwise `Broken`: drawn where it was, red, listed in the check, waiting for `Reattach`.

**Nothing is resolved while the solid is rolled back** -- its tip set to an earlier feature to
edit there: its faces are not the part's then, and a repair against them would be written back
and survive the roll forward. The feature keeps what it had, and resolves again when the tip
is back at the solid's last feature.

Nothing is deleted. Creo's split [A2] applies: a broken *placement* (a leader anchor) only moves
the leader to the feature's nearest face; a broken *semantic* reference is the error.

## 8. The check

A `PMI::Check` (App, so headless and testable) runs on recompute and on demand, reporting into the
task panel and the tree:

- syntax: characteristic versus zone shape (DIA on flatness), modifiers versus feature (M/L on
  a feature that is not of size), datums on form tolerances, a datum system on position, legal
  symbols for the `Standard`;
- references: datum letters used but not defined, defined twice, a datum system whose frame
  does not constrain what the characteristic needs (orientation needs a primary);
- model: a broken or remapped feature, a dimension whose nominal and `Driver` disagree, a basic
  dimension with a tolerance;
- coverage (later): faces of the part no annotation or the general tolerance covers -- the
  "fully toleranced" question DimXpert's colours answer [A1].

## 9. TechDraw

The 3D items are the master; TechDraw shows them (decision 4).

1. **Import Model Annotations** on a DrawViewPart, built the way Import Edge Labels is [B3]:
   for each PMI item whose plane is parallel to the view (or every item, if asked), find its
   faces and edges among the view's projected geometry by their source names (`getSource3d()`,
   `;HLRF:` faces), and create or refresh a 2D object linked to it. Items whose features are not
   visible in the view are left out. Re-running it updates and keeps the user's moves
   (`isMoved`, `ImportOffset`, as `DrawEdgeLabel` does).
2. **`DrawFeatureControlFrame`** (new): a frame with a leader, linked to a PMI::GeometricTolerance via
   `Source`; its content comes from the source. Unlinked, it holds its own data, so a drawing
   without a 3D model still gets real frames instead of balloons with glyphs [B3]. Built on the
   leader + tile structure of the weld symbol [B3].
3. **`DrawDatumSymbol`** (new): the datum triangle and letter, linked to a PMI::Datum.
4. **Dimensions**: `DrawViewDimension` gains `Source` (a PMI::Dimension). Linked, its tolerance,
   basic flag and decimals come from the source, its references from the source's features
   projected into the view; editing the tolerance in the drawing writes to the source, so there
   is one value.
5. A drawing's general-tolerance note can read the PMI::AnnotationSet's `GeneralTolerance`.

## 10. Phases

Each phase ends green on the full suites and with its own tests.

- **P0 Model** (headless, this branch first): section 10.1.
- **P1 Authoring**: the layout + stroke font (6.1); 3D view providers (6.5); planes, views and
  their activation (6.2-6.3); stacking (6.4); task panels -- an FCF builder with a live preview,
  face picking in the view, the tolerancing advisor offering only the characteristics the
  picked faces admit; the PMI workbench and its ToolContext in PartDesign; the query tool
  (pick a face, list what applies to it); GUI tests.
- **P2 STEP import**: the DimTolTool pass, imported presentation, views; the NIST set;
  OCCT fixes 5.3.1-2.
- **P3 STEP export**: AP242 semantic + tessellated presentation; round-trip and SFA; OCCT fix
  5.3.5.
- **P4 TechDraw**: Import Model Annotations, the FCF and datum objects, linked dimensions,
  and a page derived from chosen PMI::Views (one TechDraw view per PMI view: its direction, its
  objects, its clipping as a section, its annotations imported) [A8, its 5.22].
- **P5 Rest**: PMI::Views to STEP and composite frames (OCCT 5.3.3-4), recognition from
  features, the coverage check, general tolerances per ISO 22081, tables, the served scene and
  browser viewer, Assembly location tolerances; later surface texture (ISO 21920), welds, edges
  (ISO 13715), JT / 3D PDF / QIF.

### 10.1 P0: the model, headless

`src/Mod/PMI/App` (library `PMI`, Python `import PMI`), `BUILD_PMI` ON by default; no Gui yet.

| Object | Base | P0 properties |
|---|---|---|
| `PMI::AnnotationSet` | `App::DocumentObjectGroup` | `Part` (hidden link), `Standard` (ISO GPS, ASME Y14.5-2018, ASME Y14.5-2009), `GeneralTolerance`, `DecimalPlaces`; takes only PMI objects |
| `PMI::TolerancedFeature` | `App::DocumentObject` | `References` (LinkSubList to the solid body), `Kind`, `Shape` (resolved faces or derived axis/plane/point), `SavedGeometry`, `IsSize`, `Condition`, `ConditionMessage` |
| `PMI::Datum` | `App::DocumentObject` | `Letter`, `Feature`, `Shape` (the simulated datum) |
| `PMI::DatumReferenceFrame` | `App::DocumentObject` | `Compartments` (string list, "A", "B(M)", "C-D") |
| `PMI::Dimension` | `App::DocumentObject` | `Type`, `Features`, `Nominal` (measured), `Upper`, `Lower`, `Fit`, `Basic`, `Modifiers`, `DecimalPlaces` |
| `PMI::GeometricTolerance` | `App::DocumentObject` | `Characteristic`, `Feature`, `ZoneShape`, `Value`, `MaterialCondition`, `Modifiers`, `ProjectedLength`, `MaxValue`, `DatumSystem`, `Composite` |
| `PMI::Note` | `App::DocumentObject` | `Text` |

Plain document objects, not `Part::Feature`s: an annotation is not geometry, and must not be
picked up by exports, bounding boxes or a Body's tip. Enumerations follow
`XCAFDimTolObjects_*` (4.5). Module functions: `PMI.check(set)` -> list of (object, severity,
message), section 8; `PMI.findFeature(set, part, subs)` -> the existing toleranced feature on
those elements, or none (4.2's sharing rule).

P0's reference resolution is section 7's step 1 (by name, which the element map and `ShadowSub` already do on
recompute), step 3 (by geometry against `SavedGeometry`) and step 4 (broken, kept); step 2 (history)
follows in P1. Measurement in P0: diameter/radius of a cylinder or sphere, the width of two
opposed parallel planes, distance plane-plane, axis-axis, plane-axis, angle plane-plane.

Tests (`PMITests`, registered in `FreeCAD.__unit_test__`): building a set in a Body; sharing;
every check rule, both ways; editing the Body under its annotations -- length change, a fillet
renaming faces, a face removed -- and the states that follow; measured nominals; save and
reopen; a second solid; a rollback; an edit not rebuilding the Body's dependents.

### 10.2 P0 progress (2026-11-19)

Built: `src/Mod/PMI/App` with every object of 10.1, `PMI.check`, `PMI.findFeature`,
`PMI.findSet`, and `PMITests` (22 tests: the model, every check rule both ways, the repair).
What building it taught:

- **Step 2 (history) is P0, not P1.** A fillet on the top face's edges leaves the top and the
  side faces with new names (`Top;:H..,F` becomes `Top;:M;FLT;...`), and the link marks both
  missing. `Part::Feature::getRelatedElements(body, oldName)` finds the face each became; a face
  a later feature swallows returns nothing, which is the Broken case.
- **The link re-finds a missing name by geometry itself** (`searchElementCache`), silently. A
  feature whose name changed to one its history does not lead to is reported Remapped "by
  geometry" -- without that, a part rebuilt with new features read as Ok.
- **Derived properties must not be outputs.** With `OptimizeRecompute` (the default) a link sees
  only the linked object's non-output revision (docs/InputProperties.md sec 2): with `Shape` an
  output, a Body edit recomputed the toleranced feature and stopped, and the dimension on it kept
  its old nominal. `Shape`, `IsSize`, `Condition` and `Nominal` are read-only, not outputs.
- **A new object with nothing touched is skipped** by the same optimisation; every annotation
  calls `enforceRecompute()` when it is made.
- **OCCT 8 builders**: `BRepBuilderAPI_MakeEdge(...).Edge()` no longer converts to a
  `TopoDS_Shape`; `.Shape()` does.
- **A property named `State` hides every object's status list** in Python (`obj.State`, where
  "Touched" and "Invalid" are read); the annotations' is `Condition`.
- **Reworked the same day into the Body** (sec 3): the second-solid jump, the rollback freeze,
  the Body keeping its shape, letters per solid -- 27 tests. Traps on the way: the Body's `Shape`
  holds a *copy* of the tip's shape, so the comparison is with the tip shape taken last; a
  located tip is moved into place by a copy each time, so the comparison is before the move;
  each `getShape()` locates afresh and locations compare by identity, so transforms compare by
  value -- and `gp_XYZ::IsEqual(other, 0.0)` is false for equal values in OCCT 8.

### 10.3 P1 progress: the layout (2026-11-20)

Built, headless: `PMI::Font` (osifont, the ISO 3098 font already in the tree, read with
FreeType -- no Qt -- glyphs cached per height as planar faces and placed by translation, since
OCCT 8 refuses scaling locations), `PMI::Drawing` (strokes, fills, the 14 characteristic symbols
drawn as strokes -- osifont lacks perpendicularity and parallelism -- circled modifiers, ISO
arrowheads), and `PMI::Callout`, the base of the tolerance, the datum, the dimension and the
note: `Plane`, `TextHeight`, `AutoPlace`, `ShowLeader`, the generated `Presentation` and the
CAx-IF `EquivalentText` (e.g. "position|DIA0.1(M)|A|B|C(M)" in its code points). 37 tests.

- **Default plane** (6.2): a planar face's own, a little above it; an axis looked at along
  itself, its leader to the end toward the reader (first try went to the axis's middle, inside
  the part -- seen in a rendering, not a test); otherwise the standard view most square to the
  surface at the target.
- **Stacking** (6.4, first step): callouts landing together on parallel planes take slots of
  three text heights, in the set's order, so the result does not depend on recompute order.
- A callout moved off its feature's plane draws its leader to the target's projection (CAx-IF
  sec 8.3).
- Seen, not yet done: dimension and extension lines for location dimensions (a leader for now),
  and annotations stacked onto one leader.

### 10.4 P1 progress: the GUI (2026-11-20)

Built: `PMIGui` -- a view provider drawing each callout's `Presentation` on top of the model
(`Gui::So3DAnnotation`, which the render cache takes), blue, amber when remapped, red when
broken or invalid; a toleranced feature's derived axis or centre plane drawn dashed (upstream's
construction geometry); the annotation set, whose double-click makes it active in the view and
offers the PMI tool bar in PartDesign (`Gui::ToolContext`, key "pmi"). The PMI workbench and
commands Datum, Geometric tolerance, Dimension, Note, Check: each resolves the picked faces in
C++ (`PMI.solidReference`) and writes plain Python with the names in it, so a macro replays it.
The defaults: a plane's tolerance is flatness, a cylinder's a position of its axis in a diameter
zone, anything else profile of a surface; a dimension on a hole its diameter, on two opposed
planes the width, on two features their distance or angle. Edited in the property view for now
(the FCF builder panel came next, sec 10.5). GUI test `tests/gui/pmi-workbench.py`, 14 checks.

- **The first pick on a Body without a set cannot be named** -- the solid body comes with the
  set. The command makes the set first, inside its transaction, then resolves the pick. Found as
  a hang: the refusal was a message box, which hangs Xvfb; the test now closes modals and fails.
- **A size is measured on the faces, whatever the feature derives**: a hole's feature shared
  with a position tolerance is its axis, and its diameter came out 0 (found by the GUI test).
- **A callout under the part was half hidden by it** (datum A on the bottom face, looked at
  from above): the render cache draws an `So3DAnnotation` on top only with a priority other than
  0. The view provider sets 1.

### 10.5 P1 progress: the frame builder (2026-11-20)

Built: `PMI::Advisor` (App) -- per kind of feature (plane, width, hole or axis, sphere, cone)
the characteristics that apply, the usual first; the zone shapes each allows; whether M/L may be
given; whether datums are needed, optional or forbidden. `PMI.advise(feature)` from Python.
`PMI.frame(set, ["A", "B(M)"])` finds the set's datum reference frame with these compartments,
or makes one: one per distinct frame, as STEP has it. The builder panel
(`TaskGeometricTolerance`) opens when the tolerance command runs and on a double-click: the
advised characteristics, zone, value, material condition, F/T/CZ/ST/P with the projected length,
three datum compartments with their M/L. Each change is written at once, so the frame in the
view follows, and what the check finds is shown under it; OK commits, Cancel undoes it all.

- **Every tolerance the advisor offers passes the check** (`TestAdvisor`, on a plane, a hole
  and a width): the two cannot disagree.
- **A leader to a face with a hole in it pointed at the hole**: its end was the point of the
  face nearest the box centre, on the hole's rim. On a planar face it is now the box centre if
  that is in the material, else the point of a 24 x 24 grid deepest inside the face (farthest
  from its edges). With `BRepTopAdaptor_FClass2d`, classified once per face, a recompute costs
  3.6 ms; `BRepClass_FaceClassifier` per point cost 34.
- GUI test: 20 checks -- the builder offers what a hole admits, datums A and B with MMR make
  `|A|B`, no problems found, Cancel puts the value back.

### 10.6 P1 progress: linear dimensions, stacks (2026-11-20)

Built: a width or a distance (two parallel planes, a plane and an axis, two parallel axes, a
point and anything) is drawn as a drawing draws it -- extension lines from both sides, a
dimension line with arrows (from outside, pointing in, where it is narrower than 2.5 h), the
text over its middle -- in a plane along what it measures: the standard view (top, front,
right) most square to it, read from the bottom or the right, beside the part on the view's up
side; no leader. `Callout::autoPlane()` is now virtual, so the stacking slots of sec 6.4
count each callout where it really lands. A plane moved off the measured direction falls back
to text and leader.

`AttachedTo` (sec 6.4): a callout stacked under another takes its plane, sits under its
drawing with the left edges in line, has no leader of its own, and moves with it. A link to
anything but a callout, or one making a loop, is refused at once with a warning. The commands
stack by default: `PMI.stackUnder(callout)` is the set's last callout on the same feature
(never a linear or angular dimension), written into the macro as
`_obj.AttachedTo = PMI.stackUnder(_obj)`. A hole's diameter made after its position frame
reads under it.

- Seen in the iso capture: datums A (bottom face) and B (front face) land close together near
  the front corner; the default offsets of different planes do not see each other yet.

### 10.7 P1 progress: views (2026-11-20)

Built: `PMI::View` (sec 6.3) -- a camera (`Camera`, a placement whose z points at the viewer;
`Height`, 0 to fit; `Perspective`), a `Standard` (front, top, right, isometric) that drives
the camera and turns to Custom when the camera is turned by hand, `AnnotationPlane`, and a
clipping plane (`Clip`, `ClipPlane`). Membership is on the callout: `Views`, a link list;
`members()` reads it back. A callout's first view with `AnnotationPlane` set gives its default
plane -- the view's, just in front of the feature -- and a linear dimension takes the view's
frame when the view's right runs along what it measures, else falls back to a standard one.

In the GUI the view is activated as `pmiview` (a double-click, or `setActiveObject`): the
camera goes to the view's, its clipping is applied, and only its members are drawn (a switch
over each callout's drawing). Let go, the camera and clipping come back as they were and
every callout shows. One view is active per document; activating another in the same 3D view
does not restore in between. The `PMI_View` command makes a view from the camera as it is,
in the active set (or the selection's, or the active Body's), and activates it; an annotation
made while a view is active joins it and lies in its plane.

- A callout changes plane at the recompute after its `Views` change, as for any property.
- Not done: the object filter of sec 6.3 (`Objects` -- which bodies a view shows). Closed
  since, 10.15.

### 10.8 P1 progress: what applies to a face, cross-highlighting (2026-11-20)

Built: `PMI.query(obj, subname)` (`Api::query`) -- a face, picked on the Body or named on its
solid, gives the callouts on a feature holding it ("on"), then the tolerances whose datum frame
cites a datum on it ("datum A", the letters being the solid's own); in the set's order, each
once. `Api::featuresOf` is a callout's features, a dimension's several.

Cross-highlighting (sec 6.5), one selection observer for every callout, worked out once the
selection settles: a callout selected or preselected outlines its faces (their edges, followed
within 0.2 % of their size, drawn through the model); a face selected or preselected lights the
callouts that apply to it in the selection colour. Only a part that has a set is looked at, so
hovering over other parts costs nothing. The outline lives under the callout, and FreeCAD's
whole-object selection recolours what is under a selected object: it shows light blue, not the
selection green -- distinct from the face highlight, which is kept.

`PMI_Query` opens a panel that follows the pick: the face, and what applies to it with its
text and how. A pick of no face leaves the list, so clicking a callout in it selects it --
outlining its faces -- and the list stays.

- Still open in P1: a view's object filter (sec 6.3, `Objects`; closed since, 10.15), and the
  default offsets of callouts on different planes not seeing each other (sec 10.6).

### 10.9 P2 progress: the GD&T of a STEP file, read (2026-11-20)

Built: a pass after `ImportOCAF2` has made the shapes (sec 5.1), the same whether the file is
read at once or progressively. `ImportOCAF2::addPostImport` registers a pass by name; Import
does not link PMI, so a file with GD&T and no pass registered imports PMI first. Each pass gets
the XCAF document and a way from a shape label to the object made for it; the PMI pass is
`PMI::Step::read` (`App/StepImport.cpp`).

- **Faces**: a sub-shape label's owner gives the object, the face's index in the object's shape
  (unplaced) its name. All of one annotation's faces must be on one object; when they span
  objects, faces with no surface (a file with the geometry also tessellated names each face on
  both) and faces off the solid (supplemental surfaces beside it, FTC-10) are set aside first.
- **Where the set goes**: beside the shape, in its `App::Part` when it has one -- `getSet` now
  asks for a GeoFeatureGroup before a plain group; outside the Part the features linked into it
  out of scope.
- **Datums**: OCCT makes a datum label for each time a frame cites a datum, carrying the
  feature's faces when it found them; one datum is made per letter and faces. A letter no label
  gives faces is reported once ("only frames cite it"); datum targets are counted per letter
  and not made yet.
- **Tolerances**: characteristic, zone (diameter, spherical), value, maximum, material
  condition, the modifiers OCCT reads, projected length; the frame from the datums' positions
  and modifiers, through `Api::frame`. A position, coaxiality or concentricity, or a diameter
  zone, on a cylinder or cone takes its axis.
- **Dimensions**: diameters, radii, spherical sizes, thicknesses (as widths), angular sizes,
  angles and linear distances. OCCT keeps the lower deviation as a magnitude (its writer
  negates it), so it is negated; a range becomes deviations from the value. The nominal is the
  model's: a file's value the model does not measure is reported, and the model's kept.
  "Dimension presentation" labels, and ones with no type and no value -- the file gives them
  only as a graphic -- are counted, not made.
- **Placed as the file places them**: the annotation plane with the text point as origin,
  `AutoPlace` off; the text height half the file's smallest frame (ASME: a frame is twice the
  text), so a part a metre long is not lettered at 3.5.

The model learnt three things the files use: a pattern of holes of one size ("4X") is a feature
of size; zero is a tolerance at MMC or LMC; a radius may be a fillet's, on a torus (`Primitive`
gains `Torus`). A size is now measured on the first face of its kind, so a blind hole's diameter
is its cylinder's, not its drill point's cone.

Tests: `PMITests/TestStep.py` reads NIST CTC-01 (in the tree, public): datums A, B and C (B
and C each on two half cylinders: the fork fix of sec 5.3 item 6), the six tolerances and the
eight dimensions with value as its drawing gives them, every reference on the solid, placed and
lettered as the file does. `TestNistSet`, run when `FC_PMI_NIST` names the set, reads all 17
AP242 files: each gives GD&T (but `-tg`, whose PMI is only tessellated graphics) and nothing
broken. `tests/gui/pmi-step.py`: through ImportGui, at once and progressively, the same as
headless, none invalid, drawn.

The NIST set's ledger, what is not read or is read invalid, all 17 files (2026-11-20):

| What | Count | Why |
| --- | --- | --- |
| datum targets | 63, of 8 datums | read since: 15 targets, 10.11 |
| datums only cited | 4 (B, C, G, H) | FTC-06's two, STC-07's two (sec 5.3) |
| references to edges or points | 22 | read since: 37 features on edges, none on a point, 10.14 |
| a second feature with no shape | 1 | the file gives none |
| curve lengths | 10 | no dimension type for them |
| widths on other than two planes | 11 | thickness used for a depth, or a width across several faces |
| MMR on no feature of size | 3 | slots and counterbores: read since, 10.12 |
| graphic-only dimensions | 185 | presentation, not read yet (sec 5.1 step 3); read since, 10.10 |

### 10.10 P2 progress: the file's graphics, graphic-only PMI, saved views (2026-11-20)

Built, sec 5.1 steps 3 and 4:

- **Drawn as the file draws**: each callout read keeps the file's presentation shape in
  `ImportedGraphic` (read-only, hidden, moved into the set's coordinates) and `UseImported` on;
  while it is on, `Presentation` is that shape, not the one made from the semantics. An edit
  turns it off for good: the plane, the text height, what it is attached to, or any property
  of the PMI group a user sets. A recompute, a placement by the layout, or joining a view is no
  edit. The callout's strokes follow curves now (`ShapeLines`: a curve not a line is sampled
  to 0.5% of the graphic's size), as the files' symbols are arcs and circles.
- **Graphic-only PMI** (no type and no value, or a "dimension presentation") becomes a
  `PMI::Note` carrying the graphic, labelled with the presentation's name, on its feature when
  the file names one. Notes are made after everything else is read: a graphic the same as one
  read with its semantics (same edges, same extent) is that one's, given again -- CTC-01 gives
  its three datums', an angle's and a profile's so -- and makes no note; its label shows the one
  read. The note's `Text` is empty: what it says is in its graphic.
- **Saved views** become `PMI::View`s: the file's name, `Standard` Custom, the camera looking
  along the file's direction with its up, centred on the point of the line of sight nearest the
  part, the window height as `Height`, perspective when central. `AnnotationPlane` is off: the
  callouts keep the file's planes. The view's members are the callouts its labels resolve to,
  a datum label only a frame cites through its letter.

CTC-01: 21 callouts, all drawn as the file draws them; 4 notes (two texts, two sizes given only
as graphics), 5 graphics given twice; one view, `MBD_0`, showing all 21. Its capture matches the
NIST drawing. The NIST set's 185 graphic-only dimensions: 128 notes, 45 given twice, 12 with no
graphic (counted, nothing to carry); 59 saved views over the 17 files, none invalid.

Tests: `TestModel` (a file's graphic until edited; a view membership is no edit, a value is),
`TestStep` (drawn as the file draws, edited not; the notes, none a twin of a callout; the
view), `tests/gui/pmi-step.py` (the saved view activated: its camera, its callouts drawn).

- Next in P2: datum targets (10.11); slots and counterbores as features of size; edge and
  point references.

### 10.11 P2 progress: datum targets (2026-11-20)

Built, the model of sec 4.3, made by hand and read from STEP:

- **The model**: `PMI::DatumTarget`, a callout of its own; its datum lists it in `Targets` and
  gives it its letter (a new letter touches its targets). A datum with targets needs no
  feature; its `Shape` is then the targets' (a point, a centred line of its length, a
  rectangle, a disk, an area's faces), not yet a datum simulated from them.
- **Drawn** the ASME way: a circle split across, the label (`A1`) below, the size above
  (`DIA 85` as the diameter sign, `50.8X31.75`), a plain leader to the target. On the part,
  the target itself, in its plane: a point an X, a line a phantom line, a rectangle or a circle
  a phantom outline hatched at 45 deg; an area is its faces, highlighted as any feature.
- **Placed** in the active view's plane when it has one, beside the target; else in the
  surface, beside the target and clear of its size. A target has its own point: it is never
  stacked under a callout of the same face, nor takes one under it (two targets on one face
  each keep their leader).
- **The check**: a target is a datum's (listed in its `Targets`), numbered from 1, a line has
  its length, a circle its diameter, a rectangle both sizes, an area its faces; a datum has a
  feature or targets.
- **The GUI**: `PMI_DatumTarget` puts a point target where a face is picked, z out of the part.
  Picked with a datum selected, it is that datum's next; else a new datum takes the next letter.
- **Read from STEP**: OCCT gives a label each time a frame cites a target (the set's 63 labels
  are 15 targets); one target is made per letter and number. The file's axis is no frame to
  trust -- CTC-02 gives every point (0, 0, 1), FTC-06 its lines along their normal -- so the
  part gives it: z the outward normal of the face the target lies on (within 1e-3), a line
  centred on its edge and along it. The shape the file names is often loose geometry beside
  the part -- CTC-02's points and FTC-06's edges in an empty compound, CTC-02's K1 a disk of
  its own 20 from the solid -- so the face is looked for on the part's solid first, then on the
  shape the file gives. A letter only targets establish gets a datum with no feature. Drawn as
  the file draws them (10.10).

The NIST set: 15 targets in three files, of the 8 datums they establish -- CTC-02's A (three
points), B (four), C (one), K (a circle, DIA 85); CTC-05's C and D (rectangles 50.8 x 31.75);
FTC-06's J and K (two lines each, 31.75 long). Each is on a face of the part (K1 on its own
disk), none invalid; the rest of the ledger of 10.9 is unchanged.

Tests: `TestModel.TestDatumTarget` (a datum from three points, the letter the datum's, an area
or a rectangle marked within its size, the rules, saved and restored), `TestNistSet`'s
`test_datum_targets` (each datum's targets by type and number, each on its face),
`tests/gui/pmi-targets.py` (picked on a face; a new datum, then the selected datum's next; each
with its own leader; drawn).

Open: a line target on an edge lies on both its faces and takes the first found (the file's z
runs along the edge, nothing to choose by). A movable target has no symbol of its own yet. A
datum from targets is their compound, not the plane, axis or point they establish.

- Next in P2: slots and counterbores as features of size (10.12); edge and point references.

### 10.12 P2 progress: slots, counterbores and patterns of widths as features of size (2026-11-20)

Built: what a feature of size is (ASME Y14.5 1.3.32), decided in `Geom` and shared by the
feature, the width dimension and the advisor:

- **A width** (`Geom::width`): planes in pairs facing opposite ways, each pair one width apart
  -- those facing one way, moved the width along their normal, are those facing the other. One
  pair, or a pattern ("2X"), slots or tabs, side by side or in a row; a **slot** adds its round
  ends, half cylinders of half the width with axes across the planes' normal.
- **Holes** (`Geom::holes`): cylinders grouped by axis, every axis with the same radii -- a
  counterbore's hole and bore -- with the planes across the axes (the bores' floors) and cones on
  them (a countersink, a drill point); one, or a pattern. The hole, the smallest cylinder,
  stands for it: its axis is the feature's, whichever face is listed first.
- A **Width** dimension measures a slot or a pattern of widths; the Dimension command makes one
  when the faces picked together are a width (a slot's walls and ends), not a distance between
  the first two. The advisor calls a slot a width and a counterbored hole axial.

Faces facing the same way are a depth, not a width (STC-06's and STC-10's "widths" on a hole's
cylinders are `thickness` sizes on the shape aspect of a diameter: a hole's depth, still read
invalid -- a depth has no dimension type yet).

The NIST set: the three "MMR on no feature of size" are read now -- FTC-09's pattern of two
widths, STC-06's four counterbored holes, STC-09's slot. Tests: `TestModel` (a slot and two
slots' walls are of size and measure 4; two faces facing the same way are not; one and two
counterbored holes are, a counterbored hole and a plain one are not; the axis the hole's).

- Next in P2: edge and point references (10.14).

### 10.13 Found making the manual's media (2026-11-20)

The manual's PMI pages (thundereal-manual `partdesign/pmi`, a screenshot and a video script
per command, on a plate with counterbored holes and a slot) drove every command as a user
does, on a part whose features carry placements of their own. It found:

- **A datum target refused on any placed tip** (`bbf134dc4d`): a click reaches the face
  through the Body's tip feature, and `getGlobalPlacement(solid, root, sub)` -- the solid
  body not on that path -- applied the tip's own placement to the picked point, off the face.
  The path is followed to the solid's group, then the solid's placement applied. The GUI
  test's box sat at the origin; it now picks through a placed tip.
- **A target's datum lost when selected through a path** (`b35a1dc5cb`): a datum selected in
  the tree arrives as itself or as (Body, "PMI.Datum."), which a pick varies; only the first
  was taken out of the selection, and the face pick read the second.
- **A slot's tolerance a profile** (`bbf134dc4d`): the command's default looked at the first
  face only. A slot or a pattern of widths is now a position; a counterbored hole, whichever
  face comes first, a position of the hole's axis.
- **Callouts edge on from the top** (`6522ce5b96`, `bccbab4f81`): a slot's frame stood in a
  wall's plane, a hole's diameter in a plane facing out of the cylinder. Both are read
  looking down the slot or the hole now, as an axis already was (ASME: a size where it
  shows as a circle), the leader to the top.
- **Dimensions over one another** (`bccbab4f81`): a linear dimension went beside the faces it
  measures, so one between two holes' axes landed inside the part, over the part's length.
  It goes beside the whole part now, where dimensions landing together are slotted apart.

Still open from the same pictures: a planar callout seen from behind reads mirrored (a datum on
the end the standard isometric hides); FreeCAD's Bottom view turns a bottom face's callouts
upside down (it flips y). Both closed since, 10.15. In PartDesign a Body's first PMI set could
not be made: the tool bar was offered only once a set existed and was active. Closed: the Part
Design menu's PMI submenu, and an annotation made there making its set active (sec 3;
`GuiPMIWorkbench_tests_run`).

### 10.14 P2 progress: edge and point references (2026-11-20)

Built: a toleranced feature may be on edges and vertices of the solid, not only faces -- a line
element (ASME Y14.5 5.4.1: straightness of a line on a surface), a hole's rim, a corner, and
what a STEP file names as such (a dimension between two edges, a note on an edge).

- **The geometry** (`Geom::ofElement`): a straight edge is a `Line`, a circular one a `Circle`
  (centre, axis, radius), a vertex a `Point`; a face is what it was. `sameSurface` compares
  them (a line collinear, a circle of the same centre, axis and radius, a point coincident),
  so an edge is found again **by name, by history or by geometry** as a face is, each among
  its own kind: a face is never taken for an edge. `SavedGeometry` keeps edges and vertices as
  it keeps faces, so a broken one is still drawn where it was.
- **What they measure**: a distance between two edges, an edge and a face, or points; a
  circular edge stands for its cylinder -- a diameter or a radius, an `Axis` or a
  `CentrePoint` feature. A diameter on a straight edge is invalid ("a circular edge").
- **What they take**: the advisor's characteristics for a straight edge (straightness, line
  profile, orientations, position), a circular edge (circularity, position, line profile,
  circular runout) and a vertex (position); a rim's position zone may be a diameter.
- **The commands** take edge and vertex picks: a straight edge's tolerance defaults to a
  straightness, a vertex's to a position, a rim's to a position of its axis. One straight edge
  picked for a Dimension is refused with why: its length is not a dimension type yet.
- **The import**: a shape aspect's faces, or else its edges, or else its vertices, become the
  feature's references; a circular edge is axial, as a cylinder is.
- **Drawn**: a straight edge's callout is read side on, from the top, else the front, else the
  right, its leader to the edge's middle -- not along the edge to its end, as an axis is. A
  circular edge's is read down its axis, as its hole's is. The builder names a zone as the
  characteristic makes it: two lines for a straightness or a line profile, two circles for a
  circularity or a circular runout, two surfaces otherwise. Both were found making the
  manual's pictures.

The NIST set: every one of the files' edge references is read, 37 features in 10 files, all Ok,
none invalid. On them, 29 dimensions (CTC-04's on 60 edges, CTC-05's, FTC-06's -- two axes of
rims --, FTC-07's, FTC-09's four axes of rims, FTC-10's), 11 graphic-only notes and one of
CTC-05's tolerances. None is on a vertex. The ledger's invalid list is unchanged. Tests:
`TestModel.TestEdges` (an edge and a vertex are features; distances between edges, a rim's
diameter, a straight edge's refused; the advisor; picks; an edge elsewhere untouched and found
again after its feature is rebuilt, by geometry; saved and reopened), `TestStep` (CTC-01's two
notes on edges), `tests/gui/pmi-workbench.py` (two picked edges' distance of 20, a rim's
diameter of 6, an edge's straightness between two lines).

- Next: P1's leftovers (10.15), then P3, STEP export.

### 10.15 P1 closed: callouts read upright from every side; a view's objects (2026-11-20)

**Read upright.** A callout lies in its plane, so from behind it read mirrored and from below
(FreeCAD's Bottom view turns the camera about x) upside down. It turns now as a sketch's
datum label does, per frame, in its own plane:

- The App marks what turns: `Drawing::beginUpright()`/`endUpright()` around a part of the
  layout, else the whole layout before its leader (a frame, a datum's box, a note) -- a
  linear dimension marks its value alone, its extension and dimension lines staying.
  `Presentation` is then what stays (leader, lines, marks on the part) as its first
  sub-shape, and one sub-shape per part that turns; `UprightAt` (transient, rebuilt with
  `Presentation` on restore) holds where each turns about, in the plane.
- The view draws each part in a frame of its own under an `SoAutoZoomTranslation` with
  `datumFlip` on and `scaleFactor` 0 (the model's size, no zoom): the local X or Y that
  points left or down on the screen is mirrored, about the part's middle. A part's leader
  starts on its box's edge, which a mirror about the middle keeps.
- `datumFlip` is now done by Coin itself (`SoAutoZoomTranslation::doAction`), as bgfx did
  it: a pick, the GL render cache that replays the node and a direct Coin render agree. A
  zero `flipNormal` means the node's own local Z, so a Body's placement turns it too.
  The decision is now made from the screen alone (`SoAutoZoomTranslation::datumFlips`, a
  copy in bgfx): X reads to the right, or upwards on end -- anywhere from 80 deg below the
  screen's x to 100 above, so the cut lies off the vertical where views and text often sit
  exactly; then Y lies to the left of X on the screen, never mirrored; edge on, Y points up.
  The old test (projected axes, settled by a backfacing test on the normal) read float noise
  seen edge on and undid the mirror (from the Rear, 6 of 9 callouts on the top face), and
  left a vertical value reading downwards from behind.
- The render cache's box for a draw under such a node (`SoFCRenderCache::drawBoundingBox`)
  is carried through the node's matrix, turned every way it may turn: a fit framed the
  frames' strokes at the origin, where their local frame is.

**A view's objects.** `PMI::View::Objects` (a hidden link list: the view lives in the set
inside the Body it would name): while the view is active only they show, in the 3D view it
is active in -- every other object at the top of the tree hidden, and within a group holding
one it lists the rest of the group; the PMI stays. Empty shows all. The hides are a layer of
the view's transient visibility (`View3DInventorViewer::setVisibilityLayer`, owner
"PMI::View", beside an edit session's, which come first): never written into the view's
map, so a save sees none of them, and taken back when the view is let go. Hidden first,
then fitted, so a view fitting everything frames what it shows. A STEP saved view's shapes
(`XCAFDoc_ViewTool::GetRefShapeLabel`) become its `Objects` when they name a solid:
STC-10's 13 views name its solid; STC-09's name only a compound of construction curves,
which would hide the part, and are left showing all.

Tests: `tests/gui/pmi-workbench.py` (a Coin callback pass reads each turning part's local X
and Y on the screen from the Front, Rear, Top, Bottom, Left, Right and the isometric: none
reads backwards; with the turn off, 8 of 9 do from the Rear and from below),
`tests/gui/pmi-views.py` (a view listing the Body hides a box beside it in its 3D view only,
its Visibility untouched, back when let go and hidden again at once when made active),
`TestModel.TestPresentation` (what turns: a frame whole, a dimension's value only, a file's
graphic nothing), `TestViewObjects` (a view naming the Body it is in: no loop, saved and
reopened), `TestNistSet` (a saved view's objects include a solid).

- Next: a PMI view per 3D view (10.16), then P3.

### 10.16 A PMI view per 3D view, in a new one, and in a browser's (2026-11-21)

Until now a PMI view was active per DOCUMENT: one at a time, its callouts filtered by a switch
node every 3D view shared, while its camera, clipping and objects reached only the 3D view it was
made active in. A second 3D view saw the callouts filtered and the objects not.

**Per view.** What is active is held per view (`ViewProviderView`'s list of `Active`, keyed by
the `Gui::ViewerContext` and expiring with its `lifetime()`): a desktop 3D view, or a served
client's `MirrorViewer`. Everything a PMI view hides -- the callouts it does not show, and what
its `Objects` leave out -- is one layer of that view's transient visibility
(`ViewerContext::setVisibilityLayer`, now on the base, the desktop's and the mirror's alike), so
nothing is filtered in a view the PMI view is not active in, and a save sees none of it. The
callout's switch is gone. A callout made, or taken into or out of a view, remakes the layers.
On the desktop the 3D view's active object (`pmiview`) still says what is active, the tree
highlighting it; which 3D view changed is read from their active objects, not from which window
has focus.

**A new 3D view.** `PMI_ViewNewWindow` (also the view's context menu, "Show in new 3D view")
opens a 3D view of the document -- placed as an additional view is, a split by default --
titled after the PMI view, with the view active in it. Preferences, Part/Part Design, PMI:
"Double-click opens a PMI view in a new 3D view" (`Mod/PMI/DoubleClickNewView`, off); on, a
double click opens one, or brings up the 3D view the PMI view is already active in.

**A browser's view.** A browser has no tree: a callout's context menu has a **PMI views**
submenu, each view it is in made active or let go in the view the menu was opened in, plus "Show
all annotations". These entries carry `ViewProviderDocumentObject::ViewerEntryProperty`, a new
kind for `SceneContextMenu` (`viewer`): it changes only the view it runs in and nothing of the
document, so a view-only connection may run it, as it may turn its own camera (the rule is what
an entry changes). The client's camera is its own, so it is ASKED: the host sends
`{"cmd":"camera","q":[x,y,z,w],"at":[x,y,z],"height":h}` (`ViewerContext::turnClientCamera`,
`MirrorViewer` through the serving source's control channel); the browser keeps the camera from
before the first ask and `{"cmd":"camera","restore":true}` takes it back
(`restoreClientCamera`). Its camera has a fixed field of view, so a height becomes the distance
that shows it; 0 fits what the client shows. A browser's view does not clip (a section is not
streamed). "In a new 3D view" is the desktop's alone and is not offered to a browser.

Tests (10.16): `pmi-views.py` (two 3D views side by side, the new one, the preference),
`serve-pmi-view.py` (the wire, on a view-only connection), and the browser leg
`serve-pmi-view-browser.py` with `scripts/pmiview-drive.js` (not registered: node, puppeteer-core
and a Chrome). In headless Chrome the page's picture changes by 67% when the view is made active
-- top down on the plate, the datum gone -- and is the same to the pixel once it is let go. A
menu asked for from a script needs its ray to start at the page's eye: the server reads a ray as
the pixel of the client's camera it passes through. Under `.conda/limited.sh` Chrome needs
`LIMITED_MEM_GB=256`: it reserves far more address space than 24 GB, and dies at launch.

- Next: P3, STEP export (10.17).

### 10.17 P3: the GD&T of a part, written (2026-11-21)

**The pass.** `Import::ExportOCAF2::addPostExport` is the export's twin of `addPostImport`: at
the end of `exportObjects()` every pass gets the XCAF document and the label each exported
object went to (`PostExport`). The PMI module's (`PMI::Step::write`, `StepExport.cpp`) writes,
for every annotation set whose part was exported, its datums, datum targets, tolerances,
dimensions and notes into the DimTolTool, on sub-shape labels of the exported shape
(`AddSubShape`, as per-face colours are), each with its plane, its text point and its graphic.
`Import::WriterStep` then writes AP242 whenever the document has GD&T (`Tools::hasGDT`), whatever
the preferred schema, says so in the report view, and puts the preference back (decision 3).

**What maps to what.** The import's tables, inverted: a characteristic to its
`XCAFDimTolObjects_GeomToleranceType`, a zone shape to its type of value, MMR/LMR to the
material requirement, `P` and `U` to the zone modifiers with their values, the rest of the
modifiers to OCCT's; a dimension's type, its deviations (the lower one a magnitude, as OCCT
keeps it), its decimal places and modifiers. A frame's compartments become datum labels of
their own, each at its position with its modifiers, bound to the tolerance, as OCCT reads them
(sec 10.9); the datum features are written first, as OCCT's writer takes the first label of a
letter for the feature. A note is a graphic named by its text: a `DimensionPresentation` on its
feature, a `CommonLabel` without one -- what the import reads back as a note. Not written, and
reported as such: a basic dimension (OCCT has no theoretically exact dimension), a fit (H7: not
yet), a modifier with no STEP equivalent (`UF`, `OZ`); a composite frame's lower segment is
written as a frame of its own. Saved views (PMI::View) are P5's (sec 5.3.4).

**Which face is which.** The annotations refer to a solid body's faces (sec 3), and a Body's
shape, its solids' and its tip's are copies of one another: no TShape in common. An element is
found in the exported shape by its TShape, else by its element map name in the part's shape
(the names survive the copies), else by its place and size (centre and area, length, point) --
unique, or not at all. The export either keeps the part's placement in the file (the shape in
its own coordinates, a location on it) or bakes it into a copy (ExportKeepPlacement): both are
covered, the copy's elements found by their index in the part's placed shape.

**Where.** The annotations are in the set's coordinates -- the Body's own for a set in a Body,
else those the part is placed in -- and the file's in the exported shape's: the move from one
to the other is read off a face found in both. A graphic goes to OCCT as polylines and meshes:
its writer takes a curve that is not a line for its B-spline's control polygon and ignores an
edge's location (sec 5.3); a fill is a face of its triangles alone, so its edges are not written
twice. The graphics are in the callouts' blue, not OCCT's white; `Mod/PMI/ExportColor`,
Preferences, Part/Part Design, PMI, STEP export, names another.

**OCCT, fixed on the fork** (sec 5.3 items 5 and 8, and three found here):

- `SetGDTPresentationColor` on `STEPCAFControl_Writer`; white unless set.
- A graphic-only item written with its annotation plane (the two flags were swapped).
- `GetTessellation` put a null item in the set for a graphic with no curves (a note of fills
  alone) and for a face with no triangles: a model that cannot be written. Only the items made
  go in now.
- A graphic-only item on a part whose faces have no entity (a tessellated part, the NIST
  `-tg` file) wrote a draughting model item association of a null shape aspect, and the
  draughting model got no context: both "Null Reference" at write time. The item is the
  product's then, as a common label's is, and the model takes the product's context.
- `writeShapeAspect` returns nothing rather than a usage of a null item.

`WriterStep` now says what the writer's checks say when a write fails ("Null Reference"),
not "Cannot open file", which it said of every failure.

The import gained one thing: a graphic-only item with no feature and nothing read before it
goes beside the file's part (the `-tg` file's 56 notes were read as nothing).

**Measured.** A Body placed at (100, 50, 5) and turned 30 deg, with three datums, a flatness, a
position at MMC to A|B|C(M), a toleranced diameter, a distance and a note: written AP242 and
read back, the same objects and values, every plane within 4.7e-13 (the placement kept out) or
8.7e-12 (kept in), every graphic's extent within 9.2e-12. Every AP242 file of the NIST set, read,
written and read again, reads the same -- datums, tolerances with their frames, dimensions (the
nominal measured again, to 1e-3), datum targets, notes -- the `-tg` file's 56 notes included.

Tests: `PMITests/TestStep.py` -- `TestStepExport` (the values, the placements both ways, the
schema left alone without PMI), `TestStepRoundTrip` (CTC-01), and
`TestNistSet.test_every_ap242_file_round_trips` when `FC_PMI_NIST` names the set.

- Next: SFA on the output (sec 5.2 step 5); views written (P5, OCCT 5.3.4); then P4, TechDraw.

## 11. Decisions (made 2026-11-19)

1. **Names**: the upstream proposal's (#29772, [A8]) where the concepts match --
   `PMI::AnnotationSet`, `PMI::View`, `PMI::Datum`, `PMI::DatumReferenceFrame`,
   `PMI::Dimension`, `PMI::GeometricTolerance`, `PMI::Note` -- and ours where it has no
   equivalent (`PMI::TolerancedFeature`). The user, 2026-11-19.
2. **Default standard**: ISO GPS, as upstream starts; ASME Y14.5-2018 and -2009 are a property
   of each set away. (Recommendation, taken with the user's go-ahead.)
3. **AP242 on export**: automatic whenever the document has PMI; the export says so.
   (Recommendation.)
4. **Master**: the 3D model; TechDraw shows it, linked. Editing a linked tolerance in a drawing
   edits the 3D item, so there is one value; upstream's proposal is strictly one-way, which this
   includes. (Recommendation.)
5. **Presentation per item**: one, to start; STEP's n:m written as 1:1. (Recommendation.)

## 12. The upstream proposal, mapped

Issue #29772 and its sub-issues [A8], to this design:

| Upstream | Here |
|---|---|
| 29773 PMI workbench | 3 (module, workbench, ToolContext in PartDesign) -- P1 |
| 29774 standards preferences | 4.1 `Standard`, 8 -- P0 |
| 29775 container (annotation set) | 4.1 -- P0 |
| 29776 views, activation | 6.3 -- P1 |
| 29777 clipping planes | 6.3 (a view's clipping) -- P1 |
| 29778 datums (incl. targets) | 4.3 -- P0 (targets P2, with STEP) |
| 29779 datum reference frame | 4.3 -- P0 |
| 29780 dimensional specifications | 4.4 -- P0 |
| 29781 geometric tolerances | 4.5 -- P0 |
| 29782 tolerancing advisor | 8 (rules, P0) + the task panel's offer (P1) |
| 29783 text, notes, leaders | `PMI::Note` P0; leaders 6.2, P1 |
| 29784 tables | P5 |
| 29785 construction geometry | 4.2 derived kinds (axis, centre plane, centre point), drawn in P1 |
| 29786 surface texture | P5 |
| 29787 bind to geometry | 4.2 |
| 29788 repair broken references | 7 -- states P0, Reattach P1 |
| 29789 recompute behaviour | 4.4 (nominal measured, tolerances kept), 7 |
| 29790 edit annotations | P1 task panels |
| 29791 stacking | 6.4 -- P1 |
| 29792 view membership | 6.3 -- P1 |
| 29793 validate | 8 -- P0 |
| 29794 query a feature's specifications | P1 query tool (10.8) |
| 29795 TechDraw pages from views | 9 + P4 |
| 29796 STEP AP242 import | 5.1 -- P2 (semantics read 10.9; graphics, notes, views 10.10) |
| 29797 STEP AP242 export | 5.2 -- P3 |
| 29798 semantic access for other workbenches | the Python API: every object is a plain document object with typed properties, P0 |

What this design adds over the proposal: the shared toleranced feature (4.2, STEP's
`shape_aspect`), the generated graphic from P1 rather than as a later goal (6.1), repair built
into recompute (7), ASME beside ISO (4.1), and the OCCT fork work it needs (5.3).
