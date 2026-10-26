# Assembly: an abstract solver interface

Status (2026-10-23): steps 1-3 of sec 6.3 are done (`83ca3dcc4f`,
`65b0540fbe`, `09ce3133a3`, `18d3ecccc3`); sec 6.5 records what was built
and where it departs from secs 4.2/4.3. Written 2026-10-22 as a design
study on branch `AssemblyPort` (`fe871ff48c`). Extends `docs/AssemblyAudit.md` (sec 4: the solver is the
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
   |-- ChronoBackend   (optional; as built BUILD_ASSEMBLY_CHRONO, sec 6.15)
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

// A formula of time in a grammar the neutral layer owns and can evaluate
// (as built: sec 4.6.1 -- SymbolicParser's set, with tan, exp, ln and sqrt
// since sec 6.10; without acos, log). 'initialValue' is substituted before it
// gets here. (As built, sec 6.9 and 6.12: also the parsed tree, and a callable
// giving the value and the first two time derivatives at t.)
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
the same grammar. (As built, sec 6.9: the neutral layer has its own
evaluator and validates every formula with it. Since sec 6.12
OndselSolver no longer reads the text either: the Ondsel adapter hands
`runKINEMATIC` each driver as an `ExternalFunction` that calls the
evaluator for the value and the exact derivatives.)

#### 4.6.1 The formula grammar (the workbench's)

What a motion's `Formula` may say. Derived from what OndselSolver's
`SymbolicParser` (`OS/SymbolicParser.cpp`, as `ASMTRotationalMotion` /
`ASMTTranslationalMotion` call it) accepts, and from the help text of
`CommandCreateSimulation.py:541-578`; implemented by the neutral
`App/Solver/Formula.{h,cpp}` (sec 6.9). Where the two differ, this section
says so.

**Meaning.** A formula is a function of `time`, in seconds from the
simulation's start. An Angular motion's formula is the joint angle in
**radians**, a Linear motion's the slide in **mm** (OndselSolver's units
are all 1, `OS/ASMTAssembly.cpp:1125`). `initialValue` is not part of the
grammar: the builder replaces it with the joint's current value (radians
or mm, printed with 10 significant digits) before parsing. A blank formula
is the constant 0; the builder skips a motion whose formula is empty.

