# Coming from another CAD program: how they organise modelling, and what we could adopt

Status: a SURVEY and design directions, NOT BUILT, 2026-10-25. Nothing here is
to be built until the user has discussed it. Written by a research session
from a read of this tree's source and docs (nothing built or run) and a web
search of the other programs' official help. Where a statement about another
program comes from general knowledge and not from a page found, it says so,
marked "(gk)". Some official pages could not be fetched (Autodesk's marketing
site and Shapr3D's help answered 403; SOLIDWORKS help renders by script) and
were read only through a search engine's summary of them; those sources say
"via search summary" in the list at the end.

We learn from patterns here. We do not clone any product's look, layout,
icons or trade dress, and nothing below proposes to. Product names appear
only where they describe what a user is coming from, the way Solid Edge's own
Command Finder names other CAD systems [32] and SolidCraft's README names
Fusion "only to describe compatibility" [1].

## 1. Why

- SolidCraft (github.com/bherbruck/solidcraft) is a Rust parametric CAD,
  "built in pure Rust", desktop + browser + an MCP server for agents [1],
  reported as built by Claude coding agents in about ten days and as copying
  "Fusion's Design workspace on purpose": "the ribbon on top, the parts tree
  on the left and the timeline at the bottom", Fusion's shortcuts (F for
  fillet), Fusion-like parameters [2]. It draws users because it feels
  familiar, not because it is more capable -- the same article reports a 2 mm
  fillet failing on a sample enclosure [2].
- The complaint usually made about FreeCAD is the opposite of familiar: tools
  split across workbenches, each with its own toolbars and menus, and names
  that differ from what everyone else calls the same operation (Pad vs
  Extrude, Pocket vs Cut-Extrude, Thickness vs Shell, Body and Tip).
- The cheapest familiarity is not layout but WORDS, MOUSE and KEYS: a user who
  can type the word they know, orbit the way their hand expects, and press the
  key they are used to, is most of the way there. Layout is the expensive
  part, and the part where copying is least appropriate.

## 2. What this fork already has (read before proposing)

- `/` omni search (docs/OmniSearch.md): `/cmd` matches every whitespace-
  separated word against a command's title, internal name, shortcut and
  tooltip (`CommandListModel` `SearchTextRole`, `src/Gui/CommandCompleter.cpp`
  142). Commands of a module not loaded are not listed. Mirrored in the browser
  as a versioned catalog.
- On-view values and handles (docs/OnViewValueEditing.md): agreed and largely
  built for PartDesign -- the main value's label at the top middle, Tab between
  values, a digit types into the active handle, a menu on the handle's root,
  in desktop and browser. Its survey section 2 covers how Fusion, SolidWorks,
  Onshape, NX, Creo, CATIA and Shapr3D do dress-up editing; not repeated here.
- Navigation styles (`src/Gui/*NavigationStyle.cpp`): CAD (the default,
  `ViewParams.py` 949), OpenInventor (named "OpenInventor"; it is Coin's viewer
  style, not Autodesk Inventor's), Blender, Gesture, MayaGesture, OpenCascade,
  OpenSCAD, Revit, SolidWorks, TinkerCAD, Touchpad. Read from their
  `mouseButtons()` text: SolidWorks = MMB rotate, Ctrl+MMB pan, Shift+MMB and
  wheel zoom; Revit = MMB pan, Shift+MMB rotate; TinkerCAD = RMB rotate, MMB
  pan, an RMB click opens the context menu.
- A first-start panel on the Start page (`src/Mod/Start/Gui/FirstStartWidget.cpp`,
  `GeneralSettingsWidget.cpp`): language, unit system, navigation style, theme.
- Pie menus and toolbar pop-ups (`src/Gui/PieMenu.*`, `ToolbarMenuAction`):
  in Tools > Customize > Toolbars any toolbar can be given a shortcut and a
  "Pie Menu" tick, so the key pops that toolbar at the cursor as a pie or a
  plain menu (`DlgToolbarsImp.cpp` 300-395). Nothing ships bound by default.
- Shortcut conflicts resolved by priority (`ShortcutManager::setPriority`),
  and an ambiguous chord prefix waits `ShortcutTimeout` = 300 ms
  (`MiscParams.py` 75) before firing the shorter key.
- Selection-driven command lists: PartDesign's `TaskWatcherCommands`
  (`src/Mod/PartDesign/Gui/Workbench.cpp` 241-484) show, in the task panel when
  no task is open, a list per selection kind -- a face gives Sketch, Pad,
  Pocket, Fillet, Chamfer, Draft, Thickness ...; edges give Fillet, Chamfer;
  a sketch gives Pad, Pocket, Hole, Revolution, Groove, pipes, lofts, helices.
- Workbench auto-switching: double-clicking a Body activates PartDesign
  (`PartDesignParams` `SwitchToWB`, default on); editing a sketch activates
  the workbench named in the sketch's `EditingWorkbench` view property
  (default "SketcherWorkbench", applied through `Show.TempoVis`, so leaving
  the sketch restores the previous one -- `ViewProviderSketch.cpp` 811, 9497).
  The Sketcher's edit-mode toolbars are `DefaultVisibility::Unavailable` and
  forced available on entering an edit (`Sketcher/Gui/Workbench.cpp` 105-235).
- Active body auto-selection: `PartDesignGui::getBody(..., autoActivate)`
  activates the body of the selection, or the only body, when none is active
  (`Utils.cpp` 155-195); `PartDesign_NewSketch` makes a body when the document
  has none (`Command.cpp` 594-609).
- `PartDesign::Feature::Suppress` exists (`Feature.h` 57).
- docs/TransactionLog.md: an append-only log of document transactions, with
  versions and branches, Onshape as the blueprint (its sec 8.1, 16). Its own
  survey notes: "Fusion's timeline is the feature list, not an undo log"
  (sec 19). See section 6 item 7.
- docs/ThinClient.md sec 5: a browser design (not built) for a tool rail, a
  selection-contextual action bar "populated from selection type + an
  operation catalog the backend advertises", and a radial menu.
- docs/UserSettings.md: per-user settings, designed, not built.

## 3. The programs

### 3.1 Autodesk Fusion

- Modes. Workspaces: Design, Electronics, Generative Design, Render,
  Animation, Simulation, Manufacture, Drawing; "The tools in each workspace
  are organized into tabs in the toolbar according to particular design
  objectives" [4]. The Design workspace has SOLID, SURFACE, MESH, SHEET METAL
  and TOOLS tabs; "Contextual tabs ... only become active in context to the
  command you invoke" [4], and a contextual environment's tabs replace the
  default ones until it is exited (via search summary of [5]). Ctrl+[ and
  Ctrl+] step through the workspace list [3]. Modelling a part and an assembly
  happens in ONE workspace (Design); there is no "part design" vs "assembly"
  switch.
- History. One timeline per design, along the bottom. Editing a feature: "all
  the nodes to the right of the edited feature are disabled ... Fusion has
  effectively rolled the timeline to just before the edited feature" [6]; the
  marker can be dragged to any point, and "to edit anything in the timeline,
  the marker has to be to the right of the feature being edited" [6]. A design
  can be switched to direct modelling ("Do not capture Design History", which
  removes the timeline) and back, the existing geometry then captured as one
  Base Feature [7]. Imported files open without history (via search summary
  of [7]).
- Tree. The Browser on the left: components, bodies, sketches, construction,
  origin (gk). Bodies live inside components; a component is the unit of
  assembly (gk).
- Sketch. Create Sketch, then pick an origin plane, work plane or planar face;
  a Sketch contextual tab appears, it and the Finish Sketch button "highlighted
  blue to let you know that you are in a temporary mode" [12]; a Sketch
  Palette in the canvas has contextual options, Construction, Look At, Slice,
  Show Constraints [12]. Leave with Finish Sketch, or from the marking menu
  [12]. Selecting an existing sketch edits it [12].
- Finding a command. The toolbar tabs; the S key opens the "Model Toolbox"
  at the cursor [3], customisable and with a search field (gk); a right-click
  marking menu -- a radial first level, a second level for sketch tools, a
  context menu below -- whose commands change "for each workspace, toolbar tab,
  contextual environment, and active command", with "gestures" (a right-drag
  in a direction runs a command without showing the menu) and Repeat as a
  default first-level item [5].
