# GD&T / PMI: survey of other CAD programs, the standards and our code (2026-11-19)

Written for docs/GDandT.md, the design. Part A is other programs and the standards, from the
web (sources inline; anything not confirmed is marked UNCONFIRMED). Part B is what this fork and
its OCCT fork have today, read from the source on 2026-11-19 (main 7f8fef75c8, occt
LinkVibe-801 ede9c6c5c5). Cited from the design as [A n] and [B n].

## Part A: other programs and the standards

### A1. SolidWorks (DimXpert, MBD)

- What owns a tolerance: a DimXpert *feature* -- a manufacturing feature (hole, slot, notch,
  pocket, plane, width, cylinder, cone, pattern), recognised first from the model's features
  (Hole Wizard, fillets, patterns), else from topology; imported bodies get topology only.
  Annotations hang on these features, not on bare faces; compound planes/widths and
  intersect point/line/plane are derived features.
  https://help.solidworks.com/2012/English/SolidWorks/Sldworks/DimXpert_Overview.htm,
  https://help.solidworks.com/2023/English/SolidWorks/Sldworks/c_dimxpert_features.htm
- Datums on boss, cylinder, notch, plane, simple hole, slot, width; pattern datums since 2018.
  An Auto Dimension Scheme applies dimensions and tolerances with A = mating, B = locating,
  C = aligning; tolerance status colours features under/fully/over constrained.
  https://help.solidworks.com/2018/English/SolidWorks/Sldworks/r_auto_dimension_scheme_propertymanager.htm
- Presentation: annotation views (*Front, *Top...); "3D View" captures configuration, display
  state, annotation views, zoom and section.
- To drawings: "Import annotations > DimXpert annotations" on a drawing view whose orientation
  matches an annotation view -- only aligned views come over.
  https://help.solidworks.com/2022/english/SolidWorks/sldworks/t_importing_dimxpert_drawings.htm
- On edit: new faces are not toleranced; dangling DimXpert dimensions are repaired by manual
  reattach (2024). https://help.solidworks.com/2024/English/WhatsNew/c_wn2024_mbd_repairing_dangling_dims.htm
- Import: NX/Creo/AP242 PMI comes in as semantic but not editable.
  https://help.solidworks.com/2024/English/SolidWorks/Sldworks/c_import_pmi.htm

### A2. Creo

- An annotation element is owned by an annotation feature or by a modelling feature (Extrude,
  Hole); since 4.0 GTOLs, datum symbols and dimensions can also stand alone.
  https://community.ptc.com/model-based-enterprise-mbe-blog-47/mbd-standalone-annotations-vs-annotation-features-54276
- "Set datum tag" creates a datum plane/axis embedded in the annotation feature.
- References are split: *placement* (where it is drawn) versus *semantic* (what it tolerances),
  each strong or weak. A failed strong placement reference flags the feature and hides it; a
  failed semantic reference only flags.
  https://support.ptc.com/help/creo/creo_plus/usascii/model-based_definition/reference_dialog_box.html
- Orientation: annotation plane "driven by plane" or "frozen".
- GD&T Advisor (Sigmetrix, paid) checks semantics; base Creo has a syntax checker.
- To drawings: "Show Model Annotations" shows the model's own annotations, associative both ways.
  https://support.ptc.com/help/creo/creo_pma/r13/usascii/detail/dims_about_showing.html

### A3. Siemens NX

- PMI objects carry "associated objects" (the toleranced faces); each sits on an orientation
  plane, and a leader will not attach to an FCF on another plane.
- Lost reference: the annotation becomes "Retained" (kept, flagged) until reattached.
  https://support.sw.siemens.com/en-US/okba/PL7004519/Retained-Annotation-is-still-Retained-even-after-selecting-a-new-attachment-for-the-leader/index.html
- To drawings: "Inherit PMI" per drafting view -- from a model view, or "aligned to drawing",
  which takes only PMI whose plane matches the view; some styling is lost on the way.
  https://www.iim.maschinenbau.tu-darmstadt.de/CAD-Tutorial/9_pmi/2_derivation_of_drawings/index.en.html

### A4. CATIA V5 / 3DEXPERIENCE (FT&A)

- Annotations live in an annotation set, grouped by annotation planes ("views"); captures group
  annotations with an optional camera. The Semantic Tolerancing Advisor creates semantic GD&T
  to ISO, ASME or JIS.
  https://3dswym.3dexperience.3ds.com/wiki/catia-user-community/catia-3d-functional-tolerancing-and-annotations-2-fta_g6WGTlSlT1ii20jkZdd6sQ
