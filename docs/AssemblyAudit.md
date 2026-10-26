# Assembly: an audit against other CAD systems, and at scale

Status (2026-10-22): audit done, plan decided (sec 8), no product change
yet. Branch `AssemblyPort`, cut from `LinkVibe` `27e32d75f1`. This document
records where the Assembly workbench stands against the commercial and
open-source assembly tools, the defects found while reading it, and what it
costs on large assemblies, measured. Sec 8 is the plan the user set; its
companions are the upstream ledger (`docs/AssemblyPort-ledger.tsv`, with
`docs/AssemblyPort-ledger-notes.txt`), the solver interface design
(`docs/AssemblySolverInterface.md`) and the Assembly3 study with its
importer (`docs/AssemblyFromAsm3.md`).

Sources: three audits run 2026-10-22 -- a web survey of eight commercial
systems and the FreeCAD-family workbenches, an inventory of this tree with
file:line pointers, and a static plus measured performance audit. The
benchmark is kept as `scripts/assembly-bench/` (sec 4.4). Paths below are
relative to `src/Mod/Assembly/` unless they start at the repo root.

Related: `docs/Simulator.md` (the plan for dynamics, out of process),
`docs/DocumentLoad.md`, `docs/TShapeRenderCache.md`,
`docs/FarFieldProxies.md` (the fork's large-model machinery),
`docs/PartDesignPort.md` and `docs/SketcherPort.md` (how earlier upstream
ports were tracked).


## 1. What we have

The workbench is upstream FreeCAD's, ported whole by `759cd15c9c`
(2026-08-06): about 9,600 lines of C++ and 9,900 of Python, solved by
OndselSolver (`src/3rdParty/OndselSolver`, our fork, at parity with
upstream's pin). Fork-only changes since: mapped element names kept out of
object paths (`0a7ea71821`), `Type` set to "Assembly" in the constructor
(`5c79a60496`; upstream still lacks it), this fork's datums recognised
(`fe785a7bc8`), the ported `SoTransformDragger` (`b223d1b31a`), settings
behind generated classes.

The port did not record its upstream baseline; the ledger pins it to
upstream `17f203e118` (2026-08-03). The port's `src/Mod/Assembly` equals
upstream's there except for 17 files (+132/-74), all adaptations its
message lists, and its translations match that commit byte for byte.
OndselSolver is at parity: the fork's pin `ccb521b` and upstream's
`4be80eef02` have identical trees.

| area | what is here | pointer |
|---|---|---|
| joints | 13 types: Fixed, Revolute, Cylindrical, Slider, Ball, Distance, Parallel, Perpendicular, Angle, RackPinion, Screw, Gears, Belt; plus GroundedJoint. Distance maps to ~40 solver joints by the geometry picked. Length and angle limits. | `JointObject.py:65-143`, `App/AssemblyUtils.h:49-125`, `App/AssemblyObject.cpp:1192-1428` |
| solver | full rebuild per solve, run as the solver's pre-drag pass; DoF count; redundant joints listed | `App/AssemblyObject.cpp:168-294` |
| drag | preDrag / doDragStep / postDrag, Fixed groups bundled while dragging, 8 drag modes picked from the part's joints | `App/AssemblyObject.cpp:368-512`, `Gui/ViewProviderAssembly.cpp:927-1149` |
| exploded views | named views of Normal or Radial steps, trail lines, temporary explode on selection | `CommandCreateView.py` |
| motion | kinematic simulation: Angular / Linear formulas of time on Revolute, Slider, Cylindrical; playback; GIF / MP4 / AVI / WebM export | `CommandCreateSimulation.py`, `App/AssemblyObject.cpp:296-361` |
| BOM | a live spreadsheet: index, name, file, quantity, property columns, mirrored parts apart | `App/BomObject.*` |
| snapshots | placements and visibilities, restored on double-click | `CommandCreateSnapshot.py` |
| sub-assemblies | AssemblyLink, Rigid or Flexible | `App/AssemblyLink.cpp` |
| insertion | insert link (one App::Link per insert), insert new part in context | `CommandInsertLink.py`, `CommandInsertNewPart.py` |
| exchange | ASMT export; ASMT import is a stub | `App/AssemblyObject.cpp:569`, `AssemblyImport.py:25-32` |

Outside the workbench, the fork has a large-model stack an assembly can
stand on: App::Link and Part::LinkArray* (one shape behind N placements),
partial and progressive document load (a 17058-object assembly opens in
19.1 s instead of 32.7 s, `docs/DocumentLoad.md` sec 5), deferred shape
load, content-addressed shape storage with congruent-instance dedup, TShape
tessellation sharing and bgfx instanced draws (`docs/TShapeRenderCache.md`),
a SceneLadder LOD, and far-field proxies and occlusion culling (both built,
both off by default).


## 2. Against other systems

Surveyed: SOLIDWORKS, Inventor, Creo, NX, CATIA/3DEXPERIENCE, Solid Edge,
Onshape, Fusion; upstream FreeCAD 1.1, Assembly3, Assembly4, A2plus.
"Baseline" is what a mid-range system is expected to have; "high end" is
what distinguishes the top tier.

| area | baseline | ours | verdict |
|---|---|---|---|
| joints | rigid, revolute, slider, cylindrical, ball, planar, distance, angle, parallel/perpendicular, tangent, limits, joints between local frames; gear and rack-pinion | all but tangent and a first-class planar joint (only through Distance = 0); gears, rack, screw, belt beyond baseline | baseline met but tangent; missing high end: cam, slot / pin-slot, path, universal, width / symmetric, linear coupler, joint inference from picks |
| solver feedback | drag through the solver; under / fully / over-defined state per component; unsatisfiable joints shown | a DoF count and a redundant-joint list for the whole assembly; conflicting, partially redundant and malformed are never reported (sec 3) | below baseline: no per-component state, no conflict report |
| exploded views | named views of editable translate/rotate steps, usable in drawings | steps, radial, trail lines; no drawing link (`getExplodedShape` has no caller); steps not editable after the fact (upstream added that, sec 6) | below baseline on drawings and editing; missing high end: explode animation, auto-explode, smart lines |
| motion | drive joints over time with profiles, named positions, video export | met | missing high end: dynamics (mass, gravity, springs, dampers, contact), result plots, trace curves, table-driven profiles |
| collision | a static interference check listing pairs and showing the overlap; minimum distance | **nothing** | **the largest gap**, against every commercial system including Onshape. No FreeCAD assembly workbench has it, upstream has no open PR for it |
| large assemblies | a lighter open mode, saved display states, rigid sub-assemblies as one body, instanced graphics | instancing, partial and deferred load, LOD in the renderer -- none of it surfaced as an assembly mode | the infrastructure is ahead of the workbench; the workbench defeats parts of it (sec 5) |
| structure | rigid/flexible sub-assemblies, linear/circular component patterns, replace keeping joints, configurations, indented BOM, in-context parts, standard parts | sub-assemblies, BOM, in-context parts; patterns only as hand-made Link arrays; no mirror, no replace-keeping-joints, no configurations | below baseline on patterns, replace and configurations |
| other | measure incl. minimum distance, assembly mass properties, section view | core Measure, `distToShape`, clip planes; mass properties not wired into the assembly | partly met |

The high-end items most worth knowing for this fork's direction:

- **Collision while dragging** (SOLIDWORKS Collision Detection with stop and
  face highlight, Physical Dynamics, Dynamic Clearance; Inventor Contact
  Solver; Fusion contact sets) and **clearance zones** per object and pair
  (NX). Faceted versus exact checking is offered by NX and CATIA -- the
  fork's tessellation tables make a faceted mode cheap.
- **A representation ladder per component** (Creo: master, geometry,
  graphics, light graphics, symbolic, bounding box; SOLIDWORKS: resolved,
  lightweight, Large Assembly Mode, Large Design Review), switched
  automatically above a component count, and **envelope substitutes** that
  keep BOM and joints (SpeedPak, Inventor and Creo Shrinkwrap). CATIA's
  progressive, viewpoint-driven lightweight open is the same idea as the
  fork's progressive load and far-field proxies.
- **Solver diagnostics**: SOLIDWORKS MateXpert isolates the minimal
  over-defining group; its Performance Evaluation attributes rebuild time to
  mates. No vendor documents an incremental solver, but all solve a rigid
  group as one body.

The full survey, with a source link per claim, was written to the session
scratchpad and is summarised here; the tables above carry what it concluded.


## 3. Defects found on the way

Each was checked against the code.

1. **Conflicts are never reported.** `lastHasConflict`,
   `lastHasPartialRedundancies` and `lastHasMalformedConstraints` are set
   false in the constructor (`App/AssemblyObject.cpp:110-113`) and never
   again; the messages panel's "Malformed" and conflict links cannot appear
   (`Gui/ViewProviderAssembly.cpp:1840-1910`).
   **Fixed 2026-10-23, `f3920f1797`.** Upstream does not fill them either
   (checked at `d3f4da791d`). OndselSolver removes redundancy per equation
   (rows of the Jacobian found dependent become `RedundantConstraint`s) but
   cannot tell a repeated equation from a contradicting one, so
   `updateSolveStatus()` re-evaluates each removed equation at the
   solution: a residual above 1e-6 makes its joint conflicting. Per joint,
   redundant = any equation removed (as before), partially redundant =
   some but not all (planegcs's split per tag). DoF now counts only the
   kept equations, the Jacobian's rank. Malformed = an active joint whose
   references are set but unusable (both on one part, an object or element
   that no longer resolves); an incomplete joint is not. The panel shows
   conflicting, malformed, redundant, partially redundant in that order;
   Python reads it all from `getSolveStatus()`. Which of two contradicting
   joints is named depends on the solver's pivot order.
2. **Every part has mass 1 and unit inertia** (`App/AssemblyObject.cpp:1944-1946`).
   The drag code sets heavier masses on dragged parts
   (`Gui/ViewProviderAssembly.cpp:1135-1140`), but `getObjMass` (`:2042`)
   has no caller, so that weighting does nothing. It also blocks any
   dynamics (`docs/Simulator.md` stage 0).
   **Drag weighting wired 2026-10-23, `048dab4c27`.** `preDrag()` puts the
   GUI's drag masses (10, inertia scaled to match) on the MbD bodies after
   the pre-drag solve built them, so they act in the drag steps only
   (`PosICDragNewtonRaphson` weights come from `Part::fillqsuWeights`, by
   m / max(m)). **Static solves keep mass 1 on purpose**: in an
   under-constrained assembly the mass-weighted minimal motion decides
   which part moves, so real masses would change what a solve does. The
   ASMT model, the pre-drag solve and `postDrag()`'s solve are unchanged.
   Real mass and inertia from the shapes belong to the dynamics work
   (`docs/Simulator.md`). `TestDrag.test_drag_weighting` shows the effect.
3. **`solve()` and `updateSolveStatus()` call each other** (`:201-215`,
   `:227-229`) when no solver system exists. Upstream broke this loop in
   PR #32008; ours is unchanged. Not reproduced here.
   **Fixed by the upstream robustness picks (phase 0 step 2):** `solve()`
   holds `Base::StateLocker lock(solveInProgress)` and
   `updateSolveStatus()` solves on demand only when `!solveInProgress`
   (`App/AssemblyObject.cpp`, `solve()` and `updateSolveStatus()`),
   verified 2026-10-23.
4. **OndselSolver's convergence test compares a step with itself.**
   `NewtonRaphson.cpp:115` reads `dxNorms->at(iterNo)` for the previous
   step (it should be `iterNo - 1`), so "still converging" is never true,
   and `iterNo / iterMax` at `:117` is integer division, so the tolerance
   never relaxes. Newton runs extra full factorisations.
   **Fixed 2026-10-23** in `realthunder/OndselSolver` LinkVibe `92c8a0d`,
   bumped in `48d87d69ad`. The prediction was wrong: the bug made every
   position solve stop after exactly 5 iterations (one, then four
   miscounted divergences); fixed, a halving step no longer counts, and
   solves take 5 to 8. Placements are identical within 5e-13 mm on the
   bench (star 50/200, chain 50/100); solve times are within noise on the
   chains and up to ~15% on the stars. The logged step norm reaches a floor
   after one or two iterations (3e-9 on a star, up to 1e-6 on a chain), so
   `PosICNewtonRaphson`, which has no step tolerance, always ends on the
   divergence count: the early-out belongs to phase 2 step 1.
5. **A failed drag step is swallowed** (`App/AssemblyObject.cpp:453-455`);
   the part just stops following the mouse with no message.
   **Fixed 2026-10-23, `2dbeb5b9da`.** Most failures never reached that
   catch: OndselSolver's `runDragStep()` retried at smaller fractions and
   restored the parts silently; it now returns whether a step converged
   (LinkVibe `2282fe8`). `doDragStep()` returns bool, warns once per drag
   (with `postDrag()` reporting "N of M steps failed"), puts the document
   back on the last good step, and the 3D view says so in the status bar;
   the drag goes on. Python: `preDrag` / `doDragStep` / `postDrag` /
   `getDragStatus` (`1345e21cc5`).
6. **Joints not reachable from ground are silently dropped**
   (`removeUnconnectedJoints`, `:1017`); with nothing grounded the solve
   returns -6 and does nothing (`:177-181`). (Since the port the Origin
   always counts as grounded, so -6 cannot happen: the island is dropped
   and `solve()` returns 0, `TestJointTypes.test_island_without_ground_stays`.) Upstream now drags islands
   without a ground (sec 6).
7. **Zero-tolerance placement compare** in the write-back
   (`App/AssemblyObject.cpp:603` -> `Base/Placement.cpp:98`, and
   `App/AssemblyUtils.cpp:747`): parts that did not move are rewritten
   (sec 4.2, F6).
8. **ASMT import is a stub** (`AssemblyImport.py:25-32`) though registered.
9. **Most GUI commands failed since the port** (found 2026-10-22, Phase 0).
   The ported Python calls core Gui functions upstream has and the fork
   never had: `Gui.Document.openCommand` / `commitCommand` /
   `abortCommand`, `Gui.Document.TreeRootObjects`, `Gui.doCommandEval`,
   `Gui.doCommandSkip`. Create Assembly, every joint command, BOM,
   Simulation, Snapshot, Exploded View, Insert Link, Insert New Part and
   Solve raised `AttributeError`. Added in `6cac2592a1`. Still missing:
   `Gui.TaskPlacement` (the joint panel's offset buttons; needs the
   placement dialog's `setIgnoreTransactions`), `Gui.IconPosition` (joint
   overlay icons, which the fork's Gui never asks for) and
   `Gui.QtTools.deleteKeySequence` (callers fall back to the Delete key).
   A probe of every module, document and view member the workbench names
   found these; members of other objects need the GUI smoke test of
   phase 0.
   **Fixed 2026-10-23.** `Gui.TaskPlacement` and the dialog's
   `setIgnoreTransactions` (`6a84408660`); `Gui.IconPosition` and
   `getOverlayIcons()` merged into the tree icon (`02f7b5a09e`).
   `Gui.QtTools.deleteKeySequence` is left out: upstream has no
   `Gui.QtTools` either -- its own caller falls back the same way -- so
   there is no upstream API to match. The GUI smoke test
   (`AssemblyTests/TestGuiCommands.py`, `dfe91bf673`) runs all 33
   commands and scans 42 members read off views, view objects, Gui
   documents, Selection and Control; it found one more missing API,
   `ViewProviderSheet.showSheetMdi` (BOM accept, `68ca964185`), and three
   console errors (`b85a4cd4db`): every Rigid Group printed a traceback
   (`RigidPlacements` set before it existed), and Insert New Part without
   a joint recomputed a removed joint and an empty Link.


## 4. Performance at scale

### 4.1 Setup

GUI runs (`Thundereal.exe`, RelWithDebInfo, under cdb, fresh user home).
An assembly of N `App::Link` copies of one 10 mm box, the first grounded,
joined by alternating Fixed and Revolute joints, either as a **chain**
(part i-1 to part i) or a **star** (part 0 to every part). Joints are made
the way the joint task makes them. Wall seconds; every solve converged.
Run-to-run variance is about 1.5x; first-chance exceptions cost a debugger
round trip, so numbers outside cdb may be somewhat lower.

### 4.2 Measured

| phase | star 200 | star 1000 | chain 50 | chain 100 | chain 200 | chain 1000 |
|---|---|---|---|---|---|---|
| create the joints, solving off | 4.15 | **316.8** | 0.56-1.0 | 2.5 | 6.3-8.2 | **315.2** |
| `assembly.solve()` | 0.43 | 6.7 | 0.43-2.1 | 18-21 | **85-192** | not run (over 1 h extrapolated) |
| re-solve, nothing changed | 0.42 | 6.7 | same | same | same | |
| touch one leaf part, recompute | 0.43 | 6.8 | 0.22-0.48 | 36 | 115-192 | |
| add one part and joint interactively | 0.94 | 14.9 | 0.47-1.1 | 66 | **314-326** | |
| preDrag | 0.26 | 4.4 | 0.06-0.13 | 7.1 | 12.5-16.5 | |
| doDragStep, per mouse move | 0.034 | 0.34 | 0.010-0.022 | 4.7 | **11-13** | |
| redraw and event pump, per step | 0.012 | 0.029 | | | | |
| `isPartConnected` | 0.005 | 0.18 | 0.001 | 0.002 | 0.006-0.014 | 0.19 |

Fitted scaling (time ~ N^k): solve k = 1.7 on a star and 2.4 to 3.6 on a
chain, with a jump between 50 and 100 parts; drag step 1.4 on a star, about
4.5 on a chain; building all joints 2.7 to 3.0.

The drawing is not the problem: with Link instancing a redraw costs 12 ms
at 200 parts and 29 ms at 1000. The solver and the workbench's bookkeeping
are.

What a user meets: a 200-part chain -- a conveyor, a track, a linkage --
takes one and a half to three minutes per solve, over ten seconds per mouse
move while dragging, and five minutes to add one joint. Built one joint at
a time in the GUI, a 1000-part star costs about one and a half hours of
waiting.

### 4.3 Findings, by impact

**F1. The solver's elimination dominates.** OndselSolver factorises a
`std::map`-row sparse matrix with a fixed column order:
`GESpMatParPvMarkoFast::doPivoting` (`GESpMatParPvMarkoFast.cpp:44`) scans
every remaining row per pivot, O(n^2) per factorisation, 66 % of a 1000-part
star solve; `GESpMatParPv::forwardEliminateWithPivot` (`GESpMatParPv.cpp:15`)
inserts fill-in node by node, 73-85 % of a 100-part chain solve and drag
step. A singular Jacobian is signalled by throwing `SingularMatrixError`,
which re-factorises and runs a full-pivot redundancy search
(`PosICNewtonRaphson.cpp:23-37, 102-130`) -- 45 % of a chain solve, on
assemblies with no redundant joints. There is no early-out: a solved,
untouched assembly costs a full solve. Plus defect 4 of sec 3. Renaming
parts so the name order matches the chain did not help, so it is the
pivoting and fill, not the ordering of names.

**F2. Every solve rebuilds everything; edits solve everything, some twice.**
A new ASMT model and MbD system per call (`App/AssemblyObject.cpp:174`);
any touched part re-solves the whole assembly (`execute`, `:142-153`); the
joint task solves in `setJointConnectors` and again on accept
(`JointObject.py:838-870`, `:1725`: 7.9 s + 7.0 s at star 1000); a drag ends
with another full solve (`postDrag`).

**F3. Connectivity queries are O(P*J) and are called O(N) times.**
`isPartConnected` (`App/AssemblyObject.cpp:1127-1155`) rebuilds the grounded
set and joint list and, for each visited part, scans every joint with
property lookups by name (`:1052-1108`). Joint creation calls it four times
plus once per downstream part (`JointObject.py:934, 965, 1055`), so building
joints is O(N^3): 317 s for 1000, 0.84 s for the last joint alone. Drag
start answers "is this part grounded through another joint" by **writing
`Suppressed` on the part's other joints** (`:940-989`) -- property changes,
signals and undo entries for a query.

**F4. `getJoints()` does string work per joint per call**: parts compared by
`getFullName()` (`:765`, 25 % of `getDownstreamParts`), a Python `hasAttr`
per joint under the GIL (`:774-778`), and a whole-document scan to find the
joint group (`App/AssemblyUtils.cpp:379-397`). It is called by everything
above and by every drag step.

**F5. Python on the per-joint hot path.** After every solve and per drag
step, each joint's Python view provider recomputes its global frame through
`UtilsAssembly.getGlobalPlacement`, which reads `obj.Parents` -- O(N) per
call (`App/DocumentObject.cpp:1413-1455`). 0.69 s per solve at 1000 joints,
17 % of a star-1000 solve. Joint `execute` recomputes the frame in Python
too (~1.8 ms per joint).

**F6. The write-back storm.** Defect 7: a no-op re-solve rewrote 49 of 49
parts of a 50-chain and 72 of a 1000-star. Each write on an App::Link fires
two property changes (`Placement`, `LinkPlacement`), each fanning out to the
tree, observers, undo and the render cache.

**F7. Drag cost per step and per start.** Per mouse move: a path-string
parameter lookup, a full Newton run (up to five retries), `getJoints()`, a
grounded-set rebuild, the write-back, a Python redraw. Drag start hides
every other joint by `Visibility` writes (0.34 s at 1000 joints).

**F8. Joints are heavy objects.** Each builds three Python `SoSwitchMarker`
subgraphs (~13 ms per joint, also at document load, outside the progressive
load of `docs/DocumentLoad.md`). With J ~ P, joints double the object count.

**F9. Sub-assembly instances are mirrored, not linked.** A flexible
AssemblyLink creates a shadow App::Link per component and copies every
joint with 16 properties by name, matched by an O(M^2) loop
(`App/AssemblyLink.cpp:329-625`), on every execute and every change of the
source assembly. K instances of an M-part sub-assembly cost K*M objects.
Not measured.

**F10. Smaller, each linear or worse**: `ASMTAssembly::markerAt` scans all
markers building name strings, twice per joint (5.6 % of a star-1000
solve); `slidingPartIndex` calls `getJoints()` per screw/rack joint
(O(J^2)); `syncGroundedJoints` runs Python source text per missing grounded
joint, every solve; `ViewProviderAssembly::updateData(Group)` rescans all
joints per inserted part; command `IsActive` handlers loop over joints in
Python on every command-state refresh; exploded views and simulation
playback move parts by writing document placements, and restore by writing
all of them.

### 4.4 Re-measuring

`scripts/assembly-bench/bench.py` builds and times the assemblies above;
`sampler.py` beside it is a native stack-sampling profiler for one thread
of the running FreeCAD. Run the benchmark as a startup macro through a
wrapper that sets `CFG` (sizes, topologies, drag, profiling, and `out`, a
scratch directory for results), under `run_cdb.ps1 -UserHome <empty dir>
-StartupScript <wrapper>`. `preDrag`, `doDragStep` and `postDrag` are not
bound to Python, so the script calls them through ctypes on the object's
C++ pointer after checking it; a binding would retire that.


## 5. The fork's large-model stack: used, bypassed, defeated

Used for free: App::Link parts share one shape, one tessellation and (in
the bgfx renderer) instanced draws; moving one is a transform change. Link
arrays are understood as components. Document load improvements apply to
the parts.

Defeated or bypassed:

- the write-back storm (F6) on every solve and drag step;
- joints as full objects with private scene graphs and Python view
  providers (F8), none shared or instanced, all attached at load;
- flexible sub-assembly instances mirrored object by object (F9), losing
  the subtree sharing App::Link exists to give;
- exploded views and simulation playback written as document placements --
  undoable, signalled, recomputable -- for a view-only effect;
- nothing in the workbench exposes the lighter modes the stack already
  makes possible (sec 2, large assemblies).


## 6. Upstream since the port

The full account is the ledger, `docs/AssemblyPort-ledger.tsv`: every
upstream commit since the merge base `a662fbb2ff` touching
`src/Mod/Assembly`, `tests/src/Mod/Assembly` or the solver, 468 rows after
dropping translation-only commits. 425 arrived with the port, 7 arrived
without their C++ test half, and 36 are open: the 29 after the baseline and
7 that touched only the C++ tests the port never brought. Of the 29, 14
apply as they are and 16 merge cleanly three-way; the reason for each
failure is in the notes file. The notable ones (none of them here):

| upstream | date | what | kind |
|---|---|---|---|
| `3a9ab5483f` (#29605) | 08-10 | RigidGroup: lock parts together without a sub-assembly | feature |
| `3ac55b1252` (#30955) | 08-12 | exploded view: edit a move by distance and angle, with tests | feature |
| `6e90683e59` (#31622) | 08-16 | Fixed joint on a Part | fix |
| `c1df66bce9` (#31155) | 08-27 | coaxial gear joint (our solver already has its half) | feature |
| `b13dc13b35` | 08-27 | DoF with bundled fixed joints | fix |
| `67103b19c9` | 09-09 | generated arrays and suppressed link elements (written for upstream's arrays; needs adapting to ours) | feature |
| `16dd3cc01f` (#30998) | 09-10 | drag an island with nothing grounded | feature |
| `141037482c` (#32008) | 09-11 | eight robustness fixes: NaN placements rejected, the solve / status loop broken, use-after-free in isolation, Python exceptions contained in joint redraw | fixes |
| `a5908bb06e` (#32924) | 09-28 | delete an orphaned joint group | fix |
| `2372eb1c4f` (#32922) | 09-30 | rack and pinion | fix + test |
| `15bd466ccb` | 10-06 | rotation order (ours: `App/AssemblyObject.cpp:1789`) | fix |

Open upstream, not merged: #28156 (a solver interface and a Project Chrono
dynamics back end -- bears on `docs/Simulator.md`), #29478 insert
fasteners, #28052 mirror in insert, #32302 keep the solver from relaxing
ground. Upstream's C++ tests (`tests/src/Mod/Assembly`) were never ported.


## 7. Tests

Python, in `TestAssemblyWorkbench`: TestCore (creation, and one solve with
a Fixed joint), TestSubNames (fork-only), TestCommandInsertLink (mocked
UI), TestSimulationExport (the video writer only), TestSettings. No C++
tests. Uncovered: every joint type but Fixed, limits, DoF and redundancy
reporting, dragging, AssemblyLink, BOM, exploded views, snapshots, a
kinematic run, ASMT export, link arrays in assemblies.


## 8. The plan

### 8.1 Decisions (user, 2026-10-22)

1. **Take all of upstream's Assembly work**, tracked commit by commit in
   `docs/AssemblyPort-ledger.tsv`, as the Sketcher and PartDesign ports
   were. Picks carry `(upstream <hash>)` in the subject.
2. **Rework OndselSolver in depth ourselves**, in our fork
   (`realthunder/OndselSolver`, branch LinkVibe).
3. **Make the assembly-to-solver interface abstract**, so the backend can
   be switched: `docs/AssemblySolverInterface.md`.
4. **Borrow from Assembly3** -- its overall design, not only its solver
   layer -- and **import Assembly3 documents**: `docs/AssemblyFromAsm3.md`.
5. **Interference and collision detection come last.**
6. **Project Chrono is the second solver backend**, built from the start
   of the interface work: it proves the interface is not OndselSolver's
   shape under another name, it is the yardstick for benchmarking the
   OndselSolver rework, and it is the likely engine for collision and
   dynamics later. Forked to `Thundereal/chrono`, a submodule at
   `src/3rdParty/chrono` pinned to release 10.0.0 (`9faf13dd8f`; BSD-3).
   Only Chrono's core module is meant to be built.
7. **The Distance sub-types** (sec 8.3, question 3): of the 17 that fall
   to a planar distance today, LineCylinder (axis distance d + r),
   LineSphere (centre to line, d + r) and CurvePlane (the curve's plane at
   d) get a proper mapping; the other 14 (PlaneCone, CylinderCone, ConeCone,
   ConeTorus, ConeSphere, PointCone, PointTorus, LineCone, LineTorus,
   CurveCylinder, CurveSphere, CurveCone, CurveTorus, Other) are refused
   for new joints with a message, while joints of those kinds already in a
   document keep today's planar behaviour with a warning.
8. **The assembly object model is C++** (user, 2026-10-24):
   `docs/AssemblyObjectModel.md`. The container stays an `App::Part`, given
   LinkGroup-like group modes; ownership follows Body and its aux groups,
   with `PartDesign::AuxGroup` generalized for every container; Assembly3's
   Elements and ElementLinks are adopted, declared by Assembly, App::Part
   and Body alike. This narrows decision 1: upstream's commits to the
   objects that move to C++ are ported by hand, not taken as they come.

Standing rule: nothing is pushed to, or written in, any repo of the
FreeCAD GitHub org. The `upstream` remote is fetch-only; the solver's
changes stay in our fork.

### 8.2 Phases

**Phase 0 -- re-converge, then catch up with upstream.** Upstream's picks
land in the very functions the interface work moves
(`docs/AssemblySolverInterface.md` sec 6.1), so they go first.

1. One fork-only commit that puts the port's adaptations back into
   upstream's spelling. Since the port, the fork has itself taken what the
   port worked around: `App/Datums.h` (`App::Origin` now derives from
   `App::LocalCoordinateSystem`), `SoTransformDragger`, FastSignals,
   `Base::Color`, `Gui/Selection/`, lowercase Console names. The old
   spellings are now only conflict sources. It also closes a gap: checks
   for `App::Origin` alone miss `Part::LocalCoordinateSystem`.
2. The ledger's open rows in this order (`docs/AssemblyPort-ledger-notes.txt`
   has the reasons and prerequisites): the robustness set of #32008; the
   standalone fixes (DoF with bundles, rotation order, Fixed joint on a
   Part, orphaned joint group, rack and pinion); upstream's C++ tests whole;
   the coaxial gear joint, then RigidGroup; island drag; exploded-view move
   editing; the generated-arrays commit, adapted to the fork's
   `App::LinkArray` (it is written for upstream's `PartApp::LinkArray` and
   suppressible link elements, which the fork lacks); housekeeping.
   Deferred: three build commits that call CMake macros the fork does not
   define, and two that move Console to `std::format`, which waits on a Base
   decision.
3. Defects 1-5 of sec 3 that upstream does not fix; Python bindings for
   `preDrag` / `doDragStep` / `postDrag`; tests for every joint type,
   limits, DoF and drag.
   **Done 2026-10-23**: defects 1, 2, 4, 5 (`f3920f1797`, `048dab4c27`,
   `48d87d69ad`, `2dbeb5b9da`; defect 3 came with step 2), the bindings
   (`1345e21cc5`), and `AssemblyTests/TestJoints.py` (`b771e014cf`, 30
   tests: every joint type, the 20 mapped Distance sub-types, limits, DoF,
   redundancy / conflict / malformed status, drag weighting and drag
   failure). The suite is at 72 run, 1 skipped. Found on the way, not
   fixed: a NaN Placement on a `Part::Feature` recurses between its
   Placement and Shape sync until the stack overflows; OndselSolver's
   spherical joint does not converge from starts its sphere-sphere joint
   handles; the bench's drag phase does nothing at even N (the last link
   is Fixed-bundled, so it is not a drag part).
4. The rest of defect 9: `Gui.TaskPlacement` with the placement dialog's
   `setIgnoreTransactions`, and overlay icons from Python
   (`Gui.IconPosition`, `getOverlayIcons`). Then a GUI smoke test that runs
   every Assembly command on a real document, so a call into an API the
   fork lacks fails a test instead of a user.
   **Done 2026-10-23**: `6a84408660`, `02f7b5a09e`, `68ca964185`,
   `b85a4cd4db`, `dfe91bf673` (defect 9 has the details). The suite is at
   105 run, 1 skipped.

5. After the upstream merge (step 2): **an audit of header inclusion**
   (user, 2026-10-22), so that a change rebuilds only what depends on it.
   The port kept running into headers too costly to touch: a member on
   `Gui/Application.h`, `Gui/Document.h` or `Base/Vector3D.h`, or a `using`
   in `App/GeoFeatureGroupExtension.h`, each rebuilds hundreds of files, so
   each was worked around instead (`6cac2592a1`, the UnitZ spelling in
   `9c50dd3838`, the GroupExtension call in `f829545409`). The audit
   measures, per header, how many translation units include it directly
   and transitively and what a one-line change costs, finds where a
   forward declaration, a pimpl, or splitting a header would cut that, and
   proposes the changes ranked by rebuild time saved.
   **Audited 2026-10-22: `docs/HeaderInclusionAudit.md`**, scripts in
   `scripts/header-audit/`. A full build is 424 CPU-min (71 min at -j 6),
   PCH is off, and compile time is header parsing (~1 s per MB pulled in).
   Ten ranked proposals save ~390 CPU-h a year; the fork-owned splits
   (FileBlobManager.h, the Material headers, PropertyAppearance, Renderer.h's
   config structs) carry no merge cost and come first.
   **Applied in three rounds**, `docs/HeaderInclusionAudit.md` sec 6.

Done so far: step 1 (`aa0b4a9691`), step 3 (2026-10-23, above), step 4
(`6cac2592a1`, then 2026-10-23, above), and step 5 (the header audit,
applied in three rounds).

**Phase 1 -- the solver interface, results identical.** Steps 1-6 of
`docs/AssemblySolverInterface.md` sec 6.3: a solver-neutral model and
backend interface in `App/Solver/` (namespace `Assembly::Solver`, backend
chosen by an assembly property, then the preference
`Mod/Assembly/SolverBackend`, then Ondsel); OndselSolver behind it as an
adapter; drag and status through the session; the solver's headers out of
`AssemblyObject.h`; the trajectory kept in the neutral layer. Each step is
gated on identical ASMT output and identical placements, compared in one
process. Upstream's draft interface (#28156) is not adopted -- it mirrors
ASMT one to one and rebuilds on every solve -- but its names are reused so
a later merge lands beside ours. **A Chrono backend comes in this phase**
(decision 6), built from the submodule's core module: once both backends
solve the same assemblies through the interface, the interface is shown
backend-neutral, and the two can be benchmarked against each other with
`scripts/assembly-bench/`. Chrono cannot express every joint
(`docs/AssemblySolverInterface.md` sec 5.3: no Angle, PointLineDistance,
LineLineDistance or LineInPlane as primitives); the gaps are reported
through the backend's capabilities, not hidden.
**Steps 1-3 done 2026-10-23** (`83ca3dcc4f` interfaces, `65b0540fbe` the
Ondsel adapter, `09ce3133a3` the builder switch with the in-process gate,
`18d3ecccc3` the gate on recorded references and the old builder removed):
the builder emits a `Solver::Model` under upstream's function names, the
adapter turns it into the same ASMT model as before, and
`AssemblyTests/TestSolverGate.py` (46 cases) holds the solver input and the
results to what the old builder gave. The suite is at 121 run, 1 skipped.
The deviations from the design, and what step 4 (drag and status through
the session) needs, are in `docs/AssemblySolverInterface.md` sec 6.5.
**Steps 4-6 done 2026-10-23** (`cae7e1ade6` drag and status through the
session, `701f889a70` the header clean, `7b54d90575` playback from a stored
trajectory; the solver fork's `881d949` drops `freecadAssemblyObject`, local
only), and the backend selection (`6a4b278213`: the `SolverBackend`
property, empty by default, the preference, default "Ondsel", and
`AssemblyApp.getAvailableSolvers()`). `AssemblyObject` holds no solver type,
and only `App/Solver/OndselBackend.cpp` includes OndselSolver; Gui builds
without the solver's include path. The gate held through all four (step 6
added three recorded simulation cases, checked frame by frame against the
old playback in one process first), the bench is unchanged within noise,
and the suite is at 129 run, 1 skipped. The deviations -- `Session::poses()`,
drag weights through `Session::update`, the DoF double count an on-demand
status solve used to make, the trajectory kept across solves -- and what
step 7 needs are in `docs/AssemblySolverInterface.md` sec 6.6. Not yet done
from the decisions: the neutral formula evaluator. The Distance mapping of
decision 7 came after step 7 (`e320accd94`, `85a4379a0b`;
`docs/AssemblySolverInterface.md` sec 6.8). Chrono is not started.
*Since then (2026-10-23): the neutral formula evaluator (sec 6.9), the
Reference backend (sec 6.13) and the Chrono backend (sec 6.15, built with
`BUILD_ASSEMBLY_CHRONO`, off by default).*

**Phase 2 -- scale.** Where sec 4 measured the time going:

1. OndselSolver: a sparse LU with a fill-reducing ordering, the symbolic
   factorisation kept across Newton iterations and drag steps, redundancy
   found once per topology without exceptions, an early-out when already
   solved, the convergence-test fix (F1, defect 4). A reference backend on
   Eigen first, as the oracle the rework is tested against.
2. In the neutral layer, once for every backend: bundle Fixed joints,
   RigidGroups, rigid sub-assemblies and grounded parts; split into
   connected components; place loop-free and Attachment chains directly;
   solve only what a change reaches; a residual check per joint that
   reports conflicts for any backend (defect 1); write back with a
   tolerance and in one batch (F6); warm sessions keyed by topology
   (interface steps 7-9).
3. In the workbench: a connectivity index instead of O(P*J) scans and
   `Suppressed` writes (F3, F4); one solve per joint-task accept and none
   after a drag (F2, F7); joint frames in C++ and one shared marker geometry
   (F5, F8); flexible sub-assembly instances addressed, not mirrored (F9).
4. From Assembly3's solving strategy: skip assemblies nothing touched, roll
   placements back when a solve fails, solve children first with Rigid as
   the fast path, and solve once per transaction so the solver's writes
   land in the user's undo step.

**Phase 2 started 2026-10-26, with the C++ joints** (the joint task makes
them since 2026-10-25). `scripts/assembly-bench` builds them by default now
(`joint_kind`, "python" for upstream's). Measured on chain 50 and 100 they
were slower than upstream's Python joints, by what the C++ joint did for
each side and by its objects being the assembly's children, not by the
solver; fixed:

- the solver and the markers took a side's frame by resolving its
  geometry (`getTopoShape` through the component) each solve and drag
  step; the joint keeps it relative to the component on recompute
  (`Placement1/2`), and a solve moves components, not what is on them:
  `Joint::getStoredSide()` / `getFrame()` read that, and resolve only a
  joint not up to date. Solves are the Python joint's again (chain 50,
  0.5 s);
- the DoF counted the element links as parts (fixed with the C++ joint
  work, `docs/AssemblyObjectModel.md` sec 7.1);
- F3 and F4: `isPartConnected`, `getDownstreamParts`,
  `isJointConnectingPartToGround` and `removeUnconnectedJoints` walk a
  connectivity index built once per query (each joint's sides resolved
  once, a breadth-first walk over a hash set), and leave joints out of the
  walk instead of writing `Suppressed` and back. `isPartConnected` 20 ms to
  1.4 ms, `getDownstreamParts` 98 ms to 1.2 ms at chain 50;
- an Assembly aux group refreshed (O(J)) on every change of any Python
  object property of the assembly's children, which every view provider
  property change is (the object's `ViewObject` mirror): on `Proxy` only;
- a Part feature's recompute said its appearance changed three times
  through `ViewObject` (re-applying the colours held to the new visual,
  which changes no view of the object's), to every observer of the
  document: not said now (`HostOnlyView` around the re-touch). The
  `ViewObject` touch is the expression binding of view properties
  (`17300660d8`) and what a served document publishes on (`7532b0b528`);
  both see every real change as before;
- a group's tree export (`buildExport`, O(children) with a Python
  `claimChildren` per joint) was rebuilt for any property change of a child
  in a geo group: on a link property's only, and not for `_GroupTouched`.

Then, the cost of building joints, which grew with the joints there are:

- the joint's view provider claims its element links in C++
  (`AssemblyGui::ViewProviderJoint`, `ViewProviderJointPython` for
  `JointPython`): a group's tree export asked every joint's Python view
  provider for its claims at each change of the assembly's `Group`. The
  Python one still claims for a joint whose file states the generic view
  provider, as files saved before do;
- a change of a geo group's `Group` checked every member, its other owners
  and its coordinate-system links: now only the members it adds, unless
  one left, when all are checked as before
  (`tests/src/App/PartGroupMode.cpp`, both cases).

- F5 and F8: the joint's view provider draws the markers in C++, every
  marker of every joint sharing one geometry, and a solve tells a C++
  joint's view it moved its frames by a signal (`Joint::signalFramesMoved`,
  `Joint.redrawMarkers()` from Python), not by a Python call per joint. The
  Python view provider makes the joint task's preview when first shown, and
  sets the markers' pick style through the scene; it still draws all for a
  joint whose file states the generic view provider.

Chain 50 builds in 0.71 s (Python joints 0.67 s), chain 100 in 2.15 s
(1.5 s), chain 200 in 7.9 s (6.3 to 8.2 s in sec 4.2); redrawing every
joint's markers after a solve, 70 ms at chain 100, is 4.5 ms. Left: each
change of the assembly's `Group` still rebuilds its tree export
(O(children)), and the joint task asks `isPartConnected` per joint made;
and the solver itself (F1), which is what a solve, a recompute and a drag at
chain 100 spend their time in, Python joints and C++ alike.

F1, first step (2026-10-26, `docs/AssemblySolverInterface.md` sec 6.19):
OndselSolver's Newton steps are solved by a sparse LU (Eigen's `SparseLU`,
COLAMD order, rows and columns equilibrated), the old elimination taking
over only the steps whose pivots say the matrix may be singular, and
solving again from the start a run that wanders, so redundancy is found as
before and those runs end as they did. A static solve of chain 100 went
from 3.3 s to 0.19 s, of chain 200 from 190 s to 0.73 s; placements within
2e-12 mm of the old solver's, and the same start gives the same answer bit
for bit (the grounded parts are made solver bodies in name order now, not
in an `unordered_set`'s). Left of F1: the redundancy search still throws
and re-factorises; no early-out when already solved; the column order is
kept across a run's iterations only, and found again for each run (each
drag step).

**Phase 3 -- from Assembly3, and its importer** (`docs/AssemblyFromAsm3.md`).
Its behaviours, not its objects -- Element objects would put a second
object model beside upstream's, at a merge cost on every pick:

- automatic repair of a joint whose reference broke (Assembly3's `fix()`);
- replace a part keeping its joints;
- freeze a sub-assembly to its shape, with partial load, so its parts and
  joints never load -- the start of a lightweight open;
- QuickMove, point tracing while dragging, a dragger for joint offsets,
  joint-selection messages, and a pattern command (a link array plus a
  joint per element) in place of Assembly3's Multiply;
- **`Assembly_ImportAssembly3`**: converts an open Assembly3 document in
  place, in one transaction, without Assembly3 installed; joints are given
  offsets so re-solving moves nothing, which is also its acceptance test.
  Mapping, unsupported constraints and the eleven planned tests are in
  sec B of that document.

**Phase 4 -- the remaining feature gap**, each its own decision when
reached: large-assembly modes on the fork's load and render stack (a
lightweight open above a component count, envelopes from far-field
proxies); exploded views linked to TechDraw and animated; motion plots and
numeric export, real mass properties (defect 2), then dynamics as
`docs/Simulator.md` plans (a Chrono backend, possibly out of process,
through the phase 1 interface); component patterns, mirror,
configurations, the missing joints (tangent, slot, cam, path, universal,
width); per-component DoF and conflict diagnostics.

**Phase 5, last -- interference and clearance.** A static check first: a
broad phase over the part bounding boxes the render cache already has, an
exact narrow phase from `Shape.common` or the fork's `Shape.diffMaterial`
(which returns the common volume), minimum distance from `distToShape`;
report pairs, volumes, touching versus penetrating, with exclusions
(hidden, a fasteners group, a sub-assembly as a unit). Then a faceted mode
on the shared tessellations, fast enough to run per drag step: stop at
collision, highlight the faces. Then clearance thresholds per part and
pair. Each check works on shapes alone, a natural first tenant for OCCT
work out of process (`docs/ComputeBoundaries.md`).

### 8.3 Open questions

From `docs/AssemblySolverInterface.md` sec 7:

1. Decided (user, 2026-10-22): both -- an assembly property, empty by
   default, then the preference.
2. Decided (user, 2026-10-22): the neutral layer owns the motion-formula
   evaluator; OndselSolver's parser is not the shared one.
3. Decided (user, 2026-10-22): see sec 8.1, decision 7. Implemented
   2026-10-23 (`docs/AssemblySolverInterface.md` sec 6.8).