- Dialog. A command dialog with OK/Cancel; on-canvas manipulators (an arrow
  for a distance) with a value box beside them (gk; see OnViewValueEditing.md
  sec 2). Extrude is ONE command with an Operation: Join, Cut, Intersect, New
  Body, New Component; Direction One Side / Two Sides / Symmetric; Extent
  Distance / To Object / All; Taper; Thin Extrude [8].
- Selection. Both orders work: a profile or face picked first is taken by
  Extrude (gk; the help lists the dialog first and the selection after [8]).
- Mouse. Default: MMB pan, Shift+MMB orbit, wheel zoom [3]. A preference
  "Pan, Zoom, Orbit shortcuts" offers Fusion, SolidWorks, Inventor, Tinkercad,
  Alias, PowerMill presets [11] (via search summary).
- Keys (Design) [3]: E Extrude, H Hole, Q Press Pull, F Fillet, M Move, V
  visibility, A Appearance, J Joint, Shift+J As-built joint, S Toolbox;
  sketch: L Line, R 2-point rectangle, C circle, T Trim, O Offset, P Project,
  X Normal/Construction, D Sketch Dimension, I Measure; 1/2/3 window, freeform,
  paint selection.
- Terms: see section 5. Joints, seven types: Rigid, Revolute, Slider,
  Cylindrical, Pin-Slot, Planar, Ball [10]; Joint and As-built Joint [10].
  Mirror, Rectangular Pattern, Circular Pattern, Pattern on Path [9]; Shell,
  Draft, Combine (Join / Cut / Intersect) on the Modify panel [13].
- Orientation: Y-up by default, a "Default modeling orientation" preference
  (third-party tutorial [38]; no Autodesk page found). FreeCAD is Z-up.

### 3.2 SOLIDWORKS

- Modes. One document per part, assembly or drawing; the CommandManager is a
  tabbed toolbar that "is context-sensitive" and whose embedded toolbars
  change with the document type; Ctrl+PgUp / Ctrl+PgDn step its tabs
  (via search summary of [16]). Sketch is a mode inside the part (gk).
- History. The FeatureManager design tree is the history; the rollback bar
  "temporarily returns the model to an earlier state", can be dragged up and
  down, and features can be added or edited while rolled back; rolled-back
  items show grey [17]. Dragging items reorders regeneration; right-click
  gives Roll Forward, Roll to Previous, Roll to End [18].
- Sketch. Select a plane or face, then Sketch (gk); exit with Exit Sketch in
  the Confirmation Corner, the Sketch toolbar, the menu, or a double-click in
  empty space with no sketch tool active [19]. Fully defined geometry shows
  black, under-defined blue (gk).
- Finding a command. CommandManager tabs; the S key shows the "shortcut
  bars", noncontext command sets for Part, Assembly, Drawing and Sketch, with
  "Search All Commands" in them [14]; context toolbars that "appear when you
  select items in the graphics area or FeatureManager design tree" [15];
  mouse gestures (a right-drag in a direction runs a command) (via search
  summary; details page not read).