- To drawings: annotation planes become Generative Drafting views; annotation positions shifting
  on projection is a logged permanent restriction (IBM, V5R16/17).
- UNCONFIRMED: the internal model being TTRS-based.

### A5. Onshape, Fusion

- Onshape MBD, GA 2026-03-02: PMI tied to the feature tree through sketch dimensions, hole
  tolerances and tagged datums; shareable views, an inspection panel; AP242 export "future".
  https://develop3d.com/cad/mbd-goes-live-in-onshape/ ; drawings have FCF and datum tools.
- Fusion: FCF and datum tools in 2D drawings; 3D PMI announced, extent UNCONFIRMED.

### A6. The standards as a data model (ASME Y14.5-2018, ISO 1101 / 5459 / 14405)

- Characteristics: 14 in Y14.5-2009 and ISO 1101; Y14.5-2018 dropped concentricity and
  symmetry (12). Form: straightness, flatness, circularity, cylindricity. Profile: line,
  surface. Orientation: angularity, perpendicularity, parallelism. Location: position,
  concentricity/coaxiality, symmetry. Runout: circular, total.
- A feature control frame (FCF): characteristic; tolerance compartment = zone shape (none, DIA,
  S-DIA) + value + modifiers (M, L material condition; F free state; P projected + length; T
  tangent plane; U unequally disposed + value; ST statistical; CZ common zone; ACS, LE, NC,
  LD/MD/PD; ISO filters; a MAX value); then up to three ordered datum compartments, each a
  datum or a common datum (A-B), with per-datum modifiers (M/L, BSC, translation, DOF, [PT]
  [SL] [PL], ...). Full lists: CAx-IF RP Tables 13-15.
  https://www.mbx-if.org/home/wp-content/uploads/2024/06/rec_pracs_pmi_v41.pdf
- Also: composite frames (an upper pattern-locating segment over lower ones) versus stacked
  (independent) frames; all-around, all-over, between (from X to Y); unit-basis tolerances;
  datum targets (point, line, circle, rectangle, area, movable); basic dimensions (ISO TED);
  +/-, limits, ISO 286 fits (H7/g6); ISO 14405 size modifiers (LP, LS, GG, GX, GN, CC, CA, CV,
  SX, SN, SA, SM, SD, SR, ACS, SCS, CT, ...).
- A *datum feature* is the tangible face set; the *datum* is the exact plane/axis/point derived
  from it.