**Lexical.** Blanks (space, tab, newline) separate tokens and are
otherwise ignored, except between a sign and its number (below). Names are
a letter followed by letters, digits or `_`, and are **case-sensitive**
(`Sin`, `TIME` are errors). A number starts with a digit: digits, an
optional `.` and digits, an optional exponent `e`/`E` [`+`/`-`] digits,
then optionally a second, Fortran-style exponent `d` [`+`/`-`] digits that
multiplies by 10^n (`1.5d-1` is 0.15; SymbolicParser's `xDigit`). `.5` is
an error; write `0.5`. No unit suffixes (`deg`, `mm`).

**Syntax.**

    formula    := expression | (blank)
    expression := [ '+' | '-' ] term { ( '+' | '-' ) term }
    term       := factor { ( '*' | '/' ) factor }
    factor     := primary [ '^' factor ]
    primary    := number
                | sign number          -- no blank between them
                | 'pi' | 'time'
                | '(' expression ')'
                | function '(' expression { ',' expression } ')'

- `^` binds tightest and is **right associative**: `2^3^2` is 2^9; `*` and
  `/` are left associative and bind tighter than `+` and `-`.
- **A leading sign** (of the formula, of a parenthesis, of an argument)
  negates the whole first term: `-2^2` is -4, `-time*3` is -(time*3).
- **A sign glued to a number** is part of the number wherever an operand
  is expected, and so binds tighter than `^`: `2*-3`, `time/-2`, `2^-1`,
  `1 - -2`, `1--2` are accepted, and `3*-2^2` is 3*(-2)^2 = 12. A sign
  before anything else in operand position is an error: `2*-time`,
  `--time`, `1 - - 2` (write `2*(-time)` or `-2*time`).
- Constants: `pi`. There is no `e`.

**Functions** (arguments in radians where it applies):

| function | meaning |
|---|---|
| `abs(u)` | absolute value |
| `sin(u)`, `cos(u)` | |
| `asin(u)`, `arcsin(u)` | the same function |
| `arctan(u)` | one argument |
| `tan(u)` | |
| `exp(u)` | e^u |
| `ln(u)` | natural logarithm |
| `sqrt(u)` | square root |
| `rampstep(x, x0, y0, x1, y1)` | y0 for x < x0, y1 for x >= x1, the straight line between; x0, y0, x1, y1 constant, x0 != x1 (`OS/RampStepFunction.cpp`) |
| `piecewise(x, functions(f0, ..., fn), transitions(x1, ..., xn))` | the first fi with x < x(i+1), else fn (`OS/PiecewiseFunction.cpp`) |

`tan`, `exp`, `ln` and `sqrt` were added to both evaluators on
2026-10-23 (user's decision; sec 6.10). Not available: `log`, `acos`,
`atan`, `atan2`, `min`, `max`, `e` (write `exp(1)`). `spline(...)` and
`integral(...)` exist in SymbolicParser and are refused (`spline` needs its
own data form; `integral` throws "To be implemented" for most integrands).

**The help text's `exp`.** Its "Exponential: `C*exp(time/TIMEC)`" example
never ran before sec 6.10 (SymbolicParser had no `exp`; the builder then
refused it with "unknown function 'exp'"). It runs now, unchanged.

**Outside a function's domain** -- `ln` of a number <= 0, `sqrt` of a
negative, `asin` beyond +-1, `tan` at an odd multiple of pi/2, a division by
0 -- the formula is still valid: the grammar is about text, and a domain
depends on `time`. The evaluator gives what IEEE arithmetic gives (NaN, or
an infinity: `ln(0)` is -inf, the rate of `sqrt` at 0 is +inf), the way it
always did for `asin(2)` and `1/0`; it has no evaluation errors. The
position-solve kinematic run (sec 4.6) stops with `Failed` at the first
output time where a driver's value is not finite. OndselSolver's functions
give the same NaN alone, but its products drop a non-finite factor (sec
6.10), so `2*ln(time - 1)` is 2 there at t = 0.5 and NaN here; what its
`runKINEMATIC` does with a non-finite driver is not specified by the
grammar (sec 6.10 says what was seen).

**Stricter than SymbolicParser, deliberately.** SymbolicParser's
`expected()` reports nothing, so it stops at the first token it cannot use
and keeps what it read: `2t` ran as 2, `time)` and `time(2)` as `time`,
`2 3` as 2, `1.2.3*time` as 1.2. These are errors now ("expected an
operator or the end of the formula"). Also refused, where SymbolicParser
asserted, read garbage or crashed: a wrong argument count (`sin(time, 2)`),
`rampstep` with a time-dependent or equal x0/x1, `piecewise` without one
function more than transitions, `functions`/`transitions` anywhere but
inside `piecewise`, `-.5` (SymbolicParser read a stale number), an
incomplete exponent (`1e`, `2d`), and the ASMT file's `<n>` line tags.

**Values.** The evaluator computes with plain IEEE arithmetic on the tree
as parsed; OndselSolver multiplies by a reciprocal for `/` and simplifies
the expression first, and its products skip non-finite factors. The two
agree to 1e-12 relative on every formula of the test corpus (sec 6.9), not
bit for bit. Derivatives with respect to `time` are exact per node; a
piecewise formula's derivative is the derivative of the piece in effect
(jumps are not seen), and `abs`'s is sign(u) u', 0 at the kink. Since sec
6.12 OndselSolver's own derivatives agree with these to 1e-10 relative on
the whole corpus, and a simulation does not use them anyway: the Ondsel
adapter passes the evaluator's.

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
ordering. Upstream has 20 cases and 17 fall to the default Planar(d); since
decision 7 (audit sec 8.1, built in sec 6.8) 23 have a case and the other
14 were refused for new joints. Since the role mappings (sec 6.11) 36 have
a case; only `Other` between two elements is refused whatever they are,
and four Curve sub-types are refused on a curve that is not a circle, two
on one that is not planar. A refused joint already in a document solves as
before, with a warning. Every element is classified after canonical
recognition (sec 6.11): a B-spline that is a circle or a cylinder counts
as one.

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
| LineCylinder (decision 7) | LineLineDistance(d + r1); I the cylinder's axis | `ASMTRevCylJoint` |
| LineSphere (decision 7) | PointLineDistance(d + r1); swapped: I the line, J the sphere's centre | `ASMTCylSphJoint` |
| CurvePlane (decision 7) | Planar(d) on the curve's plane; refused on a non-planar curve | `ASMTPlanarJoint` |
| PlaneCone (sec 6.11) | LineInPlane(d): the cone's axis | `ASMTLineInPlaneJoint` |
| CylinderCone (sec 6.11) | LineLineDistance(d + r1) | `ASMTRevCylJoint` |
| ConeCone, ConeTorus, LineCone, LineTorus (sec 6.11) | LineLineDistance(d): the axes | `ASMTRevCylJoint` |
| ConeSphere (sec 6.11) | PointLineDistance(d + r2) | `ASMTCylSphJoint` |
| PointCone, PointTorus (sec 6.11) | PointLineDistance(d): the point to the axis | `ASMTCylSphJoint` |
| CurveCylinder (sec 6.11) | LineLineDistance(d + r1 + r2), r2 the circle's | `ASMTRevCylJoint` |
| CurveSphere (sec 6.11) | PointLineDistance(d + r1 + r2); swapped: I the circle's axis | `ASMTCylSphJoint` |
| CurveCone, CurveTorus (sec 6.11) | LineLineDistance(d + r2), r2 the circle's | `ASMTRevCylJoint` |
| Curve{Cylinder,Sphere,Cone,Torus} on a curve that is not a circle; CurvePlane, PointCurve on one that is not planar; Other between two elements | refused for new joints, the message names the element and what it is; in a document as before (Planar(d), PointInPlane(d)), with a warning | `ASMTPlanarJoint` / `ASMTPointInPlaneJoint` |
| Other with a whole object on a side (a datum plane, an LCS, a body) | Planar(d) between the objects' frames | `ASMTPlanarJoint` |

So the whole Distance family needs only seven neutral kinds: Spherical,
Planar, PointInPlane, LineInPlane, PointPointDistance, PointLineDistance,
LineLineDistance. The default row is questionable (Planar also locks the
relative orientation, e.g. for a point on a cone); decision 7 refused it
for new joints and kept it for old ones (sec 6.8), and the role mappings
(sec 6.11) gave all but `Other` a mapping of their own.

The 9 OndselSolver joint classes the workbench does not use
(`ASMTUniversalJoint`, `ASMTConstantVelocityJoint`, `ASMTPointInLineJoint`,
`ASMTRevRevJoint`, `ASMTNoRotationJoint`, `ASMTAtPointJoint`,
`ASMTInLineJoint`, `ASMTInPlaneJoint`, `ASMTCompoundJoint`) are where the
reserved kinds Universal and PointOnLine would map first -- the audit's
missing "universal" joint already exists in the solver.

### 5.3 What each backend can express

`N` native, `E` expressible by composing primitives in the adapter, `S`
neutral-layer stepping, `L` a link of the adapter's own holding the
neutral equations (Chrono's `NeutralLink`, sec 6.17), `-` not available.
Chrono as built (sec 6.15, 6.17; the PR's choices, where they differed, in
parentheses); SolveSpace per py-slvs / libslvs as used by Assembly3.

| neutral | Ondsel | SolveSpace | Chrono | scipy / Reference |
|---|---|---|---|---|
| Fixed | N | E coincident + same orientation | N `ChLinkMateFix`, on the branch the start is nearest | N |
| Revolute | N | E coincident + parallel normals | N `ChLinkMateRevolute` | N |
| Cylindrical | N | E point on line + parallel | N `ChLinkMateCylindrical` | N |
| Prismatic | N | E point on line + same orientation | N `ChLinkMatePrismatic` | N |
| Spherical | N | N coincident | N `ChLinkMateSpherical` | N |
| Planar(off) | N | E point-plane distance + parallel | N `ChLinkMatePlanar` with its distance (PR dropped the offset) | N |
| ParallelZ | N | N parallel | N `ChLinkMateParallel` | N |
| PerpendicularZ | N | N perpendicular | N `ChLinkMateOrthogonal` (`ChLinkLockPerpend` holds two rotations, not one) | N |
| AngleZ | N | N angle | L; a right angle N `ChLinkMateOrthogonal` | N |
| PointInPlane(off) | N | N point-plane distance | N `ChLinkMateDistanceZ` | N |
| LineInPlane(off) | N | E point-plane distance + perpendicular | E `ChLinkMateDistanceZ` + `ChLinkMateOrthogonal` (PR mapped to Planar: wrong) | N |
| PointOnLine | N (unused) | N | N `ChLinkMateGeneric(x, y)` | N |
| PointPointDistance | N | N | N `ChLinkDistance` | N |
| PointLineDistance | N | N point-line distance | L (PR skipped) | N |
| LineLineDistance | N | E two point-line distances + parallel | L (PR skipped) | N |
| Gear / Belt | N | - | N `ChLinkLockGear`, phase enforced; L between coaxial or non-parallel shafts / L (`ChLinkLockPulley` is velocity-level only) | N |
| CoaxialGear | N (upstream) | - | L (Chrono's gear has no carrier frame) | N |
| RackPinion | N | - | L (`ChLinkMateRackPinion` is velocity-level only) | N |
| Screw | N | - | L (`ChLinkLockScrew` is a whole screw pair) | N |
| PointOnCurve (sec 6.18) | N `PointOnCurveJoint`, the curve evaluated by the host | not examined | L (`ChLinkLockPointSpline` samples and differences, sec 6.18) | N |
| limits | N (inequality, applied while dragging) | - | active set of Chrono links (PR: after-step clamp) | bounds / penalty (Reference: active set, sec 6.13) |
| drivers | N symbolic | S | N motors with a `ChFunction` on the neutral evaluator (PR: not done) | S |
| drag | N | initial guess only (`addWhereDragged` unreliable) | weighted target, as the Reference (PR: mouse body, unstable) | initial guess (Reference: weighted target, sec 6.13) |
| DOF / redundancy | count; redundancy by marking; no conflicts | `Dof`, `Failed` list | rank of Chrono's Jacobian (PR reports false) | rank of the neutral Jacobian |
| native dump | ASMT | slvs dump | Chrono's JSON archive | the neutral model |

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
   **Done 2026-10-23** (`83ca3dcc4f`), with the deviations of sec 6.5.
2. **Ondsel adapter beside the old path.** `App/Solver/OndselBackend.cpp`
   turns a `Model` into an `ASMTAssembly` with today's names and order:
   `"OndselAssembly"`, `"marker-<name>"`, `"FixingMarker"`, joint and
   marker names = the joint's full name, `"-LimitLenMin"` etc., limit values
   printed as today (`std::to_string(min)` and `"<deg>*pi/180.0"` -- the
   neutral model carries numbers, the adapter recreates the exact strings),
   motion names. A test-only `ModelFromAsmt` is not needed: the gate compares
   texts.
   **Done 2026-10-23** (`65b0540fbe`): `App/Solver/OndselBackend.{h,cpp}`,
   registered as "Ondsel" at module init; tested against AssemblyObject's
   own ASMT statements in `tests/src/Mod/Assembly/App/OndselAdapter.cpp`.
3. **Builder switch.** `makeMbdAssembly`, `getMbDData`, `fixGroundedPart`,
   `handleOneSideOfJoint`, `getRackPinionMarkers`, `makeMbdJointOfType`,
   `makeMbdJointDistance`, `makeMbdJoint`, `create_mbdSimulationParameters`
   emit into a `Model` (rule 2); `solve`, `exportAsASMT` and
   `generateSimulation` hand it to the adapter. Document writes during the
   build (`swapJCS`, limit swaps) are kept for now.
   **Done 2026-10-23** (`09ce3133a3`, gate in-process; `18d3ecccc3`, the old
   builder removed and the gate kept on recorded references, sec 6.5).
4. **Drag and status through the session.** `preDrag`/`doDragStep`/`postDrag`
   call `beginDrag`/`dragStep`/`endDrag`; `doDragStep` still reads the target
   from the document `Placement` (the GUI is unchanged). `updateSolveStatus`
   becomes `Session::analyse` mapped to names; the Ondsel adapter keeps the
   `constraintSpec` scan and the count. Drag masses go into
   `Body::dragWeight` (still unused by the solver, as today).
   **Done 2026-10-23** (`cae7e1ade6`), with the deviations of sec 6.6.
5. **Header clean.** Remove `OndselSolver/enum.h`, the `MbD` forward
   declarations and every ASMT member from `App/AssemblyObject.h`; drop the
   vestigial gravity include; Gui no longer needs the solver include path.
   **Done 2026-10-23** (`701f889a70`).
6. **Trajectory in the neutral layer.** The Ondsel `simulate` copies every
   frame out after `runKINEMATIC` (loop over `updateForFrame` once);
   `AssemblyObject::updateForFrame` plays from the stored `Trajectory`.
   Gate: per-frame placements equal. Remove `freecadAssemblyObject` from
   `OS/ExternalSystem.h` (solver fork) and its two setters.
   **Done 2026-10-23** (`7b54d90575`; the solver fork's `881d949`, local
   only). The backend selection of sec 4.8 came between steps 5 and 6
   (`6a4b278213`).

Results change by design (tolerance gates: placements within 1e-9 mm and
1e-9 rad of the identity-path result on the corpus; DOF and redundancy
lists equal):

7. **The builder stops writing the document** (frames swapped in the model,
   limits ordered in the model). Gate: same placements; the document's
   Reference1/Reference2 order no longer changes on solve.
   **Done 2026-10-23** (`5a73e4dbfc`, gate `33b30436e0`), sec 6.7: the
   model is the one the document swap gave, so the results are identical
   after all.
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
    **The first done 2026-10-23** (`47e02a7cc5`, `6e635ce875`,
    `20f8f1ef8e`): the `ReferenceBackend`, sec 6.13, and the solver gate
    run once per registered backend. The user's order: the reference
    solver, then Chrono.
    **The second done 2026-10-23** (`3fe6057c93`, `dda9ce366d`,
    `3e4d4dbdf6`): the `ChronoBackend` in process (sec 6.15), kinematics
    only, built from the submodule with `BUILD_ASSEMBLY_CHRONO`; out of
    process and dynamics remain. Next: `PyBackend`.

### 6.5 As built: steps 1-3 (2026-10-23)

**The interface as built** (`App/Solver/`):

- `Model.h`: `Body`, `Frame`, `Joint` (the 19 `JointKind`s of sec 4.2),
  `Ground`, `Limit`, `Driver`, `Model`; every record's `Id` is its index in
  its vector, and `Model::add*()` assigns it.
- `Backend.h`: `Status`, `Diagnostics` (a `Field` bit per `known` entry),
  `BodyPose`, `Result`, `Capabilities`, `SolveOptions`, `DragOptions`,
  `SimulationSpec`, `Trajectory`, `TrajectorySink`, `Session`, `Backend`.
- `Registry.{h,cpp}`: `add` (replaces a same-named backend), `remove`,
  `find`, `names`, `requestedName`, `select`.
- `OndselBackend.{h,cpp}`: `Ondsel::toAsmt(model, simulation, debug)` (the
  adapter; it also returns each body's `ASMTPart`), `Ondsel::partPlacement`,
  `Ondsel::limitValueString`; `OndselBackend` ("Ondsel", registered in
  `AppAssembly.cpp`); `OndselSession` (load, solve, simulate, exportNative;
  drag and analyse answer `Unsupported` until step 4).
- `AssemblyObject` keeps the model being built (`solverModel`, with
  `simulationSpec`) and, until step 4, the ASMT model (`mbdAssembly`) that
  status, drag and playback read. `makeMbdModel()` hands the model to the
  adapter at the four build sites (solve, generateSimulation, exportAsASMT,
  the island drag). The builder functions kept their names, parameters and
  statement order; their return types are model handles now (`Solver::Id`
  for a part or a marker, `std::shared_ptr<Solver::Joint>` for a joint not
  yet in the model). Python and Gui see no change.

**Deviations from secs 4.2/4.3**, each where the code showed the design
short:

1. **Grounding is a `Ground` record, not only a flag.** One body can bundle
   several grounded objects (a rigid group), and each gets its own fixed
   joint, assembly marker and `FixingMarker` today. `Ground` names the
   grounded object, a frame on the ground (`Frame::body == NoId`, at the
   object's pose) and a frame on the body; `Body::grounded` is set as well,
   for backends that make the pose constant instead.
2. **Frames carry a `label`** (an index into `Model::sources`). ASMT
   addresses markers by name, and the names cannot be derived: a gear's
   carrier marker is named after the gear joint but placed by the carrier
   joint; grounding uses "marker-<name>" and "FixingMarker". Other backends
   may use the label for diagnostics or ignore it.
3. **Limits and drivers name their two frames, not a joint.** The builder
   makes them before the joint enters the model (`makeMbdJoint` returns the
   joint, `jointParts` adds it), so no joint id exists yet; and the frames
   are what a limit or a driver acts on. The joint is the one with the same
   `source`. One `Limit` record per bound, as ASMT has one limit per bound;
   the adapter writes min before max either way.
4. **`TopologyKey`, `ModelDelta` and `diff()` are not declared yet**; they
   come with step 8. Until then `Session::update` takes a whole `Model` and
   answers `NeedsReload`.
5. **`Registry::select` takes an `App::DocumentObject*`** (or a name), not
   `const AssemblyObject&`: it only reads the `SolverBackend` property, and
   the registry needs no AssemblyObject header. The per-assembly
   `SolverBackend` property itself is not added yet (it is read when
   present), nor the `SolverBackend` entry in `AssemblyParams.py`, nor
   `AssemblyApp.getAvailableSolvers()`. (All three added in `6a4b278213`,
   sec 6.6.)
6. **Angles in limits are radians**, as designed; the adapter prints
   `std::to_string(toDegrees(rad)) + "*pi/180.0"`. The round trip to the six
   printed decimals was checked with Base::toRadians / toDegrees's formulas
   over -3600..3600 deg in steps of 7e-4 deg (10 million values), and the
   gate's limit cases pass.
7. **The empty ASMT model at the start of a build stays**:
   `makeMbdAssembly()` still resets `mbdAssembly` to an empty one, because a
   build that stops early (no grounded part: -6) always left it so, and
   `updateSolveStatus` / `numberOfFrames` act on that. (Step 4 replaced it:
   a build starts with no session, which reads the same way.)
8. **A dump hook**: the preference `Mod/Assembly/SolverDumpFile` (a path,
   unset by default) writes every ASMT model handed to the solver, so the
   gate sees the input of a solve, a drag and a simulation, not only the
   export. It moves into the session (`exportNative` after `load`) at step 4.
   (Done: `OndselSession` writes it in `load` and `simulate`.)

**The identity gate** (`AssemblyTests/TestSolverGate.py`, also run by
`TestAssemblyWorkbench`). Step 3 kept the old builder beside the new one
(`App/Solver/LegacyBuilder.cpp`, preference `SolverLegacyBuilder`) and
compared them in one process: the export byte for byte, the dumped input of
every solve, drag and simulation byte for byte, and the placements after
the solve, each drag step and each simulation frame, the return codes and
the statuses exactly. A rigid group's placement cache makes a run depend on
the one before, so each phase ran the old builder until it repeated itself,
then the new one, then the old one again, and the rigid-group case clears
the cache between runs (a solve with the group suppressed). Then the old
builder was removed and the gate kept by **recording one reference per
corpus case** (`AssemblyTests/SolverGateReference.json`, 46 cases): the
SHA-256 of each ASMT text with the grounded entries sorted (their order
follows an unordered_set of pointers), the placements within 1e-9, and the
statuses exactly. The references were recorded three times, in three
processes, twice through the new builder and once through the old one; the
three files are identical. `ASM_SOLVER_GATE_RECORD=1` re-records after an
intended change; another platform skips. A 1e-7 change to a value the
adapter writes fails both forms of the gate.

Corpus: every joint type (Fixed, Revolute, Cylindrical, Slider, Ball,
Parallel, Perpendicular, Angle 30 and Angle 0; Gears and Belt with drags; a
coaxial gear with its carrier marker; Screw and RackPinion picked in the
order the builder swaps); the 20 mapped Distance sub-types, PointPoint at
d = 0 and a swapped SpherePlane pick; Slider, Revolute and Cylindrical
limits, reversed ones included, with drags into them; one simulation with
an Angular, a Linear and a Cylindrical driven both ways (with
`initialValue`); the bench's chain and star at N = 10 and 50, each with a
drag of a Revolute-joined link; a rigid group with a drag; a drag that
bundles Fixed joints. Not in it: the island drag (`prepareMbdForIslandDrag`),
which no current test builds.

**The Distance decision** (audit sec 8.1, decision 7) was not implemented
at this step (it is now: sec 6.8):
phase 0 tested the 20 mapped sub-types (`TestJoints.py`) and nothing more;
LineCylinder, LineSphere and CurvePlane still fall to Planar(d), and the
other 14 are neither refused nor warned about. Mapping them changes
results, so it belongs with the result-changing steps (7 onwards) or in a
step of its own after step 6, gated by tolerance tests.

**What step 4 needs:**

- `OndselSession` drag: `beginDrag(bodies)` records the drag bodies (the
  builder already bundles when `bundleFixed` is set, so `DragOptions` only
  passes it on) and applies the drag weights; `dragStep(targets)` does what
  `doDragStep` does today (`updateMbDFromPosition3D`,
  `updateMbDFromRotationMatrix`, `runDragStep`, `restorePosRot` on
  failure); `endDrag` runs `runPostDrag`.
- **Drag weights** go into `Body::dragWeight` and are applied to the MbD
  bodies after the pre-drag solve, never written into the ASMT model, so the
  gate's dumps stay identical (`applyDragMasses` today).
- **All poses back, not only the moved ones**: `setNewPlacements` and
  `validateNewPlacements` read every part, while `Result::moved` carries
  only those beyond tolerance. A `Session::poses()`, or a flag asking for
  all, is a deviation to make at step 4.
- `updateSolveStatus` -> `Session::analyse()`: the constraint scan, the
  equation count for DoF and the residual test of `isConflicting` move into
  the adapter; joint ids map back to names through `Joint::source`.
  `Diagnostics` needs today's DoF convention for bundled bodies (6 x (1 +
  solver bodies) minus the equations kept).
- Playback (`numberOfFrames`, `updateForFrame`) stays on the ASMT model
  until step 6, so the session must still expose it (`OndselSession::asmt()`
  exists for that).
- The session comes from `Registry::select(assembly)`; adding the
  `SolverBackend` property and preference makes the choice visible.
- The gate needs no change: its recorded drag steps and statuses are step
  4's acceptance test; only the dump hook moves into the session.

### 6.6 As built: steps 4-6 and the backend selection (2026-10-23)

`AssemblyObject` holds no solver type any more. It keeps the model it built
(`solverModel`), the session that model was handed to (`session`, from
`Registry::select(this)` in `makeMbdModel()`; a build starts without one),
the number of bodies the session was loaded with, and the last simulation's
`Trajectory` with the part-to-body map of that build. `AssemblyObject.h`
includes `Solver/Model.h` and nothing of OndselSolver; `AssemblyObject.cpp`
includes `Solver/Backend.h` and `Solver/Registry.h`; only
`App/Solver/OndselBackend.cpp` includes OndselSolver.

**Step 4** (`cae7e1ade6`):

- `preDrag` solves as before, then gives the dragged parts' bodies their
  drag masses as `Body::dragWeight`, hands the model to `Session::update`
  and calls `beginDrag(bodies)`. `OndselSession::beginDrag` puts the weights
  on the MbD bodies built by the pre-drag solve (phase 0's
  `applyDragMasses`, moved); the ASMT model keeps mass 1, so the dumps are
  unchanged.
- `doDragStep` turns each dragged part's `Placement` into its body's target
  (the bundle offset taken out, one target per body) and calls `dragStep`.
  The session sets the targets, runs `runDragStep`, and on a step that does
  not converge or throws puts the bodies back (`restorePosRot`) and answers
  `NotConverged` / `Failed` with the message the warning prints. The
  failed-step warning, the counts and `getDragStatus` are unchanged.
- `postDrag` calls `endDrag` (`runPostDrag`).
- `setNewPlacements` and `validateNewPlacements` read `Session::poses()`.
- `updateSolveStatus` maps `Session::analyse()` back to names. The scan, the
  equation count and the conflict re-check of phase 0 (`isConflicting`)
  moved into `OndselSession::analyse`, which matches the system's MbD joints
  to model joints through the ASMT joints the adapter made (their
  `mbdObject`), and reports ids; `AssemblyObject::jointNameOf` maps an id
  through `Joint::source` to the document joint, by the same name lookup as
  before.

Deviations:

1. **`Session::poses()`** (pure virtual): every body's pose, entry i for body
   i. `OndselSession` reads them back once after each load, solve, drag step
   and end of drag, and `Result::moved` compares against that cache.
2. **`Session::update(model)` takes drag weights**: a model equal to the
   loaded one apart from `Body::dragWeight` (`sameExceptDragWeights`, exact,
   in the new `Solver/Model.cpp`) is `Ok`; anything else is `NeedsReload`,
   as before. That is how the weights reach a session without a reload or a
   weight argument on `beginDrag`.
3. **`Diagnostics::dof` counts the session's bodies**: 6 x (1 + bodies) minus
   the equations kept. `AssemblyObject` adds 6 for each part left out of the
   model (not connected to ground), which gives today's number.
4. **The malformed check stays in the builder.** Those joints are found from
   the document and never reach the model, so a session cannot see them;
   `Diagnostics::malformed` is merged when a backend reports it. Ondsel
   reports none.
5. **A status query with nothing solved returns after its on-demand solve.**
   Before, the outer `updateSolveStatus` went on to scan the system the
   inner solve had already scanned, subtracting every equation from the DoF
   a second time. No test saw it: the gate and the suite read the status of
   an explicit solve.
6. **`endDrag` reports instead of throwing**: an exception in the post-drag
   solve is a `Failed` result and an error in the report view, not a C++
   exception out of `postDrag`.
7. **The dump in `generateSimulation` is written twice** (`load`, then
   `simulate` with the simulation parameters); the second is the one the
   gate reads, and its text is what `makeMbdModel` wrote before.
8. **No `freecadAssemblyObject` setter remains**: the constructor,
   `makeMbdAssembly` and `makeMbdModel` set it on the ASMT model; with no
   ASMT model in `AssemblyObject` they are gone.

**Step 5** (`701f889a70`): `OndselSolver/enum.h`, the `MbD` forward
declarations and `getMbdPlacement(std::shared_ptr<MbD::ASMTPart>)` left the
header, and 35 of the 36 OndselSolver includes (the vestigial
`ASMTConstantGravity.h` among them) and `using namespace MbD` left the
source. The solver include path was not in `Gui/CMakeLists.txt` but in
`src/Mod/Assembly/CMakeLists.txt`, at directory scope, which Gui inherited:
it is now a PRIVATE include directory of the `Assembly` target. After the
reconfigure none of AssemblyGui's 14 compile commands carries it, all 20 of
Assembly's do, and the tree builds.

**Backend selection** (`6a4b278213`, sec 4.8, sec 7 q1): the property
`AssemblyObject::SolverBackend` (`App::PropertyString`, group "Solver",
empty by default); a file without it restores empty, and a file that stored
an enumeration restores its name (`handleChangedPropertyType`). The
preference `Mod/Assembly/SolverBackend` (default "Ondsel") is in
`App/AssemblyParams.py`, with `AssemblyParams.h/.cpp` regenerated by cog.
`AssemblyApp.getAvailableSolvers()` lists the registered names. An unknown
name warns once and solves with the default (`TestCore.TestSolverBackend`).

**Step 6** (`7b54d90575`): `generateSimulation` records what
`OndselSession::simulate` streams into a `Trajectory` and keeps it with the
part map of that build; `updateForFrame` / `numberOfFrames` play from it.
`OndselSession::asmt()` is gone. Deviation: **the trajectory is kept until
the next simulation.** The ASMT model played whatever the last solve had
left in it: every static solve and drag step appended a "frame", so a solve
between two playbacks used to change the frame count and what frame 0
showed. Now a solve, a drag or an export does not touch playback
(`test_playback_survives_a_solve`). Only poses are stored; the velocities
and accelerations OndselSolver also computes are not, as nothing read them.

The solver fork: `ExternalSystem::freecadAssemblyObject` and the three
`else if` branches that tested it (bodies commented out) are gone
(`881d949` on the submodule's local `LinkVibe`, not pushed). The member was
null in every `System`'s own `ExternalSystem` (value-initialised by
`make_shared`), so no path changes.

**Gate, per step.** Steps 4 and 5 passed the recorded references unchanged
(46 cases). Step 6 added three simulation cases -- the simulation played in
reverse order, a gear driven through its revolute, a driven part in a rigid
group (the other member placed by its offset) -- 49 cases in all. Before the
old playback went, a build that kept it behind a temporary preference
played every frame of the four simulation cases both ways in one process,
each part set to a sentinel pose before each play: 48 frames, equal bit for
bit. The new cases were then recorded three times -- in that process, in a
second process through the old path, in a third through the new one into
`SolverGateReference.json` -- and the three records are identical; the
file gained only those three cases.

**Bench** (`scripts/assembly-bench/bench.py`, odd N so the dragged link is
Revolute-joined and the drag steps solve; seconds; before = `e9fd614968`,
after = step 6):

| case | solve_1 | preDrag | doDragStep (median of 10) | postDrag |
|---|---|---|---|---|
| star 49 | 0.0554 / 0.0574 | 0.0417 / 0.0455 | 0.0126 / 0.0132 | 0.0143 / 0.0144 |
| chain 49 | 0.596 / 0.605 | 0.0687 / 0.0711 | 0.0267 / 0.0278 | 0.0339 / 0.0349 |
| star 199 | 0.429 / 0.436 | 0.277 / 0.274 | 0.0794 / 0.0782 | 0.0956 / 0.0963 |
| chain 99 | 9.01 / 9.28 | 1.51 / 1.48 | 0.970 / 0.986 | 0.913 / 0.829 |

Within run-to-run noise (the "before" run's own three solves of chain 99
spread from 3.4 to 9.0 s); the largest model, star 199, is no slower.

**What step 7 needs** (the builder stops writing the document):

- `getDistanceType` and `slidingPartIndex` call `swapJCS`, and
  `makeMbdJoint` swaps reversed limits, on the document. The builder swaps
  its own frames I and J (and orders min / max) in the model instead.
- Every reader of the swapped order has to be found first: `JointObject.py`
  (offsets, the joint task's display, `preSolve` / `matchJCS`), the view
  provider, and `TestJoints.py`'s expectations of the swapped references.
- The gate changes form: placements within tolerance of the recorded
  references, the ASMT text compared only where the model swaps exactly as
  the document did; new cases pick Screw, RackPinion, the swapped Distance
  sub-types and reversed limits in both orders and assert that the document
  is unchanged after a solve.
- Not needed by step 7, still open: the Distance mapping of decision 7 (done
  since, sec 6.8), the neutral formula evaluator (sec 7 q2), `solverCapabilities()` in the
  binding, and the island drag (`prepareMbdForIslandDrag`), which no test
  builds.

### 6.7 As built: step 7 (2026-10-23)

**The builder** (`5a73e4dbfc`). `AssemblyObject` gained a private
`swapJCS(const App::DocumentObject*)`, which shadows the free `::swapJCS`
inside the builder functions and flips `jcsSwapped` (reset by
`makeMbdJoint` before each joint), and `jcsRef(side)` / `jcsPlc(side)`,
which name the document property of frame I (side 1) or J (side 2). So the
two upstream statements `swapJCS(joint);` in `makeMbdJointOfType` (Screw)
and `getRackPinionMarkers` are untouched; what changed is the property
names read after them (`"Reference1"` -> `jcsRef(1)` and so on, in
`makeMbdJoint`, `makeMbdJointDistance` and `getRackPinionMarkers`).
`getDistanceType(joint, bool* swapped = nullptr)` never writes the
document: its ten `swapJCS(joint)` calls go to a local lambda that flips
`*swapped`. The reversed-limit blocks became `std::swap` of the two local
values (both bounds are enabled there, so the enable swap was a no-op).
`::swapJCS` itself stays, exported and now unused.

Frame I is still handled first, so bodies and markers enter the model in
the order the swapped document gave them: **the solver input is byte for
byte what it was**, and the placements bit for bit.

**Readers of the swapped order**, each checked:

- `JointObject.py`: `preSolve` / `matchJCS`, `preventParallel`,
  `areJcsSameDir` read the two sides symmetrically (pick the unconnected
  one); no change. `ensureUnconnectedIsSecondRef` swaps at creation, in
  Python, with the offsets and detach flags -- a user action, not a solve;
  left as is. The task panel's `refs` and its offset widgets now stay in
  step with the document instead of disagreeing after the first solve.
- **Offsets (user-visible).** The document swap exchanged Reference1/2 and
  Placement1/2 but not Offset1/Offset2, and setting Reference1 recomputes
  the joint (`onChanged` -> `updateJCSPlacements`), so after the first
  solve each offset sat on the other side. Now an offset stays on its
  reference. Only a joint the builder swaps (Screw, RackPinion, the
  swapped Distance picks) with a non-identity offset solves differently;
  the joint task's simple offset (Offset2) now moves the second pick, as
  it does for every other joint.
- **Limits (user-visible).** A reversed min/max pair stays as typed; the
  spin boxes show what the user entered.
- `UtilsAssembly.getJointDistance` / `getJointXYAngle` feed the limit
  spin boxes of Slider / Revolute / Cylindrical, which are never swapped.
- `ViewProviderAssembly::findDragMode` called `getDistanceType` and so
  wrote the document at the start of a drag (after it had read the side's
  placement); it now only classifies.
- C++ readers outside the builder (connectivity, downstream parts,
  isolation, `slidingPartIndex`) are symmetric in the two sides.
- Tests: none asserted a swapped reference. `TestDistanceJoint`'s cases
  all pick the side the sub-type wants first; its docstring now says so.
  `TestCore.test_rack_pinion_with_slider_offset` picks the rack first.

**Gate** (`33b30436e0`). Every case now also snapshots each joint's
references, placements, offsets, detach flags and limits before the
export, simulation, solve and drag, and asserts them unchanged after
(the old builder fails this on the Screw, RackPinion and reversed-limit
cases). `gate(key=...)` checks a case against another case's record, the
document's name in the ASMT text read as that record's. New,
`TestSolverGateOrder` (5 tests): Screw slider-first and RackPinion
rack-first against the recorded swapped picks; the Cylindrical limits in
order against the reversed record; the 13 Distance sub-types the builder
swaps (LineCircle, PlaneCylinder, PlaneSphere, PlaneTorus, CylinderSphere,
CylinderTorus, TorusSphere, PointPlane, PointCylinder, PointSphere,
LinePlane, PointLine, PointCurve) built in both orders in one process and
compared; a PointPlane offset on the plane side in both orders. The
Distance cases set the references directly: `setJointConnectors` puts the
connected side first, so the recorded "SpherePlane (swapped)" case never
reached the builder's swap -- its record equals PlaneSphere's.

The comparison is in the tolerance form of this step (placements within
1e-9, statuses -- DoF, redundant, conflicting -- equal), but since the
model is unchanged the ASMT text is still compared byte for byte. Result:
no recorded case moved; across the 25 gate tests 1346 placement lists were
compared, **max deviation 0.0** (all bit-equal). No reference was
re-recorded; the new cases check against existing records or against
each other, so `SolverGateReference.json` is unchanged.

Suite 134 run, 1 skipped (129 + the 5 new); ctest 911/911; Assembly gtest
11/11.

**What step 8 needs:**

- The swap is decided per build from the document (`slidingPartIndex`
  scans every Slider; `getDistanceType` reads the shapes). A warm session
  needs it to be part of the topology key: a change that flips it (a
  Slider added, a reference re-picked) is a reload, not a value delta.
- `jcsSwapped` is builder state on `AssemblyObject`; if joints are ever
  built out of order or in parallel it has to move into a per-joint build
  context.
- Not done here, still open: the Distance mapping of decision 7 (done
  since, sec 6.8), the formula evaluator, the island drag in the corpus,
  and the creation-time swap in `ensureUnconnectedIsSecondRef` (it writes
  the document from the joint task, by design for now).

### 6.8 As built: the Distance sub-types, decision 7 (2026-10-23)

Audit sec 8.1 decision 7, in `e320accd94` (builder, warning, gate) and
`85a4379a0b` (the joint task). Of the 17 sub-types that fell to the
default Planar(d), three are mapped and 14 refused; the table in sec 5.2
is updated.

**The three mappings**, after `getDistanceType`'s ordering (the face is
side 1, the edge side 2, for every edge-face pair) and with the JCS
`UtilsAssembly.findPlacement` gives a Distance joint (`ignoreVertex`):

- **LineCylinder: LineLineDistance(d + r1).** Frame I is the cylinder: its
  JCS is on the axis, z along it. Frame J is the line: origin at its
  middle, z along it. `ASMTRevCylJoint` holds the two z axes parallel and
  J's axis d + r from I's, so the line lies d off the surface, parallel to
  the axis -- the same reading as CylinderCylinder with r2 = 0, and as
  LineLine, which also makes the lines parallel. r1 is the cylinder's
  radius (side 1), not r2 as for LineCircle, whose circle is side 2.
- **LineSphere: PointLineDistance(d + r1), sides swapped.** The sphere's
  JCS is at its centre, the line's z along the line. `ASMTCylSphJoint`
  measures J's origin from I's z axis, so the line has to be frame I: the
  case reads the radius from side 1, then calls the builder's `swapJCS`
  (the one Screw uses; it flips `jcsSwapped`, the document is not
  written). The centre ends d + r from the line, i.e. the line passes d
  off the sphere. The model is therefore the swapped one: the body of the
  line's part comes first.
- **CurvePlane: Planar(d)**, the PlanePlane mapping on the curve's plane.
  Frame I is the plane (z its normal); frame J is the curve's JCS, which
  for a circle or arc is its centre and normal (unchanged), so the case is
  byte for byte what the default gave for circles. For another planar curve
  (an ellipse, a spline) `findPlacement` used to leave the JCS at the
  identity, at the origin; it now takes the curve's plane
  (`edge.findPlane()`) through its centre of mass. That also fixes
  PointCurve on such curves, which reads the same JCS. A non-planar curve
  (a helix) has no plane and keeps the old frame; it is not refused (the
  decision does not list it) -- a gap to close if one shows up.

**The 14 refused** (PlaneCone, CylinderCone, ConeCone, ConeTorus,
ConeSphere, PointCone, PointTorus, LineCone, LineTorus, CurveCylinder,
CurveSphere, CurveCone, CurveTorus, Other) are listed once, in
`App/AssemblyUtils.cpp` `refusedDistanceTypes()`; `isDistanceTypeRefused`
reads it, and Python reads it through `AssemblyApp`
(`getRefusedDistanceTypes()`, `getDistanceType(joint)` by enumerator name,
`isDistanceJointRefused(joint)`). One deviation: **Other is refused only
when both sides name a vertex, an edge or a face.** A whole object on a
side -- a datum plane, an LCS, a body -- also classifies as Other, and
there the planar distance between the two objects' frames is exactly what
a user picking two datum planes means; refusing it would have taken away
a working case the decision did not aim at. `isDistanceJointRefused`
applies that rule; the builder and the task both call it.

- **New joints** (`JointObject.py`, `TaskAssemblyCreateJoint`): when two
  picks would make a refused Distance joint, the pick is not taken. The
  references are set on the joint to classify them (as
  `setJointConnectors` would, without its solve); a refused second pick is
  removed from the selection and the joint goes back to one reference; a
  refused initial selection is cleared. A label in the panel and a warning
  in the report view say "A distance between these elements (PlaneCone) is
  not supported: the solver has no exact mapping for it. Select other
  elements." Changing another joint's type to Distance on refused picks
  shows the same, and OK refuses them.
- **Joints already in a document** solve as before (the default case,
  Planar(d)), with one warning: "Assembly: the Distance joint <doc#name> is
  between elements (PlaneCone) the solver has no mapping for; it is solved
  as a planar distance, as before. New joints of this kind are refused."
  Once per joint and sub-type while the `AssemblyObject` lives
  (`warnedRefusedDistance`, keyed by full name and sub-type), so once per
  document load: a drag or a solve-per-edit session does not repeat it,
  and re-picking the joint into another refused kind says so again. Opening
  such a joint in the task shows no refusal and OK accepts it as it is;
  only a new pick or a type change is checked.

**Gate.** `TestSolverGateDistanceMapped` adds six cases (55 in all).
CurvePlane on a circle and the refused PlaneCone and PointTorus were
recorded from the build before the change (HEAD `758aad4c26`) and,
recorded again after it, are identical -- export digest, solve, placements
and statuses. LineCylinder, LineSphere and CurvePlane on an ellipse were
recorded from the build of the change; they differ from the pre-change
records, as intended (LineSphere's DoF goes from 33 to 35: a point-line
distance takes one freedom where Planar took three). The 49 earlier
records are untouched (the file gained only the six). `TestSolverGateOrder`
builds LineCylinder, LineSphere and CurvePlane in both orders and they
agree. `TestJoints.TestDistanceMapped` checks the new placements on the
geometry, not the JCS: the line parallel to the cylinder's axis and d + r
from it; the sphere's centre d + r from the line; every point of the
circle and of the ellipse d above the plane, along its normal; and a
PlaneCone joint built in the document solves as Planar(d) and warns once
over three solves. `TestGuiCommands` covers the task: a refused initial
selection, a refused second pick followed by an accepted one, a type
change to Distance, and an existing PlaneCone joint edited and accepted.

Suite 145 run, 1 skipped (134 + 11 new); ctest 911/911 (the two
TransactionMeasureTest cases flaked in the parallel run and pass alone);
Assembly gtest 11/11.

### 6.9 As built: the formula evaluator (2026-10-23)

Sec 7 q2, in three commits: the evaluator (`ccecaae903`), the builder's
parse and validation (`a8ec840b7f`), and the position-solve kinematic run
of sec 4.6 (`61edb5d599`). The grammar is sec 4.6.1.

**The evaluator** (`App/Solver/Formula.{h,cpp}`, no OndselSolver header).
`FormulaTree::parse(text, &error)` returns a shared, immutable tree, or
null with a `FormulaError` (message, 0-based position; `describe()` says
"... at column N"). `value(t)` and `evaluate(t)` -- the latter a
`FormulaValue` {value, rate, acceleration}, the first and second time
derivatives exact per node (second-order forward mode: each node combines
its children's value and two derivatives; `u^w` takes the constant-exponent,
constant-base and general `exp(w ln u)` forms). `dependsOnTime()`. The
parser is recursive descent in SymbolicParser's own shape (expression /
term / symfunction / raisedTo), which is what makes the precedence quirks
of sec 4.6.1 (`3*-2^2` is 12) come out the same. `rampstep` is lowered at
parse time to the piecewise form `RampStepFunction` builds. Errors name the
token found and, for a misspelt case (`Sin`, `TIME`), the right spelling.

**The model.** `Formula` keeps `text` (the record) and gains `tree`, the
builder's parse. `sameExceptDragWeights` compares the text only. The
builder (`makeMbdJoint`) parses each motion's formula once, after the
`initialValue` substitution, and uses the same parse for the `Both` driver
and the single one a cylindrical joint gets. A formula that does not parse
is reported once per build against its motion:

    Assembly: the formula of Doc#Motion is not valid: unknown function 'exp' at column 1: exp(time)

and `generateSimulation` returns -1 (with its usual "Generation of
simulation failed") before the model reaches a session. There is no
`Diagnostics` field for it: diagnostics are per joint, and the motion is
not one; the report view and the return code are the existing status path
of a simulation. Static solves and drags build no drivers, so nothing
changes for them.

**The Ondsel adapter is unchanged**: it hands OndselSolver the text, which
`runKINEMATIC` parses and evaluates itself, so every simulation result is
what it was. The gate passes with its records untouched (no re-recording).

**How the two evaluators compare** (`tests/src/Mod/Assembly/App/
FormulaEvaluator.cpp`; the gtest links OndselSolver for this only). 77
formulas -- every formula of `AssemblyTests` (with `initialValue`
substituted as the builder prints it, negative and exponent forms too), the
help text's examples with numbers for the capitals, the property's and the
dialog's defaults, and the grammar's corners (every sign rule, every
precedence and associativity case, every number form, every function,
`piecewise`, `rampstep`, blank) -- are evaluated by both at 97 times in
[0.01, 2.5]: **no disagreement** beyond 1e-12 relative. Derivatives match
five-point finite differences to 1e-6 relative (away from the pieces'
transitions and `abs`'s kinks). Inputs where the two differ are all
rejections, and all intended (sec 4.6.1): SymbolicParser **accepts** `2t`,
`2 3`, `time)`, `time(2)`, `1.2.3*time`, `3pi`, `2*time 5`, `2 $ time` by
dropping what follows the first complete formula; the evaluator rejects
them, and the test asserts both halves. Sixteen more errors are rejected
by both, with the evaluator's position checked; twelve malformed calls
(`sin(time, 2)`, a time-dependent `rampstep` bound, `spline`, `-.5`, ...)
are rejected by the evaluator and not handed to SymbolicParser, which
asserts or reads garbage on them.

**The help text.** Its "Exponential" example (`C*exp(time/TIMEC)`) had no
`exp` to call in either evaluator; it never ran, and failed at the build
with the message above instead of inside the solver. Since sec 6.10 both
evaluators have `exp`, and it runs; the help text is unchanged (it is
translated, and now correct).

**The default kinematic run, sec 4.6** (`Solver/Backend.{h,cpp}`):
`simulateByPositionSolves(session, model, spec, sink)`. At each output time
`tStart + k * tOutput` it holds every driver at its formula's value (a
constant formula printed `%.17g`, so the double survives the text), loads
the model with the bodies at the previous frame's poses, solves, and sends
`poses()` to the sink. It is a free function, not `Session::simulate`'s
default body, because the base class does not keep the model; a backend
without kinematics answers `simulate` with it. No backend does yet.
`OndselAdapter.positionSolvesFollowTheKinematicRun` builds the gate's
simulation (`TestSolverGateSimulation.simulation_doc`: a revolute at
`36*time`, a slider at `5*time + initialValue`, a cylindrical at `90*time`
and `2*sin(time)`) as a model and runs it both ways: the eleven frames agree
to **5e-13** in position and rotation angle. One difference in shape:
`runKINEMATIC` reports the start twice (the assembled pose, then t = 0), the
position solves once per output time; the test drops Ondsel's first frame.

Tests: Assembly gtest 20/20 (11 + 7 `FormulaEvaluator` + 2 `OndselAdapter`);
ctest 920/920 (the two `TransactionMeasureTest` cases flaked in the
parallel run and pass alone); the Assembly suite 146 run, 1 skipped (145 +
`TestSolverGateSimulation.test_bad_formula_refused`: `36*time)`,
`exp(time)` and `2*-time` give -1 and no frames, `36*time` then runs).

### 6.10 As built: tan, exp, ln, sqrt (2026-10-23)

The user's decision: the grammar gains `tan`, `exp`, `ln` and `sqrt`
(sec 4.6.1), in both evaluators, so the help text's Exponential example
runs as written.

**OndselSolver** (submodule `61fa24e`, branch `LinkVibe`).
`SymbolicParser::intrinsic()` maps the four names. `Exponential` and `Ln`
existed and were reused, both unfinished: `Exponential::getValue()`
returned `log(x)`, and neither had a derivative. `Sqrt` and `Tan` are new,
in the style of `Sine`/`Cosine`. `Reciprocal`'s derivative was `-(x^2)`
instead of `-1/x^2`; fixed, since `ln'` and `sqrt'` go through it (it also
corrected the velocity and acceleration of every motion that divides by
something time-dependent; positions never used it).

**The evaluator** (`Formula.cpp`): `Op::Tan`, `Exp`, `Ln`, `Sqrt`, with
exact first and second derivatives. `ln` and `sqrt` of a constant argument
have rate 0 (not NaN times 0). `simulateByPositionSolves` now returns
`Failed` at an output time where a driver's value is not finite, instead of
printing `nan` into a constant formula.

**Domain errors, as measured.** The evaluator gives NaN or an infinity
(sec 4.6.1). OndselSolver's `runKINEMATIC`, on a slider at
`ln(time - 0.5)`: `generateSimulation` returns 0 and every frame leaves
the part where it was -- the NaN at t = 0 is never recovered from, even
after t = 0.5. `sqrt(0.5 - time)` is right up to t = 0.4 and stuck from
0.5 on. `2*ln(time - 0.5)` gives 2 before t = 0.5 (its product drops the
NaN factor) and the right values after. So OndselSolver does not report a
domain error; the builder does not check values (a domain depends on time,
and checking it would mean sampling), and the run "succeeds" with frames
that do not follow the formula. A position-solve backend fails instead.

**Tests.** `FormulaEvaluator.cpp`: 18 new corpus formulas (95 in all),
among them `2*exp(time/0.8)` (the help text's `C*exp(time/TIMEC)`),
`sqrt(1 + time^2)`, `ln(2 + sin(time))`, `tan(time/4)`, `tan(time + 2)`
(cos < 0), compositions of all four: values agree with SymbolicParser to
1e-12 relative, derivatives with five-point differences to 1e-6. New
`derivativesMatchOndselSolver`: SymbolicParser's own symbolic derivatives,
simplified and differentiated as `ASMTRotationalMotion::createMbD` and
`EndFrameqct` form them for `runKINEMATIC`, against the evaluator's, to
1e-10 relative, on the whole corpus. It found three things in OndselSolver
that are not about the new functions and were left as they were, recorded by
the test (which asserted they still disagreed, so a fix would show). All
three are fixed since sec 6.12, and the test now requires agreement:

- `abs`, `asin`/`arcsin` and `arctan` have no derivative (`FunctionX`
  throws "To be implemented" when the derivative `runKINEMATIC` needs is
  formed). Not tried in a kinematic run here; the help text's arctan
  examples are such formulas.
- A power with a time-dependent base and exponent (`(time + 1)^time`,
  `time^(sin(time) + 2)`) gets a wrong derivative: `Symbolic::times`
  appends to the terms vector of the `Product` it is handed, which the
  `Power`'s derivative still shares.
- A piecewise whose x is not `time` itself (`rampstep(2*time, ...)`,
  `piecewise(time - 1, ...)`): `PiecewiseFunction::differentiateWRTx`
  differentiates the pieces, which are functions of time, with respect to
  x.

New `domainErrors` (the IEEE values, and the `2*ln(time - 1)` difference);
`errorsWithPositions` uses `log` and `acos` where it used `exp`, and checks
`Exp` gets the case hint. Python: `TestSolverGateSimulation.test_exp_motion`
runs a slider at `2*exp(time/0.8)` through `generateSimulation` and checks
the part's z at t = 0, 0.5, 0.7 and 1 against the formula to 1e-6 mm (and
x, y unmoved); `test_bad_formula_refused` uses `log(time)` for the unknown
function. The gate's records are untouched.

Assembly gtest 22/22 (20 + 2); ctest 920/922 in the parallel run, the two
`TransactionMeasureTest` cases pass alone; the Assembly suite 147 run, 1
skipped.

### 6.11 As built: geometry recognition and role mappings (2026-10-23)

The user's decision on Distance "Other", four points: interpret geometry
the flexible way Assembly3 does -- recognise canonical geometry first,
keep whole-object picks, refuse what is truly freeform with a message
that says why, and map the 14 refused pairs by role.

**1. Recognition** (`App/ElementGeometry.{h,cpp}`, not upstream). The
one place the workbench reads an element's curve or surface type. An
analytic element (line, circle, ellipse; plane, cylinder, cone, sphere,
torus) is taken as it is, through the same adaptor `edge.Curve` and
`face.Surface` use. Any other (B-spline, Bezier, offset, surface of
revolution or extrusion) goes to OCCT's `ShapeAnalysis_CanonicalRecognition`
-- `IsLine`, `IsCircle`, `IsEllipse` for an edge; `IsPlane`,
`IsCylinder`, `IsCone`, `IsSphere` for a face, in that order -- and, for
the torus, which it does not try, to the converter it drives
(`GeomConvert_SurfToAnaSurf` aimed at `GeomAbs_Torus`). What is still
unrecognised is freeform.

- *Tolerance:* 1e-6 of the element's bounding-box diagonal (Assembly3's
  normalised 1e-6, `asm3/utils.py` `fit_rotation_axis_to_surface1`
  callers), at least `Precision::Confusion()`. A B-spline circle from
  `Part.Circle().toBSpline()` is recognised with a gap of 1.1e-8 (the
  fit's, not the curve's: centre and radius off by 4e-9); `toNurbs()`
  cylinders, cones, spheres, tori and planes at 1e-14 or less.
- *Cache:* by the element's TShape, in its own coordinates (the location
  is applied on the way out), so the same face of a linked part placed
  ten times is converted once. Held by handle, at most 4096 of each,
  cleared when full. `AssemblyApp.clearElementGeometryCache()` and
  `getElementGeometryConversions()` exist for the test that checks it.
- *Readers:* `isEdgeType`, `isFaceType`, `getFaceRadius`,
  `getEdgeRadius` (so `getDistanceType` and the builder's radii), the
  refusal (below); Python through `AssemblyApp.getElementGeometry(shape)`
  (the analytic Part geometry, or None) and `analyzeElement(shape)` (type,
  recognised, gap, tolerance). `UtilsAssembly.elementGeometry()` wraps the
  first; `findPlacement` and `findElementClosestVertex` use it, so every
  joint type frames a recognised element as its analytic form: a revolute
  on a B-spline circle takes the circle's centre and normal. Part has no
  binding for canonical recognition (only `toPlane`/`toLine`), so the one
  source of truth is the Assembly module's.
- *What changes:* only elements that were not analytic. A recognised
  cone's axis may come out reversed and a recognised plane's or
  circle's x axis turned (the fit picks its own parametrisation): the
  distance kinds ignore both, a joint that uses x (Fixed, Angle) on such
  an element sees it. An existing joint on a B-spline element that is now
  recognised changes classification (from Other, Planar(d), to its proper
  sub-type) without a message.
- *A torus's frame.* `findPlacement` tests `"Part::GeomTorus"`, but a
  torus is `Part::GeomToroid`, so upstream frames every torus at its face's
  centre of gravity. That is near the axis for a torus of revolution
  through 360 degrees, but off it for a part of one (a fillet's quarter),
  and every axis reading below would measure from the wrong line. Every
  torus face now takes the torus's centre (sec 6.14).

**2. Whole objects** stay as built in sec 6.8: Other with a whole object on
a side (a datum plane, an LCS, a body) is not refused and solves as
Planar(d) between the objects' frames. Now tested
(`TestDistanceRefusal.test_whole_objects_are_other_and_accepted`).

**3. Refusal says why.** `refusedDistanceTypes()` is now `{Other}`; the rule
is `distanceJointRefusal(joint, type)` in `AssemblyUtils.cpp`, which
returns the reason, naming the element by its name and the picked
component's label, and what it is:

- Other between two elements: the first face that is not analytic, "the
  face Face1 of Free is not planar, cylindrical, conical, spherical or
  toroidal: it is a B-spline surface"; else (two edges) the first edge
  that is not a line or a circle, "the edge Edge1 of EllM is not a line
  or a circle: it is an ellipse".
- CurveCylinder, CurveSphere, CurveCone, CurveTorus on a curve that is not
  a circle (the curve stands for its axis and radius): the same edge
  message.
- CurvePlane and PointCurve on a curve that is neither a line, a circle,
  an ellipse nor planar (`TopoShape::findPlane`, what `edge.findPlane()`
  calls): "the edge Edge1 of HelixM is not a line and not planar: it is a
  B-spline curve". Sec 6.8 left the helix as a known gap; its frame was
  the identity at the origin.

`isDistanceJointRefused` is `!distanceJointRefusal().empty()`; Python has
`AssemblyApp.getDistanceRefusal(joint)`. The joint task shows "A distance
between these elements is not supported: <reason>. Select other
elements." (label and report view), with the same pick handling as sec
6.8. A refused joint already in a document solves as before (Planar(d);
CurvePlane, PointCurve their own case) and warns once per joint and
sub-type: "Assembly: the Distance joint <doc#name> is between elements
(<sub-type>) the solver has no mapping for: <reason>. It is solved as
before; new joints of this kind are refused."

**4. Role mappings.** Assembly3's constraints take a role from each
element (`asm3/constraint.py` `_p` point, `_n` normal, `_l` line, `_w`
plane). The workbench's existing mappings already read roles: a cylinder
is its axis plus a radius (PlaneCylinder, CylinderCylinder, PointCylinder,
LineCylinder), a sphere its centre plus a radius, a circle its axis plus
a radius (LineCircle, CircleCircle), a torus its axis with radius 0 next
to an axis or a point (CylinderTorus, TorusSphere; `getFaceRadius` gives
0 for it) and its equatorial plane next to a plane (PlaneTorus,
TorusTorus). Each new mapping is an existing one with one element read
the way that mapping already reads its kind: **a cone is its axis, as a
cylinder of radius 0** (no constant offset from the axis exists; its JCS
is on the axis at the apex, z along it), a torus and a circle as above.
So `d` keeps the meaning it has everywhere else: the gap from the
surface of a cylinder, sphere or circle's cylinder, to the axis of a
cone or torus.

| pair (I, J after the ordering) | reuses | neutral (value) | meaning of d |
|---|---|---|---|
| PlaneCone | PlaneCylinder, r = 0 | LineInPlane(d) | the cone's axis parallel to the plane, d from it |
| CylinderCone | CylinderCylinder, r2 = 0 | LineLineDistance(d + r1) | the cone's axis parallel to the cylinder's, d from the cylinder |
| ConeCone | CylinderCylinder, r = 0 | LineLineDistance(d) | the axes parallel, d apart |
| ConeTorus | CylinderTorus, r1 = 0 | LineLineDistance(d) | the cone's and the torus's axes parallel, d apart |
| ConeSphere | CylinderSphere, r1 = 0 | PointLineDistance(d + r2) | the sphere d from the cone's axis |
| PointCone | PointCylinder, r = 0 | PointLineDistance(d) | the point d from the cone's axis |
| PointTorus | TorusSphere, r = 0 | PointLineDistance(d) | the point d from the torus's axis |
| LineCone | LineCylinder, r = 0 | LineLineDistance(d) | the line parallel to the cone's axis, d from it |
| LineTorus | CylinderTorus with a line | LineLineDistance(d) | the line parallel to the torus's axis, d from it |
| CurveCylinder (a circle) | CircleCircle / CylinderCylinder | LineLineDistance(d + r1 + r2) | the axes parallel, the circle d from the cylinder |
| CurveSphere (a circle) | CylinderSphere; swapped as LineSphere | PointLineDistance(d + r1 + r2) | the sphere d from the circle's cylinder |
| CurveCone (a circle) | LineCircle | LineLineDistance(d + r2) | the axes parallel, the circle d from the cone's axis |
| CurveTorus (a circle) | LineCircle | LineLineDistance(d + r2) | the axes parallel, the circle d from the torus's axis |

Left refused, because no reading is defensible:

| pair | why |
|---|---|
| Other between two elements | an element that is freeform after recognition (no point, axis or plane of its own), or two edges of which one is neither a line nor a circle (an ellipse has a plane but no axis to keep parallel or measure from) |
| CurveCylinder, CurveSphere, CurveCone, CurveTorus on an ellipse or a planar spline | the mapping reads the curve as an axis and a radius; only a circle has them |
| CurvePlane, PointCurve on a non-planar curve | the mapping reads the curve as a plane |

Alternatives weighed and not taken: a cone's apex as its point (PlaneCone
as PointInPlane, PointCone as PointPointDistance) -- every other
mapping reads an axisymmetric surface as its axis, and the apex of a
frustum lies off the face; and PointTorus to the centre -- TorusSphere
already measures a point (the sphere's centre) from the torus's axis.
None of the readings makes a cone or a torus touch its partner
(tangency is not a neutral kind).

**Existing documents.** A joint of the 13 sub-types solved as Planar(d)
(with the sec 6.8 warning); it now solves with its mapping, so a document
saved with one moves on its next solve, and the DoF changes (PlaneCone
in the gate: 33 to 34; PointTorus 33 to 35). That is applied, not asked:
the old result was the questionable default the decision set out to
replace. The builder logs it once per joint and sub-type, at log level
(the report view shows it when log messages are on): "Assembly: the
Distance joint <doc#name> (PointTorus) is solved as the point d from the
torus's axis; before the role mappings (2026-10-23) it was a planar
distance." Log, not warning: a joint made after the change would
otherwise warn on every load, and nothing records which joints are old.

**Gate.** `TestSolverGateDistanceMapped`'s two refused cases changed
classification, so only they were re-recorded, under new keys (the label
lost "(refused, planar)"): PlaneCone (export digest `ec2ef719b2b8...` to
`2bf10b8d3471...`, DoF 33 to 34) and PointTorus (`980d6927e445...` to
`7232f1d9c0b4...`, DoF 33 to 35). The other 53 records are byte for byte
what they were (checked on the parsed file: every old key but the two is
equal). New `TestSolverGateRoleMappings` records 17 cases: the 13
mappings, CylinderCylinder and LineCircle on a `toNurbs()` cylinder, and
two refused joints in a document (CurveCylinder on an ellipse, Other on a
freeform face; Planar(d)). `TestSolverGateOrder.test_role_mappings_both_orders`
builds nine of them in both orders (CurveSphere swaps in the case). 72
records in all.

**Tests** (`TestJoints`). `TestDistanceRoleMappings`: each of the 13 on
the solved shapes (axes parallel and their distance, a point's or a
centre's distance to an axis, the cone's axis in a plane d above);
a quarter torus's JCS on the axis and PointTorus measured from it; a
mapped joint logs, no warning. `TestElementRecognition`: the analysis of
`toNurbs()` shapes (seven kinds and a freeform face), the tolerance, the cache (one
conversion for a face analysed three times, once moved), the same JCS
for a B-spline cylinder face and circle as for the analytic ones (1e-6),
the same CylinderCylinder classification and solve (placements within
1e-6), a revolute on a B-spline circle. `TestDistanceRefusal`: the eight
messages above and two accepted curve cases, whole objects.
`TestDistanceMapped` and the four refusal tests in `TestGuiCommands` use
a freeform face where they used a cone.

Suite 171 run, 1 skipped (147 + 24); ctest 922/922; Assembly gtest 22/22.

### 6.12 As built: OndselSolver's derivatives and external motion functions (2026-10-23)

Two steps the user approved after sec 6.10: fix what
`derivativesMatchOndselSolver` found in OndselSolver's differentiation, so
an ASMT file is right for any consumer, and stop OndselSolver from parsing
and differentiating the workbench's formulas at all.

**The fixes** (submodule `4e99d2f`, branch `LinkVibe`).

- `Abs`, `ArcSine` (`asin`, `arcsin`) and `ArcTan` get `differentiateWRTx`.
  `abs'` is a new `Sign` node: sign(x), **0 at x = 0** (the mean of the
  one-sided slopes, what the evaluator gives), its own derivative 0 (the jump
  is not seen, as a piecewise function's are not). `x/abs(x)` was not taken:
  NaN at 0, and `Product::getValue` silently drops a NaN factor. `asin' =
  1/sqrt(1 - x*x)`, `atan' = 1/(1 + x*x)`, the squares as products (no `Power`
  of a negative base).
- `Symbolic::times` and `Symbolic::sum` inserted into the terms vector of the
  `Product` or `Sum` they were handed, so the argument changed with the
  result. The case that showed: `Power::differentiateWRTx` forms `sum(y, -1)`;
  when the exponent `y` is itself a `Sum` -- as it is in the velocity of
  `(time + 1)^time`, which the acceleration is formed from -- the exponent
  was decremented in place, and the velocity, still in use, changed. Both
  functions now copy the vector before inserting. The new node copied it
  anyway, so only aliased callers see a difference.
- `PiecewiseFunction` (so `rampstep` too) overrides `differentiateWRT`:
  the same selection of the pieces' derivatives with respect to the
  variable. The pieces are functions of time; `FunctionX`'s
  `differentiateWRTx() * dx/dt` differentiated them with respect to x (0 for
  a piece of time) and was right only for x = `time`. A `RampStep`'s middle
  piece is a `Polynomial` of x, whose own derivative applies the chain rule.

Verified by `derivativesMatchOndselSolver`, now without its list of expected
disagreements: no throw, and first and second derivatives within 1e-10
relative of the evaluator's at 97 times, for all 95 corpus formulas
(`abs(sin(time) - 0.3)`, `asin(time/4)`, `arctan(3*time - 2)^2`, `(time +
1)^time`, `time^(sin(time) + 2)`, `rampstep(2*time, ...)*time`,
`piecewise(time - 1, ...)` among them). New `absAtItsKink`: both give 0 at
the kink, -1 and 1 on either side.

**The external node** (submodule `232d69b`). `MbD::ExternalFunction`, a
`FunctionX` holding `ExternalTimeFunction` -- `std::function<double(double
time, int order)>`, the order-th time derivative -- an order, and a name
for printing.

- `getValue()` is `f(xx->getValue(), order)`; xx is the time it reads, a
  motion's `geoTime` (`ASMTTime`, mbdUnits->time times the system's time).
- `differentiateWRT(var)` applies the chain rule through xx: d xx / d var,
  simplified, is 1 for the system's time in units of 1, and then the result
  is the same callback with order + 1; another constant c gives c times
  that; 0 gives 0 (any variable xx does not depend on); anything else the
  product. This is what the task's "order + 1 when var is the time" comes
  to, without assuming the time unit is 1.
- Atomic: `expandUntil` and `simplifyUntil` return the node itself,
  `clonesptr` copies it, `isConstant` is false (so `Sum` and `Product`
  never fold it into a number). Prints as `external[36*time]''(ASMTTime)`.

Every path a motion's block takes keeps it one node:
`ASMTRotationalMotion::createMbD` multiplies by `1/mbdUnits->angle`
(`Symbolic::times`: a `Product` of the node and 1) and simplifies (the
constant 1 drops out: the block is the node); `EndFrameqct` differentiates
with respect to the system's time and simplifies, twice (order 1, order 2);
`EndFrameqct2` (a `GeneralMotion`'s `EulerAngles`) differentiates without
simplifying, which gives the same nodes directly.

The API: `ASMTRotationalMotion::setRotationZ(text, function)`,
`ASMTTranslationalMotion::setTranslationZ(text, function)`,
`ASMTGeneralMotion::setrIJI(i, text, function)` and `setangIJJ(i, text,
function)`. `createMbD` builds the node on `geoTime` where it parsed the
text, then goes on as before. The text is kept, because it is what
`outputFile` writes; the one-argument setters, or an empty function, keep
the text path (an ASMT file read back has no function).

**FreeCAD.** `Solver::Formula` gains `function`, a `FormulaFunction`
(`std::function<FormulaValue(double)>`: value, rate, acceleration at t),
`functionOf(tree)`, and `evaluator()` -- `function`, else the tree's, else
a parse of the text's, empty when that fails. The builder sets `function`
from the tree it parsed, so every driver carries its callable; a backend
that takes functions instead of text calls it, and a driver could be given
one with no text behind it. The Ondsel adapter (`toAsmt`) passes each
motion component as `externalFunction(formula)`: the evaluator's value,
rate and acceleration for orders 0, 1, 2, NaN beyond (`runKINEMATIC` asks
for no more). A text the evaluator rejects (only a hand-built model can
have one; the builder refuses it, sec 6.9) has no evaluator and goes over
as text, as before. `exportAsASMT` and the dump file still write the
formula text: the gate's ASMT digests are unchanged.
`simulateByPositionSolves` holds each driver at `evaluator()`'s value
(the tree's, the same double as `value(t)`), and resets `function` with the
held constant.

**Gate.** `TestSolverGate`'s simulation frames are compared by position
(1e-9 mm) and rotation angle (1e-9 rad; from the vector part of the
relative quaternion, since `acos` near 1 cannot see 1e-9) instead of per
value, and each simulation case records its largest deviation
(`SIMULATION_DEVIATION`, also printed to the report view). Everything else
is compared as before (digests and statuses exactly, the other
placements per value within 1e-9). Not re-recorded. **Largest deviation
seen: 0 mm and 0 rad** in `test_simulation`, its reverse playback and
`test_simulation_gears`, **0 mm and 1.1e-16 rad** in
`test_simulation_rigid_group`: the gate's formulas (`36*time`,
`5*time + initialValue`, `90*time`, `2*sin(time)`, `45*time`) have the same
derivatives either way, and the positions are solved to tolerance at each
output time, so the frames came out the same.

**Tests.** gtest: `externalFunctionIsTheEvaluator` (every corpus formula
as a node through the units product, `simplified()` and two
differentiations: one node of order 0, 1, 2 each time, its values equal to
the evaluator's bit for bit at 97 times; on a `geoTime` of 2t the
derivatives are 2 and 4 times the evaluator's; a variable the time does not
depend on gives 0); `absAtItsKink`; `OndselAdapter.driversAreExternalFunctions`
(the gate simulation's four motions carry their text and a function equal
to the evaluator by order, a driver's own `function` is used when it has no
tree, a rejected text goes over as text). Python
(`TestSolverGateSimulation`, not recorded): `test_abs_motion`
(`4*abs(time - 0.45)`, slider), `test_asin_motion` (`10*asin(time/2)`),
`test_arcsin_angular_motion` (`arcsin(0.9*time)`, revolute),
`test_arctan_motion` (the help text's smooth step as an angle),
`test_power_of_time_motion` (`(time + 1)^time`),
`test_piecewise_of_2time_motion` (`piecewise(2*time, functions(time,
2*time - 0.3, 0.75 + 0.5*time), transitions(0.6, 1.4))`, revolute) and
`test_rampstep_of_2time_motion`: each runs, and the part at all eleven
output times is where the formula puts it, to 1e-6 mm or rad (a revolute's
axis point fixed to 1e-6, the rotation about z only).

Assembly gtest 25/25 (22 + 3); ctest 922/924 in the parallel run, the two
`TransactionMeasureTest` cases pass alone; the Assembly suite 178 run, 1
skipped (171 + 7).

### 6.13 As built: the Reference backend (2026-10-23)

Sec 6.3 step 10's first backend and sec 7 q4, in three commits: the
backend and its gtests (`47e02a7cc5`), the neutral checker in Python
(`6e635ce875`), the gate once per backend (`20f8f1ef8e`).
`App/Solver/ReferenceBackend.{h,cpp}`, Eigen only, registered as
"Reference" after "Ondsel" (`AppAssembly.cpp`), so the `SolverBackend`
property, the preference and `getAvailableSolvers()` name it. The whole
model at once, simple rather than fast: it is the oracle other backends
are tested against.

**Variables.** Per body a position and a rotation increment: the spatial
rotation vector, applied on the left of the body's quaternion, which is
renormalised after every step (no Euler angles, no singularity). A body
a `Ground` record pins is not a variable: it is put where its first
Ground holds it (left bit for bit when it is there within 1e-12) and
stays. The metric of a solve is `|dx|^2 + |dtheta|^2 / 4` per body (mm,
rad): OndselSolver's, whose Euler parameters move by half the angle.

**Equations**, each in the form OndselSolver uses for it, so that the
rank of the Jacobian -- and with it the DoF and the redundancy --
answers the same question. `r` is oJ - oI; `xI, yI, zI` frame I's axes;
"cos(a, b)" a direction cosine.

| kind | equations |
|---|---|
| Fixed, Ground | r along the three global axes; cos(yI, xJ), cos(zI, xJ), cos(zI, yJ) |
| Revolute | r global (3); cos(zI, xJ), cos(zI, yJ) |
| Cylindrical | r.xI, r.yI; cos(zI, xJ), cos(zI, yJ) |
| Prismatic | r.xI, r.yI; cos(yI, xJ), cos(zI, xJ), cos(zI, yJ) |
| Spherical | r global (3) |
| Planar(v) | r.zI - v; cos(zI, xJ), cos(zI, yJ) |
| ParallelZ / PerpendicularZ / AngleZ(v) | cos(zI, xJ), cos(zI, yJ) / cos(zI, zJ) / cos(zI, zJ) - cos v |
| PointInPlane(v) / LineInPlane(v) / PointOnLine | r.zI - v / that and cos(zI, zJ) / r.xI, r.yI |
| PointPointDistance(v) | \|r\| - v |
| PointLineDistance(v) / LineLineDistance(v) | (r.xI)^2 + (r.yI)^2 - v^2 / that and cos(zI, xJ), cos(zI, yJ) |
| Gear, Belt (value / value2 = ratio) | orbit(J, I) + ratio orbit(I, J) - c; coaxial frames (OndselSolver's test, 1e-9): angleZ(K, J) + ratio angleZ(K, I) - c, a carrier frame K required |
| RackPinion(p) / Screw(p) | r.xI + p thetaZ(I, J) - c / 2 pi r.zI - p thetaZ(I, J) - c |

`thetaZ(I, J)` is the angle of xJ about zI from xI, `orbit(I, J)` the angle
of J's origin about zI (OndselSolver's `AngleZIeJe`, `OrbitAngleZIeJe`).
A coupler's constant `c` is its value at the start, as OndselSolver's is,
and its angles are unwrapped across iterations from OndselSolver's first
reading (atan2 when the cosine is positive, else [0, 2 pi)). The
Jacobian is analytic: every quantity carries its derivative with respect
to the six variables of each of the (at most three) bodies it depends on;
a gtest checks every kind, the couplers and the drivers against central
differences.

**Solve.** The pose closest to a target in the metric, subject to the
equations: the start for a static solve, the drag targets for a drag
step. Equations dependent at the start are set aside (what OndselSolver
does at its first singular matrix); the rest are solved by
**restoration** -- minimum-norm Gauss-Newton steps, Levenberg-Marquardt
damping and backtracking when a step does not reduce the residual, in a
metric that weighs a turn by the body's largest lever arm squared, and no
body turned more than a radian per step (without that, a far start turned
a box through ten radians to touch a plane) -- alternated with steps
**along the manifold** towards the target: the projected gradient, and
after six rounds the reduced Newton step (the Lagrangian's Hessian by
central differences of the analytic gradients, on the null space of the
kept equations, when positive definite), each step restored onto the
manifold and taken when it brings the poses closer. It stops when the
projected step is below 1e-11, the kept equations below 1e-11 (1e-9
accepted when rounding stalls there). Then equations set aside that the
solution shows independent and unsatisfied join and the solve repeats,
so a singular start does not stick. Without the Newton step the gate's
PointCone case stopped 0.05 % farther from the start than OndselSolver's
solution.

**Limits** are inequalities, by an active set: a solution that violates
a bound gets the bound as an equation (essential for an angle,
displacement for a slide, as OndselSolver's limits) and the solve
repeats; a bound whose multiplier says the solution would leave it is
released. In static solves too -- OndselSolver applies limits only in
drag steps. A rotation limit's angle is counted in turns from
OndselSolver's first reading. Limits are not in the diagnostics.

**Drag.** `dragStep` is the same solve with the dragged bodies' targets
and weights (`Body::dragWeight`; the others held where they are with
weight 1), so a dragged body follows the mouse as far as the joints and
the other bodies' resistance allow; a step that fails is retried half
way, then the poses go back and it answers `NotConverged`. `endDrag` is a
static solve from the dragged state. `update` takes drag weights and
answers `NeedsReload` for anything else.

**Drivers and simulation.** A driver is an essential equation: the slide
along zI at the formula's value, the turn thetaZ(I, J) at it with its
turns counted -- the formula is an absolute angle, so the count starts at
the turn nearest the formula's value and follows the motion. `simulate`
solves each output time in one session from the previous frame
(`simulateByPositionSolves`, which reloads the model per frame and so
loses the turns, is not used). Outside a formula's domain the run stops
with `Failed` (sec 4.6.1).

**Diagnostics.** The DoF from the rank of the Jacobian by column-pivoted
QR, rows scaled to unit length, threshold 1e-8 relative to the largest
pivot; in `OndselSession`'s convention, 6 x (1 + bodies) less the
equations kept (sec 6.6, deviation 3). Which equations are dependent:
Gram-Schmidt with reorthogonalisation over the rows in OndselSolver's
order (its categories -- essential, displacement, perpendicular -- then
model order), an equation dependent when what is left of its gradient is
below 1e-8 of its norm (at least 1); the two counts are compared, a
mismatch logged. A joint with a dependent equation is redundant,
partially when not all of its equations are, conflicting when a dependent
equation is unsatisfied by more than 1e-6 (OndselSolver's threshold).
The same `Diagnostics` fields as `OndselSession` fills, the same meaning.

**Capabilities** (sec 5.3's Reference column): every `JointKind`, limits,
drivers, drag, kinematics (by position solves), redundancy, conflicts;
not dynamics, warm update, per-body DoF; native format "neutral" --
`exportNative` writes the neutral model as text (bodies at their current
poses, frames, grounds, joints, limits, drivers), which nothing reads
yet. `exportAsASMT` writes it too when the assembly's backend is the
Reference: it is the session's `exportNative`.

**The checker** (sec 4.4 item 7): `Solver::Reference::jointResiduals(model,
poses)` and `groundResiduals`, each joint's largest error in natural
units (mm, rad; the distance itself for the squared forms, the slide for
a screw), couplers against the relation at the model's poses.
`AssemblyObject::getSolverResiduals()` / `assembly.getSolverResiduals()`
apply it to the session's poses, by joint name: for any backend.

**Gate** (`TestSolverGate.py`). Every gate class runs again per
registered backend other than OndselSolver (`TestSolverGateJointsReference`
and so on, generated; the backend set by the `SolverBackend` property),
compared with the OndselSolver records by the oracle's rules
(`gate_backend`): converged wherever OndselSolver converged; DoF and the
redundant, partially redundant, conflicting and malformed lists equal;
placements within 1e-7 where the DoF is 0, otherwise every joint
satisfied to 1e-9 by the checker and the solution no farther from the
start than OndselSolver's in the Reference's metric; the drag steps
OndselSolver took taken, each leaving every joint satisfied; simulation
frames within 1e-7. The OndselSolver runs, the records and how they are
compared are unchanged. Result, all 72 records:

- **Converged** in all 72, including PointPoint at d = 0, where
  OndselSolver returns -1 (the builder makes it a Spherical joint;
  OndselSolver does not converge from the box 50 mm away and leaves it
  there; the Reference puts the vertices together).
- **Fully constrained** (OndselSolver's DoF 0: Fixed): placements
  8.3e-11 from the record. Every joint of every case satisfied to 1.0e-11.
- **Closest to the start:** of the 69 cases with a free DoF where
  OndselSolver's solution satisfies the joints, neither moved anything in
  13 (the start already solved), the Reference's solution is as close as
  OndselSolver's in 39 (within 0.1 %) and closer in 17 (PointLine 122.7
  against 292.5, PointPoint 2295 against 2899, CylinderCylinder 854
  against 2624, in mm^2 + rad^2 / 4); never farther.
- **DoF and redundancy** equal in 70; two differ, both cases where
  OndselSolver leaves joints unsatisfied (measured with the checker) and
  calls them conflicting: `test_drag_bundles_fixed` (the Revolute 0.80
  and the Fixed 0.92 off; record DoF 8, both redundant and conflicting --
  C starts 50 mm from where the Fixed joint puts it, and OndselSolver
  solves without the equations it set aside on the way; the Reference
  satisfies both, DoF 1, nothing redundant) and the LineTorus role mapping
  (0.73 mm off, the box not moved, DoF 49; the Reference satisfies it,
  DoF 45, as both count for LineCone, ConeCone and ConeTorus).
  `BACKEND_DIFFERENCES` records them, asserted as they are.
- **Drag:** the same steps taken in all eight drag cases, every joint
  satisfied after each.
- **Simulation:** the four cases disagreed with the records, and the
  records were wrong. OndselSolver's angular motion held only the sine of
  the angle (`ZRotation`: the direction cosine of I's y and J's x), zero
  half a turn away too; the gate's drivers turn 3.6, 9 and 4.5 rad per
  0.1 s output step, and from the previous frame it landed on the root
  nearest, half a turn off at every other frame (and the gear driven
  through it followed the wrong turn). Against OndselSolver run again at a
  tenth of the output step the Reference's frames agreed to 3.4e-10
  (`BACKEND_SIMULATION`); the gtest `drivenAnglesFollowTheFormula` checked
  them against the formulas and asserted OndselSolver's half turns, so a
  fix would show. At a 0.01 s step the two agree to 1e-7 on the gate's
  simulation (`simulationFollowsTheKinematicRun`). *Fixed in OndselSolver
  (sec 6.16)*: the records were taken again, `BACKEND_SIMULATION` is
  empty, and the Reference's frames are compared with them directly
  (3.5e-10 at most).

**Bench** (`scripts/assembly-bench/bench.py` through the preference, the
runs of sec 6.6; seconds, Reference / Ondsel, one run each):

| case | solve_1 | preDrag | doDragStep (median of 10) | postDrag |
|---|---|---|---|---|
| star 49 | 0.054 / 0.056 | 0.028 / 0.044 | 0.0049 / 0.013 | 0.0023 / 0.014 |
| chain 49 | 0.052 / 0.60 | 0.031 / 0.069 | 1.28 / 0.027 | 0.0025 / 0.034 |
| star 199 | 2.36 / 0.43 | 0.42 / 0.27 | 0.18 / 0.083 | 0.12 / 0.099 |
| chain 99 | 0.31 / 9.07 | 0.088 / 1.47 | 13.7 / 1.00 | 0.018 / 0.79 |

Dense linear algebra on the whole model: the star at 199 pays for it.
A chain's drag step turns every link and runs into the reduced Newton
step, whose Hessian by differences costs 12 evaluations of the system per
body. Speed is not this backend's job. The JCS origins after a solve are
within 2.3e-12 mm (OndselSolver 1e-14): the Reference stops at 1e-11.

**Tests.** Assembly gtest 38/38 (25 + 13 `ReferenceBackend`); the
Assembly suite 216 run, 1 skipped (178 + 37 generated gate tests +
`TestSolverBackend.test_reference_backend`); ctest 938/938.

**Not done:** per-body DoF, `overDefinedGroups`, warm updates beyond drag
weights; nothing reads the neutral text yet; `PyBackend` and Chrono are
the next backends of step 10.

### 6.14 As built: a torus is Part::GeomToroid (2026-10-23)

Upstream's `UtilsAssembly` tests a face's surface for `"Part::GeomTorus"`
in two places; a torus is `Part::GeomToroid`, so neither ever matched.

- `findPlacement` framed a torus at its face's centre of gravity instead
  of the torus's centre. Sec 6.11 had moved only a partial torus (centre
  of gravity off the axis) to the centre; now every torus face is framed
  at `surface.Center`, as a sphere is.
- `findElementClosestVertex` returns a sphere face itself, with no vertex,
  and meant to do the same for a torus. A torus face instead looked for a
  circle edge to take the centre of, so a pick on a fillet snapped to the
  centre of one of its edge arcs. It now takes the face, as a sphere does.

Nine gate records were recorded again, all of them torus cases:
PlaneTorus, CylinderTorus, TorusTorus, TorusSphere, PointTorus (mapped and
role), ConeTorus, LineTorus, CurveTorus. In each, the export digest and
the solver input changed, because the frame moved by rounding onto the
axis. Statuses and DoF are unchanged. Placements moved by at most 2.6e-13
(ConeTorus), and seven of the nine not at all. The Reference backend's
LineTorus disagreement (sec 6.13) is unchanged.

Simulation note, from the Reference backend's run (sec 6.13), checked
against the records. In `test_simulation`, the box driven at `36*time`
with a 0.1 s output step advanced 3.6 - pi = 0.4584 rad per recorded
frame instead of 3.6. OndselSolver's angular driver held only the sine
of the angle, so from the previous frame the solver took the root half a
turn short. The records kept OndselSolver's frames as they were, and the
Reference was checked against OndselSolver at a tenth of the step
instead -- until sec 6.16 fixed the driver in OndselSolver and the four
simulation records were taken again.

### 6.15 As built: the Chrono backend (2026-10-23)

Sec 6.3 step 10's second backend, in three commits: the build option
(`3fe6057c93`), the backend and its gtests (`dda9ce366d`), the gate and
the selection test (`3e4d4dbdf6`). `App/Solver/ChronoBackend.{h,cpp}`,
registered as "Chrono" after the Reference when built, so the
`SolverBackend` property, the preference and `getAvailableSolvers()` name
it. Kinematics only (static solves, drag, simulation); dynamics, and the
out-of-process form of sec 4.7, are not attempted.

**Build.** `BUILD_ASSEMBLY_CHRONO` (off by default, in
`InitializeFreeCADBuildOptions.cmake`) builds the core library of the
`src/3rdParty/chrono` submodule (Thundereal/chrono, release 10.0.0) with
Chrono's own CMake, as an external project (`src/3rdParty/ChronoCore.cmake`):
Chrono's lists address their tree through `CMAKE_SOURCE_DIR`, so they
cannot be `add_subdirectory()`'d. Only `Chrono_core`, static, with demos,
tests, OpenMP, SIMD, CUDA, MPI, Thrust and HDF5 off, with this tree's
compiler and flags; `CHRONO_BUILD_JOBS` caps the nested build (6 here).
About 500 objects, half an hour at 6 jobs; the library is 1.3 GB with its
debug information, and Chrono's first configure copies its 750 MB `data`
directory into the nested tree (a cost of using its top-level CMakeLists
unchanged). The imported target `ChronoCore` carries the library;
Chrono's include directories and `CH_STATIC` go to `ChronoBackend.cpp`
alone (its include root has a directory named `chrono`, which must not
shadow the standard header elsewhere), and `ASSEMBLY_WITH_CHRONO` guards
the registration. `ChronoBackend.h` includes no Chrono header (the session
keeps its system behind a pointer). With the option off the module builds
as before and the gtests run 38; verified both ways on this tree.

**The model.** A `ChSystemNSC` with `ChSolverSparseQR` and no gravity or
collision; a `ChBody` per body (mass 1, inertia 1/4, below); a body a
`Ground` pins `SetFixed()` where its first Ground holds it, as the
Reference pins; a fixed world body for frames on no body. Chrono's links
measure frame 1 in frame 2 and the neutral joints J in I, so frame J is on
Chrono's body 1, frame I on body 2.

| neutral | Chrono | equations |
|---|---|---|
| Fixed | `ChLinkMateFix` | 6 |
| Revolute / Cylindrical / Prismatic / Spherical | `ChLinkMateRevolute` / `-Cylindrical` / `-Prismatic` / `-Spherical` | 5 / 4 / 5 / 3 |
| Planar(v) | `ChLinkMatePlanar`, `SetDistance(v)` | 3 |
| ParallelZ | `ChLinkMateParallel` | 2 |
| PerpendicularZ | `ChLinkMateOrthogonal` | 1 |
| AngleZ(pi/2) | `ChLinkMateOrthogonal` | 1 |
| PointInPlane(v) | `ChLinkMateDistanceZ`, `SetDistance(v)` | 1 |
| LineInPlane(v) | `ChLinkMateDistanceZ` + `ChLinkMateOrthogonal` | 2 |
| PointOnLine | `ChLinkMateGeneric(x, y)` | 2 |
| PointPointDistance(v) | `ChLinkDistance` | 1 |
| Gear (r1, r2 > 0, parallel shafts, not coaxial) | `ChLinkLockGear`, ratio r2 / r1, phase enforced from the start | 1 |
| RotationZ / TranslationZ driver | `ChLinkMotorRotationAngle` / `ChLinkMotorLinearPosition`, spindle / guide free | 1 |
| limit bound, while active | `ChLinkMateDistanceZ` / a constant `ChLinkMotorRotationAngle` | 1 |

`ChLinkLockGear` measures each gear's turn from the line between the two
shafts, which is OndselSolver's `OrbitAngleZIeJe`: its relation a1 + a2 /
tau = phase is OndselSolver's orbit(J, I) + ratio orbit(I, J) = c with
tau = 1 / ratio. Its phase correction moves its markers at each update and
is clamped to 45 degrees, so it holds at position level once within that
(`gearFollowsADrag`; the gate's gear drag and driven gear).

**Refused**, each with a reason (`Chrono::refusal()`; the gaps sec 8 of
the audit expected, and three it did not). *Since sec 6.17 none of these is
refused: a link of our own, `NeutralLink`, holds the neutral equations for
each; only a gear without a ratio (second radius zero) and a coaxial gear
without a carrier frame are refused, as the Reference fails them.*

| kind | why |
|---|---|
| AngleZ other than a right angle | no link holds an angle between two axes but 90 degrees (`ChLinkMateOrthogonal`) or 0 (`ChLinkMateParallel`) |
| PointLineDistance | no link for a point's distance from a line (`ChLinkDistance` is point to point, `ChLinkMateDistanceZ` point to plane, `ChLinkRevoluteSpherical` a fixed radius in a fixed plane) |
| LineLineDistance | no link holds two lines parallel at a distance (`ChLinkMateCylindrical` holds them at 0) |
| Gear as a belt (a negative radius) | `ChLinkLockPulley` couples its pulleys at velocity level only (its phase correction is a TODO in Chrono 10), and `ChLinkLockGear`'s epicyclic form divides by zero for equal radii |
| Gear between coaxial frames, or non-parallel shafts | no line between the shafts to measure from; `ChLinkLockGear` takes non-parallel shafts for a bevel gear (refused at load) |
| CoaxialGear | no carrier frame |
| RackPinion | `ChLinkMateRackPinion` recomputes its contact frames at every update and holds the rack against the pinion at velocity level only: a position solve does not couple them |
| Screw | `ChLinkLockScrew` is a whole screw pair (five equations, the axes held together); the neutral Screw is the coupling alone, one equation |

Both distances are exactly expressible by composing links with an
auxiliary body (a revolute about I's axis carrying a point at radius v,
and a point-on-line or cylindrical to it); that is a composition of the
adapter, not a Chrono link, and was not done. A model with a refused joint
loads (`load` is `Ok`), and every solve, drag step and simulation answers
`Unsupported` with "the Chrono backend cannot solve this assembly: the
joint Doc#Joint (PointLineDistance): Chrono has no link for ...", which
`AssemblyObject::solve` prints and answers -1; `analyse()` lists the
joints in `Diagnostics::unsupported` and knows no DoF. `Capabilities::joints`
is the 13 kinds of the first table (AngleZ not among them, though a right
angle is accepted; every kind since sec 6.17). Not skip-and-say: a solve
that ignored the joint would report a pose that does not satisfy it.

**Branches.** OndselSolver writes "axes parallel" as two direction cosines,
which hold zJ = -zI as well as zJ = zI, and a fixed rotation as three, which
hold the identity and the half turns about x, y and z. Chrono's mates hold
components of the relative quaternion, which only the identity (or a turn
about z) zeroes. So when a link is made, frame J is turned onto the
solution the start is nearest: a half turn about x when zJ starts against
zI (Revolute, Cylindrical, Planar, ParallelZ), the nearest of the four for
Fixed and Prismatic. The joint is unchanged; Chrono holds the branch
OndselSolver would converge to from there
(`keepsTheBranchTheStartIsNearest`). Drivers and limits on such a joint
take the same half turn about x, which keeps xJ and so thetaZ.

**Solve** -- the pose closest to the target (the start for a static solve,
the drag targets for a drag step) in the Reference's metric |dx|^2 +
|dtheta|^2 / 4 per body, subject to Chrono's constraint equations. Chrono's
own `ChAssemblyAnalysis` is a Newton iteration whose step is the KKT solve
[M Cq'; Cq 0] [dx; -l] = [0; -C], the step of least M-norm onto the
linearised constraints; with mass 1 and inertia 1/4, M is that metric. The
backend runs the same iteration itself, through `ChSystem::
StateSolveCorrection` (Chrono's KKT assembly and solver, its state
increments, its constraint residuals and Jacobians), in three parts:

- *restoration*: the Newton iteration with backtracking on |C| until |C| is
  below 1e-11 (1e-9 accepted when rounding stalls), in a metric where a turn
  weighs as much as moving the body's farthest frame by as much, and no body
  turned more than a radian per step. Without that a box 50 mm from the
  plane its vertex must touch turned through radians (the gate's PointPlane,
  PointCurve, PointPoint and PointSphere failed so; the Reference met the
  same, sec 6.13);
- *pull*: the KKT solve with right-hand side [M (target - x); -C], whose
  step goes to the point of the linearised constraints closest to the
  target, restored and taken while it brings the poses closer;
- after six rounds, a *reduced Newton step*: the null space Z of Chrono's
  Jacobian (column-pivoted QR), multipliers making the gradient of the
  Lagrangian normal to it, its Hessian on Z by central differences of that
  gradient with Chrono's Jacobian at the perturbed states, and the step Z
  (Z' H Z)^-1 Z' g when Z' H Z is positive definite (up to 120 freedoms).
  The plain step zigzags on a curved constraint: on the far PointInPlane
  case it crawled for 200 rounds and stopped 0.007 % farther than the
  Reference; with the Newton step both agree (`farStartWithManyGroundedBodies`).

Limits are an active set: a bound a solution violates gets a Chrono link
holding it there and the solve repeats (up to eight rounds); every bound is
released after the solve. In static solves too, as the Reference.

**Drag.** As the Reference: the dragged bodies pulled to their targets with
their drag weights as Chrono masses, the others held where they are with
weight 1; a step that fails is retried half way, then the poses go back and
it answers `NotConverged`. `endDrag` is a static solve from the dragged
state. `update` takes drag weights.

**Drivers and simulation.** Each driver is a motor whose function is
`FormulaChFunction`, a `ChFunction` whose `GetVal`, `GetDer` and `GetDer2`
are the neutral evaluator's value, rate and acceleration
(`Formula::evaluator()`), never Chrono's finite differences (`GetDer3` is
NaN: the evaluator has no third derivative; nothing kinematic asks for it).
`motionFunctionIsTheEvaluator` compares them bit for bit.
`ChLinkMotorRotationAngle` holds the angle modulo a full turn (the quaternion
component sin((theta - f) / 2)), not modulo half a turn as OndselSolver's
sine did before sec 6.16, so a driven body never lands half a turn off; to
count the turns for what it drives (a gear), each output interval is split
so that no
driven angle moves more than a quarter radian per step (divided by the
largest gear ratio), and each step starts from the pose predicted by
Chrono's velocities: the KKT solve of Cq v = -Ct, whose Ct are the motors'
exact rates (`GetDer`). Each step is then solved closest to the previous
one. Outside a formula's domain the run stops with `Failed` (sec 4.6.1).
The motors exist during `simulate` only.

**Diagnostics.** Chrono's constraint Jacobian (`GetConstraintJacobianMatrix`)
at the current poses, each row owned through its link's `GetOffset_L()`;
the rank by column-pivoted QR (rows scaled, 1e-8), which rows depend on the
ones before them by Gram-Schmidt in OndselSolver's order (its categories --
essential, displacement (x, y, z rows, distances), perpendicular (rotation
rows) -- then model order), conflicting when a dependent row is
unsatisfied by more than 1e-6: the Reference's analysis on Chrono's
equations, in `OndselSession`'s DoF convention. A joint whose frames are on
one rigid thing gets no link and counts as all dependent, conflicting when
the checker finds it unsatisfied.

Where it cannot match OndselSolver's convention:

- *Conflicting joints.* OndselSolver (and the Reference) set equations
  dependent at the start aside and solve the rest, answering `Ok` with the
  conflict reported. Chrono's KKT solve takes the inconsistent system in
  the least-squares sense and the Newton iteration does not converge: the
  solve answers `NotConverged` and leaves the bodies where they were, and
  `analyse()` there (which `AssemblyObject` calls after a failed solve too)
  names the conflict, the same joints the Reference names
  (`rankOnHandCountedModels`). Chrono's own `RemoveRedundantConstraints`
  could deactivate rows first, but picks them by its QR pivoting, not
  OndselSolver's order; not used.
- *Equations.* Chrono's rotation rows are quaternion components, OndselSolver's
  direction cosines: the same counts and ranks where the constraints are
  regular (every gate case), different singular sets elsewhere.
- *Limits* apply in static solves (OndselSolver applies them in drag steps
  only), and a rotation bound is held modulo a turn, so an angle more than
  a turn beyond its bound is held a turn away.
- *A refused joint* fails the whole solve, where OndselSolver solves.

**Capabilities**: the 13 kinds, limits, drivers, drag, kinematics,
redundancy, conflicts; not dynamics, warm update, per-body DoF. Native
format `chrono-json`: `exportNative` (so `exportAsASMT` with this backend)
writes Chrono's own JSON archive of the system (`ChArchiveOutJSON`).

**Gate** (`TestSolverGate*Chrono`, the 37 generated tests, against the
OndselSolver records by the oracle's rules of sec 6.13). New
`BACKEND_REFUSED`: a case a backend refuses, with the reason, asserted as
recorded -- OndselSolver's record solved it; the backend's solve returns -1
and moves nothing, a drag takes no step, a simulation fails -- and
`TestSolverGateOrder` expects the refusal in both orders. Result, all 72
records:

- **Refused: 31** (none since sec 6.17). The 23 Distance sub-types and role
  mappings the builder maps to PointLineDistance (CylinderSphere,
  TorusSphere, PointCylinder, PointLine, LineSphere, PointTorus twice,
  ConeSphere, PointCone, CurveSphere) or LineLineDistance (LineLine,
  LineCircle, CircleCircle, CylinderCylinder, CylinderTorus, LineCylinder,
  CylinderCone, ConeCone, ConeTorus, LineCone, LineTorus, CurveCylinder,
  CurveCone, CurveTorus, and the B-spline CylinderCylinder and LineCircle);
  Angle 30; Belt; the coaxial gear; RackPinion; Screw.
- **Converged** in the other 41, including PointPoint at d = 0, where
  OndselSolver does not (`BACKEND_DIFFERENCES`, as for the Reference).
  Every joint satisfied to 1.1e-11 by the neutral checker after every solve
  and drag step.
- **Fully constrained** (Fixed): placements 9e-16 from the record.
- **Closest to the start**: of the 38 cases with a free DoF and a record
  without a conflict, neither moved anything in 9, Chrono's solution is as
  close as OndselSolver's (within 0.1 %) in 25 and closer in 4 (PointPoint
  2288 against 2899, PointSphere 2351 against 4159, Perpendicular 0.350
  against 0.363, the rigid-group simulation's solve); never farther.
- **DoF and redundancy** equal in all but `test_drag_bundles_fixed`, the
  Reference's own disagreement: Chrono satisfies the Fixed and the Revolute
  OndselSolver leaves conflicting, DoF 1, nothing redundant
  (`BACKEND_DIFFERENCES`). LineTorus, the Reference's other, is refused.
- **Drag:** the same steps taken in all ten drag cases it solves (the
  Belt's is refused), every joint satisfied after each.
- **Simulation:** the four cases were `BACKEND_SIMULATION`'s (OndselSolver's
  records half a turn off); against OndselSolver at a tenth of the output
  step Chrono's frames agreed to **1.6e-12** (gears), 2.5e-12 (rigid group),
  4.5e-13 (the others). The gear driven at 9 rad per output step follows
  with its turns counted. Since sec 6.16 they are compared with the
  re-taken records directly: 1.7e-11 (gears), 2.6e-13 (rigid group),
  2.3e-13 (the others).

**Tests.** Assembly gtest 51/51 (38 + 13 `ChronoBackend`; 38 with the
option off); ctest 950/951 in the parallel run (the
`TransactionMeasureTest` flake, which passes alone; 938 + 13); the Assembly
suite 254 run, 1 skipped (216 + 37 generated gate tests +
`TestSolverBackend.test_chrono_backend`), no failures.

**Bench** (`scripts/assembly-bench/bench.py` through the preference, the
cases of sec 6.6 and 6.13, the three backends run one after the other on
this build; seconds, Chrono / Ondsel / Reference, one run each):

| case | solve_1 | preDrag | doDragStep (median of 10) | postDrag |
|---|---|---|---|---|
| star 49 | 0.047 / 0.091 / 0.055 | 0.028 / 0.072 / 0.032 | 0.0069 / 0.019 / 0.0055 | 0.0019 / 0.020 / 0.0036 |
| chain 49 | 0.057 / 0.61 / 0.079 | 0.033 / 0.072 / 0.033 | 0.018 / 0.027 / 1.42 | 0.0040 / 0.035 / 0.0036 |
| star 199 | 1.64 / 0.51 / 2.42 | 0.35 / 0.30 / 0.43 | 0.20 / 0.10 / 0.20 | 0.091 / 0.16 / 0.14 |
| chain 99 | 0.33 / 14.6 / 0.32 | 0.11 / 1.55 / 0.093 | 0.086 / 1.16 / 14.4 | 0.024 / 0.89 / 0.022 |

Chrono's sparse KKT solves beat OndselSolver everywhere but the star at
199 (a hub with 199 links: its static solve three times slower, its drag
step twice; every Newton and pull step assembles and factors the whole KKT
system, which was not profiled further), and the Reference's dense algebra
on the larger models. A
chain's drag step, which costs the Reference its finite-difference
Hessian per body, stays at tens of milliseconds. The JCS origins after a
solve are within 7e-13 mm (chain 99) and 2.3e-12 mm (star 199), as the
Reference's; OndselSolver's within 1e-14.

**Not done:** dynamics and the out-of-process session; per-body DoF,
`overDefinedGroups`, warm updates beyond drag weights; the refused kinds
(the auxiliary-body compositions above would give PointLineDistance and
LineLineDistance exactly; a position-level rack, belt and screw coupling
would need links of our own -- done for all of them in sec 6.17); setting
conflicting equations aside as OndselSolver does.

### 6.16 As built: OndselSolver's angular motion counts its turns (2026-10-23)

What sec 6.13 found and sec 6.14 noted, fixed in OndselSolver itself
(submodule `bb86857`, superproject `86245287e2` and `e54953e909`).

**Cause.** `ZRotation::initializeGlobally` (`ZRotation.cpp:31` before the
fix) held a RotationalMotion with one equation, the direction cosine of
I's y and J's x, frame I being the motion's `EndFrameqct` turned by the
commanded angle: sin(theta - phi), zero at phi and at phi + pi.
`FullMotion` (a GeneralMotion, which `makeMbdJoint` builds for a
Cylindrical driven both ways) had the same cosine as its fourth equation
(`FullMotion.cpp:36`), which with the other two cosines also holds the
half turn about z. A kinematic run steps straight to the next output time
(`BasicQuasiIntegrator::selectStepSize`) and solves from the previous
pose, with no prediction (`PartFrame::prePosKine`), so a command that
moved more than a quarter turn took the body to the nearer root, half a
turn off.

**Fix.**

- `AngleZIeqctJeqc`: the angle of J's x about I's z for the moving frame
  I, atan2 of the two direction cosines (I's x and y against J's x), its
  whole turns counted across iterations as `AngleZIecJec` counts them for
  gears and limits. Across time steps the count follows frame I's own
  turn: when the command moves by d, the value the count stays nearest to
  moves by -d, so the value is the bodies' turn less the commanded angle,
  with turns. It starts at the turn nearest the command, (-pi, pi]. Its
  time partials (velocity, acceleration solves) are those of atan2 through
  `DirectionCosineIeqctJeqc`'s.
- `AngleZConstraintIqctJqc`: that angle = 0, essential, as the cosine was.
  `ZRotation`'s equation and `FullMotion`'s (I's y, J's x) cosine are now
  this. One equation each as before; where it holds its gradient is the
  cosine's, so the rank, the redundancy and the DoF are the same (the 68
  non-simulation records are unchanged byte for byte, the simulation
  records' solves and statuses too).
- `EndFrameqct::turnAboutZ` (and `EndFrameqct2`'s, for rotation orders):
  the frame's turn about z from its angle blocks, NaN when it is not a
  turn about z alone (the count then follows the bodies only).
- `PrescribedMotion` suggests integrator steps (`suggestSmallerOrAccept
  DynStepSize`, the hook `PartFrame` uses for its rotation limit) that
  split the step to the next output time into equal parts with at most
  pi/4 of frame turn each. The equation is right whatever the step; this
  bounds the Newton iteration. Each iteration turns a body less than half
  a turn (a linearised Euler-parameter step), so without it 18 rad per
  output step stopped 3e-8 off when `isConvergedToNumericalLimit` relaxed
  its tolerance with the iteration count, and 27 rad met a singular
  matrix. With the equation alone, steps of 0.36, 1.8, 3.6 and 9 rad per
  frame already followed the formula to 1e-9.

**Couplings.** Gear, belt, rack and pinion, and screw measure their angles
with `AngleZIecJec` / `OrbitAngleZIecJec`, which count turns per
iteration, so they follow the driven body once it goes through the
commanded angle: no change needed. Checked with a gear and a belt (B at
-/+45*time when A is driven at 90*time), and a rack and a screw, whose
slides (7 * 90*time and -3 * 90*time / (2 pi)) show the accumulated turns
directly, at output steps up to 27 rad per frame.

**Evidence.**

- OndselSolver's `tests/test.cpp`, `rotationalMotionCountsTurns`: a gear
  pair driven at 90*time, output steps 0.01 to 0.3 s, both gears at the
  formula to 1e-9 at every frame (built standalone with
  `ONDSELSOLVER_BUILD_TESTS`).
- gtest `OndselAdapter.drivenAnglesFollowTheFormula` (a revolute at
  36*time, a cylindrical at 90*time and 2*sin(time); steps 0.01 to 0.3 s;
  largest error 7.1e-15 rad at every step), `couplingsFollowADrivenTurn`;
  `ReferenceBackend.drivenAnglesFollowTheFormula` now asserts
  OndselSolver's frames follow the formula instead of its half turns.
- The four simulation records, re-taken; only `simulation/frames` changed
  in each. Against the formulas (turn about the joint axis, axis points,
  slides, the rigid group's other member): `test_simulation` and its
  reverse playback were pi off at every other frame, now 2.1e-14 rad;
  `test_simulation_gears` pi off at eight of ten frames, now 1.6e-11 (B's
  phase from the start solve); `test_simulation_rigid_group` pi off at
  every other frame, now 1.1e-14.
- `BACKEND_SIMULATION` is empty: the Reference's frames are within
  3.5e-10 of the records, Chrono's within 1.7e-11.

The ASMT text is unchanged (the motion's text is what it writes).

**Tests.** Assembly gtest 53/53 (51 + 2); ctest 953/953 in the parallel
run (951 + 2); the Assembly suite 254 run, 1 skipped, no failures.

**Not done:** a GeneralMotion turning about x or y as well keeps the
cosine branches of those axes (the half turns about x and y), as every
OndselSolver joint holding axes parallel does (sec 6.15, Branches); the
count of its z turn then follows the bodies only.

### 6.17 As built: Chrono holds the neutral equations (2026-10-23)

The 31 gate records the Chrono backend refused (sec 6.15) had one cause:
no Chrono link for PointLineDistance, LineLineDistance, AngleZ other than a
right angle, a belt, a gear between coaxial or non-parallel shafts,
CoaxialGear, RackPinion and Screw. Decided (user): one link of our own
that evaluates the neutral layer's equations -- the Reference's, which are
OndselSolver's forms with an analytic Jacobian -- rather than a
composition per kind. In three commits: the shared equations
(`4845e7b5dc`), the link and its gtests (`2eea6fb09e`), the gate
(`23f0a02332`).

**The shared equations.** `App/Solver/JointEquations.{h,cpp}`: the
Reference's derivative machinery (a scalar or vector with its gradient per
body, at most three bodies), the frame states, and one function for every
`JointKind`'s rows (`Equations::jointRows`), moved unchanged out of
`ReferenceBackend.cpp`. What a joint keeps between evaluations is a
`JointState`: the coaxial test of a gear, a coupler's constant (the
relation it has at the start) and its angles with their turns counted;
`startState()` gives it at the start poses (OndselSolver's first reading,
then the constant). The Reference evaluates through it; its 13 gtests and
its gate classes pass unchanged, with no new entry.

**NeutralLink** (`ChronoBackend.cpp`), a `ChLinkBase` holding one joint:

- *Bodies.* Frames I, J and, for a coaxial gear, the carrier K, each on a
  `ChBody` or constant (the world's frames, and a body Chrono holds fixed:
  its pose does not move and its variables are inactive). The distinct
  moving bodies are the link's variables, one `ChConstraintNgeneric` per
  equation over them (one to three bodies; Chrono's own links stop at two).
  The row count is fixed at construction: the equations at the start.
- *C and Cq.* `Update()` builds the frame states from the bodies' poses
  and calls `Equations::jointRows`: C is the raw residuals, Cq the
  gradients converted to Chrono's coordinates. The Reference
  differentiates with respect to each body's position and the spatial
  rotation vector applied on the left (q' = exp(dtheta) q); Chrono's
  velocity coordinates are the position and the angular velocity in the
  body's own frame, and its increment turns q into q exp(dw)
  (`ChBody::IntStateIncrement`). So dtheta = R dw, and a row's rotation
  block g becomes g R: Cq = [g_x, g_theta R]. Positions are the same
  coordinate (a `ChBody`'s frame is its centre of mass, which is the
  body's placement here). `IntLoadConstraint_C`, `IntToDescriptor`,
  `InjectConstraints`, `ConstraintsBiLoad_C` and the rest are
  `ChLinkDistance`'s, over the N rows; `IntLoadConstraint_Ct` is zero (no
  equation depends on time).
- *Turns.* A coupler's angles are unwrapped against the angles last
  committed, as in the Reference. `Update()` only reads (it may run at a
  trial pose, a backtracking step or a finite-difference probe);
  `commit()` takes the reading at the current poses. The backend commits
  where the Reference does -- at every pose its restoration accepts and at
  a converged restoration -- and its saved states carry the committed
  angles (`Impl::State`), so a rejected step, a rejected pull and the
  reduced Newton step's probes leave the count where it was, a failed run
  restores it with the poses, and a kinematic run's predicted step starts
  from the last frame's. The kinematic run's step is shortened by a neutral
  gear's ratio as by a native one's.
- *Diagnostics.* Each row's category is the equation's (OndselSolver's),
  so the rank analysis of sec 6.15 orders them as the Reference does.
- *Export.* Registered in Chrono's class factory as `AssemblyNeutralLink`,
  so the JSON archive names it (`ArchiveOut` writes the kind, values and
  constant; nothing reads it back).

**Which link.** `Chrono::nativeLink()`: Chrono's own link wherever sec
6.15 had one (unchanged); `NeutralLink` for AngleZ other than a right
angle, PointLineDistance, LineLineDistance, CoaxialGear, RackPinion,
Screw, and a Gear unless both radii are positive and the shafts parallel
and not coaxial (ChLinkLockGear). `Chrono::refusal()` refuses only a gear
without a ratio (second radius zero); a coaxial gear without a carrier
frame is refused at load. The Reference fails both. `supportedKinds()` is
every kind.

**Checked.** gtest `neutralJacobianInChronosCoordinates`: for AngleZ,
both distances, a belt, a coaxial gear, a rack and a screw on free bodies
at random poses, Chrono's `GetConstraintJacobianMatrix` against central
differences of the link's constraint values under Chrono's own increments
(position, and q exp(h e_k)), and against the Reference's gradient turned
into the body frame; C equal to the Reference's residuals. Dropping the
conversion (Cq = [g_x, g_theta]) fails it (and
`eachKindSolvesFromAPerturbedStart`). `neutralCouplersFollowADrag`: belt,
coaxial gear, rack and pinion, screw, each relation held at position
level and the drag result the Reference's within 1e-7.
`neutralCouplersCountTheirTurns`: a pinion dragged through 16.2 rad in
0.9 rad steps moves its rack by 7 mm per radian of the whole angle; driven
at 90*time (9 rad per output step) it moves the rack 630 mm in a second,
frame for frame the Reference's. `eachKindSolvesFromAPerturbedStart` takes
AngleZ(0.5) and the two distances; `refusalsAndTheLinkEachKindGets`
replaces `refusedKindsSayWhy`.

**Gate** (`TestSolverGate*Chrono`, by the oracle's rules of sec 6.13).
`BACKEND_REFUSED` is empty; all 72 records:

- **Converged** in all 72; every joint satisfied to 2.4e-13 by the neutral
  checker after every solve and drag step in the 31 formerly refused
  (PointLine the largest).
- **DoF and redundancy** equal to OndselSolver's in all of the 31 but
  LineTorus, where Chrono now agrees with the Reference, not with
  OndselSolver: OndselSolver leaves the box 0.73 mm off and calls the joint
  conflicting, DoF 49; the NeutralLink satisfies it, DoF 45, nothing
  redundant -- the Reference's diagnosis, recorded in `BACKEND_DIFFERENCES`
  as for the Reference. With `test_drag_bundles_fixed` and PointPoint at
  d = 0 (sec 6.15) Chrono has three recorded disagreements, the Reference's
  three.
- **Closest to the start**, of the 30 with a free DoF and no conflict in
  the record: neither moved anything in 4 (the Belt, the coaxial gear, the
  rack, the screw: their starts satisfy the joints), Chrono as close as
  OndselSolver (within 0.1 %) in 14 and closer in 12 (PointLine 122.6
  against 292.5, CylinderCylinder and its B-spline form 854 against 2624,
  LineLine 443 against 733, PointCylinder 540 against 1388, LineCone 773
  against 1145, PointTorus (both) 2540 against 3181, LineCircle B-spline
  290 against 966; the Angle of 30 degrees by 0.3 %, LineSphere and
  CurveSphere by 0.2 %); never farther. In every one Chrono's distance is the
  Reference's to 1e-10 relative: the same equations, the same metric.
- **Drag:** the Belt's step taken, as OndselSolver's; every joint satisfied.
- `TestSolverGateOrderChrono` solves the PointLineDistance and
  LineLineDistance sub-types in both orders, the same result.

**Bench** (`scripts/assembly-bench/bench.py` with the Chrono backend,
the cases of sec 6.15; none has a joint the NeutralLink holds, so this
checks that the natively mapped models pay nothing). Seconds; before is
this build with `ChronoBackend.cpp` of `911aa17823`, after the median of
three runs:

| case | solve_1 | preDrag | doDragStep (median of 10) | postDrag |
|---|---|---|---|---|
| star 49 | 0.046 / 0.046 | 0.029 / 0.037 | 0.0067 / 0.0075 | 0.0023 / 0.0017 |
| chain 49 | 0.057 / 0.058 | 0.033 / 0.034 | 0.018 / 0.018 | 0.0061 / 0.0031 |
| star 199 | 1.67 / 1.62 | 0.35 / 0.38 | 0.19 / 0.19 | 0.095 / 0.064 |
| chain 99 | 0.36 / 0.35 | 0.10 / 0.099 | 0.091 / 0.088 | 0.024 / 0.025 |

Within the spread of the three after runs (star 49's preDrag 0.031 to
0.040, star 199's solve 1.54 to 1.92). What the native models do more: an
empty vector of committed angles gathered and scattered with each state.

**Tests.** Assembly gtest 56/56 (53 - `refusedKindsSayWhy` + 4: the
link-choice test, the Jacobian, the coupler drags, the turns; 40 with
`BUILD_ASSEMBLY_CHRONO` off, this tree reconfigured off, the module and
its tests built and run, then on again); ctest 956/956 (953 + 3; the
`TransactionMeasureTest` pair failed in the parallel run and passed
alone); the Assembly suite 254 run, 1 skipped, no failures (the same
tests: 31 of the generated gate tests now solve instead of asserting a
refusal).

**Native or neutral.** Not switched here, but the neutral equations would
be better than Chrono's link for:

- *Gear.* `ChLinkLockGear` holds its relation at position level only
  through a phase correction that moves its markers at each update and is
  clamped to 45 degrees per update (sec 6.15); the neutral Gear is exact at
  position level with its turns counted, as OndselSolver's, and needs no
  parallel-shaft test.
- *The joints holding axes parallel* (Revolute, Cylindrical, Planar,
  ParallelZ, Fixed, Prismatic, LineLineDistance's parallelism). Chrono's
  mates hold quaternion components, one branch, which is why sec 6.15 turns
  frame J onto the branch the start is nearest; the neutral direction
  cosines hold both, as OndselSolver does, and their rank behaves as
  OndselSolver's and the Reference's where the constraints are singular
  (sec 6.15, "Equations").

Against that, a native mate's rows are a few fixed matrix products, the
neutral link's a pass of the gradient machinery per row (allocations
included) at every update; on the bench's large models that would cost
time. Measure before switching. PointPointDistance (`ChLinkDistance` is
|r| - d with the same gradient), PerpendicularZ and the incidences
(`ChLinkMateOrthogonal`, `ChLinkMateDistanceZ`, `ChLinkMateGeneric`) gain
nothing.

**What dynamics would need** (not attempted: kinematics only, as the
backend). The link has C and Cq; Ct is zero. Chrono's velocity-level
time steppers (NSC, Euler implicit linearised) use exactly those, plus:

- *Reactions.* `GetReaction1/2` report zero; the multipliers are kept
  (`IntStateScatterReactions`) but not turned into a force and torque on
  the frames.
- *Acceleration-level terms.* An acceleration analysis or an
  acceleration-level stepper (HHT) needs `ConstraintsBiLoad_Qc`, Qc =
  -(dCq/dt) v, the equations' second derivatives along the velocity; the
  shared equations carry first derivatives only. (A driver's motor already
  gives its own `GetDer2`.)
- *Tangent stiffness* (`LoadKRMMatrices`, Kc = d(Cq' l)/dq) for implicit
  steppers that use it; optional.
- Real masses and inertias instead of the solve's metric (sec 6.15), and
  limits as unilateral constraints rather than an active set.

**Not done:** switching native kinds; the neutral link's speed
(allocations per update) was not tuned.

### 6.18 As built: the Cam joint, a point on a curve (2026-10-24)

The queued item (b) of `docs/AssemblyDynamics.md`: a kinematic cam, a new
neutral `JointKind::PointOnCurve` -- a point of body J held on a curve of
body I, two equations -- and a user-facing "Cam" joint on it, on all three
backends. In five commits: the submodule (`11abf1281f`, OndselSolver `d7bde2c`), the
neutral kind and its backends (`22729b3797`), the Cam joint (`ee8d033406`),
the builder hazard (`e684880330`), the gate (`47cea11335`).

**The neutral representation.** `JointKind::PointOnCurve` is appended to
the enum (after Screw, before `Count_`). `Model::curves` holds `Curve`
records (id, source, `std::shared_ptr<const CurveGeometry> geometry`), a
record of their own so that the later lift-off cam (`CurveContact`) can
reference the same curve; `Joint::curve` names the joint's, and
`Joint::value` is the roller's signed offset (0 for a point follower). The
curve is given in the coordinates of the joint's frame I.

`CurveGeometry` (`App/Solver/Curve.h`, no OCCT) is the small neutral
interface: `first()`, `last()`, `closed()`, `evaluate(u)` (the point and
its first two derivatives), `planeNormal()` (zero when the curve lies in
no single plane; a line: zero), `describe()` (one line of text), `same()`.
The one implementation, `OcctCurve` (`App/CamJoint.cpp`), holds the edge's
exact `Geom_Curve` -- moved into frame I's coordinates -- and evaluates it
with its own `D2`: no sampling, no conversion. It is closed when its ends
meet (`Precision::Confusion()`), planar when `BRepLib_FindSurface` finds a
plane through it, and describes itself as the B-spline `GeomConvert` makes
of it (degree, knots and multiplicities, poles and weights, 17 digits):
that text is what the native dumps carry (the ASMT `curve`, the Reference's
neutral text, Chrono's archive) and what `same()` compares, so a rebuilt
model with the same cam compares equal (`sameExceptDragWeights`). Nothing
reads the text back yet; a session in another process (sec 4.7) would
rebuild a B-spline from it, which is exact for every curve OCCT converts
(the conics are rational).

*Parametrisation.* The curve's own over [first, last]. A closed curve
repeats with the period last - first: its parameter is kept unwrapped --
it counts the turns a follower makes round a cam -- and wrapped only to
evaluate. An open curve is extended beyond its ends along its end tangents
(a straight line, second derivative zero), so the closest point is defined
everywhere and a follower driven past an end goes on along the tangent
instead of jumping or stopping.

*The projector* (`projectOnCurve`): the closest point by descent from a
given parameter -- Newton on (x - C(u)).C'(u) = 0 where it is convex,
the largest step downhill where it is not (past a centre of curvature),
backtracked until |x - C|^2 decreases, no step longer than an eighth of the
parameter range. It finds the closest point in the basin of the parameter
it starts from and never jumps across the curve to a closer stretch
(`neverJumpsAcrossTheCurve`: inside a long ellipse, a point nearer the top
stays on the bottom when it starts there). `projectOnCurveGlobally` (128
samples, then the descent) is used only where there is nothing to follow:
at a joint's start, in the builder, and in the checker.

**The equations.** At the closest point C(u) of the curve to x (frame J's
origin in frame I's axes), with T the unit tangent, N1 the unit part of a
reference direction a normal to T, N2 = T x N1, e = x - C(u):

    r1 = e . N1,    r2 = e . N2 - offset.

For a planar curve a is the plane normal, so N1 is the plane normal (r1 the
height above the plane) and N2 the normal in the plane (r2 the distance
from the curve, or from its offset). For a 3D curve a is the direction the
curve's sampled tangents point along least (the eigenvector of the smallest
eigenvalue of sum(t t')); a curve whose tangents come within 2.6 degrees
of every direction (|cos| > 0.999) has no smooth frame and is refused. Both
rows are displacements (OndselSolver's category), the natural error is the
row itself.

*The curve parameter: a projection, not an unknown.* u is a function of the
poses, the stationary point of |x - C(u)|, and the gradient includes the
implicit-function derivative du/dx = C' / D, D = C'.C' - e.C'':

    grad r_k = N_k + ((e - offset N2) . N_k' / D) C'

(N_k' the frame's derivative along the curve, from C' and C''), exact
wherever the projection is. An internal unknown per joint was the
alternative; it would have added a variable that is not a body to every
backend (the Reference's and Chrono's variables are bodies, the NeutralLink
holds rows over bodies only), a third equation (x = C(u)) and an unknown to
the DoF count, and a new variable type to OndselSolver. The projection
keeps every backend's variables as they were and the joint at two rows. The
parameter is state like a coupler's turn count: `JointState::angles[0]`
(`jointAngleSlots(PointOnCurve)` is 1), set at the start by the global
search, followed from the last committed value at each evaluation, and
committed where the Reference and the NeutralLink commit angles (accepted
poses); a rejected step or a finite-difference probe leaves it where it
was, a failed run restores it with the poses.

The Hessian, which OndselSolver uses (its Lagrangian in the static solve,
its accelerations), is the exact one where the equations hold:

    H_k = (N_k' C'^T + C' N_k'^T - (O'.N_k') C' C'^T / D) / D,

O' = C' + offset N2' the offset curve's derivative; off the curve it is
off by terms of the residual.

*Closed curves turning many times.* A follower round a closed cam crosses
the seam once a turn; the unwrapped parameter goes on past `last`, so the
projection never sees a seam. A curve that is only C1 at its seam (OCCT's
periodic interpolation leaves a knot of multiplicity 2 there) has its
curvature jump, and with it the gradient off the curve (e != 0): the
equations stay continuous with a continuous gradient on the curve, which is
all a Newton iteration needs; the gtests use a uniform periodic B-spline (C2
at its seam) where they compare with differences.

**The roller: an offset in the plane, by projection.** A roller of radius r
has its centre held at distance r from the cam curve along the in-plane
normal at the centre's closest point (r2 = e . N2 - offset, offset = +-r),
i.e. on the curve's offset by r, in the curve's plane. The offset curve is
not built (`Geom_OffsetCurve`, or `BRepOffsetAPI` on a planar wire): the
closest point of a point to the offset curve is the one on the curve itself
(the offset's normals are the curve's), so the same projector serves, no
third derivative is needed (`Geom_OffsetCurve::D2` needs the basis curve's
`D3`), and the lift-off cam's gap (`AssemblyDynamics` sec 7.4: g =
distance - followerRadius) is the same quantity. This is the "circle
tangent to the curve" formulation, and it is the offset curve's as long as
the roller is smaller than the curve's radius of curvature on its side;
where it is not (an undercut cam), the offset curve has cusps, the roller
cannot follow either way, and the projection's D is kept positive (no
message). The side is the one the centre starts on (the sign of e . N2 at
the global closest point, in the builder), so the joint's offset is signed.
A roller needs a planar curve; a 3D cam with a roller (a barrel cam) is
refused. *Flat-faced followers are not done*: a face tangent to the curve is
a different equation (the face's plane tangent to the curve, a contact on
the envelope), not a point on an offset.

**Backends.**

- *Reference.* Through `JointEquations` (`curveRows`: x as three scalars
  with their gradients, the rows of `pointOnCurveRows`, the gradient by the
  chain rule through x). `System::curveOf()`, the start state from the
  global search; a model whose PointOnCurve has no curve fails to load
  ("a point-on-curve joint has no curve"). The checker (`jointResiduals`)
  seeds every curve parameter by the global search at the poses it measures,
  since it measures poses it did not follow the motion to. `exportNative`
  writes `curve <id> "<source>" <description>` lines and the joint's curve.
- *Chrono.* The NeutralLink, with the curve (`nativeLink()` false; the link
  is refused at load when the curve is missing). Not `ChLinkLockPointSpline`:
  it finds its nearest point by sampling the whole `ChLine` at every update
  (`FindNearestLinePoint`: it can jump to a closer stretch of a cam), to
  1e-6, takes the tangent and the curvature by finite differences (a step of
  1e-4 of the parameter), and holds the point by moving its marker onto the
  curve at each update, as `ChLinkLockGear` holds its phase (sec 6.15); it
  would also need the curve converted to a `ChLineBspline`. The NeutralLink
  evaluates the exact curve, follows the closest point and has the analytic
  Jacobian (`neutralPointOnCurveInChronosCoordinates`: Chrono's Jacobian
  against differences under Chrono's own increments, its rows the
  Reference's).
- *OndselSolver: done in our fork* (submodule `d7bde2c`). A
  `PointOnCurveFunction` hook evaluated by the host -- the "ExternalFunction
  for constraints" route, not an internal parameter -- in three classes:
  `PointOnCurveConstraintIJ` (one equation G = f_row(x), x = rIeJeIe as
  three `DispCompIeqcJeqcIe`, every first and second partial by the chain
  rule through x, as `DistancexyConstraintIqcJqc` does it for x^2 + y^2),
  `PointOnCurveJoint` (the two equations, one function), and
  `ASMTPointOnCurveJoint` (`setCurve(text, function)`; the ASMT file records
  the description under `curve`; a length unit other than 1 is scaled into
  the host's units). The adapter (`OndselBackend.cpp`, `curveFunction`)
  hands it the neutral rows; since OndselSolver has no accepted-pose event,
  the function follows the closest point from one call to the next (its
  Newton iterates and its kinematic run's steps follow each other) and
  searches the whole curve at its first call. An ASMT file with a cam reads
  back (`readJoints` knows the class) and refuses to run ("needs its curve
  evaluated by the host"). OndselSolver's own test,
  `pointOnCurveHostFunction` (a circle host function, the disk cam below at
  10*time, steps of 0.05 and 0.2 s), holds the analytic lift to 1e-9.

*DoF.* Two equations everywhere: a free body held on a curve keeps four
freedoms in OndselSolver's count and the Reference's
(`countsTwoEquations`, every curve of the gtests). A planar cam whose
follower is already held in the cam's plane -- the usual mechanism, a
follower on a slider -- has one equation dependent: all three backends
report one joint partially redundant (OndselSolver, by its own order, the
cam's revolute, and the Reference and Chrono, ordering as OndselSolver
does, the same), DoF 1 (the cam's turn). That is the 3D form of a planar
mechanism, as for two revolutes on one axis; a "curve in a plane" kind
(one equation, the profile surface) would avoid it and is not done.

**The document: footprint on upstream files.**

- `App/AssemblyUtils.h`: `JointType::Cam` appended (one line).
- `App/AssemblyObject.cpp`: `case JointType::Cam:` (PointOnCurve) in
  `makeMbdJointOfType`, and one call after the frames are set in
  `makeMbdJoint` (`addCamCurve`, `App/CamJoint.cpp`).
- `JointObject.py`: "Cam" appended to `JointTypes` and
  `TranslatedJointTypes`; `setJointType` extends a joint's own type list
  first when it lacks the new type (a joint from an older file lists twelve);
  `findPlacement` ignores the vertex for a Cam as for a Distance (a circle
  edge is framed at its centre: the roller's); `ensureUnconnectedIsSecondRef`
  does not swap a Cam's references (they have roles: Reference1 the cam,
  Reference2 the follower); the fork-owned `refusedDistancePick` and
  `showDistanceRefusal` hand a Cam pick to `JointCam.py`.
- New files: `JointCam.py` (the pick check and its message),
  `App/CamJoint.{h,cpp}` (the OCCT curve, `camJointRefusal`, `addCamCurve`),
  `App/Solver/Curve.{h,cpp}`; `AssemblyApp.getCamRefusal(joint)` in the
  fork-extended `AppAssemblyPy.cpp`.

Old files load unchanged: the enumeration keeps every older value, and no
property is added. The type is in the joint task's type list (no toolbar
command, no icon of its own: the generic joint icon).

*Picks.* Reference1, the cam: an edge, any curve. Reference2, the follower:
a vertex, or a circular edge (a roller: its centre, its radius). A roller
needs a planar cam curve. The task asks after each pick and refuses the
pick with the reason ("A cam between these elements is not supported: the
face Face1 of Cam is not an edge: the cam is a curve of the cam part (an
edge). Select other elements."). A refused Cam joint already in a document
is left out of the solve with a warning.

*The builder* (`addCamCurve`): the cam edge through the reference's path
from the moving part (`Part::Feature::getTopoShape`), into the part's frame
(the part's global placement, as `handleOneSideOfJoint` takes a JCS there),
the bundle's offset, then frame I's: the curve in frame I's coordinates.
The follower point is frame J's origin (`Placement2`: the vertex, or the
circle's centre); the roller's side from the start poses.

**The builder hazard** (`AssemblyDynamics` sec 1). `getMotionsFromSimulation`
read every child of a Simulation's Group as a motion when it had properties
named Joint, MotionType and Formula. It now takes a child whose Python
proxy's class is named `Motion` (CommandCreateSimulation's), skips one with
a proxy of another class whatever its properties, and reads one without a
proxy -- a script's (the gate's helpers make them so), or a Motion whose
module did not load (its proxy is None) -- by its properties, as before.
`TestSimulationMotions`: a Motion drives a revolute, a proxy-less motion a
slider, and a `ForceElement`-proxied child carrying the three properties
does not drive the third body. (Found on the way, not fixed:
`CommandCreateSimulation.Motion()` creates its Joint as
`App::PropertyXLinkSubHidden`, a type this fork does not have, so the
simulation task's "add motion" fails in this fork; the test gives the
proxy to a motion made with `App::PropertyXLinkSub`.)

**Tests.**

- gtest `PointOnCurve` (9): the curve factory (closed, planar, `same`,
  moved into a frame); `followsAClosedCurveForManyTurns` (a circle, steps of
  0.3 and 1.9 rad both ways for 12 turns: the parameter is the angle with
  its turns to 1e-12; a periodic B-spline for 10 turns: the parameter grows
  by a period a turn, the closest point the global one to 1e-10);
  `neverJumpsAcrossTheCurve`; `openCurveExtendsAlongItsTangents`;
  `residualAndJacobianByDifferences` (a line, a circle with and without a
  roller and across its seam, planar and 3D B-splines, a periodic B-spline
  across its seam and with a roller; cam and follower both free; on and
  0.7 mm off the curve: the rows as constructed, the analytic Jacobian
  against central differences to 1e-6 relative); `hessianByDifferences`
  (where the equations hold, against differences of the gradient);
  `countsTwoEquations` (Reference and OndselSolver: DoF 6 + 4, the follower
  moved the 0.4 mm to the curve and no farther); `camSolvesOnEveryBackend`
  and `camFollowsTheDriverForManyTurns` (the disk cam below, a point and a
  roller, every registered backend: the static solve on the analytic
  relation to 1e-9, DoF 6 + 1; 4 s at 10 rad/s with 0.2 s output steps, the
  lift analytic to 1e-9 at every frame). Plus
  `ChronoBackend.neutralPointOnCurveInChronosCoordinates`, and the existing
  link-choice test's PointOnCurve lines.
- Python `TestCamJoint` (6): the type appended (index 13; Belt still 12); an
  older joint takes the Cam type; the picks and their refusals (a face, a
  seam line, a helix with a roller) and `JointCam.pickRefusal` with one
  pick; the static solve on every backend from 0.8 mm off (lift to 1e-9
  against the cam's angle after the solve, every joint satisfied, DoF 1);
  the disk cam simulation -- an eccentric circle (radius 20, centre 6 from
  its axis) on a revolute driven by an Angular motion at 10*time for 4 s
  (more than six turns) with 0.2 s output steps (2 rad, more than a quarter
  turn), a point follower and a roller of radius 3 on a slider along y
  through the axis, whose height is 6 sin(theta) + sqrt((20 + r)^2 -
  36 cos(theta)^2) -- and a dwell-rise-dwell profile (a periodic B-spline
  through 72 points of a cycloidal rise of 6 mm on a base radius of 18) whose
  lift is checked against the B-spline's own evaluation (the point of the
  curve on the follower's line, by Newton on the curve's value and
  derivative). Largest lift error over every frame:

  | case | OndselSolver | Reference | Chrono |
  |---|---|---|---|
  | disk cam, point | 8.5e-14 | 7.5e-11 | 9.4e-12 |
  | disk cam, roller | 8.9e-14 | 7.2e-11 | 4.3e-12 |
  | B-spline cam | 5.4e-13 | 1.2e-10 | 5.3e-12 |

  `TestSimulationMotions` (1), above.
- Gate (`TestSolverGateCam`, three new records, recorded with OndselSolver
  under `ASM_SOLVER_GATE_RECORD=1` for these cases only; the 72 earlier
  records are unchanged, compared on the parsed file and in the text diff,
  which adds lines only): a point follower starting 0.8 mm off the disk cam,
  with a simulation (10*time, 2 s, 0.2 s steps); a roller starting 0.6 mm
  inside, with a drag of the cam through 30, 60 and 90 degrees; the
  B-spline profile 0.5 mm off, with a simulation. The generated
  `TestSolverGateCamReference` and `TestSolverGateCamChrono` agree with the
  records on every status (DoF 1, the revolute partially redundant, in all
  three), converge, are as close to the start (0.31795 against 0.31796,
  0.18189 against 0.18189, 4.806e-4 against 4.810e-4), take the drag steps,
  and play the simulation frames within 1.2e-10 (Reference) and 7.7e-12
  (Chrono) of the records. `BACKEND_DIFFERENCES` and `BACKEND_REFUSED` gain
  nothing.

Assembly gtest 66/66 (56 + 9 `PointOnCurve` + 1 `ChronoBackend`; 49 with
`BUILD_ASSEMBLY_CHRONO` off, this tree reconfigured off, the module and its
tests built and run, then on again); OndselSolver's own test passes; ctest
966 (956 + 10; the `TransactionMeasureTest` pair failed in the parallel run
and passed alone); the Assembly suite 270 run, 1 skipped, no failures (254 +
6 `TestCamJoint` + 1 `TestSimulationMotions` + 9 gate tests).

**Not done:** flat-faced followers; a cam profile made of several edges (a
wire: the joint takes one edge); a one-equation "point on the profile
surface" kind for planar mechanisms; reading a curve back from its
description (out of process); the lift-off cam of `AssemblyDynamics` sec 7.4,
which needs the same projector and a force element.

### 6.19 As built: a sparse LU for OndselSolver's Newton steps (2026-10-26)

Performance at scale (audit sec 8.2, Phase 2, F1). On a chain of boxes (the
bench's Fixed and Revolute joints alternating) a static solve took 3.3 s at
100 parts and 190 s at 200; 65-80 % of it in
`GESpMatParPv::forwardEliminateWithPivot`, a Gaussian elimination on rows
kept as `std::map`s, in the natural column order, filling in node by node;
most of the rest in `handleSingularMatrix`, its fallback for matrices it
took for singular.

**The solver** (`OndselSolver/SparseLUSolver.{h,cpp}`, the submodule):
`SystemNewtonRaphson::matrixSolverClassNew()` -- the static solve, the drag
steps and the kinematic position and acceleration solves -- now gives a
`SparseLUSolver`: Eigen's `SparseLU` with a COLAMD column order (a fill
reducing order from the matrix's pattern alone), the column order kept from
one step to the next while the pattern stays. The rows are conditioned as
`GESpMatParPvMarkoFast` conditions them (scaled to a largest magnitude of
1, entries below 4 eps dropped), then rows and columns are equilibrated
(Ruiz, four passes, factors that are powers of 2, so scaling rounds
nothing). The equilibration is not optional: the minimum movement rows
carry weights of 1e3 to 1e6 (`Part::fillqsuWeights`) against constraint
rows of order 1, so after the row scaling alone the multipliers' columns
are tiny and the pivots fall with the size of the model (row scaled, in
the LU's order: 8e-13 at 50 parts, 4e-15 at 200), where no tolerance tells
a regular matrix from a singular one. That is also why the old elimination
took long chains for singular and spent a fifth of its time in its fallback.
Equilibrated, the smallest pivots are 3e-9 at 50 parts, 1e-9 at 100 and
1.3e-10 at 200. It decides no singularity of its own (but an all zero row,
as `GESpMatParPvMarkoFast` does): a pivot below 1e-12, a failed
factorization or a non-finite answer hands that step to
`GESpMatParPvMarkoFast`, which solves it or throws `SingularMatrixError`
as before, so the redundant constraint removal runs as it did.

**Same start, same answer.** COLAMD orders by the pattern, and the
factorization and the solve run single threaded in a fixed order: the same
matrix gives the same answer bit for bit. Two things outside the solver
are needed for the same assembly to give the same matrix: the grounded
parts are made solver bodies in the order of their names
(`AssemblyObject::fixGroundedParts`, which took them in the order of an
`unordered_set` of pointers, so the column order could change from one run
to the next); the rest of the model was already in document order.
Verified on chains of 50, 100 and 200 parts, two documents in each of two
processes: every placement equal bit for bit (`float.hex`); and by
`TestSolverBackend.test_same_start_same_solution_bit_for_bit` (12 boxes,
six revolutes free, solved twice from the same start).

**A Chrono trap.** With `BUILD_ASSEMBLY_CHRONO` the Chrono core library is
linked into the same module, and Chrono's `EIGEN_SPARSEMATRIX_PLUGIN`
declares a virtual `SetElement` in `Eigen::SparseMatrix`: its sparse
matrices carry a vtable, and the linker keeps one copy of each inline
member. The first build used `int` indices, which Chrono's sparse solvers
also instantiate, and ran Chrono's `resize()` on a matrix without the
vtable (28 rows and 0 columns after `resize(28, 28)`, then an access
violation in `setFromTriplets`). The solver's matrices are indexed by
`std::int64_t`, a type Chrono does not instantiate.

**Degenerate starts.** Where a start is far from consistent, OndselSolver's
Newton iteration wanders -- steps growing to 1e8 and beyond, redundant
equations found on the way -- before it converges, if it does; the gate's
`test_drag_bundles_fixed` (sec 6.13) and the distance cases are such
solves. There the outcome turns on the last bits of every step, so the
sparse LU, whose rounding differs, ended elsewhere: in
`test_partially_redundant` other equations were set aside and both joints
reported conflicting; in `test_drag_bundles_fixed` the iteration reached a
singular matrix where nothing was left to remove, and `PosICNewtonRaphson::
run()` restarted the identical solve for ever (14,472 rounds before it was
stopped). Both are handled in `run()`:

- a step more than 1000 times the first (and above 1) is taken as
  wandering, and the run is solved once more from its start with
  `GESpMatParPvMarkoFast` throughout, all constraints back
  (`legacyElimination`, `canReplay`): it then ends as it did before, bit for
  bit. In the Assembly suite that is 44 solves, all of small models started
  far from their joints: 40 with Distance joints (whose squared forms send
  the steps far), the rest `test_drag_bundles_fixed`,
  `test_partially_redundant` and `test_drag_moves_fixed_neighbour`; none in
  the bench chains' first solves. But an ordinary edit wanders too: in the
  bench, turning the middle joint's offset 5 degrees about x sends the
  largest position step from 10 mm through 786 mm to 29 m in five
  iterations, with the old elimination as with the new, before a
  redundancy search sets equations aside (of Revolute joints, in a chain
  with nothing redundant) and the rest converges. Replayed, that edit
  costs at 200 parts what it did before -- a minute -- and so do a touched
  leaf, a joint added and a drag (GUI bench, chain 200);
- a singular matrix whose redundancy search sets nothing new aside is
  solved again the same way if it was not already, and otherwise fails
  ("singular matrix, no redundant constraint found") instead of looping:
  that loop was there before, any start that reached it hung.

**Results** (headless, `asm.solve()` on the bench chain after a recompute;
seconds, the faster of two runs; the old elimination first):

| parts | solve_1 | solve_2 |
|---|---|---|
| 50 | 0.30 -> 0.069 | 1.34 -> 0.072 |
| 100 | 3.26 -> 0.19 | 3.23 -> 0.18 |
| 200 | 191 -> 0.73 | 154 -> 0.61 |

The placements are within 1.1e-13 mm of the old solver's at 50 and 100
parts and 1.8e-12 at 200.

**Gate.** Every placement, status, drag step and simulation frame within
the gate's 1e-9 of the records. Six digests were taken again, each the
model handed to a drag (`drag/dump`), which carries the solve before it:
the bench chain and star at 10 and 50, the roller follower's drag and the
rigid group. Only those six lines of `SolverGateReference.json` changed,
merged from a recording to a temporary file, so the placements stay those
the old elimination recorded. `test_drag_bundles_fixed` replays and matches
its record exactly.

**Tests.** ctest 1000/1000; the Assembly suite 371 run, 59 skipped, no
failures (370 + the new test).

**Not done:** a large assembly whose solve wanders is solved twice, the
second time at the old elimination's speed -- as slow as before, and the
abandoned first try on top; without the replay it is little faster, the
redundancy search (`GESpMatFullPvPosIC`, a full pivoting elimination) then
taking the time. The cause is the iteration's lack of any step control:
no damping, no line search, no bound on a part's turn per step, which the
Reference and Chrono backends have (sec 6.13, 6.15). The drag steps and
kinematic runs have no replay (they have no redundancy rounds; a
degenerate drag step may end in other last bits than before); the
velocity solves keep `GESpMatParPvPrecise`; past about 1000 parts in a
chain the equilibrated pivots reach the 1e-12 tolerance and steps go to
the old elimination again.

Sec 6.20 took the replay out again: with OndselSolver's steps held to
reducing the constraints' error no solve wanders off, and none replays.

### 6.20 As built: what a solve weighs, and steps that keep to the constraints (2026-10-27)

Sec 6.19 left an ordinary edit wandering: on the bench chain (boxes, Fixed
and Revolute joints alternating) turning the middle joint's offset 5 degrees
sent OndselSolver's steps metres away from 50 parts up; it then set aside
equations of joints that were not redundant and returned with joints
0.07-0.15 off and rc 0. Chrono gave up on the same edit, and the Reference
solved it slowly at 100 parts and gave up at 200. Four causes, found in turn:

1. **Where the distance is measured.** Every backend solves for the pose
   closest to the start, and measured the distance at the body's frame --
   the part's placement, here the box's corner. A revolute leaves its part
   free to turn about the joint axis, and that turn moved the corner; so the
   closest pose had every free part downstream of the edit turned to keep its
   corner near where it was, a large turn per part, where the plain answer
   tilts the chain and turns nothing.
2. **What a turn weighs.** Against a millimetre, a turn of a radian weighed
   1/4 (the Reference's and Chrono's metric) or, in OndselSolver, whatever
   `Part::fillqsuWeights` made of the inertias after normalising them to the
   largest -- with the unit inertias every body has, the same 1/4. A part's
   size never entered: a step that a small translation could make was made
   by turning bodies through radians, where the linearisation is worthless.
3. **The multipliers' curvature.** OndselSolver's Newton step is the KKT
   step of the closest pose, the Hessian including the multipliers times the
   constraints' second derivatives. Far from a solution the multipliers come
   to the weights (up to 1e6) times the distance times the lever arms --
   1e10 on the edit -- and those terms decide the step.
4. **When to stop.** OndselSolver's convergence test took the RMS of the
   whole step, multipliers included; with multipliers of 1e10 their rounding
   never fell below the test's thresholds, which are those of positions.

**The metric** (`Solver::centredOnJoints`, `Backend.{h,cpp}`; AssemblyObject
wraps every backend's session in it). Before the model reaches the backend,
each body that is not grounded and has a joint frame gets a frame at the
centre of the box of its joint frames' origins (the body's axes kept) and an
inertia of 4 r^2, r half that box's diagonal (at least 1 mm): a turn then
weighs as much as moving the box's corner along its arc (Euler parameters
move by half the angle). Poses come out in, and drag targets go in from, the
body's own frame; a pose the solve left where it started comes out as the
start exactly. `Solver::jointMetric(model)` computes the centre and the
inertia; the Python `AssemblyObject.getSolverMetric()` gives them per object
(the point in the object's frame, the inertia, the solver body), and the
gate measures with it. All three backends weigh a turn by the body's
inertia:
- OndselSolver: `Part::fillqsuWeights` normalises the inertias by the
  largest mass, not the largest inertia, so aJ / m stays a length squared;
  the drag weights scale the body's inertia, not a unit one;
- the Reference: the rotation weights times the inertia;
- Chrono: the bodies' inertia times it (the restoration keeps its lever arm
  weights, which the centring makes smaller).

**OndselSolver's steps** (`PosICNewtonRaphson::calcDXNormImproveRootCalcYNorm`,
the submodule). The Jacobian rows of the constraints g are their exact first
derivatives, so the Newton step is a descent direction for |g|^2; the rows of
the minimum movement hold the multipliers' terms, which are not all there,
so the whole residual is no such measure (backtracking on it failed at a
step factor of 3e-8). A Newton step is taken while it reduces |g|^2
(Armijo, below 1e-18 taken as it is). The first that does not ends the
search for the closest pose: from there each step restores the constraints
only -- the same system with the multipliers at zero, which drops their
terms, and nothing pulling back to the start: the step of least movement
onto the linearised constraints, in the metric above -- cut back by halves
until it reduces |g|^2. The step norm of the convergence test is the
positions' alone (`AnyPosICNewtonRaphson::calcdxNorm`). A solve whose Newton
steps all reduce |g|^2 is solved as before. The replay of sec 6.19
(`legacyElimination`, `canReplay`, the wandering test) never fired any more
and is gone; the guard against a singular matrix that sets nothing new
aside stays.

The four changes were tried apart: the centring alone solved the edit and
broke the first solve of `test_same_start_same_solution_bit_for_bit` (twelve
boxes from a jittered start: eight joints reported conflicting), whose
Newton steps had converged by luck; backtracking on |g| alone then crawled
(steps cut to 1/32, 60 iterations, 0.06 off), because the restoring steps
turned bodies through radians; with the inertia from the joints' spread the
restoration takes full steps and converges quadratically. The positions'
step norm alone, without the centring, ended the edit with 12 joints
reported redundant.

**Results.** The edit (headless, `asm.solve()` after the offset change;
moved: the largest displacement of a part):

| parts | OndselSolver | Chrono | Reference |
|---|---|---|---|
| 50 | 0.20 s | 0.41 s | 1.2 s |
| 100 | 0.56 s | 4.8 s | 20 s |
| 200 | 1.0 s | fails | 104-130 s |

All three move the parts the same 20.942 mm at 50, 42.749 mm at 100 and
86.367 mm at 200, every joint within 1e-11 by the neutral checker, nothing
reported redundant. Before, OndselSolver returned wrong answers (128 s at
100 parts), Chrono failed from 50 parts and the Reference from 200. The
bench's later phases at 200 parts on OndselSolver: a touched leaf 1.0 s, a
joint added 1.2 + 0.8 s, preDrag 2.1 s, ten drag steps turning the dragged
part 30 degrees 4.1 s (Chrono 2.9 s at 100, the Reference 19 s at 100).

**Gate.** Taken again whole: the model text of every case changed (the
centred frames and the inertias are in it), and with it every digest; the
placements of the cases with freedom left moved (another metric, another
closest pose); those with none moved by at most 9e-16. Three statuses
changed, each a solve that had wandered off: PointPoint d=0 now solves
(ret -1 before), `test_drag_bundles_fixed` has DoF 1 and nothing redundant
(DoF 8, two joints conflicting before), LineTorus DoF 45 and nothing
conflicting (49 and one before). Those were the three entries of
`BACKEND_DIFFERENCES`, where both backends disagreed with OndselSolver; all
three backends agree now and the entries are gone. The backends' check of
"no farther from the start than OndselSolver" measures in the new metric
(`getSolverMetric`, each solver body once); OndselSolver's solutions are
within 0.2 % of the backends' closest (LineSphere), the rest equal.

**Tests.** The Assembly suite 371 run, 59 skipped, no failures; ctest
1000/1000.

**Not done:** Chrono still fails the edit at 200 parts ("Chrono's assembly
did not converge"), and its drag there turns nothing. A
solve that falls back to restoring ends on the constraints near the start,
not at the closest pose (within 0.2 % of it in the gate). The Reference is
dense, so at 200 parts it takes minutes.

### 6.21 As built: bodies held rigidly leave the solve (2026-10-27)

**Assembly3's way.** Before its solver runs, `Constraint.getFixedParts`
takes every Attachment constraint with a fixed part on one side, computes the
other part's placement from the two elements' placements (a matrix product,
no solver), makes that part fixed, and removes the constraint; the parts so
fixed can fix the next. An Attachment with no fixed side stays a solver
constraint (a PlaneCoincident with the angle locked). Only Attachments count,
and its cascade is one pass in list order (the `nxt` list it loops on is
never filled).

**Here** (`Solver::groupedRigidly`, `Solver/RigidGroups.{h,cpp}`; the
session wraps `centredOnJoints`, which wraps the backend's): the same idea
for any joints, and for bodies that reach no fixed body.
- From the grounded bodies: a body whose joints to the held ones carry 6
  equations or more is tried. One Fixed joint places it by multiplying its
  frames, as Assembly3 places an Attachment; any other set is solved for that
  body alone by the Reference backend (six unknowns, the others' frames on
  the ground), and taken when it converges with DoF 0 and no dependent
  equation. A placed body is held in turn, until no body can be placed.
- Then the first remaining body (model order) is a ground of its own, and
  the bodies it holds rigidly the same way become one body with it, each at
  the offset its placing gave; then the next remaining body, until none is
  left.
- Never taken out: a set of joints with a dependent equation (it stays with
  the backend, which finds and reports the redundancy as before), a body a
  limit or a driver touches, a joint that couples rotations or holds a point
  on a curve (their equations count turns or follow a point, not the two
  poses alone).

The smaller model has the placed bodies' frames on the ground, each group's
members' frames on its first body, the joints that placed a body removed.
Poses, drag targets (a member's target taken to its group's body), the
diagnostics' joint ids and per-body DoF map back; the DoF is the same, each
body taken out with six independent equations. A model with nothing to
take out goes to the backend unchanged. `Session::bodyMetric()` says what
each body weighs and which solver body it moves with, so
`getSolverMetric()` reports the metric of the solve that ran, and the gate
counts a group once.

Where a Fixed joint starts violated, the group's first body keeps its start
and the member goes to it, where the whole solve moved both towards each
other; `test_drag_bundles_fixed` (C 50 mm from where its Fixed joint puts it)
is the gate's one case that moved for this, 13.7 mm. The other five records
taken again (the bench chains and stars, the Fixed joint case) changed their
digests and moved by rounding.

**Results** (the bench chain, OndselSolver, before -> after; the chain's
Fixed joints pair its boxes, so the backend sees half the bodies):

| | 100 parts | 200 parts |
|---|---|---|
| solve | 0.23 -> 0.13 s | 0.90 -> 0.64 s |
| the 5 degree edit | 0.30 -> 0.18 s | 1.11 -> 0.65 s |
| a touched leaf | 0.28 -> 0.21 s | 1.00 -> 0.77 s |
| preDrag | 0.40 -> 0.33 s | 2.06 -> 1.60 s |
| ten drag steps | 1.30 -> 1.25 s | 4.12 -> 3.71 s |

Chrono's solve at 100 parts went from 0.36 to 0.09 s and the edit from
4.1 to 0.75 s; at 200 it still fails the edit and its drag. The Assembly
suite 372 run (the new `TestSolverBackend.test_rigid_groups`), 59 skipped,
no failures; ctest 1000/1000.

**Not done:** a group is placed from its first body's start, not the
closest pose of all its bodies; a body is tried against the held ones alone,
so two bodies that are rigid only together (neither alone) stay with the
backend; the drag still bundles Fixed joints by the parts' current offsets
(`bundleFixed`) before this sees the model.

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
  Done (`6a4b278213`).
- Tests (`AssemblyTests/`) -- the identity gate, then the corpus run once per
  registered backend against the Reference oracle.


## 7. Open questions

1. Per-assembly backend property, or preference only as the PR does? The
   property is Assembly3's practice and makes a file reproducible; the
   preference is cheaper. Proposed: both, property empty by default.
   Decided (user, 2026-10-22): both (audit sec 8.3). Implemented
   2026-10-23 (`6a4b278213`, sec 6.6).
2. Does the neutral layer own the formula evaluator (a small parser), or
   keep OndselSolver's `SymbolicParser` as a library for all backends?
   Decided (user, 2026-10-22): the neutral layer owns it. Implemented
   2026-10-23 (`ccecaae903`, `a8ec840b7f`, `61edb5d599`; grammar sec 4.6.1,
   as built sec 6.9). Since sec 6.12 `runKINEMATIC` evaluates the
   neutral evaluator too, through `ExternalFunction`; the text still goes
   into the ASMT file.
3. The 17 Distance sub-types that fall to Planar(d): keep, map properly
   (several have exact primitives: PointCone and PointTorus are point-surface
   distances), or report unsupported?
   Decided (user, 2026-10-22): audit sec 8.1 decision 7. Implemented
   2026-10-23 (`e320accd94`, `85a4379a0b`, sec 6.8).
4. Is the `ReferenceBackend` worth writing before the OndselSolver rework, as
   the oracle that lets the rework be tested against something other than
   itself? Proposed: yes; it is a few hundred lines on Eigen, which the
   module already includes (`App/CMakeLists.txt:14`).
   Decided (user, 2026-10-23): yes, the reference solver first, then
   Chrono. Implemented 2026-10-23 (`47e02a7cc5`, sec 6.13); it came to
   about 2000 lines with the diagnostics, limits, drag and simulation.
5. Answered by the user 2026-10-22: the rework stays in our fork; nothing
   goes to the FreeCAD org. The original question: should the solver fork's
   changes (warm update, the reverse dependency
   removal) be offered to upstream OndselSolver, or kept fork-only? The same
   question as the audit's open decision 2.
