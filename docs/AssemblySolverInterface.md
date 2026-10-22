# Assembly: an abstract solver interface

Status (2026-10-22): design proposal, read-only study. Branch `AssemblyPort`
(`fe871ff48c`). Extends `docs/AssemblyAudit.md` (sec 4: the solver is the
measured bottleneck; sec 8.2 phase 2: the solver rework and the warm
model). Paths are relative to `src/Mod/Assembly/` unless they start at the
repo root. `OS/` abbreviates `src/3rdParty/OndselSolver/OndselSolver/`.
"PR" means upstream FreeCAD PR #28156; "diff:N" is a line of its diff as
fetched 2026-10-22 (`gh pr diff 28156 --repo FreeCAD/FreeCAD`).

Two goals pull on the design and both are honoured:

- the solver (OndselSolver, our fork) is reworked in depth -- sparse LU with
  an ordering, cached factorisation, structural decomposition -- and must
  be swappable for other backends;
- every upstream Assembly commit is ported, so the workbench code that reads
  the document must stay recognisably upstream's, hunk for hunk.


## 0. Conclusions

1. **The coupling is narrow in files, wide in lines.** Everything that
   touches OndselSolver lives in `App/AssemblyObject.{h,cpp}` (one include
   of `OndselSolver/enum.h` in the header, 33 includes and ~110 call sites
   in the .cpp) plus one reverse dependency inside the solver
   (`OS/ExternalSystem.h:20-22,45` names `Assembly::AssemblyObject`). Gui
   and Python reach the solver only through `AssemblyObject` methods. That
   method surface (solve, preDrag/doDragStep/postDrag, generateSimulation,
   updateForFrame, numberOfFrames, getLast*, exportAsASMT) is a good
   stable facade: keep it, swap what is behind it.
2. **Upstream PR #28156 is the wrong shape to align with.** It is a thin
   mirror of ASMT (string marker paths, Ondsel expression strings, a
   rebuild per solve by stated design), a stalled draft since 2026-08-31,
   conflicting with main, with a Chrono backend that lacks motions, limits,
   three joint kinds and redundancy. Align only the free things: the
   namespace `Assembly::Solver`, the preference key
   `Mod/Assembly/SolverBackend` with value `"Ondsel"`, the Python name
   `AssemblyApp.getAvailableSolvers()`, and its one sound structural
   choice -- the document-to-joint translation stays in
   `AssemblyObject.cpp` under upstream's function names. Put our files in
   `App/Solver/` so a later upstream merge of the PR adds files beside ours
   instead of add/add conflicts on the same paths.