- Dialog. The PropertyManager in the left pane (gk); the Confirmation Corner
  (OK / Cancel in the graphics area's top right), and D moves OK/Cancel to the
  pointer [20]. Instant3D: "create and modify model geometry using drag
  handles and rulers" [25].
- Selection. Selection-first is the house style -- the context toolbar is
  built on it [15] -- and commands also run first and ask (gk).
- Mouse. MMB rotate, Ctrl+MMB pan, Shift+MMB zoom, wheel zoom toward the
  pointer [22].
- Keys (via search summary of [21]): Ctrl+B rebuild, Space the View Selector
  and orientation dialog, F zoom to fit, S shortcut bar [14], D Confirmation
  Corner to pointer [20]. Few modelling single keys by default (gk).
- Terms. Extruded Boss/Base and Extruded Cut are separate tools [24], as are
  Revolved, Swept and Lofted Boss/Base and Cut (gk); Hole Wizard and Shell on
  the Features toolbar [24]. Mates on three tabs: Standard (coincident,
  parallel, perpendicular, tangent, concentric, lock, distance, angle),
  Advanced (profile center, symmetric, width, path, linear coupler, limit),
  Mechanical (cam, slot, hinge, gear, rack and pinion, screw, universal joint)
  [23]. Planes Front / Top / Right, Front = XY, i.e. Y-up (gk).

### 3.3 Onshape

- Modes. Tabs in one document: Part Studios, Assemblies, Drawings (gk). A
  Part Studio holds MANY parts driven by ONE feature list [28]. Toolbars at the
  top "change based on the current workflow": Document, Feature, Sketch,
  Assembly, Drawing [29].
- History. The Feature list is "a parametric history of work, containing a
  Rollback bar" [29]; drag it, right-click a feature for "Roll to here", the
  arrow keys step it [28]. Separately, the version graph (versions, branches,
  per-user undo as forward operations) -- the blueprint of TransactionLog.md
  sec 16 and its sources.
- Sketch. A sketch is a feature in the list (gk); its dialog is accepted with
  the checkmark like any feature (gk). Sketch keys: l line, c circle, d
  dimension, q construction toggle, u Use (project/convert) [26].
- Finding a command. The Feature toolbar, where "the last used feature or
  tool moves to the top of the tool group" [29]; "Search tools" (Alt+C) [26]
  [29]; s opens a shortcut toolbar at the cursor [26].
- Dialog. "A solid blue field requires selection in the graphics area"; "a
  field outlined in blue requires keyboard input" [29] -- the dialog itself
  is the selection prompt. Extrude: result New / Add / Remove / Intersect;
  end Blind, Up to next, Up to face, Up to part, Up to vertex, Through all,
  Symmetric for Blind and Through all; a manipulator arrow; "Click the
  checkmark to accept"; a Preview slider [30].
- Selection. Both orders (gk).
- Mouse. RMB drag rotate, wheel zoom, MMB drag or Ctrl+RMB pan, Alt+RMB keeps
  the vertical axis upright [27]; presets to match SOLIDWORKS, NX, Creo,
  AutoCAD [27].
- Keys [26]: shift+e Extrude, shift+w Revolve, shift+f Fillet; shift+1/5/7
  front/top/iso; f fit; m Fastened mate; j show mates; s shortcut toolbar.
- Terms. Mates: Fastened, Revolute, Slider, Cylindrical, Pin slot, Planar,
  Ball, Parallel, Tangent; Relations (gear, rack and pinion, screw, linear)
  tie mates together [31]. Mate connectors define where parts meet [31].
  Z-up, Top = XY (gk).

### 3.4 Siemens Solid Edge and NX

- Solid Edge has two environments in one part: Ordered (history) and
  Synchronous (history-free, "feature-based, history-free" with Live Rules
  that "find and maintain 'strong' geometric conditions during a move") [34].
  Ordered edits roll back to the feature's creation point, so the user "does
  not see how the edit will interact with the subsequent features" [34].
  Features move from ordered to synchronous by right-click [32]. Siemens'
  own blog still advises ordered for sweeps, lofts, thin walls (via search
  summary of [34]). A short note only: synchronous editing on B-rep is
  out of scope for this survey, but it is the nearest analogue to the
  "direct edit" a Fusion user does with Press Pull.
- Solid Edge UI: a ribbon, PathFinder (the tree, with ordered and synchronous
  sections), a command bar for options and data entry, a radial menu, the
  steering wheel for direct moves [32]. And **Command Finder**: "If you are
  familiar with another CAD system, you can find the equivalent Solid Edge
  command using Command Finder" [32]; a Siemens blog gives typing "Remove" to
  list the commands that remove material [32]. This is exactly proposal 1
  below, shipped by a vendor for this reason.
- Solid Edge terms (mostly gk): Extrude / Protrusion, Cutout, Revolve,
  Revolved Cutout, Sweep, Loft, Round, Chamfer, Thin Wall, Draft, Hole,
  Pattern, Mirror; assembly relationships ground, mate, planar align, axial
  align, parallel, connect, angle, cam, gear, tangent, center-plane, with
  FlashFit choosing among the first three [33].
- Solid Edge mouse: not verified; no page found.
- NX: a ribbon and roles -- Welcome, Essentials (a trimmed command set for
  beginners) and Advanced [35]. Its Command Finder became a Global Search in
  NX 2206 [35]. The role idea -- one program, a smaller UI for someone new --
  is relevant to proposal 5.

### 3.5 Short notes

- Shapr3D. An "adaptive" UI that "automatically recommends tools as you make
  selections" -- a face makes Offset Face active, a body Move/Rotate; select
  a sketch element and the sketch tools come up [36] (via search summary;
  pages 403). Touch and pen first; type a value while dragging (gk, and
  OnViewValueEditing.md sec 2). The model for ThinClient.md sec 5.
- Plasticity. F opens a command palette, type a name to run [37]; keys 1-4
  set control point / edge / face / solid selection [37]; Hollow and Thicken
  rather than Shell [37]. Direct modelling without a parametric history (gk).
  Artists coming from Blender; FreeCAD's Blender style is the nearest.
- SolidCraft. Fusion's Design layout on purpose, a timeline, Fusion's keys,
  a Rust kernel wrapping truck [2]; features named the Fusion way: extrude,
  revolve, sweep, loft, fillet, chamfer, shell, draft, holes, threads,
  patterns, mirror, split, combine; joints, contact sets [1]. Its pull is
  familiarity, which is the point of this document.

## 4. Comparison

| Dimension | FreeCAD (this fork) | Fusion | SOLIDWORKS | Onshape | Solid Edge |
|---|---|---|---|---|---|
| Mode model | workbenches, each a whole UI; auto-switch to PartDesign on a Body, to Sketcher in a sketch | workspaces; Design holds parts AND assemblies; tabs per data type; contextual environments [4] | one document per part / assembly / drawing; CommandManager tabs by document type [16] | tabs in a document: Part Studio, Assembly, Drawing; toolbars follow the tab [29] | environments (Part, Assembly, Draft); Ordered and Synchronous in one part [34] |
| History | per Body: `Group` order + Tip; showing a feature rewinds to it, no recompute; Set tip; tree drag reorders | one timeline per design, bottom; marker; edit rolls back [6]; history can be turned off [7] | FeatureManager tree + rollback bar [17] | Feature list + rollback bar per Part Studio [28]; version graph | PathFinder; ordered section rolls back on edit [34] |
| Container of a part | Body (single solid by default, `SingleSolid`) inside an App::Part | body inside a component | the part file (multibody allowed) | a part, many per Part Studio | the part file |
| Sketch entry | Create sketch -> attach dialog or the selected face; workbench switches | Create Sketch -> pick plane; contextual tab [12] | pick plane -> Sketch (gk) | sketch feature, pick plane (gk) | (gk) sketch on a plane |
| Sketch exit | Close / Leave sketch (task panel) | Finish Sketch [12] | Confirmation Corner, double-click empty [19] | checkmark (gk) | (gk) Close Sketch |
| Finding a command | workbench toolbars + menus; `/cmd` search; selection lists in the task panel; user-made pies | tabs; S toolbox [3]; marking menu [5] | CommandManager; S shortcut bar with search [14]; context toolbar [15]; gestures | toolbar; Search tools Alt+C; s shortcut toolbar [26] | ribbon; radial menu; Command Finder [32] |
| Feature dialog | task panel (OK/Cancel/Apply); on-view handles and typed values (OnViewValueEditing) | dialog + manipulators (gk) | PropertyManager; Confirmation Corner [20]; Instant3D [25] | dialog; blue fields ask for selection; checkmark; preview slider [29][30] | command bar (gk) |
| Additive / subtractive | separate commands (Pad / Pocket, Revolution / Groove, Additive / Subtractive pipe, loft, helix) | one command, Operation field [8] | separate Boss / Cut tools [24] | one command, result field [30] | separate (gk) |
| Selection order | both; dress-ups accept none; a wrong body is refused with a message | both (gk) | both; selection-first favoured [15] | both (gk) | both (gk) |
| Default mouse | CAD style: MMB pan, MMB+LMB/RMB rotate | MMB pan, Shift+MMB orbit [3] | MMB rotate, Ctrl+MMB pan, Shift+MMB zoom [22] | RMB rotate, MMB pan [27] | not verified |
| Mouse presets | 11 styles | 6 presets [11] | -- | 4 presets [27] | -- |
| Single-key defaults | Sketcher yes (D, H, V, P, N, T, E, S, chords G,L ...); Assembly yes (F, R, C ...); PartDesign none | many [3] | few; S, F, D, Space [14][20][21] | shift+letters; s [26] | (gk) few |
| Up axis | Z | Y by default [38] | Y (gk) | Z (gk) | Z (gk) |

## 5. Terminology crosswalk

"--" is no direct equivalent found. "(gk)" is general knowledge, no page found.
The FreeCAD column gives this fork's command title; upstream FreeCAD has
since renamed some (upstream `main`, read 2026-10-25: "Revolve", "Mirror",
"Linear Pattern", "Additive Pipe").

### 5.1 Part modelling

| FreeCAD (PartDesign; Part in brackets) | Fusion | SOLIDWORKS | Onshape | Solid Edge |
|---|---|---|---|---|
| Body | body (in a component) | solid body (gk) | part | body (gk) |
| App::Part (container) | component | part file / subassembly (gk) | Part Studio (gk) | part file (gk) |
| Tip, Set tip | timeline marker [6] | rollback bar [17] | rollback bar [28] | -- |
| Suppress | Suppress (gk) | Suppress (gk) | Suppress (gk) | Suppress (gk) |
| Create sketch | Create Sketch [12] | Sketch | Sketch | Sketch |
| Pad [Part: Extrude] | Extrude, Join / New Body [8] | Extruded Boss/Base [24] | Extrude, New / Add [30] | Extrude / Protrusion (gk) |
| Pocket | Extrude, Cut [8] | Extruded Cut [24] | Extrude, Remove [30] | Cutout (gk) |
| Extrusion (fork: a standalone extruded shape) | Extrude, New Body [8] | -- | Extrude, New [30] | -- |
| Revolution [Part: Revolve] | Revolve (gk) | Revolved Boss/Base (gk) | Revolve [26] | Revolve (gk) |
| Groove | Revolve, Cut (gk) | Revolved Cut (gk) | Revolve, Remove (gk) | Revolved Cutout (gk) |
| Additive / Subtractive pipe [Part: Sweep] | Sweep (gk) | Swept Boss/Base, Swept Cut (gk) | Sweep (gk) | Sweep (gk) |
| Additive / Subtractive loft [Part: Loft] | Loft (gk) | Lofted Boss/Base, Lofted Cut (gk) | Loft (gk) | Loft (gk) |
| Additive / Subtractive helix | Coil (gk) | helix curve + sweep (gk) | helix + sweep (gk) | Helix (gk) |
| Hole | Hole [3] | Hole Wizard [24] | Hole (gk) | Hole (gk) |
| Fillet | Fillet [3] | Fillet | Fillet [26] | Round (gk) |
| Chamfer | Chamfer | Chamfer | Chamfer | Chamfer |
| Draft | Draft [13] | Draft (gk) | Draft (gk) | Draft (gk) |
| Thickness [Part: Thickness] | Shell [13] | Shell [24] | Shell (gk) | Thin Wall (gk) |
| Mirrored [Part: Mirroring] | Mirror [9] | Mirror (gk) | Mirror (gk) | Mirror (gk) |
| LinearPattern | Rectangular Pattern [9] | Linear Pattern (gk) | Linear pattern (gk) | Pattern (gk) |
| PolarPattern | Circular Pattern [9] | Circular Pattern (gk) | Circular pattern (gk) | Pattern, circular (gk) |
| PathPattern (fork) | Pattern on Path [9] | Curve Driven Pattern (gk) | Curve pattern (gk) | Pattern along curve (gk) |
| MultiTransform | -- | -- | -- | -- |
| Scaled | Scale (gk) | Scale (gk) | Transform, scale (gk) | -- |
| Boolean operation [Part: Cut / Fuse / Common] | Combine: Cut / Join / Intersect [13] | Combine (gk) | Boolean (gk) | Union / Subtract / Intersect (gk) |
| Split operation | Split Body (gk) | Split (gk) | Split (gk) | Split (gk) |
| Datum plane | construction plane, Offset Plane (gk) | Reference Plane (gk) | Plane (gk) | reference plane (gk) |
| Local coordinate system | -- (joint origin, gk) | Coordinate System (gk) | Mate connector [31] | coordinate system (gk) |
| Shape binder, Sub-shape binder | Project / Include 3D Geometry (gk) | Convert Entities (gk), Insert Part (gk) | Use [26], Derived (gk) | Include (gk) |
| Clone | -- | Insert Part (gk) | Derived (gk) | -- |
| [Part: 3D Offset] | Offset Face, Press Pull [3] (gk for Offset Face) | Offset Surface (gk) | Offset surface (gk) | Offset (gk) |
| Spreadsheet, VarSet, expressions | Change Parameters, user parameters (gk) | Equations, Global Variables (gk) | Variables, Configurations (gk) | Variable Table (gk) |

### 5.2 Sketcher

| FreeCAD Sketcher (default key) | Fusion (key [3]) | SOLIDWORKS | Onshape (key [26]) | Solid Edge |
|---|---|---|---|---|
| Line (G, L) | Line (L) | Line | Line (l) | Line |
| Rectangle (G, R) | 2-Point Rectangle (R) | Corner Rectangle (gk) | Rectangle (gk) | Rectangle (gk) |
| Circle from center (G, C) | Center Diameter Circle (C) | Circle | Circle (c) | Circle |
| Dimension (D) | Sketch Dimension (D) | Smart Dimension (gk) | Dimension (d) | Smart Dimension (gk) |
| Toggle construction geometry (G, N) | Normal / Construction (X) | For construction (gk) | Construction (q) | Construction (gk) |
| Create external geometry (G, X) | Project (P) | Convert Entities (gk) | Use (u) | Include (gk) |
| Carbon copy | -- | -- | -- | -- |
| Trim edge (G, T) | Trim (T) | Trim Entities (gk) | Trim (gk) | Trim (gk) |
| Offset (Z, T) | Offset (O) | Offset Entities (gk) | Offset (gk) | Offset (gk) |
| Coincident (C) | Coincident (gk) | Coincident (gk) | Coincident (gk) | Connect (gk) |
| Point on object (O) | Coincident (gk) | Coincident, Pierce (gk) | Coincident, Pierce (gk) | Connect (gk) |
| Horizontal (H), Vertical (V) | Horizontal/Vertical (gk) | Horizontal, Vertical (gk) | Horizontal, Vertical (gk) | Horizontal/Vertical (gk) |
| Parallel (P), Perpendicular (N), Tangent (T) | same (gk) | same (gk) | same (gk) | same (gk) |
| Equal (E), Symmetric (S) | Equal, Symmetry (gk) | Equal, Symmetric (gk) | Equal, Symmetric (gk) | Equal, Symmetric (gk) |
| Block (K, B) | Fix (gk) | Fix (gk) | Fix (gk) | Lock (gk) |
| Lock position (K, L) | Fix (gk) | Fix (gk) | Fix (gk) | -- |
| Leave sketch | Finish Sketch [12] | Exit Sketch [19] | checkmark (gk) | Close Sketch (gk) |

The Sketcher's single keys clash with Fusion's on the same letter with a
different meaning: C (coincident vs circle), T (tangent vs trim), O (point on
object vs offset), P (parallel vs project). D (dimension) agrees with Fusion
and Onshape.

### 5.3 Assembly

| FreeCAD Assembly (key) | Fusion | SOLIDWORKS | Onshape | Solid Edge |
|---|---|---|---|---|
| New Assembly (A) | (Design workspace itself; gk) | New Assembly (gk) | Assembly tab (gk) | Assembly (gk) |
| Insert Component (I) | Insert into current design (gk) | Insert Components (gk) | Insert (gk) | Place part (gk) |
| New Part (P) | New Component (gk) | New Part (gk) | -- | -- |
| Fixed Joint (F) | Rigid [10] | Lock mate [23] | Fastened mate [31] | rigid / ground (gk) |
| Revolute Joint (R) | Revolute [10] | Hinge mate [23] | Revolute mate [31] | axial align + mate (gk) |
| Cylindrical Joint (C) | Cylindrical [10] | Concentric mate [23] | Cylindrical mate [31] | Axial align [33] |
| Slider Joint | Slider [10] | -- (combination of mates, gk) | Slider mate [31] | -- |
| Ball Joint | Ball [10] | -- (gk) | Ball mate [31] | -- |
| Distance Joint | joint offset (gk) | Distance mate [23] | mate offset (gk) | mate with offset (gk) |
| Parallel / Perpendicular (M) / Angle Joint | -- (gk) | Parallel / Perpendicular / Angle mates [23] | Parallel mate [31] | parallel, angle [33] |
| Rack and Pinion Joint (Q) | Motion Link (gk) | Rack and pinion mate [23] | Rack and pinion relation [31] | -- |
| Screw Joint | Motion Link (gk) | Screw mate [23] | Screw relation [31] | -- |
| Gears Joint | Motion Link (gk) | Gear mate [23] | Gear relation [31] | gear [33] |
| Belt Joint | -- | -- | -- | -- |
| Toggle Grounded | Ground (gk) | Fix (gk) | Fix (gk) | ground [33] |
| Exploded View (E) | Animation workspace (gk) | Exploded View (gk) | Exploded view (gk) | Explode (gk) |
| Bill of Materials (O) | Bill of Materials (gk) | BOM (gk) | BOM (gk) | parts list (gk) |
| Simulation (V) | Motion Study (gk) | Motion Study (gk) | -- | -- |

Note the shortcut clash: our Assembly's F is Fixed Joint; Fusion's F is
Fillet [3]; SOLIDWORKS' and Onshape's f is fit to window [21][26].

## 6. Where FreeCAD differs, and what it costs a newcomer

Concretely, for someone who has modelled a bracket in Fusion and opens this
fork to do the same:

1. **Words do not find the tools.** `/cmd extrude` matches by title, name,
   shortcut and tooltip. It finds `PartDesign_Extrusion` (this fork's
   standalone extruded shape: "The resulting shape standalone and not merged
   into tip", `Command.cpp` 1205) and Part's "Extrude...", both the WRONG
   tool for adding material to a body; it does not find Pad, whose tooltip is
   "Pad a selected sketch". `/cmd shell` finds nothing in PartDesign
   (Thickness: "Make a thick solid"). `/cmd project` does not find "Create
   external geometry". "fix" finds Block ("Constrains the selected edges as
   fixed") by luck. "rigid", "mate", "cut extrude", "combine", "rectangular
   pattern" find nothing. Read from the tooltips in the source, not run.
2. **Two kits of the same tools.** Part (Extrude, Revolve, Loft, Sweep,
   Thickness, Cut/Fuse/Common) and PartDesign (Pad, Revolution, Additive
   loft, Additive pipe, Thickness, Boolean) overlap in purpose and differ in
   name and in what they act on. A newcomer picks by name, and the familiar
   names are on the Part side. Every other program surveyed has ONE kit.
3. **Additive and subtractive are separate commands.** Fusion and Onshape
   have one Extrude with an operation field [8][30]; SOLIDWORKS has separate
   Boss and Cut tools [24] and is the nearest to us. A Fusion user looks for
   the Cut toggle in the Pad panel and does not find it; changing a Pad into
   a Pocket is delete and redo.
4. **Not a gap at a sketch; a gap elsewhere.** (Corrected 2026-10-26 by the
   user; the first draft called this a cost.) Editing a sketch brings up the
   Sketcher's tools from whatever workbench is active and gives the previous
   set back on leaving (section 2: `EditingWorkbench` through TempoVis, the
   edit-mode toolbars forced available in `Sketcher/Gui/Workbench.cpp`). That
   is Fusion's contextual Sketch tab [12] and SOLIDWORKS' CommandManager swap
   in effect. The gap is that other editing contexts do not follow the
   pattern: a TechDraw page and an assembly still need the user to switch
   workbench by hand -- proposal 7.5 (the spreadsheet deliberately not).
5. **Assembly is another workbench.** Fusion places joints in the same
   Design workspace as features [3][10]; here Insert, Joint and Ground are in
   the Assembly workbench, and a part is edited in PartDesign. A switch per
   round trip, auto-switch on a Body double-click softening one direction.
6. **Body and Tip are concepts without a counterpart a newcomer recognises.**
   The Body is close to Fusion's body and Onshape's part, but it must be
   active to receive features: a selection in another body is refused ("Select
   an edge, face, or body from an active body.", `Command.cpp` 1760) where
   Fusion and Onshape simply act on what is selected. The Tip is the
   rollback bar, but it is a property set from a context menu ("Set tip"), not
   a thing one drags.
7. **History: not a gap.** (Corrected 2026-10-26 by the user.) A Body's
   PartDesign features ARE the history steps of Fusion's timeline [6] and of
   the SOLIDWORKS / Onshape feature tree [17][28]: each feature is one step,
   in order, and each holds its own result shape. Rewinding is done by
   visibility -- show an earlier feature and that is the part at that step --
   which is faster than a rollback, because nothing is recomputed; Set tip
   is there when later features should really be ignored. None of this is the
   undo/redo transaction history (docs/TransactionLog.md), which Fusion has
   too and which is a different thing. What a Fusion user lacks is only the
   word: they look for "timeline" and "rollback", which proposal 7.1's
   aliases and 7.2's "coming from" help can point at the Body tree and at
   showing a feature.
8. **Mouse.** The default CAD style (MMB pan, MMB+LMB rotate) matches none of
   the four; every one of them is one setting away (section 2), but only if
   the user knows the setting exists and which style is "theirs". The names do
   not help: a Fusion user will not guess "Revit", an Onshape user will not
   guess "TinkerCAD", and "OpenInventor" reads as Autodesk Inventor.
9. **Keys.** PartDesign has no single-key shortcuts at all. The Sketcher and
   Assembly do, on letters that mean something else elsewhere (5.2, 5.3).
   The view keys differ too: 0-6 here, F / Space / Ctrl+1.. in SOLIDWORKS,
   shift+digits in Onshape.
10. **Front is not the same plane.** SOLIDWORKS (gk) and Fusion by default
    [38] are Y-up, so their Front plane is XY; FreeCAD is Z-up and its Front
    view looks at XZ. A newcomer who sketches "on the front plane" by habit
    picks XY here, which is our Top. Nothing to fix in us; worth one line in
    the "coming from" help.
11. **Command-first can dead-end in a message box.** Pad with nothing
    selected opens a feature picker; a dress-up with a selection in another
    body, or two bodies, stops with a `QMessageBox` (`Command.cpp` 1754-1760).
    Onshape's dialog asks for the selection in a blue field [29]; Fusion's
    dialog waits for it (gk).

What is NOT a gap, against this survey: on-view handles and typed values
(built, OnViewValueEditing.md), search over everything (built), selection-
first for dress-ups (built), a pie or pop-up toolbox (built, unbound).

## 7. Proposals, ranked by value over cost

Sizes are rough: S = days, M = one to two weeks, L = more. "Upstream" is the
merge risk against FreeCAD `main`.

### 7.1 Search aliases from the crosswalk -- value high, size S

- What. Each command carries ALIASES: the names other programs use, each
  tagged with the program(s). `/cmd extrude` lists Pad first, described "Pad
  -- Extrude (Join) in Fusion, Extruded Boss/Base in SOLIDWORKS, Extrude (Add)
  in Onshape", then Pocket ("Extrude (Cut) ..."), then Part's Extrude and
  PartDesign Extrusion with their own descriptions. `/cmd shell` gives
  Thickness; `/cmd project` external geometry; `/cmd mate` and `/cmd rigid`
  the joints. Solid Edge's Command Finder is the precedent [32].
- Where. `CommandListModel::data()` `SearchTextRole` and `DescriptionRole`
  (`src/Gui/CommandCompleter.cpp` 125, 142); a data file of aliases (one row
  per command: name, alias, program, a note such as "operation Cut") loaded
  once, e.g. `src/Gui/Resources/CommandAliases.json` or a Python table a module
  registers like `freecad.params` does for parameters; the browser catalog
  rows (`OmniControl.cpp` `Catalog`) gain the field so the viewer box matches
  the same way. Ranking: an alias hit sorts after a title hit, and the
  description says which alias matched.
- Gap to close with it. Commands of unloaded modules are not listed today
  (OmniSearch.md sec 1). An alias row for a command of an unloaded workbench
  should still appear ("Pad -- PartDesign, loads the workbench") and run
  `Std_Workbench` then the command. That is the M part of this item if wanted.
- Also: the crosswalk could be offered for TOOLTIPS ("also known as Extrude
  in ...") -- cheap, but tooltip text is translated and upstream-owned;
  keeping the aliases in one fork-owned file avoids touching every command.
- Risks. Almost none upstream: the model is the fork's. Product names in
  the UI are nominative use, as Command Finder [32] and SolidCraft's README [1]
  do; a disclaimer line in About is cheap. Aliases are English; translators
  may add localised ones, but the product terms themselves are what users
  type. Wrong mappings mislead -- every row should be checked against the
  program's help, and section 5's "(gk)" rows first.

### 7.2 "Coming from ..." on the first-start panel -- value high, size S to M

- What. One more choice on the first-start panel, above the navigation
  style: "I am coming from: FreeCAD / Fusion / SOLIDWORKS / Onshape / Solid
  Edge / Blender / none". It sets a bundle, each item visible and changeable
  afterwards:
  - navigation style -- Fusion -> Revit style (MMB pan, Shift+MMB orbit,
    matching [3]); SOLIDWORKS -> SolidWorks style ([22]); Onshape ->
    TinkerCAD style (RMB rotate, MMB pan, RMB click menu; Ctrl+RMB pan not
    checked); Blender -> Blender; Solid Edge -> not mapped until its mouse is
    verified. Plus `ZoomAtCursor`, `InvertZoom` to match, each to be checked
    against the program by hand.
  - the search aliases of 7.1 ranked for that program first;
  - an optional SHORTCUT SET for PartDesign and the Sketcher (7.3);
  - the S toolbox of 7.3;
  - the Std view keys left alone.
- Also in Preferences > Navigation: show the style's user-facing name with
  the programs it resembles ("Revit -- also Fusion's default"), so the
  mapping is findable later; and rename the "OpenInventor" entry's display
  to say "Coin / Open Inventor viewer" (a display string only).
- Where. `GeneralSettingsWidget.cpp` (one combo), a preset table (C++ or a
  `freecad.params`-style Python file), `ShortcutManager` for the key set.
  Per docs/UserSettings.md the bundle is a per-user setting: a browser user
  coming from Onshape gets Onshape's mouse in their viewer, independent of
  the host -- which also needs the browser viewer's own gesture table
  (ThinClientUI.md "Camera") to take a preset.
- Risks. Upstream: the Start module is upstream's and changes; the widget
  addition is small. Existing users: the panel only shows at first start;
  nothing changes for them. A preset that changes shortcuts must say so and
  be undoable (store the previous set).

### 7.3 A default S toolbox, and an optional key set -- value medium-high, size S

- What. Ship, per workbench, a "Toolbox" toolbar (hidden as a bar) bound to
  S through the existing `ToolbarMenuAction` / pie machinery: the modelling
  commands most used in that workbench, with a `/cmd` search line at the top
  (the omni box opened in command mode at the cursor). This is Fusion's Model
  Toolbox [3], SOLIDWORKS' shortcut bar [14] and Onshape's s [26] -- three
  programs, the same key, so S is the right default.
- The key. S is a chord prefix here (`S, B` Std_SelBack and `S, F`
  Std_SelForward, `CommandView.cpp` 4374 and 4418; `S, G`
  Std_LinkSelectLinked, `CommandLink.cpp` 905), so a bare S fires only
  after the 300 ms `ShortcutTimeout`. Either move those three chords or
  accept the delay. They are core `Std_` commands upstream has as well, so
  moving them changes a default every FreeCAD user knows -- better done as
  part of the "coming from" bundle (7.2) than for everyone.
- The optional key set (only through 7.2, never by default): Fusion-like
  single keys for PartDesign -- E Pad, H Hole, F Fillet (Fusion's Q, Press
  Pull, has no counterpart and stays unbound), and the Sketcher's L, R, C,
  T, O, P, X as Fusion has them [3]. It collides with the Sketcher's own single keys (C, T, O, P mean
  constraints today, 5.2) -- so it is a swap of the Sketcher's set while the
  preset is on, not an addition; the ShortcutManager's priority handles
  workbench-vs-workbench clashes but not two meanings in one workbench.
- Where. `src/Mod/*/Gui/Workbench.cpp` (`setupToolBars` gets one more
  `DefaultVisibility::Hidden` item) or a fork-owned table applied by a
  `WorkbenchManipulator` (`src/Gui/WorkbenchManipulator.cpp`), which avoids
  editing upstream's workbench files; `DlgToolbarsImp`'s parameters for the
  shortcut and pie flag.
- Risks. Low with a manipulator. A key set that moves the Sketcher's letters
  breaks muscle memory of existing users -- hence never default.

### 7.4 Selection-first that follows the selection -- value medium-high, size S to M

- What. Three small changes in PartDesign's command layer:
  1. A feature command whose selection lies in a body other than the active
     one ACTIVATES that body (with a status-bar note) instead of refusing
     ("Selection is not in Active Body", `Command.cpp` 1757-1761). The
     machinery is `makeBodyActive` / `getBody(..., autoActivate)` already.
  2. A command with a wrong or missing selection opens its task panel with
     the selection field waiting (Onshape's blue field [29]) instead of a
     `QMessageBox`, where the panel already has a selection mode (dress-ups,
     Pad's profile, Hole's positions).
  3. A selection of SEVERAL bodies for a dress-up: one feature per body,
     or ask -- to be decided (open question).
- Where. `src/Mod/PartDesign/Gui/Command.cpp` (`dressupGetSelected`,
  `prepareProfileBased`), `Utils.cpp`.
- Risks. Upstream conflicts in `Command.cpp` (upstream edits it often);
  keep the change in a helper. Activating a body silently surprises an
  existing user in a multi-body document -- show it, and keep a preference to
  refuse as before.

### 7.5 Fewer whole-UI switches for part modelling -- value high, size M

- What. Two steps, separable:
  1. **TechDraw follows the sketch's pattern, without the flicker.**
     (Rewritten 2026-10-26: the first draft proposed changing the sketch,
     which already does this -- section 6 item 4.) A TechDraw page's tools
     come up when its view is active, from whatever workbench, and go when
     another view is. Assembly in edit may follow later. **The spreadsheet
     does not** (user, 2026-10-26): not until its switch can be subtler than
     a toolbar change.

     The constraint is the split view (docs/SplitViews.md): a page and a 3D
     view now sit side by side in cells, and clicking between them is
     frequent. Copying the sketch's mechanism -- a global workbench switch
     on each activation -- would swap the global toolbars on every click,
     and that flicker is already annoying where it happens today. A switch
     reuses the toolbars (`ToolBarManager::setup` shows and hides them, it
     does not rebuild them), so the visible cost is most likely layout: the
     two sets have different widths, the toolbar rows reflow, a row
     appears or goes, and every view under them resizes and redraws. Not
     measured yet; the first step is to record a switch and confirm that.

     Design, in order of preference:
     a. **The page owns its tools (recommended).** The page's cell carries
        TechDraw's toolbar in its own header strip, the way every Blender
        area has its own header with that editor's tools (SplitViews.md
        sec 1 takes Blender as the reference already). Activating a cell
        then changes nothing global: no workbench switch, no toolbar
        reflow, no menu rebuild. The global toolbars stay those of the
        workbench the user chose for modelling. The commands are the same
        `Gui::Command`s; a click on a cell's strip activates that cell
        first, so a command finds its page as the active view as now. A
        page opened as a plain tab shows the same strip. What still needs
        the TechDraw workbench (its menus, the preferences page) keeps it.
     b. **If a global switch stays, make it layout-stable.** Only the
        context's own toolbars are swapped, into a reserved slot whose size
        does not change (one "context" toolbar position, as the sketch's
        edit bars could also use); the shared ones are never touched; the
        main window paints once at the end (`setUpdatesEnabled` across the
        switch); and the switch waits for intent -- an activation by a
        click inside the page, not by focus passing through it.
     The sketch keeps its global switch: an edit is a mode the user enters
     on purpose and stays in, not a view clicked past.
  2. **One "Part modelling" set.** PartDesign's bars plus what a part
     modeller reaches for across workbenches today: the Boolean, Measure,
     Std_LinkActions, Insert Component and the basic joints (Fixed,
     Revolute, Slider) from Assembly when an Assembly is active, TechDraw's
     "new page" from the selection. Done as a WorkbenchManipulator or a new
     composite workbench, so PartDesign's own definition stays upstream's.
     NX's Essentials role [35] is the precedent for a smaller set by
     intent; Fusion's single Design workspace [4] for one place to model
     parts and joints.
- Where. Step 1a: `src/Mod/TechDraw/Gui/MDIViewPage.cpp` (a header strip
  built from TechDraw's toolbar items), `src/Gui/ViewArea.cpp` (a cell's
  header for the hosted view's tools). Step 1b: `ToolBarManager::setup`,
  `MainWindow::onWindowActivated`. Step 2:
  `src/Mod/PartDesign/Gui/Workbench.cpp` or a manipulator, `ToolBarManager`.
- Risks. 1a: a page in a narrow cell has little room for a strip (it can
  fold into a menu button, as Blender's headers do); users' toolbar
  customisations of TechDraw must reach the strip. 1b: toolbar state save
  and restore across workbenches is fiddly (the comment at Sketcher
  `Workbench::activated` admits losses already). Upstream: low for step 1,
  medium for step 2.

### 7.6 A selection-contextual mini bar on the desktop -- value medium, size M

- What. Shapr3D's adaptive tools [36] and SOLIDWORKS' context toolbar [15]:
  after a pick in the 3D view, a small bar of icons near the pick with what
  applies to it. The SETS exist: PartDesign's `TaskWatcherCommands` filters
  and lists (`Workbench.cpp` 241-484) say exactly "face -> Sketch, Pad,
  Pocket, Fillet ...". Draw them at the selection instead of (or as well as)
  in the task panel.
- Where. A new widget under `src/Gui/` (a frameless tool window like
  `OmniSearchBox`), fed by the active workbench's watchers through a
  data-only description, which is also docs/ThinClient.md sec 5's "operation
  catalog" -- one catalog for the desktop bar, the browser's contextual bar
  and an agent.
- Risks. Visual noise: fade in after a pause, off by preference, never over
  the pick. Upstream: none (new files). It overlaps 7.3's toolbox; together
  they are SOLIDWORKS' pair (S bar + context bar [14][15]).

### 7.7 A Body history timeline -- WITHDRAWN 2026-10-26

The first draft proposed a Fusion-style strip of the Body's features with a
draggable rollback marker. Withdrawn on the user's correction: the Body's
features already are that history, and visibility already rewinds it,
faster than a rollback because nothing recomputes (section 6 item 7). A
second widget over the same list adds nothing; the words "timeline" and
"rollback" go into 7.1's aliases instead. The undo/redo transaction history
stays a separate thing (docs/TransactionLog.md).

### 7.8 Extrude-with-operation as a panel switch -- value medium, size M, later

- What. In the Pad and Pocket panels a "Join / Cut" switch that turns the
  feature into the other type in place (a new object of the other type
  taking the same profile, parameters, name position in the Body and
  expressions, then the old one removed), so a Fusion or Onshape user's
  mental model -- one Extrude, pick the operation [8][30] -- works without a
  new feature type. Same for Revolution/Groove, the pipes, lofts, helices.
- Risks. Expressions and references to the old object, TNP references from
  later features, undo as one step. It is the one item that touches the
  document model, so it waits for 7.1 to show whether the name alone was the
  problem.

### 7.9 Not proposed

- A ribbon. Fusion's ribbon is trade dress as much as layout, and FreeCAD's
  toolbars can already be arranged into tabs by the overlay and toolbar
  machinery; the gain is in 7.5's fewer switches, not in the ribbon's look.
- Renaming commands. Upstream is already renaming (Revolve, Mirror); aliases
  give the same findability without a translation and merge cost, and keep
  every existing tutorial valid.
- Synchronous / direct editing. Out of scope (3.4); noted as the nearest
  analogue to Press Pull, which 7.1 should map to Part's offset and to Pad
  on a face with a note, honestly, rather than pretend.

### 7.10 Order, if all were wanted

7.1 (aliases) -> 7.2 (coming from, with navigation only) -> 7.3 (S toolbox)
-> 7.4 (selection follows body) -> 7.5 step 1 (TechDraw follows the
sketch's pattern) -> 7.6 (context bar, with ThinClient's catalog) -> 7.5
step 2 -> 7.8. The first three are a week together and touch almost nothing upstream.

## 8. Open questions for the user

a. Aliases: in the search only, or also in tooltips? Product names visible
   in the UI at all, or only "also called Extrude" without naming programs?
b. Unloaded workbenches: should `/cmd` list (and on pick load) commands of
   workbenches not loaded yet? It is what makes the aliases useful from the
   Start page.
c. "Coming from": which programs in the list? Is Onshape -> TinkerCAD style
   acceptable, or a new "Onshape-like" style with Ctrl+RMB pan and Alt+RMB
   upright rotate [27]?
d. S as the toolbox key: move `S, B` / `S, F` / `S, G` to free it (for
   everyone, or only under a "coming from" preset), or keep them and live
   with the 300 ms wait?
e. Fusion-like single keys as an opt-in set: wanted at all? If so, does
   the Sketcher's set swap (C circle instead of coincident) while it is on?
f. Selection in another body: activate that body (with a note) by default,
   or keep refusing by default and make activating a preference?
g. TechDraw (7.5.1): the page's own tool strip (a), or a layout-stable
   global switch (b)? Does anything of TechDraw's need the global menus
   while a page is active?
h. The "Part modelling" set (7.5.2): a manipulator over PartDesign, or a new
   workbench with its own name? Should Assembly's joints appear there?
i. (Withdrawn with 7.7.)
j. Extrude-with-operation (7.8): worth the document-model change, or do
   aliases suffice?
k. Should the browser viewer follow the same "coming from" preset for its
   own mouse and touch gestures (ThinClientUI.md "Camera")?

## Sources

1. SolidCraft README: https://github.com/bherbruck/solidcraft
2. Botmonster, "SolidCraft is a Fusion 360 alternative AI wrote in 10 days":
   https://botmonster.com/ai/solidcraft-is-a-fusion-360-alternative-ai-wrote-in-10-days/
3. Fusion keyboard shortcuts reference:
   https://help.autodesk.com/cloudhelp/ENU/Fusion-GetStarted/files/GUID-F0491540-0324-470A-B651-2238D0EFAC30.htm
4. Fusion, Workspaces:
   https://help.autodesk.com/cloudhelp/ENU/Fusion-GetStarted/files/GS-WORKSPACES.htm
5. Fusion, Marking menu reference:
   https://help.autodesk.com/cloudhelp/ENU/Fusion-GetStarted/files/GUID-6514ABC1-CB75-4F0B-AB0E-316FAD36BA93.htm
6. Fusion API, Custom Features (timeline roll-back on edit, the marker):
   https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/CustomFeatures_UM.htm
7. Fusion, Parametric and Direct modelling modes:
   https://help.autodesk.com/cloudhelp/ENU/Fusion-Designs/files/ASM-CAPTURE-DESIGN-HISTORY.htm,
   https://help.autodesk.com/cloudhelp/ENU/Fusion-Designs/files/ASM-DO-NOT-CAPTURE-DESIGN-HISTORY.htm
   (via search summary)
8. Fusion, Extrude:
   https://help.autodesk.com/cloudhelp/ENU/Fusion-Model/files/SLD-EXTRUDE-SOLID.htm
9. Fusion, Mirrors and patterns:
   https://help.autodesk.com/cloudhelp/ENU/Fusion-Model/files/SLD-PATTERNS.htm
   (via search summary)
10. Fusion, Joint types:
    https://help.autodesk.com/cloudhelp/ENU/Fusion-Assemble/files/GUID-8818AE31-958A-4A59-989B-9875A174C67A.htm
    (via search summary)
11. Fusion blog, pan / zoom / orbit preferences (403 when fetched; via search
    summary): https://www.autodesk.com/products/fusion-360/blog/quick-tip-pan-zoom-orbit-preferences/
12. Fusion, Start a sketch / Sketch Palette (via search summary):
    https://help.autodesk.com/cloudhelp/ENU/Fusion-Sketch/files/GUID-88CC0E51-AD05-4028-BF59-FACA5EC0FA2B.htm,
    https://help.autodesk.com/cloudhelp/ENU/Fusion-Sketch/files/GUID-4183A4B7-E002-4396-AD5A-7FF3C8B2F33A.htm
13. Fusion, Modify tools (Shell, Draft, Combine; via search summary):
    https://help.autodesk.com/view/fusion360/ENU/?guid=GD-MODIFY-TOOLS
14. SOLIDWORKS, Shortcut Bars (via search summary):
    https://help.solidworks.com/2026/English/SolidWorks/sldworks/Chunk1122975052.htm
15. SOLIDWORKS, Context Toolbars (via search summary):
    https://help.solidworks.com/2022/english/SolidWorks/sldworks/c_context_toolbars.htm
16. SOLIDWORKS, CommandManager (via search summary):
    https://help.solidworks.com/2021/english/SolidWorks/sldworks/c_commandmanager.htm
17. SOLIDWORKS, Rollback Bar (via search summary):
    https://help.solidworks.com/2024/english/SolidWorks/sldworks/c_rollback_bar.htm
18. SOLIDWORKS, FeatureManager Design Tree Overview (via search summary):
    https://help.solidworks.com/2016/english/solidworks/sldworks/c_featuremanager_design_tree_overview.htm
19. SOLIDWORKS, Exiting Sketches (via search summary):
    https://help.solidworks.com/2025/english/solidworks/sldworks/t_Exiting_Sketches.htm
20. SOLIDWORKS, Moving the Confirmation Corner options to the pointer (via
    search summary): https://help.solidworks.com/2016/english/whatsnew/c_command_confirmation.htm
21. SOLIDWORKS, Selected Keyboard Shortcuts (script-rendered; via search
    summary): https://help.solidworks.com/2023/english/solidworks/sldworks/c_time_saving_keyboard_shortcuts.htm
22. SOLIDWORKS, Middle Mouse Button Functions (via search summary):
    https://help.solidworks.com/2024/English/SolidWorks/sldworks/r_Middle_Mouse_Button.htm
23. SOLIDWORKS, Mate PropertyManager tabs (via search summary):
    https://help.solidworks.com/2026/english/solidworks/sldworks/r_mate_pm_mechanical.htm,
    https://help.solidworks.com/2024/english/SolidWorks/sldworks/r_types_of_mates_swassy.htm
24. SOLIDWORKS, Extrude PropertyManager and Features toolbar (via search
    summary): https://help.solidworks.com/2024/english/SolidWorks/sldworks/r_extrude_propertymanager.htm,
    https://help.solidworks.com/2021/english/SolidWorks/sldworks/r_Features_Toolbar_features.htm
25. SOLIDWORKS, Overview of Instant3D (via search summary):
    https://help.solidworks.com/2023/english/SolidWorks/Sldworks/c_instant3d_functionality.htm
26. Onshape, Keyboard shortcuts: https://cad.onshape.com/help/Content/shortcut_keys.htm
27. Onshape, View navigation, and tech tip on changing rotate / pan / zoom
    (via search summary):
    https://cad.onshape.com/help/Content/View/view_navigation_and_the_view_cube.htm,
    https://www.onshape.com/en/resource-center/tech-tips/tech-tip-changing-rotate-pan-and-zoom
28. Onshape, Feature and Part lists (via search summary):
    https://cad.onshape.com/help/Content/PartStudio/features_and_parts_lists.htm
29. Onshape, User Interface Basics:
    https://cad.onshape.com/help/Content/Home/user_interface_basics.htm
30. Onshape, Extrude: https://cad.onshape.com/help/Content/extrude.htm
31. Onshape, Mates and Relations (via search summary):
    https://cad.onshape.com/help/Content/Assembly/mates.htm,
    https://cad.onshape.com/help/Content/relations.htm
32. Solid Edge ST6 help, user interface tour (a third-party mirror of the
    Siemens help), and Command Finder:
    https://soliddna.com/SEHelp/ST6/EN/user_interface/guitour1a.htm,
    https://www.soliddna.com/SEHelp/ST5/EN/a_h/cmfind1h.htm,
    https://blogs.sw.siemens.com/solidedge/the-solid-edge-command-finder-is-your-friend/
    (the last two via search summary)
33. Solid Edge ST6 help, assembly relationships (mirror; via search summary):
    https://soliddna.com/SEHelp/ST6/EN/assemble_parts/const1a.htm
34. Siemens, Synchronous Technology, and the hybrid modelling blog (vendor
    pages; via search summary):
    https://solidedge.siemens.com/en/solutions/products/3d-design/synchronous-technology/,
    https://blogs.sw.siemens.com/solidedge/hybrid-modeling-in-solid-edge-with-syncronous-and-ordered-modes/
35. Siemens NX roles, and Command Finder in NX 2206 (via search summary):
    https://blogs.sw.siemens.com/designcenter/nx-roles/,
    https://support.sw.siemens.com/en-US/okba/PL8663295/Unable-to-add-command-to-Ribbon-Tab-after-using-command-finder-Search/index.html
36. Shapr3D, Selecting geometry and Adaptive user interface (403 when
    fetched; via search summary):
    https://support.shapr3d.com/hc/en-us/articles/7770768736924-Selecting-geometry,
    https://support.shapr3d.com/hc/en-us/articles/7873882619548-Adaptive-user-interface
37. Plasticity manual, Command Palette, Selection Mode, command list (via
    search summary): https://doc.plasticity.xyz/plasticity-essentials/command-palette,
    https://doc.plasticity.xyz/plasticity-essentials/selection-mode,
    https://doc.plasticity.xyz/all-commands
38. Warwick WMG, Fusion axis orientations and units (third-party tutorial;
    via search summary):
    https://warwick.ac.uk/fac/sci/wmg/about/outreach/resources/fusion_tutorials/fusion_axis_orientations_and_units_preferences.pdf
