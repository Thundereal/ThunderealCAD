# Assembly3 against our Assembly: what to borrow, and an importer

Status (2026-10-22): a study, nothing changed. It extends
`docs/AssemblyAudit.md` (branch `AssemblyPort`).

Revised 2026-10-24: the Element verdict of A.1 was wrong, and so was the
merge-cost premise behind it. The object model moves to C++ with Assembly3's
Elements and ElementLinks (ElementBindings in this tree since 2026-10-28;
Assembly3's names stay below): `docs/AssemblyObjectModel.md`. Section B's
mapping of elements onto upstream's joint references is superseded by it;
Assembly3 elements now map 1:1.

Sources:
- the Assembly3 clone at `D:\works\sw\FreeCAD_assembly3` (master, `773b977`,
  2025-11-08, version 0.12.3);
- its wiki (https://github.com/realthunder/FreeCAD_assembly3/wiki, last
  commit 2023-12-12);
- one real Assembly3 sample file, unpacked and read as XML;
- this tree.

Path conventions:
- `asm3/...` is under `FreeCAD_assembly3/freecad/asm3/`.
- `Assembly/...` is under `fcad/src/Mod/Assembly/`.
- Other fork paths start at `fcad/src/`.
- Audit numbers (F1-F10, defects 1-8, phases 0-5, sec 8.2) refer to `docs/AssemblyAudit.md`.

Merge cost is judged against the stated policy: every upstream Assembly
commit is ported. Anything that changes upstream's object model (JointGroup /
JointObject / AssemblyLink, the Reference1/2 + Placement1/2 + Offset1/2 joint)
is paid for again on every port. Anything additive (new files, new commands,
behaviour behind existing properties, C++ internals upstream is unlikely to
touch) is cheap.


## 0. The one-paragraph comparison

The two designs answer the same question differently.

- **Assembly3** puts an interface layer between parts and constraints.
  - Every geometry reference is a named, label-addressed `Element` object
    owned by the assembly.
  - Constraints hold `ElementLink`s to those elements, never to geometry.
  - Elements of a child assembly are re-exported upward by label.
  - Each constraint is translated into a primitive geometric vocabulary --
    SolveSpace's params, points, normals and workplanes -- and solved per
    assembly, children first.
- **Upstream's Assembly** has no interface layer.
  - A joint is one object holding two `PropertyXLinkSub` references rooted
    at the moving component, plus two cached joint coordinate systems (JCS).
  - Joints are translated whole into OndselSolver multibody joints.
  - The whole assembly is solved at once, including mirrored flexible
    sub-assemblies.

Assembly3 is stronger on reference stability, nesting, partial or frozen
loading and solve avoidance. Upstream is lighter per joint (one object
against three to five), richer in joint semantics (limits, gears, motion)
and is what we port. The best borrowings are therefore behaviours, not
objects:
- solve avoidance and the write-back tolerance;
- children-first rigid solving;
- freeze with partial load;
- auto-fix of broken references;
- the mover conveniences;
- per-transaction auto-solve.

The Element object model is taken as well (revised 2026-10-24, sec A.1).


## A. What to borrow

### A.1 Object model

**What Assembly3 has.**

Every container is a Python proxy on a core C++ type, given a dynamic
`App::LinkBaseExtensionPython` (`asm3/assembly.py:240-242`) and configured
as a link group at run time (`configLinkProperty`, `asm3/assembly.py:320-334`).

- **Assembly**: `Part::FeaturePython` (`asm3/assembly.py:4463-4488`).
  - Its `Group` is fixed by index: `[0]` constraint group, `[1]` element
    group, `[2]` part group, `[3]` relation group (`asm3/assembly.py:4318-4435`).
  - Subnames use those indices: `'2.'` is the parts (`asm3/assembly.py:4579-4591`).
  - It carries `SolverType`, `Freeze` and `BuildShape`
    (`asm3/assembly.py:4227-4249`).
- **AsmPartGroup**: holds the parts.
  - A part is anything with a Placement, usually an `App::Link`. Dropping an
    object creates a link (`asm3/assembly.py:478-483`).
  - A child assembly, or a Link to one, is just another part.
  - It also has `DerivedFrom`, which inherits another assembly's parts as
    locked parts (`asm3/assembly.py:404-437`).
- **AsmElementGroup / AsmElement**: an Element is a `Part::FeaturePython`
  link to `(part, "Body.Face3")` through a `PropertyXLink`
  (`asm3/assembly.py:648-653`).
  - It keeps a copy of the element shape in its own `Shape`
    (`asm3/assembly.py:785-872`).
  - It has an `Offset` applied in the element's own frame
    (`asm3/assembly.py:874-913`) and a `Detach` that freezes it
    (`asm3/assembly.py:614-615`, `:794-796`).
  - Labels are unique among siblings (`asm3/assembly.py:3322-3341`), so they
    can be used as names.
  - An element of a child assembly is referenced as
    `Child.1.$ElementLabel.` -- a label reference into the child's element
    group (`asm3/assembly.py:1197-1209`).
- **AsmConstraintGroup / AsmConstraint / AsmElementLink**:
  - A constraint is an `App::FeaturePython` link group of ElementLinks.
  - Its type is a persisted integer `_ConstraintType` plus a `ConstraintType`
    enumeration.
  - Changing the type swaps the attached sub-proxy and adds or removes that
    type's dynamic properties (`asm3/proxy.py:91-150`, `asm3/constraint.py:458-481`).
  - An ElementLink always links through a *local* element of the same
    assembly, creating `_Element` objects as needed
    (`asm3/assembly.py:2128-2150`), and has its own `Offset`
    (`asm3/assembly.py:1858-1880`).
- **AsmRelationGroup**: one `AsmRelation` per part, listing the constraints
  that involve it (`asm3/assembly.py:3428-3557`). This is a navigation index
  kept as document objects.
- **Freeze**: a frozen assembly stores its parts' compound in the part
  group's `Shape` (`asm3/assembly.py:4156-4225`, `:4303-4313`). Its groups
  answer `canLoadPartial` with 1 or 2 (`asm3/assembly.py:445-446`, `:628-629`,
  `:3253-3254`, `:3488-3489`), so in an external document neither its
  constraints nor its parts are created.

**Why elements make references robust.**

- A constraint never names geometry, so a part edit that renames a face is
  fixed in one place, the deepest Element, and every constraint using it
  follows.
