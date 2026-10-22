# Assembly: an audit against other CAD systems, and at scale

Status (2026-10-22): audit only, no product change. Branch `AssemblyPort`,
cut from `LinkVibe` `27e32d75f1`. This document records where the Assembly
workbench stands against the commercial and open-source assembly tools,
the defects found while reading it, and what it costs on large assemblies,
measured. Sec 8 proposes an order of work; nothing in it is decided yet.

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

**The upstream baseline of the port was not recorded**, and there is no
ledger like PartDesign's. From upstream's history the cut lies between
2026-07-20 and 2026-08-10, most likely the end of July (sec 6).

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
2. **Every part has mass 1 and unit inertia** (`App/AssemblyObject.cpp:1944-1946`).
   The drag code sets heavier masses on dragged parts
   (`Gui/ViewProviderAssembly.cpp:1135-1140`), but `getObjMass` (`:2042`)
   has no caller, so that weighting does nothing. It also blocks any
   dynamics (`docs/Simulator.md` stage 0).
3. **`solve()` and `updateSolveStatus()` call each other** (`:201-215`,
   `:227-229`) when no solver system exists. Upstream broke this loop in
   PR #32008; ours is unchanged. Not reproduced here.
4. **OndselSolver's convergence test compares a step with itself.**
   `NewtonRaphson.cpp:115` reads `dxNorms->at(iterNo)` for the previous
   step (it should be `iterNo - 1`), so "still converging" is never true,
   and `iterNo / iterMax` at `:117` is integer division, so the tolerance
   never relaxes. Newton runs extra full factorisations.
5. **A failed drag step is swallowed** (`App/AssemblyObject.cpp:453-455`);
   the part just stops following the mouse with no message.
6. **Joints not reachable from ground are silently dropped**
   (`removeUnconnectedJoints`, `:1017`); with nothing grounded the solve
   returns -6 and does nothing (`:177-181`). Upstream now drags islands
   without a ground (sec 6).
7. **Zero-tolerance placement compare** in the write-back
   (`App/AssemblyObject.cpp:603` -> `Base/Placement.cpp:98`, and
   `App/AssemblyUtils.cpp:747`): parts that did not move are rewritten
   (sec 4.2, F6).
8. **ASMT import is a stub** (`AssemblyImport.py:25-32`) though registered.


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

None of these are here (checked by grep):

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


## 8. Proposed order of work

A proposal for the user to decide; nothing here is started.

**Phase 0 -- footing.** Port upstream's C++ Assembly tests and add Python
tests per joint type, limits, DoF and drag (bind `preDrag` / `doDragStep` /
`postDrag` to Python). Take upstream's robustness set (#32008) and the
small fixes of sec 6, and fix defects 1-5 of sec 3. Record the upstream
baseline and start a ledger, as PartDesign did.

**Phase 1 -- scale, where the measured time goes.**
1. OndselSolver: a sparse LU with a fill-reducing ordering (Eigen
   SparseLU with COLAMD, or KLU), the symbolic factorisation kept across
   Newton iterations and drag steps; redundancy found once per topology
   without exceptions; an early-out when the residual is already within
   tolerance; the convergence-test fix. Expected: chain solves from minutes
   to well under a second, drag steps interactive.
2. Structure before solving: always bundle Fixed groups into one body,
   split into connected components, and place loop-free parts directly --
   the solver is needed only for closed loops. Most real assemblies are
   mostly trees.
3. A connectivity index on `AssemblyObject` (part -> joints, built once,
   invalidated on Group and joint changes), pointer compares, no
   `Suppressed` writes for queries (F3, F4). Joint creation from O(N^3) to
   about O(N).
4. Write-back with tolerance and in one batch; during a drag, move view
   transforms only and commit placements once at the end (F6, F7).

**Phase 2 -- keep the solver warm and the joints light.** Keep the MbD
system between solves keyed by topology; solve only the components a
change reaches; one solve per joint-task accept; no re-solve after a drag
(F2). Joint frames computed in C++, one shared marker geometry, visuals
created only for shown joints (F5, F8). Flexible sub-assembly instances
addressed through sub-object paths instead of mirrored objects (F9).

**Phase 3 -- the largest feature gap: interference and clearance.**
A static check first: a broad phase over the part bounding boxes the render
cache already has, then an exact narrow phase per candidate pair from
`Shape.common` or the fork's `Shape.diffMaterial` (which returns the common
volume), and minimum distance from `distToShape`; report pairs, volumes,
touching versus penetrating, with exclusions (hidden, a fasteners group, a
sub-assembly as a unit). Then a faceted mode on the shared tessellations,
fast enough to run per drag step: stop at collision and highlight the
faces. Then clearance thresholds per part and per pair. Each check runs on
shapes alone, so it is a natural first tenant for running OCCT work out of
process (`docs/ComputeBoundaries.md`).

**Phase 4 -- the rest of the gap, each its own decision.**
- Large-assembly modes that surface what the stack already does: a
  lightweight open (graphics only, shapes deferred) switched on above a
  component count, rigid sub-assemblies solved as one body, envelope
  substitutes built on far-field proxies.
- Exploded views: upstream's step editing, a TechDraw link through
  `getExplodedShape`, explode / collapse animation sharing the simulation's
  playback and export.
- Motion: real mass properties (defect 2), position / velocity plots and
  numeric export of a run, then dynamics as `docs/Simulator.md` plans,
  watching upstream's Chrono PR.
- Structure: component patterns as Link arrays the workbench creates,
  mirror, replace keeping joints, configurations; the missing joints
  (tangent, slot, cam, path, universal, width).
- Solver diagnostics: per-component DoF state, conflicts reported, and a
  minimal over-defining group (the MateXpert idea).

Open decisions for the user:

1. Does `AssemblyPort` also take upstream's Assembly commits with a ledger,
   as `PartDesignPort` did, or only the picks named above?
2. Is OndselSolver ours to rework in depth (phase 1.1), or should the
   sparse factorisation go upstream to Ondsel as well?
3. Phase 3 before or after phase 2? Interference does not depend on the
   solver work, and it is the gap users meet first.
