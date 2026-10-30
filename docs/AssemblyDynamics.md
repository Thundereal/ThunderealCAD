# Assembly: contact and rigid-body dynamics (a later phase)

Status (2026-10-23): this is a design study only. No code, build or FreeCAD run
stands behind it. Branch `AssemblyPort`.

Progress (2026-10-28): both queued pieces of work are built -- Chrono's
`NeutralLink` (queued (a)) and `PointOnCurve` (queued (b)). Phase D0 is
built (2026-10-28):
- Built: mass properties from shape and material (`App/MassProperties.{h,cpp}`,
  `AssemblyObject.getMassProperties()`, tests `AssemblyTests/
  TestMassProperties.py`). Per component, from the shape features it is made
  of (itself, what a link links to, a container's children; a Body is one,
  elements are none), each solid's `BRepGProp::VolumeProperties` at its
  feature's `ShapeMaterial` density, or the default material's
  (`Mod/Assembly/DefaultMaterial`, a card's UUID, Aluminum Generic unless
  set), the component then "Assumed". The inertia is about the centre of
  mass, along the assembly's axes; components and the total combine by the
  parallel-axis theorem (`MassProperties::add`). **The Default card is no
  material**: every shape has it until one is chosen, and its density,
  1 kg/m^3 (`Standard/Default.FCMat`), is a placeholder -- taken at its word,
  every unchosen part weighed a thousandth of water.
- Built: the Physics group and `MassOverride` (`App/Physics.{h,cpp}`). C++
  objects, as `docs/AssemblyObjectModel.md` decision 0 has every Physics
  item, not the Python features sec 5.2 sketches: `PhysicsItem`, which the
  Physics aux group (`AssemblyGroup::PhysicsRole`, made on demand:
  `getGroup("Physics", True)`) shows, and `MassOverride` on it --
  `Component` (sec 5.2's `Part`, renamed: a component may be a link or a
  sub-assembly), `Mode` {Density, Mass, Full}, `Density`, `Mass`,
  `CenterOfMass` (the component's frame), `Inertia` (six floats, kg mm^2,
  the component's axes: there is no inertia unit to type them with).
  Density: the shape at that density. Mass: the shape's centre, its inertia
  scaled to that mass. Full: nothing from the shape, the centre and the
  inertia turned into the assembly's frame (R I R^T). The last of several
  overrides of a component is used, with a warning.
- Built: the neutral `Inertia` (`Solver/Model.{h,cpp}`, sec 6.1 as
  sketched): `Model::version` 2 and `Model::dynamics`, a `Dynamics` of
  `gravity` and `inertias` for now, empty in every kinematic build;
  `moved()` (an inertia in another frame, R I R^T) and `add()` (the
  parallel-axis sum, the weakest origin kept); `sameExceptDragWeights`
  compares the block. The builder, `AssemblyObject::addDynamics()`, sums
  the components' mass properties per solver body (a rigid group's members
  are one, by `objectPartMap`) and gives them in the body's frame; the
  Fixed-joint bundling (`groupedRigidly`, SI sec 6.21) merges its members'
  into the kept body's, and a body held to the ground drops its own. From
  Python, `getBodyInertias()` builds the model as a solve does and returns
  the records. Tests: `DynamicsModel.*` (gtest, a session that keeps the
  model it is given checks the bundling) and `TestMassProperties.
  test_body_inertias`.
- Built: the unit contract's factors (`Solver/Units.h`, namespace
  `Solver::SI`): internal units times the factor is SI, the table of sec 8.1
  and velocity, acceleration, energy, power and the dampings beside it, and
  `impactStiffness(e)` = force / length^e, the Adams scaling, with
  `impactDamping`. The test (`DynamicsModel.unitFactorsAreFreeCADs`) checks
  each against FreeCAD's own units -- one SI unit parsed by
  `Base::Quantity` is the factor's inverse -- so the table cannot drift
  from them. Chrono's kinematic solves keep mm, their tolerances set for it;
  the dynamics D1 adds convert.
- Built: the Mass Properties command (`Assembly_MassProperties`,
  `CommandMassProperties.py`, on the tool bar after Create Simulation): a
  table of the components -- material, density, volume, mass, centre of
  mass -- with the assumed ones in orange and a tooltip saying why, a total
  row, the whole's inertia below, and an Override box per row. Ticking it
  makes a `MassOverride` in Physics, in Mass mode at the mass shown (so
  nothing changes until it is edited), and selects it for the property
  view; unticking removes it; both undoable. The table follows the
  document (an observer, deferred to the next event loop turn). **A table
  refreshed from an item's own signal crashed**: ticking emits itemChanged
  inside the item's setData, and a fill there clears the table and the item
  Qt is still in -- an access violation in `QTreeWidgetItem::setData`, the
  first run getting away with it.

Phase D1 is under way (2026-10-28):
- Built, step 1 of sec 4's order: a Dynamic analysis of an unpowered
  mechanism under gravity. The C++ `Simulation` has `AnalysisType`
  {Kinematic, Dynamic}, `Gravity` (the document's coordinates, mm/s^2,
  turned into the assembly's frame for the model) and `GravityEnabled`;
  `SimulationSpec` has `analysis` and `maxStep` (0: the backend's, 1 ms on
  Chrono). A Dynamic study adds the dynamics block (`addDynamics()`) and runs
  on the first backend whose `Capabilities::dynamics` is set -- Chrono, now
  -- whichever the assembly solves with. `centredOnJoints` moves the
  inertias with the frames it moves. `ChronoSession::simulateDynamics()`:
  the model in SI (`Solver::scaled`, every length times `SI::length`), each
  body's frame at its centre of mass (a `ChBody`'s inertia is about its own
  frame, and the kinematic build makes ChBody, not ChBodyAuxRef), a system
  of its own with the real masses and full inertia tensors, gravity, HHT
  (alpha -0.2) on the session's direct solver (LUThenQR), the poses back.
  Refused, with the reason in the report: no masses, a free body without
  mass, and what a dynamic run does not take yet -- motions (D1 step 3),
  limits (stops) and cams (a curve is not scaled).
- **The accelerations at the start are assembled first**
  (`DoAssembly(FULL)`): HHT takes those it is given, none from rest, and the
  run lagged a fraction of a step behind -- the pendulum's period long by
  0.2% at 1 ms, the error first order in the step. Assembled: 0.004% at
  1 ms, second order (0.001% at 0.5 ms).
- Test: `TestDynamics` -- a rod hinged at one end, released horizontal,
  swings down in the quarter period of a pendulum let go at 90 deg
  (sqrt(I / (m g d)) K(sin 45 deg)), within 0.05%, the hinge held to 1e-6
  mm, the lowest point 50 mm below it; nothing moves without gravity; a
  motion is refused.