- Replacing a part keeps constraints as long as the new part's assembly
  exports elements with the same labels (wiki Replacing-Part).
- A broken element is repaired in two steps (`asm3/assembly.py:722-783`,
  switched on by `AutoFixElement`, `asm3/gui.py:700-710`):
  1. first from topological-naming history (`Part.getRelatedElements`);
  2. then by geometric search against the stored shape copy
     (`shape.searchSubShape`).
- The `$Label` indirection also decouples a parent from a child assembly's
  internals.

**How App::Link is used.**

- Parts are links.
- Every Assembly3 group is itself a link group (`GroupMode` auto-delete,
  `asm3/assembly.py:326-334`). That gives per-assembly `VisibilityList` and
  scene-graph reuse.
- A sub-assembly reused N times is N `App::Link`s to one assembly object; the
  sample file does exactly this (sec B.6).
- Link arrays are first-class: each array element is a separately movable
  part, `(array, index)` (`asm3/assembly.py:1754-1813`). The solver writes
  either the element's `Placement` or one entry of the array's
  `PlacementList` (`asm3/assembly.py:2284-2309`).
- A sub-assembly is always rigid in its parent: it is solved by itself first
  (sec A.3), and the parent moves the whole link.

**Against our JointObject.**

- Upstream stores `Reference1/2` as `PropertyXLinkSub` rooted at the moving
  component, `[Component, ["Body.Face6","Body.Vertex3"]]` (`Assembly/JointObject.py:365-381`,
  `Assembly/UtilsAssembly.py:1273-1346`).
- The mapped (TNP) name is kept by the property as a shadow sub
  (`App/PropertyLinks.cpp:5513-5541`), while the Python stores plain names
  (`Assembly/UtilsAssembly.py:1285-1289`). At the topological-naming level we
  are therefore as safe as an Element: both rest on the same shadow-sub
  machinery.
- What we lack is:
  1. A **single point of repair** shared by several joints: three joints on
     one face hold three references, and a renamed face breaks all three.
  2. **Any repair at all**: `Joint.execute` raises "Broken link" when a sub
     contains `?` (`Assembly/JointObject.py:816-834`).
  3. **Replace keeping joints** (audit sec 2, "structure").
  4. **Stable interfaces of a sub-assembly**: joints into a flexible
     sub-assembly are copied by property name into mirrored objects
     (`Assembly/App/AssemblyLink.cpp:561-686`, F9).

**Verdict (revised 2026-10-24): adopt Element and ElementLink** (built as
`Part::ElementBinding`, so named since 2026-10-28).