- Default principle differs: ASME implies envelope (Rule #1), ISO independency -- the standard
  has to be recorded per part.

### A7. STEP AP242 semantic PMI (CAx-IF Recommended Practices v4.1, 2024-06-20)

- A toleranced feature is a `shape_aspect` tied to its `advanced_face`s by
  `geometric_item_specific_usage` (GISU); one shape_aspect per face set, shared by every
  annotation on it. Derived features (axis, centre plane, centre point, apex) are
  `derived_shape_aspect` subtypes with geometry in a `constructive_geometry_representation`.
  Several faces as one: `composite_group_shape_aspect`; `all_around_shape_aspect`;
  `between_shape_aspect`.
- Dimensions: `dimensional_location` / `dimensional_size` (+ angular, with path), values with
  `plus_minus_tolerance`, ranges, `limits_and_fits`, decimals.
- Datums: `datum_feature` -> `datum` ('A'); targets `placed_datum_target_feature`;
  `datum_system` of ordered `datum_reference_compartment`s, one instance per distinct DRF,
  reused.
- Tolerances: `geometric_tolerance` subtypes combined as complex instances (with datum
  reference, with modifiers, with maximum tolerance, unit basis, unequally disposed) +
  `tolerance_zone`; composite = `geometric_tolerance_relationship` 'composite'.
- Presentation: polyline (`annotation_curve_occurrence`) or tessellated
  (`tessellated_annotation_occurrence`), each callout on an `annotation_plane` (x = reading
  direction, z = toward the reader, everything coplanar). Character-based 3D text is on hold:
  text is shipped as strokes or triangles.
- "Semantic + presentation linked": `draughting_model_item_association` joins a semantic item to
  its callout (n:m), so the semantics stay the definition and the graphic can be regenerated.
- Saved views: `draughting_model` + `camera_model_d3` + `model_geometric_view`.
- AP203e2/AP214e3 carry polyline graphics only (no semantics).
- Test data (free, unrestricted): NIST CTC-01..05, FTC-06..11, STC-06..10 in AP242 and AP203.
  https://www.nist.gov/el/systems-integration-division-73400/mbe-pmi-validation-and-conformance-testing/download-free-cad
  Validator: NIST STEP File Analyzer and Viewer (SFA), https://github.com/usnistgov/SFA.

### A8. Open source

- OCCT XDE: GD&T labels (`XCAFDoc_Dimension` / `_GeomTolerance` / `_Datum`, objects
  `XCAFDimTolObjects_*`) linked to shape and sub-shape labels; AP242 read and write since 7.0,
  graphic-only GD&T since 7.2, tessellated presentation import 2024. Saved views and clipping
  planes are stored but written to XBF only, never STEP.
  https://occt3d.com/dev/doc/overview/html/occt_user_guides__xde.html
  OCCT does not generate presentation for GD&T it is given (that is the commercial "PMI
  Visualization" component). A forum report has tolerance values read as null from NX/NIST
  files in 7.8/7.9, fixed by 8.0rc3 (UNCONFIRMED in our tree).
- FreeCAD upstream: TechDraw has GD&T SVG symbols and fakes FCFs with balloons
  (https://wiki.freecad.org/TechDraw_Geometric_dimensioning_and_tolerancing). Issue #29772
  (2026-05-02, ~26 sub-issues, no PRs) proposes a semantic PMI workbench, ISO-GPS first:
  `PMI_AnnotationSet`, `PMI_View`, `PMI_Datum`, `PMI_DatumReferenceFrame`, `PMI_Dimension`,
  `PMI_GeometricTolerance`, `PMI_Note`, `PMI_Leader`, `PMI_ConstructionGeometry`, TNP
  references, repair, advisor, TechDraw derivation, AP242 I/O.
  https://github.com/FreeCAD/FreeCAD/issues/29772
- The FreeCAD-GDT addon (juanvanyo, unmaintained since 2024): Python objects AnnotationPlane,
  DatumFeature, DatumSystem, GeometricTolerance, Annotation; one modifier per tolerance, no
  datum modifiers, no STEP. https://github.com/juanvanyo/FreeCAD-GDT
- CadQuery, build123d, Mayo: nothing beyond OCCT's XCAF.

### A9. Pitfalls every program reports

1. Reference loss on edit. Everyone has an explicit broken state with manual reattach, never
   silent deletion (SW dangling, NX Retained, Creo strong/weak flags).
2. Semantic and graphic disagreeing. NIST AMS 100-10 found 411 failures in 10,692 checks across
   four CAD systems -- e.g. an FCF exported as text with no semantics, a projected-zone length
   missing from the graphic. https://nvlpubs.nist.gov/nistpubs/ams/NIST.AMS.100-10.pdf
   Write the graphic *from* the semantics.
3. Reuse in drawings works only where the annotation plane is parallel to the sheet (SW, NX);
   positions shift on projection (CATIA).
4. Annotation planes must encode the reading direction; leaders out of plane detach; text seen
   from behind must not read mirrored.

## Part B: what we have

### B1. STEP import and export (src/Mod/Import)

- `ReaderStep.cpp` sets colour/name/layer/SHUO modes only (one-shot 302-305, streamed 157-160);
  OCCT's own defaults leave GD&T and view reading ON (`STEPCAFControl_Reader.cxx:324-333`), so
  the XCAF document *already holds* the GD&T of a file we import -- and `ImportOCAF2` throws it
  away. The only reference to it in src/ is the OCAF debug browser
  (`Import/Gui/OCAFBrowser.cpp:269-273`).
- `ImportOCAF2::scanElementColors` (444-520) maps sub-shape labels to `FaceN` with
  `findShape()` -- the same mapping a GD&T reference needs.
- `WriterStep.cpp:45-83` transfers with `STEPCAFControl_Writer`; DimTol mode is on by default but
  the document has no DimTol labels. `ExportOCAF2.cpp` already creates sub-shape labels
  (`AddSubShape`, 422, 542) for per-face colours and materials -- what GD&T attachment needs.
- Schema: `write.step.schema` from the `StepScheme` parameter (`PartParams.cpp:827`); AP242DIS is
  offered in `DlgExportStep.ui`, not in the import/export preference page.
- To verify: the streamed import runs OCCT's attribute passes per batch; `ReadGDTs` adds labels
  without a lookup, so a multi-root file may get duplicates.

### B2. OCCT GD&T (occt, unchanged by the fork)

- `XCAFDoc_DimTolTool`: `AddDimension`/`SetDimension`, `AddGeomTolerance`/`SetGeomTolerance`,
  `AddDatum`/`SetDatum`/`SetDatumToGeomTol`, `GetRefShapeLabel`, presentations.
- Object classes carry everything A6 lists except composite frames: dimension type (24 kinds),
  values, +/-, ISO 286 class, decimals, ISO 14405 modifiers; tolerance type (14 + None), zone
  type, value, material modifier, zone modifier + value, 17 modifiers, max value; datum name,
  22 modifiers, modifier with value, targets, position. Presentation: plane, text point,
  `TopoDS_Shape` presentation.
- Reader: `ReadGDTs` (5990+), `readDatumsAP242` (4343), presentation `readPMIPresentation`
  (3215), views `ReadViews` (6583+). Datum targets are skipped on read (4059).
- Writer: `writeDGTsAP242` runs only with `WriteSchema == 5` (AP242DIS), else the legacy
  AP214-style `writeDGTs` (Writer.cxx:796-803). Shapes referenced must have been transferred
  ("Cannot find RI" otherwise, 3094). Presentation is tessellated only, curve colour
  hard-coded white (4945). Saved views are never written. `writeDGTsAP242` ignores its label
  list (4929). No `geometric_tolerance_relationship` (composite) in reader or writer.
- Reference for API use: `Draw/TKXDEDRAW/XDEDRAW/XDEDRAW_GDTs.cxx` (~60 commands); tests
  `tests/gdt/*`, e.g. `tests/gdt/export/A1` round-trips NIST CTC-01.

### B3. TechDraw

- `DrawViewDimension` has +/- tolerance (`EqualTolerance`, `OverTolerance`, `UnderTolerance`,
  format specs), `TheoreticalExact` (basic), `References2D`/`References3D`, and the fork's
  `SavedNames` + `fixByName()` (1717-1788) re-finding references by element name.
- GD&T in TechDraw is pictures: `Symbols/gd-and-t/*.svg`, `Resources/ToleranceSymbols/*.svg`,
  glyph insertion into format specs (`TaskCustomizeFormat.cpp:95-125`). No FCF object.
- `DrawWeldSymbol` + `DrawTile`/`DrawTileWeld` + `DrawLeaderLine` (leader, tile grid, text
  cells) is the closest existing shape for a 2D FCF object.
- The projection names every projected edge/face/vertex after its 3D source
  (`GeometryObject::nameEdgeGeometry`, 309-345; `getSource3d()`, `;HLR:`/`;HLRF:` names;
  `DrawViewPart::getGeometryName`/`getGeometryReference`). docs/TopoNamingEnhance.md sec 3,
  docs/TechDrawStoredGeometry.md 6.5.
- The precedent to copy: Import Edge Labels (`CommandEdgeLabels.cpp`, `DrawEdgeLabel`), which
  asks a 3D view provider for per-element annotation info keyed by mapped name
  (`ViewProvider::getEdgeInfo`, `Gui/ViewProvider.h:539-560`), matches it to `getSource3d()`, and
  creates 2D objects that keep the user's moves and edits (`isCustomized`, `isMoved`,
  `ImportOffset`). Used by sheet-metal bend notes and exploded-view BOM balloons.

### B4. PartDesign, Part and the element map

- `Body::isAllowed` takes datums, binders, sheet-metal rules, `App::DatumElement`, LCS, VarSet,
  AuxGroup, and any type a module registers with `Part::BodyBase::allowType(type, kind)`
  (`Part/App/BodyBase.h:86-91`); `AuxGroup` sorts them into roles.
- `FeatureSheetMetalRule` is a parameters-only `Part::Feature` living in a Body -- the closest
  model for a non-solid annotation object. `FeatureHole` already carries thread class and fit.
- References: `PropertyLinkSub(List)` with mapped element names, `ShadowSub` (new/old name
  pair), `updateElementReferences`; `ComplexGeoData::getElementHistory` traces a name through
  later features.
- 3D labels: `SoDatumLabel` (distance, angle, radius, diameter, text; XY plane + transform, with
  a render-cache bridge), `So3DAnnotation`, and the fork's on-view value editing
  (`docs/OnViewValueEditing.md`). No FCF glyph.
- Our docs: no GD&T/PMI design so far; docs/ShapeAppearanceDesign.md puts ISO 1302 surface
  texture "out of scope -- the rung above".