- Built: the Simulation task panel's Analysis box (`CommandCreateSimulation.
  py`, added in code above the .ui's Motions): Kinematic or Dynamic, and a
  Gravity box; the Motions greyed in a Dynamic study, which does not run
  them yet; and a run that fails says so under Generate (before, the panel
  went on to a player of no frames). Upstream's Python simulation, not
  converted, has no analysis: the box is disabled for it.
- Built, step 2: springs and dampers.
  - `SpringDamper` (`App/Physics.{h,cpp}`), a Physics item: two ends, each
    a `Component` and a `Point` in its coordinates (no component: the
    ground, the assembly's coordinates) -- sec 5.2's `Reference1/2` as two
    picked points for now, not element bindings; `Stiffness`, `FreeLength` (0:
    the length when the run starts), `Preload` (pushing the ends apart),
    `Damping` (a quantity in kg/s, N s / m: FreeCAD has no damping
    property). Linear: the exponents of sec 5.2 later.
  - `Simulation::Elements`: the force elements a study uses. A mass
    override counts whatever a study lists: it is the assembly's mass, not
    a study's. The panel lists the springs, ticked to use; a new study ticks
    them all, and a spring made by the command is ticked in every Dynamic
    study (the "on unless unticked" of sec 5.1, for an element made outside
    a study).
  - Neutral: `ForceKind::SpringDamper`, a `ForceElement` on two frames
    (`Dynamics::forces`), so the scaling to SI, the centring and the
    bundling carry it as they carry a joint's frames; `scaled()` scales
    its rest length and preload (a force scales as a length when mass and
    time stay), its stiffness and damping not. The builder,
    `addForces()`, makes the frames on the components' bodies; a part a
    spring alone holds becomes a body of its own (before `addDynamics()`
    weighs the bodies). Chrono: a `ChLinkTSDA` (its force on body 1 along
    the line from body 2: f - k (l - l0) - c dl/dt, a positive f pushing
    apart).
  - GUI: Spring / Damper (`Assembly_CreateSpring`,
    `CommandCreateSpring.py`): two picked elements, each end a vertex's
    point, a circle's centre or an edge's or a face's centre of mass,
    free at the distance between them, 1 N/mm; drawn as a coil of eight
    turns between its ends (`AssemblyGui::ViewProviderSpringDamper`),
    redrawn as their components move -- dragged, solved, played back.
  - Tests: a 10 mm cube on a slider over a spring of 10 N/m swings its half
    period, pi sqrt(m / k), within 0.1%; damped at 5% of critical, each
    swing smaller by the logarithmic decrement within 1%; a spring the study
    does not list does nothing.
- Built, step 3 in part: the motor modes. `Motion::Control` {Position,
  Speed, Effort} (the motion dialog's Control choice): Position, the
  angle (rad) or distance (mm), as before; Speed, their rate; Effort, the
  torque (N m) or force (N) -- the units a user writes, which the builder
  takes to the model's internal mN mm and mN (`scaledFormula`). Neutral:
  `Driver::rotationControl` / `translationControl` (`MotorControl`), and
  `scaled()` scales a driver's formula by what it gives (a distance, a
  speed or a force as a length, a torque as a length squared). Chrono's
  dynamic run makes `ChLinkMotorRotationAngle / Speed / Torque` and
  `ChLinkMotorLinearPosition / Speed / Force`, spindle and guide free (the
  joint holds the rest), so motions are no longer refused there; a
  Kinematic study refuses a Speed or an Effort motion (as a bad formula).
  Tests: Position holds the angle to its formula against gravity (1e-6
  rad), Speed turns at its rate (1e-4), Effort accelerates by tau / I about
  the hinge, a Kinematic study refuses Effort.
- Built, step 3: the reactions and their plots. A run reports values with
  each frame besides the poses: `Solver::Channel` {source, quantity,
  width}, `TrajectorySink::channels()` once and `values()` before each
  frame (the centring and rigid-group wrappers pass them on), kept in
  `Trajectory::channels / values` and read by
  `AssemblyObject.getSimulationResults()` (Times, and Channels: Object,
  Name, Quantity, Values). Chrono's dynamic run reports, per document
  joint, the reaction Force and Torque on the part of its **first
  reference** (frame I), in the assembly's axes, the torque about frame
  I's origin -- not sec 8.4's "on J, in frame I": the part a user picks
  first is the one asked about, and one set of axes plots across joints;
  its model joints' links summed. A Chrono link gives the wrench on either
  body in its own frame (`GetReaction1/2`); our `NeutralLink` (gears,
  screws, the bevel gear) gives it from its rows, Cq^T times the
  multipliers, a body the rows do not move taking the opposite of the
  rest. Also `MotorTorque` / `MotorForce` (`GetMotorTorque/Force`: an
  Effort motor reports its formula) and a spring's `SpringForce`
  (positive pushing apart) and `SpringLength`. In mN, mN mm and mm. The
  Simulation panel's Results box plots a channel over the run (a vector's
  X, Y, Z and its length, in N, N m or mm) with the frame shown as a
  cursor and its values under it. Tests: the hinge reaction on the
  grounded block is the opposite of m (a - g) for the swinging rod, a from
  the angle (1e-3 of m g), with no torque; a Speed motion holding the rod
  level applies m g d, and an Effort motion of what it reported holds the
  rod level; a spring's force is -k (L - L0); a rack falling along a
  pinion is pushed back by m (g - a), a = g m / (m + I / r^2) (the
  NeutralLink path, 1e-4); a kinematic run reports no channels. That test
  found the dynamic run never committed the neutral links' angles (the
  kinematic run does, at each accepted pose): a pinion past half a turn
  read as turned the other way, and the rack jumped a pitch. It commits
  after every step now.
- Built, step 4: the static analysis. `AnalysisType` Static
  (`Analysis::Static`): where the parts settle under gravity, the
  springs and the Effort motions, at each output time -- a quasi-static
  sweep, a Position motion's value at that time held; one frame when
  TimeEnd is TimeStart. It takes the masses, gravity and the elements as a
  Dynamic study does, and refuses a Speed motion (it holds nothing still).
  Chrono's static Newton (`DoStaticNonlinear`) was not used: only its mates
  carry a tangent stiffness, so a pendulum has none in its free turn, and
  a Newton step goes to the nearest equilibrium, stable or not. The run is
  dynamic relaxation with kinetic damping instead: HHT steps from rest
  with the time held, every body set to rest at each peak of the kinetic
  energy, settled once a whole cycle moves no frame by more than the
  study's GlobalErrorTolerance (mm, a turn counted at the body's farthest
  frame); then the acceleration analysis at rest gives the reactions. It
  descends, so it finds a stable equilibrium -- or stays on an unstable one
  it starts exactly on. 50000 steps without settling fail the study ("a
  part keeps moving"). Tests: a pendulum from horizontal hangs straight
  down (1e-4 mm), the hinge carrying m g; a block on a spring sags by m g /
  k (1e-5 mm), the spring's force m g; an Effort of half m g d holds the
  rod 60 deg below horizontal at every frame (1e-3 deg); a Speed motion is
  refused; a block on a slider with nothing under it fails in about a
  second.
- D1 is complete as sec 4 orders it. Its leftovers follow.
- Built: the joints' limits as rigid stops, which the dynamic and static
  runs refused before. Events between steps, since HHT with the direct
  solver takes no unilateral constraint: a step that takes a limit's value
  past a bound adds a link holding it there (`boundLink()`, the kinematic
  run's, shared), and the positions and velocities are projected onto it
  (`DoAssembly` POSITION | VELOCITY, residual 1e-14: Chrono's velocity
  projection adds C over its own 1e-6 s) -- the plastic impact, the
  impulse leaving the least kinetic energy. A restitution e
  (`Simulation.Restitution`, the panel's Analysis row, Dynamic only) takes
  that impulse 1 + e times and lets the stop go; an impact slower than
  gravity gives in two steps stays plastic, so a part resting on a stop
  does not chatter. A held stop is let go once its reaction on frame I's
  body pulls rather than pushes along zI. A static run settles on its
  stops without bouncing. A stop's force is its joint's, in the reaction
  channels. Tests: a block dropped 4 mm onto its stop stays there, the
  slider carrying m g; restitution 1 and 0.5 bounce it back 4 mm and 1 mm
  (1%: measured 4.011 and 1.003 at 1e-4 s steps, the crossing caught a
  step late); a static study settles it on the stop; a pendulum limited to
  30 deg either way swings onto the stop and stays, dynamic and static.
- Built: torsion springs. `Assembly::TorsionSpring` (the Physics group;
  Assembly_CreateTorsionSpring, on the one Revolute or Cylindrical joint
  selected, 1 N m per radian, free where the run starts, ticked in every
  Dynamic and Static study): Joint, Stiffness (a torque per radian, mN mm
  internally), FreeAngle (the joint's turn from where the run starts at
  which it is free), Preload (a torque), Damping. On the joint's own model
  frames (the builder finds the joint's Revolute or Cylindrical record by
  source; on none, a warning and nothing), `ForceKind::TorsionSpring`,
  scaled as a torque (length squared), its angle not at all. Chrono's
  `ChLinkRSDA` with frame I's body first and J as the motors see it, so
  its angle is the joint's, as the limits read it; the rest angle the
  start's plus FreeAngle. Channels `SpringTorque` (positive turning the
  joint up) and `SpringAngle` (rad; the panel shows deg). Tests: free 0.1
  rad from the start without gravity, the rod swings 0 to 0.2 rad by half
  periods of pi sqrt(I / k) (1e-3), its torque -k (turn - 0.1); with
  gravity, a static study holds it where k turn = m g d cos(turn) (1e-6
  rad); on no such joint it does nothing.
- Built: cams in dynamic and static runs, which refused them. `scaled()`
  now scales the curves too (`scaledCurve()`, Curve.h: the point and both
  derivatives times the factor, the parameter kept; the projection's
  tolerances are relative to the parameter, so a curve in metres projects
  as one in mm did). Test (TestCamJoint): the disk cam driven at 10 rad/s
  in a Dynamic study without gravity -- the follower keeps the lift (1e-6
  mm), and the cam joint's reaction on the cam is minus the follower's
  mass times its acceleration along the slider (2e-3 of the largest), a
  point follower and a roller.
  That test found the start of a dynamic run wrong when something moves
  there, in three ways, each fixed:
  - The start's accelerations came from one FULL assembly pass, whose
    velocity projection takes the jump from rest to the motions' speeds
    over its own step of 1e-6 s: the cam's start read ten thousand times
    its force. Now positions and velocities are projected first, then the
    FULL pass (`assembleFull`), the 2e-6 s of acceleration the two
    projections add to the velocities taken back.
  - Chrono's assembly analysis has no accelerations that motion gives (its
    velocity projection holds the positions): HHT started from them, and a
    whole step left the velocities off, the cam's force 15% off at the
    first frame, 1.5% at the second. The run now starts in steps growing
    from h / 1024 (64 of them, then h / 16 ... h / 2, together the first
    step): 0.5% at the first frame, 0.06% at the second, 1e-5 from the
    third.
  - The start's reactions are read after 32 and after 64 of those smallest
    steps, a line through the two taken back to the start (0.08% on the
    cam); what the state alone gives (a spring's force, length, angle) is
    read at the start itself. A probe step whose state was put back did
    not do: HHT's multipliers carry a quarter of the step before's, and the
    probe disturbed the start for the later frames.
  The static run's reactions take the two-pass assembly too: from rest, a
  Position motion's rate had read as an impulse; now its inertia counts,
  which a slow sweep's is small.
- Built: a spring's ends on elements. `SpringDamper::Binding1/2`: an end on
  an element is the origin of the element's frame, the offsets applied, as
  a joint's side is -- through an element binding the spring owns, a child
  of the assembly made by `Joint::makeSide()` (the element found or
  declared, and shared; the binding new), claimed in the tree by the
  spring's view and deleted with it (`Joint::removeBinding()`, the
  convenience element too when nothing else binds to it). The end's
  Component and Point are then output and read-only: the component the
  binding is on, set with the binding, and the point in its coordinates,
  taken at each recompute (`Joint::resolveBinding()`, `getSide()`'s core
  made static), so a solve moving the component needs no recompute and a
  binding that does not resolve (a child not loaded) keeps the point it
  last had. Without a binding the end is the free point it was; making an
  end free leaves it where it is. `getEnd()` / `getEndPoint()` give an end
  either way, for the builder (`addForces()`) and the coil. Python:
  `setEnd(side, component, subname, vertex="")`, `clearEnd(side)`,
  `getEnd(side)`, `getEndPoint(side)` (`SpringDamper.pyi`).
  - Task panel (`CommandCreateSpring.TaskSpring`): each end picked in the
    3D view as a joint's side is (the vertex nearest the cursor, or the
    element's own frame: a face's middle, a circle's centre), a dashed
    line from the other end to where the hovered element would put it,
    Pick and Free per end, an offset along the element's normal (the
    binding's `Offset`); stiffness, free length with "Current length",
    preload and damping. The free length follows the distance between the
    ends until it is edited. The command opens it (ends already picked go
    to the ends), and so does a double click on a spring; Cancel undoes
    the whole edit, a new spring with it.
  - Found on the way: a side on an element its component declares (an
    App::Part, a Body, a sub-assembly: "$$Elements.$X.") was framed as a
    whole object -- the path names no sub-element -- so every such joint's
    frame was the element's placement, the identity, rather than its
    face's. `resolveBinding()` now frames it by its geometry when the
    element's chain of targets ends at a named sub-element.
  - Tests (`TestSpringEnds`, 13): ends on faces, read-only while bound;
    following a resize, a face a fillet renames (a body in a part), a
    vertex, the binding's offset, and the component moved without a
    recompute; a re-pick deleting the old binding; Free keeping the end;
    two springs sharing one element with their own offsets, the element
    going with the last; delete and undo; `makeSpring()` with either kind
    of end; save and restore; an end on a child not loaded staying on the
    link. TestDynamics: the oscillator with its ends on the faces swings
    as with the points (1e-9 mm). TestElementJoint: a joint through two
    components that declare elements frames the faces as convenience
    elements do. GUI (TestGuiCommands): the command on two picked faces,
    picking in the panel, Free and re-pick, Cancel, and a double click.
- D1's leftovers are all built. Sec 4's table lists more for D1 than its
  four ordered steps: bushings, forces and torques, initial velocities,
  redundancy removal, inverse dynamics in kinematic runs, CSV export,
  traces, the stale flag, and the run on a worker thread with cancel.
  Those come before D3 (the phase order is D0, D1, D3, D2), one at a time.
- **D3 is measured before it is built** (the user's call, 2026-10-29): what
  running out of process would buy here -- the runs' lengths, how often a
  run crashes or hangs the session, what a worker thread with cancel
  already gives -- decides it. If it is not worth it, D3 is skipped and D2
  follows D1.
- Built: applied forces and torques. `Assembly::AppliedLoad` (the Physics
  group, ticked per study as the springs are -- P3, sec 5.2's
  `AppliedLoad` moved from the Simulation for that): `Kind` {Force,
  Torque}; the application frame on an element through an element binding
  the load owns, or a free `Frame` on `Component` (the spring's ends'
  machinery, now `PhysicsItem`'s helpers: `makeEndBinding`, `resolveEnd`,
  `releaseBinding`, `syncComponent`); `Direction` in `DirectionAxes`
  {Frame: turning with the component, Assembly: fixed}; `Magnitude` (N or
  N m, the unit following Kind) times `Profile`, a formula of time in the
  motions' grammar (empty: 1); `Reaction`, the component taking the
  opposite load at where the frame was at the start (none: the ground).
  - Neutral: `ForceKind::AppliedForce` / `AppliedTorque`, a
    `ForceElement` with `direction`, `followsFrame` and a `magnitude`
    formula in mN or mN mm (Magnitude times the profile, one text, the
    record); frame J is the reaction's, at frame I's start. `scaled()`
    scales the formula as a force or a torque. A profile that does not
    parse fails the generation, naming the load.
  - Chrono: a `ChLoadBodyBody` subclass (`AppliedLoad` in
    ChronoBackend.cpp) in a `ChLoadContainer`: the wrench on frame I's
    body, its opposite on J's, the magnitude read at the time the load was
    last updated to -- each integrator stage's, so a profile converges at
    second order (halving the step quarters the error, measured 4.006),
    not first as a load held over a step would.
  - GUI: Force / Torque (`Assembly_CreateLoad`, `CommandCreateLoad.py`): a
    panel as the spring's -- where it acts (Pick, Free, the offset along
    the element's normal), Kind, Magnitude, Profile, Direction with
    Reverse, the axes, and the reaction (Pick, Ground); a double click on
    a load opens it. Drawn as an arrow on the frame's outer side (a pull
    from the point, a push to it) or a curled arrow about a torque's axis
    (`ViewProviderAppliedLoad`, `ArrowLength`).
  - Tests (TestDynamics): a constant force up a slider, z0 + F t^2 / 2m
    (1e-6 mm), at a free point and on a face; a sine profile against
    (F0/m)(t/w - sin(w t)/w^2), at second order; a torque on the hinged
    rod, T t^2 / 2I (1e-4 rad); a force at the rod's tip across it in its
    own axes against the closed form, and fixed in the assembly against
    theta'' = k cos(theta) integrated in the test (1e-4 rad); action and
    reaction on two blocks; a static sag (m g - F) / k; a bad profile; a
    load not listed. TestSpringEnds' TestLoadEnd (5): on a face, following
    a resize, the direction turning with the part or not, a torque's unit,
    Free, and deleting it beside a spring sharing its element.
    TestGuiCommands: the command on a picked face, and the panel.
- Built: bushings. `Assembly::Bushing` (Physics, ticked per study): its
  frame as a load's -- the frame half of the load moved into a base,
  `FramedItem` (`Component`, `Frame`, `Binding`; `getEnd`,
  `getEndPlacement`, `setEnd`, `clearEnd`, its own Python binding), which
  `AppliedLoad` and `Bushing` both derive from; `Other`, the component
  holding it (none: the ground); `TranslationalStiffness`,
  `RotationalStiffness`, `TranslationalDamping`, `RotationalDamping`,
  `Preload` and `PreloadTorque`, vectors along and about the frame's axes in
  the internal units (sec 5.2's table had no preload torque; it costs
  nothing). Free where the run starts: P3's rule that the start pose is the
  solved assembly.
  - Neutral: `ForceKind::Bushing` with `k6`, `c6` and `preload6`; frame J
    the other's, at frame I's start, as a load's reaction frame.
    `scaled()` scales the rotational values and the preload torque as a
    length squared, the preload force as a length.
  - Chrono: `ChLoadBodyBodyBushingGeneric` (a diagonal 6x6), not
    `ChLinkBushing`: a load gives the implicit integrator its numerical
    jacobian, where `ChLinkLock`'s spring forces carry none. Chrono's
    numerical jacobian leaves the load at its last perturbed state (the
    velocity's last column); the subclass (`Bushing` in ChronoBackend.cpp)
    computes it again at the state. Its channels: `BushingForce` and
    `BushingTorque`, on the component, in the assembly's axes.
  - GUI: Bushing (`Assembly_CreateBushing`, `CommandCreateBushing.py`): the
    load's panel made a base, `TaskFramed` (the frame's Pick, Free and
    offset, the other part's Pick and Ground, the transaction), with a grid
    of stiffness, damping and preload along and about x, y, z, in N/mm,
    N m/rad, N s/mm, N m s/rad, N and N m (plain spin boxes: a quantity
    box shows the unit schema's choice, a torque stiffness in joules). A new bushing is 1000 N/mm and
    100 N m/rad, damped at 0.5 N s/mm and 0.05 N m s/rad. Drawn as a ring
    of rubber in the frame's xy plane (`ViewProviderBushing`, `Size`); the
    load's and the bushing's view providers share `ViewProviderFramedItem`.
  - Tests (TestDynamics): the block on its slider held to the ground and
    preloaded, against a step's response, undamped and at 20% of critical;
    the rod's hinge with a rotational stiffness and a preload torque, about
    the frame's y; two blocks parting about a bushing between them at
    sqrt(2k/m), with the force channel; a static sag m g / k; not listed.
    TestSpringEnds' TestBushingEnd (4). TestGuiCommands: the panel.
- Built: initial velocities. `Assembly::InitialCondition` (and its
  Python variant, which the study's panel lists with the motions and edits
  through `InitialConditionDialog`), a child of the Simulation as a motion
  is -- a start state is the study's (sec 5.1): `Mode` {Joint, Component};
  a Revolute, Cylindrical or Slider `Joint`, its `MotionType` and `Rate`
  (deg/s or mm/s, the unit following the type); or a `Component`, its
  `LinearVelocity` (mm/s) and `AngularVelocity` (deg/s), in the assembly's
  axes. No `Locked` yet. A Dynamic study only: a static run starts and ends
  at rest, and a kinematic one has its motions.
  - The linear velocity is the component's origin's, not its centre of
    mass's (sec 5.2 said a body's): a point the user sees, and a frame,
    which survives the rigid groups' reduction where a body id does not.
  - Neutral: `InitialVelocity` {RotationZ, TranslationZ, Body} on frames
    -- a joint's frames I and J from its record, as a torsion spring's; a
    body's frame I at the component's origin. A rate on a joint a motion
    drives is left out with a warning.
  - Chrono: on the start assembly, a body's velocity set (its centre of
    mass's from v + w x (com - origin)); a joint's rate held by a Speed
    motor of the joint's kind while one velocity projection makes every
    velocity consistent with the joints -- the least mass-weighted change,
    so what a joint does not allow is taken out -- then the motor removed.
    The start's projections each add 1e-6 s of the forces' acceleration,
    and `assembleFull` takes back that one too.
  - Found on the way, a fault of the Speed motions since they were built:
    Chrono's speed motors hold their joint to the integral of the rate
    from an offset of 0, so a joint not at its zero -- a slider 2 mm open
    -- was pulled there at the start. The offset is now the joint's value
    at the start (`startHere`), for the motions and the temporary motors
    alike. A Speed motion's frames still run up to 2e-6 s of its rate
    ahead (measured, 2e-4 mm at 100 mm/s); not chased.
  - Tests (TestDynamics, 7): a component started up its slider under
    gravity, z0 + v0 t - g t^2 / 2 (1e-6 mm); one given a velocity across
    the slider and a turn, only the slide kept; a hinge's rate of 90 deg/s
    and a slider's of 100 mm/s, against the closed form and a Speed motion
    of the joint; the rod's own velocity given as a turn about the hinge,
    which needs no change (the origin to centre of mass conversion); a
    static study ignoring one; and the Speed motion from 2 mm open.
    TestGuiCommands: the dialog, a joint's rate and then a component's.
- Built: redundancy removal, the `Remove` half of sec 6.1's redundancy.
  Before a Dynamic or Static run the Chrono backend finds the joint rows
  that depend on the rows before them -- the rank analysis `analyse()`
  already made, in OndselSolver's order (its categories, then model order),
  now shared as `rowInfo()` / `independentRows()` -- and takes them out:
  a mate's coordinate through `SetConstrainedCoords`, a neutral link's
  equation through `NeutralLink::dropRows`, a link with no row left removed
  whole (its reaction then reads zero). The motion is the same, and the
  solve is regular: before, the LU of the singular system left the
  multipliers at 1e19 and LUThenQR's QR took over.
  - The joints whose reactions are not unique are those whose rows were
    taken out and those the taken-out rows repeat (each one's combination
    of the independent rows). Their "Force" and "Torque" channels carry
    `Channel::unique` = false, `getSimulationResults()` gives it as
    `Unique`, and the panel's Results box lists them "(not unique)" with a
    tool tip saying why. The run warns: "the dynamic analysis takes out 5
    equations of redundant joints, which repeat what other joints hold;
    the reactions at ... are not unique".
  - A dependent row the start does not satisfy (1e-6 mm, or 1e-6 for an
    angle's row) is a conflict: the run is refused, naming the joints.
  - Not built: `RedundantJoints = Bushings` (the replacement of a
    redundant joint by a bushing), and so no study property yet: removal
    is what every run does.
  - Sec 10's test row said a door on two coaxial revolutes has "one
    revolute's 5 rows dependent". In OndselSolver's order the
    displacement rows come first, so it is 2 of the first hinge's (its
    tilts, which the two hinges' points fix between them) and 3 of the
    second's; both hinges are redundant, as `analyse()` has always said
    (`rankOnHandCountedModels`).
  - Tests: C++ `ChronoBackend.dynamicRunTakesOutRedundantRows` -- the
    door on two hinges swings as on one (1e-8 mm), the two reactions add
    up to the one's (1e-6) and are flagged; a puck on a plane and on a
    line through the plane's origin loses one of the line's two equations
    (the neutral link held partly) and slides on as started (1e-9 mm);
    two Fixed joints that disagree refuse the run. TestDynamics: the
    pendulum on a second coaxial hinge, the same. TestGuiCommands: the
    Results box's label and tool tip.
- Built: inverse dynamics in kinematic runs (sec 6.3's "Kinematic +
  reactions", on Chrono). `Simulation::Reactions` (the panel's
  "Reactions" check, shown for a Kinematic study) sets
  `SimulationSpec::reactions`; the study then takes the masses, gravity
  and its Elements as a Dynamic one does, and runs on a backend that runs
  dynamics, whichever the assembly solves with.
  - Chrono's kinematic run keeps its frames and hands them to
    `simulateDynamics(spec, sink, &frames)`, which builds the dynamic
    run's system (SI, centres of mass, force elements, the motions as
    Position motors, redundant rows taken out) and, instead of
    integrating, at each frame: the poses Chrono's assembly finds 1e-4 s
    either side with the motors at those times (four forward ones where a
    formula has no value before the frame), central differences of them
    for the velocities and accelerations, and the multipliers solving
    Cq' L = M a - f by least squares, scattered to the links -- so every
    reaction, motor effort, spring and bushing channel reads as in a
    dynamic run. The frames' poses are the kinematic run's, unchanged.
  - Chrono's own acceleration analysis (`DoAssembly` FULL) was no use: its
    velocity step holds the constraints' rates at the current time, so a
    driven joint's acceleration and the centripetal terms come out as
    nothing (the 1e-6 s step the dynamic start compensates is the same
    thing).
  - A motion that leaves a freedom undriven has no unique answer: the run
    warns ("need the motions to drive every freedom, and 1 are left
    free") and plays its frames without values. Limits are not stops
    here: a kinematic run's active bound carries no reaction yet.
  - Tests (TestDynamics): the pendulum driven by theta = 2 t^2, its hinge
    force m (a - g) and motor torque I alpha - m g d cos(theta) against
    the closed form (1e-6 of the weight's); a slider-crank (a closed loop,
    3 redundant rows) driven at 10 rad/s, its motor torque against a
    Dynamic run of the same Position motion -- within 5e-5 of the peak at
    every frame but the first after the start (3.7e-4, the dynamic run's
    start), tested at 1e-3; the undriven pendulum reporting nothing.
    TestGuiCommands: the check, shown for Kinematic and hidden for Dynamic,
    and gravity and the elements following it.
  - Found on the way: OndselSolver's kinematic run gave the start twice
    (22 frames for 21 times, Times [0, 0, 0.01, ...]), and the first was
    not the start: it is runKINEMATIC's INPUT output, the poses as handed in,
    so a run after a run began where the last one ended (a trace through
    the frames bent back to it). Fixed: `OndselSession::simulate` leaves it
    out; the gate's records keep it and its check drops it.
- Built: CSV export, and the same table to a spreadsheet (sec 8.4's "CSV
  or a Spreadsheet of any channels"). `CommandCreateSimulation.
  resultsTable(assembly)` makes the table of the last run's values: the
  time, then every channel's components (a vector's X, Y, Z) in the units
  the Results box shows, each column named "Revolute: Reaction force X (N)",
  "(not unique)" added to a redundant joint's; `exportResultsCsv(assembly,
  path)` writes it (UTF-8, numbers to 17 digits, so they read back
  exactly), `exportResultsSheet(assembly)` puts it in a new
  `Spreadsheet::Sheet` of the document, the names in row 1. The Results box
  has "Export CSV..." and "To Spreadsheet". A run that reported no values
  (a kinematic one without its reactions) has nothing to export.
  - All the channels, not one plot's (SOLIDWORKS exports per plot): a
    table a spreadsheet filters is the more useful default, and the
    plots are one channel each anyway.
  - Not built: the measures of sec 8.5 (a point's velocity, relative
    motion, energies) as columns -- they come with the measures.
  - Tests: TestDynamics, the pendulum's run to a CSV file read back exactly
    and to a sheet, and nothing from a kinematic run; TestGuiCommands, the
    To Spreadsheet button.
- Built: traces (sec 8.5's `TraceCurve`). `SimulationTrace.py`: a
  `Part::FeaturePython` with `TraceCurve` as its proxy, a child of the
  Simulation as its motions are (sec 5.2's table put it in the assembly;
  under its study it goes with it); `Part`, `Point` (in the part's
  coordinates) and `Relative` (empty: the assembly's coordinates; a part:
  the path as seen riding on it, in its coordinates, the feature placed
  where that part is -- a cam's profile from its follower). No
  `Simulation` link: the parent is the study. Its shape is a B-spline
  through the point at every frame of the last run of its study, periodic
  when the path ends where it began, consecutive repeats dropped; a vertex
  for a point that never moves.
  - What a simulation holds is never a component (`isStudyChild()`):
    an object in a plain group of an App::Part is in the Part's Group too,
    so a trace was one of the assembly's shapes: it was listed as a
    movable part (seen), and the collectors behind the mass properties,
    the DoF and the BOM take any shape there. `getAssemblyComponents()`,
    the Parts group's `isComponentObject()`, the BOM and
    `UtilsAssembly.getSubMovingParts` now leave a Simulation's children
    out.
  - The frames come from `AssemblyObject.getSimulationPlacements(obj)`
    (new: each frame's Placement of a component, as playback writes it,
    the document left alone) and `getSimulationResults()["Simulation"]`
    (new: the study the stored frames are of). A trace recomputed without
    a run of its own study -- a file just opened (results are not stored
    yet, sec 8.4), or another study run since -- keeps the curve it has.
  - The panel's "Trace" button, beside "v0", traces the selected vertex
    (or where a pick hit a part); a run remakes its study's traces
    (`updateTraces`). The motion list shows them; a double click on one
    does nothing.
  - Not built: Inventor's 2D/3D sketch output; the workbench tool of sec
    9's row (the panel's button stands for it).
  - Tests: TestDynamics -- not a movable part nor in the mass
    properties' components; the rod's tip over the swing through its every
    frame (1e-7 mm) and on the 100 mm circle about the hinge (0.01 mm
    between frames); riding on the rod itself, one point; another study's
    run leaving it as it was and its own run remaking it; a full driven
    turn closed and periodic, 2 pi 100 long to 1e-3 (the Ondsel input
    frame found through this one). TestGuiCommands: the vertex pick in the
    part's coordinates, and the button.
- Built: the stale flag (sec 8.4), for the run the assembly keeps -- the
  results are not stored in the file yet, so there is no model hash to
  keep with them; the model itself is kept instead. A run keeps the model
  and the spec it was run from. `AssemblyObject::isSimulationStale(sim)`
  builds sim's model again as a run would (`buildSimulation()`, split out
  of `generateSimulation()`; the session, the model, the spec and the
  parts the last solve left are put back, so the solve's status and a
  drag do not see it) and compares: `Solver::sameMechanism()`, the
  records with positions, directions and inertias within 1e-9 (relative)
  and the rest exactly, the bodies' placements left out -- where the
  parts stand is not the mechanism, and playback moves them -- and the
  spec field for field.
  - `Simulation::ResultsStale` (read-only, not saved) says it in the
    property editor; the assembly sets it after a recompute that followed
    a change in the document while it keeps frames (once per edit: the
    rebuild's own messages, a formula that no longer parses, come once).
    The study's tree icon carries the tree's recompute mark, and the open
    panel says "The results are of an earlier version of the assembly or
    of this study: generate them again." A run clears it, and a study
    whose frames another study's run replaced is not stale.
  - What makes a run stale is what its model holds: the joints and their
    frames, the parts' shapes through their inertias in a dynamic study
    (not in a kinematic one), the motions, the elements, gravity, and the
    study's own settings. Moving a part is not; nor is the start pose.
  - Found on the way: the mass properties after a played frame were of the
    parts where they had stood. They read the shapes through the
    assembly's shape cache (`Part::PropertyShapeCache`), which waits for
    the assembly itself to change, and playback (or a solve) writes the
    placements and purges the parts' touched state -- so a dynamic run
    started from a played frame had its inertias in the wrong place, the
    centre of mass 3.3 mm off at the third frame of the pendulum. Not even
    a recompute helped. `setNewPlacements()` now clears that cache when
    it moves anything.
  - Tests (TestDynamics): fresh after a run, after playback and after
    moving the rod; stale on the study's end time, a joint's frame and
    the rod's length, fresh again on undoing each, the solve's status
    kept through the check, fresh again on a run; a study whose frames are
    gone, not stale; a kinematic study not stale on a mass change. The
    mass properties at two played frames, where the rod is (1e-9 mm).
    TestGuiCommands: the panel's line and the tree icon, on and off.
- Built: the run on a worker thread, with cancel -- the last of D1's
  leftovers. `AssemblyObject::startSimulation(sim)` builds the model on
  the main thread, from the document, as `generateSimulation()` does, and
  runs it on a `std::thread` with the session it was loaded into: the
  assembly lets go of that session (its next solve builds another), so a
  solve, a drag or a recompute meanwhile touches nothing the run uses, and
  the run touches no document object (the formulas are the neutral
  evaluator's trees, the curves OCCT geometry read only).
  `simulationProgress()` gives how far it got (the time of its last frame,
  the frames made), `cancelSimulation()` has the sink refuse the next
  frame, which every backend takes as Cancelled, and `finishSimulation()`
  joins it and takes its frames as a run here does (-7 when cancelled,
  nothing kept). Python: `startSimulation`, `getSimulationProgress`,
  `cancelSimulation`, `finishSimulation`; `generateSimulation()` stays as
  it was, on the calling thread.
  - The GIL: the console calls a Python observer from the worker under the
    GIL, so whatever joins the worker on a thread that holds it --
    `finishSimulation()` called from Python, the assembly's destructor --
    lets go of it while it waits.
  - The panel's Generate starts the run there and polls it every 50 ms: a
    progress bar over the study's time and a Cancel, then the player as
    before; "The simulation was cancelled." after a cancel. Closing the
    panel cancels a run going.
  - Not cancellable midway: an OndselSolver run (a kinematic one), which
    makes all its frames inside `runKINEMATIC` before handing over the
    first; it stops only then.
  - Found on the way: a dynamic run whose joints tie nothing to the ground
    crashed FreeCAD (an integer division by zero in Eigen's SparseLU,
    `memInit` of a 0 x 0 matrix: Chrono's assembly of a system with no
    free variable). `LUThenQR` takes the empty system as solved.
  - Tests (TestDynamics): the pendulum on two hinges run there gives the
    frames a run here gives (1e-12), its redundancy warning reaching a
    Python observer from the worker, the assembly solving meanwhile; a
    run of 1000 s cancelled after three frames, done within 5 s and
    keeping nothing; a run here cancelling the one going; nothing to move,
    six frames. TestGuiCommands: Generate's progress row, the player after
    it, and Cancel on a long dynamic run.
- D1 is built.
- **D3 measured, 2026-10-29: skipped; D2 follows D1.** What running out of
  process would buy, against what is here now:
  - The runs' lengths (headless, wall clock of `generateSimulation()`,
    best of two; the tests' mechanisms and chains of 20 mm links hinged end
    to end, falling from horizontal):

    | run | frames | s |
    |---|---|---|
    | pendulum, dynamic, 1 s | 101 | 0.03 |
    | pendulum, dynamic, 10 s | 1001 | 0.21 |
    | pendulum, static, 1 s by 0.1 | 11 | 0.03 |
    | slider-crank, kinematic (OndselSolver), 10 s | 1001 | 1.9 |
    | slider-crank, kinematic with reactions (Chrono), 10 s | 1001 | 0.77 |
    | slider-crank, dynamic, 10 s | 1001 | 0.53 |
    | chain of 10 links, dynamic, 1 s | 101 | 0.33 |
    | chain of 30 links, dynamic, 1 s | 101 | 1.0 |
    | chain of 100 links, dynamic, 1 s | 101 | 26.7 |
    | chain of 30 links, kinematic (OndselSolver), 1 s | 101 | 0.75 |

    Seconds for the mechanisms a study is made of; the long one is a
    solver matter, not a process one: a quarter of the 100-link chain's
    linear solves go to `LUThenQR`'s QR because Eigen's SparseLU fails to
    factor them (a zero pivot) once the chain moves fast -- none in its
    first 0.3 s. Refining the LU solution does not help (it is not
    reached); a pivoting threshold or another ordering is the lead. Open.
  - Crashes and hangs: one crash in a run (the empty system, found and
    fixed above, `34ebcfed94`); OndselSolver's hang on a singular matrix
    and the solve/status recursion were fixed before (SI sec 6.19,
    `2253269d53`); its formula parser's crashes went with the neutral
    evaluator (SI sec 4.6). None open.
  - What the worker thread gives already: the GUI stays live through a
    run, with its progress and a Cancel that stops a Chrono run (dynamic,
    static, kinematic with reactions) at its next frame. Not covered: an
    OndselSolver kinematic run cannot be stopped midway (2 s for a
    thousand frames of the slider-crank), and a crash in a backend still
    takes the session down.
  - So D3 would buy crash isolation for a class with nothing open, and the
    stop of an OndselSolver run that is seconds long, for its 3-4 weeks.
    Not now. Taken up again if crashes in runs turn up, or when the
    browser tier wants a solver on the server (its playback of stored
    results needs none: the frames are data).
- Built, D2's first step: contact between primitives, convex hulls and
  closed meshes, in dynamic and static runs on the Chrono backend.
  - The document: `Assembly::ContactSet` in the Physics group (Python:
    `CommandCreateContact.newContactSet(assembly, groupA, groupB=())`),
    ticked in a study's Elements as any other element. GroupA's
    components touch GroupB's, or, GroupB empty, one another; nothing
    else touches (a set is the pair table's row). Its law is Adams'
    IMPACT, applied per patch (below): a stiffness and an exponent,
    `MaxDamping` (N s/mm,
    reached at `Penetration` through a STEP), and friction by speed:
    `StaticFriction` below `StictionVelocity`, `DynamicFriction` above
    `FrictionVelocity`, STEPs between (defaults 10, 0.01 mm, 0.3, 0.25,
    0.1 and 10.16 mm/s). The stiffness and the exponent are `Stiffness`
    (N/mm^e) and `Exponent` (1e5 and 1.5) where `StiffnessFrom` is Value,
    or, Materials (a new set's), Hertz's from the components' materials:
    4/3 E* R^0.5 and 1.5, 1/E* = (1 - nuA^2)/EA + (1 - nuB^2)/EB, each
    component's Young's modulus and Poisson's ratio from its shapes'
    material cards (the softest where they differ; the default material's,
    Aluminum Generic, 70 GPa and 0.35, where a card gives none). By
    `RadiusFrom` Part, a ball's: R, the contact's radius, by the parts'
    sizes, 1/R = 1/RA + 1/RB, each the radius of the ball of the solid's
    volume. By Patch, by the patch's relative curvature
    (`patchStiffness()`): at each point the two sides' curvature tensors
    summed in the contact plane, and its principal curvatures k1 >= k2, the
    patch's their means. Two flat faces (k1 none): a ball's, R the patch's
    extent, the farthest of its points from their centre, and a single
    point on two flat faces the parts' sizes. Round or oval (k2 at least a
    hundredth of k1): a ball's, R 1/sqrt(k1 k2) -- Hertz's ellipse within a
    few percent below an aspect of ten. A line (a cylinder on its side, a
    torus lying flat on its ring): Johnson's line contact
    (`johnsonLine()`), the axis of a cylinder of R 1/k1 sinking
    d = P / (pi E*) (2 ln(4R/a) - 1) under P a unit length, a =
    sqrt(4 P R / (pi E*)) the contact's half width, L the line's length;
    as the power law tangent to it at the patch's depth now (or, before it
    presses, the depth an impact at its speed reaches), refitted at each
    collision detection: k d^e through Johnson's force there, e its slope
    (2 ln - 1) / (2 ln - 2), 1.05 to 1.1 (Pereira et al.'s explicit fit to
    Johnson takes 1.094). Pereira et al. and the review of Skrinjar et al.
    (2018) put Johnson's first among the line laws; without its logarithm,
    pi/4 E* L d, it is five times as stiff at a metal's sinks of 1e-7 mm.
    Each patch has its own exponent. Aluminium on aluminium comes to about
    1e5 N/mm^1.5 for a radius of 5 mm, the Adams default. Where
    `StiffnessFrom` is Penetration, Drake's penetration allowance: at
    `Exponent`, k = m g / d^e, m the lighter moving body of each pair and d
    `AllowedPenetration` (0.01 mm), so that it sinks d under its weight
    whatever its mass or material (standard gravity's where the run has
    none) -- the largest penetration acceptable runs the fastest. The
    damping is
    `MaxDamping` reached at `Penetration` where `DampingFrom` is Value, or,
    Restitution (a new set's), by the coefficient of restitution
    `Restitution` (0.6): Hunt and Crossley's,
    Fn = k d^e (1 + a / v0 dd/dt), v0 the speed the contact came in at (the
    fastest its points approach while it does not press, kept from then on;
    at least the speed of an impact that goes as deep as the patch presses
    now, sqrt(2 k d^(e+1) / ((e+1) m)), which an impact never passes and a
    contact pressed deeper by its load does -- a follower that started on
    its cam came in at none, and at the 1 mm/s that left, its damping ratio
    in the hundreds, it took substeps of 0.2 us; at rest under its weight it
    is about a / 2), a exactly r's (`huntCrossley()`, Carvalho and Martins
    2019): with u = a / v0 dd/dt the impact integrates in closed form,
    m v0^2 / (a^2 k) (u - ln(1 + u)) = -d^(e+1) / (e+1), so u - ln(1 + u) is
    the same coming in at u = a and going out at -a r -- for any exponent,
    a line contact's as a ball's, any stiffness and mass. Flores et al.'s
    approximation 8 (1 - r) / (5 r), the first try, rebounded short (0.26
    for 0.3, 0.79 for 0.8), Gonthier et al.'s (1 - r^2) / r is close below
    0.5 and not above, Lankarani and Nikravesh's 3 (1 - r^2) / 4 only near
    1 (the review of Skrinjar et al., 2018). It scales with the stiffness,
    and is none at no depth, with no STEP. A set saved before
    `StiffnessFrom` and `DampingFrom` restores as Value for both
    (`ContactSet::Restore()`).
  - The curvature (`vertexCurvatures()`): a sphere's all round, a
    cylinder's side round its axis, a cylinder's cap and a box's faces
    none; a mesh's and a hull's at each vertex of their contact mesh, the
    curvature tensor of a height quadric, A x^2/2 + B x y + C y^2/2 + D x +
    E y, fitted to the vertex's neighbours in the tangent plane of its
    normal (the faces' within 40 deg; a turn of 40 deg or more is the
    solid's edge, and a flat face up to it stays flat). A point takes the
    mesh triangle's corner nearest it, or the hull's mesh point farthest
    out along the normal, where a convex shape touches. A torus's tube
    (2 mm) reads 1.997, a hemisphere (5 mm) 4.99, an ellipsoid's pole
    (12 mm) 11.99.
  - The line's length (`lineLength()`): the deepest point of each
    cross-section, across the line the curved way, chained by their
    minimum spanning tree, a path whose ends are close closed into a ring.
    A torus lying flat reads 62.38 mm of its ring's 62.83 (the tessellated
    polygon's), where the patch's extent is 10.7.
  - The command, Assembly_CreateContact (toolbar, after the bushing):
    the components selected go in group A, and its panel (a double click
    opens it too) picks either group in the 3D view -- a pick adds a
    component, taking it out of the other group, and a pick of one
    already in the group takes it out -- and takes the law: where the
    stiffness comes from, the contact radius, the allowed penetration, and
    the stiffness itself on a
    spin box and a slider of powers of ten, 1 to 1e7. Under them, how the
    lightest component of group A rests on the law: how far it sinks and
    the run's steps through an impact there, (mg/k)^(1/e) and a fiftieth
    of the period (from the materials, by the parts' sizes; the run takes R
    where they touch). OK waits for
    group A; Cancel undoes the edit, or deletes the set the command made.
    A new study ticks it with the other elements, and its ContactForce
    channel is "Contact force" in N in the results. The study command no
    longer waits for a joint a motion can drive when the assembly has a
    physics element: a drop test has none.
  - The geometry, sec 7.1's first rungs (`App/ContactShapes.cpp`): each
    solid of a component is the primitive it is -- a sphere, a box or a
    cylinder, recognised by its faces and checked by its volume -- or its
    convex hull, from the contact geometry cache's triangles, or else
    those triangles themselves (rung 3a), wound to agree across their
    edges and outward (`orientOutward()`). A solid none of these -- its
    mesh not closed, not manifold, or in pieces, as a cube with a hollow
    in it is -- is refused by name and the run fails. Shapes ride on
    frames, so the rigid groups carry them as they carry everything else.
  - The force (`Solver/ChronoBackend.cpp`, `ContactLoad`): a load per pair
    of bodies a set lets touch, given at each step's start the points
    collision detection found, and evaluating the law at whatever state
    the integrator asks for. Its numerical Jacobian makes the contact
    implicit, by differences of a hundredth of the shallowest point's depth
    (at most Chrono's 1e-8 m: a metal's line sinks 3e-10 m); Chrono makes
    no contact of its own (Bullet's
    narrowphase callback refuses them all: its SMC contact is explicit,
    fixed over a step). HHT with alpha -1/3 and no step control when there
    is contact. A step is cut into substeps: a fiftieth of the stiffest
    point's impact period (the body's mass shared among its patches
    pressing), the damping's limit 2m/c, and half the time to a contact
    still coming
    (its speed and gravity) -- for the impacts: a load at rest keeps none of
    them. It rests once its deepest point's depth has stayed within 2% of
    itself, or within the run's tolerance (1e-6 mm), for five of its
    periods (an impact's sink and rebound move it all the way); HHT, given
    its Jacobian, damps the period it steps over, as RecurDyn and Adams,
    stepping by their error, cut the step only about an impact. A substep
    longer than the strict rule's that moves a load at rest out of its
    range is taken again by the rule. A substep that finds a new contact deeper
    than a tenth of what its impact will reach is taken again an eighth as
    long; one whose Newton iterations ran off to a state not finite, too,
    and below 1e-9 s the run fails ("the contact's integration diverged")
    instead of handing Bullet a NaN; and one whose iterations stopped
    unconverged (HHT's terminated solves, whose state it takes as it is),
    taken as it is only below 1e-9 s.
  - The law is a patch's, not a point's (`patchesOf()`, `ContactLoad`):
    the points of one stretch of surface touching another push as one
    point would at the patch's depth, so a face resting flat pushes the
    same however many points its clipping or its mesh gave it. Two points
    are in one patch where both bodies' shapes under them meet -- the same
    shape, or mesh triangles with a corner in common -- and their normals
    are within 45 deg: a ball in a V-groove or a box in a corner touches in
    two, a table's legs on the floor in one each. Each point pushes by its
    own law's force times a share, sum(s^2) / sum(s)^2 over the patch's
    springs s = k d^e: equal depths share equally, so the patch pushes
    k d^e, and a deeper point carries more, so a tilt is pushed back.
    Per point, a box's four corners were four times as stiff as a sphere's
    one point, a hexagon's six six times, and a mesh as stiff as it was
    fine.
  - The points: Bullet's GJK gives a convex pair one point a query, and a
    face landing on a face then rocks on whichever corner it found. A
    pair of a hull and a hull or a box has Bullet's polyhedral features,
    and is clipped face against face along the axis separating them most,
    every corner at once, four to a manifold and as many manifolds as it
    takes (`PolyhedralAlgorithm`, registered for those
    pairs in place of Bullet's convex pair; two boxes keep Bullet's
    box-box). The axis is the backend's own separating axis search: every
    face of each, and the edge pairs that make faces of the Minkowski
    difference (Gregorius, GDC 2013), most of them passed over in two dot
    products. It is kept for the pair's next query and measured again
    there, by the shapes' extents along it: still separating them, it
    proves them apart however they moved, and otherwise it is taken while
    they have moved less than 10 um and 1e-3 rad since its search. The
    clipping finds both faces from the support vertices' faces, not among
    all of them. A
    cylinder's pair with a box or a hull is clipped the same way, its cap
    as a 16-gon of its rim starting at the rim's lowest point, or its side
    as the segment between its caps' lowest points, against the face
    turned most toward it (`cylinderPoints()`). Other pairs (a sphere, a
    cylinder against a cylinder) keep GJK's points.
  - Meshes (rung 3a): a `ChTriangleMeshConnected` of the solid's
    triangles, its vertices in the body's frame (the connected path's
    triangles ignore a shape's frame), each triangle a Bullet shape of its
    own knowing its neighbours. A triangle against a box, a hull or another
    mesh's triangle goes to `PolyhedralAlgorithm` too, its features
    `satOfTriangle()`'s: its face, grown by the envelope, and its edges'
    arcs to the neighbours' faces -- convex edges only, so the flat edges
    inside a face make no axis and a part sliding over them does not bump
    into them. A triangle stands for the solid behind it: a contact normal
    out of either triangle's back is not taken, a neighbour's face that
    proves the best normal leaves the points to the neighbour's own pair,
    and an edge pair touches at the edges' closest points -- inside both
    edges, or not at all. A pair touching least out of a triangle's back
    gives no points unless a valid axis touches as little: that touch is
    the neighbours'. A triangle's points are grown toward the other
    shape, as a ball grows it, not along its own normal. Those axes cannot
    prove a pair apart, so a pair they find touching is tested again as
    two plain convex shapes, flat edges and all (`satApart()`), the
    separating axis kept as any other. The triangles share their corners,
    and a corner's points come once per load.
  - Found on the way, each measured before it was fixed:
    - Chrono builds Bullet in float unless `CH_USE_BULLET_DOUBLE` is on,
      and Bullet's GJK tolerances are absolute: 1e-6 on squared lengths,
      a square millimetre in the backend's metres. A cylinder lying on the
      plate was not found until it was 6 mm into it (GJK's distance 6.5 mm
      for a gap of 0.27). `src/3rdParty/ChronoCore.cmake` turns it on, and
      passes `BT_USE_DOUBLE_PRECISION` to the sources that include
      Bullet's headers so they see its types as the library has them.
    - Chrono's hull of a hull's points (Bullet's old HullLibrary) moves
      them by about half a micrometre and merges any within a millimetre
      of each other: a prism stood 0.5 um low. The features are the exact
      hull of the points (`cbtConvexHullComputer`), each face moved out by
      the envelope and each vertex onto its faces' planes, so they are the
      shape Chrono reports contacts on.
    - Bullet's multipoint perturbation (more points per query by turning
      the pair a little) put its points up to 20 um off on a flat face,
      and lost a bouncing ball half a percent of its height: not used.
    - The narrowphase's normal can be NaN (EPA failing to a zero normal):
      such a point is dropped.
    - Bullet's SAT (its `m_enableSatConvex`) projects both shapes on every
      pair of edge directions. Two 256-sided prisms, one dropped across
      the other, 0.1 s: 55 s, against 1.7 s on GJK's axis -- which put
      them a millimetre wrong. The backend's search does that run in
      1.4 s; a full search of theirs is 1.5 ms (768 edges each, 33000
      edge pairs real Minkowski faces, cap rim against cap rim), and
      searching on all of the run's 108000 queries took 180 s, so it is
      the kept axis that pays. A hexagonal prism lying across a larger one
      now rests on the ridge at 14.33 mm, as the geometry says (10 + 5
      sin 60); Bullet's clipping had it at 15.45.
    - The convexity test held every point of the mesh against every
      triangle's plane, and a curved surface's long thin triangles -- an
      ellipsoid's at its poles, a cone's -- have points beyond their
      neighbours in front of the plane by more than the deflection: both
      refused as not convex. It is local now (`convexMesh()`): the
      triangles' windings made to agree across their shared edges and
      turned outward by the signed volume, no neighbour across an edge in
      front of a triangle's plane by more than twice the deflection. A
      closed, connected surface convex at every edge is convex (Tietze).
      A mesh it cannot read (not closed, not manifold) falls back to the
      old test.
    - An ellipsoid's hull, 5795 vertices and 11558 faces, then had no
      features: a vertex on a curved surface has its faces' normals all
      but parallel, and growing it by the envelope solved a 3 x 3 system
      that is all but singular. It went through the plate. The growth
      is an SVD now, the directions the normals do not span left out.
      Its run then took 72 s for 0.3 s: re-placing all of the shape for
      the kept axis, Bullet's clipping searching all 11558 faces, each
      substep's collision detection twice (Chrono runs it as a step
      starts, at the state the substep before ended on; the system now
      skips that one). 8.7 s now.
    - Meshes, rung 3a's tests:
      - `PolyhedralAlgorithm` came from the dispatcher's pool, whose
        elements are sized for Bullet's own algorithms (184 bytes; it is
        240), the size checked by an assert only: a mesh's pairs overran
        them and the run died in Bullet's compound algorithm. It is
        allocated on its own now.
      - Two triangles facing the same way side by side -- a part's side
        flush with the wall of the slot it rests beside -- were taken to
        overlap by the part's width along their plane, and the part flew
        off at 1e14 rad/s. Hence the axes out of a triangle's back.
      - Then the convex edges alone could not tell a channel's wall from a
        part's side 7 mm away (the one axis between them the cross product
        of two diagonals): hence `satApart()`.
      - An edge pair took the triangle's face as its reference, a right
        angle off the axis, and pushed the part sideways as deep as the
        face was wide; then a kept axis put a corner on an edge 20 mm off.
        Hence the edges' closest points, inside both.
      - A wall's top edge grown along the wall's normal stood an envelope
        below the top beside it, and a part overhanging the slot by 0.04 mm
        rested 0.02 mm low. Hence the growth toward the other shape.
      - A torus took 76 min for 0.3 s: its 267 points within the envelope
        each took a share of its mass for the substep, and only the ring it
        rests on presses. The pressing points share it now, when any do.
        Under the default law that ring still rested in substeps of 1.4 us
        -- a fiftieth of the law's period on each point's share of the
        mass -- and 0.3 s took 10 minutes. The law per patch was the fix.
    - The law per patch:
      - The patch's spring first was its deepest point's, max(s): a kink
        where the deepest changes, and the Newton iterations of a cube
        landing flat on its mesh ran off on it, the cube thrown at 3 km/s.
        sum(s^2) / sum(s) is smooth, equal to s when the points are.
      - A hexagonal prism resting under a soft law stood 0.5 um high and
        2e-3 rad tilted: Bullet's manifold keeps four points, a fifth
        replacing one, and a new point near one, within the breaking
        threshold, replacing that. It kept four corners, off its centre. Hence
        the manifolds four to one, each emptied first.
      - The cube with a corner cut out, landing under the soft law, fell
        through the plate: tilted past the slack, its side's triangles that
        meet the base at a corner only touched the plate least along its
        top face, out of their backs, and the valid axis left was its
        bottom face, 20 mm deep. Hence no points from a pair touching
        least out of a triangle's back.
      - Grouping by every pair of points, a torus's 1560 points a query
        (260 once the copies went) took 18 ms, and it ran twice a substep:
        once after it, and again at the next one's start on the same
        results. Only points sharing a key on both bodies are compared now,
        and a step's start on kept results keeps the points: 0.05 s of the
        torus 129 s -> 14 s.
      - The torus under the default law: one patch, substeps of 6.7 us
        (1.4 per point), at rest by 0.06 s; its test's 0.1 s takes about
        a minute. The cube with the corner cut out rests tilted 1.6e-3 rad
        under the soft law, as an off-centre load on an elastic support
        does: its centre of mass is off its base's.
    - The stiffness from the materials, the curvature's estimates:
      - The angle between faces over their centres' spacing read a torus's
        tube as tighter by half again: its triangles are half quads, a
        triangle's normal the quad's and its centre a third of the way
        across. The circle through an edge's ends with the vertex's normal
        is exact on a polygon of a circle; the most of those at a vertex
        read the tube a fifth tighter, a slightly wrong normal over a short
        edge, and a tensor fitted to them as much.
      - About an ellipsoid's pole the faces' mean normal was 0.004 rad off,
        and over a tenth of a millimetre's edge the circles read 1/12 mm as
        anything from 1/30 to 1/6. The height quadric's linear terms take
        up the normal's error.
      - A hull's polyhedral features are no mesh to read: Bullet merges
        faces all but in one plane, and the vertices it kept about the
        pole curved it as 1/8 mm. The hull keeps its contact mesh's
        triangles for it.
      - The patch's extent first capped every radius: its points are all
        those within the contact's margin, and the ellipsoid's, on the
        triangle at its pole, were half a millimetre across. Only two flat
        faces take it now.
      - The extended law: the curvature tensors averaged over a torus's
        ring, curving across the tube at each point a different way round,
        read it as a round contact of 4 mm -- hence the means of each
        point's principal curvatures; its ring's spanning tree was the
        ring less a segment, 8% short with 13 points; and points at one
        depth knocked one another out of the line on a rounding's 1e-16,
        40 of the ring's 89 kept, until a point lost only to one deeper by
        10 nm.
      - Under aluminium the line law made a cylinder on its side and a
        torus lying flat 6e5 and 2e6 N/mm stiff, sunk 1e-7 mm, and the
        damping given by value, whole at 0.01 mm, all but none there: they
        bounced on, 0.93 mm up at 0.1 s. Hence the damping by restitution,
        scaling with the stiffness.
      - Johnson's law and the rule for loads at rest took the cylinder of
        aluminium on its side from 68 s to 15 s for 0.3 s: its Newton
        iterations stopped unconverged on 26858 of its substeps, every one
        past a third of its period, until the Jacobian's differences were
        the contact's own size, and then 0.2 s. The torus of aluminium on
        its ring rocks on its facets by 5e-8 mm, sunk 6e-8: within 2% it
        never rested, hence the tolerance.
      - The notched cube landing flat from the materials (k 1.4e5) was
        thrown half a kilometre, and so it was at a value of 1.41e5 (1e5,
        1.2e5 and 2e5 landed): a landing substep's Newton iterations
        stopped unconverged, and HHT took the state, finite. Hence the
        retake of a terminated solve.
  - Envelope 2e-5 m, margin 1e-4 m: Chrono's defaults (3 cm, 1 cm) are a
    part's size here.
  - Results: a ContactForce channel per set, the force on group A's
    bodies.
  - Tests (TestContact): a ball resting sunk by (mg/k)^(1/e) (1e-5 mm),
    its contact force the weight (1e-5), the same rest from a Static
    study; an undamped bounce back to its height (5e-4 mm); a block
    sliding at mu g; a prism resting on its hull under a soft law, sunk
    by (mg/k)^(1/e) as the ball (1e-5 mm), and landing level under the
    default one (1e-4 mm, 1e-4 rad), and on a larger prism's top face; an
    ellipsoid resting on its pole (0.01 mm), a torus on its tube (0.01 mm,
    of the soft material);
    the law per patch: a cube with a hole through it sunk by the ball's
    rule, and the channel upside down on its two legs by half its weight's
    (1e-5 mm); Hertz's law from soft materials (10 MPa): a ball sunk by its
    closed form, R its own or by the parts' sizes (1e-6 mm), the prism's
    base by its extent (2e-5 mm: its corners grown by the envelope) or the
    sizes (1e-6), a cylinder on its side by Johnson's (1e-6), and of
    aluminium (1e-8 mm), the torus lying flat as on Johnson's power law at
    its sink for its ring and a
    hemisphere on its dome as on Hertz's for R 5 (5e-5 mm); damped by
    restitution, a ball dropped a millimetre rebounding r^2 of it (2% at
    r 0.8 and 0.3); a set saved before StiffnessFrom and DampingFrom restored as
    Value for both; by an allowed penetration, a ball of 1.4 g and a box of
    10.8 g each sunk it (1e-6 mm); a cube with a corner
    cut out landing level on its base (1e-4 mm, 1e-4 rad), a box resting
    on the floor of a slot in a block and not on the block's hull, and the
    notched cube on the block beside the slot, mesh on mesh, its sides
    flush with the slot's wall; a cylinder
    landing on its cap, a tall one landing tilted and tipping onto its
    cap, one landing on its side and staying there; a cube with a hollow
    in it refused; a set's pairs only.
  - Tests (TestGuiCommands): the panel's picks, law, OK and Cancel, and
    a new study ticking the set; the stiffness from the materials, a
    value or an allowed penetration, and the damping by restitution or a value, each idling the
    other's rows, the slider's powers of ten and the rest it shows; OK
    refused with group A empty.
  - Not yet: 2D path contact (sec 7.4), stops, sensors, contact plots; the decomposition (rung 3b) and a ground mesh's BVH (rung 4).
    A sphere or a cylinder against a mesh keeps GJK's point per triangle.
    The rebound by restitution is r^2's of aluminium -- a ball dropped a
    millimetre reaches 0.0894 for r^2 0.09 at r 0.3, 0.364 for 0.36, 0.645
    for 0.64, 0.907 for 0.9025 -- and short of it where r is small and the
    contact long, of a soft material (10 MPa): 0.055, 0.316, 0.609 and
    0.891, gravity working through the 2 ms of contact, which the closed
    form has none of. A torus of aluminium dropped a millimetre bounces some
    twenty-five times at r 0.6 before it rests, each impact in substeps of
    0.3 us: 27 s for 0.1 s (its test is of the soft material: 8 s).
    A tumble is slow too: the tall cylinder's 0.3 s takes a few seconds, in
    substeps of microseconds through each impact.

- Built: cams that lift off (sec 7.4's `CurveContact`, as a contact set).
  - The document: `Assembly::CamContact`, a `ContactSet` (Physics; Python
    `CommandCreateContact.newCamContact(assembly, joint)`, the command
    `Assembly_CreateCamContact` on a selected Cam joint, ticked in the
    Dynamic and Static studies) whose `Joint` is a Cam joint; GroupA,
    GroupB and RadiusFrom hidden, the law the set's, by default an allowed
    penetration (0.01 mm) with no friction. `Reverse` puts the follower on
    the curve's other side. In a Dynamic run the joint is not made
    (`ContactPair::cam`, `liftCams` in the session's load); a ContactLoad
    between the cam's body and the follower's pushes instead, its one point
    found at each collision detection from the exact curve
    (`ContactHandover::CamFollow`): the curve's closest point to frame J's
    origin in frame I's coordinates (`projectOnCurve`, warm-started from the
    last; `projectOnCurveGlobally` the first time), the curve's normal in
    its plane N2 = T x N1 (N1 the reference square to the tangent, the
    joint's), the follower's point a roller's radius back along it, the
    normal N2 to the follower's side. It is one patch, and the whole law
    applies: the stiffness from an allowed penetration, a value or the
    materials (as the allowance there: a curve has no surface to read),
    damping by restitution, friction along the tangent, the step rules.
    Kinematic runs keep the joint.
  - The side (`camFollowerSide()`, CamJoint.cpp): a roller's, its offset's
    sign; on a closed planar curve, outside -- the sign of the curve's
    sampled area against its reference axis; otherwise the side of the
    follower's centre of mass from the curve's normal, at the start.
  - The start: a follower within a micrometre of its cam starts as the
    joint would have it -- the joint's NeutralLink held over the start's
    assembly, as an initial velocity's motor is, and taken off. From rest,
    the cam at 10 rad/s came at it as an impact at 60 mm/s, the follower
    sunk 0.03 mm, three times its allowance, before it rode.
  - Newton's test: HHT's is on its iterations' increments, in
    accelerations and multipliers, 1e-10 absolute by default, and a
    constraint's round-off of 1e-15 m is eps / h^2 there. Below a
    microsecond no step passed: each of the 64 smallest starting a run ran
    its 20 iterations, |R| at 1e-17 and two iterates apart, and a contact's
    substep taken again for that, an eighth as long, failed the more -- the
    follower stuck at a nanosecond, its test running three hours. The
    increments are now a thousandth of the run's tolerance in position, /
    h^2, the multipliers' as they would move the lightest body
    (`tolerate()`, each substep of a run with contact, which takes a
    substep again on it; a run without keeps Chrono's tolerances, its 20
    iterations getting the start's reactions closer -- stopped at these, a
    hinge's was 0.3% off); the case's first 1.9 ms took 75 s, then
    0.03 s.
  - Tests (TestCamJoint, `test_cam_lifts_off`): the disk cam (e 6 mm)
    turning at 10 and 80 rad/s, the follower on its slider, flat and a
    roller of 3 mm, under gravity down the slider -- on the cam while the
    cam falls away slower than g, e w^2 < g, below 40.4 rad/s. At 10 it
    rides the lift sunk 0.0096 to 0.0136 mm (within 0.02, never above it),
    at 80 it is thrown 20 mm up and lands on the cam's crest a turn later;
    frames of a 400th of a turn, two turns, 5 s for the four.
    TestGuiCommands (`test_cam_contact`): the command idle until a Cam
    joint is selected, then the set on it, selected, by an allowed
    penetration with no friction, ticked in the Dynamic study and not the
    Kinematic one, and one undo taking it out.
  - Not yet: a follower that is itself a curve (Adams CVCV), 2D path
    contact, a cam's pressure angle and slip in the results.

It plans the phase that `docs/AssemblyAudit.md` sec 8.2 phase 4 calls
"dynamics as `docs/Simulator.md` plans". That phase runs on the Chrono
backend that `docs/AssemblySolverInterface.md` sec 6.15 built for
kinematics. Read those two documents first.

Conventions:
- "SI sec N" means a section of the solver interface document, "audit sec N"
  one of the audit, and "Simulator sec N" one of `docs/Simulator.md`.
- Paths are relative to `src/Mod/Assembly/` unless they start at the repo
  root.
- `CH/` abbreviates `src/3rdParty/chrono/src/chrono/`, and `OS/` abbreviates
  `src/3rdParty/OndselSolver/OndselSolver/`.

Two pieces of work are queued ahead of this phase, and the design builds on
both:

- **(a) A custom Chrono link that evaluates the neutral layer's own
  equations.** These are the Reference backend's residuals and Jacobians,
  SI sec 6.13. The link lets Chrono hold the kinds it refuses today:
  AngleZ, the two distances, Belt, CoaxialGear, RackPinion and Screw, which
  account for 31 of the 72 gate records (SI sec 6.15).
- **(b) A "point on curve" cam joint kind in the neutral layer, for all
  three backends.** This is a kinematic cam: the follower point is held on
  a curve.

Web sources were read on 2026-10-23, and each claim about another product
carries its URL. Some vendor help is behind a login: Siemens is, and
Dassault's help.3ds.com could not be reached. Where a claim comes only from
a fact sheet, a community wiki, a tutorial or a forum, the text says so.


## 0. Conclusions

1. **The others agree on the shape. We copy that shape, not one product.**
   - **Study tiers.** Every commercial system separates a kinematic tier
     from a dynamic tier:
     - SOLIDWORKS: Animation / Basic Motion / Motion Analysis.
     - Creo: Position and Kinematic without the MDO licence; Dynamic,
       Static and Force Balance with it.
     - CATIA: V5 KIN against 3DEXPERIENCE dynamics.
   - **Bodies and joints come from the assembly.** Each turns mates into
     joints and merges rigid groups into one body.
   - **Contact is opt-in and per pair or group.** "Components pass through
     each other" unless contact is defined (SOLIDWORKS).
   - **Contact parameters are an Adams-style penalty law, with a
     restitution alternative.**

   The workbench already has the kinematic tier and the trajectory
   playback. This phase adds a **Dynamic** and a **Static** analysis type
   to the existing Simulation object. Nothing about it is a new workbench.

2. **Chrono 10's core, as we already build it, covers phases D1 and D2
   without another Chrono module.** It provides:
   - gravity;
   - real mass and a centre of mass off the body frame (`ChBodyAuxRef`);
   - springs and dampers (`ChLinkTSDA`, `ChLinkRSDA`) and bushings
     (`ChLinkBushing`);
   - motors with position, speed and torque control;
   - joint limits as unilateral constraints;
   - Bullet-based collision with primitives, convex hulls, sphere-swept
     triangle meshes and 2D arc/segment paths;
   - penalty (SMC) and complementarity (NSC) contact;
   - HHT and implicit Euler timesteppers;
   - static equilibrium;
   - contact reporting callbacks.

   `chrono_cascade` is not needed. It pins OCCT 7.9.2/7.9.3, and we
   already have the shapes, `BRepGProp` and the tessellation.

3. **The default contact method is penalty (Chrono SMC) with a direct
   sparse solver. That is accuracy first, as Simulator sec 4 ruled.**
   - SMC contact forces are explicit forces, so joints stay exact.
     Reactions are those of a direct KKT solve, and HHT can be used.
   - NSC needs an iterative VI solver, and Chrono states that HHT "can't be
     used for systems that handle contacts in a DVI approach"
     (https://api.projectchrono.org/simulation_system.html).
   - NSC remains as a "Restitution" option for piles and drop tests. It
     maps to Adams POISSON, as SMC maps to Adams IMPACT.
   - The impact law is the Adams/SOLIDWORKS one: k, e, cmax, dmax, with a
     STEP-ramped damping. It is installed through
     `ChSystemSMC::SetContactForceTorqueAlgorithm`, so parameters taken from
     the literature and other tools carry over unchanged.

4. **Mass for dynamics is a separate record from the kinematic body
   mass.**
   - Static solves keep mass 1 on purpose (audit sec 3 defect 2), and the
     Ondsel adapter writes `Body::mass` into ASMT, so the gate depends on
     it.
   - Dynamics therefore adds `Model::dynamics` with an `Inertia` per body.
     A kinematic model leaves it empty, so the identity gate and the
     `TopologyKey` do not move.
   - Inertia comes from `BRepGProp` over each part's shape and the
     `ShapeMaterial` density. Overrides are held by assembly-level
     objects, never written into linked part files.

5. **Units: the neutral model carries FreeCAD's internal consistent set.**
   - That set is mm, kg, s, with mN for force (`Base::Unit::Force` is
     (1, 1, -2)). F = m a holds in it, and property values need no
     conversion.
   - The Chrono adapter converts to SI at load and back on output.
   - The unit contract is one table (sec 8.1), with a free-fall test that
     fails by a factor of 1000 if anyone gets it wrong.

6. **Redundant constraints are removed before a dynamic run, and the
   affected reactions are labelled "not unique".**
   - This is what SOLIDWORKS does
     (https://help.solidworks.com/2025/English/SolidWorks/motionstudies/c_Redundant_Mates_SWMotion.htm)
     and what the Inventor FAQ warns about.
   - The neutral rank analysis already finds the dependent rows in
     OndselSolver's order (SI sec 6.13, 6.15).
   - Replacing a redundant joint with a bushing is offered as an option,
     as SOLIDWORKS offers it.

7. **Motors get a control mode on the existing Motion object.** The modes
   are Position (today's meaning, the default), Speed and Effort
   (torque/force).
   - Simscape's mode switch shows the pattern: prescribe motion and the
     effort is computed, or prescribe the effort and the motion results
     (https://www.mathworks.com/help/sm/ref/revolutejoint.html).
   - A Position motor in a Dynamic run reports the torque it takes. A
     Position motor in a Kinematic run on a backend that supports inverse
     dynamics does the same.

8. **Results are raw state, and measures are derived after the run.**
   - The stored state is per-body pose, velocity and acceleration, joint
     and motor reactions, element forces, and contact events.
   - Plots, traces and sensors are defined after the run without
     re-running, as SOLIDWORKS Results and Plots and Creo measures work.
   - Results persist as a versioned file blob beside the trajectory, and
     are marked stale by a model hash, as Inventor's red bolt and Creo's
     playback files do.

9. **Contact geometry is a per-body ladder, cached by TShape.** In order:
   1. analytic primitive when the solid is one;
   2. convex hull when the solid is convex;
   3. sphere-swept connected mesh ("precise"), or convex decomposition
      ("fast"), for the rest;
   4. static BVH mesh for grounded bodies;
   5. 2D arc/segment paths for planar cams.

   It is computed in App, keyed like `docs/TShapeRenderCache.md`'s instance
   table, at a contact deflection of its own. The same cache feeds
   "interference during motion" and audit phase 5's interference check.

10. **Cams: a kinematic cam is the queued PointOnCurve joint, and a cam
    that can lift off is a curve-contact force on the exact curve.** The
    force is evaluated by the neutral layer's projector against the B-rep
    curve, not against a tessellation.
    - Both Adams and SOLIDWORKS make this split. In Adams, PTCV/CVCV are
      bilateral and CONTACT is intermittent. SOLIDWORKS distinguishes
      "Curves always touch" from intermittent contact
      (https://help.solidworks.com/2025/English/SolidWorks/motionstudies/c_curve_curve_contact.htm).
    - Creo has "Enable liftoff" on the cam-follower connection
      (https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/simulate/mech_des/cams_liftoff.html).
    - Faceted cam contact gives noisy accelerations. That noise is the
      reason the others offer curve contact at all.

11. **Backends declare what they can run.**
    - Chrono runs everything.
    - OndselSolver can offer inverse dynamics (reactions in a kinematic
      run). Its acceleration solve already carries the mass matrix and
      `ConstantGravity`, and its ASMT items hold `FXonI`...`TZonI`. It has
      no forward integrator.
    - The Reference backend runs none of it, unless the user wants a small
      forward-dynamics oracle (open question 7).
    - A backend that cannot run an element that changes the physics
      **refuses** the run, as SI sec 6.15 refuses joints. An output it
      cannot compute is only a warning.

12. **Phasing**, each phase useful on its own:
    - **D0:** mass properties and the unit contract.
    - **D1:** forward dynamics without contact, with inverse dynamics and
      plots.
    - **D2:** contact, cams with lift-off, stops, drop tests, and
      interference during motion.
    - **D3:** the out-of-process session.
    - **D4:** loads to FEM, then flexible bodies.

    Effort and risk are in sec 4.


## 1. Where we start

| what | state | where |
|---|---|---|
| neutral model | bodies, frames, 19 joint kinds, grounds, limits, drivers (formula of time); `MassProps` per body exists but is the kinematic placeholder (mass 1; drag weights separate) | `App/Solver/Model.h` |
| session API | `load / update / solve / beginDrag / dragStep / endDrag / poses / analyse / simulate(spec, sink) / exportNative`; `Capabilities::dynamics` exists and is false everywhere | `App/Solver/Backend.h` |
| simulation spec | tStart, tEnd, tOutput, errorTolerance, hMin, hMax | `Backend.h` `SimulationSpec` |
| trajectory | poses per body per output time, owned by the neutral layer, in memory only; playback reads it (SI sec 6.6, `7b54d90575`) | `AssemblyObject.h:342` |
| document objects | `Simulation` (Python, `App::GroupExtensionPython`; aTimeStart, bTimeEnd, cTimeStepOutput, fGlobalErrorTolerance, jFramesPerSecond) holding `Motion` children (Joint, MotionType Angular/Linear, Formula) | `CommandCreateSimulation.py:103-360` |
| how motions are read | every `Group` child of the Simulation is a candidate motion; the builder skips children without `Joint` / `MotionType` / `Formula` | `App/AssemblyObject.cpp:516-595`, `:2640-2720` |
| Chrono | core only, static, `BUILD_ASSEMBLY_CHRONO`, no OpenMP / Thrust / HDF5 (so Bullet collision only, no multicore collision), kinematics only: `ChSystemNSC` + `ChSolverSparseQR`, no gravity, mass = drag weight | `src/3rdParty/ChronoCore.cmake`, `App/Solver/ChronoBackend.cpp` |
| OndselSolver | kinematic and drag runs only. `runKINEMATIC`'s acceleration solve assembles the mass matrix (`Part::fillAccICIterJacob`), `ConstantGravity` adds m g, `ForceTorqueItem`s exist, and `ASMTItemIJ` carries reaction histories (`FXonI`... `TZonI`). No forward integrator: `DYNAMIC` exists only as an `AnalysisType` enum value and in comments; contact is declared and empty (Simulator sec 2) | `OS/AccNewtonRaphson.cpp`, `OS/ConstantGravity.cpp`, `OS/ASMTItemIJ.h:37-38` |
| materials | `Part::Feature::ShapeMaterial` (a `Materials::PropertyMaterial`); a Mechanical/Density model; no friction or restitution model | `src/Mod/Part/App/PartFeature.h:74`, `src/Mod/Material/Resources/Models/Mechanical/Density.yml` |
| plotting | the Plot module (matplotlib) | `src/Mod/Plot/` |

Two facts constrain the design.

- **Mass 1 in static solves is deliberate.** Under-constrained assemblies
  move the lightest way, and real masses would change what a static solve
  does (audit sec 3, defect 2). Physical mass therefore must not reach a
  static or drag solve.
- **The Simulation's `Group` is read as a list of motions.** New child
  objects must not carry properties named `Joint`, `MotionType` or
  `Formula`, or the builder will take them for motions. Better still, the
  builder should filter on the Motion proxy type. That is a one-line,
  fork-owned change in `getMotionsFromSimulation`.

Upstream's direction is the same as ours, only behind:
- FreeCAD PR #28156's Chrono backend is kinematic only (`ChSystemNSC`,
  gravity 0, `DoAssembly(POSITION)`, "joint motions are not yet
  implemented"). It has been a draft since 2026-09-21
  (https://github.com/FreeCAD/FreeCAD/pull/28156).
- Koh's FreeCADMbD, the OndselSolver author's own repo, adds `runDYNAMIC`
  with a `DynIntegrator` and BDF starters
  (https://github.com/aiksiongkoh/FreeCADMbD). That is a possible source if
  OndselSolver is ever to integrate forward dynamics. Nothing here depends
  on it.


## 2. How the others do it

### 2.1 Tiers: what each study type computes

| product | kinematic (motion prescribed) | inverse dynamics (reactions, motor effort) | forward dynamics | static / equilibrium | contact |
|---|---|---|---|---|---|
| SOLIDWORKS | Animation: key points, no mass or gravity | Motion Analysis | Basic Motion ("approximate ... mass, collisions, or gravity", for presentations); Motion Analysis (the Adams-derived solver, needs Premium) [1] | -- | Basic: Contact, Gravity, linear Spring; Motion Analysis: solid and curve contact, materials, friction [2] |
| Inventor Dynamic Simulation | imposed motion on joint DOFs (position / velocity / acceleration) | "Unknown Force" (inverse statics over a sweep of one joint) [3] | yes (5th-order Runge-Kutta) | Unknown Force | 2D and 3D contact joints, opt-in per pair [4] |
| Fusion | Drive Joints, Motion Links, Motion Study keyframes ("kinematic motion analysis based on joints") | -- | **none** in the assembly environment; Event Simulation is explicit FEA, microsecond steps, ~1 ms default duration [5] | -- | Contact Sets: interference stops motion; no friction, restitution or stiffness [6] |
| Creo Mechanism | Position, Kinematic (no MDO) | Dynamic with servo motors; a duration of 0 reports initial accelerations and reactions | Dynamic (MDO; adaptive RK3, `mdo_integration_method` auto / implicit / explicit) | Static, Force Balance (loadcell locks) [7] | cam-follower (lift-off, friction, restitution); 3D contact only sphere-sphere, sphere-plane, cylinder-cylinder, cylinder-plane [8] |
| NX / Simcenter 3D Motion | Articulation (interactive, quasi-static or kinematic), Spreadsheet run | Normal run | Normal run (Siemens' own solver since NX 13) | Normal run, static | tessellated 3D contact (sag, length, angle controls) plus analytic primitives; 2D curve contact [9] |
| Solid Edge | Simply Motion (a Dynamic Designer subset; the vendor names MSC/ADAMS as its solver) | Motion Professional | yes | -- | 3D solid and 2D curve contact [10] |
| CATIA V5 / 3DEXPERIENCE | V5 KIN: laws, sensors, clash "stop on collision", swept volumes; no mass [11] | Motion Designer role | 3DEXPERIENCE dynamics scenario: gravity, contacts with friction and restitution [12] | -- | contacts grouped (ball-ball, ball-funnel) [12] |
| Onshape | Animate one DOF of one mate; no keyframes | -- | none; no gravity, no contact while moving [13] | -- | static interference check only [13] |
| MSC Adams (reference) | kinematic analysis (contact is output only there) | yes | GSTIFF / WSTIFF / HASTIFF / HHT / Newmark; I3 / SI2 / SI1 | equilibrium, IC, linear | CONTACT: IMPACT or POISSON, analytic or tessellated (RAPID / Parasolid) [14] |
| Simscape Multibody | per joint: Motion "Provided by Input", Torque "Automatically Computed" | same switch | yes | -- | Spatial Contact Force on convex hulls only; smooth spring-damper [15] |

[1] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/c_deciding_which_motion_study.htm,
https://help.solidworks.com/2025/English/SolidWorks/motionstudies/c_Motion_Analysis.htm
[2] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/r_motionmanager_tools.htm
[3] https://help.autodesk.com/cloudhelp/2019/ENU/Inventor-Help/files/GUID-4BC1E591-2390-49AC-B5AE-AC398440BC0C.htm
[4] https://help.autodesk.com/cloudhelp/2021/ENU/Inventor-Help/files/GUID-E82B3CA0-3943-4932-91E6-1771885511B8.htm
[5] https://help.autodesk.com/cloudhelp/ENU/Fusion-Simulate/files/GUID-8329D22A-3FAD-4A20-B330-2D9522FF76F4.htm,
https://help.autodesk.com/cloudhelp/ENU/Fusion-Assemble/files/GUID-6FEC8081-9E93-4A5F-B31E-8B7AA86790F6.htm
[6] https://help.autodesk.com/cloudhelp/ENU/Fusion-Assemble/files/ASM-CONTACT-SETS.htm
[7] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/analyses_top.html,
https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/AboutDynamicAnalysis.html
[8] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/about_3d_contact.html
[9] the Simcenter 3D Motion Modeling fact sheet (the help itself requires a login),
https://www.emixa.com/hubfs/Emixa/Marketing/PDF%20downloads%20-%20Siemens%20PLM/Simcenter-3D-Motion-Modeling-Fact-Sheet.pdf ;
the solver replacement is from a Siemens knowledge-base title,
https://support.sw.siemens.com/en-US/okba/PL8001017/Replacement-and-retirement-for-NX-Motion-Simulation,-NX-Mechanism-and-NX-Motion-Simulation-for-Ideas/index.html
[10] https://soliddna.com/SEHelp/ST6/EN/se_applications/motion1a.htm,
https://www.design-simulation.com/DDM/SolidEdge/ddm/index.php . No public source confirms a later move to the Simcenter solver.
[11] the Dassault community wiki,
https://3dswym.3dexperience.3ds.com/wiki/catia-user-community/dmu-kinematics-simulator-2-kin_5TFJgAkXQbuBy3zwMAj1dg
[12] https://3dswym.3dexperience.3ds.com/wiki/catia-user-community/catia-motion-designer_EfE4SaMxTYGX56qvFRdLwA,
https://cad.grabcad.com/tutorials/introduction-to-dynamic-simulation-part-2-catia-3dexperience (a tutorial). The 3DEXPERIENCE solver and its settings could not be verified.
[13] https://cad.onshape.com/help/Content/mate.htm,
https://cad.onshape.com/help/Content/View/interference_detection.htm,
https://forum.onshape.com/discussion/12909/animated-assembly-part-interference-and-basic-new-user-questions (a forum post, not a statement from staff)
[14] the Adams 2021.0.2 Solver User's Guide,
https://help-be.hexagonmi.com/bundle/Adams_2021.0.2_Adams_Solver_User_Guide/raw/resource/enus/Adams_2021.0.2_Adams_Solver_User_Guide.pdf
[15] https://www.mathworks.com/help/sm/ref/revolutejoint.html,
https://www.mathworks.com/help/sm/ref/spatialcontactforce.html

**Fusion's gap is worth noting.** One of the two most used hobbyist and
prosumer tools has no rigid-body dynamics in its assembly environment.
Onshape has none either; its users export to Simscape for dynamics
(https://www.mathworks.com/help/sm/ug/onshape-import.html). A Chrono-backed
Assembly would put FreeCAD ahead of both.

### 2.2 Model elements

| element | SOLIDWORKS Motion | Inventor | Creo (MDO) | Adams | take for us |
|---|---|---|---|---|---|
| mass | from material density; Override Mass Properties (mass, CoM, principal moments or Jxx..Jzz) | material in iProperties, override there; sketch-only parts get 1 kg and 0.01 kg m^2 | density per part (computed) or assigned; Mechanism values override the others but only inside Mechanism [16] | PART: CM marker, IP = xx, yy, zz[, xy, xz, yz] | computed from shape and material, override per part at assembly level (Creo's scope) |
| gravity | one per assembly; direction from a face, an edge or X / Y / Z; standard magnitude [17] | **off by default**; 9810 mm/s^2; by a grounded entity or components [18] | on / off per analysis (External Loads tab) | ACCGRAV, components default 0 (opt-in) | per Simulation, **on** for Dynamic and Static (the user asked for one), -Z of the assembly, 9806.65 mm/s^2 |
| motor | rotary, linear, path; constant speed, distance, oscillating, segments, data points (Akima / cubic / linear, of time or of any result), expression; Servo (displacement / velocity / acceleration) in event studies; "motors are not forces" [19] | imposed motion per joint DOF: position / velocity / acceleration, constant or Input Grapher | servo: position / velocity / acceleration x profile (constant, ramp, cosine, SCCA, cycloidal, parabolic, polynomial, table, user-defined); force motor [20] | MOTION: displacement / velocity / acceleration, function of time only | control mode {Position, Speed, Effort} on the Motion object, profile = formula (exists) or table |
| spring / damper | F = -K abs(x - x0)^n sign + F0, n in -4..4; torsional; free length = current distance; damper -c v^n [21] | spring, spring-damper, damper, jack (Input Grapher force), spiral spring; free length = distance at creation [22] | extension, compression, torsion: K, unstretched length; dampers | SPRINGDAMPER, SFORCE, BUSHING, BEAM, FIELD, GFORCE | TSDA / RSDA with a nonlinear exponent and preload; bushing |
| force / torque | action-only or action-reaction; constant, step, harmonic, segments, data points, expression (may read results) [23] | point + direction or components, fixed or associative direction | force / torque loads; force motors | SFORCE, VFORCE, VTORQUE, GFORCE | action-only or action-reaction, global or follower, formula |
| joint limits / stops | (limit mates) | Bounds: min / max with stiffness and damping (penalty) [24] | range limits with a coefficient of restitution [25] | IMPACT / BISTOP functions | limits become stops: hard (NSC) or penalty k, c (SMC), optional restitution |
| joint friction | per mate, dynamic Coulomb only | dry friction coefficient and a radius | per connection, MDO | FRICTION (with stiction) | later (D2): a friction torque from the reaction and a pin radius |
| initial conditions | initial velocity per component [26] | per joint DOF: position, locked, velocity, "computed" | snapshot plus velocity conditions (point, motion axis, angular, slot); validity check [27] | PART VX..WZ, IC, EXACT | the start pose is the solved assembly; initial velocity per joint DOF and per body |
| redundancy | "replace redundant mates with bushings"; otherwise zero reaction at the eliminated mate [28] | Mechanism Status and Redundancies; non-unique joint loads | -- | removed automatically, with a warning | remove dependent rows, label the reactions, bushing option |

[16] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/about_mass_properties.html
[17] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/HIDD_DVE_SIM_GRAVITY.htm
[18] https://help.autodesk.com/cloudhelp/2016/ENU/Inventor-Help/files/GUID-5F4F6D46-D322-4F26-A3B5-B837851F23BA.htm
[19] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/HIDD_DVE_AEMSIM_MOTORS_NEW.htm
[20] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/To_Define_a_Motor.html
[21] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/HIDD_DVE_SIM_SPRINGS.htm
[22] https://help.autodesk.com/cloudhelp/2025/ENU/Inventor-Help/files/GUID-15E9713F-3E47-4F41-B355-1B4D0C1CA092.htm
[23] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/HIDD_DVE_SIM_FORCE.htm
[24] https://help.autodesk.com/cloudhelp/2014/ENU/Inventor/files/GUID-DCAAE740-D53A-40B5-993B-A732F466B1EC.htm
[25] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/ToSetARangeLimit.html
[26] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/HIDD_DVE_SIM_IVELOCITY.htm
[27] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/initial_condit_top.html
[28] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/c_Redundant_Mates_SWMotion.htm

### 2.3 Contact

| | SOLIDWORKS [29] | Inventor | Creo | Adams [14] | Simscape [15] | RecurDyn [30] |
|---|---|---|---|---|---|---|
| definition | component list (all pairs) or Group 1 against Group 2 | a contact joint per pair, 2D (planar loops) or 3D | a 3D contact per pair; a cam-follower connection | CONTACT with IGEOM / JGEOM lists (up to 32767) | a block per pair | contact entities by family |
| geometry | polygons at a "3D Contact Resolution", or "Use Precise Contact" (B-rep, "analytically correct", slower) | 3D: points on the tessellation; 2D: loops | analytic only: sphere, plane, cylinder, plus a vertex as a sphere | analytic (cylinder, ellipsoid, box, frustum; 2D point / plane / circle / arc / curve) or tessellated (RAPID) or Parasolid; "whenever possible use analytical geometries" | convex hull of a solid, disk, plane, point, grid surface; **no mesh, so concave parts are wrong** | 18 analytic primitive pairs, Geo (node-to-facet), Solid; "If you can apply the primitive contact, please use Primitive Contact"; Hermite smoothing of facets; CPM divides k by the number of contact nodes |
| normal law | Impact: Fn = k g^e + STEP(g, 0, 0, dmax, cmax) dg/dt; or a restitution coefficient | stiffness, damping (spring-damper on penetration); 2D: restitution, default 0.8 | Young's modulus, Poisson's ratio, damping per part (Hertz-like; the law is not documented) | IMPACT (k, e >= 1.5, c ~ 1 % of k, dmax ~ 0.01 mm; View defaults 1e5 N/mm, 10 N s/mm, e 2.2) or POISSON (penalty, restitution; "HHT is incompatible with POISSON") | smooth spring-damper: 1e6 N/m, 1e3 N/(m/s), transition width 1e-4 m | K = (4/3) E sqrt(R), exponent 1.5, damping ~ 1e-4 K |
| friction | mu_s, mu_d with static and dynamic transition velocities (dry steel defaults: 0.30 / 0.25, 0.1 / 10.16 mm/s); material pair table [31] | coefficient plus a regularisation velocity (1e-3 m/s in the examples) | static and kinetic (MDO) | mu_s, mu_d, stiction and friction transition velocities (> 5 ERROR); "no contact stiction in Adams" | mu_s 0.5, mu_d 0.3, critical velocity 1e-3 m/s | static and dynamic with a threshold velocity |
| restitution | as an alternative to impact | 2D only | cams and limits | POISSON | via damping | rebound damping factor |
| cams | curve-to-curve: "always touch" = a constraint, intermittent = a force | 2D contact between loops | cam-follower: "Enable liftoff", restitution, friction; cams are extrusions (2D) [32] | PTCV / CVCV bilateral; CONTACT for lift-off | -- | Cam 2D, curve-to-curve |

[29] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/HIDD_DVE_SIM_3DCONTACT.htm
[30] FunctionBay, "Easy guide to understanding RecurDyn Contact" (2016),
https://support.functionbay.com/uploads/2017/02/2016_Easy-guide-to-understanding_RecurDyn_Contact_20170124_ENG_KOR.pdf ,
https://functionbay.com/documentation/onlinehelp/Documents/contact3.htm
[31] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/c_friction_overview_motion.htm
[32] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/about_cam_follconns.html,
https://support.ptc.com/help/creo/creo_pma/r7.0/usascii/simulate/mech_des/cams/ToDefinePropertiesForCam-FollowerConnections.html

### 2.4 Solver settings exposed

| product | output | step / integrator | tolerance | events |
|---|---|---|---|---|
| SOLIDWORKS | frames per second (sampling, not playback speed) | GSTIFF (default), SI2_GSTIFF, WSTIFF; maximum iterations (25); initial / minimum / maximum step; Jacobian re-evaluation [33] | Accuracy (a contact tutorial uses 1e-4 and 1000 fps) | none; "set [the maximum step] to the same order as the short-lived events" |
| Inventor | final time, images (100 by default) | RK5, steps rejected above Solver Precision | 1e-6 translational, rotational 1e-3; Assembly Precision 1e-6 m; capture and regularisation velocities [34] | "precise calculations for simulation events" (jumps to the contact instants) |
| Creo | duration, frame count / rate, "minimum interval" (really the maximum step) | adaptive RK3; auto / implicit / explicit | relative tolerance (1e-3, range 1e-10..0.1) x characteristic length (mean bounding-box diagonal) [35] | the step is cut at limits and other events |
| Adams | output steps | GSTIFF / WSTIFF / HASTIFF / HHT / Newmark; I3 / SI2 / SI1; HINIT = output / 20; HMAX = output step; KMAX 6; MAXIT 10; HHT alpha -0.3 | ERROR 1e-3 (1e-5 for HHT); "run at the default and again 10x tighter, compare" | a new contact rejects the step and resets the order to 1; SENSOR located to TERROR 1e-6 by secant or bisection |
| NX (RecurDyn-era API) | step count | static Newton / robust Newton; dynamic integrator type; initial / max step | static and dynamic error tolerance [36] | interference objects with Stop_On_Event |

[33] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/HIDD_MOTION_STUDY_CM_ADV_OPTS.htm,
https://help.solidworks.com/2025/English/SolidWorks/motionstudies/c_integration_methods_analysis.htm
[34] https://help.autodesk.com/cloudhelp/2026/ENU/Inventor-Help/files/GUID-3F34ACBD-79D5-4EAC-B0E7-3334ECB2FD65.htm
[35] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/To_define_mech_settings.html
[36] https://docs.plm.automation.siemens.com/data_services/resources/nx/12/nx_api/custom/en_US/nxopen_python_ref/NXOpen.Motion.RecurdynSolverProperty.html

What users actually touch is few: duration, an output rate, one accuracy
number, and an integrator choice hidden under "Advanced". The rest are
defaults that seldom change.

### 2.5 Results and post-processing

| | SOLIDWORKS | Inventor | Creo | Adams / NX / CATIA |
|---|---|---|---|---|
| plots | Results and Plots: displacement / velocity / acceleration, trace path, CoM; motor force and torque, reaction, friction, contact; momentum, energy, power; in any component's frame, against time, frame or another result [37] | Output Grapher: per joint P, V, A, effort, driving force; contact status; spring extent; 3D contact penetration, points, force; FFT; compare runs; min / max search [38] | measures: position, velocity, acceleration, separation, cam (curvature, pressure angle, slip); MDO: connection reaction, net load, loadcell, impact, impulse, user expression [39] | Adams measures (object, point-to-point, function); NX: graphs synchronised with the animation, FFT, integrate, differentiate |
| traces | trace path, to a Curve feature | traces to a 2D or 3D sketch | trace curves and cam-synthesis curves, reusable as cam or slot curves [40] | CATIA traces and swept volumes |
| interference | Find Interferences Over Time: frame, time, components, volume [41] | -- | interference during playback; motion envelope as a part | Adams run-time clearance; NX faceted or precise; CATIA clash "stop on collision" |
| sensors | motion data sensors (max / min / RMS / value at a time) with alerts; they stop a run only as event-based triggers [42] | alert colours | -- | Adams SENSOR: HALT, RETURN, RESTART, DT, located in time |
| export | CSV per plot; motion loads to Simulation (one or several frames); .adm; video | Excel or text; FEA (Stress Analysis, ANSYS); AVI | loads to Creo Simulate for one body; AVI / MPEG | spreadsheet, numeric, movies; NX load transfer, JT |
| result lifecycle | Calculate before replay; out-of-date marker | construction mode against simulation mode; stale results flagged | each run a playback file, reusable across sessions [43] | .res / .req / .gra files |

[37] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/r_cats_subcats_motion_results.htm
[38] https://help.autodesk.com/cloudhelp/2019/ENU/Inventor-Help/files/GUID-B9353E90-69F7-46DA-9AFC-15249BC4BF2E.htm
[39] https://support.ptc.com/help/creo/creo_pma/r9.0/usascii/simulate/mech_des/types_of_measures.html
[40] https://support.ptc.com/help/creo/creo_pma/r7.0/usascii/simulate/mech_des/results/about_trc_curves.html
[41] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/HIDD_INTERFERENCE_CHECK_DLG.htm
[42] https://help.solidworks.com/2025/English/SolidWorks/motionstudies/r_event_based_motion_view.htm
[43] https://support.ptc.com/help/creo/creo_plus/usascii/simulate/mech_des/about_playback.html

### 2.6 FreeCAD-side precedents

| project | what it did | why it stayed niche |
|---|---|---|
| upstream Assembly (1.1) | kinematic Simulation and Motion objects; formula of `time`; `runKINEMATIC` | no mass, forces or contact |
| Assembly3 | SolveSpace / sympy constraint solving; interactive move and "trace part move" (https://github.com/realthunder/FreeCAD_assembly3) | no time-based motion |
| Assembly4 | animation by stepping a document variable; plot and export libraries (https://github.com/leoheck/FreeCAD_Assembly4.1/blob/main/docs/Tutorial2/TUTORIAL_2.md) | kinematic, by re-solving sketches |
| Nikra-DAP | planar dynamics (Nikravesh's DAP): bodies, revolute and translation joints, gravity, springs and dampers, a material density (default 1000 kg/m^3), plots and traces (https://github.com/NikraDAP/FreeCAD-NikraDAP) | planar only, requires Assembly4, no contact |
| MBDyn "Dynamics" workbench | writes an MBDyn input, runs it, animates and plots (https://github.com/sasobadovinac/freecad-mbdyn-dynamics-workbench) | an external binary; numbers held as `App::PropertyString` (an anti-pattern); successors MbDFEM and a GSoC 2026 "Motion workbench" are in progress (https://blog.freecad.org/2026/02/27/amazing-multibody-dynamics-with-fem/, https://blog.freecad.org/2026/05/02/gsoc2026-projects-announced/) |
| Assembly2MuJoCo | exports Assembly joints to MJCF; no density yet (https://github.com/AnesBenmerzoug/FreeCAD-Assembly2MuJoCo) | export only |
| PR #28156 | the solver interface plus a Chrono backend, kinematic only | a draft; breaks models while dragging |

The lesson from these is consistent: none of them lives in the native
assembly model. Each is tied to Assembly4, export-only, or an external
binary fed strings. Here the dynamics run on the same document objects,
the same neutral model, the same session and the same playback as the
kinematic simulation.

### 2.7 Patterns we take, and from where

| # | pattern | from |
|---|---|---|
| P1 | one Simulation object, an analysis-type switch, kinematic and dynamic in one UI | SOLIDWORKS study types, Creo analysis types |
| P2 | bodies and joints from the existing assembly; rigid groups are one body with combined mass | every product; SOLIDWORKS rigid groups |
| P3 | mechanism elements (springs, dampers, contacts, mass overrides) defined once; each analysis picks which loads and motors it includes | Creo (Motors and External Loads tabs) |
| P4 | motor = control mode x profile, with a profile library and a table with Akima / cubic / linear interpolation | Creo servo motors, SOLIDWORKS data points, Simscape actuation modes |
| P5 | contact opt-in, by groups (A against B, or all pairs in a list) | SOLIDWORKS contact groups, Fusion contact sets, Adams geometry lists |
| P6 | a material-pair table for mu_s / mu_d / restitution, overridable per contact set | SOLIDWORKS material pairs, the Adams View table |
| P7 | an impact law k, e, cmax, dmax with STEP-ramped damping, or restitution; friction with transition velocities | Adams IMPACT / POISSON, SOLIDWORKS elastic properties |
| P8 | analytic primitives first, tessellation second, a "precise" switch | Adams, RecurDyn, SOLIDWORKS "Use Precise Contact", NX |
| P9 | a cam is a joint (persistent) or a contact (lift-off) on the exact curve | Creo "Enable liftoff", SOLIDWORKS curve contact, Adams PTCV / CVCV |
| P10 | joint stops with restitution, or with stiffness and damping | Creo range limits, Inventor bounds, Simscape limits |
| P11 | redundant constraints removed and reported; bushing replacement | SOLIDWORKS, Adams, Inventor |
| P12 | results defined after the run (plots, traces, sensors) from stored state | SOLIDWORKS Results and Plots, Creo measures |
| P13 | traces to a curve or sketch feature; trace curves reused as cam profiles | SOLIDWORKS, Inventor, Creo cam synthesis |
| P14 | interference over the motion as a report: frame, time, pair, volume | SOLIDWORKS Find Interferences Over Time, Adams run-time clearance |
| P15 | sensors that stop a run, located in time | Adams SENSOR, CATIA "stop on collision", NX Stop_On_Event |
| P16 | an accuracy check: run again 10x tighter and compare | Adams View guidance; SOLIDWORKS "run multiple simulations with variations in settings" |
| P17 | a drag tool that records an initial pose, through the solver | Inventor Dynamic Part Motion |
| P18 | stale results flagged; Calculate before replay | Inventor, SOLIDWORKS |
| P19 | loads at chosen frames exported to FEM | SOLIDWORKS, Inventor, Creo |


## 3. What Chrono 10's core already gives us

Everything below is in `Chrono_core` as `ChronoCore.cmake` builds it. No new
Chrono module is needed until D4.

| need | Chrono class (CH/...) | note |
|---|---|---|
| system | `physics/ChSystemNSC.h`, `physics/ChSystemSMC.h` | one contact method per system; with no contact the choice is moot (https://api.projectchrono.org/simulation_system.html) |
| body with CoM off the reference frame | `physics/ChBodyAuxRef.h` (`SetFrameCOMToRef`, `SetFrameRefToAbs`) | our body frame is the part placement; the CoM comes from `BRepGProp` |
| gravity | `ChSystem::SetGravitationalAcceleration` | |
| springs and dampers | `ChLinkTSDA` (k, r, rest length, actuator force, `ForceFunctor` for nonlinear laws, internal ODE states), `ChLinkRSDA` | the SOLIDWORKS law `-K abs(x - x0)^n sign + F0` is a `ForceFunctor` |
| bushings | `ChLinkBushing` (Mount: 6 compliant DOF; Spherical: 3) | also how a redundant joint is replaced |
| motors | `ChLinkMotorRotationAngle / Speed / Torque`, `ChLinkMotorLinearPosition / Speed / Force`, `...Driveline` (with `ChShaft*`) | `GetMotorTorque()` / `GetMotorForce()` give the effort; `ChLinkMotorRotationSpeed::AvoidAngleDrift` |
| drivelines | `ChShaft`, `ChShaftsGear`, `ChShaftsClutch`, `ChShaftsMotor*`, `ChShaftsTorsionSpring`, `ChShaftsPlanetary` ... | a later option for gearboxes; out of scope here |
| limits as stops | `ChLinkLock` with `ChLinkLimit` (unilateral `constr_lower` / `constr_upper`, or penalty with Kmin / Kmax, Rmin / Rmax) | `ChLinkMate` has no limits; the kinematic backend's active-set links (SI sec 6.15) are not stops |
| point on a curve | `ChLinkLockPointSpline` (a `ChLine`), `ChLinkLockTrajectory` | a fallback for (b); the queued neutral link (a) is preferred, as it is exact on the B-rep curve |
| collision | `collision/bullet/` (`ChCollisionSystem::Type::BULLET`); `MULTICORE` needs Thrust and is not built | |
| shapes | sphere, box, cylinder, capsule, cone, ellipsoid, barrel, rounded box and cylinder, cylindrical shell, convex hull, triangle mesh, 2D arc / segment / path, point, segment | `ChCollisionShapePath2D`: closed paths of arcs and segments in a plane, "useful for mechanisms that work on a plane" |
| mesh behaviour | `ChCollisionShapeTriangleMesh(material, mesh, is_static, is_convex, radius)`: a `ChTriangleMeshConnected` becomes sphere-swept per-triangle proxies (`cbtCEtriangleShape`, winged edges); static becomes a BVH mesh; convex becomes a hull; **otherwise Chrono silently runs HACDv2 convex decomposition (512 hulls)** (`CH/collision/bullet/ChCollisionModelBullet.cpp:446-615`) | we choose the branch explicitly, never the silent fallback |
| decomposition | `ChConvexDecompositionHACD`, `...HACDv2` (`chrono_thirdparty/HACD*`) | |
| pair filtering | `ChCollisionSystem::BroadphaseCallback` (return false to skip a pair), `NarrowphaseCallback`; 15 collision families | callbacks, since our contact sets are arbitrary pairs |
| per-pair materials | `ChContactContainer::AddContactCallback` (edits the composite material per contact) | the material-pair table |
| contact results | `ChContactContainer::ReportAllContacts` (points, normal, distance, forces), `GetContactableForce` | contact force plots, interference events |
| contact materials | NSC: friction (static and sliding), rolling, spinning, restitution, cohesion, compliance; SMC: Young's modulus, Poisson's ratio, restitution, adhesion, kn / kt / gn / gt | |
| SMC force laws | Hooke, Hertz (default), PlainCoulomb, Flores; `SetContactForceTorqueAlgorithm(ChContactForceTorqueSMC)` for our own | the Adams IMPACT law goes here |
| timesteppers | `EULER_IMPLICIT_LINEARIZED` (default; DVI contact), `EULER_IMPLICIT`, `EULER_IMPLICIT_PROJECTED`, `TRAPEZOIDAL`, `NEWMARK`, `HHT` (2nd order, numerical damping; not with DVI contact), explicit RK, Heun, Leapfrog | implicit steppers have step control on Newton convergence (`SetStepControl`, `SetMinStepSize`), **not on local truncation error** |
| linear solvers | SparseLU, SparseQR (rank-revealing), the VI solvers PSOR / APGD / BB / ADMM, MINRES, GMRES, BiCGSTAB | Pardiso and MUMPS are separate modules |
| statics | `DoStaticLinear`, `DoStaticNonlinear`, `DoStaticNonlinearRheonomic`, `DoAssembly` | NSC contacts are unsupported in assembly (`UNSUPPORTED_CONTACTS`) |
| reactions | `ChLink::GetReaction1/2()` (a wrench) | |
| flexible bodies | `fea/`: corotational tetrahedra (4 and 10 node), hexahedra, ANCF, Euler / IGA / Timoshenko beams, Reissner / BST shells, `ChMesh`, `ChContactSurfaceMesh`, loaders from TetGen / Abaqus / GMF | D4; modal reduction is `chrono_modal`, which needs Spectra |
| serialisation | `ChArchiveJSON` | already the backend's `exportNative` |

**`chrono_cascade`: read it, do not link it.** It requires OCCT 7.9.2 or
7.9.3 ("Other versions of OpenCASCADE are not compatible",
https://api.projectchrono.org/module_cascade_installation.html), and this
fork is on 8.0.1. All it adds is STEP loading, mass properties from shapes,
and triangulation for display, and we have all three already:
- the document;
- `BRepGProp`;
- `BRepMesh` plus the TShape tables of `docs/TShapeRenderCache.md`.

`ChCascadeBodyEasy` remains a short reference for how a body's CoM is set
through `ChBodyAuxRef` (Simulator sec 6).


## 4. Scope and phasing

| phase | delivers | depends on | effort (one developer) | risk |
|---|---|---|---|---|
| D0 mass and units | mass, CoM and inertia per part from shape and material, overrides, an assembly mass-properties table; the unit contract (sec 8.1); `Model::dynamics` with `Inertia` | nothing | 1-2 weeks | low |
| D1 dynamics without contact | Dynamic and Static analyses on Chrono; gravity; springs, dampers, bushings; forces and torques; motor modes Position / Speed / Effort; limits as stops; initial velocities; redundancy removal; inverse dynamics in kinematic runs (Chrono; Ondsel optional); results store, plots, CSV, traces; stale flag | D0; queued (a), so that every joint kind runs | 6-8 weeks | medium: conditioning, redundancy, the expectations around energy drift |
| D2 contact | contact sets, materials and the pair table, the geometry ladder (sec 7), the impact and restitution laws, friction, cams with lift-off (curve contact), 2D path contact, stops with restitution, drop tests, contact plots, interference over the motion, sensors that stop a run | D1; queued (b) | 8-12 weeks | high: CAD geometry quality, performance, noisy forces, parameter tuning |
| D3 out of process | `RemoteBackend` for any session (SI sec 4.7), progress, cancel, crash isolation, the browser tier playing the stored results | the generic RemoteBackend | 3-4 weeks | medium |
| D4a loads to FEM | joint reactions and inertia loads at chosen frames into a FEM analysis (inertia relief or fixed) | D1; the Fem workbench | 2-3 weeks | medium |
| D4b flexible bodies | a part meshed by the Fem workbench (tetrahedra) as a Chrono `ChMesh` (corotational), or modal reduction (`chrono_modal`) | D2; Spectra if modal | 8-12 weeks | high |

**Phase order: D0, D1, D3, D2, D4** (decided, open question 4). D3 comes
before D2, so a contact run that hangs or crashes on a bad mesh cannot
take the user's session with it. D1 runs in process on a worker thread,
with cancel.

**The order within D1 is chosen so each step ships.**

1. Gravity and masses, with a Dynamic analysis of an unpowered mechanism.
   A pendulum, or a slider-crank falling under gravity.
2. Springs and dampers.
3. Motor modes, and reactions with their plots.
4. Static equilibrium, which is Creo's "Static". It is what a CAD user
   means by "where does it settle", and costs one `DoStaticNonlinear`.

**D2 starts with primitives and convex hulls**, which are fast and exact.
Curve contact for cams follows, then meshes. Meshes carry the risk, so they
come last within the phase.

**Ordering against the audit's decision 5 ("interference and collision come
last").** D2's geometry ladder and the audit's phase 5 interference check
use the same per-TShape contact cache (sec 7). Whichever comes first builds
the cache, and the other reuses it. D2 itself stays after the rest of audit
phase 4.

**Out of scope here**, each to be revisited on its own:
- hydraulics (`ChHydraulicActuator`);
- drivelines (`ChShaft*`);
- control co-simulation (FMI is a separate `chrono_fmi` module);
- granular material and fluids (Simulator stages 4-6);
- vehicles.


## 5. Document model

### 5.1 Principles

- **New objects live in new files.** Joints, `JointObject.py` and
  `AssemblyObject.cpp`'s translation are upstream code (SI sec 6.1), and
  upstream joints get no dynamics properties. Per-joint dynamic data
  (initial velocity, friction, stop behaviour) is held by new objects that
  link to a joint, as `Motion` already does.
- **Both Creo's split and SOLIDWORKS's per-study view (P3; decided, open
  question 1).** Where an element is owned and where it is shown are kept
  apart, as in the rest of FreeCAD:
  - **Owned** by an assembly-level **Physics** group: mass overrides,
    springs and dampers, bushings, contact sets, contact materials, cam
    contacts, joint dynamics. Physics is an `App::DocumentObjectGroup`,
    like Joints. Like Joints, its members are also listed in the
    assembly's own Group, because `GroupExtension::addObjects` adds them
    to the enclosing GeoFeatureGroup. The assembly therefore owns their
    placement and 3D, and the group only organises the tree.
    (Revised 2026-10-24: with the C++ object model Physics becomes an aux
    view of the assembly's own `Group`, `docs/AssemblyObjectModel.md`
    sec 3; ownership is unchanged.)
  - **Used** by a Simulation through its `Elements` link list. The tree
    does not show them under the Simulation: they are listed in its task
    panel, and the tree stays as it is. Claiming them there (the
    PartDesign `AuxGroup` way, `PartDesign/App/AuxGroup.h`) is possible
    later if a reason turns up; the user's call (2026-10-24) is to wait
    and see.
  - `Elements` is a plain link list, not group membership. A Simulation
    is a `GroupExtensionPython`, and a GroupExtension allows one plain group
    per object (`GroupExtension.cpp`, `addObjects`). Putting an element in
    a Simulation's Group would take it out of Physics.
  - **No private elements.** Every element lives in Physics, the one place
    a user looks for them, as a Body's sketches all show in its Sketches
    group. A Simulation uses an element by linking to it.
    - In the study task panel the user either creates an element or picks
      existing ones from a list. A created element always goes into
      Physics, and is added to that study's `Elements`.
    - A new study's list starts with every existing element ticked (Creo's
      default), and the user unticks what it leaves out. That is a panel
      default, not a stored flag.
    - Dragging an element onto a Simulation adds it there. An element no
      study uses stays in Physics, as an unused sketch stays in its Body.
  - Each Simulation holds its scenario: analysis type, motions and motors,
    gravity, applied forces, initial conditions, sensors, settings and
    results. These are per-study by nature and stay in its Group.
- **Every number is a typed property** (`App::PropertyLength`,
  `PropertyForce`, `PropertyStiffness`, and so on). Values are stored in
  internal units (sec 8.1) and shown through the unit schema. Nothing is
  stored as `App::PropertyString` (the anti-pattern of sec 2.6).
- **Backward compatibility** follows the pattern `Simulation` and `Motion`
  already use: `hasattr` guards in `__init__` and in `onDocumentRestored`.
  - A file without the new properties opens as a Kinematic simulation and
    behaves exactly as before.
  - In an older build, a new file restores the new Simulation properties as
    dynamic properties and ignores them. The new object types load with
    "proxy class not found", show as broken and do no harm.
  - Results blobs are ordinary included files.

### 5.2 Objects

Dynamic objects are all Python features (`App::FeaturePython` with a proxy),
as `Simulation` and `Motion` are. Their solver-facing parts are read in C++
by `AssemblyObject`'s builder, by property name, as motions are read today.

| object | lives in | properties (internal units) | Chrono |
|---|---|---|---|
| `Simulation` (existing, extended) | assembly's Simulations | existing a..j; new: `AnalysisType` {Kinematic, Dynamic, Static} (default Kinematic); `SolverBackend` (empty: the first registered backend that can run the type); `Gravity` (vector, default (0, 0, -9806.65) mm/s^2) and `GravityEnabled` (default true for Dynamic and Static); `ContactMethod` {Impact, Restitution}; `Integrator` {Auto, HHT, ImplicitEuler}; `MaxStep` (Time; default = output step); `Accuracy` (float; maps to Newton tolerances, sec 8.2); `ContactTolerance` (Length; the contact mesh deflection, sec 7.2); `RedundantJoints` {Remove, Bushings}; `Elements` (LinkList: the Physics elements it uses, sec 5.1); `KeepResults` {None, Motion, All} (default Motion); `Results` (PropertyFileIncluded); `ResultsHash` (string, read-only) | system settings |
| `Motion` (existing, extended) | Simulation | existing Joint, MotionType, Formula; new: `Control` {Position, Speed, Effort} (default Position); `Profile` (link to a `MotionTable`, optional, overriding Formula) | `ChLinkMotorRotationAngle / Speed / Torque`, `...Linear...`; Position = today's motion |
| `MotionTable` | Simulation | `Points` (PropertyVectorList of (x, y)); `Independent` {time, joint value}; `Interpolation` {Akima, Cubic, Linear} (P4) | a `ChFunction` built from the table (the neutral evaluator's) |
| `MassOverride` | Physics | `Part` (link); `Mode` {Density, Mass, Full}; `Density`; `Mass`; `CenterOfMass` (vector, in the part frame); `Inertia` (6 values, about the CoM) | `ChBodyAuxRef` mass and inertia |
| `SpringDamper` | Physics | `Reference1`, `Reference2` (two picks, as joints pick; points on two parts or on ground); `Kind` {Translational, Torsional}; `Stiffness`; `Exponent` (default 1); `FreeLength` / `FreeAngle` (default: the distance or angle at creation; `TrackModel` bool, as SOLIDWORKS's "update to model changes"); `Preload`; `Damping`; `DampingExponent`; display fields (coil diameter, coils) | `ChLinkTSDA` / `ChLinkRSDA` with a `ForceFunctor` |
| `Bushing` | Physics | `Reference1`, `Reference2`; `TranslationalStiffness`, `RotationalStiffness`, `TranslationalDamping`, `RotationalDamping` (3 each, in frame I); `Preload` | `ChLinkBushing` (Mount) |
| `AppliedLoad` | Simulation | `Kind` {Force, Torque}; `Reference` (point and direction, or a vector); `Direction` {Global, FollowPart}; `Reaction` (link to a part, or empty for action only); `Formula` (magnitude over time) | `ChForce`, or a `ChLoad` body-body wrench |
| `JointDynamics` | Physics | `Joint` (link); `StopMode` {Hard, Penalty, Off} for that joint's limits; `Restitution`; `StopStiffness`; `StopDamping`; `Friction` (D2) with `PinRadius` | `ChLinkLock` limits |
| `InitialCondition` | Simulation | `Joint` + `Rate` (rad/s or mm/s), or `Part` + `LinearVelocity` + `AngularVelocity` (global); `Locked` (bool) | body velocities after the start assembly |
| `ContactSet` | Physics | `GroupA` (LinkList of parts), `GroupB` (empty: all pairs within A) (P5); `Material` (link to a `ContactMaterial`, or empty: from the parts' materials through the pair table); overrides of every material field; `Geometry` {Auto, Precise, Fast}; `Enabled` | broadphase filter, composite material callback |
| `ContactMaterial` | Physics (also a default library) | `StaticFriction`, `DynamicFriction`, `StictionVelocity`, `FrictionVelocity` (P7; defaults: SOLIDWORKS's dry steel, 0.30 / 0.25 / 0.1 mm/s / 10.16 mm/s); `Restitution`; `Stiffness`, `Exponent`, `MaxDamping`, `Penetration` (impact law); `YoungsModulus`, `PoissonRatio` (Hertz option) | SMC or NSC material |
| `CamContact` | Physics | `Cam` (edge or edges on one part), `Follower` (a vertex, a circle or an edge on another), `LiftOff` (bool; false = the PointOnCurve joint of queued (b)); `Material`; `OutwardNormal` flip | false: the neutral PointOnCurve link; true: a curve-contact force (sec 7.4) or a 2D path contact |
| `Sensor` | Simulation | `Measure` (sec 8.5 grammar: a channel name and a component); `Compare` {Above, Below, Crosses}; `Value`; `Action` {Stop, Mark} | evaluated per accepted step by the neutral layer |
| `ResultPlot` | Simulation | `Channels` (list of names), `XAxis` (time or a channel), `Frame` (a part or global) | none: post-processing |
| `TraceCurve` | the assembly (a Part::Feature) | `Simulation` (link), `Part`, `Point` (in the part frame), `Relative` (part or ground); `Shape` = a B-spline through the stored frames | none: post-processing |

**Mass properties without an override** come from each part's shape
through `BRepGProp::VolumeProperties`, using the density of its
`ShapeMaterial`.
- A part with no material, or a material without a density, gets the
  preference `Mod/Assembly/DefaultMaterial`, a material card whose default
  is aluminium (2700 kg/m^3) (decided, open question 11).
- The panel then shows "assumed material" in warning colour. The run is
  not refused.
- Inventor gives sketch-only parts 1 kg and 0.01 kg m^2, and Nikra-DAP and
  Simscape default to 1000 kg/m^3. A visible warning matters more than
  which default is used.
- A part without a solid (a sketch, a datum) has no mass. A body made only
  of massless parts is refused in a Dynamic run, with its name.

The neutral builder combines the members of a bundle (a rigid group, Fixed
joints, a rigid sub-assembly) into one `Inertia` by the parallel-axis
theorem. This is SOLIDWORKS's rigid-group rule, and the bundling happens in
the neutral layer already (SI sec 4.4).

### 5.3 Persistence and results

- `Results` is an `App::PropertyFileIncluded` on the Simulation. It goes
  through the fork's file blob store (`docs/FileBlobsManager.md`), so undo
  copies a reference, not the data.
- The format is a zip holding `index.json` plus one `.npy` per channel
  (sec 8.4).
- `KeepResults` {None, Motion, All} defaults to Motion, which saves what
  playback and the animation exports need. The storage is in sec 8.4
  (decided, open question 5). With None, the results live in memory and in
  the transient directory only, and a reopened document shows "not
  calculated".
- `ResultsHash` is a hash of the neutral model, the spec, the backend name
  and the backend version.
  - A recompute that changes the hash marks the results stale. This is
    P18: an overlay icon on the Simulation, and Calculate enabled.
  - Stale results still play, labelled.


## 6. Neutral layer extensions and backend capabilities

### 6.1 Model.h (sketch)

`Model::version` becomes 2. Every new record is in one sub-structure that a
kinematic build leaves empty. Then:
- `sameExceptDragWeights`, the ASMT gate and the kinematic results do not
  change;
- `TopologyKey` gains only "the dynamics block is present", plus the counts
  and kinds of its records.

```cpp
namespace Assembly::Solver {

// Physical mass properties of a body (a bundle), for dynamics only. Kinematic
// solves keep Body::mass (the placeholder, audit defect 2) and never read this.
struct Inertia {
    Id body = NoId;
    double mass = 0.0;                    // kg
    Base::Vector3d com;                   // mm, body frame
    std::array<double, 6> inertia {};     // kg mm^2 about com, body axes: xx yy zz xy xz yz
    enum class Origin : std::uint8_t { Shape, Override, AssumedDensity } origin {};
};

enum class MotorControl : std::uint8_t { Position, Speed, Effort };
// Driver gains:  MotorControl control = MotorControl::Position;
// Speed: the formula is the rate (rad/s, mm/s); Effort: torque (mN mm) or force (mN).

struct Table {                            // a MotionTable, for formulas and functors
    std::vector<double> x, y;
    enum class Interp : std::uint8_t { Akima, Cubic, Linear } interp {};
};

enum class ForceKind : std::uint8_t {
    SpringDamper,         // along oJ - oI (TSDA)
    TorsionSpringDamper,  // about zI, thetaZ(I, J) (RSDA)
    Bushing,              // 6 components in frame I
    Force, Torque         // on frame J's body; frameI NoId: action only
};
struct ForceElement {
    Id id = NoId, source = NoId;
    ForceKind kind {};
    Id frameI = NoId, frameJ = NoId;
    double k = 0, exponent = 1, rest = 0, preload = 0, c = 0, cExponent = 1;
    std::array<double, 6> k6 {}, c6 {};   // Bushing
    Formula magnitude;                    // Force / Torque, along zI or zJ (follower)
    bool follower = false;
};

struct ContactMaterial {
    double muStatic = 0.3, muDynamic = 0.25, vStiction = 0.1, vFriction = 10.16;  // mm/s
    double restitution = 0.0;
    double stiffness = 0, exponent = 1.5, maxDamping = 0, penetration = 0.01;     // impact law
    double young = 0, poisson = 0.3;      // Hertz option; 0: not given
};

// Contact geometry, shared by content (sec 7): many bodies may reference one record.
struct ContactGeometry {
    std::uint64_t hash = 0;               // content hash
    enum class Kind : std::uint8_t {
        Sphere, Box, Cylinder, Capsule, Cone,     // size[] holds the dimensions
        ConvexHull,                               // points
        Mesh,                                     // points + triangles (connected)
        Hulls,                                    // a decomposition: one hull per part
        Path2D,                                   // arcs and segments in the xy plane
        Curve                                     // exact curve (B-spline poles, knots, weights)
    } kind {};
    std::array<double, 3> size {};
    std::vector<Base::Vector3f> points;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<std::uint32_t> partStart;         // Hulls: first point of each hull
    double sweptRadius = 0;                       // Mesh: sphere-swept layer
    double deflection = 0;                        // the tessellation's chord error, for reports
};
struct ContactShape { Id body = NoId; Id geometry = NoId; Base::Placement local; Id material = NoId; };
struct ContactPair {                       // a ContactSet: A x B, or all pairs within A
    Id id = NoId, source = NoId;
    std::vector<Id> groupA, groupB;        // body ids
    Id material = NoId;                    // NoId: per-body materials via the pair table
};
struct CurveContact {                      // a cam that can lift off
    Id id = NoId, source = NoId;
    Id curve = NoId;                       // ContactGeometry of kind Curve, on frameI's body
    Id frameI = NoId, frameJ = NoId;       // the follower's point (and radius) on frameJ
    double followerRadius = 0;
    Id material = NoId;
};
struct Stop { Id limit = NoId; enum class Mode : std::uint8_t { Hard, Penalty } mode {};
              double restitution = 0, k = 0, c = 0; };
struct InitialVelocity { Id joint = NoId; double rate = 0;          // or
                         Id body = NoId; Base::Vector3d v, w; };    // mm/s, rad/s, global
struct Sensor { Id id = NoId, source = NoId; std::string channel; std::uint8_t compare = 0;
                double value = 0; bool stop = true; };

struct Dynamics {
    Base::Vector3d gravity;                // mm/s^2, assembly frame; zero: off
    std::vector<Inertia> inertias;
    std::vector<ForceElement> forces;
    std::vector<Table> tables;
    std::vector<ContactMaterial> materials;
    std::vector<ContactGeometry> geometry;
    std::vector<ContactShape> shapes;
    std::vector<ContactPair> contacts;
    std::vector<CurveContact> curveContacts;
    std::vector<Stop> stops;
    std::vector<InitialVelocity> initial;
    std::vector<Sensor> sensors;
};
// Model gains:  std::optional<Dynamics> dynamics;   (empty for every kinematic build)
}
```

The cam joint of queued (b) is a `JointKind::PointOnCurve` that references
the same `ContactGeometry` of kind Curve. One curve record therefore serves
the persistent cam (a joint) and the lift-off cam (a `CurveContact`).

### 6.2 Backend.h (sketch)

```cpp
enum class Analysis : std::uint8_t { Kinematic, Dynamic, Static };