3. **The neutral model sits at the body / frame / joint level**, not at
   the SolveSpace primitive level (Assembly3's lesson) and not at the ASMT
   object level (the PR's mistake): plain data with integer ids, a closed
   enum of ~19 joint kinds with typed scalar parameters, limits, drivers,
   and per-body grounded / mass / drag-weight flags. It is serialisable as
   is, so a backend can live in another process.
4. **Everything backend-independent is done once in the neutral layer**:
   element classification, radius folding and JCS swaps (today
   `getDistanceType` writes the document while classifying), rigid
   bundling (Fixed joints, RigidGroups, rigid sub-assemblies), connected
   components, the warm session cache keyed by topology, value deltas,
   write-back with tolerance, a backend-independent residual check per
   joint, and the trajectory store for playback.
5. **Migration is ten small steps; the first six keep results bit-identical**,
   gated by an exact test that already exists in the product: the ASMT
   text that `exportAsASMT` writes. If the new path produces the same ASMT
   text as the old one in the same process, OndselSolver receives the same
   input and gives the same output. Steps 7-10 change results by design
   (warm sessions, always-bundle, components, second backend) and are gated
   by tolerance tests.
6. **Python changes are small**: `JointObject.py` and
   `CommandCreateSimulation.py` keep calling the same `AssemblyObject`
   methods. What changes is one query-by-writing hack
   (`JointObject.py:933-936`), one Ondsel-specific pre-solve workaround
   (`preventParallel`, `:993`), the joint-type list filtered by backend
   capability, and the formula help text that documents Ondsel's grammar
   (`CommandCreateSimulation.py:541-560`).


## 1. Coupling inventory

All in `App/AssemblyObject.cpp` unless noted. The `MbD` namespace is
imported wholesale (`:96`).

### 1.1 Build and headers

| what | where |
|---|---|
| `OndselSolver` linked into the Assembly App lib | `App/CMakeLists.txt:18-24` |
| include path / external-solver switch | `CMakeLists.txt:3-12`, `cMake/FreeCAD_Helpers/InitializeFreeCADBuildOptions.cmake:12`, `src/3rdParty/CMakeLists.txt:282-286` |
| public header pulls `OndselSolver/enum.h` and forward-declares `MbD::ASMTPart/Assembly/Joint/Marker` -- so every Gui file that includes `AssemblyObject.h` needs the solver's include path | `App/AssemblyObject.h:35-44` |
| ASMT types in the public class: `makeMbd*`, `getMbdPlacement`, `MbDPartData`, `mbdAssembly`, `objectPartMap` | `App/AssemblyObject.h:105-140, 210, 259-261` |
| 33 solver includes, one vestigial (`ASMTConstantGravity.h`, never used) | `:53-85` |
| reverse dependency: the solver names the workbench class; set at `:116` and `:654`, all callbacks commented out | `OS/ExternalSystem.h:20-22, 45`, `OS/ExternalSystem.cpp:26-67` |

### 1.2 Model building

| role | code | where |
|---|---|---|
| assembly container, name `"OndselAssembly"`, debug flag from `LogSolverDebug` | `makeMbdAssembly` | `:651-663` |
| a body per moving part, created lazily on first reference, mass 1 and unit inertia | `getMbDData`, `getMbDPart`, `makeMbdPart` | `:1882-1960` (mass `:1942-1946`) |
| rigid bundling of Fixed-joined parts, drag only (`bundleFixed`), offsets in `MbDPartData.offsetPlc` | `getMbDData` | `:1899-1925`, flag set `:370-372` |
| self-referencing joint inside a bundle dropped with a warning | `isMbDJointValid` | `:1859-1880` |
| grounding: a marker on the assembly, a `FixingMarker` on the part, an `ASMTFixedJoint` named after the part | `fixGroundedParts`, `fixGroundedPart` | `:896-938` |
| marker per joint side: JCS placement made relative to the moving part (and to the bundle offset); marker named by the joint's full name; path string `"/OndselAssembly/<part>/<joint>"` | `handleOneSideOfJoint` | `:1675-1719` |
| rack marker re-oriented (X along the slide, Z parallel to the pinion) | `getRackPinionMarkers` | `:1721-1812` |
| joint type -> ASMT class (13 types) | `makeMbdJointOfType` | `:1192-1270` |
| Distance -> one of 9 ASMT classes by the picked geometry (37 sub-types, sec 5.2) | `makeMbdJointDistance` | `:1272-1429` |
| classification of the picked geometry; **writes the document** (`swapJCS`) while classifying | `getDistanceType` | `App/AssemblyUtils.cpp:159-377` |
| radii folded into distances | `getFaceRadius`, `getEdgeRadius` | `App/AssemblyUtils.h:146-147` |
| slider detection for Screw and RackPinion; calls `getJoints()` per joint (O(J^2)); swaps JCS in the document | `slidingPartIndex` | `:1814-1857`, used `:1239-1246, :1733-1740` |
| joint naming, markers I/J | `makeMbdJoint` | `:1431-1458` |
| length and angle limits as `ASMTTranslationLimit` / `ASMTRotationLimit`, values as **expression strings** (`"<deg>*pi/180.0"`), tol `"1.0e-9"`; skipped when a simulation runs; **swaps min/max in the document** when reversed | `makeMbdJoint` | `:1460-1565` |
| motions: `ASMTRotationalMotion`, `ASMTTranslationalMotion`, or `ASMTGeneralMotion` when a Cylindrical joint has both; formulas are Ondsel symbolic strings with `initialValue` substituted | `makeMbdJoint` | `:1566-1670` |
| simulation parameters (tstart, tend, hout, errorTol; hmin/hmax hard-coded) | `create_mbdSimulationParameters` | `:1171-1190` |
| model assembly loop | `jointParts` | `:1157-1169` |

### 1.3 Static solve

| what | where |
|---|---|
| `solve()`: identity placements, sync grounded joints, **new ASMT model per call**, ground, joints, `runPreDrag()` as the solve, write-back, Python joint redraw, status | `:168-218` |
| ground check returns -6 | `:177-182` |
| unreachable joints dropped | `removeUnconnectedJoints` `:186`, def `:1017` |
| inside the solver: `runPreDrag` makes a new `System` each call | `OS/ASMTAssembly.cpp:1334-1345` |
| read-back: position and quaternion per `ASMTPart` | `getMbdPlacement` `:458-473` |
| write-back with exact compare | `setNewPlacements` `:582-608` |
| solve on recompute | `execute` `:142-153` |

### 1.4 Drag

| what | where |
|---|---|
| `preDrag`: full solve with `bundleFixed`, then picks drag parts, skipping bundled members | `:368-404` |
| `doDragStep`: reads each dragged part's **document `Placement`** (the GUI wrote it), pushes it into the `ASMTPart`, `runDragStep`, validates, writes back, redraws visible joints; **swallows all failures** | `:406-456` |
| validation: a grounded part must not move | `validateNewPlacements` `:475-506` |
| `postDrag`: `runPostDrag`, which is **another full solve** in a new `System` | `:508-512`, `OS/ASMTAssembly.cpp:1422-1433` |
| inside the solver: up to five bisection retries, then `restorePosRot` with no error raised | `OS/ASMTAssembly.cpp:1347-1420` |
| drag masses (10 for dragged parts) passed in, never read (`getObjMass` has no caller) | `Gui/ViewProviderAssembly.cpp:1135-1140`, `:2042-2059` |
| Gui calls | `Gui/ViewProviderAssembly.cpp:1145` (preDrag), `:637` (doDragStep), `:1181-1182` (postDrag), gated by `SolveOnMove` |

### 1.5 Kinematic simulation

| what | where |
|---|---|
| `generateSimulation`: new model with motions, no limits, `runKINEMATIC` | `:296-329`, `OS/ASMTAssembly.cpp:1442-1451` |
| playback: `updateForFrame` asks the live ASMT model to restore frame i, then writes placements | `:345-361`, `numberOfFrames` `:363-366` |
| Python | `CommandCreateSimulation.py:927-935` (run, slider), `:977`, `:1041`, `:1120`, `:1157` (playback and export loop) |
| simulation object properties `aTimeStart`, `bTimeEnd`, `cTimeStepOutput`, `fGlobalErrorTolerance`, `jFramesPerSecond` | `CommandCreateSimulation.py:108-171` |
| motion object: `MotionType` Angular / Linear, `Formula`, `Joint` | `CommandCreateSimulation.py:290-339` |
| formula grammar documented to the user (Ondsel's parser) | `CommandCreateSimulation.py:541-560` |

### 1.6 Status, DOF, redundancy

| what | where |
|---|---|
| `updateSolveStatus`: DOF = 6 x (1 + components) minus **a count of constraints** (not a rank); redundancy by scanning `constraintSpec()` for a `"Redundant"` prefix; joint names recovered by splitting the solver path at `#` | `:220-294` |
| conflicts, partial redundancy, malformed: never set (audit defect 1) | ctor `:110-113`, getters `App/AssemblyObject.h:216-255` |
| solve <-> status recursion (audit defect 3) | `:201, :215, :227-229` |
| consumers | `Gui/ViewProviderAssembly.cpp:322` (signal), `:1842-1893` (messages), `Gui/Commands.cpp:181-249` (select redundant / malformed; comment: "the solver currently reports conflicting constraints as redundant") |

### 1.7 Export

| what | where |
|---|---|
| `exportAsASMT`: builds a model (no solve) and writes ASMT text | `:569-580`, `OS/ASMTAssembly.cpp:1284` |
| bindings and command | `App/AssemblyObjectPyImp.cpp:180-195`, `App/AssemblyObject.pyi:147`, `CommandExportASMT.py:78` |
| import is a stub | `AssemblyImport.py:25-32`, registered `Init.py:38-39` |

### 1.8 Python reaching the solver

All through `AssemblyObjectPy` (`App/AssemblyObjectPyImp.cpp:47-234`,
`App/AssemblyObject.pyi:22-165`):

| call | where |
|---|---|
| `assembly.solve(storePrev)` behind `solveIfAllowed` | `JointObject.py:146-150`; callers `:804, :809, :814, :861`, `CommandInsertNewPart.py:202` |
| `undoSolve`, `clearUndo` | `JointObject.py:869, :1752` |
| connectivity queries used to decide what a pre-solve may move; **`Suppressed` written to ask** | `JointObject.py:933-936, :965, :1055-1056, :1221` |
| `isJointConnectingPartToGround` | `JointObject.py:1006-1007` |
| pre-solve (match JCS so Newton starts in the right branch) | `JointObject.py:912-981` |
| Ondsel-specific workaround: rotate a part 10 deg so Angle / Perpendicular do not start singular | `JointObject.py:993-1036` (comment at `:994`) |
| joint task accept: a full recompute, i.e. a second solve | `JointObject.py:1725` |
| joint-type lists, mirrored by the C++ enum | `JointObject.py:65-143`, `App/AssemblyUtils.h:48-64` |
| simulation and export | sec 1.5, 1.7 |

Not solver-facing but on the same path: `getJoints`, `isPartConnected`,
`getDownstreamParts` (audit F3, F4) -- they belong to the neutral layer's
connectivity index (sec 4.4), not to any backend.


## 2. Upstream PR #28156

Summary:

- **Author** oursland; opened 2026-03-06; last code push 2026-06-29;
  **draft since 2026-08-31** (converted by chennes); mergeable =
  CONFLICTING; milestone 27.1; Ubuntu CI failing on the head. 23 files,
  +4951/-498, 11 commits. FPA grant #47 (funded Q2 2025) planned: the
  abstraction plus a compile-time second solver, then runtime selection,
  then Python-installable solver addons.
- **Interface** (`App/Solver.h` 692 lines, `Solver.cpp`, `SolverRegistry.*`,
  `OndselSolver.*`, `ChronoSolver.*`, namespace `Assembly::Solver`):
  `using MarkerRef = std::string` (documented as Ondsel's path format);
  `Item`/`ItemIJ` with names and marker refs; `enum class JointClass` with
  20 values that are the ASMT classes; one concrete data class per joint
  with typed setters; `Limit` carrying an **expression string**;
  `Motion` carrying **Ondsel formula strings**; abstract `Part`
  (`pushPlacement`, `addMarker`), abstract `Assembly` (`addPart`,
  `addGroundMarker`, `addJoint`, `addLimit`, `addMotion`, `solveStatic`,
  `runKinematic`, `preDrag(DragContext)` with pick point and camera,
  `dragStep(parts, mousePos3D)`, `postDrag`, `numberOfFrames`,
  `updateForFrame`, `querySolveStatus` -> `{constraintsApplied, joints[name,
  isRedundant]}`, `exportFile`, `setDebug`); `AssemblySolver` factory with a
  `make*()` per joint. Registry singleton; factory takes the raw
  `AssemblyObject*`. Backend chosen by preference `Mod/Assembly/SolverBackend`
  (default `"Ondsel"`), applied by a new `AssemblyObject::resetSolver()`;
  not a document property. `AssemblyApp.getAvailableSolvers()` in Python.
- **What moved**: all solver includes left `AssemblyObject.cpp`; MbD left the
  header. **What stayed**: the whole document-to-joint translation stays in
  `AssemblyObject.cpp`, renamed (`makeMbdJoint` -> `makeJointOnly` +
  `addJointLimitsAndMotions`, `makeMbdJointOfType` -> `makeJointOfType`,
  `getMbDData` -> `getPartData`, `isMbDJointValid` -> `isJointValid`).
  `AssemblyObject.cpp` +381/-404 in 33 hunks. No Python changes.
- **Chrono backend** (Chrono 10.0.0, optional via `find_package(Chrono
  CONFIG QUIET)` -> `HAVE_CHRONO`): `ChLinkLock*` for the lower pairs,
  `ChLinkLockPointPlane/PointLine`, `ChLinkDistance` for SphSph,
  `ChLinkMateRackPinion`, `ChLinkLockScrew/Gear/Pulley`. **Angle, RevCyl,
  CylSph skipped**; Planar's offset dropped; LineInPlane mapped to Planar
  (over-constrains). Motions not implemented, limits not enforced (clamped
  after the step), redundancy always false, DOF miscounts `SetFixed` parts,
  no export. Drag via a mouse body and a spherical lock; the author: "it
  would sometimes make the model break on dragging" (2026-09-21).
- **Defects visible in the diff**: a double `runPreDrag` at every drag start
  (`AssemblyObject::preDrag` solves, then `OndselAssembly::preDrag` runs it
  again); every joint redrawn through Python on every drag step; GUI camera
  data in an App-level interface.
- **Review**: no approvals, no change requests; wording nits (maxwxyz) and
  one design comment (hyarion: the drag sphere belongs in the view
  provider -- done). No maintainer objected to an abstraction. In the grant
  discussion the Assembly maintainers (PaddleStroke, shaise) doubted that
  several solvers are needed ("if the new solver ... indeed faster ... it
  should replace it altogether"); yorik and hyarion favoured abstraction.
- **Staleness**: base `01c7c7002f` (2026-06-27). Since then 15 upstream
  commits touched `AssemblyObject.cpp`, several in the very functions the PR
  rewrites: RigidGroup #29605, island drag #30998, coaxial gear #31155 (a
  joint kind missing from `JointClass`), DoF with bundles `b13dc13b35`,
  rack-pinion #32922, the std::format sweep.

### 2.1 Merge-cost assessment

Adopting the PR's shape would buy conflicts twice: once now against the
upstream commits it has not absorbed, and again when it is rebased and
probably reshaped (a draft, author-acknowledged drag bug, no real
review). Its interface also blocks every goal of the rework: a string
marker path and a rebuild per solve rule out a warm model; Ondsel
expression strings in limits and motions tie every backend to Ondsel's
parser; `querySolveStatus` cannot carry conflicts or per-body DOF.

What is worth taking, because it costs nothing and lowers the cost of a
future merge whichever way the PR goes:

- the namespace `Assembly::Solver`;
- the preference key and values: `Mod/Assembly/SolverBackend`, `"Ondsel"`,
  `"Chrono"`;
- `AssemblyApp.getAvailableSolvers()` and a `resetSolver()` hook with the
  same meaning;
- **keeping the translation code in `AssemblyObject.cpp`**, in upstream's
  function bodies, so upstream fixes to it still land as local hunks;
- optional backends via `find_package` + `#ifdef`, compiled only when found.

What not to share: the paths. The PR adds `App/Solver.h`,
`App/SolverRegistry.h`, `App/OndselSolver.h`, `App/ChronoSolver.h`. If ours
used the same paths with different content, an upstream merge would be an
add/add conflict over every file with nothing reusable on either side.
With ours in `App/Solver/`, an upstream merge of the PR lands beside ours,
and reconciling becomes one deliberate change: either wrap their Chrono
backend as one of our backends (their item model is a mechanical
translation from ours) or drop it.


## 3. What Assembly3's backend layer teaches

Source: `D:\works\sw\FreeCAD_assembly3\freecad\asm3\`.

- **Registration** by metaclass: each `System` subclass registers by an
  integer id and a name (`proxy.py:98, 275-308`; `system.py:15, 102-124`;
  `sys_slvs.py:28-42`; `sys_sympy.py:213-239`). **Selection is per
  assembly** through a persisted hidden id plus a visible enumeration
  (`proxy.py:232-262`), default the first real backend, no global
  preference. Backends attach their own properties to the assembly while
  selected (`system.py:99-104`). A file naming a missing backend gets an
  install prompt and a lazy retry (`system.py:31-40`, `proxy.py:258-259`).
- **The API is py-slvs, by duck typing**: params, entities (point, normal,
  workplane, line, circle, `addTransform` over a 7-param pose), constraints
  with a trailing `group=`, `solve(group, reportFailed, findFreeParams)`,
  read-back of `Dof` (only logged) and `Failed` (constraint handles mapped
  back to document constraints, `solver.py:116-133`). Groups do three jobs
  at once: unknowns (group 1), constants and fixed parts (group 2), and
  provenance (one group per document constraint, `solver.py:42-87`).
- **Translation**: constraint classes carry no backend code; they bind by
  name, `getattr(system, 'add' + ClassName)` (`constraint.py:852-860`), and
  are skipped as "not supported in solver" when the method is missing,
  filtered at solve (`assembly.py:4356-4359`) and disabled at insert
  (`assembly.py:2436-2441`). Composite assembly constraints are lowered to
  slvs primitives in a `SystemExtension` mixin every backend inherits
  (`system.py:146-466`), together with redundancy / auto-relax
  **heuristics** that count same-type constraints per part pair
  (`system.py:208-420`; its authors note it can fail).
- **Entities**: per-element builders (`_p`, `_n`, `_l`, `_w`, `_c`,
  `constraint.py:30-403`) make a constant local entity transformed by the
  part's 7 pose params; cached per part and element. Fixed parts are
  constants, not equations (`constraint.py:532-703`), and Attachment chains
  to a fixed part are solved analytically before the solver sees them
  (`constraint.py:605-701`).
- **No warm model**: a fresh system per solve and per drag frame
  (`sys_slvs.py:41-42`); drag only moves the initial guess
  (`mover.py:239-260`; `addWhereDragged` commented out, `solver.py:96-106`).
  Write-back skips unchanged parts and rolls back on failure
  (`solver.py:137-220, 384-399`). No component split.
- **sympy backend**: residual classes per primitive, single-symbol
  equations peeled off, the rest a sum of squares to
  `scipy.optimize.minimize` (Powell by default). No DOF, no failure list,
  slow, shelved; its value is as a reference oracle and a runnable dump of
  the system (`sys_sympy.py:1102-1138, 1147-1294`).

Taken into the design below:

- element geometry -> frame on a body is separate from the backend;
- a body is one pose block; grounded means constant, not extra equations;
- loop-free attachments to ground are placed before the solver (sec 4.4);
- every backend handle maps back to its document object (here, by owner id);
- a capability query, used by the UI;
- unchanged placements are not written; a failed solve rolls back;
- a reproducible dump of the solver input (ASMT text for Ondsel; the
  neutral model's own serialisation for every backend).

Avoided: binding by method name; lowering and redundancy heuristics inside
the backend; residuals whose branch depends on current values (they break a
persistent system); a rebuild per solve and per drag frame; drag as a mere
initial guess; groups overloaded with three meanings; status by exception.


## 4. The proposed interface

### 4.1 Layers

```
 Python (JointObject.py, CommandCreateSimulation.py, ...)
   |  unchanged AssemblyObject methods
 AssemblyObject  (facade: solve, preDrag/doDragStep/postDrag,
   |              generateSimulation, updateForFrame, getLast*, export)
 SolverContext   (neutral layer, one per assembly; App/Solver/Context.*)
   |-- ModelBuilder: document -> Model   (the code now in makeMbd*,
   |                                      handleOneSideOfJoint, ...; stays
   |                                      in AssemblyObject.cpp, sec 6.1)
   |-- Decomposer:   bundles, ground, components, topology keys
   |-- Session cache: TopologyKey -> warm Session
   |-- Checker:      residual per joint, diagnostics merge
   |-- Trajectory store, write-back with tolerance
 Backend / Session  (abstract; App/Solver/Backend.h)
   |-- OndselBackend   (the only file including OndselSolver headers)
   |-- ReferenceBackend (C++ Eigen LM on the neutral residuals; test oracle)
   |-- PyBackend       (trampoline to a Python object: py-slvs, scipy, addons)
   |-- ChronoBackend   (optional, find_package)
   |-- RemoteBackend   (any of the above in a solver process)
```

### 4.2 The neutral model (`App/Solver/Model.h`)

Plain data only: no `App::DocumentObject*`, no shared_ptr graph, no
strings on the hot path. `Base::Placement` is used in process; the wire
form is 7 doubles.

```cpp
namespace Assembly::Solver {

using Id = std::uint32_t;
inline constexpr Id NoId = 0xffffffffu;

struct MassProps {
    double mass = 1.0;                       // today's placeholder, audit defect 2
    Base::Vector3d com;                      // in the body frame
    std::array<double, 3> principal {1, 1, 1};
    Base::Rotation principalAxes;
};

struct Body {
    Id id = NoId;                 // stable while the object lives; assigned by the context
    Id source = NoId;             // index into Model::sources (the document object)
    Base::Placement placement;    // current pose, the solve's starting point
    bool grounded = false;
    MassProps mass;
    double dragWeight = 1.0;      // replaces setObjMasses (Gui/ViewProviderAssembly.cpp:1137)
};

// A marker: a frame fixed on a body, given in the body frame.
struct Frame {
    Id id = NoId;
    Id body = NoId;
    Base::Placement local;
};

enum class JointKind : std::uint8_t {
    // lower pairs                                  equations
    Fixed,            // frames coincide                 6
    Revolute,         // origins coincide, zI || zJ      5
    Cylindrical,      // oJ on axis zI, zI || zJ         4
    Prismatic,        // oJ on axis zI, rotations equal  5
    Spherical,        // origins coincide                3
    Planar,           // oJ in plane(I)+value, zI || zJ  3
    // orientation only
    ParallelZ,        // zI || zJ                        2
    PerpendicularZ,   // zI . zJ = 0                     1
    AngleZ,           // angle(zI, zJ) = value           1
    // incidence with an offset
    PointInPlane,     // oJ in plane(I) + value          1
    LineInPlane,      // PointInPlane and zI . zJ = 0    2
    PointOnLine,      // oJ on axis zI                   2   (unused today)
    // distances
    PointPointDistance,  // |oJ - oI| = value            1
    PointLineDistance,   // dist(oJ, axis zI) = value    1
    LineLineDistance,    // axes parallel at value       3
    // couplers (relative rotations / slides about z)
    Gear,             // value*thetaI + value2*thetaJ   1   (Belt: value2 < 0)
    CoaxialGear,      // as Gear, angles measured against carrier frame K
    RackPinion,       // slide(I along x) = value*theta(J)  1
    Screw,            // slide(I along z) = value*theta/(2 pi)  1
    // reserved: Universal, ConstantVelocity, Tangent, Slot, Cam, Path
    Count_
};

struct Joint {
    Id id = NoId;
    Id source = NoId;             // owning document joint; one source may own several
    JointKind kind {};
    Id frameI = NoId, frameJ = NoId, frameK = NoId;   // K only for CoaxialGear
    double value = 0.0;           // offset | distance | angle (rad) | radius | pitch
    double value2 = 0.0;          // second radius
};

enum class LimitKind : std::uint8_t { TranslationZ, RotationZ };
struct Limit {
    Id id = NoId, source = NoId, joint = NoId;
    LimitKind kind {};
    std::optional<double> min, max;   // mm, rad -- numbers, never expression strings
    double tolerance = 1e-9;
};

// A formula of time in a grammar the neutral layer owns and can evaluate:
// numbers, time, pi, + - * / ^, sin cos tan asin acos atan exp ln log sqrt
// abs. 'initialValue' is substituted before it gets here.
struct Formula { std::string text; };
enum class DriverKind : std::uint8_t { RotationZ, TranslationZ, Both };
struct Driver {
    Id id = NoId, source = NoId, joint = NoId;
    DriverKind kind {};
    Formula rotation, translation;
};

struct Model {
    std::uint32_t version = 1;
    std::vector<std::string> sources;   // document names, for diagnostics and dumps
    std::vector<Body> bodies;           // in insertion order (sec 6.2: order matters)
    std::vector<Frame> frames;
    std::vector<Joint> joints;
    std::vector<Limit> limits;
    std::vector<Driver> drivers;
};

// Structure only: bodies, grounded flags, joint kinds and the frames' bodies,
// limits present / absent, drivers present / absent. Values do not enter it.
struct TopologyKey { std::uint64_t hash; bool operator==(const TopologyKey&) const = default; };
TopologyKey topologyKey(const Model&);

// Values only, against a model with the same TopologyKey.
struct BodyPose { Id body; Base::Placement placement; };
struct ModelDelta {
    std::vector<BodyPose> bodies;                          // moved outside the solver
    std::vector<std::pair<Id, Base::Placement>> frames;    // offsets, re-picks of same kind
    struct JointValue { Id joint; double value, value2; };
    std::vector<JointValue> joints;
    std::vector<Limit> limits;                             // changed bounds
    std::vector<Driver> drivers;                           // changed formulas
};
ModelDelta diff(const Model& before, const Model& after);  // requires equal keys

}  // namespace Assembly::Solver
```

Notes:

- **Grounding is a flag, not a joint.** The Ondsel adapter turns it back
  into today's assembly marker + `FixingMarker` + `ASMTFixedJoint`
  (`:912-938`); other backends make the pose constant (Assembly3's group 2,
  Chrono's `SetFixed`).
- **Suppressed joints are absent**, so suppressing changes the topology key.
- **Owner ids replace string paths.** Diagnostics come back as joint ids,
  mapped to `sources`; nothing parses `"/OndselAssembly/...#Joint001"`
  (`:236-244`) any more.
- **One document joint -> several solver records** (a joint, two limits, a
  driver; a CoaxialGear's carrier), all carrying the same `source`.

### 4.3 Backend, session, results (`App/Solver/Backend.h`)

```cpp
namespace Assembly::Solver {

enum class Status : std::int8_t {
    Ok = 0, Failed = -1, NoGround = -6,      // -1 and -6 keep today's return codes
    NotConverged = -2, Singular = -3, Unsupported = -4, NeedsReload = -5,
    Cancelled = -7,
};

struct Diagnostics {
    int dof = -1;                              // -1: unknown
    std::vector<int> bodyDof;                  // per Model::bodies; empty: unknown
    std::vector<Id> redundant;                 // joint ids
    std::vector<Id> conflicting;
    std::vector<Id> partiallyRedundant;
    std::vector<Id> malformed;                 // e.g. a self-referencing joint
    std::vector<Id> unsupported;               // kinds this backend cannot express
    std::vector<std::vector<Id>> overDefinedGroups;   // minimal sets, optional
    std::uint32_t known = 0;                   // bit per field: "the backend computed it"
};

struct Result {
    Status status = Status::Failed;
    std::vector<BodyPose> moved;               // only bodies that moved beyond tolerance
    Diagnostics diagnostics;                   // when requested
    double residual = 0.0;
    int iterations = 0;
    std::string message;
};

struct Capabilities {
    std::bitset<static_cast<size_t>(JointKind::Count_)> joints;
    bool limits = false, drivers = false, drag = false, kinematics = false,
         dynamics = false, warmUpdate = false, redundancy = false,
         conflicts = false, bodyDof = false, outOfProcess = false;
    std::string nativeFormat;                  // "asmt" for Ondsel, empty if none
};

struct SolveOptions {
    double tolerance = 1e-9;
    int maxIterations = 0;                     // 0: backend default
    bool diagnostics = true;
};
struct DragOptions { bool bundleFixed = true; };
struct SimulationSpec {
    double tStart = 0, tEnd = 1, tOutput = 1e-2, errorTolerance = 1e-6;
    double hMin = 1e-9, hMax = 1.0;            // today hard-coded at :1187-1188
};

// Trajectory: times x bodies poses; owned by the neutral layer, so playback
// needs no live solver (today updateForFrame asks the ASMT model, :356).
struct Trajectory {
    std::vector<Id> bodies;
    std::vector<double> times;
    std::vector<Base::Placement> poses;        // times.size() * bodies.size()
};
class TrajectorySink {
public:
    virtual ~TrajectorySink() = default;
    virtual bool frame(double time, const std::vector<BodyPose>& poses) = 0;   // false: cancel
};

// One warm system: one connected component (or the whole model).
class Session {
public:
    virtual ~Session() = default;
    virtual Status load(const Model& model) = 0;
    virtual Status update(const ModelDelta& delta) = 0;     // NeedsReload if it cannot
    virtual Result solve(const SolveOptions&) = 0;

    virtual Status beginDrag(const std::vector<Id>& bodies, const DragOptions&) = 0;
    virtual Result dragStep(const std::vector<BodyPose>& targets) = 0;
    virtual Result endDrag() = 0;                            // no full re-solve required

    virtual Diagnostics analyse() = 0;                       // at the current state
    virtual Status simulate(const SimulationSpec&, TrajectorySink&);   // default: sec 4.6
    virtual Status exportNative(const std::string& path) { return Status::Unsupported; }
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;                    // "Ondsel"
    virtual std::string label() const = 0;                   // translated, for the UI
    virtual Capabilities capabilities() const = 0;
    virtual std::unique_ptr<Session> createSession() = 0;
};

class Registry {
public:
    static Registry& instance();
    void add(std::unique_ptr<Backend>);                      // C++ at module init; Python via PyBackend
    Backend* find(std::string_view name) const;
    std::vector<std::string> names() const;
    // Assembly property 'SolverBackend' if set and known, else the preference
    // Mod/Assembly/SolverBackend, else "Ondsel". An unknown name warns once.
    Backend* select(const AssemblyObject&) const;
};

}  // namespace Assembly::Solver
```

`Session` is deliberately small and value-typed: every argument and result
is data that can be serialised (sec 4.7). Nothing GUI-side crosses it
(the PR's camera and pick point stay in the view provider: the view
provider turns a mouse move into a target pose, as it does today at
`Gui/ViewProviderAssembly.cpp:545-626`).

### 4.4 The neutral layer: done once, not per backend

`SolverContext` (owned by `AssemblyObject`, replacing `mbdAssembly`,
`objectPartMap`, `draggedParts`, `motions`):

1. **Build.** `ModelBuilder` turns the document into a `Model`. It owns
   everything that today is mixed into the ASMT translation but has nothing
   to do with a solver: element classification (`getDistanceType`), radius
   folding (`getFaceRadius`/`getEdgeRadius`), slider detection for Screw
   and RackPinion, the rack frame re-orientation, Angle == 0 -> ParallelZ,
   PointPoint with d == 0 -> Spherical, limit min/max ordering, `initialValue`
   substitution in formulas, and the coaxial-gear carrier search (upstream
   `c1df66bce9`). **It must not write the document**: today
   `getDistanceType` and `slidingPartIndex` call `swapJCS` and
   `makeMbdJoint` swaps limit properties (`App/AssemblyUtils.cpp:178-362`,
   `:1245, :1739, :1479-1489, :1530-1540`). The builder swaps its own frames
   I and J instead. (Keeping the document write in step 3 of sec 6 keeps
   results identical; removing it is its own step, because the swapped
   document is what the next build reads.)
   Cost: one pass over joints with C++ frame math -- small next to a solve
   once the connectivity index of audit F3/F4 exists. **Rebuild and diff**
   is chosen over change tracking: it needs no observer wiring into joint
   property changes, and it cannot miss an edit.
2. **Bundle.** Union-find over Fixed joints, upstream's RigidGroups
   (`3a9ab5483f`, `rebuildRigidClusters`), and rigid sub-assemblies (already
   one moving part, `App/AssemblyUtils.cpp:665-720`). Each bundle is one
   `Body` with member offsets (today's `MbDPartData.offsetPlc`,
   `App/AssemblyObject.h:131-135`). All grounded bodies are merged into one
   ground body. A joint with both frames in one bundle produces no
   equations; the checker evaluates its residual instead and reports it
   redundant if satisfied, conflicting if not -- today it is dropped with a
   warning (`:1859-1880`).
3. **Split.** Connected components over the joints between non-ground
   bundles; ground is a shared fixed boundary, so two mechanisms hanging off
   ground are independent. A component with no ground is an island:
   skipped as today (`removeUnconnectedJoints`), or solved with one body
   held when dragged (upstream `16dd3cc01f`).
4. **Place loop-free parts directly** (later, audit sec 8.2 phase 2.2): a
   component that is a chain of Fixed joints from ground is already one
   bundle after step 2; a pendant body hanging by one lower pair can be
   projected onto that pair's manifold in closed form. This is per joint
   kind, in the neutral layer, and backend-independent.
5. **Keep sessions warm.** Each component has a `TopologyKey`. The context
   keeps `TopologyKey -> unique_ptr<Session>` (a small LRU, so toggling a
   joint off and on reuses the old session). Same key as last time:
   `session->update(diff(old, new))`; different: `createSession()->load()`.
   A backend without `warmUpdate` answers `NeedsReload` and gets a `load`.
6. **Solve only what changed.** A component is dirty when the delta touches
   one of its bodies, frames, joints, limits or drivers, or when it was just
   loaded. Clean components are not solved -- the early-out audit F1 asks
   for, given to every backend for free.
7. **Check.** After a solve, the neutral layer evaluates every joint's
   residual from its definition in sec 4.2 (a few lines per kind). This
   replaces `validateNewPlacements` (`:475-506`, which only checks grounded
   parts) with a backend-independent test: a joint left unsatisfied after an
   `Ok` is reported conflicting, which closes audit defect 1 for every
   backend, including those without diagnostics. The same residuals give
   the `ReferenceBackend` and an optional rank analysis (DOF, redundancy,
   per-body DOF by SVD / column-pivoted QR of their Jacobian) for backends
   whose `Capabilities` lack it.
8. **Write back** once, in a batch, with a tolerance (audit defect 7, F6),
   and during a drag only to view transforms (audit F7), committing at
   `endDrag`.
9. **Map diagnostics** from ids to document objects, filling
   `lastRedundantJoints` etc. (`App/AssemblyObject.h:270-280`), which keeps
   `Gui/ViewProviderAssembly.cpp:1842-1893` and `Gui/Commands.cpp:181-249`
   unchanged.

### 4.5 Incremental updates in practice

| user action | topology | what crosses the interface |
|---|---|---|
| edit a Distance / Angle / Offset / gear radius | same | `update({joints or frames})`, solve that component |
| drag a part | same | `beginDrag`, `dragStep(target poses)` per mouse move, `endDrag` |
| move a part by its Placement, recompute | same | `update({bodies})`, solve its component |
| toggle a limit's value | same | `update({limits})` |
| enable / disable a limit, suppress a joint, add a joint, ground a part, add a part | changed | `load()` for the components affected; others untouched |
| re-pick a reference to an element of another kind | changed (kind differs) | `load()` |

Warm `update` needs OndselSolver to accept new marker placements, joint
values and part poses on an existing `System` without `runPreDrag` building
a new one (`OS/ASMTAssembly.cpp:1342`). That is part of the solver rework
(audit sec 8.2 phase 2.1: the symbolic factorisation is kept per topology);
until then the Ondsel adapter reports `warmUpdate = false`.

### 4.6 Kinematics without a kinematic backend

A kinematic run with prescribed drivers is a sequence of position solves:
at each output time evaluate the driver formulas, set the driven joint
coordinates, solve warm from the previous frame. The default
`Session::simulate` does exactly that, using the neutral formula evaluator,
so any backend that can solve and accept a driven coordinate (implemented
as a temporary joint value or an extra equation) plays a simulation.
OndselSolver overrides it with `runKINEMATIC` (which also integrates
velocities and accelerations), streaming frames into the sink. Either way
the trajectory is stored in the neutral layer, which is also
`docs/Simulator.md` sec 7's "trajectory, recorded and played back" -- the
same format later serves Chrono dynamics and the browser tier.

The formula grammar must be written down as the workbench's, not Ondsel's.
Phase one can keep using OndselSolver's `SymbolicParser` as the evaluator
(it is linked anyway); a backend that parses formulas itself must accept
the same grammar.

### 4.7 Out of process

Everything in sec 4.2 and 4.3 is plain data, so `RemoteBackend` is a
`Backend` whose `Session` serialises calls over a pipe to a solver process
that hosts any in-process backend. The warm system lives there; per drag
step only the target poses go out and the moved poses come back (tens of
bytes per body), so the pipe adds tens of microseconds to a step that costs
milliseconds. A crash in the solver process surfaces as `Status::Failed`;
the context respawns it and reloads the model from its own copy.

It fits `docs/ComputeBoundaries.md` as follows: the solve is not a DAG
edge (Boundary B) and not a semantic operation (Boundary A); it is a
session configured from document state, which is `docs/Simulator.md` sec 7
Boundary C. Interactive drag follows the "two-tier client" rule of
ComputeBoundaries sec 1: the context may hold a local in-process session
for dragging and a remote one for heavy solves and simulation, chosen per
purpose by the registry.

### 4.8 Selection

- Preference `Mod/Assembly/SolverBackend` (string, default `"Ondsel"`), in
  `App/AssemblyParams.py` beside `SolveOnRecompute` (`:42`), same key as the
  PR.
- An optional per-assembly property `SolverBackend` (enumeration of the
  registered names; empty = use the preference) -- Assembly3's lesson that a
  document should be able to say which solver it was built with. Fork-only,
  added in the constructor region we already own (`:118-125`); a file naming
  an unknown backend falls back with one warning.
- Python: `AssemblyApp.getAvailableSolvers()` (the PR's name) and
  `assembly.solverCapabilities()` (new), used by the joint task.


## 5. Mapping the workbench joints

### 5.1 The 13 joint types (`App/AssemblyUtils.h:49-64`, `:1192-1270`)

| joint | neutral | Ondsel class today | limits / drivers |
|---|---|---|---|
| Fixed | Fixed; folded into a bundle when bundling is on | `ASMTFixedJoint` (none when `bundleFixed`) | -- |
| Revolute | Revolute | `ASMTRevoluteJoint` | RotationZ limit; RotationZ driver |
| Cylindrical | Cylindrical | `ASMTCylindricalJoint` | both limits; RotationZ, TranslationZ or Both driver (`ASMTGeneralMotion`) |
| Slider | Prismatic | `ASMTTranslationalJoint` | TranslationZ limit; TranslationZ driver |
| Ball | Spherical | `ASMTSphericalJoint` | -- |
| Distance | by sub-type, sec 5.2 | 9 classes | -- |
| Parallel | ParallelZ | `ASMTParallelAxesJoint` | -- |
| Perpendicular | PerpendicularZ | `ASMTPerpendicularJoint` | -- |
| Angle | AngleZ(abs(angle)); ParallelZ when angle == 0 mod 2 pi | `ASMTAngleJoint` / `ASMTParallelAxesJoint` | -- |
| RackPinion | RackPinion(pitch radius); I = rack frame re-oriented by the builder | `ASMTRackPinionJoint` | -- |
| Screw | Screw(pitch); I = the sliding side | `ASMTScrewJoint` | -- |
| Gears | Gear(r1, r2); CoaxialGear(r1, r2, K) after upstream `c1df66bce9` | `ASMTGearJoint` | -- |
| Belt | Gear(r1, -r2) | `ASMTGearJoint` | -- |
| GroundedJoint | `Body::grounded` | marker + `ASMTFixedJoint` (`:912-938`) | -- |

### 5.2 The Distance sub-types (`App/AssemblyUtils.h:83-129`, `:1272-1429`)

The enum has 37 values (with `Other`). `d` is the Distance property, `r1`,
`r2` the radii of the first and second element after the builder's
ordering. 20 have a case; 17 fall to the default Planar(d).

| sub-type | neutral (value) | Ondsel class |
|---|---|---|
| PointPoint | Spherical if d == 0, else PointPointDistance(d) | `ASMTSphericalJoint` / `ASMTSphSphJoint` |
| LineLine | LineLineDistance(d) | `ASMTRevCylJoint` |
| LineCircle | LineLineDistance(d + r2) | `ASMTRevCylJoint` |
| CircleCircle | LineLineDistance(d + r1 + r2) | `ASMTRevCylJoint` |
| PlanePlane | Planar(d) | `ASMTPlanarJoint` |
| PlaneCylinder | LineInPlane(d + r2) | `ASMTLineInPlaneJoint` |
| PlaneSphere | PointInPlane(d + r2) | `ASMTPointInPlaneJoint` |
| PlaneTorus | Planar(d) | `ASMTPlanarJoint` |
| CylinderCylinder | LineLineDistance(d + r1 + r2) | `ASMTRevCylJoint` |
| CylinderSphere | PointLineDistance(d + r1 + r2) | `ASMTCylSphJoint` |
| CylinderTorus | LineLineDistance(d + r1 + r2) | `ASMTRevCylJoint` |
| TorusTorus | Planar(d) | `ASMTPlanarJoint` |
| TorusSphere | PointLineDistance(d + r1 + r2) | `ASMTCylSphJoint` |
| SphereSphere | PointPointDistance(d + r1 + r2) | `ASMTSphSphJoint` |
| PointPlane | PointInPlane(d) | `ASMTPointInPlaneJoint` |
| PointCylinder | PointLineDistance(d + r1) | `ASMTCylSphJoint` |
| PointSphere | PointPointDistance(d + r1) | `ASMTSphSphJoint` |
| LinePlane | LineInPlane(d) | `ASMTLineInPlaneJoint` |
| PointLine | PointLineDistance(d) | `ASMTCylSphJoint` |
| PointCurve | PointInPlane(d) (plane of the curve) | `ASMTPointInPlaneJoint` |
| PlaneCone, CylinderCone, ConeCone, ConeTorus, ConeSphere, PointCone, PointTorus, LineCylinder, LineSphere, LineCone, LineTorus, CurvePlane, CurveCylinder, CurveSphere, CurveCone, CurveTorus, Other | Planar(d) (default) | `ASMTPlanarJoint` |

So the whole Distance family needs only seven neutral kinds: Spherical,
Planar, PointInPlane, LineInPlane, PointPointDistance, PointLineDistance,
LineLineDistance. The default row is questionable (Planar also locks the
relative orientation, e.g. for a point on a cone) and should become a
proper mapping or "unsupported" later, but it is kept as is for identical
results.

The 9 OndselSolver joint classes the workbench does not use
(`ASMTUniversalJoint`, `ASMTConstantVelocityJoint`, `ASMTPointInLineJoint`,
`ASMTRevRevJoint`, `ASMTNoRotationJoint`, `ASMTAtPointJoint`,
`ASMTInLineJoint`, `ASMTInPlaneJoint`, `ASMTCompoundJoint`) are where the
reserved kinds Universal and PointOnLine would map first -- the audit's
missing "universal" joint already exists in the solver.

### 5.3 What each backend can express

`N` native, `E` expressible by composing primitives in the adapter, `S`
neutral-layer stepping, `-` not available. Chrono per the PR's code and the
Chrono 10 API; SolveSpace per py-slvs / libslvs as used by Assembly3.

| neutral | Ondsel | SolveSpace | Chrono | scipy / Reference |
|---|---|---|---|---|
| Fixed | N | E coincident + same orientation | N `ChLinkLockLock` | N |
| Revolute | N | E coincident + parallel normals | N `ChLinkLockRevolute` | N |
| Cylindrical | N | E point on line + parallel | N | N |
| Prismatic | N | E point on line + same orientation | N | N |
| Spherical | N | N coincident | N | N |
| Planar(off) | N | E point-plane distance + parallel | N lock planar; offset needs a shifted frame (PR dropped it) | N |
| ParallelZ | N | N parallel | N `ChLinkLockParallel` | N |
| PerpendicularZ | N | N perpendicular | N `ChLinkLockPerpend` | N |
| AngleZ | N | N angle | - (PR skipped; a `ChLinkMateGeneric` or custom link) | N |
| PointInPlane(off) | N | N point-plane distance | N `ChLinkLockPointPlane` with shifted frame | N |
| LineInPlane(off) | N | E point-plane distance + perpendicular | - (PR mapped to Planar: wrong) | N |
| PointOnLine | N (unused) | N | N `ChLinkLockPointLine` | N |
| PointPointDistance | N | N | N `ChLinkDistance` | N |
| PointLineDistance | N | N point-line distance | - (PR skipped) | N |
| LineLineDistance | N | E two point-line distances + parallel | - (PR skipped) | N |
| Gear / Belt | N | - | N `ChLinkLockGear` / `ChLinkLockPulley` | N |
| CoaxialGear | N (upstream) | - | ? (carrier frame) | N |
| RackPinion | N | - | N `ChLinkMateRackPinion` | N |
| Screw | N | - | N `ChLinkLockScrew` | N |
| limits | N (inequality, applied while dragging) | - | after-step clamp only (PR) | bounds / penalty |
| drivers | N symbolic | S | N with `ChFunction` (PR: not done) | S |
| drag | N | initial guess only (`addWhereDragged` unreliable) | mouse body (PR, unstable) | initial guess |
| DOF / redundancy | count; redundancy by marking; no conflicts | `Dof`, `Failed` list | none (PR reports false) | rank of the neutral Jacobian |
| native dump | ASMT | slvs dump | -- | the neutral model |

Two constraints on the list: SolveSpace (libslvs and py-slvs) is **GPLv3**,
so it can only be a user-installed addon through `PyBackend` or a separate
solver process, never linked into the LGPL build -- exactly how Assembly3
ships it. Chrono is BSD-3 and OndselSolver LGPL. And a backend reports what
it lacks through `Capabilities`; the joint task greys those joint types out
and the context reports existing joints of those kinds in `unsupported`
(Assembly3's skip-and-say, but typed).


## 6. Migration

### 6.1 Ground rules for merge cost

1. **Port the upstream commits that touch the coupling first** (audit sec 6):
   `3a9ab5483f` RigidGroup, `c1df66bce9` coaxial gear, `16dd3cc01f` island
   drag, `141037482c` robustness set, `b13dc13b35` DoF with bundles,
   `2372eb1c4f` rack-pinion, `15bd466ccb` rotation order, `6e90683e59`.
   Measured against upstream history, those land exactly in `preDrag`,
   `doDragStep`, `solve`, `handleOneSideOfJoint`, `makeMbdJoint`,
   `updateSolveStatus`, `slidingPartIndex`, `getRackPinionMarkers`,
   `setNewPlacements` -- the functions this work changes. Upstream touched
   `AssemblyObject.cpp` 47 times since 2025-01; about two commits a
   month, and the pending ones above all land in these functions.
2. **The translation stays in `AssemblyObject.cpp`, under upstream's
   function names and in upstream's statement order.** Only the emitted
   type changes, line for line: `auto mbdJoint =
   CREATE<ASMTRevCylJoint>::With(); mbdJoint->distanceIJ = X;` becomes
   `auto mbdJoint = model.addJoint(JointKind::LineLineDistance);
   mbdJoint->value = X;`. An upstream fix to these lines then conflicts only
   on the substituted token and is resolved mechanically. No renames (the
   PR renames; if it lands, its renames are one more mechanical pass).
3. **New code goes in new files** (`App/Solver/*`), which upstream never
   touches, and the public `AssemblyObject` API stays as is.
4. **Each step is its own commit, with a gate.**

### 6.2 The identity gate

`exportAsASMT` writes the complete solver input as text. A test builds the
model through the old path and through the new one, in the same process on
the same document, and compares the two ASMT texts byte for byte; then it
solves both and compares placements exactly. Same process matters: grounded
parts come from an `unordered_set` of pointers (`:896-910`), so part order --
and with it OndselSolver's column order -- varies from run to run. The new
builder must reproduce today's insertion order (grounded parts in set order,
then parts in order of first reference by joints, markers in creation
order), or results drift in the last bits.

Corpus: one assembly per joint type, one per Distance sub-type that has a
case, limits on Slider / Revolute / Cylindrical, one simulation with Angular,
Linear and a Cylindrical with both, the audit's chain and star at N = 10
and 50, a rigid sub-assembly, a bundled drag. Drag is replayed from a
recorded list of target placements through the bench script's entry
points (`scripts/assembly-bench/bench.py`), once Python bindings for
preDrag / doDragStep / postDrag exist (audit sec 8.2 phase 0).

### 6.3 Steps

Results identical (ASMT text and placements equal):

1. **Interfaces only.** Add `App/Solver/Model.h`, `Backend.h`,
   `Registry.{h,cpp}`; compiled, unused.
2. **Ondsel adapter beside the old path.** `App/Solver/OndselBackend.cpp`
   turns a `Model` into an `ASMTAssembly` with today's names and order:
   `"OndselAssembly"`, `"marker-<name>"`, `"FixingMarker"`, joint and
   marker names = the joint's full name, `"-LimitLenMin"` etc., limit values
   printed as today (`std::to_string(min)` and `"<deg>*pi/180.0"` -- the
   neutral model carries numbers, the adapter recreates the exact strings),
   motion names. A test-only `ModelFromAsmt` is not needed: the gate compares
   texts.
3. **Builder switch.** `makeMbdAssembly`, `getMbDData`, `fixGroundedPart`,
   `handleOneSideOfJoint`, `getRackPinionMarkers`, `makeMbdJointOfType`,
   `makeMbdJointDistance`, `makeMbdJoint`, `create_mbdSimulationParameters`
   emit into a `Model` (rule 2); `solve`, `exportAsASMT` and
   `generateSimulation` hand it to the adapter. Document writes during the
   build (`swapJCS`, limit swaps) are kept for now.
4. **Drag and status through the session.** `preDrag`/`doDragStep`/`postDrag`
   call `beginDrag`/`dragStep`/`endDrag`; `doDragStep` still reads the target
   from the document `Placement` (the GUI is unchanged). `updateSolveStatus`
   becomes `Session::analyse` mapped to names; the Ondsel adapter keeps the
   `constraintSpec` scan and the count. Drag masses go into
   `Body::dragWeight` (still unused by the solver, as today).
5. **Header clean.** Remove `OndselSolver/enum.h`, the `MbD` forward
   declarations and every ASMT member from `App/AssemblyObject.h`; drop the
   vestigial gravity include; Gui no longer needs the solver include path.
6. **Trajectory in the neutral layer.** The Ondsel `simulate` copies every
   frame out after `runKINEMATIC` (loop over `updateForFrame` once);
   `AssemblyObject::updateForFrame` plays from the stored `Trajectory`.
   Gate: per-frame placements equal. Remove `freecadAssemblyObject` from
   `OS/ExternalSystem.h` (solver fork) and its two setters.

Results change by design (tolerance gates: placements within 1e-9 mm and
1e-9 rad of the identity-path result on the corpus; DOF and redundancy
lists equal):

7. **The builder stops writing the document** (frames swapped in the model,
   limits ordered in the model). Gate: same placements; the document's
   Reference1/Reference2 order no longer changes on solve.
8. **Warm sessions and value deltas** (needs the OndselSolver rework of
   audit phase 2.1: update markers and values on a live `System`). Remove the
   full re-solve at `endDrag` and the joint task's second solve
   (`JointObject.py:1725`) once the warm result is trusted.
9. **Decomposition in the neutral layer**: always bundle (Fixed, RigidGroup,
   rigid sub-assemblies, all grounded parts), components, dirty-only solves,
   residual check, write-back with tolerance and in a batch. This is where
   audit F1/F2/F6 are paid off for every backend at once.
10. **Second and third backends.** `ReferenceBackend` (Eigen
    Levenberg-Marquardt on the neutral residuals, with rank diagnostics)
    first, as the oracle every other backend is tested against; then
    `PyBackend` (py-slvs or scipy as user-installed addons, matching the
    grant's "solvers installable as Python addons"); then Chrono for
    dynamics, out of process per `docs/Simulator.md` stage 2, via
    `RemoteBackend`.

### 6.4 Where Python must change

- `JointObject.py:933-936` -- **stop writing `Suppressed` to ask a
  question.** Add `isPartConnected(part, excludeJoint)` to the binding and
  call it. Needed by step 9 regardless: a `Suppressed` write changes the
  topology key and would reload sessions twice per joint pick.
- `JointObject.py:993-1036` (`preventParallel`) -- an Ondsel-specific start
  perturbation. Becomes conditional on a capability flag
  (`assembly.solverCapabilities()["needsNonParallelStart"]`) or moves into the
  Ondsel adapter as an initial-guess tweak. `preSolve` / `matchJCS`
  (`:912-981`) stay: a good start helps every backend.
- `JointObject.py:65-143` and the joint task's type list
  (`TaskAssemblyCreateJoint`, `:1563`) -- filter joint types by the backend's
  capabilities. The lists themselves stay in sync with
  `App/AssemblyUtils.h:49-64` as today.
- `JointObject.py:1725` -- the accept-time recompute stays until step 8 makes
  the warm result trustworthy; then accept commits the result already
  solved in `setJointConnectors`.
- `CommandCreateSimulation.py:926-935, 977, 1041, 1120, 1157` -- no API
  change: `generateSimulation`, `numberOfFrames`, `updateForFrame` keep their
  meaning, now backed by the stored trajectory. Playback may move view
  transforms instead of document placements (audit F10) as a separate step.
- `CommandCreateSimulation.py:541-560` -- the help text becomes the
  workbench's formula grammar (sec 4.6), identical in content today.
- `CommandExportASMT.py:78`, `App/AssemblyObject.pyi:147` -- keep
  `exportAsASMT` (upstream name) and add `exportSolverModel(path)` that uses
  the backend's native format, or the neutral model's own dump when there is
  none; the command is enabled when the backend has a native format.
- `App/AssemblyParams.py:42` area -- add `SolverBackend` (string, "Ondsel").
- Tests (`AssemblyTests/`) -- the identity gate, then the corpus run once per
  registered backend against the Reference oracle.


## 7. Open questions

1. Per-assembly backend property, or preference only as the PR does? The
   property is Assembly3's practice and makes a file reproducible; the
   preference is cheaper. Proposed: both, property empty by default.
2. Does the neutral layer own the formula evaluator (a small parser), or
   keep OndselSolver's `SymbolicParser` as a library for all backends?
3. The 17 Distance sub-types that fall to Planar(d): keep, map properly
   (several have exact primitives: PointCone and PointTorus are point-surface
   distances), or report unsupported?
4. Is the `ReferenceBackend` worth writing before the OndselSolver rework, as
   the oracle that lets the rework be tested against something other than
   itself? Proposed: yes; it is a few hundred lines on Eigen, which the
   module already includes (`App/CMakeLists.txt:14`).
5. Answered by the user 2026-10-22: the rework stays in our fork; nothing
   goes to the FreeCAD org. The original question: should the solver fork's
   changes (warm update, the reverse dependency
   removal) be offered to upstream OndselSolver, or kept fork-only? The same
   question as the audit's open decision 2.