The first verdict here ("do not adopt", costed by the merge burden on
upstream's Python joints and by object counts) misread the design: it
treated elements as a pool of references owned by the assembly that uses
them. In Assembly3 an Element is an interface declared by the assembly that
holds the geometry, for its parents to connect to, and an ElementLink is how
an assembly uses an element of a direct child (or its own) to form a
constraint (wiki, Concepts / Element). The shadow subnames of a joint
reference give topological-naming safety, but not a declared interface,
not a single point of repair across hierarchy levels, and not replacement
by label.

With the object model moving to C++ (`docs/AssemblyAudit.md` decision 8),
the merge premise is gone too. The design, including elements on Body and
App::Part and a shape copy on each ElementLink, is
`docs/AssemblyObjectModel.md`.

### A.2 Solver abstraction

**What Assembly3 has.**

- `asm3/proxy.py` `ProxyType` is a metaclass registry of "proxies of
  proxies".
  - A class registers itself by being defined with a positive `_id`.
  - The host object keeps a persisted integer `_<Kind>Type` and an enum
    `<Kind>Type`. Switching the enum detaches one implementation and attaches
    another, adding its dynamic properties (`asm3/proxy.py:91-190`).
- The same metaclass serves constraint types (`Constraint`), solver backends
  (`System`, `asm3/system.py:15-94`), SciPy algorithms (`_AlgoType`,
  `asm3/sys_sympy.py:15-210`) and GUI commands (`AsmCmdManager`,
  `asm3/gui.py:173`).
- **The backend is chosen per assembly** by the `SolverType` enum: `None`
  disables, SolveSpace is id 1 and the default
  (`asm3/system.py:23-29`, `:102-135`). An unknown id triggers the
  "install py_slvs" prompt (`asm3/system.py:31-40`).
- **The interface is SolveSpace's own entity vocabulary.** A backend
  provides `addParamV`, `addPoint3d`, `addNormal3d`, `addWorkplane`,
  `addTransform`, `addPointsCoincident`, `addParallel`, `addPointOnLine` ...,
  `solve`, `getParam`, and `Failed`/`Dof`.
  - Assembly-level constraints are composed from those primitives in a
    shared mixin, `SystemExtension` (`asm3/system.py:146-466`). For example,
    PlaneCoincident is points coincident plus parallel normals or same
    orientation (`:316-361`); AxialAlignment is point on line plus parallel
    (`:397-421`).
  - Each constraint class names the system method it needs
    (`constraintFunc`, `asm3/constraint.py:852-860`) and maps element shapes
    to entities through typed converters (`_p`, `_n`, `_l`, `_w`, `_c`,
    `asm3/constraint.py:30-400`).
  - Support is checked per backend: an unsupported constraint is disabled
    with an error (`asm3/assembly.py:2420-2441`, `:4356-4359`;
    `asm3/system.py:88-94`).
- Each part is 7 params (position plus quaternion) in a "solving group"; a
  fixed part goes in a fixed group (`asm3/solver.py:272-324`).

**What made a backend easy or hard.**

- `sys_slvs.py` is 72 lines because SolveSpace natively *is* the interface
  (`asm3/sys_slvs.py:28-71`).
- `sys_sympy.py` is 1346 lines because it re-implements every SolveSpace
  entity and constraint as a SymPy equation:
  - least squares through `scipy.optimize.minimize` with lambdified
    Jacobian and Hessian (`asm3/sys_sympy.py:1097-1294`);
  - one-variable equations pre-solved (`:1181-1202`).
- It is effectively dead: its import is commented out
  (`asm3/init_gui.py:19-24`).
- Lesson: an abstraction pitched at one backend's primitives makes that
  backend free and every other one a rewrite. It also cannot express what
  upstream needs (limits, gears, screws, belts, motions, masses).

**Redundancy and DOF.**

- SolveSpace returns the failed constraint handles and the remaining DOF
  (`asm3/sys_slvs.py:53-71`). These are mapped back to constraint objects by
  group handle (`asm3/solver.py:115-133`). That is a conflict report, which
  we lack (defect 1).
- Before solving, `SystemExtension` counts same-type constraints between
  each part pair, including implicit ones through shared elements, and
  **auto-relaxes** them (`asm3/system.py:146-314`, `AutoRelax` property
  `:99-100`).
  - Example: a second PlaneCoincident between the same two parts is
    reduced to a 2D PointOnLine (`:339-356`).
  - Over the limit, the constraint is skipped with a warning.
- **Drag**: SolveSpace's `addWhereDragged` is commented out as not working
  (`asm3/solver.py:96-106`). Dragging is a plain re-solve from the moved
  initial placement (sec A.4).

**Verdict.**

- Do **not** port the SolveSpace-vocabulary interface. Upstream's open PR
  #28156 proposes a solver interface plus a Project Chrono back end at the
  joint level (joint type, two JCS, parameters), which is the right level
  for OndselSolver, Chrono and `docs/Simulator.md`. Track that PR instead.
  The merge cost of a home-grown interface would be high.
- Borrow:
  1. *Per-assembly backend choice by enum*, if #28156 lands. It costs one
     property; its `ProxyType` swap is unnecessary in C++.
  2. *Map failed handles back to joints* for the conflict report (defect 1).
     OndselSolver already knows which constraints were found redundant; the
     work is the mapping, as in `asm3/solver.py:115-133`.
  3. *Pre-solve structural redundancy counting*: catch duplicate or
     implicit constraints between a part pair before the solver.
     - Gain: OndselSolver's exception-driven redundancy search is 45 % of a
       chain solve (F1); a cheap structural pass can avoid it, or at least
       name the culprit.
     - Cost: per joint type, the DOF it removes between a pair.
     - Merge: C++ internal, cheap.
- A SolveSpace back end for us is technically easy -- Assembly3's
  `SystemExtension` already maps PlaneCoincident, Axial and Attachment to
  slvs primitives -- **but SolveSpace (and `py_slvs`) is GPLv3**. It can only
  be an optional pip-installed module, exactly as Assembly3 ships it; never
  linked into FreeCAD or bundled.

### A.3 Solving strategy

**What Assembly3 does.**

1. **Per assembly, children first.** `solve()` takes the selected (or all)
   assemblies and orders them topologically with
   `FreeCAD.getDependentObjects(..., 1)` (`asm3/solver.py:351-370`). Each
   assembly is solved alone with its own constraints; a child is a rigid
   part in its parent.
2. **Untouched assemblies are skipped.** `Assembly.execute` touches the
   system (`asm3/assembly.py:3946-3956`). The solve loop skips an untouched
   one (`asm3/solver.py:379-383`) and clears the flag after a solve (`:383`).
   Assemblies with `Freeze`, or solver `None`, are skipped (`:341-343`).
3. **SmartRecompute.** `AsmVersion` version stamps on elements, element
   links and constraints skip a recompute when nothing upstream changed
   (`asm3/assembly.py:569-588`, `:1931-1985`, `:2458-2461`, `:2751-2762`).
4. **Fixed parts.**
   - Explicit Lock constraints: a face or non-linear edge fixes the part;
     a vertex or linear edge fixes a point or line
     (`asm3/constraint.py:968-1084`).
   - Implicitly fixed parts: a part with no or read-only `Placement`, or a
     `Fixed` workplane (`asm3/constraint.py:532-550`).
   - If there is no lock at all, the part of the first element of the first
     constraint (`:582-592`).
5. **Solve without the solver where possible.** Attachment constraints
   reachable from a fixed part are placed by matrix product, iterating until
   no more can be placed, and removed from the solve
   (`asm3/constraint.py:605-701`). This is the audit's phase 2.2 ("place
   loop-free parts directly"), for rigid joints only.
6. **Write-back with a tolerance.** A part is written only if
   `isSamePlacement` fails at 1e-6 (`asm3/solver.py:172-188`,
   `asm3/utils.py:644-664`). This is exactly what defect 7 / F6 lacks.
7. **Rollback.** Every write is recorded, and on any exception the
   placements are restored in reverse (`asm3/solver.py:384-399`).
8. **Pattern shortcut.** A multiplied constraint solves only the first of
   N coplanar pairings; the other array instances are moved by the same
   relative offset afterwards (`asm3/solver.py:222-243`,
   `asm3/constraint.py:1190-1228`). N instances cost one constraint.
9. **One solve per transaction.** Changes only *schedule* a solve
   (`Assembly.autoSolve`, `asm3/assembly.py:4023-4033`). The solve runs from
   the document observer's `slotBeforeCloseTransaction`
   (`asm3/mover.py:484-486`, `asm3/assembly.py:4040-4055`), so the solver's
   writes land in the same undo step as the edit that caused them. A part
   placement change by the user schedules one too (`checkPartChange`,
   `asm3/assembly.py:3994-4021`).

Against ours:
- upstream solves inside `AssemblyObject::execute` on every recompute
  (`Assembly/App/AssemblyObject.cpp:142-153`);
- it rebuilds everything (F2);
- it has no untouched-skip and no tolerance on the write-back (F6);
- it mirrors flexible sub-assemblies (F9).

**Verdict.** Borrow, in this order:

| item | gain | cost | upstream conflict |
|---|---|---|---|
| (6) write-back tolerance | kills the no-op write storm, F6 | a few lines in `setNewPlacements` (`Assembly/App/AssemblyObject.cpp:582`) | none; upstream fix-shaped |
| (2) skip untouched | a re-solve of an unchanged assembly costs ~0 instead of a full solve (sec 4.2: 6.7 s at star 1000) | a dirty flag set from Group, joint and part-placement changes | low: C++ internal |
| (7) rollback on failure | a failed solve leaves no half-moved parts | snapshot placements; `savePlacementsForUndo` exists (`:190-192`) | low |
| (1) children first, rigid | each rigid sub-assembly solved once, not once per instance | solve AssemblyLink targets first by dependency order | **medium**: upstream's Flexible mode is a different model; keep it, but make Rigid the fast path |
| (5) direct placement of rigid chains | Fixed-joint trees need no Newton | the audit's phase 2.2 generalises this to all loop-free joints | low |
| (9) solve per transaction | one solve per user action, inside its undo step | an observer on transaction close; replaces solve-in-execute | **medium**: changes when upstream's solve runs; keep `SolveOnRecompute` as the switch |
| (3) version stamps | skip joint executes when inputs are unchanged | per-joint input hash | low |
| (8) pattern shortcut | N-instance patterns at the cost of one joint | needs a pattern joint first (A.4) | depends on A.4 |

### A.4 Interaction

**Mover** (`asm3/mover.py`):

- `AsmMovingPart` (`:17-285`) puts the assembly view provider in edit mode
  with a dragger at the selected element's frame (`:63-110`).
- On each motion it writes the part placement and runs a full solve with
  `dragPart` and a rollback list (`:168-262`). The drag is one transaction
  (`asm3/assembly.py:4819-4841`).
- Modifiers: Ctrl skips the solve, Shift skips everything (`:243-254`).
- A locked axis or point constrains the dragger: translation is zeroed and
  only yaw kept (`:223-235`).
- `moveElement` drags an element's `Offset` instead of the part
  (`:176-182`): interactive editing of a connector offset.
- `Trace` draws the path of a point as a `Part::Polygon` while dragging
  (`:263-277`).
- `AxialMove` is the same mover with the non-centerball dragger
  (`asm3/gui.py:520-534`).
- `QuickMove` (`asm3/mover.py:399-450`): the part follows the mouse on the
  view plane until a click; Esc aborts the transaction. It is meant for
  fetching far-away parts.
- `LockMover` refuses to move fixed parts (`:287-294`).

Against ours:
- Upstream's drag goes through OndselSolver's drag API with modes
  (`Assembly/Gui/ViewProviderAssembly.cpp:927-1149`), which is better than
  Assembly3's re-solve per motion.
- Worth borrowing:
  - **QuickMove**: about 60 lines, a new command, no conflict;
  - **Trace while dragging**: closes the audit's "trace curves" motion gap
    for drags, low cost;
  - **dragger-edit of Offset1/Offset2** in the joint task: upstream has a
    dialog only (`Assembly/JointObject.py:2000-2001`).

**Constraint creation from selection.**

- `AsmConstraint.getSelection` (`asm3/assembly.py:2817-2958`) accepts
  geometry picked at any depth and finds the **lowest common assembly** of
  the picks.
- It auto-creates elements, exporting them up through each intermediate
  child assembly (`asm3/assembly.py:1178-1209`).
- It validates every element against the constraint's typed converters,
  with messages like "requires the 2nd element to be a planar face"
  (`asm3/constraint.py:862-895`, `:1110-1133`).
- **Borrow** the validation messages into the joint task. They are additive
  and help users: upstream silently computes a JCS from whatever was picked.
- The common-ancestor search matters only with nested flexible
  sub-assemblies. Leave it to upstream's AssemblyLink work.

**Multi-element and pattern constraints.**

- One constraint may hold N elements:
  - *Multi* fans out as (E1, Ei) (`asm3/constraint.py:1232-1290`);
  - *Cascade* chains as (E1,E2), (E2,E3) ... (`:1292-1327`).
- **Multiply** turns the first element's part into a link array and pairs
  its instances with all coplanar circular edges of the same radius in the
  second element (`asm3/assembly.py:2477-2707`, `:3020-3161`). Pairing is by
  nearest position, kept stable across recomputes (`prevOrder`, `:2585-2657`).
  Pressing the command again expands it into individual constraints
  (`:3106-3155`).
- Gain for us: the "component patterns" gap of audit sec 2 -- fasteners
  into a hole pattern in one action.
- How to borrow without diverging:
  - a **command** that takes one joint plus a hole pattern;
  - it turns the part into an `App::Link` array, ElementCount = holes;
  - it creates one joint per array element, as upstream already supports
    LinkElements as components (`Assembly/UtilsAssembly.py:124-131`,
    `Assembly/App/AssemblyObject.cpp:2078-2105`).
- That keeps upstream's model at the cost of N joint objects (F8).
- A real pattern joint (one object, N pairings, solved once, as in A.3 item
  8) would be the better structure but is a model change. Only worth it if
  upstream wants it.

**Locking.**

- Upstream's `GroundedJoint` is whole-part only (`Assembly/JointObject.py:1276-1340`).
- Assembly3's lock of a vertex or edge, leaving rotation free, has no
  equivalent. It is rarely needed: a Ball or Revolute joint to a grounded
  datum expresses it.

**Element visualisation and navigation.**

- On-top-when-selected elements (`asm3/assembly.py:313-316`).
- Show coordinate system per element (`:1348-1426`).
- Flip element or flip part (`:1561-1600`).
- "Go to relation" and "select linked / linked final" (`asm3/gui.py:887-1055`).
- Measure pseudo-constraints living in the assembly, with expression-bound
  labels (`asm3/constraint.py:733-742`, `:2047-2076`).
- Part folders inside the part group (`AsmPlainGroup`,
  `asm3/assembly.py:5070-5320`) with subnames flattened.

Of these, upstream has flip (`flipOnePart`) and joint visuals. Worth taking:
- the *per-part joint list* ("go to relation") as an **in-memory index**, not
  document objects. It doubles as the connectivity index of audit phase 2.3
  (F3/F4).
- "toggle part visibility" (`asm3/gui.py:604-640`).

### A.5 Everything else

- **Undo.**
  - Every Assembly3 command opens and closes its own transaction, aborting
    on error (`asm3/gui.py:466-485`, `asm3/assembly.py:2961-3017`).
  - Undo and redo cancel a pending auto-solve and refresh the mover
    (`asm3/mover.py:475-490`).
  - Upstream mostly relies on Gui command transactions plus
    `savePlacementsForUndo`.
  - Borrow the solve-inside-the-transaction rule of A.3 item 9.
- **Recompute integration.**
  - Assembly3 keeps the solver out of `execute` and drives it from
    transaction close. It pauses its scheduler during a document recompute
    (`asm3/mover.py:495-499`, `asm3/assembly.py:4079-4099`).
  - Upstream couples solve to recompute (F2). This is the larger design
    difference and the one most worth moving towards. The merge cost is
    modest if done behind upstream's existing `SolveOnRecompute` preference.
- **Logger.**
  - `FCADLogger` is a tagged `FreeCAD.Logger` hierarchy, `asm3.main`,
    `asm3.sys` and so on, with per-tag levels (`asm3/utils.py:12-20`,
    `asm3/FCADLogger.py`).
  - `logger.catch`, `report` and `catchTrace` wrap calls and route
    exceptions to the log or a message box.
  - Upstream uses `App.Console` directly. The pattern is cheap to take for
    new fork-only Python (the importer, B), without touching upstream files.
- **Selection to element.**
  - `deduceSelectedElement` guesses a sub-element when a whole object is
    picked (`asm3/utils.py:107-121`, `asm3/assembly.py:2929-2937`).
  - It also handles link arrays and draft wires as parts.
  - Small, additive.
- **Broken references.**
  - The auto-fix of A.1. Elements also keep a geometry copy, so an
    ElementLink still resolves when the lower element is missing (wiki,
    release 0.9).
  - Ours raises (`Assembly/JointObject.py:816-834`).
- **BuildShape.** The assembly as a fused, cut or common solid
  (`asm3/assembly.py:4156-4225`), useful for downstream features. Upstream's
  App::Part already exposes a compound. Low priority.
- **fromLinkGroup.** Converts a plain link group tree into assemblies
  (`asm3/assembly.py:4606-4666`). It is a precedent for B's structure pass.
- **Freeze plus partial load.**
  - The largest large-model idea in Assembly3, and it lands on machinery
    this fork already has (`App/DocumentObject.h:832`,
    `App/Document.cpp:3822`, `:4063-4121`; `App/FeaturePython.cpp:365`;
    `docs/DocumentLoad.md`).
  - A `Freeze` on AssemblyObject / AssemblyLink would:
    - store the compound shape;
    - skip solving;
    - make the joints and parts of an external sub-assembly document not
      load at all.
  - Gain: the "lightweight open" of audit phase 4, for nested
    multi-document assemblies.
  - Cost: one property, a stored shape, `canLoadPartial` overrides on
    AssemblyObject, JointGroup and AssemblyLink.
  - Merge: additive (a fork-only property and virtuals). Upstream has a
    generic `ObjectStatus::Freeze` (`App/DocumentObject.h:78-81`), which is
    unrelated.


## B. Importing Assembly3 documents

Built 2026-10-25 against the C++ object model, not upstream's joints:
`docs/AssemblyObjectModel.md` sec 8 (step 4f). It reads the document
through the bundled Assembly3 code instead of the structural classifier
of B.1, and maps elements to `Part::Element`s and constraints to joints
whose element bindings carry the offsets; what follows is the plan it
started from.

### B.1 Loading without Assembly3 installed

Without Assembly3 installed, an Assembly3 document opens with every object
present and every property readable, but no proxies. Every object has a core
C++ type: `Part::FeaturePython` for Assembly, AsmElementSketch and
AsmWorkPlane; `App::FeaturePython` for the rest; `App::Link` for parts. The
behaviour lives in `module="freecad.asm3.assembly"` proxies whose saved
state is JSON `null` (`AsmBase.dumps`, `asm3/assembly.py:228-238`; the
sample's `Python value="bnVsbA=="`).

- **The proxy is refused.** `PropertyPythonObject::Restore` refuses a proxy
  module that is neither loaded nor under a registered Mod root. It logs one
  Error per object ("... the object is left without a Proxy") and sets the
  Proxy to None (`App/PropertyPythonObject.cpp:361-374`, `:409-419`). View
  provider proxies take the same path. `finishRestoring` then shows the
  object with no proxy (`Gui/ViewProviderFeaturePython.cpp:611-631`).
- **The object survives.** It is created from its C++ type. Dynamic
  properties are recreated from their saved type
  (`App/PropertyContainer.cpp:679-681`, `:729-737`): `Group`, `LinkedObject`,
  `_ConstraintType`, `Offset`, `Freeze`, `_SolverType` and the rest. The
  dynamic `App::LinkBaseExtensionPython` is re-added too
  (`App/ExtensionContainer.cpp:304-314`, `:367-412`).
- **What does not come back.** `configLinkProperty` runs at run time in
  `linkSetup` (`asm3/assembly.py:320-334`, `:604-618`), so the extension is
  *not* wired to `Group` / `LinkedObject`. Two things follow:
  - `getSubObject("2.")`, `"$Label."` lookups and link-group visibility do
    not work through Assembly3 containers. The importer must walk `Group`
    lists and resolve subnames itself.
  - Parts are ordinary `App::Link` objects and resolve normally. External
    documents load through `PropertyXLink` as usual.
- **Without Proxy the class name is gone.** The importer classifies objects
  by structure instead:
  - an assembly is a FeaturePython with `_SolverType` or `SolverType`;
  - its `Group[0..3]` are the constraint, element, part and relation groups;
  - a constraint is a child of the constraint group (and has
    `_ConstraintType`);
  - an ElementLink is a child of a constraint;
  - an element is a child of an element group.
- **Optional quieter open.** An "Open Assembly3 document" entry can first
  insert a tiny inert shim for `freecad.asm3.assembly` into `sys.modules`.
  The module is then "already loaded" and accepted by `moduleAllowed`, the
  ~70 errors go away and class names are kept.
  - Only when the real module is not importable.
  - Only for that open.
  - Classes with no-op `__setstate__` / `onDocumentRestored`.
  - The structural classifier must still work without it.
- **With Assembly3 installed.** This fork has every core API Assembly3 needs
  (`cacheChildLabel`, `LinkedChildren`, `flattenSubname`, `canLoadPartial`,
  `redirectSubName`, `Part.getRelatedElements`, `searchSubShape` -- all
  present under `App/Link*`, `App/DocumentObject*`,
  `Mod/Part/App/AppPartPy.cpp`). So the proxies attach and auto-solve may
  fire. The importer should read the same raw properties, which works either
  way, and set `AsmCmdManager.AutoRecompute = False` for the duration if that
  module is loaded.

### B.2 Object mapping

| Assembly3 | ours | notes |
|---|---|---|
| Assembly (top level) | `Assembly::AssemblyObject` + `Assembly::JointGroup` | same Label, Placement. `SolverType` dropped. `BuildShape` dropped (compound is the default). `Freeze` -> see last row |
| Assembly inside a part group (as itself) | nested `Assembly::AssemblyObject` (upstream creates these with `newObject` inside an active assembly, `Assembly/CommandCreateAssembly.py:73-82`) | converted first (children-first) |
| `App::Link` to an Assembly in a part group | `Assembly::AssemblyLink`, `Rigid = True`, `LinkedObject` = converted child | Assembly3 sub-assemblies are always rigid, which matches Rigid exactly. AssemblyLink mirrors the child's components as local links (`Assembly/App/AssemblyLink.cpp:329-490`), so references must be remapped to the mirrored child (B.4). Find it by scanning the AssemblyLink's children for the one whose `LinkedObject` is the source |
| AsmPartGroup children: `App::Link`, features, App::Part | moved into the AssemblyObject as is | parts are already links; nothing to re-create |
| link array part (`ElementCount > 0`) | the same `App::Link` array; each instance is a component through `App::LinkElement` | upstream addresses arrays only through `ElementList` (`Assembly/UtilsAssembly.py:124-131`). A collapsed array (`PlacementList` only, `ShowElement` false, `asm3/assembly.py:1790-1813`) must be expanded first. Assembly3 index `i` -> element `i` |
| AsmPlainGroup (folder) | `App::DocumentObjectGroup` inside the assembly | upstream recurses into groups (`Assembly/App/AssemblyUtils.cpp:756-814`) |
| AsmWorkPlane | a datum plane (or `Part::Feature` with its stored shape) | `Fixed` -> GroundedJoint |
| draft wire or circle as a part, moved point by point (`asm3/solver.py:144-221`) | none | keep the object as a fixed part; drop its sketch constraints |
| AsmElementGroup / AsmElement / AsmElementSketch | none (consumed) | resolved into joint references (B.4) |
| AsmConstraintGroup / AsmConstraint / AsmElementLink | joints in the JointGroup (B.3) | `Disabled` -> `Suppressed` (`App::SuppressibleExtensionPython`, `Assembly/JointObject.py:178`) |
| AsmRelationGroup / AsmRelation | none | derived data; delete |
| `DerivedFrom` | none | import the inherited parts as grounded parts, report it |
| `Freeze = True` | convert anyway, report it | the part group's stored `Shape` is the frozen result. If the parts are unavailable (partial load), offer to import the frozen compound as one grounded `Part::Feature` |

### B.3 Constraint mapping

The type comes from the persisted `_ConstraintType` integer
(`asm3/constraint.py` class `_id`s). Notes on how one constraint becomes
joints:

- **Element counts.** One Assembly3 constraint with N elements becomes N-1
  joints:
  - Multi: (E1,Ei);
  - Cascade (`Cascade` true): (Ei,Ei+1);
  - see `asm3/constraint.py:1232-1327`.
  - Pairs whose two elements are on the same part are skipped, as Assembly3
    does (`asm3/system.py:213-214`).
- **Which joint wins.** A Fixed, Revolute or Cylindrical joint is "fully
  satisfied", so its two JCS must coincide at import (B.4).
- **Where the source is.** "Assembly3 code" points at the SolveSpace
  composition in `asm3/system.py`.

| id | Assembly3 | ours | fidelity |
|---|---|---|---|
| 0 | Locked, face / non-linear edge (`asm3/constraint.py:968-1008`) | GroundedJoint on the part | exact |
| 0 | Locked, vertex | Ball joint to a grounded datum point at that vertex | exact, adds 2 objects |
| 0 | Locked, linear edge | Revolute joint to a grounded datum line along it | exact, adds 2 objects |
| -- | no Lock anywhere | GroundedJoint on the part of the first element of the first constraint (`asm3/constraint.py:582-592`); also on parts with no or read-only Placement (`:539-547`) | exact |
| 45 | Attachment | Fixed, JCS at the two element frames, no offset (`asm3/constraint.py:681-684`) | exact |
| 46 | AttachmentOffset | Fixed, element-link offsets folded into Offset1/2 (`asm3/assembly.py:1864-1880`) | exact |
| 35 | PlaneCoincident, `LockAngle` false | Revolute. Assembly3 is "origin1 = origin2 + R2*(OffsetX, OffsetY, Offset); normals parallel" (`asm3/system.py:316-361`) -> Offset2 = Placement((OffsetX, OffsetY, Offset)) | exact |
| 35 | PlaneCoincident, `LockAngle` true | Fixed, Offset2 = Placement((OffsetX, OffsetY, Offset), Rotation(Angle, AnglePitch, AngleRoll)) (`asm3/system.py:172-183`) | exact |
| 36 | AxialAlignment, `LockAngle` false | Cylindrical (point on line plus parallel, `asm3/system.py:397-421`) | exact |
| 36 | AxialAlignment, `LockAngle` true | Slider, with the angle as an Offset2 rotation | exact |
| 37 | PlaneAlignment, `LockAngle` false | Distance (PlanePlane -> `ASMTPlanarJoint`, `Assembly/App/AssemblyObject.cpp:1314`), Distance = `Offset` | exact |
| 37 | PlaneAlignment, `LockAngle` true | Distance as above; the angle lock is **dropped** (we have no planar joint with locked rotation) | approximate, reported |
| 2 | SameOrientation | **none**. Nearest is Parallel, which loses the twist | approximate or drop, reported |
| 291 | MultiParallel | Parallel per pair (plane-plane, line-line); Perpendicular for plane-line (`asm3/system.py:423-437`); `LockAngle` -> as SameOrientation | exact without LockAngle |
| 27 | Angle | Angle (`Supplement` -> 180 - Angle) | exact in 3D; the optional third (workplane) element is dropped |
| 28 | Perpendicular | Perpendicular | exact in 3D |
| 1 | PointsCoincident | Ball (or Distance PointPoint 0, `:1282`) | exact in 3D; projected 2D form (third element) dropped |
| 3 | PointInPlane | Distance PointPlane, 0 (`ASMTPointInPlaneJoint`, `:1380`) | exact |
| 7 | PointsPlaneDistance | Distance PointPlane, `Distance` | exact |
| 4 | PointOnLine | Distance PointLine, 0 (`:1406`) | exact in 3D |
| 8 | PointLineDistance | Distance PointLine, `Distance` | exact in 3D |
| 5 | PointDistance | Distance PointPoint, `Distance` | exact in 3D |
| 44 | PointsDistance (cascade) | Distance PointPoint chain | exact |
| 26 | PointsOnCircle (`_p`, `_c`, `asm3/constraint.py:1537-1548`) | Distance between the point and the circle's cylindrical face if one is referenced; else **none** | approximate, reported |
| 39 | Colinear | Cylindrical | exact in 3D (Assembly3 treats it as a sketch constraint with optional workplane) |
| 48 | Symmetric | **none** | dropped, reported |
| 38 | SketchPlane; 6, 9-25, 30, 33, 34 (sketch and 2D families on draft geometry) | **none** | dropped: they shape draft wires, not place parts |
| 40-43 | Measure* | **none** as joints; optionally core Measure objects | dropped or converted, reported |
| -- | `Multiply` true on 35 / 36 | per array instance i: one Revolute / Cylindrical between `Array.i` and the i-th paired edge. Pair by nearest position at import, as Assembly3 does (`asm3/assembly.py:2585-2657`); the saved file is solved, so distances are ~0. `NoExpand` limits the second side to the one edge | exact; N joint objects |

### B.4 Element references -> Reference1/2, Placement1/2, Offset1/2

**Step 1: resolve the chain without Assembly3 code.**

Start from an ElementLink and follow it to a part-group-relative path.
Three file generations must be read:
1. Current: `ElementLink.LinkedObject` is the local AsmElement itself, or
   `(ElementGroup, "$Label.")`. `AsmElement.LinkedObject` is
   `(part, "Body.Face3")`.
2. Older: the AsmElement points at `(PartGroup, "Part.Body.Face3")` (the
   `migrate` cases, `asm3/assembly.py:631-646`).
3. Oldest, as in the sample: the ElementLink points straight into a child
   assembly, `(PartGroup, "Link005.Elements001.$Element011.")`, and the
   groups are `App::FeaturePython` (`asm3/assembly.py:1916-1929`,
   `:4111-4154`).

Resolve token by token:
- `N.` -> `Group[N]` of an assembly (0 constraints, 1 elements, 2 parts);
- `$L.` -> the child of that group with Label L;
- `Name.` -> the object by name (through `App::Link` to its target, for a
  child assembly);
- a final `FaceN` / `EdgeN` / `VertexN`, or a `;#` mapped name, is the
  element.

Recursing through child elements, as `getElementSubname`
(`asm3/assembly.py:937-972`, `:2050-2082`) does, gives
`[subAsmLink, ..., part, "Body.Face3"]` plus every Offset met on the way:
the Element `Offset` and the ElementLink `Offset`. An Element with
`Detach = True` contributes its stored `Placement` instead of geometry.

**Step 2: map to our reference.**

`Reference = [component, [sub, sub]]` (`Assembly/UtilsAssembly.py:1273-1346`).

- The component is the converted part: the `App::Link`, the
  `App::LinkElement` of an array, or the `AssemblyLink` for a rigid child.
- For a rigid child, `sub` continues through the AssemblyLink's *mirrored*
  child link names. For a nested AssemblyObject it continues through its
  real children.
- The vertex slot repeats the element itself (`[sub, sub]`). That asks
  `findPlacement` for the element's own centre: circle centre, line
  midpoint, a face's centre of gravity, the cylinder axis point
  (`Assembly/UtilsAssembly.py:1082-1127`).
- Keep any mapped name: the property recomputes its shadow.

**Step 3: compute Offset1/2 so the joint holds at the saved poses.**

The two systems put the frame in different places:

| element | Assembly3 (`asm3/utils.py:362-522`) | ours (`findPlacement`) |
|---|---|---|
| planar face | position: bounding-box centre | position: centre of gravity |
| line | rotation: Z onto the direction | rotation: `Part.Plane(normal=dir).Rotation`, so a different X |
| cylinder | position: `surface.Center` | position: axis point at the centre of gravity |
| circle, rotation of a face | same as ours | same as Assembly3 |

So a straight copy would shift parts. For each connector k:

```
G_asm3_k = PartGlobal * asm3ElementPlacement(shape) * ElementOffset * ElementLinkOffset
G_ours_k = UtilsAssembly.getJcsGlobalPlc(findPlacement(ref_k), ref_k)   # with Offset_k = identity
Offset_k = G_ours_k^-1 * G_asm3_k
```

The global helpers are at `Assembly/UtilsAssembly.py:358-363`. Then add
the type's own offset from B.3 to Offset2, and for Revolute, Cylindrical and
Slider pick the sign of Z (flip Offset2 by 180 deg about X) that matches the
current poses: Assembly3's "parallel" accepts both senses.

Result:
- the imported joint is satisfied at the saved poses;
- it keeps following the geometry through the reference;
- a re-solve should move nothing.

The alternative, `Detach_k = True` with `Placement_k` set directly, is exact
but frozen, and is the fallback when `findPlacement` returns identity
(unsupported geometry).

### B.5 Shape of the import, and its steps

**Shape.** A command, **Assembly_ImportAssembly3** ("Convert Assembly3
assemblies"), works on the open active document, in place, as one
transaction:
- Assembly3 documents are usually multi-document. The sample links five
  external part files with relative `PropertyXLink`s. Converting the open
  document keeps those links valid; the user then saves, or saves as.
- A file importer cannot be registered for `.FCStd`.
- An optional second entry opens a file with the shim of B.1 and runs the
  command.
- A module function `AssemblyImportAsm3.convert(doc, report=None)` lets
  tests and FreeCADCmd run it headless; it needs no GUI.

**Where.** New fork-only files:
- `Assembly/AssemblyImportAsm3.py` for the logic;
- `Assembly/CommandImportAsm3.py`, listed in `Assembly/CMakeLists.txt:21-38`
  and imported in `Assembly/InitGui.py:65-75`;
- the command name added to `cmdListMenuOnly` next to Assembly_ExportASMT
  (`Assembly/InitGui.py:95-99`).

The ASMT stub `Assembly/AssemblyImport.py:25-32` is unrelated: leave it.
Merge cost: none, as no upstream file changes beyond two list entries.

**Steps.**

1. **Discover.** Find the assemblies structurally (B.1). Snapshot every
   part's global placement. Turn off `SolveOnRecompute` and Assembly3's
   `AutoRecompute` for the duration. Open one transaction.
2. **Order** children first: an assembly before any assembly whose part
   group contains it or a link to it.
3. **Build containers**, per assembly:
   - create the AssemblyObject and JointGroup;
   - move the part-group children in (folders as DocumentObjectGroups);
   - convert links to Assembly3 assemblies into AssemblyLinks (Rigid);
   - expand collapsed link arrays.
4. **Resolve elements** (B.4 step 1) for every ElementLink. Record failures
   as "broken element" with the label path.
5. **Emit joints** (B.3).
   - Create each joint directly, `Joint(obj, typeIndex)` plus property
     writes, **not** through the joint task. The task pre-solves and solves
     per joint (F2, F3: 317 s for 1000 joints).
   - Set `Reference1/2`, then `Offset1/2` (B.4 step 3).
   - Labels follow the Assembly3 constraint label.
   - Copy `Disabled` to `Suppressed`.
   - Expressions on Assembly3 constraint properties (`Offset`, `Angle`, ...:
     wiki Constraint-with-Expressions) are re-bound to the joint property
     that received the value, where one exists. Otherwise report them.
6. **Ground** (B.3, Lock rows), including the implicit first part.
7. **Remove** the Assembly3 objects: relation, constraint and element
   groups and their children, then the old assembly and part group. This
   is done once, as `removeObjectsFromDocument`. An option keeps them
   hidden instead, for comparison.
8. **Solve once.** Compare every part's global placement with the snapshot
   (tolerance 1e-6 mm / 1e-6 rad). Close the transaction, or abort and
   restore on failure.
9. **Report.** Per assembly: joints created, constraints dropped or
   approximated with their reason, broken elements, parts that moved on the
   verification solve. Print to the console, and in the GUI show it in a
   dialog. Keep the dropped list in a property on the AssemblyObject so it
   survives saving.

### B.6 Test cases

Samples:

- **`essais_asm3.zip`**
  (https://github.com/realthunder/files/raw/master/misc/essais_asm3.zip,
  not kept in this tree): a wind
  turbine sub-assembly, FreeCAD 0.17 era.
  - `asm3_essai_methodo.FCStd` holds two assemblies, 12 constraints, 31
    elements, 22 element links and one AsmElementSketch.
  - Five external part documents: `masselotte`, `axe_masselotte`,
    `elast_masselottes`, `ogive`, `roue_dentee_ogive`.
  - It exercises:
    - the oldest reference format (B.4, generation 3);
    - App::FeaturePython part groups (pre-`upgrade`);
    - one child assembly used directly (`Assembly001` in `Parts`) **and**
      through two links (`Link004`, `Link005`);
    - PlaneCoincident x8, Colinear x2, Locked x2 and PointsPlaneDistance on
      a sketch vertex.
  - Multi-document `PropertyXLink`s.
  - The `.FCStd1-8` backups are older revisions of the same file.
- **The wiki's `data/wind-turbine-parts.zip`**: the same parts, no
  assembly.
- **Issue attachments**: 75 URLs gathered in
  `docs/AssemblyFromAsm3-samples.txt`. Pick by constraint type, e.g.:
  - `files/6176139/freecad.asm3.angle.issue.FCStd.zip` (Angle);
  - `files/3073178/point-in-plane-wrong.fcstd.remove-me.zip` (PointInPlane);
  - `files/5434606/HEVO2.zip` (a larger model);
  - `files/12513761/empty-assembly.FCStd` (degenerate).
- `asm_LegoHouse.zip` in the same repo is **not** Assembly3 (0 asm3
  references; it looks like Assembly4) -- use it as a negative case:
  "nothing to convert".
- There are no samples in the Assembly3 repo itself or its history.

Tests (Python, `TestAssemblyWorkbench`, headless where possible; none may
need Assembly3 or `py_slvs` installed):

1. **Synthetic, per constraint type.**
   - Build the Assembly3 structure by hand: FeaturePython objects with the
     same dynamic properties, `Group` layout, `_ConstraintType`, Offsets and
     Proxy None -- what a restored file looks like.
   - Two `Part.makeBox` links placed in a satisfied pose.
   - Assert, for every row of B.3:
     - joint type and count;
     - re-solve moves nothing;
     - after perturbing one part, a solve restores it.
2. **Cascade versus Multi** with three elements: 2 joints each, with the
   right pairs.
3. **Locks**: face, vertex, linear edge, and none (implicit first part).
4. **Multiply**: a 1-element array plus a plate with 6 equal coplanar holes
   gives a 6-element array and 6 joints, and no part moves.
5. **Nested**: a child assembly used directly and through two links gives a
   nested AssemblyObject plus two Rigid AssemblyLinks. A reference through
   a link resolves to the mirrored child.
6. **Reference generations 1-3** (B.4) each resolve to the same component
   and sub.
7. **Offsets**: Element `Offset`, ElementLink `Offset` and `Detach` give an
   exact pose.
8. **Dropped and approximated types** appear in the report and in the stored
   property.
9. **The sample file** (needs the archive, so it belongs in an opt-in or
   data-gated test):
   - opens with Proxy None;
   - converts with zero broken elements;
   - Colinear maps to Cylindrical;
   - the solve moves nothing beyond tolerance.
10. **With the shim** (B.1): no restore errors, and the same result as
    without.
11. **Undo** restores the original document exactly. Abort on an injected
    failure leaves the document untouched.


## C. Summary of recommendations

| borrow | where | cost | merge cost |
|---|---|---|---|
| write-back tolerance; skip untouched; rollback | AssemblyObject C++ | small | low |
| children-first rigid solving, rigid as the fast path | AssemblyObject / AssemblyLink | medium | medium (Flexible stays upstream's) |
| solve once per transaction, out of execute | an observer + preference | medium | medium |
| conflict report from failed handles; structural redundancy pre-pass | AssemblyObject, OndselSolver mapping | medium | low |
| Freeze with partial load for sub-assemblies | new property + `canLoadPartial` overrides | medium | low (additive) |
| auto-fix of broken joint references | Joint.execute | small-medium | low |
| QuickMove, drag trace, dragger for Offset | new commands | small | none |
| joint selection validation messages | joint task | small | low |
| pattern command (array + N joints) | new command | small | none |
| Assembly3 importer (B) | new files | about 1000 lines plus tests | none |
| Element and ElementBinding objects (revised 2026-10-24) | `docs/AssemblyObjectModel.md` | large | the object model leaves upstream's (decision 8) |
| **not** the SolveSpace interface, **not** relation objects | -- | -- | -- |