struct SimulationSpec {                 // existing fields kept
    Analysis analysis = Analysis::Kinematic;
    enum class Integrator : std::uint8_t { Auto, HHT, ImplicitEuler } integrator {};
    enum class ContactMethod : std::uint8_t { Impact, Restitution } contact {};
    double hhtAlpha = -0.2;
    bool reactions = false;             // kinematic run: compute inverse dynamics
    bool removeRedundant = true;        // false: redundant joints become bushings
    int maxNewton = 0;                  // 0: backend default
};

struct BodyState { Id body; Base::Placement pose; Base::Vector3d v, w, a, alpha; };
struct FrameData {
    double time;
    std::vector<BodyState> bodies;
    std::vector<double> channels;       // in the order of the channel table, sec 8.4
};
struct Event { double time; enum class Kind : std::uint8_t {
    SensorTripped, ContactBegin, ContactEnd, LiftOff, StopHit, Warning } kind; Id source; double value; };

class TrajectorySink {                  // frame() kept: kinematic sinks are unchanged
public:
    virtual bool frame(double time, const std::vector<BodyPose>& poses) = 0;
    virtual bool state(const FrameData& f);          // default: frame(time, poses of f)
    virtual void channelTable(const std::vector<std::string>& names) {}
    virtual void event(const Event&) {}
};

struct DynamicsCapabilities {
    bool forward = false, inverse = false, statics = false, gravity = false;
    std::bitset<5> forces;                // per ForceKind
    std::bitset<3> motorControl;          // per MotorControl
    std::bitset<10> contactGeometry;      // per ContactGeometry::Kind
    bool impactContact = false, restitutionContact = false, friction = false;
    bool curveContact = false, stops = false, sensors = false, eventLocalisation = false;
    bool deterministic = false;           // same input, same build: bitwise equal
};
// Capabilities gains:  DynamicsCapabilities dyn;   ('dynamics' becomes dyn.forward)
```

`simulate()` keeps its signature. A `Dynamic` or `Static` spec on a session
loaded with a `Model` that has `dynamics` is a dynamic run. Sensors are
evaluated by the neutral layer from `FrameData` after each accepted internal
step (sec 8.3), which needs `state()` at internal steps and not only at
output times. `FrameData` carries a flag for which kind of step it is.

### 6.3 Who can run what

| | Ondsel | Reference | Chrono |
|---|---|---|---|
| Kinematic (today) | yes | yes | yes, for the kinds it holds; all once queued (a) lands |
| Kinematic + reactions (inverse dynamics) | **possible**: the acceleration solve already carries M and gravity (`OS/AccNewtonRaphson.cpp`, `OS/ConstantGravity.cpp`), and reactions are in `ASMTItemIJ`. It needs `ASMTConstantGravity` and the force items wired in the adapter, and the real `Inertia` (not `Body::mass`) put into the ASMT parts **for this run only**, so static solves keep mass 1 | no (the equations exist; open question 7) | yes: multipliers of the KKT solve at each frame, `GetMotorTorque` |
| Dynamic (forward) | no (no integrator in our fork; Koh's FreeCADMbD has one, sec 1) | no, or an oracle without contact (open question 7) | yes |
| Static equilibrium | no | no | yes: `DoStaticNonlinear` (SMC or no contact) |
| springs, bushings, forces | inverse only | -- | yes |
| motor Speed / Effort | no (Position only) | -- | yes |
| stops | no | -- | yes (`ChLinkLock` limits) |
| contact (all kinds) | no | -- | yes |
| curve contact | no | -- | yes, as a `ChLoad` (sec 7.4) |
| deterministic | yes | yes | yes in our build (no OpenMP, sec 8.3) |

**Refusal** follows SI sec 6.15 exactly. The model loads (`Ok`), and then
`simulate()` answers `Unsupported` with one message that names each element
the backend cannot run and why. For example: "the Ondsel backend cannot run
a Dynamic analysis: no forward integrator", or "SpringDamper001: the Ondsel
backend has no spring element".

The rules:
- An element that changes forces, constraints or motion is never skipped.
  A run that ignored a spring would report a motion the mechanism does not
  have.
- An *output* the backend cannot produce, such as rolling-friction torque
  or a contact pressure, is a warning, and its channel is empty.

**Selection.** The Simulation's `SolverBackend` property chooses the
backend. When it is empty, the choice is the backend selected for the
assembly (SI sec 4.8) if it can run the analysis type, and otherwise the
first registered one that can. So an assembly dragged with Ondsel runs its
dynamics on Chrono without the user changing anything, and the task panel
says which backend runs.

**The start state** is the assembly as solved by its own backend. Chrono
then makes its constraints hold to its tolerance at t0, the step
`ChSystem::DoAssembly` performs. The difference from the kinematic backend's
pose is within the gate's 1e-9 (SI sec 6.15 measured 1.1e-11), so the start
is the same within tolerance.

**Redundancy** before a dynamic run:
1. The neutral rank analysis lists the dependent rows (SI sec 6.13, 6.15).
2. With `RedundantJoints = Remove`, those rows are deactivated in Chrono, in
   OndselSolver's order and not Chrono's QR pivoting, so the choice is the
   same on every backend. The run reports "N equations removed; reactions at
   Joint003 and Joint007 are not unique" (P11), and those reaction channels
   are flagged.
3. With `Bushings`, every joint with a dependent row becomes a
   `ChLinkBushing` of the study's stiffness, as SOLIDWORKS does.

Conflicting joints refuse the run, as they make the start pose infeasible.


## 7. Contact geometry

### 7.1 The ladder

The ladder is applied per body (bundle) and per member solid. Each solid
takes the first rung that applies.

| rung | when | Chrono | accuracy | cost |
|---|---|---|---|---|
| 1 primitive | the solid *is* a sphere, a box, a cylinder, a cone or a capsule: every face analytic and the solid equal to the primitive, checked by the fork's canonical recognition (SI sec 6.11) plus a volume check against `BRepGProp` to 1e-9 relative | `ChCollisionShapeSphere / Box / Cylinder / Cone / Capsule` | exact | lowest |
| 2 convex hull | the solid is convex: the volume of the hull of its contact mesh within 0.1 % of the solid's volume | `ChCollisionShapeConvexHull` from the mesh points | within the deflection | low |
| 3a precise (default for D2) | non-convex and moving | `ChTriangleMeshConnected` + `sweptRadius` (per-triangle sphere-swept proxies) | within the deflection; concave contact correct | high: per triangle |
| 3b fast | non-convex, moving, `Geometry = Fast` | our decomposition (`Hulls`) into `ChCollisionShapeConvexHull`s | within the decomposition's error, which is visible to the user | medium |
| 4 static mesh | the body is ground | `ChCollisionShapeTriangleMesh(is_static = true)` (BVH) | within the deflection | low |
| 5 planar path | a cam or a planar contact on a plane picked by the user, between edges | `ChCollisionShapePath2D` from `Geom2dConvert_ApproxArcsSegments` (OCCT, `TKGeomBase`) at the deflection | arcs are exact for circles; G1 elsewhere | low |
| 6 exact curve | a `CamContact` with lift-off | `CurveContact`, sec 7.4 | exact to the B-rep curve | low |

Only solids in some `ContactSet`, or the cams of a `CamContact`, get
geometry. Nothing is built for parts that cannot touch, which is SOLIDWORKS's
"the more contact pairs, the slower" turned into a default.

Chrono's silent fallback is never reached. A non-convex, non-static mesh
without connectivity would be decomposed by HACDv2 into 512 hulls. Every
mesh we hand it is either connected (3a), static (4) or one of our hulls
(3b).

**Decomposition library for 3b.** The candidates:
- Chrono's HACDv2 (already linked);
- V-HACD 4, reported as BSD-3-Clause
  (https://awesome.ecosyste.ms/projects/github.com%2Fkmammou%2Fv-hacd);
- CoACD, collision-aware and reported as MIT
  (https://github.com/sarahweiii/CoACD), which keeps the clearance between
  concave features that mechanisms rely on.

Start with HACDv2, which needs no new dependency. Then measure against
CoACD on the D2 corpus before adding one (open question 9). Verify each
licence file before linking.

### 7.2 Tessellation and the caches

- **Contact needs a mesh of its own.** The display mesh is deflected for
  the eye, as a fraction of the bounding box (`docs/TShapeRenderCache.md`
  sec 3). Contact needs a chord error below the penetration it resolves:
  Adams suggests dmax ~ 0.01 mm, and RecurDyn exposes a "plane tolerance
  factor" for the same reason.
- **The default deflection** is the Simulation's `ContactTolerance`:
  0.01 mm, or 1e-4 of the body's bounding-box diagonal, whichever is
  larger, with an angular deflection of 5 deg. It is reported per body as
  "penetration error up to x mm".
- **The cache.** An App-side `ContactGeometryCache` is keyed like
  `InstGeomKey` (`docs/TShapeRenderCache.md` sec 3):
  - the key is (TShape, orientation, quantized deflection) per leaf solid,
    in the leaf's own frame;
  - the value is the rung's geometry with its content hash.
  
  So a fastener used 200 times is meshed once, and the 200 bodies
  reference one `ContactGeometry` by id. The model then carries it once
  across a process boundary (sec 4.7 of SI: the hash lets a remote session
  skip what it already has).
- **Meshing must not disturb the display mesh.** It runs on a copy
  (`BRepBuilderAPI_Copy`), or as a second triangulation, since OCCT keeps a
  list of them per face and the active one is unchanged. It is never done by
  re-meshing the shared TShape in place. The second triangulation is
  preferred, as it is free memory-wise when the deflections agree.
- *As built (2026-10-27, `docs/AssemblyInterference.md`):*
  `App/ContactGeometry.{h,cpp}`. `ContactGeometryCache` is keyed by
  (TShape, orientation, deflection asked), one `ContactMesh` each: the mesh
  of a copy, its `BRepExtrema_TriangleSet` and the solid's classifier, in
  the solid's own frame; entries whose solid nobody else holds are dropped
  as it grows. The interference check, its frames and a drag's collision
  watch use it; a pose moves an instance, never the mesh. No content hash
  yet, no worker, no second triangulation (always the copy).
- **Work placement.** The cache is computed in a worker; meshing and
  convexity tests are OCCT work. It is a natural tenant of the
  out-of-process OCCT workers in `docs/ComputeBoundaries.md`. Until those
  exist it runs on a thread pool at Calculate, with a progress bar.

### 7.3 Interference as a by-product

The same shapes give P14 (SOLIDWORKS's Find Interferences Over Time) at
almost no cost.

1. For each stored frame (or each N-th frame), set the poses in a Chrono
   collision system that holds only these shapes, with no dynamics.
2. Run its collision detection with a `NarrowphaseCallback` that records
   each pair and its depth and returns false, so no contact is made.
3. Report frame, time, pair and maximum depth.
4. Confirm the flagged pairs exactly on the B-rep. Use `Shape.common`, or
   the fork's `Shape.diffMaterial` for the volume, and `distToShape` for
   clearance. These are audit phase 5's narrow phase, run only where the
   faceted check found something.

This works after a kinematic run too, which makes it the cheapest useful
D2 deliverable. Audit phase 5's static check is the one-frame case.

*As built (2026-10-27, `docs/AssemblyInterference.md`):* without Chrono.
`findInterferences(frames=...)` and the Check Interference panel's "Over
the simulation's frames" pose the components from the stored trajectory,
leaving the document alone, and run the static check's own narrow phase at
each frame: Exact (booleans) or Fast (the meshes of step 1 here, one per
TShape, moved per frame, crossings confirmed by exact point classification).
A pair neither of whose components moved is not checked again. Depth is not
reported; Exact gives the volume.

### 7.4 Cams

| user intent | object | neutral | Chrono | Ondsel / Reference |
|---|---|---|---|---|
| follower always on the cam (kinematic, or dynamic with a bilateral reaction) | `CamContact.LiftOff = false` | `JointKind::PointOnCurve` (queued (b)), the follower's centre on the cam curve offset by the roller radius | the queued neutral link (a) | (b) gives all three |
| follower may lift off and impact (dynamic) | `CamContact.LiftOff = true` | `CurveContact` | a `ChLoadCustomMultiple` / `ChLoadBodyBody`, its force from the exact curve | refused |
| planar cam between two profiles (a Geneva wheel, a slot) | `ContactSet` on edges, plane picked | `ContactGeometry::Path2D` on both | `ChCollisionShapePath2D` pair | refused |

**`CurveContact`.** Each step, the follower point's distance to the cam
curve is found by the neutral layer's projector, the same one that holds the
PointOnCurve joint. It runs on the B-spline the builder took from the B-rep
edge with `GeomConvert`, as a Newton iteration warm-started from the
previous parameter.
- The gap is g = distance - followerRadius.
- When g < 0, the normal force is the impact law of sec 8.2:
  k (-g)^e + STEP(-g, 0, 0, dmax, cmax) (-g)'.
- Friction is along the tangent.
- The force acts on both bodies at the contact point.

This gives a smooth lift curve where faceted contact would give noise in
accelerations. It also gives lift-off and the bounce back, which is exactly
what SOLIDWORKS's intermittent curve contact and Creo's "Enable liftoff"
offer. A follower that is itself a curve (curve on curve, Adams CVCV) is a
later extension: two projectors and a closest-pair search.

*As built (SI sec 6.18), the projector this section reuses:*
`App/Solver/Curve.h`. It runs on the exact OCCT curve behind the neutral
`CurveGeometry` (its own D2), not on a `GeomConvert` B-spline; the B-spline
is only the curve's description in dumps. `projectOnCurve` is the
warm-started Newton (from the last parameter, never jumping across the
curve, a closed curve's turns counted); `pointOnCurveRows(curve, offset, x,
from)` gives e . N2 - offset, which with offset 0 and the sign of the side
is the signed distance, so g = distance - followerRadius is its second row
with offset = followerRadius, with its exact gradient. The curve is a
`Model::curves` record (not a `ContactGeometry` yet), shared by reference.

*As built (D2):* the lift-off cam is a `CamContact` set on the Cam joint,
not a `LiftOff` flag on it -- a set's law, stiffness, damping and friction,
apply as they do to any contact -- and its force is the set's ContactLoad
with one point from the projector at each collision detection, not a
custom link: the step rules, the patch's law and the retakes are the
contact's. The persistent joint holds the start (sec D2's "Built" entry).


## 8. Solver settings, units, determinism, results

### 8.1 Units

The neutral model carries FreeCAD's internal units, which are consistent
(force = mass x length / time^2). The Chrono adapter converts to SI at load
and back on output. Chrono's defaults, collision envelope and margin,
penetration recovery speed, and the examples of its documentation all
assume SI-sized numbers. We set every one of them explicitly anyway, in SI.

| quantity | internal / neutral | Chrono (SI) | factor |
|---|---|---|---|
| length | mm | m | 1e-3 |
| mass | kg | kg | 1 |
| time | s | s | 1 |
| force | mN (kg mm / s^2; `Base::Unit::Force`) | N | 1e-3 |
| torque | mN mm | N m | 1e-6 |
| linear stiffness | mN / mm | N / m | 1 |
| torsional stiffness | mN mm / rad | N m / rad | 1e-6 |
| linear damping | mN s / mm | N s / m | 1 |
| inertia | kg mm^2 | kg m^2 | 1e-6 |
| density | kg / mm^3 | kg / m^3 | 1e9 |
| Young's modulus | kg / (mm s^2) = kPa | Pa | 1e3 |
| gravity | mm / s^2 | m / s^2 | 1e-3 |
| angle | rad | rad | 1 |

The impact stiffness k with exponent e has units of force / length^e. It
scales by 1e-3 x 1e3^e. This is the pitfall Adams warns about ("stiffness
must be unit-scaled by (length factor)^exponent"), and the adapter has one
tested function for it.

### 8.2 Solver settings exposed, and their mapping

These are the user-facing settings, the few that sec 2.4 found users
actually touch.

| setting | default | Chrono |
|---|---|---|
| start, end, output step (existing a, b, c) | 0, 1 s, 0.01 s | output sampling |
| `MaxStep` | the output step (Adams' HMAX) | the fixed step h; output steps are integer multiples of h, so h is rounded down to divide the output step |
| `Accuracy` (existing `GlobalErrorTolerance`, upstream's `fGlobalErrorTolerance`, renamed only in the UI) | 1e-6 | Newton relative tolerance; absolute tolerances from it times the characteristic length (Creo's rule: the mean bounding-box diagonal) |
| `Integrator` | Auto | Auto: HHT (alpha -0.2) with a direct sparse solver when the contact method is Impact or there is no contact; `EULER_IMPLICIT_LINEARIZED` with the ADMM VI solver (SparseQR inside) for Restitution |
| `ContactMethod` | Impact | `ChSystemSMC` + our impact algorithm / `ChSystemNSC` |

Advanced settings, hidden by default:
- the HHT alpha;
- the maximum Newton iterations;
- the minimum step under step control;
- the contact envelope and margin;
- NSC's minimum bounce speed and maximum recovery speed;
- the bushing stiffness used for `RedundantJoints = Bushings`.

**Accuracy is checked, not assumed.** Chrono's implicit steppers adapt the
step on Newton convergence, not on a local truncation error estimate (sec
3). Adams' GSTIFF and Inventor's RK5 do adapt on the error. So "Check
accuracy" is a button that does what the Adams guide advises (P16):
1. Run again at h / 4 and with a 100x tighter `Accuracy`.
2. Report the largest difference per channel against the run's own values.
3. Mark the results "verified to x" or "not converged".

This replaces an error-controlled integrator honestly, and it is the
discipline the benchmarks in sec 10 use.

**Events.** Contact onset is resolved by the step size. With SMC, the
impact law and a step small against the contact duration resolve the
impact; a warning fires when a contact lasts fewer than 10 steps. Sensors
are located to one internal step, and optionally refined by bisection:
- save the state before the step (`ChSystem::StateGather`);
- re-step at h/2 until the crossing is within the time tolerance;
- restore.

This is Adams' SENSOR located to TERROR.

### 8.3 Determinism

Our Chrono build has OpenMP, SIMD and Thrust off, and runs Eigen
single-threaded. Bullet's broadphase depends on insertion order, which the
builder fixes: bodies in model order, shapes in model order.

So the same input on the same build gives bitwise-equal results. A test
asserts it, and `DynamicsCapabilities::deterministic` states it. Across
platforms and compilers, results agree to tolerance, not bitwise. The
results file records the backend, its version, the build hash and the
spec, so a stored result can always be regenerated and compared. That
answers Simulator sec 9's determinism question for this workstream.

If OpenMP is ever enabled for speed (`SetNumThreads`), the result loses
the deterministic flag, and the UI says so.

### 8.4 Results storage

Results are a channel table: one row per output time, one column per
channel. They are recorded at every output time.

Channels always recorded (raw state):
- per body: pose (7), linear and angular velocity (6), linear and angular
  acceleration (6);
- per joint: reaction force and torque on J, in frame I (6);
- per motor: effort (1) and power (1);
- per force element: force (1 or 6) and deflection (1 or 6);
- per contact pair: total normal and friction force and maximum
  penetration (4);
- system: kinetic energy, potential energy (gravity and springs), and the
  work done by motors and dampers, so the energy balance is a plot.

Events go to a separate list: contact begin and end, lift-off, stop hit,
sensor tripped.

**Size.** 100 bodies, 1000 frames, about 30 doubles per body plus the
joints comes to about 30 MB uncompressed, and float64 is kept for accuracy.

**Saved in the .FCStd, efficiently (decided 2026-10-23, open question 5).**
The user wants results in the file when that is cheap, with playback and
exports working from them. The storage rules:
- **Tiers.** `KeepResults` {None, Motion, All}, default **Motion**.
  - Motion keeps what playback and the animation exports need: the pose
    block (7 per body per frame) and the event list. 100 bodies x 1000
    frames is 5.6 MB raw.
  - All adds every channel of the table above.
  - A plot or sensor that needs a channel the file does not hold marks the
    results partial and offers a re-run, rather than guessing.
- **Compression.** Each channel block is stored column-major (one channel
  over time), delta-coded along time, and byte-shuffled before deflate (the
  HDF5 shuffle filter's trick). Smooth trajectories then compress several
  times better than plain deflate does on float64. The figure is to be
  measured on the benchmark set before the default is fixed; the tier
  default drops to None if Motion does not stay small.
  The values are kept bit-exact: delta coding is done on the integer bit
  patterns, not on the values, so decoding is exact.
- **Blob storage.** The archive is one `App::PropertyFileIncluded` blob
  (docs/FileBlobsManager.md). Content-addressed and reference-counted, a
  touch or an undo step never copies it, and saving an unchanged result
  does not rewrite it.
- **Lazy.** Opening a document reads only `index.json`. The blocks are
  read the first time playback, a plot or an export asks for them, so a
  file with long studies opens as fast as one without.
- **Stale.** Results carry the model hash. A change to the mechanism keeps
  the stored results playable but marks them stale in the tree and the
  panel, until the study is re-run.

**Animation and exports from stored results.**
- Playback, the timeline scrubber and the synchronised plots read the pose
  block, with no solver.
- The existing MP4/GIF export (`CommandCreateSimulation.py`) renders from
  the same frames.
- New exports:
  - CSV or a Spreadsheet of any channels;
  - **animated glTF**: the parts' meshes plus one node animation per
    body, for Blender and web viewers;
  - the browser tier's stream (Simulator sec 7), which plays the same
    pose block.
- Export works on stale results too, and says so in the exported file's
  metadata.

**Format.** A zip with:
- `index.json`: schema version, units, channel names, owners (document
  object names), the spec, the hash;
- one `.npy` per channel group.

`numpy.load` reads it directly, so Python post-processing and the tests
need no reader of ours. It is written from C++ with a few dozen lines (the
.npy header is trivial) through the zip writer the fork already has.

The `Trajectory` (poses per output time) stays what playback reads (SI sec
6.6). It is the pose block of the same table, so playback, GIF/MP4 export
and the browser tier's stream (Simulator sec 7) are unchanged consumers.

### 8.5 Measures, plots, export

Every derived quantity is computed after the run from the channel table. A
plot, trace or sensor added later does not re-run the solver (P12). The
measures are:
- the velocity or acceleration of a point on a body (from the body's v, w,
  a, alpha);
- relative motion between two bodies, in any part's frame;
- joint coordinates;
- the separation between two points;
- for a cam: pressure angle and slip velocity, from the curve and the
  contact force.

Measure names are a small grammar: `Body.Part001.Velocity.z`,
`Joint.Joint003.Reaction.Fx`, `Motor.Motion001.Power`, `System.KineticEnergy`.
The same names are the `Sensor` channels and the `ResultPlot` channels.

**Plotting.** The Plot module draws into a docked panel whose time cursor is
synchronised with playback (Inventor's Output Grapher, NX's synchronised
graphs). The panel exports CSV (SOLIDWORKS) and a spreadsheet (the
Spreadsheet workbench, through the same table).

**Traces.** A `TraceCurve` is a `Part::Feature` whose shape is a B-spline
through the stored positions of a point. It is usable as sketch or cam
geometry (Creo's cam synthesis, P13).


## 9. UI / UX sketch

**Toolbar and menu** (the Simulation submenu):
- Create Simulation, which exists;
- Gravity;
- Spring / Damper;
- Bushing;
- Force / Torque;
- Contact Set;
- Cam Contact;
- Mass Properties;
- Initial Condition;
- Sensor;
- Calculate;
- Results and Plots;
- Trace;
- Interference over Time.

The layout follows SOLIDWORKS MotionManager's tool row
(https://help.solidworks.com/2025/English/SolidWorks/motionstudies/r_motionmanager_tools.htm),
and each tool greys out when the active Simulation's analysis type or
backend cannot use it. That is SOLIDWORKS's per-study-type availability,
driven here by `Capabilities`.

**Tree.**

    Assembly
      Joints ...
      Physics                      <- new group, owns every mechanism element
        Mass overrides, SpringDamper001, Bushing001, ContactSet001,
        ContactMaterials, CamContact001, JointDynamics001, Spring002
      Simulations
        Simulation001  [Dynamic]   <- type shown in the label; stale overlay
          Motion001 [Speed]        <- existing motions, control mode shown
          Gravity, Force001, InitialCondition001, Sensor001
          Plot001                  <- the Physics elements it uses are in
                                      its task panel, not here (sec 5.1)

**The Simulation task panel** (`TaskAssemblyCreateSimulation`) gains the
following:
- An "Analysis" combo at the top: Kinematic, Dynamic, Static (P1).
- The backend that will run it, with a refusal message inline when the
  backend cannot (sec 6.3).
- An includes table (P3, Creo's tabs). It lists every Physics element with
  a check box, a New button that creates one into Physics, plus gravity
  on/off and direction. Ticking edits `Elements` (sec 5.1).
- The existing time fields and the motion list. Each motion's row gets a
  Control column.
- Calculate with a progress bar and Cancel. Results are flagged stale
  (P18) when the model changes.
- "Check accuracy" (sec 8.2).
- The existing player. During playback, vectors can be shown on the
  model: reactions, motor torque and contact forces, scaled. Inventor and
  SOLIDWORKS both draw them.

**Mass Properties** is a table of parts: material, density (assumed values
in warning colour), volume, mass, CoM, and an Override check box. The
assembly totals (mass, CoM, inertia) sit at the bottom. That closes the
audit's "mass properties not wired into the assembly" row on its own, which
is why it ships in D0.

**Spring / Damper** is picked like a joint: two elements on two parts. The
free length defaults to the current distance, with a "track model" box
(SOLIDWORKS). An optional coil is drawn between the points, for display
only.

**Contact Set** has two selection lists, Group A and Group B (P5). An empty
B means all pairs within A. A material-pair combo (P6) shows the friction
and elastic fields; editing any field turns the combo to "Custom". The
geometry choice is Auto, Precise or Fast (P8), and the panel shows per part
which rung it got, with its penetration error.

**Results and Plots** is a three-level picker (SOLIDWORKS: category,
sub-category, component), then the entities, then the frame of reference:
- Displacement / Velocity / Acceleration;
- Forces;
- Energy and Power;
- Cam.

The plot docks next to the 3D view, synchronised with the player. The
min/max search and CSV export are in its context menu (Inventor).

**Drag to set the initial pose** (P17, Inventor's Dynamic Part Motion). In
a Dynamic simulation, dragging a part moves it through the kinematic solver
as today. "Record as initial pose" then stores the poses as the
simulation's start. The current behaviour, where the start is the
assembly's own pose, remains the default.


## 10. Testing

**Analytic benchmarks.** Each is a Python test that builds a document, runs
the Simulation through `generateSimulation` and checks channels against a
formula. Tolerances are stated per case and checked at h and h/4.

| case | checks | reference |
|---|---|---|
| free fall | z(t) = -g t^2 / 2 with g = 9806.65 mm/s^2 | **the units test**: off by 1000 if the contract breaks |
| simple pendulum | small-angle period 2 pi sqrt(L/g); large amplitude: period by the complete elliptic integral; energy drift over 10 periods (HHT alpha = 0 for this test, as alpha < 0 dissipates) | IFToMM planar simple pendulum: 1 kg, L = 1 m, 10 s, drift < 5e-5 J (https://www.iftomm-multibody.org/media/problems/planar%20simple%20pendulum/planar%20simple%20pendulum.pdf) |
| spring-mass(-damper) | omega = sqrt(k/m); logarithmic decrement for c | closed form |
| compound pendulum | a box on a revolute: period with the inertia about the pivot | closed form; checks `BRepGProp` inertia and the parallel-axis transform |
| slider-crank, kinematic + reactions | slider position, velocity, acceleration in closed form; crank torque by virtual work for a slider mass; equal on Ondsel (once wired) and Chrono | closed form; IFToMM planar slider-crank (drift < 1e-3 J) for the dynamic variant |
| four-bar | Grashof crank-rocker angles in closed form; IFToMM four-bar with a diagonal spring | IFToMM |
| Andrews' squeezer | a stiff 7-body benchmark | IFToMM reference results (https://www.iftomm-multibody.org/media/problems/andrews'%20squeezer%20mechanism/andrews'%20squeezer%20mechanism.pdf) |
| static equilibrium | a spring-loaded lever's angle | closed form |
| motor modes | Speed: omega = const and the torque that takes against a damper; Effort: alpha = T / J | closed form |
| stops | a pendulum against a limit: rebound speed = e x impact speed (Hard, NSC); Penalty: no penetration beyond F/k | closed form |
| redundancy | a door on two coaxial revolutes: the run reports 5 rows dependent (2 of one hinge's, 3 of the other's, in OndselSolver's order), the motion equals the single-hinge case, and the reactions are flagged not unique | -- |
| cam-follower, persistent | a disk cam whose profile is a cycloidal rise: follower lift = the profile function to the projector tolerance | closed form (the cam built from the function) |
| cam-follower, lift-off | a spring-loaded follower: lift-off at the speed where m a_min = -(k (x + x0) + m g); no lift-off below it | closed form |
| bouncing ball | Restitution (NSC): apex heights h_n = e^(2n) h0; Impact (SMC): the measured coefficient within 5 % of the one the damping was set for | closed form |
| block on an incline | sticks for tan(theta) < mu_s; slides with a = g (sin - mu_d cos) | closed form |
| cylinder rolling down an incline | a = (2/3) g sin(theta) without slip | closed form |
| primitive against mesh | a sphere on a plane by rung 1 and by rung 3a: equal rest height within the deflection | consistency |
| interference over time | the slider-crank with a deliberately long pin: frames and depth listed; the exact confirm agrees | constructed |

**Cross-checks against published results.** The IFToMM library's reference
`results.txt` files give time histories and energy for the planar pendulum,
the slider-crank, the Bricard (redundant), the double four-bar (singular)
and Andrews' squeezer (https://www.iftomm-multibody.org/benchmark/browse/).
Its stiff flyball governor reference was computed with the Adams solver,
which makes it the one Adams-produced published reference we can compare
against. IFToMM has no contact problem, so the contact cases above are our
own analytic references. The Adams tutorial kit's cam-follower and
falling-stone examples
(https://www.uni-ulm.de/fileadmin/website_uni_ulm/uzwr/lehre-seminare/CompBiom_2016/ADAMS-Tutorial.pdf)
can be rebuilt as documents to compare shapes of curves, not digits.

**Gate integration.** This extends the gate of SI secs 6.2, 6.13 and 6.15.
- The kinematic gate is untouched. A kinematic build has no `dynamics`
  block, which a gtest asserts, so `TestSolverGate*` keeps passing
  unchanged.
- New recorded dynamic cases are added: about 15 of the above as
  documents, with a recorded channel table each. Gate rules:
  - the same build is bitwise-equal (determinism);
  - other builds are within a tolerance per channel;
  - a backend without `dyn.forward` gets the `BACKEND_REFUSED` treatment
    of SI sec 6.15, with its reason asserted.
- gtests cover the adapter's pure functions:
  - unit conversion, including k with exponent e;
  - inertia transform and bundle combination;
  - the impact law against Adams' formula;
  - the curve projector;
  - the geometry ladder's classification on primitives, a convex part and
    an L-bracket;
  - the `.npy` writer read back.
- Persistence tests:
  - a 1.1-era file with a kinematic Simulation loads and plays as before;
  - a new file round-trips every new property;
  - a results blob survives save and load, and goes stale on an edit.
- Bench: `scripts/assembly-bench/` gains a gravity chain (N = 49, 99)
  and a contact pile (N = 50 boxes), timed per simulated second.


## 11. Open questions

**Answered by the user (2026-10-23, 2026-10-24):**
- 1: both. Ownership and display are separate, as elsewhere in FreeCAD:
  every element is owned by the Physics group, the one place to find
  it. A study lists what it uses in `Elements`, shown in its task panel
  rather than the tree; its task panel creates an element
  (always into Physics) or picks existing ones. No private elements.
  Sec 5.1.
- 5: results are saved in the .FCStd, provided it is efficient, and
  animation and exports work from them. The storage design is in sec 8.4.
- 6: no OndselSolver dynamics. All dynamics, inverse dynamics included,
  goes to Chrono, and OndselSolver stays kinematic.
- 9: HACDv2, the decomposition already in Chrono. No new dependency.
- 11: a preference sets the default material for parts without one; its
  default is aluminium (2700 kg/m^3). The Mass Properties table marks
  every part that uses it as assumed.
- 12: gravity is on by default for new Dynamic and Static simulations.
- 13: interference moves forward into D2, sharing D2's contact cache.

**Answered by the user (2026-10-24): the proposals for 2-4, 7, 8 and 10
stand.**
- 2: Impact (Chrono SMC, direct solver) is the default contact method;
  Restitution (NSC) is the option for piles and drop tests.
- 3: SI inside Chrono, converted in the adapter; the neutral model stays
  in FreeCAD's internal units.
- 4: D1 in process on a worker thread with cancel; D3 (out of process)
  before D2 (sec 4).
- 7: the Reference backend gets a forward-dynamics oracle at the end of
  D1, as the no-contact benchmarks' second implementation.
- 8: in two steps. First the motor's own joint `angle` and `rate` as
  names beside `time`; any measure later. Tables are a `MotionTable`
  object, not grammar.
- 10: D4a (loads to FEM) first; modal reduction (`chrono_modal`) later;
  direct FEA bodies only for a specific need.

The questions as they were put:

2. **The default contact method.**
   - Proposed: Impact (Chrono SMC, an Adams-style law, direct solver), as
     accuracy first.
   - Restitution (NSC) would be faster for piles and drop tests, but it
     makes the joints only as accurate as an iterative solver.
3. **Units inside Chrono.**
   - Proposed: SI in the adapter, with the neutral model in FreeCAD's
     internal mm-kg-s-mN.
   - Alternative: run Chrono directly in mm-kg-s. That saves the conversion
     but leaves every Chrono default and tolerance scaled wrongly, and the
     mass matrix less well balanced.
4. **In process or out of process for dynamics.**
   - Simulator sec 8 stage 2 says out of process. The Chrono backend is in
     process today (SI sec 6.15).
   - Proposed: D1 in process (on a worker thread, with cancel), and the
     RemoteBackend in D3, before D2's meshes reach users. Should D3 instead
     come before D2?
5. **Results in the .FCStd.** Proposed: opt-in (`KeepResults`, default
   off), with recompute on open marked stale.
6. **OndselSolver inverse dynamics.** Wire `ConstantGravity`, the force
   items and reactions into the Ondsel adapter, giving reactions in
   kinematic runs without Chrono? Or leave dynamics of every kind to
   Chrono and keep Ondsel kinematic?
7. **A forward-dynamics oracle in the Reference backend?** It would be a
   dense M qdd + Cq' lambda = Q solve with RK4 and projection, about 500
   lines on the existing residuals. It would give the no-contact
   benchmarks an independent second implementation, as the Reference does
   for kinematics. Or are analytic and IFToMM references enough?
8. **The formula grammar.** Torque and force formulas often depend on state:
   a motor curve T = T0 (1 - w / w0), or a damping-like law.
   - Allow the motor's own joint `angle` and `rate` as names beside `time`?
     Creo and SOLIDWORKS allow any measure.
   - Add a table form (Akima, cubic, linear) as a `MotionTable` object
     rather than in the grammar? Sec 4.6.1 of SI refuses `spline` because
     it needs a data form.
9. **The convex decomposition library.** HACDv2 (already in Chrono),
   V-HACD 4 (BSD-3) or CoACD (MIT, collision-aware). That would be a new
   third-party dependency, so it is the user's call after a measured
   comparison.
10. **Flexible bodies.** The choices:
    - D4a loads-to-FEM only;
    - direct FEA bodies (Chrono `fea/`, in core);
    - modal reduction (`chrono_modal`, which needs Spectra, and is the
      industry's usual route).

    Or none, leaving it to the MbDFEM / Motion-workbench efforts upstream
    (sec 2.6)?
11. **Parts without a material.** Use a default density (proposed: 7850
    kg/m^3, with a warning in the panel), or refuse the Dynamic run until
    every part has one?
12. **Gravity's default.** On for new Dynamic and Static simulations
    (proposed), or off as in Inventor and Adams?
13. **Interference ordering.** Audit decision 5 puts interference last. Sec
    7.3 makes interference over time nearly free once D2's cache exists,
    and the static check of audit phase 5 is its one-frame case. Keep the
    decision, or let the shared cache bring phase 5 forward to D2?
