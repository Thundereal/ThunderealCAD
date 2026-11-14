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
  - Not yet: the decomposition (rung 3b) and a ground mesh's BVH (rung 4).
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
  - Not yet: a follower that is itself a curve (Adams CVCV), a cam's
    pressure angle and slip in the results (built since: "Built: a cam's
    pressure angle and slip" below). (A planar cam between two profiles
    is the profile contact, below.)
- Built: sensors that stop a run or mark the time (sec 5.2's `Sensor`,
  P15: Adams' SENSOR, CATIA's stop on collision, NX's Stop_On_Event).
  - The document: `Assembly::Sensor` (SensorPython), a child of a
    Dynamic or Static study as a motion and an initial velocity are
    (Python `CommandCreateSimulation.newSensor(sim, source, quantity,
    threshold, component, compare, action)`; the panel's gauge button and
    `SensorDialog`, which a double click on it in the list opens again).
    `Source` is what reports the value -- a joint (its reaction, or the
    effort of the motion driving it; the motion itself maps to its
    joint), a spring, a torsion spring, a bushing or a contact set (a cam
    contact too) -- and `Quantity` one of the run's channels (sec 8.4:
    Force, Torque, MotorTorque, MotorForce, SpringForce, SpringLength,
    SpringTorque, SpringAngle, BushingForce, BushingTorque, ContactForce;
    and a contact set's events, ContactBegin and ContactEnd, by their
    speed: "Built: sensors on contact events" below);
    `Component` a vector's Magnitude, X, Y or Z in the assembly's axes;
    `Compare` Above or Below; `Threshold` a quantity in the quantity's
    unit (`Sensor::updateUnit()`: force, moment, length or angle);
    `Action` Stop or Mark. Its label says what it watches and where:
    "Spring: Spring length above 5.5 mm". A Kinematic study warns and
    ignores it. Not built: sec 8.5's measure grammar (a point's velocity,
    a joint's coordinate), which the measures bring; Crosses, which Above
    and Below make between them.
  - The model: `Solver::Sensor` in `Dynamics::sensors`, in the model's
    units, as the run reports values (`scaled()` leaves it; an angle's
    degrees to radians in the builder), compared by `same()`, so a sensor
    changed marks the results stale. Its watched source matches a
    channel by name and quantity; one that matches none warns and does
    nothing.
  - The run: after each step, valuesNow() read, a sensor trips where its
    value is past the threshold and was not at the last reading -- one
    already past at the start trips when it comes back and passes again.
    The trip is located by bisection to max(hMin, 1e-6 of the output
    step), each try a step from the state before it taken back whole: the
    system's state and accelerations, the stops held (their links added
    or removed), the limits' turn counts, the contacts' bookkeeping
    (impact speed, rest), the cams' curve parameters, then collision
    detection and the neutral links' angles committed again. There it is
    an event (Backend.h's `Event`, through `TrajectorySink::event()`; the
    two wrapping sinks, CentredSession's and GroupedSession's, pass it on,
    and `GroupedSession` keeps the sensors when it reduces the model): a
    stop ends the run with its last frame at that time, a mark takes the
    rest of the step. With contact the watched steps are the contact's
    substeps (`contactStep()`): an impact is over in tens of
    microseconds, and a ball landing between two millisecond steps went by
    unseen, its contact force's sensor tripping at a later bounce. A
    Static study's sensors are read at its output times, from the first.
  - Results: `getSimulationResults()["Events"]`, each with Time, Object,
    Name, Value (in the quantity's unit as the channels are) and Stopped;
    the panel lists them under the player, "Stopped at 0.0397646 s by
    Spring: Spring length above 5.5 mm".
  - Tests (TestDynamics): the oscillator's spring length rising above its
    free length stops the run a quarter period in, 0.0258109306 s against
    0.0258108171 (1e-5), the length there 4.00000006 mm and the last frame
    there; marking, above and below, the run goes to its end with an event
    every half period (1e-4 of it); a sensor on a quantity its source does
    not report does nothing. TestContact: a ball dropped a millimetre stops
    the run on landing at sqrt(2 h / g) (1e-6 s), just touching.
    TestGuiCommands: the dialog's sources (the joint, not its grounding),
    its quantities per source, the component idle for a single value, the
    threshold in the unit the results show, written and read back; the
    panel's lines for a mark and a stop.
- Built: stops per joint, rigid, penalty or off (P10: Inventor's bounds,
  Creo's range limits, Simscape's hard stop). Sec 5.2 planned this as
  `JointDynamics`, with the joint's friction; that is two objects as built,
  as the torsion spring is its own, and the friction is its own too (below).
  - The document: `Assembly::JointStop` (JointStopPython), a Physics item
    ticked per study (`Assembly_CreateJointStop`, on the one Slider,
    Cylindrical or Revolute joint selected; `CommandCreateSpring.
    makeJointStop()`). `Joint`; `Mode` {Rigid, Penalty, Off}; Rigid's
    `Restitution` (0 to 1), over the study's for that joint; Penalty's
    `Stiffness` and `Damping` for a length limit, `TorsionalStiffness` and
    `TorsionalDamping` for an angle limit (defaults 1e6 N/m, 1e3 N s/m,
    1e3 N m/rad, 1 N m s/rad); only the fields its mode and its joint's
    kind use are shown. A joint without one stops rigidly at the study's
    restitution, as before; a second stop on a joint warns and does
    nothing, as does one on a joint without limits in the run. A kinematic
    study holds the limits as it always did.
  - Limits in a study with motions: upstream's builder drops every limit
    when a study moves anything, so they cannot clash with its motions. A
    Dynamic or Static study now keeps them, but on the joints a Position or
    Speed motion holds: an Effort pushes, and an arm driven onto its stop
    is what a stop is for.
  - The model: `Solver::Stop` in `Dynamics::stops`, one per limit (the
    limit's index, its mode, the restitution -- negative: the study's --
    and k, c as the limit's kind is); `scaled()` scales an angle's k and c
    as a torque, compared by `same()`, kept by `GroupedSession`.
  - The run: a rigid stop is the one the D1 leftovers built, at its own
    restitution; when one step hits several, the projection's impulse
    bounces at the least of theirs, so none gains energy. A penalty stop is
    a `StopLoad`, Chrono's body-body load between the limit's frames: past
    its bound by d, max(0, k d + c dd/dt) along zI for a slide, about it
    for a turn -- never pulling -- on frame I's body, and its opposite on
    J's, in the joint's reaction channels. Implicit through Chrono's
    numerical jacobian; the turn counted from the one last committed. The
    steps are cut to resolve it, as a contact's are: a fiftieth of the
    period of the lighter body on the spring (its mass, or for a turn its
    inertia about the joint's axis), and 2 m / c, once it is past the bound
    or will be within a period; half the time to get there before. Off:
    the limit is not stopped.
  - Tests (TestDynamics), the dropped block on a 1000 N/m stop: at rest
    m g / k in, 0.0264779550 mm against 0.026477955 (dynamic; 2e-8 mm off
    static), the slider carrying m g (4e-16; 8e-7 static); undamped, it goes
    in 0.487093 mm against m g (h + d) = k d^2 / 2's 0.487482 (the output
    step's sampling, 1e-4 s) and comes back to 3.9983 of its 4 mm; a rigid
    stop's restitution 1 over the study's 0 bounces it back 4.011 mm; Off,
    it falls through as if free (2e-13 mm); an Effort of 1 N pushing it onto
    a penalty stop settles it (m g + 1 N) / k in (1.4e-8 mm), the Effort's
    own joint's limit kept. The pendulum on a 10 N m/rad stop at 30 deg
    settles where k (a - 30 deg) = m g L cos a (1e-4 deg). TestGuiCommands:
    the command idle until such a joint is selected, the stop made Penalty,
    ticked in the Dynamic study only, the fields shown per mode, one undo.
- Built: contact plots (sec 8.4's per pair channels, per set as built).
  - Each contact set reports, besides `ContactForce` (its force on group
    A's bodies), `ContactNormalForce` and `ContactFrictionForce` -- that
    force's part along its points' normals and the rest, so the two add up
    to it -- and `ContactPenetration`, its deepest point's depth, 0 apart,
    in the length unit. The ContactLoad keeps them from the last
    `ComputeQ()` at the state, as it kept the force (`normalOnB`,
    `deepest`); a set's are summed, and the deepest taken, over its loads.
    The panel's Results box plots any of them and the CSV and spreadsheet
    exports carry them; a sensor watches any of them (a penetration's
    threshold a length).
  - Tests (TestContact): the ball at rest pressed by its weight and sunk
    by the law's closed form (1e-5 mm), no friction, the normal and
    friction parts adding to the force at every frame (1e-9), nothing
    before it lands; the block sliding along the plate has the weight for
    its normal force (1e-3) and a quarter of it, the dynamic friction,
    against its slide. A sliding block's deepest point is its leading
    edge, which friction's moment presses: 0.0223 mm against 0.020 at rest
    in the manual's video (allowed penetration 0.02 mm).
  - Not yet: a pair's own channels within a set. Contact begin and end
    as events: built (below).
- Built: profile contact (sec 7.4's 2D path, rung 5), for mechanisms that
  move in a plane: a Geneva wheel and its driver, a pin in a slot.
  - The document: `Assembly::ProfileContact`, a `ContactSet` (Physics;
    `Assembly_CreateProfileContact` on planar faces of two components,
    `CommandCreateContact.newProfileContact(assembly, a, facesA, b,
    facesB)`, ticked in the Dynamic and Static studies) whose `ProfileA`
    and `ProfileB` are a component's planar faces each (`PropertyLinkSub`,
    any number); GroupA, GroupB and RadiusFrom hidden; its law a set's, by
    default an allowed penetration with no friction (Materials runs as
    Penetration: a profile has no width for Hertz's law). The faces stand
    for their parts' sections in the plane, at whatever height they are.
  - Not `ChCollisionShapePath2D`: Chrono's 2D algorithms take one point a
    pair, ignore a segment's ends (a corner on a flat is not seen), never
    let a hollow arc meet a segment, and lean on Bullet's margins.
  - The model: `ContactShape::Kind::Profile` on a frame in the face's
    plane, its `pieces` (`ProfilePiece`: a segment, or an arc by its centre
    and signed sweep) in `loops`, the face to their left; `size.x` and
    `size.y` how far the face's solid reaches below and above the plane.
    `profileOf()` (ContactShapes.cpp) reads each wire's edges in order
    through their pcurves on the plane (`BRepTools_WireExplorer`), lines
    and circles as they are, any other curve by
    `Geom2dConvert_ApproxArcsSegments` within 1e-3 mm, and turns a loop
    round when a point just left of its longest piece is not in the face
    (`BRepClass_FaceClassifier`). Faces not parallel where the run starts
    are refused.
  - The run: no Bullet shape. A ContactLoad per pair of bodies, its
    points found at each collision detection (`ContactHandover::
    ProfileFollow`, as a cam's): B's profile brought into A's plane (its
    loops turned round once when its plane faces A's), and
    `profileTouches()` finds where each goes deepest into the other, or
    within Bullet's envelope of it -- each convex corner of either, an
    arc's point farthest across a segment of the other, or along the line
    of the centres from an arc of the other -- the other's nearest
    boundary point to it, with an outward normal (a corner's between its
    pieces') telling in from out. Exact on lines and arcs. Each point is at
    the middle of where the two solids overlap through the plane, so that
    a free part is pushed through its centre's height and not tipped out of
    its plane (applied at the faces' height, a disc resting on an edge was
    turned over by its own weight). A point's patch keys are its pieces
    (`Point::piecesA`, `piecesB`; a corner's two): a flat lying on a flat,
    touching at both corners, is one patch, and a pin and a locking disc
    on one wheel are two.
  - Tests (TestProfileContact): a disc dropped a millimetre onto a plate's
    edge, gravity in the plane, rests sunk by the allowance (1e-6 mm), its
    weight on the plate (1e-5); a block dropped flat rests level on both
    corners (1e-6 rad) sunk by the allowance -- two patches would sink it
    2^(-2/3) of it -- the plate's profile its bottom face, facing the
    block's the other way; a disc dropped into a round notch wider than it
    settles at the notch's bottom (1e-4 mm); a four-slot Geneva drive, the
    driver's pin and its locking disc on the wheel, the driver turning
    once a second: the pin's centre on the slot's axis within the slot's
    clearance while it is in it, the wheel a quarter turn round after
    (0.2 deg), held there. Its run of a second takes 1.7 s; the wheel
    follows the ideal motion within 0.2 deg, picking up 10 ms late across
    the slot's clearance, and the locking disc holds it within 0.1 deg.
    TestGuiCommands: the command idle until planar faces of two components
    are selected, then the contact on them, ticked in the Dynamic study
    only, one undo.
  - Not yet: a face's part through its own thickness (the faces' solids
    give the height only); faces not parallel to each other.
- Built: joint friction (sec 2.2's "a friction torque from the reaction and
  a pin radius"; Inventor's coefficient and radius, Adams' FRICTION).
  - The document: `Assembly::JointFriction` (JointFrictionPython), a
    Physics item (`Assembly_CreateJointFriction` on the one Slider,
    Cylindrical or Revolute joint selected; `CommandCreateSpring.
    makeJointFriction()`), ticked in the Dynamic studies only: a Static
    study's joints slip at no speed. `Joint`; `StaticFriction`,
    `DynamicFriction`, `StictionVelocity`, `FrictionVelocity` (a contact
    set's defaults, 0.3, 0.25, 0.1 mm/s, 10.16 mm/s); a turn's `PinRadius`
    (5 mm) and `PreloadTorque`, a slide's `PreloadForce`, each shown for
    the joint's kind only.
  - The model: `Solver::Friction` in `Dynamics::frictions`, on the joint's
    frames (which keep their ids through the reductions, where the
    joints' records do not), whether it slides and turns (Prismatic,
    Revolute, Cylindrical), the law, the radius and the preloads;
    `scaled()` scales the speeds and the radius as lengths, the preloads as
    a force and a torque. One per joint: a second warns and does nothing.
  - The run: a `FrictionLoad`, Chrono's body-body load between the
    joint's frames, on frame I's body and its opposite on J's. The slip is
    J's from I's, along zI and about it at the radius, s their length; the
    coefficient by s, Adams' STEP through no slip from the other way's
    (steepest at rest) up to the static at the stiction velocity, then to
    the dynamic at the friction velocity; along the axis
    mu (N + preload force) slide / s, about it mu (radius N + preload
    torque) turn / s. N is the joint's reaction across its axis: its
    links' force on frame I's body less its part along the axis, taken
    after each step (and at the start's assembly) and held over the next
    -- the constraints' reaction, which the step solves with it. Implicit
    through the numerical jacobian. Its value is the joint's reaction's
    too, and its own channels: `FrictionForce` along the axis and
    `FrictionTorque` about it, which a sensor can watch.
  - The steps: between the stiction and the friction velocities the
    friction is flat or falls as the slip grows, and Newton's iterations
    ran off there -- a block stopping on its rail at 1 ms steps crept on
    at 2 mm/s, its friction at the static's; at 50 us it stopped dead. So
    while a joint slips in that band the substep is half the time its
    friction takes to change the slip by the stiction velocity, the
    lighter body moving (for a turn its inertia about the axis over the
    radius squared); faster, half the time to come down to the band.
    Stuck (below half the stiction velocity) or sliding fast, the run's
    steps stand. The contact law's STEP from no slip, flat at rest, was
    the first try and changed nothing: the band was the trouble.
  - Tests (TestJointFriction): a block thrown along its rail at 200 mm/s,
    its weight across the slider, slows at mu_d g (1e-3 mm, the friction
    mu_d m g to 1e-4) and stops at v0^2 / (2 mu_d g) (0.2 mm; 8.1566
    against 8.157), dead; on a rail tilted 30 deg, steeper than mu_d, it
    slides at g (sin a - mu_d cos a), at 10 deg the friction holds it
    (0.01 mm in 0.1 s); a wheel spinning on its axle, its weight across
    it, slows at mu_d R m g / I, the friction torque mu_d R m g (1e-3); a
    preload torque alone, with no gravity, slows it at mu_d T / I. The
    four run in a second. In the manual's video three blocks thrown at
    300 mm/s with 0.1, 0.25 and 0.5 stop at 45.8873, 18.3549 and 9.1775 mm
    against 45.8872, 18.3549 and 9.1774. TestGuiCommands: the command idle
    until such a joint is selected, the friction ticked in the Dynamic
    study and neither the Static nor the Kinematic one, a slider's
    preload force shown and a turn's fields not, one undo.
  - Not yet: the reaction's bending moment and the axial force as loads
    (Adams' friction arm and bearing radius); a Kinematic run's inverse
    dynamics, whose motor efforts leave the friction out; a Ball joint's.
- Built: contact events (sec 8.4's "contact begin and end"; a cam's
  lift-off is its contact's end): two bodies of a contact set starting to
  press on each other and letting go, each pair of bodies on its own --
  a set's load per pair (primitives, hulls and meshes, a cam, a profile).
  Nothing to set: every dynamic run reports them.
  - The model: `Event::Kind` {Sensor, ContactBegin, ContactEnd}
    (`Backend.h`), the set as the source, `bodyA` and `bodyB` the pair's
    bodies by their source names (the set's group A side first, a cam's
    cam; "" the ground), `value` the speed the pair's deepest point comes
    in at or leaves at, mm/s. A body standing for a rigid group (sec 6.1's
    grouping) is named by the group's first part. (Since: the part that
    touched, "Built: contact events name the parts that touched" below.)
  - The run (`advance()`, ChronoBackend): a load touches while its deepest
    point presses (depth above none), read at the end of each substep
    kept on the points found there, and from the run's start: what
    presses at the start was never made, and makes no event. A change is
    located within its substep where that point's depth crosses none, on
    the line between the substep's two ends, both read on the points found
    at its start (a change those points do not see: at the end, or at the
    start). The speed comes from the free side, where the bodies are
    apart: coming in, the start's speed and acceleration carried forward
    to the crossing; a line between the ends took in the stiff contact's
    braking, a ball landing at 139.99 mm/s for 140.05. Leaving, read
    again a substep later and carried back -- HHT's accelerations at a
    substep's end mix in the force at its start (-11.7 m/s^2 for g's -9.8
    where the ball left), and its velocity settles in the next substep:
    read where it left, 0.06% to 0.28% slower than its flight took, a
    substep on 1e-6 of the speed it comes back in at. Static runs report
    none.
  - The events queue (`sendEvents()`): a sensor's and the contacts' wait
    for their step to be kept -- a sensor's bisection takes its tries'
    back with the rest of the state -- and go out in time order before
    the frame that follows; a contact let go waits for its second reading,
    holding back what comes after it to the next frame (the run's last
    sends everything).
  - Results: `getSimulationResults()["Events"]` entries gain `Kind`
    ("Sensor", "ContactBegin", "ContactEnd") and, a contact's, `Bodies`
    (the two components, None the ground); `Value` its speed. The panel's
    lines under the player: "Contact at 0.0142809 s: Ball and Plate, in at
    140.05 mm/s (Contact)", "Let go at ...: ..., out at ...".
  - Tests (TestContact): a ball dropped a millimetre bouncing at
    restitution 0.8 -- the events alternating, made and let go, each the
    set's and the pair's, the ball first; made first at sqrt(2 h / g)
    (1e-6 s) coming in at sqrt(2 g h) (1e-5); each let go at r times its
    speed in (2%) and made again 2 v / g later (1e-6 s), coming in at the
    speed it left at (1e-4); a sensor marking the force's rise among them,
    in time order, with the first. A box resting from the start, sunk by
    the allowed penetration: no event. The sensor stopping on landing has
    the contact's event before its own. TestGuiCommands: the panel's
    lines for a contact made and let go between a sensor's mark and stop.
  - Not yet: which parts of a rigid group touched (the pair is the
    group's bodies). (The events are kept in the document with the
    results since: "Built: results kept in the document" below; the
    parts: built, below.)
- Built: sensors on contact events (P15's Stop_On_Event, on sec 8.4's
  contact begin and end): a sensor on a contact set whose `Quantity` is
  `ContactBegin` or `ContactEnd` trips at each contact made, or let go, by
  any pair of the set, coming in or leaving at a speed at or above its
  `Threshold` (a velocity; 0, every one) or, `Compare` Below, under it.
  Stop on the first landing slower than 45 mm/s is a drop test run until
  its bouncing is about over; stop on the first let go is a cam's first
  lift-off.
  - The document: `Sensor::QuantityEnums` gains the two after the
    channels', `updateUnit()` makes the threshold a velocity. The label
    says which and the speed when there is one: "Ball on plate: Contact
    made below 45 mm/s", "...: Contact let go", "...: Contact made at 50
    mm/s or more". The dialog offers them after a contact set's values,
    the component idle, the compare worded "Speed at least" and "Speed
    below".
  - The run (ChronoBackend): `eventWatches`, the sensors on a set's events
    (one on a set the run has none of, or in a Static run, warns and does
    nothing). Each event queued is seen once, as the step that found it is
    kept (`Queued::seen`, in the bisection's snapshot with the queue): a
    sensor that marks adds its event at the contact's time and speed; one
    that stops is located as a value's trip is -- the step bisected to
    max(hMin, 1e-6 of the output step), each try asking whether an event
    not yet seen trips a stopping sensor -- and the run's last frame is
    there. A contact let go matches a stopping sensor on its first reading
    of its speed; its speed is then read again a step on and the state
    taken back (`settleReadings()`), as is a let go pending at any stop
    and at the run's end, which before went out on its first reading.
  - The panel's lines under the player: the sensors' first, the contacts'
    from the start in the lines left, so a stop late in a run of bounces
    shows: "Marked at 0.124812 s by Ball 0.5 on plate: Contact made below
    100 mm/s", "Stopped at 0.383028 s by Ball 0.8 on plate: ...".
  - Tests (TestContact): the ball dropped a millimetre, restitution 0.8:
    marking each contact made below 100 mm/s, the third landing (about
    90 mm/s) and not the first two, at the contact's time and speed;
    stopping on the first let go, the last frame there (1e-8 s), the ball
    just leaving at the contact's speed, 0.8 v0 (2%); stopping on the
    first landing below 100 mm/s, the third, the ball just touching.
    TestGuiCommands: the dialog on a contact set -- the two after its
    values, the component idle, mm/s, the compare's words and back, the
    labels -- and reopened on the sensor; the panel's lines past eight,
    the stop kept after the first seven contacts.
  - Not yet: a count (stop at the third contact), a pair within the set
    (any pair trips it), and Crosses. (Built since: "Built: a sensor's
    count, its pair, and Crosses" below.)
- Built: results kept in the document (sec 8.4's storage, open question
  5): each study keeps its last run, saved with the file, and shows it
  again when it is opened -- after another study's run, or in a later
  session -- with no solver.
  - The document: `Simulation::KeepResults` {None, Motion, All}, default
    Motion (Motion: the poses and the events, which playback, the video
    export and the traces read; All: the channels' values too; None:
    nothing, the run lasting until another study runs, as before), and
    `Simulation::Results`, an `App::PropertyFileIncluded` (hidden, an
    output) through the blob store (docs/FileBlobsManager.md): an undo
    step references it, an unchanged one is not written again.
    `ResultsStale` is saved now. The panel's row "Keep in the document"
    sets KeepResults.
  - The archive (`SimulationArchive.{h,cpp}`): a zip of `index.json` --
    format, schema 1, units, the tier kept, the bodies' ids, the parts each
    moves (by name; another document's by full name) with their offsets,
    the channels, the events -- and `times.npy` (frames), `poses.npy`
    (frames, bodies, 7: position, then the quaternion x, y, z, w) and, All,
    `values.npy` (frames, width), float64 little endian, deflated; numpy
    reads each as it is. Written at the end of each run (`keepFrames()`,
    `archiveResults()`), read only when asked
    (`AssemblyObject::loadSimulationResults(sim)`, Python too): opening
    the document reads none of it. The panel opened on a study loads its
    results and shows the player at the first frame (a run shows the
    last); kept as Motion, a Dynamic or Static study's panel says the
    plots' values were not kept.
  - Size, measured: 20 pendulums (41 bodies, 1001 frames): the poses
    2.30 MB raw, 0.98 MB deflated (0.43); with All the values (120 a
    frame) 0.96 MB raw, 0.74 MB deflated; the archive 0.98 MB as Motion,
    1.72 MB as All. Sec 8.4's column-major delta coding with a byte
    shuffle measured 0.32 on the same poses against deflate's 0.43: a
    quarter smaller, at the price of numpy reading the blocks as they
    are; not taken. So 100 bodies over 1000 frames is about 2.4 MB as
    Motion -- Motion stays the default.
  - The stale flag, per study: the model and spec each study's results
    were run from (`baselines`, by the study's name), so every study with
    results is checked after a recompute that followed a change, not only
    the one whose frames are played. Another study's run leaves a study
    with an archive as it was, its flag and baseline kept; one that keeps
    nothing has its frames go and is not stale, as before. A study opened
    from the file has no model in memory: it is built from the document
    as it was saved, at the first change after opening
    (`captureBaselines()`, on `signalBeforeChangeObject`, so before the
    change lands); one saved stale stays stale. Upstream's Python joints
    are converted to C++ joints when a file is opened, under other names:
    results run on them are stale once opened.
  - Tests (TestSimulationResults): a pendulum's run kept as All, saved,
    closed and opened: read back, the same times, values and events bit
    for bit and the same placement at each frame (1e-12), and numpy reads
    the archive's blocks; Motion without the values, None with nothing to
    read back; two studies each reading back their own; opened again, the
    study stale on its end time and on a joint's frame and fresh when
    each is put back, read back or not; saved stale, opened stale, and
    played. TestDynamics' stale flag: another study's run leaves the
    first stale, kept; keeping nothing, not. TestGuiCommands: the panel
    opened on a study after another's run shows its kept frames, the
    player at the first, and the Keep row sets KeepResults.
  - Not yet: the browser tier reading the archive (the animated glTF
    export: built, below); the results' own channels (a plot of a channel
    the archive lacks says so only through the panel's line); an archive
    from a later schema is refused, not read in part.
- Built: animated glTF export (sec 8.4's "the parts' meshes plus one node
  animation per body, for Blender and web viewers"): the frames the
  assembly holds -- a run's, or a study's read back from the document --
  as glTF 2.0, a .glb or a .gltf with its .bin.
  - Where: the panel's Save Animation offers "Animated glTF, binary"
    (.glb) and "Animated glTF" (.gltf) beside the videos;
    `SimulationGltf.exportGltf(assembly, path)` from Python, without a GUI
    too, returns what it wrote (Frames, Nodes, Animated, Meshes, Bytes).
  - The file (`SimulationGltf.py`, Python and numpy, no new dependency):
    metres and Y up as glTF has them -- a root node turns the assembly's Z
    up to Y, under it the assembly at its global placement. A node per
    component as the assembly nests them (App::Part and sub-assemblies
    are nodes with their children; a link, a link array, a body a leaf
    with its shape), named by label, the document name in its extras;
    the joints, studies, views and BOM groups and what the view hides
    left out. Each leaf's shape (`Part.getShape(transform=False)`) is
    meshed face by face at its view's Deviation, a normal per vertex from
    the face's own triangles (smooth across a face, sharp at its edges),
    one primitive per colour, its material the view's ShapeAppearance
    (sRGB to linear, metallic 0, roughness 0.5, blended when transparent;
    grey without a view). Components showing one shape the same way share
    its mesh. A component the run moves has a translation and a rotation
    channel, LINEAR, on one shared time input from the run's first frame
    (its start time in the asset's extras), each quaternion on the side
    of the one before so a viewer turns the short way; frames not after
    the one before are dropped, as glTF wants. The asset's extras name the
    study and say whether its results were stale (sec 8.4: "export works
    on stale results too, and says so").
  - Checked against others' readers: Khronos' glTF validator (2.0.0-dev.
    3.10) reports nothing -- no error, warning, info or hint -- on a
    pendulum's .glb and .gltf and the Geneva drive's .glb; three.js r170's
    GLTFLoader and AnimationMixer, in headless Edge, play the Geneva
    drive (one driver turn, 241 frames, 109 kB) with the wheel within
    1.3e-5 deg of ThunderealCAD's own playback at every frame (float32),
    from the same camera (the manual's `assembly/simulation/gltf-export`).
  - Tests (TestSimulationResults): a pendulum's .glb read back -- the
    rod's translation and rotation at each frame those playback writes
    (1e-8 m, 1e-6), its times the run's from its start, the base still,
    the rod's mesh its box in its own coordinates (1e-7 m), unit normals,
    indices in range, accessors' bounds -- and the .gltf and .bin the
    same; results read back from the document export byte for byte as
    the run did; stale ones say so; a hidden part is left out.
    TestGuiCommands: Save Animation's glTF format with no suffix typed
    writes a .glb, a part coloured as its view shows it.
  - Not yet: a test of nested components (App::Part, sub-assemblies,
    links) -- the walk takes them, the tests are of flat assemblies; Joint
    copies that follow a component (`relocateCopies`) stand still in the
    file; a link's own scale is not looked at; textures; frames thinned
    where the motion is straight.
- Built: a sensor's count, its pair, and Crosses (the event sensors' "not
  yet").
  - The document: `Sensor::Count` (an integer, at least 1, default 1),
    the trip it acts on -- those before counted and let by; a Stop stops
    at it, a Mark marks it and each after; `Sensor::Pair` (a link list),
    a contact set's events' pair -- one component, any pair with it; two,
    that pair; none, any pair of the set (more than two warns, the first
    two used; on another quantity it warns and is not used); `Compare`
    gains Crosses, a value passing the threshold either way. The dialog's
    rows "Between" (two combos of the set's components, GroupA, GroupB and
    a profile contact's two, else the assembly's; idle but for events) and
    "On trip"; Crosses offered for a value and not for an event, which
    compares a speed. Labels: "Balls on plate: Contact made by Ball 4 mm,
    from the 2nd", "...: Contact made, at the 7th", "...: Contact force
    crossing 0 N".
  - The model: `Solver::Sensor` gains `crosses`, `count` and `frames`, a
    frame on each of the pair's parts (`frameOn()`): frames keep their ids
    through the reductions, and the run names each one's body as the events
    do -- a part in a rigid group, its group's body, so any of the group's
    parts names the pair.
  - The run (ChronoBackend): a value's sensor counts its passes in the
    steps kept (`Watch::passes`; `passBy()` on a step where none acts, and
    `tripsAt()`), and bisects only for a pass it acts on (`anyTrips()`
    asks `acts(passes + 1)`), so a pass let by costs nothing. An event's
    (`EventWatch`: its pair's body names, its passes) counts the events it
    matches, in time order (`eventTrips()`); a stopping one bisects for
    the event that brings its count to the mark (`eventStops()`).
  - A fix found on the way: a value's sensor read the value only at the
    end of each piece it watched, while `advance()` resolved an impact
    within the piece in substeps of its own (the contact found late, the
    substep taken again an eighth as long) -- a whole impact could begin
    and end inside one piece, unseen. At 1 ms output steps a contact force
    sensor marked a bouncing ball's first two landings and not the third.
    Now a piece in which a contact was made and let go is taken again
    halfway to the contact, the rest in the contact's own substeps, which
    then see it coming. Not cut at the contact itself: a piece starting
    where the ball touched read the impact's speed as none, and the
    bounce came out at 53 mm/s for 72.
  - A sensor's bisection takes steps of its own, so the run's path after
    a located trip is not the path without the sensor: the ball's third
    landing moved by up to 120 us between sensor settings (0.05563 s with
    none, 0.05556 to 0.05568 s with one), each run's events and marks
    agreeing with each other to 1e-6 s.
  - Tests (TestContact): the ball dropped a millimetre, restitution 0.8:
    stopping at the third contact made, there to 1e-8 s, the ball just
    touching; marking from the second, each landing after the first; the
    contact force crossing none, marked at each of the six contacts made
    and let go to 1e-6 s, up at a landing and down at a let go, and
    stopped at its fourth crossing, the second let go. Two balls in one
    set, the second dropped from 3 mm landing after the first: a sensor on
    the second ball, on it and the plate, or on the plate and it, stops at
    its landing, sqrt(2 h / g) to 1e-6 s; on the two balls, which never
    touch, never; the set's second contact made is the same landing.
    TestGuiCommands: the dialog -- Crosses for the force and not for an
    event (an event falling back to Above), the pair idle for a value and
    the set's components for an event, the count, the labels, reopened.
    In the manual's video (`assembly/simulation/sensor-counts`) three balls
    in one set: marks at the 4 mm ball's second and third landings, the
    stop at the set's seventh, the 10 mm ball's second, each at its
    landing's time.
  - Not yet: a count that starts again (every third); Crosses on an
    event's speed; a pair of a cam's contact named by its cam and
    follower. (The count that starts again, and the cam's pair: built,
    below.)
- Built: a cam's pressure angle and slip (sec 8.5's measures "for a cam:
  pressure angle and slip velocity"; the cams' "not yet").
  - The run (ChronoBackend): a cam contact's set (`ContactPair::cam`)
    reports two more channels after its four, `CamPressureAngle` (rad)
    and `CamSlip` (mm/s), read at the one point its collision detection
    last found (`camReading()`), touching or not. The pressure angle is
    the acute angle between the contact's normal (N2 to the follower's
    side) and the way the follower's point of contact moves -- its
    velocity, or where it stands still (a rise's top) its acceleration; 0
    at rest. The slip is that point's velocity from the cam's material
    point under it, along the curve's unit tangent (`CamFollow::tangent`,
    kept with the parameter at each detection), signed by the curve's
    direction.
  - The document and the panel: `Sensor::QuantityEnums` gains the two
    (the angle's threshold in degrees, to the run's radians as a torsion
    spring's angle; the slip's a velocity); `CommandCreateSimulation`'s
    "CamContact" kind offers them for a cam contact, after its contact
    values ("Pressure angle", deg; "Cam slip", mm/s), and the Results box,
    CSV and spreadsheet carry them.
  - Found on the way, two older faults:
    - A run's start values were read before its first collision
      detection, when the contact loads have no points: a cam's pressure
      angle read 0 at the first frame, and so would a contact force of a
      part resting from the start. Now detected first: the box resting
      on the plate reads its weight, 105.9118 mN, at the first frame.
    - A cam contact whose follower is fixed to another part (a roller
      and its stem, one body of the rigid groups' smaller model) diverged
      at once ("the contact's integration diverged"): the reduction
      renumbers the joints it keeps (`jointOrigin`), and copied the
      contacts with the cam joint's old id, which then named another
      joint or none -- the run kept the cam's joint beside the contact.
      The cam's id is now mapped to its new one; it is the only joint
      reference in the model by id (limits, drivers and the rest are on
      frames).
  - Tests (TestCamJoint): the disk cam (E 6, R 20) at 10 rad/s under
    gravity down the slider, a point follower and a roller of 3 mm: the
    pressure angle asin(E |cos theta| / (R + r)) at each of 201 frames
    (5e-4 rad; 2.0e-4 and 1.5e-4 measured), its largest asin(E / (R + r)),
    the slip the follower's point's speed from the cam's along the
    tangent (0.2 mm/s of some 200; 0.085 and 0.091 measured); the roller
    fixed to a stem riding the cam as a lone roller does (0.02 mm), the
    stem with it. TestGuiCommands: a sensor's dialog offers both for a
    cam contact and not for a plain set, in degrees and mm/s, and its
    label. In the manual's video (`assembly/simulation/cam-pressure-angle`)
    a roller of 5 mm with its stem: the largest angle 13.892 deg against
    13.887, within 0.007 deg over the turn; a sensor marking the angle
    rising past 12 deg at 149.99 and 329.99 deg against 150.03 and
    330.03; the slip -260 to -140 mm/s, the rate times R + E and R - E.
  - Not yet: the angle's direction (which side the normal leans to); a
    pressure angle for a profile contact's pairs; the contact force split
    into the part along the follower's motion and the side load on its
    guide. (The split, and with it the direction: built, below.)
- Built: a cam's drive force and side load (the pressure angle's "not
  yet"): a cam contact's force split along the follower's line of motion
  and across it.
  - The run (ChronoBackend): `camReading()` gains `way`, the unit vector
    along the follower's point's velocity (at a standstill its
    acceleration) turned away from the cam, the normal's side; with no
    motion, the normal. `CamDriveForce`, the set's ContactForce's part
    along it, and `CamSideForce`, the rest, width 3 each, after
    `CamSlip`: they add up to ContactForce, the force on the cam, as the
    normal and friction parts do. At rest the drive is the normal force
    and the side load the friction.
  - The angle's direction is the side load's: the pressure angle stays
    its size, 0 to 90 deg -- what a design limits, and what a sensor
    "above 30 deg" wants -- and the side load points to the side the
    force leans to. atan(side / drive) along the guide's axis is the
    signed angle.
  - The document and the panel: `Sensor::QuantityEnums` gains the two
    (a force's threshold); `CommandCreateSimulation`'s "CamContact" kind
    offers them after the slip, as vectors (`SensorVectors`: a component
    or the size), "Cam drive force" and "Cam side load", N.
  - Tests (TestCamJoint): on the disk cam at 10 rad/s, for the point
    follower and the roller: drive and side load adding up to the force
    (1e-9), the drive along the slider and the side load across it (1e-5
    of the force), the side load's X the force's size times
    E cos theta / (R + r), signed (5e-4; 1.9e-4 and 1.5e-4 measured).
    TestGuiCommands: the sensor's dialog offers both for a cam contact
    and neither for a plain set, the component live, N, the label "...:
    Cam side load X above 2 N". In the manual's video
    (`assembly/simulation/cam-side-load`) the roller with its stem: the
    lean within 0.007 deg of -asin(E cos theta / (R + r)), its size
    within 0.005 deg of the pressure angle; a sensor on the side load's X
    crossing 0 marks at 90.000 and 270.000 deg.
  - Seen in the video, not this change's: the contact force bumps by
    about 3% every 15 deg of the turn (10 output frames), up a frame and
    down the next; ContactForce has it as the split does.
  - Not yet: a pressure angle for a profile contact's pairs; the bumps'
    cause. (The bumps: found and fixed, below; a profile contact's angle:
    built, below.)
- Built: a cam's point followed within a substep (the bumps' "not yet"):
  the cam contact's force bumped, and its smooth part was off too.
  - The cause: a contact load's points are collision detection's, found
    as a substep starts and fixed on each body through it. On a cam the
    follower slides along the curve, ~200 mm/s on the disk cam, and the
    cam's material point under it a substep later is off the curve by
    (slip h)^2 / 2 rho. At the run's substeps, a fiftieth of the
    contact's period (0.1 ms of 5.2), that is nothing; but once the
    contact has rested five periods (advance(), `settled`) a substep runs
    to the output step's end, 0.4 to 0.9 ms, and the roller, sinking
    0.01 mm, ended one 4% deeper. A step taken again (the depth's range
    past 2%) left nothing behind; one kept -- most in the cam's falling
    half, where the force eases -- set the follower ringing on the
    contact's spring. So the bumps every 26 ms, the run's time and not
    the cam's angle: as many at an output step of 0.65 ms as of 2.6, and
    more and larger at a looser tolerance, whose wider rest kept more
    long substeps. Measured with a trace of each substep's length, its
    retakes and the load's rest.
  - The fix (ChronoBackend): `ContactLoad::follow`, set for a cam's load,
    puts its one point where the curve is under the follower at the
    state ComputeQ() is given -- HHT's iterates and the jacobian's
    differences included -- from the parameter collision detection found
    (`CamFollow::place()`, which collision detection uses too). A
    substep's end depth for the contact events (`touchOf()`) takes it
    the same way (`pointsNow()`).
  - Tests (TestCamJoint, test_cam_contact_force): the disk cam at
    10 rad/s under gravity, frictionless, the point follower and the
    roller: the force along the slider, |F| cos(pressure angle), is
    m (g + y'') at every frame past the first 20 ms, to 1e-3 (1.2e-4 and
    1.0e-4 measured). Before the fix it failed, 2.3% off for both. On the
    manual's roller with its stem the force was 1.1% under to 4.3% over
    it, bumps and all; a headless run of the roller alone, now within
    8e-5 at output steps of 2.6 and 0.65 ms and at a tolerance of 1e-3.
  - Found on the way, in the manual's video re-made: a sensor's mark
    kicked the run. A step that trips a sensor was bisected along the
    run itself, each half kept from the state before, so the run went
    through steps halving down to the locating precision, 1.7 ns. HHT
    holds the joints in position, and a step of h turns their round-off
    e into a speed e / (beta h): the follower came out of each mark
    moving 4e-5 m/s square to its slider, its force ringing (0.7%) and
    its pressure angle, read from its speed's direction where it turns
    round, 1.3 deg off at 271.5 deg. The bisection is now aside
    (`watchedPiece()`): each try one step from the piece's start; a
    stop ends the run at the state located, as before, and a mark goes
    back to the piece's own step, so the run goes on as it would
    unwatched -- a second trip within the piece located the same way
    from the first. The piece's earlier marks are kept across the
    restores.
  - Tests (TestCamJoint, test_cam_contact_force): with and without a
    sensor marking the side load's X crossing 0: the force as above,
    the pressure angle atan(side / drive) at every frame (0.01 deg; 1e-6
    measured), the marks at 90 and 270 deg (0.01 deg; 90.0000017 and
    270.0000000). The marked cases failed on the bisection kept along
    the run (0.5% and 0.7% off). TestAssemblyWorkbench 576 OK, 88
    skipped, its sensor and event tests among them.
  - Not yet: a profile contact's points are fixed the same way through a
    substep (a Geneva wheel's pin slides in its slot); the profile is
    found again only at collision detection. (For an arc against a line:
    built, below.)
- Built: a profile contact's arc against a line followed within a
  substep (the "not yet" above), for a cam made of profiles: a disc
  turning under a flat-faced follower, or a pin pressing a slot's wall.
  - Measured first: the disk cam (E 6, R 20) at 10 rad/s as a profile
    contact, a block of 60 x 10 x 10 riding it flat on a slider under
    gravity, frictionless, its force m (g + y'') in closed form. With the
    points fixed: 0.85% high on average and ringing up to 4% after each
    long substep at rest, as the Cam joint's contact rang. Without the
    long substeps (an experiment: no profile load settled) it was still
    0.82% high, smooth -- the second half of the fault, which a substep
    of any length has: the law's damping reads the approach speed at the
    material point, which drifts from the contact's as it slides off the
    curve, slip^2 t / rho, so its error is first order in the substep. On
    the Cam joint the long substeps had hidden it.
  - The run (ChronoBackend): profileTouches() keeps how each point was
    found (`ProfileFound`); for an arc's point farthest across a line, the
    other's nearest on that line, the load finds it again at each state
    ComputeQ() is given (`ProfileFollow::place()`, through the load's
    `follow`, which now takes all its points; the Cam joint's takes its
    one): the arc's farthest point across the line, held at the arc's end
    past it (`farthestOnOrEnd()`), and the nearest on the line, held at
    its end (`nearestOnLine()`), the normal the line's. `pieceIn()` is one
    piece of `profileIn()`, moved into A's plane at the state.
  - Not followed, and why, each found on the way to the Geneva drive's
    test (0.5 s) and its manual run (two turns at two output steps):
    - An arc against an arc (the line of the centres): followed, the pin
      striking its slot's mouth ran Newton's iterations off, 13 mm deep
      in steps of 6 ns, and the wheel was left behind. Most such pairs
      are a pin in a hollow of nearly its radius, on all of it at once;
      their error, (slip h)^2 times the curvatures' difference, nearly
      none.
    - A point leaving its pieces dropped back to collision detection's:
      the force jumped within Newton's iterations, and the second turn
      ran off. Held at the pieces' ends instead.
    - Off an end or a corner, the side read as nearestOn() reads it (along
      x - y): a Newton iterate 10 mm off read 10 mm deep where the line
      had it 10 mm clear. Now from the line; and a point whose other is a
      corner is not followed -- both are points of their material, exact.
    - An arc's point whose other's nearest is on a piece beside its line:
      the farthest across that line is not the point nearest the other.
    - A convex corner against the other's nearest piece: followed with
      the arcs, the second turn ran off (50 kN); each alone did not, and
      the cause was not found. A corner is a point of its material, exact
      on its side; not followed.
  - Tests (TestProfileContact, test_disk_cam_flat_follower): the block's
    force m (g + y'') at every frame past 45 deg, to 1e-3 (7.3e-5
    measured); 0.81% off before. The Geneva test as it was. TestAssembly-
    Workbench 577 OK, 88 skipped. The manual's Geneva run, two turns at
    output steps of 4.2 and 1.04 ms: the wheel's angles within 0.0013 deg
    of each other (0.0076 with the points fixed), the pin's impact on its
    slot's wall 1.615 N at both (0.555 and 1.336 N fixed: the impact's
    peak lay between frames as the substeps fell).
  - Not yet: an arc against an arc followed (a roller on a cam's arcs);
    a corner against a curved piece; the cause of the corners' run-off.
    (An arc against an arc: built, below.)
- Built: a profile contact's arc against an arc followed within a substep
  (the "not yet" above): a roller on a cam's arc, a pin in a hole.
  - Measured first: the disk cam under a roller of 5 mm on the slider, as
    a profile contact, its force along the slider m (g + y''), y = E sin
    theta + sqrt((R + r)^2 - (E cos theta)^2): 2.3% high at every frame,
    smooth -- the damping's half of the fault, larger than the flat
    face's 0.85% as the relative curvature, 1/R + 1/r, is five times
    the flat's 1/R. A bar hung by a hole of 3.005 mm on a grounded pin of
    3 mm, swinging from 30 deg frictionless: its swing died down, 30 to
    29.93 deg over four swings, the law's damping reading the points'
    slide off the arcs as an approach; its force rough, 250 mN frame to
    frame on a 180 mN load at an output step of 1 ms.
  - The run (ChronoBackend): profileTouches() keeps an arc of A's against
    one of B's as `ProfileFound::Kind::ArcArc`, with the sides of the line
    of the centres each point and the normal were found on (`sides`);
    `ProfileFollow::place()` puts each on that line at the state, held at
    its arc's end past it (farthestOnOrEnd()), the normal along it.
  - Why it now holds where it ran off before (the "not followed" above):
    tried again alone, on the arcs-against-lines follow as it stands, the
    Geneva drive's two turns at both output steps are bit for bit the
    same as without it -- its arcs touch the other's arcs only at corners
    or past the arcs' reach, which are not followed -- so the run-off was
    the dropped-back points and the reading off ends, since replaced. The
    pin in a hole, the line of the centres 0.005 to 0.06 mm long and
    turning as fast as the bar swings, stays smooth: holes of 3.0002,
    3.005, 3.05 and 3.5 mm all run, the force rougher without it at each
    (7, 40, 59 and 1546 mN frame to frame without, 0.5, 1.6, 14 and 532
    with, the last bouncing in its hole).
  - Tests (TestProfileContact): test_disk_cam_roller_follower, the
    force along the slider m (g + y'') at every frame past 45 deg to 1e-3
    (9.1e-5 measured; 2.3% before), its lean the pressure angle
    asin(E cos theta / (R + r)) to 0.01 deg (0.006: the roller sinks the
    law's allowance); test_pin_in_hole_pendulum, the bar at every turn of
    its first second at 30 deg to 0.01 (0.0033 measured; 0.018 at the
    second turn before). TestAssemblyWorkbench 579 OK, 88 skipped. In the
    manual's video (`assembly/simulation/profile-roller`) a roller of 5 mm
    on a stem: within 0.0091% of m (g + y'') past 45 deg, a frame here and
    there flicking that far, and the lean within 0.0058 deg.
  - Found on the way, not this change's: a part held only by profile
    contacts tips out of their plane. The bar on its pin, free, tilted
    from round-off, 1e-14 rad at 0.1 s, about tenfold each 0.1 s, until
    at 1.4 s it left the pin and flew off -- with the points followed or
    fixed. The force acts at the middle of where the two solids overlap
    through the plane, which moves with the part's height, and nothing
    stiffens its tilt; a real pin's contact along its length would. The
    tests hold the bar with a parallel joint.
  - Not yet: a corner against a curved piece; the cause of the corners'
    run-off; a part on profile contacts alone kept in their plane. (The
    plane: built, below.)
- Built: a part on profile contacts alone kept in their plane (the "not
  yet" above): a bar hung by its hole on a pin, nothing else holding it.
  - The cause: a pair's points were taken at one height, the middle of
    where the two solids overlap through the plane, and B's profile was
    brought into A's plane whatever its tilt -- so a tilt changed no
    depth, and the force, at one point, held none. A free part's tilt
    grew from round-off, tenfold each 0.1 s; let go tipped by 1e-4 rad,
    the bar went past 0.1 rad in 0.19 s and slid 27 mm off the pin.
  - The run (ChronoBackend): each pair's points are taken at two heights
    through the overlap, the two-point Gauss rule's, the middle plus or
    minus a 2 sqrt(3)-th of its length (`ProfileFollow::levels()`,
    `height()`), each its own point in the pair's patch -- so the patch's
    force is what one point's was, split in two, and its moment against a
    tilt that of a contact spread evenly along the overlap, for a force
    linear in the depth. At each height B's profile is its section there:
    B's frame slid along its own axis to that height (`sectionAt()`), its
    prism's profile the same anywhere along it, exact to within the
    square of the tilt. A tilted part's section moves across the plane
    with the height, deeper at one point than the other. The followed
    points (`ProfileFound::level`) are found again at their own height.
  - Untilted, the two points are one split in two: the Geneva drive's two
    turns at both output steps as before within 5e-6 deg and 5e-5 N, the
    profile tests as they were.
  - Tests (TestProfileContact, test_pin_in_hole_pendulum): the bar free,
    its parallel joint gone, let go tipped by 1e-4 rad about its width:
    every turn of its first second at 30 deg to 0.01 (0.0031 measured),
    the tilt never past 2e-4 rad (1e-4) and under 1e-5 in the last tenth
    (1.2e-6). Before, it tipped to 3.08 rad. TestAssemblyWorkbench 579
    OK, 88 skipped. In the manual's video (`assembly/simulation/
    profile-plane`) the same bar, its tilt dying down to 3.3e-7 rad in
    the last tenth of the second, 4.6e-3 mm along the pin at most; the
    run on the code before, its tilt past 0.1 rad at 0.19 s.
    Tipped about its length instead, along the contact's normal, where
    the hole's section moves along the pin's arc: held as well, 1.2e-4
    rad down to 1.2e-6 in the second.
  - Not yet: a corner against a curved piece; the cause of the corners'
    run-off. (Both: built, and not reproduced, below.)
- Built: a profile contact's corner followed within a substep (the "not
  yet" above): a knife-edge follower on a cam, a slot's corner sliding on
  a locking disc.
  - Measured first: the disk cam under a knife edge -- a wedge's corner
    on a stem, on the slider -- its force along the slider m (g + y''),
    y = E sin theta + sqrt(R^2 - (E cos theta)^2): 4.5% off at worst with
    the points fixed. The corner is a point of its material, but the
    disk's point under it slid off as the cam turned, and with it the
    normal: the damping's error again, first order in the substep.
  - The run (ChronoBackend): profileTouches() keeps a convex corner of
    A's or B's against the piece of the other's nearest it as
    `ProfileFound::Kind::CornerA` / `CornerB` -- 'at' the piece the corner
    starts, 'across' that nearest piece, only where the nearest is on one
    piece; `ProfileFollow::place()` takes the corner where it is and the
    nearest on that piece: a line's (nearestOnLine()), or an arc's on the
    line from its centre through the corner, held at its ends
    (farthestOnOrEnd()), the normal out of its material there, as
    nearestOn() has it.
  - The run-off (the "not followed" of the arcs against lines): not
    reproduced. With corners followed beside the arcs, the Geneva drive's
    two turns run at both output steps, the wheel's angles within 0.00087
    deg of each other (0.0013 before, the slots' corners sliding on the
    locking disc now followed), within 0.008 deg of the run before. As
    for the arcs against arcs, the run-off came with the attempts since
    replaced -- the points dropped back to collision detection's, the
    side read off ends.
  - Tests (TestProfileContact, test_disk_cam_knife_follower): the force
    along the slider m (g + y'') at every frame past 45 deg to 1e-3
    (6.4e-5 measured; 0.87% off at 45 deg before), its lean the pressure
    angle asin(E cos theta / R') to 0.01 deg, R' the disk's radius less
    the allowance the edge sinks (4.5e-4 deg). TestAssemblyWorkbench 580
    OK, 88 skipped. In the manual's video (`assembly/simulation/
    profile-knife`): within 0.0064% past 45 deg, a frame here and there
    flicking that far, the lean within 0.0005 deg.
  - Not yet: a point whose other's nearest is a vertex -- a corner on a
    corner, an arc past the end of a line -- is not followed. (Measured,
    below: not needed, and left so.)
- Built: a profile contact's pressure angle, slip, drive force and side
  load (the cam's pressure angle's "not yet" above), its B the follower.
  - The run (ChronoBackend): a profile contact's set (`isProfileSet()`)
    reports CamPressureAngle, CamSlip, CamDriveForce and CamSideForce
    after its contact channels, as a cam contact's does, read by
    `profileReading()`: from the set's points where they are now
    (pointsNow()), the pressing ones by their depths -- a touch is two
    points, at its two heights, and a flat on a flat two corners -- the
    normal the set's normal force's, the follower's point's velocity and
    acceleration and A's the points' weighted the same; touching none,
    the nearest point's. The angle and the way from them as a cam's
    (`leaning()`, which camReading() now shares); the slip along A's
    outline, z_A x normal, the way it runs with A's face to its left.
  - The panel: `sensorSourceKind()` takes a ProfileContact as a
    "CamContact", so a sensor's dialog offers the four on it.
  - Tests (TestProfileContact's disk cam, ride_disk_cam(), for the flat
    follower, the roller and the knife edge): the pressure angle at every
    frame past 45 deg, 0 for the flat and asin(E |cos theta| / reach)
    for the others, to 0.01 deg (4.4e-5, 0.0059, 4.5e-4 measured); the
    drive and side load adding up to the force, the drive along the
    slider and the side load across it, to 1e-5 of the force (1e-12);
    the slip, the follower's point's speed from the disk's along its
    outline at the disk's point under it, to 0.05 mm/s (0.0007 to
    0.0035). TestGuiCommands' sensor quantities: a profile contact
    offers the four. TestAssemblyWorkbench 580 OK, 88 skipped; the GUI
    suite's TestGuiCommands 65 OK. In the manual's video
    (`assembly/simulation/profile-pressure`), the roller: the pressure
    angle within 2.1e-4 deg of asin(E |cos theta| / R'), R' the centres
    apart less the allowance, the drive within 0.0091%, the side load
    within 0.0011 mN, the slip within 0.0024 mm/s; a sensor marking the
    angle past 12 deg at 149.992 and 329.990 deg against 149.992 and
    329.992.
- Built: contact events name the parts that touched (the events' "not
  yet" above): a body standing for a rigid group, or a part placed on the
  ground, named by the part of it whose shape touched.
  - The model: `Frame::part` (Model.h), the component a frame is on where
    the model's body is not its own; the grouping (`RigidGroups.cpp`)
    sets it from the original body's source on every frame it moves, a
    member's onto its group's body and a placed part's onto the ground.
    Before, a part placed on the ground -- a pad fixed to the grounded
    plate -- was named as the ground, None.
  - The run (ChronoBackend): each contact point keeps its shapes' frames
    (`ContactLoad::Point::frames`): a Bullet point's from its shapes
    (`ShapeOn::source`, a mesh triangle's by its mesh), a cam's the
    joint's I and J, a profile contact's its two profiles'. A touch's
    deepest point carries them (`Touch::frames`), and an event names the
    parts at the one where it presses -- at the end made, at the start let
    go (`partName()`: the frame's part, else its body's source, the
    ground ""; a frame not known, the body's name as before). A sensor's
    pair is named the same way, so a pair of parts within groups matches
    their events.
  - Found on the way, and fixed: a dynamic run left out the joints of
    parts nothing joins to the ground. `buildSimulation()` drops the
    joints that do not reach a grounded part (`removeUnconnectedJoints()`,
    as a solve does), but a contact set or a spring makes bodies of such
    parts after -- so a rocker of a beam and two feet fixed to it, dropped
    on a plate, fell apart into three, and a cube fixed on top of one a
    spring holds stayed put while the other swung alone. Those joints now
    come back where one of their parts is a body of the run, until none
    is left to add, before the masses weigh the bodies.
  - Tests: TestContact, test_events_name_the_parts_that_touched -- a ball
    fixed under a holder (the group, the holder first) dropped a
    millimetre onto a pad fixed to the grounded plate: its landing at
    sqrt(2 h / g) (1e-6 s) names (ball, pad), the ball still under the
    holder at the end (1e-6 mm), and a sensor on that pair stops there;
    before, the pad read None. TestDynamics, test_spring_holds_fixed_pair
    -- a cube a spring holds, no gravity, a second fixed on top: the two
    10 mm apart at every frame (1e-6) and swinging by half a period of
    sqrt(2 m / k) (1e-3); before, the top one stayed put, and the two were
    off their 10 mm in the first frames. TestAssemblyWorkbench 582 OK, 88
    skipped; the GUI
    suite's TestGuiCommands 65 OK. In the manual's video
    (`assembly/simulation/event-parts`): the rocker, its right foot over a
    pad 3 mm high, the left over the plate, the beam its group's first
    part: 164 events, each naming "Left foot and Plate" or "Right foot and
    Pad", the first at 0.02474 s in at 242.57 mm/s against sqrt(2 h / g)
    and sqrt(2 g h), 0.024735 s and 242.5694.
- Built: a sensor that marks every so many trips (the counts' "not yet"
  above, "every third").
  - The document: `Sensor::Repeat` (a bool, default off): a sensor that
    marks acts on every Count'th trip -- that one, twice that, and on --
    rather than that one and each after; one that stops does not look at
    it (it stops at its Count'th either way). The dialog's "Every such
    trip" beside "On trip", live for Mark only; the label "...: Contact
    made, every 3rd".
  - The model and the run: `Solver::Sensor::repeat`, set only for a mark
    (AssemblyObject), compared in `same()`; ChronoBackend's `acts()` takes
    a repeating mark's pass when `passes % count == 0`. A value's sensor
    and an event's count their passes as before, so the counting -- and
    the bisection only for a pass it acts on -- is unchanged.
  - Tests (TestContact, test_sensor_repeats): the ball dropped a
    millimetre, restitution 0.8, 0.1 s: a mark on every 2nd contact made
    at the 2nd, 4th and 6th, at their times to 1e-8 s; on every 2nd
    crossing of the contact force through none, at each let go (1e-6 s),
    the force there none; a stop with Repeat at the 2nd contact made, the
    run ending there. TestGuiCommands: the box live for a mark and idle
    for a stop, the label "every 3rd", reopened checked, a stop clearing
    it ("at the 3rd"). TestAssemblyWorkbench 583 OK, 88 skipped; the GUI
    suite's TestGuiCommands 65 OK. In the manual's video
    (`assembly/simulation/sensor-repeat`), a ball dropped 10 mm for 0.5 s:
    27 contacts made and 9 marks of every 3rd, each at its contact's time
    exactly; 26 let go and 26 marks of every 2nd force crossing, each
    within 1e-7 s of its let go.
- Built: a cam contact's pair named by its cam and its follower (the
  counts' "not yet" above).
  - The run named them already: a cam contact's points carry the Cam
    joint's frames, I on the cam and J on the follower, and an event the
    parts of those (`partName()`), the cam first -- a follower fixed to a
    stem, the stem first in their group, is the follower and not the stem.
  - The dialog did not: `contactComponents()` read a set's GroupA and
    GroupB and a profile contact's profiles, which a cam contact has none
    of, and fell back to every movable part of the assembly. It now takes
    a cam contact's Joint's two sides (`JointObject.sideMovingPart()`),
    the cam and the follower, so its sensor's pair offers those two.
  - Found on the way, and fixed: an event's speed read below none. A
    contact made is carried forward from its substep's start, and one let
    go back from a substep after, each with the acceleration at that end;
    where the free acceleration changes fast -- a cam's follower grazing
    the cam -- that takes it past none, a hop's landing at -12.8 mm/s and
    a lift-off at -32.6, and a sensor on every contact made (speed at
    least 0) missed those landings. The depth crossed none the way it
    went, so the speed is now never below none (`noteTouch()`,
    `readLeaving()`).
  - Tests: TestCamJoint, test_cam_contact_events_name_cam_and_follower --
    the disk cam at 45 rad/s, a little above the 40.4 at which gravity
    keeps the follower on, a roller fixed to a stem (the stem first in
    their group) hopping along it four turns: every let go and landing
    (cam, roller) and none below none; a sensor on (roller, cam) marking
    every landing at its time, one on (stem, cam) none;
    contactComponents() the cam and the roller. Before, the first let go
    read -5.9 mm/s. TestGuiCommands' cam sensor quantities: the pair's
    boxes offer the cam and the follower alone (before: every part).
  - Not yet: a grazing contact's speed itself -- floored, not found: the
    carry takes the acceleration at one end, which a hop's lift-off and
    a grazing landing change within the substep. (Built, below: the
    acceleration carried was not the speed's.)
- Measured, not built: a profile contact's point whose other's nearest is
  a vertex, followed within a substep (the corners' "not yet" above).
  - Where it presses: only at a hollow vertex. At a convex one the point
    is outside the other, within the envelope; inside, the nearest is on a
    piece. So the case is a corner in a hollow corner -- a knife edge in a
    V notch, the pivot of a balance or a Kater pendulum -- or an arc's
    point sunk behind a notch's bottom, which takes an arc deeper than its
    pieces either side.
  - The scene: a knife 30 mm long, its edge 30 deg either side, across two
    posts notched 45 deg either side, a bob hanging between them, let go
    at 10 deg, frictionless (restitution 0.5), a profile contact between
    the knife's end face and both posts' faces. On the code as it stands
    (the points fixed through a substep) it swings about the edge as
    about a pin: its period 4 sqrt(I / m g d) K(sin(5 deg)) to 7e-5,
    every turn of two seconds at 10 deg to 0.0021 deg, the edge sunk
    6.3 um and shaking across the notches by up to 2 um -- the contact's
    spring holds it there and, frictionless, nothing damps a shake
    across it.
  - Tried: each such point followed, the other's nearest on its two pieces
    at the vertex, the normal along x - v there, out of the material (away
    from a convex vertex, towards a hollow one). The swing the same: the
    period, the turns within 0.0023 deg, five seconds' loss 0.011 deg
    against 0.007 fixed. The shake a little smaller: 1.1 um against 1.9
    in the test's scene, 1.6 against 1.8 in the manual's, which differs
    only in the posts' height.
  - Found on the way, with the points followed: the knife twisted 1e-4 rad
    about the vertical at 1.36 s of the manual's scene, its ends 0.8 um
    across their notches either way, and stayed so to the end, the shake
    gone with it. A twist given at the start comes back within 50 ms. The
    contact counts as resting from 18 ms on -- a swing changes its depth
    by under the 2% of `resting()` -- and its substeps grow to the output
    step; from 1.34 s it stayed settled. Fixed, the same scene neither
    stayed settled nor twisted. Not built: the gain, a shake a few tenths
    of a micrometre smaller, is not worth a twist held for no cause found.
  - Tests (TestProfileContact, test_knife_in_notch_pendulum): the period
    to 2e-4 (7e-5 measured), every turn at 10 deg to 0.02, the edge
    across the notches under 3 um (1.95) and sunk under 7 (6.35).
    TestAssemblyWorkbench 585 OK, 88 skipped. In the manual's video
    (`assembly/simulation/profile-knife-edge`) the posts taller, on a
    base: the period to 1.3e-4, the turns within 0.012 deg, the edge
    1.8 um across at most.
  - Not yet: a swinging pivot taken as at rest -- `resting()` reads the
    deepest point's depth alone, which a pin or a knife edge keeps while
    the part swings past it.
- Built: a contact event's speed carried along its substep by the speed's
  own growth (the grazing contact's "not yet" above).
  - Measured first: the disk cam at 45 rad/s, its roller on a stem thrown
    off, landing, bouncing, landing again and thrown off as the cam comes
    round, five events a turn. Each flight is free, y'' = -g along the
    slider: a parabola fitted to its frames, solved against the disk,
    gives how fast it lands and goes off along the line of the centres.
    The landings read within 2.7 mm/s of that, the bounces up to 25 mm/s
    slow (104.2 read 89.9 at an output step of a 480th of a turn), and the
    roller leaving as the cam came round at 0 for 1.68 -- carried below
    none and floored.
  - The cause: the carry's acceleration (`touchOf()`) was the material
    points' relative acceleration along the normal. On a cam the point
    slides along each body and the normal turns, and the disk's material
    point under the roller is pulled round at 5e4 mm/s^2: leaving, the
    speed grew at -1080 mm/s^2 and read -39550; within a landing's
    substep it grew at +18000 and read -20400, so the carry went the wrong
    way.
  - The run (ChronoBackend): for a load that follows its points (a cam
    contact's, a profile contact's), the deepest point's speed is taken
    again a microsecond either side along the bodies' motion (`ahead()`),
    its points found again there, and the carry is their difference. The
    others' points are fixed on each body, their normal fixed: the
    material points' acceleration is theirs, as before. The floor at none
    stays, no longer reached in these runs.
  - Tests (TestCamJoint, test_cam_contact_event_speeds): the speed of every
    event whose flight has two frames clear of the disk, 34 of 40 at a
    120th of a turn, to 0.1 mm/s (0.041 measured); on the code before, the
    first let go read 0 for 1.68. `hopping_roller()` builds the scene for
    it and for the events' names test. TestAssemblyWorkbench 586 OK, 88
    skipped. At other output steps and rates, the same scene by a probe:
    a 480th of a turn 0.094 mm/s at most, a 1000th 0.33, 42 rad/s 0.034.
    In the manual's video (`assembly/simulation/event-speeds`), its roller
    5 mm: 32 of 40 within 0.18 mm/s, the landings within 0.0024; 21.4 on
    the code before.
  - Found on the way: the same scene at an output step of a 1200th of a
    turn (0.116 ms) stops, "the contact's integration diverged"; at a
    1000th and coarser it runs. Not this change's: the code before stops
    the same.
  - Not yet: that divergence; a profile contact's event speeds, carried
    the same way now, not measured against a closed form.
- Measured, not built: a swinging pivot taken as at rest (the knife edge's
  "not yet" above). The pin-in-hole pendulum (TestProfileContact's, a bar
  on a 3 mm pin through a 3.005 mm hole, let go at 30 deg) counts as at
  rest 18 times in two seconds, a quarter of a swing apart from 0.4 s on
  -- at the turns and at the bottom, where the load on the pin, and so
  the depth, stands still -- and its substeps grow while it is. Its
  period through it: 0.394444 to 0.394446 s at an output step of 1 ms, 0.394438 to 0.394454 at 10 ms, where a settled substep may
  grow to 10 ms; the closed form about the hole's centre 0.394379, the pin
  5 um off it. The turns within 0.0033 deg of 30 at 1 ms, within 0.093 at
  10 ms -- a turn read off the frame nearest it, 5 ms off at most, is
  that much short. No harm found; `resting()` left as it is.
- Built: a profile contact's geometry checked against brute force
  (`Solver/ProfileGeometry.h`, moved out of ChronoBackend.cpp so that a
  test reaches it: the profiles, their nearest points, profileTouches()).
  - The test (tests/src/Mod/Assembly/App/ProfileGeometry.cpp,
    `ProfileGeometry.pointsAgainstBruteForce`): 16 pairs of profiles -- a
    disc, a box, rounded boxes, a knife, a V notch, a hole, a slot, a
    groove -- B turned at random and brought along a way in turned at
    random until the two overlap by a depth chosen, by bisection: a gap of
    half the envelope, 1e-7, 1e-6, 1e-5 and 1e-4 m. Brute force walks each
    boundary in 0.1 mm steps, its distance to the other's exact on its
    lines and arcs, its side by a ray's crossings of them, each deepest
    point found again by a golden-section search. Each place one boundary
    goes into the other must have a point there at least as deep; each
    point must be on its boundaries, its normal of length 1, its depth its
    own point's, none deeper than the deepest; the deepest, or the nearest
    within the envelope, found. Each case run both ways round, B as A.
  - It catches the finder broken: an arc against an arc tried only one way
    along the line of the centres, 48 of 480 cases failed (a pin as A in a
    hole as B, which the test first lacked -- each pair had its hole as A
    only); B's corners not tried, 205; the normal off a corner the nearest
    piece's instead of along the line between the points, 9 (a knife's
    edge sunk below a V notch's bottom, nearest its vertex, which a way in
    turned by up to 30 deg never reached: that pair added, its way within
    1e-3 rad).
  - On the code as it stands: 480 cases, 1310 points, none failed, in 2.7
    s. So where the profile contact's points are found, at one instant, is
    right; the faults found since (the follows within a substep, the
    corners, the event speeds) were each in how a point is carried through
    the substep, not in where it is found.
  - Not yet: the cam contact's point (its curve projection), the 3D
    contacts' points (the separating axis test, the clipping, a cylinder's
    points, the meshes'), checked the same way; each follow within a
    substep (ProfileFollow::place()) against the points found again.
- Built: contacts whose points travel along their surfaces, against
  closed forms -- the faults found since the cam's follow were each a point
  carried through a substep, so scenes where the point moves the whole run.
  - Profile contacts (TestProfileContact): a half disc of radius 10 rocking
    on its arc from 10 deg, its angle against the exact model of one
    freedom integrated by RK4 -- frictionless, its point sliding along the
    arc and the line, to 0.034 deg for a second, every turn within 0.012
    deg of 10, its centre of mass not across by 1e-11 mm (measured again
    2026-11-13: 0.071 deg in the run's steps of 1 ms since a standing
    contact steps as a resting one, step 13 below -- the steps' error, as
    their square: 0.030 at 0.5 ms, 0.020 at 0.25, 0.017 at 0.1, which is
    the disc's sink; the test runs it at 0.25 ms to 0.03, and at 1 ms to
    0.1); on friction of
    0.5 it rolls, to 0.059 deg, its circle's centre where rolling puts it
    to 0.0011 mm, each turn 0.008 deg lower than the last (the stiction's
    slip). A disc of radius 5 swinging 10 deg in a hollow of 25,
    frictionless: its period 4 sqrt(l / g) K(sin(5 deg)) to 4.6e-5, l the
    20 mm and the disc's sink, 0.010 mm.
  - 3D contacts (TestContact): a ball and a roller on their side rolling
    down a slope of 10 deg on friction of 0.5: 5/7 and 2/3 g sin 10 deg,
    to 7e-4 and 6.3e-4, their slip held at 0.02 mm/s.
  - Found: a ball rolling on a level plate gains speed -- 3.1 mm/s^2 at 0.5
    m/s, 5.7 at 1 m/s; a roller 0.92 at 1 m/s. Down the slope the same
    shows as an acceleration too high by 2.4e-4 at 30 mm/s growing to
    2.4e-3 at 460, at either output step. A 3D contact's point is
    collision detection's, fixed on each body through the substep
    (ContactLoad::follow is a cam's or a profile contact's only): the ball
    turns under it, carrying it v h / 2 behind the centre, 2 um at 460
    mm/s with substeps of 10 us, and the normal force there turns the ball
    on -- e / (r tan(10 deg)) of the acceleration, the size measured.
    TestContact's test_ball_rolls_on_level, expected to fail until it is
    fixed -- fixed by the entry below.
  - Not yet: a ball rolling across a hull or a mesh.
- Built: a sphere's and a cylinder side's point found again at each state
  (rollAlong(), as ContactLoad::follow) -- the fault the entry above found.
  At each state the load is given within the substep, the round side's
  point is its surface's farthest along the normal (a cylinder's at the
  point's own place along its axis), the other side's under it, as deep
  as the fixed points are: exact on a face, the force through the ball's
  centre. Two spheres' normal along their centres. A cylinder's cap, the
  normal within 60 deg of its axis as cylinderPoints() takes it, is a
  face: its points stay fixed. Bullet's loads take it when a point of
  theirs is on a sphere or a cylinder, and lose it when none is.
  - Measured: a ball sent at 1 m/s on a level plate -7e-12 mm/s^2 (was
    5.7), a roller 3.9e-4 (was 0.92); down the slope of 10 deg 2e-9 and
    1e-8 off 5/7 and 2/3 g sin (were 7e-4 and 6.3e-4). The tests hold them
    to 5e-3 mm/s^2 and 1e-5; test_ball_rolls_on_level passes.
  - Not yet: the normal turning with the other body within the substep (a
    ball on a tipping plate: the force stays through the centre, its
    direction a substep old) -- built in the entry below; a round part that
    is a hull or a mesh, not a sphere; a cylinder rolling on its rim.
- Built: a ball on a tipping plate, against its closed form -- and the two
  faults it found. A plate turned at 1 rad/s about a horizontal axis in its
  top face, a ball of radius 5 starting on the axis turning with it,
  friction 0.5: along the plate s'' = 5/7 (s w^2 - g sin(w t)), so
  s = 5 g / 12 (sinh(k t) / k - sin t), k = sqrt(5/7), 145 mm by 0.5 s.
  - Found first: the ball rode up to 13 um off the plate from 0.11 s on
    and ended 1.6 mm short; with the normal turned (below), 4 um from
    0.217 s and 0.30 mm. Bullet's manifold kept the ball's older points:
    the ball's point under one rolls off it without sliding and nothing
    breaks it, and each keeps the normal it was found with -- up to 0.9 mm
    from the ball's point, their normals up to 5e-3 rad old, 17400 of them
    handed over in the run. A
    sphere's pair now keeps one point, the other shape's nearest its
    centre: the true one on a convex shape, which every one of Bullet's
    shapes a sphere meets is.
  - Then the normal: fixed in space through the substep, it lags the face
    by omega h. Turning with the face's body (Point::normalOn, the other
    side's frame), the plane through the face's point is the face. Against
    the exact model with the friction law's slip (an RK4 of s, s' and the
    ball's spin, F = -mu(slip) N, the cubic STEP to muStatic at the
    stiction speed), at a stiction speed of 0.01 mm/s, over 0.5 s: turning,
    2.4e-3 mm over at output steps of 1 ms and 6e-5 at 0.1 ms; fixed, 6.3e-3
    short and 3.2e-2 -- worse the finer the steps. At the default 0.1 mm/s
    the slip itself puts the ball 3.6e-3 ahead of rolling; the run ends
    5.9e-3 ahead, at 4 rad/s 1.3e-3 for the slip's 0.9e-3.
  - TestContact's test_ball_on_tipping_plate holds s to 0.015 mm of the
    closed form and the ball within 1e-4 mm of the plate; with the one
    point taken back out it read 0.30 mm and 4.4 um.
  - Not yet: a box or a hull on a tipping plate -- its points fixed on each
    body and the normal fixed in space, which face it belongs to not known
    from Bullet's point (built in the entry below); a round part that is a
    hull or a mesh; a cylinder rolling on its rim.
- Built: a face's normal turned with its body, any shape's. A point of two
  flat shapes -- a box's, a hull's, a mesh triangle's -- takes the body
  whose face the normal is (hasFace(): a face of B's whose outward normal
  is the normal reversed, else one of A's that is the normal; the
  clipping's normal is its face's own, exactly; a triangle's either way);
  an edge against an edge, neither, its normal fixed as before. A cylinder's
  cap on a face, which left rollAlong() before its normal was turned, now
  turns too. The load follows its points (ContactLoad::follow) when one has
  a face or a round shape.
  - A 10 mm cube on the tipping plate, its centre over the axis, turning
    with it. Frictionless it slides from the start, s = g / (2 w^2)
    (sinh(w t) - sin(w t)), 105 mm by 0.4 s: with the normal fixed in space
    3.8e-2 mm short at output steps of 1 ms and 2.8e-2 at 0.1 ms, all but
    not converging; turned, 2.3e-3 over and 2e-4. On friction of 0.5 it
    sticks, creeping by the friction's slip, until tan(theta) passes 0.5
    at 0.46 s and slides off: against the exact model with the slip (the
    ball's RK4, its spin held to the plate's) at 0.6 s, 4.857 mm, fixed
    9.9e-3 short and 3.6e-3, turned 5.4e-4 over and 4e-5.
  - The probe gave the cube its centre's velocity at first, where an
    initial condition's linear velocity is its origin's -- the cube's
    corner: a kick of 5 mm/s, 0.7 um of slide in its first 5 ms. Turning
    with the plate is the origin's (0, 0, 5) there, w x its position.
  - TestContact's test_cube_on_tipping_plate (frictionless) holds s to
    0.01 mm of the closed form; with the faces' turning taken out it read
    3.8e-2. The ball's and the cube's share the scene (tipping()).
  - Not yet: a round part that is a hull or a mesh (measured in the entry
    below); a cylinder rolling on its rim; an edge against an edge on a
    turning body.
- Measured: round parts that are hulls. A solid is a sphere, a cylinder or a
  box only when it is that primitive exactly; any other convex one is the
  hull of its mesh (chord 1e-4 of its size, at least 0.01 mm), any other
  closed one the mesh -- a real wheel, roller or ball with a chamfer, a
  fillet or a hub is a hull. A roller of radius 5 and 20 long, its rims
  chamfered 0.5 (50 facets round), and a ball of radius 5 flatted 0.5 deep
  at both sides, against the closed forms the primitives pass to 1e-8,
  their k^2 their own:
  - Rolling down 10 deg: the roller 12% slow, leaping 0.89 mm off the plate
    at 20 mm/s, its speed rippling 11.6 mm/s; the ball (a mesh then, below)
    1.7% slow. On the level at 1 m/s they lost 130 and 135 mm/s^2 and
    hopped 0.35 and 1.0 mm.
  - The leap: the law flipped between Johnson's line and Hertz's ball from
    one collision detection to the next, at one depth 106 N and 0.68 N on a
    roller of 0.041 N. A hull's curvature was the mesh's fit at the hull's
    point farthest out along the normal, which a facet lying face down ties
    at its corners, and the fits there read 1/7.7 mm round (130 1/m, 0) one
    way and (259, 81) the other -- a ball's. And the line read 38.9 mm for
    19: a facet's four corners, a path end to end, taken for a ring
    (lineLength()) and closed.
  - Built: the solid's own curvature (ContactShape::surfaces): at each of a
    hull's or a mesh's points, each face of the solid it is a node of, its
    outward normal and principal curvatures there by the node's
    parameters (BRepLProp_SLProps), a pole's a hair off it; the backend
    takes the hull's point nearest the contact and its face nearest the
    normal (ShapesOn::curvatureAt()), a mesh's at the triangle's corner
    nearest as before, the fit where there are none. A line closes into a
    ring only when its ends are nearer than half its length.
  - Found on the way: the flatted ball was no hull but a mesh. Its sphere
    cut across its parallels came meshed with diagonals the wrong way by
    the rim, folds 0.11 mm deep at a chord of 0.01, and convexMesh() took
    them for the solid's -- any four points of a sphere fold one way or the
    other. An edge whose two triangles are all on one face of the solid
    convex at each of their corners is now the mesher's; the solid's edges
    between faces and its saddles and hollows are judged as before.
  - Now: rolling down, the roller 2.1% slow and never off the plate,
    rippling 2.2 mm/s; the ball (a hull) 2.1% slow, never off. On the level
    at 1 m/s the roller loses 64 mm/s^2, hopping 0.38 mm, the ball 117,
    0.34 mm. What is left is the facets: a polygon rolling knocks at each
    edge, a 50-gon's about 40 mm/s^2 at 100 mm/s were each knock wholly
    inelastic (an estimate, not measured apart), and past sqrt(g r), 0.22
    m/s here, each edge throws it up -- 0.2 mm at 1 m/s.
  - Tests: ContactShapes.chamferedRollerCurvature and flattedBallCurvature
    (C++), every point's faces against the solid's own; TestContact's
    test_hull_roller_rolls_down, within 4% and never 1 um off the plate.
  - Not yet: a hull rolling as round as it is -- a solid whose touching
    face is a cylinder or a sphere taken as that primitive, or a hull's
    curved faces rolled on their own surface (built in the entry below).
- Built: a hull's or a mesh's round faces rolled as the primitives roll.
  Not the solid taken for a primitive: a wheel's hub, a roller's chamfers,
  a ball's flats and a shaft's flat stay the hull's or the mesh's. Each face
  of the solid that is a cylinder's or a sphere's bulging out (its normal
  away from the axis or the centre at its middle) is a round
  (ContactShape::rounds): its axis or centre, radius, a cylinder's reach
  along the axis and its angles about it from the face's parameters; each
  point's faces (ContactShape::surfaces) name theirs. A point collision
  detection finds on the hull or the mesh is on a round when, at the
  shape's point nearest (a hull's, a triangle's corner), the solid's face
  whose normal is nearest the contact's is a round's, within 0.3 rad
  (roundOn()); the point is then that round's, a sphere or a cylinder on
  the body as a primitive is (ShapeOn::rounds), found again on it at each
  state (rollAlong()) -- and put there as it is handed over, for the laws
  to read the depth off the round, not off the facet a deflection inside
  it. A part of a round only -- a D shaft's cylinder beside its flat -- is
  that only: past its angles roundSupport() leaves the point to the facets.
  Concave rounds -- a bore, an inner race -- are not rounds: a hole's
  contact is not a primitive's.
  - Measured, against the closed forms the primitives pass: the chamfered
    roller rolling down 10 deg -3e-9 (2.1% slow on its facets), the
    flatted ball -9e-9 (2.1%), a ring -- a bore of 2 through the roller,
    a mesh -- 2.8e-5 with k^2/r^2 0.58; all three on the level at 1 m/s
    keep 1000.0000 mm/s and never leave the plate (the roller lost 64
    mm/s^2 and hopped 0.38 mm, the ball 117 and 0.34). The roller's run
    took 0.1 s of wall time where its facets' took 0.5. A D shaft rolls
    on its round at the axis's height, dips over its flat, and rests on
    it.
  - Tests: TestContact's test_hull_roller_rolls_down to 1e-4 (rollRound()),
    test_mesh_ring_rolls_on_level to 5e-3 mm/s^2, and
    test_d_shaft_rests_on_its_flat within 2 um of its flat.
  - Found, not explained: the D shaft rests 0.97 um low on its flat where
    its law's sink is 39 nm (k 5.39e9, e 1.5, its four points 38.6 nm
    deep) -- its hull lies about 0.93 um inside the solid's flat. The same
    with the rounds taken out; a hexagonal prism's hull rests at its law's
    sink (test_hull_rests). Not chased.
  - Not yet: a pin in a bore -- a concave round (built in the entry below);
    two rounds of hulls meeting, a cylinder's against a cylinder's (as two
    primitive cylinders: their normal stays the one found); a round whose
    face is trimmed across its parameters, its angles the face's box (the
    flatted ball's sphere: the point there is on the flat's face anyway).
- Built: a pin in a bore, a ball in a bore or a socket -- hollow rounds.
  - Measured first: a pin of radius 4 let go 10 deg off the bottom of a
    bore of radius 10 through a block (not convex: a mesh). Set touching
    the bore, as a designer mates it, it started inside the bore's facets,
    which lie up to the mesh's deflection (0.01 mm) inside its true
    surface: 10 um into a stiff line contact, it was thrown out at about a
    kilometre a second. Set 0.02 mm clear, it landed, bounced 0.8 mm, and
    was pushed out of the hole along its axis at 300 mm/s: Bullet's normals
    against the bore's long thin facets leaned along it.
  - A cylinder's or a sphere's face whose normal points to its axis or
    centre is a hollow round (ContactShape::Round::concave; a mesh's only,
    a hull has none); roundSupport() takes its surface facing 'outward'
    across the axis or the centre. A round inside a hollow one -- a
    primitive cylinder or sphere, or a convex round face -- on two bodies a
    load joins is a round pair (ContactHandover::RoundPair), and its points
    are the two surfaces' own (hollowPoints()): a cylinder's at both ends
    of its stretch within the bore's reach, a ball's at its centre, each
    along the bore's radius through it; none outside the bore, nor across
    it (a pin over 25 deg off its axis), nor past a part round's angles.
    Bullet's points between the inner shape and the bore's triangles are
    dropped, however their normals lean -- one 18 deg off took no round
    and stood 75 um deep on its facet, a 3000 N knock. The normal is along
    the hollow one's radius through the inner one's axis or centre
    (rollAlong()); the law reads the two rounds' own curvature
    (roundCurvature(), the hollow one's negative: a pin of 4 in a bore of
    10 conforms as a line of radius 1/(1/4 - 1/10)).
  - Then the rounds' curvature broke a cylinder standing on its cap: read
    off the side wherever the normal was not its axis exactly, the cap
    took a ball's law and test_cylinder_on_its_cap ran 118 s for 1. A
    cylinder's cap is flat within 60 deg of its axis, as curvatureAt()
    has it.
  - Measured: the pin rolling, its period 0.190715 s against 0.190708
    (3.8e-5), its centre within 2.2e-7 mm of 6 from the axis; sliding
    (frictionless) 0.155720 against 0.155712 (5e-5); a chamfered hull
    roller as the pin 3.5e-5; a ball rolling across the bore 1.4e-4. The
    swing loses about 0.1% of its amplitude a period, the law's damping as
    the load rises and falls through it. The hull roller's run takes 8.4 s
    for 1 (the primitive's 1.3): Bullet still clips the hull against the
    mesh's triangles, whose points are then dropped.
  - Tests: TestContact's test_pin_rolls_in_bore and test_pin_slides_in_bore
    to 2e-4, test_ball_rolls_in_bore to 5e-4 (swingInBore()), each within
    1 um of its radius; ContactShapes.boreIsAHollowRound (C++).
  - Not yet: Bullet left out of a round pair's narrowphase, not only its
    points dropped (the hull roller's time; built in the entry below); a
    pin's other faces -- its ends, its chamfers -- against the bore's wall
    when it tilts, which are dropped with the rest; a cylinder in a
    socket.
- Built: Bullet leaves a round pair's shapes out. A round pair's inner
  shape and its hollow face's triangles are skipped before their narrowphase,
  not only their points dropped. The hull roller in the bore spent 6.3 s of
  its 7.8 s run in Bullet, clipping its 280-point hull against each
  triangle; the dropping stays, in case Bullet still reports any.
  - How: Bullet asks gCompoundChildShapePairCallback (and the
    compound-compound one) about each child pair of a compound before
    running it. ContactCollisionSystem points them at collides(), which
    refuses a shape in `inner` against a triangle in `hollow` under the same
    round pair's number. The answer is this system's only while its own Run()
    is on the stack (a thread_local), so another Chrono system in the process
    is untouched. addPolyhedralFeatures() records the Bullet shapes each
    Chrono shape made (`made`): a mesh's triangles in order, any other's one.
  - Measured, over a second: the hull roller's run 7.8 s -> 1.1 s (Bullet
    6.27 s -> 0.04 s), the primitive pin's 1.3 s -> 1.1 s. Each period is
    the same to the last digit printed (roller 3.536e-5 off, pin 3.790e-5,
    ball 1.393e-4).
  - Tests: TestContact's test_roller_rolls_in_bore, the chamfered roller
    (a hull) to 2e-4, now fast enough to run (swingInBore("roller")).
  - Not yet: a pin's other faces against the bore's wall when it tilts
    (the entry below); a cylinder in a socket.
- Built: a pin tilted in a bore, and the pairs that are no round inside a
  hollow one.
  - Found: a pin of 9.9 hanging 30 out of a bore of 10, 10 in, went
    through the bore's wall. rollAlong() turned every point of a round pair
    to the bore's radius through the pin's middle, outside the bore and
    below its axis, so the point where the inner end's top rim pressed on
    the bore's top was turned onto the pin's underside. Now each point takes
    the radius through the pin's axis where the point is along it.
  - Then it rested 4.4 um into the mouth's bottom edge, 2.2% too steep: on
    the bore's radius, the mouth's point is the pin's side square to the
    bore, r t^2 off its side square to the pin at a tilt t. hollowWay(),
    which both hollowPoints() and rollAlong() use, takes a point beyond the
    bore's end from the end's rim, the edge pressing on the pin's side: the
    way from the pin's axis, square to it, to the rim's point. A ball beyond
    the end takes the same, from its centre.
  - Found on the way, a regression of the round pairs: every convex round
    was paired with every hollow one on the other part, and Bullet's points
    between them dropped. A ball of 6 on a hole of 4, a check valve's seat,
    had no points of its own (too big to go in) and fell through the block.
    A pair now needs the inner one to fit (fitsIn(): a ball or a cylinder
    narrower than the bore, a ball smaller than a socket). A pin more than
    25 deg off a bore's axis has no points of the rounds', and Bullet's are
    its again, both left in and not dropped (inLine();
    ContactCollisionSystem::live, refreshed as each of its runs starts).
  - Measured: the hanging pin's tilt 1.17105 deg against the exact
    1.17053 (4e-4), the mouth's edge 54 nm into it and its rim 34 nm into
    the bore, each the contact's sink; a chamfered roller rests on its
    chamfer's edge, 1.20927 deg against 1.2079 for its side 0.3 shorter.
    The ball on the hole rests centred from 1 mm off, 4.4 um over sqrt(20)
    (the rim's facets, as before the round pairs); one of 3.9 clipping the
    rim of the hole of 4 is pushed into it and falls through. The pins and
    the ball swinging in the bore are unchanged.
  - Tests: TestContact's test_pin_hangs_out_of_bore (its tilt to 1e-3,
    both contacts within 1 um) and test_ball_sits_on_hole (centred, within
    0.01 mm).
  - Not yet: a short pin lying across a bore, on its end rims, is thrown
    off at kilometres a second -- before the round pairs too, a primitive
    cylinder's rims on a mesh's curved facets (the entry below); a cylinder
    in a socket; a ball on a hole too small for it rests on the rim's
    facets, Bullet's (4.4 um high), not on the rim itself.
- Built: a primitive cylinder on a mesh, and a pin lying across a bore.
  - Found: worse than a bore -- a pin of 2 by 6 dropped on its side onto
    the flat top of an L-shaped block (a mesh) rocked and was thrown off
    hundreds of millimetres. Bullet's GJK and EPA on a triangle, which has
    no inside, give a cylinder one point a triangle: lying on its side, one
    point at one end. A box's or a hull's face had cylinderPoints()'s
    clipping; a triangle had none. cylinderTrianglePoints() clips the
    cylinder's cap rim (a polygon of 16) or its side's segment to the
    triangle, each point within reach of its plane, the triangle's normal
    turned toward the cylinder.
  - Across a bore, Bullet's points on the facets came and went: it read the
    rims 24 um off the facets where the true wall had them 0.1 um in, lost
    them all while they pressed, and the next substep, with no point to
    limit it, was 0.9 ms after one of 0.8 us -- HHT took the last
    substep's contact accelerations into it and threw the pin at 1.6 m/s.
    Two fixes. A cylinder across a bore, both caps within its length, has
    the rounds' own points too (held()): along any line on the cylinder
    the distance from the bore's axis is convex, so its deepest points are
    on its rims, each rim's farthest from the axis (rimFarthest(): the
    rim's point along the radius, the radius through that point, again,
    till they agree), along the radius through it -- through the cap's
    centre instead it sank 27 um. And a load pressing as a substep ends
    that collision detection then finds nothing of keeps the next substeps
    short, growing back by two.
  - Then it rested exactly but ran 74 s for 0.3 s: at 0.6 g its rims sink
    0.17 nm, it chattered from 0.4 nm off to 0.1 nm in, and a load had to
    press to rest. Resting is now its deepest point within the run's
    tolerance of touching (1 nm by default): 50 s. The rest is a see-saw
    the pin keeps up on its two rims, 0.01 rad/s about the vertical with
    30 um/s along the bore -- a light part's stick-slip on stiff contacts
    and the regularised friction, at most 1 nm deep, that never settles.
  - Measured: across the bore, its centre 0.07 nm from 2 - sqrt(91) at
    0.1 s, 1.6e-5 mm by 0.3 s; on the L-block, dropped flat or tipped
    10 deg, it lies on its side at 22.000000 mm, level. The hanging pin,
    the seat and the bore swings unchanged.
  - Tests: TestContact's test_pin_lies_across_bore (to 0.04 s, its centre
    to 1e-4 mm) and test_pin_lies_down_on_mesh (tipped 10 deg, to 0.05 s:
    on its side to 1e-4 mm, level to 0.01 deg).
  - Not yet: the see-saw's time -- a light part chattering on stiff
    contacts takes substeps of a fiftieth of the contact's period (0.3 us
    here) for as long as it lasts (the entry below); a cylinder across a
    bore with a cap out of it keeps Bullet's points, now clipped to the
    facets.
- Built: a light part at rest in steps as long as the run's -- the see-saw's
  time. The see-saw was neither stick-slip nor friction's: the pin lying on
  the L-block's face, frictionless, kept it up the same, at 2000 substeps a
  millisecond. Held at the strict substeps throughout, its rattle died
  within a millisecond, its depth flat at its sink, 0.055 nm. It was the
  steps over its rest.
  - Found: HHT takes the accelerations it is given into a step, (1 - gamma)
    h of them into the velocities. The pin rattling passes through touching
    every 15 us, falling freely there; a step of 0.99 ms from such an
    instant carried -g into it and ended 23 nm up, and the landing, found
    late, cut the steps short again. Taken to rest while it rattled, the
    pin rattled on: the steps over it pumped the rattle up to whatever rest
    allowed. A range of two tolerances, steps that moved a load at rest
    taken again an eighth as long, a landing late only from a tolerance off,
    steps at rest held to a period, steps growing by twice the last: each
    moved the rattle to its new bound, or changed nothing. Its
    accelerations set to none before each step at rest cured it, which for
    a body that does accelerate is a step of the first order.
  - Built: a load at rest that a step does not resolve -- one longer than a
    fiftieth of its period -- starts it with its points' normal
    accelerations those of its bodies' mean accelerations, over the last
    five of its periods or over the last substep when that was longer: the
    rattle's average, none. The least change to the bodies' own
    accelerations that does it, in the mass's measure, solved over the
    load's points (a line of them along a pin's side is short of rank: the
    pseudo-inverse), and only to bodies on no joint (meanAccelerations()).
    Each body's velocities are kept at the end of each substep (Moment),
    going on only from the state they last left: a stop's bounce, a
    sensor's step taken back or a part put to rest starts them again.
  - Found on the way: the bodies' mean accelerations whole, not only the
    normal's, lag what moves them -- over a long step its secant, half a
    step behind -- and the ball on the tipping plate ended 0.10 mm off, and
    eight of TestProfileContact's failed; the change given to a body on a
    joint too, which holds its accelerations as well, put a cam follower's
    contact force 0.014 N off of 270. A mean carried on by its own change
    to the window's end amplified the noise; the change left out where it
    moves a point less than the tolerance over the step let the rattle
    back; given only on the step after the strict ones, the pins rattled
    on.
  - Measured, over 0.1 s: the pin on the L-block's face 9.6 s -> 0.4 s
    (108513 substeps before), across the bore 10.5 s -> 0.7 s (124807);
    each, from its first rest, at its sink, 0.0553 and 0.0578 nm deep,
    where it rattled up to 5e-5 mm. TestAssemblyWorkbench: 606 OK in
    143 s (167).
  - Tests: TestContact's test_pin_lies_across_bore and
    test_pin_lies_down_on_mesh now to 0.1 s, each still within 1e-8 mm
    from 0.06 s (stillFrom()).
  - Not yet: a light part at rest on a joint -- a lever lying on a stop --
    keeps the rattle's substeps; a cylinder across a bore with a cap out of
    it.
- Built: a ball on the rim of a bore too narrow for it, a check valve's on
  its seat (the pin's tilt's "not yet" above). It rested on Bullet's points
  on the facets: the rim's polygon, inside the circle by the polygon's
  sagitta, and the ball of 6 on the hole of 4 stood 4.4 um high.
  - A ball, a primitive's or a round face's, on one body of a load and a
    bore all round that it does not fit (fitsIn()) on the other are a seat
    (ContactHandover::SeatPair). When the ball's centre is past one of the
    bore's ends, its points are on that end's circle: the rim's point
    nearest the ball's centre and fifteen more round it at equal turns
    (seatAt()), each within reach, the normal along the line from the
    rim's point to the centre -- centred, the ball touches it all round.
    Each is found again at each state at its turn from the nearest
    (Point::seat, rollAlong()). Bullet's points between the ball and the
    bore's mesh within twice the rim's sagitta of the circle are dropped:
    on the bore's facets and on the face around the hole, they lie on the
    polygon. The sagitta is the mesh's own, from its widest turn between
    vertices on either end's circle (rimSagitta(): 4.97 um here, the
    deflection's 0.01 mm bounding it).
  - The law reads the ball's curvature against the rim's none, a ball on
    a flat; a sharp edge's Hertz contact has no radius of its own.
  - Measured: dropped 1 mm off the axis it rolls onto the rim and rests
    centred to 1e-6 mm, 63 nm under sqrt(20) -- its sink, 47 nm along the
    16 normals -- where it stood 4.4 um over; set centred, the same.
    Touching one side of the rim its centre is on a cone about the axis,
    rising 4 / sqrt(20) for each mm off it (41.8 deg), so rolling down it
    the ball meets the far side at the centre nearly head-on, at 84 deg: it
    hops 0.27 mm and is centred to 1 um by 66 ms. On the facets it rolled
    across the centre and up the far side, 0.47 mm off, and was still
    0.01 mm off at 0.3 s.
  - Tests: TestContact's test_ball_sits_on_hole to 1e-4 mm (was 0.01).
  - Not yet: a ball on a chamfered or rounded mouth -- the chamfer's cone,
    the round's torus, are Bullet's facets; a cylinder's rim on a bore's
    rim (a pin standing in a hole too narrow for it).
- Built: friction by the pair of materials (P6, the material-pair table), the
  last of D2's deliverables (sec 4). One scale, clean and dry in air, from
  one source: the Engineering ToolBox. Steel on steel is 0.50 / 0.42 there,
  above the sets' own 0.30 / 0.25, which stay for a pair the table has not.
  - The table is data: `Mod/Assembly/ContactFriction.json`, and a user's of
    that name in `<user data>/Assembly` read over it -- its families tried
    first, its pairs in place of the module's (FrictionTable). A family is
    words: a component's card (its first shape's that has one; the Default
    card is none) is in the first family one of whose words its name, its
    KindOfMaterial or one of its tags holds. The cards' Appearance parents
    are no guide -- PC and PET inherit Acrylic's, Copper-230, a red brass,
    Bronze's -- their tags are: "Brasses", "Phosphor Bronze", "Aluminum
    Bronze" (a bronze, so brass and bronze are tried before aluminium).
  - Its 21 pairs, each with what the source gave, static / dynamic: steel
    0.50 / 0.42; on steel, aluminium 0.61 / 0.47, brass 0.51 / 0.44, copper
    0.53 / 0.36, nylon 0.40, acrylic 0.40, bronze 0.35, cast iron 0.23,
    PTFE 0.05, graphite 0.10; on cast iron, copper 1.05 / 0.29, brass 0.30,
    bronze 0.22; each on itself, copper 1.6, aluminium 1.05 / 0.40, glass
    0.90 / 0.40, acrylic 0.80, wood 0.25, nylon 0.15, graphite 0.10, PTFE
    0.04 / 0.04. One coefficient given is the static one, the dynamic five
    sixths of it (the source's own steel on steel, 0.42 / 0.50); a range,
    or two rows, its low end. Not in it: stainless steel and titanium, which
    the source has not; restitution, which the speed sets more than the
    pair.
  - The first table (2026-11-03, the same day) had the metals from Deulin,
    Mikhailov, Panfilov and Nevshupa's friction in air (tribonet.org) --
    steel 0.30, the sets' default -- and the plastics on steel from the
    ToolBox, clean and dry. The two scales did not mix: nylon on steel, at
    0.40, slid harder than steel on steel, and the manual's video showed
    it. Most sources have dry steel on steel at 0.5 to 0.8; a nylon's
    datasheet (Ertalon 6 SA) has it on steel at 0.4 to 0.6 dynamic.
  - The set: `FrictionFrom` {Materials, Value}, a new set's Materials, a cam's
    and a profile's Value (frictionless as given), a set saved before it
    Value. The builder gives each contact shape its component's family
    (ContactShape::family, Dynamics::families) and the model the table's
    pairs of those (Dynamics::frictionPairs); a load takes its first two
    shapes' pair's coefficients (ContactLaw::FrictionFrom), the set's own
    where the table has not the pair or a part has no card in a family --
    which the build says, once per set and pair, with the coefficients
    taken. The panel's "Friction from" picks it.
  - Found on the way: two copies of the dynamics block field by field --
    the builder's (addDynamics()) and the rigid groups' (RigidGroups.cpp) --
    dropped the new fields, and the run took the set's 0.25 for PTFE. And a
    card's name is the Material manager's, its file's -- PA6-Generic, not
    the "Polyamide 6" inside it: the manual's video found nylon sliding on
    steel's 0.25 until "PA6" was one of its words.
  - Tests: TestContact's test_friction_from_materials, a block sliding on a
    steel plate at 200 mm/s, its friction over its normal force: PTFE 0.04,
    steel 0.42, Copper-230 0.44 (a brass by its tags), PA6 0.33, wood the
    set's 0.25, PTFE by Value 0.25, each to 1e-3; ContactFriction.* (C++):
    FreeCAD's own cards in their families, pairs either way round, every
    pair of the table's families with dynamic <= static, steel over nylon
    over PTFE on steel, a missing file nothing.
  - Not yet: per-pair restitution; a component of several materials takes
    its first shape's family; the panel does not show the pairs the
    materials give before the run.
- D2 is built (2026-11-03).
- **D4a, loads to FEM: the plan (2026-11-04).** A component at a frame of a
  run, as a static stress analysis in the Fem workbench, solved by CalculiX.
  - How the others do it. SOLIDWORKS (Import Motion Loads) puts each mate's
    reaction on the mate's load-bearing faces, chosen before the run, adds
    gravity and the angular velocity and acceleration as body loads, and
    switches on inertial relief; several frames make a design study.
    Inventor (Export to FEA) does the same per joint, its faces picked
    automatically, one part and one frame per analysis, held by "detect
    and eliminate rigid body modes". Creo puts each joint's, spring's,
    motor's and cam's load at its point (a total load at a point), gravity
    with the translational acceleration, and the turn as centrifugal
    loads; inertia relief is the analysis' option. Adams exports joint and
    applied forces with the accelerations and velocities, a step per
    frame, for Nastran or Ansys (inertia relief, weak springs).
  - What CalculiX 2.23 has (its manual, all of it searched): no inertia
    relief; `*DLOAD` GRAV, any acceleration on an element set or one
    element; CENTRIF, omega^2 about an axis, one per element set in a
    static step; no angular acceleration and no Coriolis body load. A
    remote load is `*COUPLING` with `*DISTRIBUTING` (Nastran's RBE3: the
    load spread, the faces not stiffened) or `*KINEMATIC`, or `*RIGID BODY`.
    FreeCAD's FEM writes GRAV (ConstraintSelfWeight, all elements),
    CENTRIF (ConstraintCentrif, a frequency about an edge) and `*RIGID BODY`
    (ConstraintRigidBody: a force and moment at a reference point, the
    faces made rigid); no distributing coupling, no angular acceleration,
    no inertia relief.
  - The loads on a component at a frame, its free body: each joint's
    reaction at its frame I's origin (on the body of frame I as the run
    reports it; on the other, minus that, the torque about the same
    point), each spring's and bushing's force at its end, gravity, and the
    inertia: the mass at the centre of mass with its acceleration a, the
    turn omega and its rate alpha. A body load per unit mass at r from the
    centre: g - a - alpha x r - omega x (omega x r). The run does not
    report a, omega or alpha yet; contacts report a set's force, not
    where it acts.
  - How it is held. With no inertia relief, the part is held at one joint
    -- the one with the largest reaction, or the user's -- by its coupling's
    reference node fixed in all six directions, that joint's own load left
    out. One node's six is statically determinate: if the loads balance,
    the support carries just the left-out reaction, and what it carries
    beyond that is the imbalance, reported as the check (the run's
    reaction against the analysis'). A distributing coupling, so neither
    the support nor the loads stiffen the faces they act on.
  - Steps, each shipping:
    1. The run reports each body's motion: "Acceleration" (its centre of
       mass's, mm/s^2), "AngularVelocity" (rad/s) and "AngularAcceleration"
       (rad/s^2), model axes, width 3, from Chrono's bodies (their frames
       are at the centres). Tests: a pendulum's centripetal acceleration,
       a body falling at g, a wheel spun up at a known torque.
    2. `AssemblyObject.getLoadsOn(component, frame)`: the free body above,
       in the component's own coordinates at rest (the frame's pose undone),
       each load with its source, point and the faces it acts on, and the
       Newton-Euler residual (sum of the loads less m a, and of the moments
       less the rate of the angular momentum) as the check. A joint's faces
       are those of its reference on that component: a face as is; an edge,
       the faces along it that share its axis (a hole's bore for a circle);
       a vertex, none (a point load at the node nearest). Tests: the
       slider-crank's rod, the residual below 1e-6 of the largest load.
    3. FEM, our fork: ConstraintRigidBody's `Coupling` {Rigid, Distributing},
       Distributing writing `*COUPLING` with `*DISTRIBUTING`; and an inertia
       load (`ConstraintBodyMotion`: a point, a, omega, alpha), written as one
       GRAV for g - a, a CENTRIF about the axis of omega through the point,
       and alpha x r as each element's own GRAV at its centroid. Tests, with
       ccx: a bar spun about one end, its stress rho omega^2 (L^2 - x^2) / 2;
       a bar spun up from rest, its bending against the closed form.
    4. The command (Assembly_LoadsToFem, from the Simulation panel's
       Results box at its cursor): a Fem Analysis of the component at that
       frame -- the component's shape, its material, a Netgen mesh, a
       coupling per load on its faces, the body motion load, the held
       joint -- solved on request. Test: the slider-crank's rod at its
       largest load, the held joint's reaction in the analysis against the
       run's within 1%.
    5. Several frames, one analysis each, by the Results box's range;
       contact loads once a contact reports where it acts.
  - CalculiX on this box: conda-forge's calculix 2.23 in `.conda/fem`
    (with gmsh 4.15), apart from `.conda/freecad`, whose pinned stack it
    would move; its MinGW DLLs (`Library/mingw-w64/bin`) must be on PATH.
    The release packages need it as a dependency.
- Built, D4a step 1: the bodies' motion in the run's results. Each moving
  body reports "Acceleration" (its centre of mass's, mm/s^2), "AngularVelocity"
  (rad/s) and "AngularAcceleration" (rad/s^2), model axes, width 3, after
  the other channels; the panel shows them in m/s^2, deg/s and deg/s^2, and
  the CSV and spreadsheet exports carry them. A dynamic run's start takes
  the accelerations as it takes the reactions (the line through the 32nd
  and 64th smallest steps); a kinematic run's reactions bring them from
  the motions' differences.
  - **The accelerations are M^-1 (f + Cq' L), not the integrator's.** HHT's
    own acceleration is of forces blended across the step (alpha -0.2): on
    the pendulum released from horizontal its centre's was off the closed
    form by 0.07% of g at 10 ms, growing to 0.8% at the bottom, while the
    hinge's reaction was within 1e-4. From the forces the step ended with,
    gravity, loads and the reactions reported: within 1e-4 of g too, and
    the angular rate within 3e-4 -- what makes a free body's loads balance
    (step 2's check).
  - Tests: TestDynamics' test_body_motion (the pendulum's acceleration,
    turn and rate against the closed form at each frame) and
    test_body_motion_kinematic (driven time^2 about the hinge: 2 t rad/s at
    2 rad/s^2, its centre 50 mm out); the CSV's columns; the C++ redundancy
    test counts the body channels after the reactions.
- Built, D4a step 2: a component's free body at a frame
  (`MotionLoads.loadsOn(assembly, component, frame)`, Python). In the
  component's own coordinates (its Placement's, the frame's pose undone),
  the model's units (mN, mN mm): each joint's reaction at its frame I's
  origin -- as reported on frame I's body, its opposite on the other --
  with the joint's reference on the component, for step 4's faces, and
  whether it is unique; each spring's force at its end, along the line
  from the other end; each bushing's force and torque at its frame, its
  opposite on its Other; the mass, centre, inertia, gravity, and the
  motion step 1 reports (none for a grounded part). Residual: the loads
  with m g less m a, and their moment about the centre less I alpha +
  omega x I omega. Missing: what acts on it that it does not take yet --
  a contact set it is in (a set reports no point of action), a torsion
  spring on its joint, an applied load (no channel). A joint's reaction
  already holds its friction and its stops'; it was taken to hold its
  motor's effort too, which it does not (step 5's second half). The
  parts are put back as they were.
  - **The mass properties read shapes through the assembly's shape cache**,
    which kept a component where it stood when a script wrote its
    placement and purged it -- as the player putting the parts back does:
    the second rod's centre was taken at another frame's pose, 126 mm off,
    its moment 2381 mN mm out of balance. `massPropertiesOf` now clears the
    cache first. The Mass Properties table after a playback read the same
    stale shapes.
  - Tests (TestDynamics): the pendulum's rod, a double pendulum's two rods
    (the first between two hinges, the first reference of one and the
    second of the other), the oscillator's block on its slider and spring:
    each free body balancing within 1e-6 of its loads at frames across the
    run, the hinge at (0, 5, 5) on the rod, the spring's force k (4 -
    length); TestMassProperties' test_moved_by_script.
- Built, D4a step 3: the FEM side (our fork of the Fem workbench).
  - Measured first, on ccx 2.23 by hand (a bar of bricks, 100 x 10 x 10):
    a force or moment through `*COUPLING` with `*DISTRIBUTING` loads the
    face as a plain clamp's load does (the bricks' bending 4% stiff, as
    full integration is); a distributing coupling as the support -- its
    reference node fixed -- gives 1e11 mm, nonsense: CalculiX takes no
    constraint there. `*KINEMATIC` does not parse in 2.23. `*RIGID BODY`
    as the support matches the plain clamp exactly. GRAV lines add, on
    all elements and on one, in any directions; CENTRIF adds to GRAV. And
    a distributing coupling's reference node is reported unmoved: its
    faces' nodes are what moves.
  - ConstraintRigidBody's `Coupling` {Rigid, Distributing}. Distributing
    writes its faces as an element face surface, `*COUPLING, REF NODE,
    SURFACE` with `*DISTRIBUTING` over 1 to 6, its forces on the reference
    node's degrees of freedom 1 to 3 and its moments on 4 to 6 (no
    rotation node); the output prints its faces' nodes' displacements. The
    checks refuse a distributing coupling with a mode Constraint: hold
    the part with a rigid one.
  - ConstraintBodyMotion (Python, `ObjectsFem.makeConstraintBodyMotion`):
    ReferencePoint, Gravity, LinearAcceleration (the point's), and
    AngularVelocity and AngularAcceleration, the document's axes, mm and
    s. Written as one GRAV on all elements for g - a, one CENTRIF of
    omega^2 about omega's axis through the point, and a GRAV on each
    element of - alpha x r at its centroid: the resultant exact, each
    element's own moment about its centroid left out. It needs the
    density, which the checks ask for.
  - CalculiX on this box: `.conda/ccx`, ccx.exe with the MinGW and MKL
    DLLs it loads copied beside it, so it runs alone -- `.conda/fem`'s
    `Library/bin`, where they came from, also holds gmsh's Qt, which must
    not reach FreeCAD's PATH. The FEM finds it by its preference or on
    PATH.
  - Tests (`femtest/app/test_motionloads.py`, TestFemApp; skipped without
    ccx): a steel bar of second order tetrahedra (Netgen, 2.5 mm) held at
    x = 0: 100 N across its far face through a distributing coupling bends
    it F L^3 / (3 E I) + F L / (k G A) within 1%, the clamp taking 100 N;
    1e4 N mm about z turns its tip up M L^2 / (2 E I) within 1%; a
    distributing coupling held is refused; turning about its clamped end
    at 100 rad/s and 1e4 rad/s^2, gravity cancelled by the acceleration,
    the clamp takes rho A L^2 / 2 omega^2 along -x and alpha along +y
    within 1%, nothing along z.
- Built, D4a step 4: loads to FEM. `MotionLoads.makeAnalysis(assembly,
  component, frame, hold=None, maxSize=None)` and the Simulation panel's
  Results box "To FEM...", which asks for the part (those whose motion the
  run reports) and takes the frame shown. It makes, in the document:
  - a copy of the part's shape (`Part::Feature`, "<part> at <t> s") where
    the part stands now -- at rest, or at the frame the player shows: the
    loads are in its own coordinates and go wherever it is put;
  - an analysis with a CalculiX solver (static), the part's material --
    its card's elasticity (the default material's where it has none) and
    the density that makes the run's mass, an override's too -- and a
    Netgen mesh, second order, a fortieth of the shape's diagonal unless
    given;
  - each load through a distributing coupling at its point, on its faces:
    a joint's, those of its reference on the part (a face itself; a
    circular or straight edge, the cylinders and cones along it on its
    axis -- a hole's bore for its rim -- or every face along it); a
    spring's or bushing's, the face nearest its point;
  - the part held at one joint, the given one or the one with the largest
    force, through a rigid coupling fixed in its six directions, that
    joint's load left out;
  - the body motion load: gravity, the centre of mass's acceleration, the
    turn and its rate.
  The panel says what is held, what the free body is missing (contacts,
  torsion springs, applied loads) and that a redundant joint's load is one
  split of it; the user runs it from the FEM workbench.
  - **What CalculiX reports holding the part leaves out the body load on
    the held face's own nodes**, which goes into the coupling without
    passing through the part: on the double pendulum's first rod 0.6% of
    its weight at a 4 mm mesh, 0.3% at 2 mm (first order, the face's share
    of the mass); the hold's moment off by 1e-3 of force times length at
    4 mm, a quarter of that at 2 mm. The stresses are not touched by it.
    Otherwise the analysis balances as the run does: the hold carries the
    run's reaction there, within 0.2% at the swing's middle and end.
  - Tests: TestDynamics' test_loads_to_fem: the double pendulum's first
    rod at 80 ms -- the couplings, Rigid at the more loaded hinge's end
    face and Distributing at the other's, the body motion load, the mesh;
    solved where ccx is found, the hold's force within 0.5% of the run's
    plus 1% of the weight, its moment within 0.5% of force and weight
    times 100 mm. Skipped without ccx.
  - The manual's video (`assembly/simulation/loads-to-fem`): the double
    pendulum's first rod at each of its 81 frames, 5 ms apart, solved, its
    largest von Mises stress in the middle third against beam theory from
    the same free body (N and M from the hinge forces and rho A (g - a(x))
    along it; |N| / A + |M| c / I): within 1% wherever it is over a fifth
    of its peak, 96 kPa at 175 ms.
  - Found by the video: the FEM mesh view's `NodeColor`, set as a dict,
    read back but was not drawn; `setNodeColorByScalars` was (its scale 0
    to the largest value given). Fixed in FEM: a dict picks its
    `ColorMode` (it drew only in `ByNode`, the default being `Overall`),
    `resetNodeColor` draws in every mode, and `setNodeColorByScalars`
    takes an optional scale, reads no colour past its list for nodes not
    given, and refuses lists of different lengths
    (`femtest/gui/test_meshcolor.py`, run by
    `tests/gui/fem-workbench-suite.py`).
  - Not yet: see step 5's.
- Built, D4a step 5 (first half): a range of frames, solved, its stresses
  in the Results box. `MotionLoads.solveFrames(assembly, component,
  frames, hold=None, maxSize=None, progress=None)`; "To FEM..." asks for
  the part and either the frame shown (step 4's analysis, to run) or
  frames from, to and every, solved now behind a progress dialog with a
  Cancel.
  - One analysis for every frame: the loads are in the part's own
    coordinates, so the copy, its mesh, the couplings on their faces and
    the body motion load are made once (`_build`) and each frame's values
    set into them (`_setFrame`), written and solved in turn. Held at one
    joint throughout: the given one, or the one with the largest force at
    any of the frames. Then solved once more at the frame of the largest
    stress, so the analysis is left there with its FEM result (the others'
    are read from CalculiX's files, not kept: 81 result meshes would be
    81 copies of the mesh).
  - Each frame's largest von Mises stress (MPa, from the .frd's nodal
    stresses), the node it is at (in the part's coordinates), and the
    check -- what holds the part in the analysis (the rigid coupling's
    reaction in the .dat) against the run's reaction at that joint -- go
    into a `StressSeries` (App::FeaturePython, MotionLoads) in the
    analysis, kept in the document. It also keeps the held joint's force
    channel at each frame as the run reported it: the series is a channel
    of the run's results (`stressChannels`, Quantity "VonMises",
    "Largest von Mises stress") only while the run is the one it was
    taken from -- the same study, the same times, those forces within
    1e-9 -- so a run again with other loads drops it rather than plotting
    stresses of another run.
  - The Results box plots a channel with gaps (None at a frame not
    solved): a line through each run of values, a dot for one alone; its
    value line says "not solved at this frame"; the CSV leaves the cell
    empty, the spreadsheet the cell blank.
  - **Found on the way, the Results box lost the channel chosen on every
    new run**: it looked the key up with `QComboBox.findData`, which
    matches a Python object stored as item data by identity -- and the key,
    a (name, quantity) tuple, is made anew on each fill. Probed: findData
    of the stored tuple 0, of an equal tuple made anew -1, of a list -1.
    `resultIndex` compares the keys. The same bug kept "To FEM..." from
    showing the stress it had just solved.
  - **Found on the way, in FEM: `ccxtools.start_ccx`** set the solver's
    threads with `os.putenv`, passed its None as the child's environment
    (so ccx took FreeCAD's, holding what putenv wrote only by luck), left
    OMP_NUM_THREADS set in FreeCAD after the run when it was not before,
    and changed FreeCAD's working directory for the run, not restored when
    it raised. Now the child gets its own environment and working
    directory, and starts hidden on Windows, as the file's other
    subprocesses do.
  - The double pendulum's first rod at its 81 frames (the manual's video,
    `assembly/simulation/loads-to-fem-frames`): 92 s, about 1.1 s a frame
    at the default mesh (a fortieth of the diagonal, 2.5 mm). Its largest,
    96.41 kPa at 175 ms at x = 54 mm, is the per-frame video's middle-third
    peak; at the 66 frames where the largest is in the middle third, the
    two agree within 0.14% (2.5 mm against 3 mm meshes). The hold within
    0.071% of the largest reaction at every frame (0.43% of the rod's
    weight: the held face's own share of the body load, step 4).
  - **The largest over the whole part takes in what the couplings do at
    their faces.** At 7 of the 81 frames (0.19 to 0.195 s and 0.29 to
    0.31 s, 0.03 to 0.06 MPa, where the rod's bending is small) it is on
    the held face, at x = 0, where the rigid coupling stiffens the face;
    the stress there is the coupling's, not the part's. Where it is says
    so (VonMisesAt, and the CSV); a largest away from the loaded faces is
    not yet.
  - Tests: TestDynamics' test_loads_to_fem_frames (five frames, one mesh,
    one result, the hold against the run at each within test_loads_to_fem's
    tolerance, the result's largest the series', the channel's gaps, the
    CSV column; run again with twice the gravity, the series is not a
    channel any more); TestGuiCommands' test_results_keep_the_channel_chosen
    and test_results_with_gaps.
  - Not yet: the largest away from the loaded faces; several frames as
    steps of one CalculiX run (one input written, one start); contacts, torsion
    springs and applied loads, which the run cannot place yet (step 5's
    other half); a part in a rigid group (its body's motion is the
    group's, its loads from the others in it unknown).
- Built, D4a step 5 (second half): contacts, torsion springs and applied
  loads in a part's free body, and so in its stress analysis.
  - The run reports where a contact set presses each of its parts: per
    part, "PartContactForce", "PartContactPoint" and "PartContactTorque"
    -- the force its points press the part with, their centre weighted by
    their forces (the origin while none presses), and the torque about
    that centre: the set's resultant on the part, exact. A channel gains
    `on`, the part a value is on where its source has several (Backend.h;
    the archive's "on", which a reader before it reads past; Python's
    "On"). The parts are those the set's shapes are on, by the names the
    contact events give them -- a rigid group's own part, not the group.
  - The run reports each applied load: "AppliedForce" or "AppliedTorque" on
    its component, at its frame's origin. Its reaction takes the opposite
    where the frame's origin was at the start, which stays on the reaction
    (Chrono's body-body load); `loadsOn` takes that point from the run's
    first frame.
  - A torsion spring needs no channel: Chrono's, minus its torque about
    frame I's z on frame I's body, plus on frame J's.
  - `loadsOn` takes them (Kinds "Contact", "Load", "TorsionSpring"), and an
    end bound to an element through its binding -- a spring's, a bushing's
    and a load's ends were read off their Component properties, which an
    end on an element leaves empty. Missing is only what a run kept from
    before reports no channel of.
  - **Found on the way, a joint's reaction does not hold its motor's
    effort**, which step 2 took it to: a motion drives a joint through a
    motor link of its own, whose torque is the joint's "MotorTorque"
    channel (Chrono's, -T about zI on frame I's body, as a torsion spring
    is; a linear motor's force along zI). The tipping plate of the contact
    tests, turned by a motion, was 529.21 mN mm out of balance about its
    hinge, the motor's torque at that frame to the digit. A joint's load
    in a free body is now its reaction and its motor's effort.
  - To FEM: a contact's coupling moves with its point, frame by frame --
    its reference node there, on the part's face nearest it; where it
    does not press, as it was, unloaded. A torsion spring goes through its
    joint's coupling, one for the two, and is held with it where its joint
    is the hold: the hold stands in for both, and the check is against
    both (`_heldLoad`, the series' HeldTorque).
  - **Found on the way, in FEM: a coupling's reference node off by
    round-off was refused by CalculiX** -- "ERROR reading *NODE", and every
    set on it after. `write_constraint_rigidbody` wrote its coordinates
    with str(), up to 23 characters (-1.1050188541972261E-14), and
    CalculiX reads 20 a field. Written with 13 significant digits, as the
    step's loads already were. A joint's point never had the round-off; a
    contact's, from the run, does.
  - The Results box keys a channel by its part too, and names it so:
    "Contact (Ball): Contact force on the part".
  - Tests: TestDynamics' test_loads_on_torsion, test_loads_on_applied_load
    (a force between the double pendulum's rods, its profile 1 + 10 t, and
    a torque on the first), test_loads_on_driven (a kinematic run's motor)
    and test_loads_to_fem_torsion (a spring on each hinge, solved:
    HeldTorque the hinge's and its spring's, and the hold against it);
    TestContact's test_part_contact_channels (a ball at rest: its force at
    its lowest point, sunk; the plate's opposite there),
    test_loads_on_ball, test_loads_on_sliding_cube (the cube's free body,
    its four corners' resultant, and the driven plate's) and
    test_loads_to_fem_contact (a lever on a post, solved); TestGuiCommands'
    test_results_on_parts; FEM's test_reference_node_fields. Every free
    body balances within 1e-6 of its loads.
  - The manual's video (`assembly/simulation/loads-to-fem-contact`): a
    steel lever 120 x 10 x 8 on an eccentric disc turned at 2 rev/s, a
    torsion spring at its hinge and 3 N down at its tip, the contact by the
    steel's own law, 101 frames solved in 85 s. The disc's point moves
    along the lever from x = 67 to 88 mm, 5.3 to 9.5 N. The largest stress,
    2.456 MPa at 0.23 s, is at the hinge, where the spring's 259 N mm
    enters: beam theory 2.43 MPa. The hold within 0.35% of the largest
    reaction, its torque within 0.024%; the force's difference 0.020 N at
    a 3 mm mesh, 0.011 N at 1.5 mm -- taken then for the held face's own
    body load (step 4), it was the disc's push on the underside's nodes at
    the hinge's end, the held face's too (the next entry).
  - **Found and fixed: a substep taken again started from reactions not
    finite.** The video's disc on its lever, by the steel's own law -- a
    cylinder's side on a face, Johnson's line contact, about 2e8 N/m at its
    9 N -- stopped at 0.14 s, "the contact's integration diverged", at 1.5
    rev/s and up, by any motor, with friction or without, the disc a
    cylinder or a hull, and with a set law from 1e7 N/mm^1.5. Not the law,
    not the jacobian (measured at the state: the normal's 2.3e8 N/m, the
    tilts' k r^2): HHT's Newton trace had the substep of 1 ms run off, and
    every one taken again from the state before it, down to 4 ns, start
    with a residual not finite at its first iteration. HHT takes the
    reactions it starts from out of the system, alpha's share of the old
    Cq' l in the residual; the substep that ran off left them there not
    finite, and advance() put back the state and the accelerations but
    not the reactions. It does now (takeBack()), and the sensors'
    snapshot keeps them too, which put back a step to locate a trip with
    the reactions of a later state. TestContact's test_lever_on_fast_cam:
    the lever on the disc by the steel's law, 101 frames, sunk under a
    micrometre; it stops at 0.14 s without the fix.
  - **And then: a torsion spring a whole turn out after such a substep.**
    With the run going through, the video's lever lost its disc at 0.145 s,
    the hinge's reaction 454 N and its torque 6032 N mm -- the return
    spring's 1000 N mm/rad times a turn. Chrono's ChLinkRSDA counts its
    turn at each update from the last angle it saw, and the states of the
    substep that ran off moved the count on; it is private, not to be put
    back. A TorsionLoad of our own replaces it: the same torque, preload -
    k (turn - rest) - c rate, the turn of xJ about zI measured as Chrono's
    and as a penalty stop's, its count taken from the angle committed after
    each step (handleStops(), each frame of a kinematic run, the sensors'
    snapshot) -- implicit through Chrono's numerical jacobian, where the
    link was explicit. A motor's own count (ChLinkMotorRotation's mrot) is
    reported only: its position constraint is the formula's rotation.
    test_lever_on_fast_cam has the spring, its torque the turn's.
  - Not yet: see the next entry's.
- Built, D4a: a point load on a patch, the stress away from the loads.
  - **A load at a point was spread along its whole face.** A contact's,
    a spring's or a bushing's end, an applied load, a joint on a vertex:
    each through a distributing coupling on the face nearest its point,
    all of it -- the same force and moment about the point, but along the
    face: a push at a bar's middle bent it as a load along all its length
    (FEM's test: the tip 0.0715 mm for the point load's 0.0603). FEM, our
    fork: ConstraintRigidBody's `CouplingRadius`, a distributing coupling
    on the element faces within it of the reference node only (by the
    centre of each element's nodes on the faces; the nearest where none
    is), its node set theirs. MotionLoads gives such a load a patch a mesh
    size round; a joint's referenced faces stay whole, a bore or a face
    being what it acts on. test_motionloads' test_coupling_radius: through
    3 mm, 10 element faces, the tip 0.0596 mm against 0.0603; the whole
    face, 362, 0.0715 against a uniform load's 0.0722.
  - Found by it: the lever's hold, 0.35% of the largest reaction off the
    run's, is within 0.056% with the push on its patch. On the whole
    underside the push's nodes at the hinge's end were the held face's
    too, and their share went into the hold without passing through the
    lever -- not the held face's body load it was taken for; halving the
    mesh had halved both.
  - Each frame's largest von Mises stress away from the loaded faces:
    the nodes within a mesh size of the hold's faces and of a whole-face
    coupling's, and within a patch's radius and a mesh size of its point,
    left out (`_nearLoads`), where the stress shows a coupling's own
    stiffness or the load's concentration. The series keeps it
    (VonMisesAway, NaN where no node is that far; VonMisesAwayAt), the
    Results box plots it ("Largest von Mises stress away from the loaded
    faces"), the summary gives it.
  - The double pendulum's first rod (the manual's loads-to-fem-frames
    video, which now plots it): the same as the largest at 74 of the 81
    frames; at the 7 whose largest was the held face's (0.19 to 0.31 s),
    the rod's own at x = 20 to 58 mm -- in the middle third (0.19, 0.195,
    0.29 s) the per-frame video's beam theory within 0.6%. The lever on
    its cam: the same as the largest, the hinge's bending.
  - Tests: test_loads_to_fem_frames (the largest away a mesh size off both
    end faces, the same as the largest where that is; the two channels),
    test_loads_to_fem_contact (the contact's patch).
  - Not yet: see the next entry's.
- Built, D4a: a part of a rigid group.
  - A rigid group is one body to the run: its motion is reported at the
    group's centre of mass under its representative's name (the least,
    AssemblyObject's rigidRepByPart), and its loads are not split among
    its parts. **loadsOn took the representative as if the group's motion
    were its own and left the rest out, unsaid**: a pendulum of a rod and
    a tip in a group, the rod's free body out by 154 mN and 2436 mN mm,
    Missing empty; the tip it refused, no motion reported.
  - Now (`_rigidGroup`, as AssemblyObject unites the groups not
    suppressed): a member's centre's acceleration is the group's carried to
    it, a + alpha x r + omega x (omega x r); its own loads as any part's;
    and what the rest of the group holds it with is what balances it, Kind
    "Group", on the faces where it meets the others (`_groupFaces`: those
    sharing area with another's solid, else touching, else the nearest),
    at their centre -- where they share only part of them, on the round
    about it that takes that part in (Radius, then the coupling's
    CouplingRadius; FEM's rigid coupling takes it too now). A grounded
    group stands still. `movingParts` lists every member for To FEM...;
    a member is held at its group as at a joint (the largest load, or the
    one asked for), and a series held so knows its run by the group's
    acceleration (`_runMark`).
  - Checked apart: the two halves' group loads, the rod's from the
    hinge's reaction, equal and opposite within 1e-6 of the weight (the
    test) -- within 2e-15 N in the manual's video; and the members' m a
    summing to the group's.
  - The manual's video (`assembly/simulation/loads-to-fem-group`): an
    L-shaped pendulum, a rod and an arm 10 x 10 x 60 under its far end,
    the arm solved at 81 frames, held where it meets the rod: its hold
    within 0.42% of the largest (1.2% of its weight, the held face's own
    body load at a 1.5 mm mesh); its largest 0.275 MPa at a corner of the
    held face, 0.218 MPa away from it.
  - Tests: TestDynamics' test_loads_on_rigid_group,
    test_loads_on_rigid_group_patch (the rod's load from the arm on a
    round of 5 sqrt(2) about the 10 x 10 patch of its underside, solved);
    FEM's test_rigid_coupling_radius.
  - Not yet: see the next entry's.
- Built, D4a: a contact's places on a part.
  - A contact set pressing a part at places apart was one resultant at
    their centre, and To FEM put it there: a lever across two posts, their
    pushes in the middle of the span. The run reports each place too:
    "PartContactPlaces" per part, width 40 -- four of force, centre of the
    points weighted by their forces, torque about it, and how far the
    points reach from it; a place a patch of a pair of bodies' points
    (ContactLoad's patches), the most pressed first, the fourth the rest.
    A channel past a vector's three numbers goes to Python as a tuple and
    is kept out of the Results box's plots (resultChannels). loadsOn gives
    a contact load per place, with its reach; To FEM a coupling per place
    on its patch, the reach and a mesh size round, moved at each frame --
    a box flat on a face on its whole footprint, a ball on a dot -- and a
    place not pressed at a frame suppressed (left out of the input).
  - **Found by it: a run's start was out of balance where a contact began
    in its smallest steps.** The start's reactions are taken back from
    them (HHT has none before a step: 2 at 32 of h/1024 less at 64), and
    the bodies' accelerations were too, while a contact's force is read
    at the start. A steel lever let go across two steel posts read 131
    rad/s^2 at the start for the 122 of one with nothing under it, its
    free body 5% of its weight out, To FEM's hold at the first frame 5%
    off at any mesh. The start's accelerations are now its own forces --
    its loads' at the start, a contact's as it stands -- and Cq' L of the
    reactions taken back (2 L halfway less L at the end), through each
    body's mass and inertia at its start turn (motionFrom, the frames'
    too). The free body balances; the accelerations are as good as the
    reactions taken back, which still have the posts' first push in them:
    a lever's turn 3.6% under its free rate (test_start_with_contact
    takes 5%).
  - The manual's video (`assembly/simulation/loads-to-fem-places`): a
    steel lever across two posts, a block dropped onto it between them,
    101 frames: up to three places at once, at the posts (x = 36 and 95
    mm) and under the block (14.8 N at its landing, on its edge); the hold
    within 0.051% of the largest reaction, its torque within 0.064%. Its
    largest away from the loaded faces at the landing, 2.00 MPa at x = 4
    mm, a mesh size and a little off the held face, is still the hold's:
    one mesh size is less than a coupling's reach into the stress, about a
    section's depth.
  - Tests: TestContact's test_part_contact_places (two places at the posts
    summing to the resultant and its moment; solved, a coupling each, and
    at the start, none pressed, none in the analysis),
    test_start_with_contact.
  - Not yet: see the next entry's.
- Built, D4a: impact frames.
  - **The video's block landed with the plot flat** (the user's eye): an
    impact of steel on steel lasts some 60 us and fell between the output
    frames 2.5 ms apart -- the block's landing at 63.9 ms, 626 mm/s, in
    none; the stress spiked only at a bounce at 140 ms, 68 mm/s, that
    happened to span a frame.
  - A Dynamic study's `ImpactFrames` (the panel's "Impact frames";
    SimulationSpec::impactFrames): after each substep kept, each contact
    load's force (watchImpacts); the state where it was the most in an
    impact, its values and poses, is kept, and when the load lets go or
    eases to half of that, it becomes a frame, sent in time order before
    the next output frame (sendImpacts). A load resting at a steady force
    gives none; a sensor's step back drops those past it. The frames'
    times are then uneven; the Results box plots by time, a channel's
    values are at each frame, To FEM solves each.
  - The places video again with them: 101 output frames and 80 at
    impacts' peaks (the block's bounces, the lever's rattle on its posts);
    at the landing, 63.88 ms, the block's 528 N on the lever, the lever's
    largest 64.6 MPa under it and 54.3 MPa away from the loaded faces,
    where it showed 2.2 MPa at the bounce before; the hold within 0.013%
    of the largest reaction, its torque within 0.40%.
  - Test: TestContact's test_impact_frames.
  - Not yet: several frames as steps of one CalculiX run (the frames'
    runs at once instead, below); the margin the largest away from the
    loaded faces leaves (built, below); a part of a group meeting the
    others nowhere (held at the face nearest them); a run's start
    reactions taken back with a contact begun in them (the accelerations
    now balance them, not the free motion's); a static analysis of a
    60 us impact, where the lever's own vibration is a dynamic one's.
- Built, D4a: frames through each impact, playback speed, peak-safe plots
  (sec 8.6).
  - **The impact frames' video was choppy** (the user's eye): each frame
    at a peak held the motion at an instant. And the plot drew each impact
    as a spike from the output frames either side, its rise and fall not
    in it.
  - The run keeps frames through each impact, thinned by Douglas-Peucker
    on the contact forces (sec 8.6); `ImpactFrames` is now on by default.
    The sampling only watches: at the output times the same poses as
    without, to 1e-12.
  - Playback by the run's time (`PlaybackSpeed`, Auto by default;
    `updateForTime()`), the plot zoomed with the speed and drawn by its
    columns' extremes (M4).
  - To FEM solves every frame by default. The first cut of the thinning,
    5% of each load's largest so far, thinned the block's bounces away
    after its landing; and solved at each impact's peak alone, the stress
    zoomed in was a line 5 ms wide.
  - The places video again: 327 frames, 101 output frames and 226
    through the impacts, the run 2.2 s; the block lands at 63.87 ms, its
    largest 528 N at 63.88 ms, 15 frames through it. Of its 13 impacts on
    the lever, the smallest 1.5 N. Solved at every frame in 337 s: the
    lever's largest 64.6 MPa, 54.3 MPa away from the loaded faces, at the
    peak. The hold within 0.013% of the largest reaction, its torque
    within 0.96% of the largest (0.40% at the peaks alone). The video, at
    Auto, 582 frames, 19.4 s at 30 fps, as slow as 1/17,500 of real time
    through the landing.
  - The torque's 0.96%, looked into: the lever's free body balanced in
    force to 1e-15 at every frame, in moment not, out by up to 8.65 N mm
    at frames through the impacts -- at the set-off frames, each the
    substep before the forces moved, kept with its values on the line
    between its neighbours'. Its contact point and force each so, but not
    their moment: r x F is not linear in them. Its values are now read
    too, at no cost a run (2.05 s for the 2.2); every frame balances to
    3e-8 N mm. Test: TestContact's test_impact_frames_balance (65 N mm
    out at the landing's set-off before, its bound 6). Solved again at
    every frame, the hold's torque is within 0.49% of the largest: the
    analysis' own share now, as at the peaks alone (0.40%) -- the contact
    couplings sharing edge nodes with the held face.
  - Tests: TestSimulationResults' test_update_for_time,
    test_playback_speeds, test_peak_safe; TestContact's
    test_impact_frames; TestGuiCommands' test_playback_speed.
  - Then, the user's eye on the video: a plot whose run's time narrowed
    with the speed zoomed the whole timeline. It now goes by the video's
    time, the slowed parts alone stretched, with a movie player's slider
    under it: ticks spreading and closing up through the easing, the
    times each slowed span begins and ends, a click or drag going to a
    time, + and - zooming about it (sec 8.6). In the playback video at
    the landing's peak, the plot shows 2.9 to 72.8 ms of the run, the
    landing's 0.1 ms a third of it.
  - Then, the user's eye again: from 64 to 95 ms the block flew in slow
    motion. The lever rattling on its posts, 59 N at most, was slowed as
    much as the landing; Auto is now weighted by the swing of the contact
    forces, with `SlowMotion` its scale (sec 8.6). The playback video at
    Auto went from 19.4 s to 5.4 s, the landing still slowed to 1/17,500
    of real time.
  - Not yet: see the next entry's.
- Built, D4a: the stress away from the loads, a section's depth off them.
  - **The largest away from the loaded faces left a mesh size off them**,
    less than a coupling's or a concentrated load's reach into the stress,
    about a section's depth (Saint-Venant's). The places video read 54.3
    MPa at the landing at x = 55 mm, between the block and the near post,
    where beam theory from the run's loads gives 22 to 41 MPa, as the
    lever's own inertia goes.
  - Now `_sectionDepth`: the part's shortest span through the load, along
    its principal axes (its mesh nodes' spread), each from where the line
    first meets the mesh to where it last leaves it (the nodes within a
    mesh size of it), at least a mesh size. A bar's thinner side at its
    middle or its end; across a hinge's bore, wall to wall. A whole face's
    through its centre; a patch's through its point, by where it is to a
    half mesh size, as it moves.
  - The videos again: the double pendulum's rod, 10 mm, the same at its 81
    frames -- the 7 the margin moved were the rod's own already. The cam's
    lever, 8 mm: 2.065 MPa at x = 8.3 mm, against the largest 2.457 at the
    hinge's face; beam theory there, the hinge's torque less its force
    over 8.3 mm, 1.99. The group's arm, 10 mm: 0.160 MPa 10.4 mm below its
    held top, where it read 0.218 MPa 2.4 mm below it. The places lever, 8
    mm: at the landing 13.56 MPa at x = 20 mm, between the hinge and the
    near post, beam theory from the hinge's 84.5 N 12.4 MPa; between the
    post and the block, 33 mm, nothing is a section's depth off both.
  - The manual's cam video had taken impact frames by their new default,
    26 through the disc turning at full speed from rest against the lever,
    and labelled its frames as if 5 ms apart: its scene now sets
    `ImpactFrames` off.
  - Tests: TestDynamics' test_section_depth, test_loads_to_fem_frames
    (10 mm off both end faces).
  - Not yet: see the next entry's.
- Built, D4a: the frames' solves, 0.89 s a frame to 0.24.
  - Where a frame's time went (the places lever, 40 frames, a 3 mm mesh,
    profiled): CalculiX 0.37 s, the input 0.39 -- of it 0.25 s FEM's
    writer finding each coupling face's nodes again, `getNodesByFace` per
    node by BRepExtrema, on a mesh that had not changed -- and the result
    0.09.
  - FEM, our fork: MeshSetsGetter's `cache`, a dict FemToolsCcx keeps
    between writes (`mesh_sets_cache`): the mesh's element tables, and each
    reference's nodes while it is the same shape at the same place; cleared
    when the mesh's nodes or counts change. The input to the card the same
    as a fresh writer's (FEM's test_mesh_sets_cache, through a coupling
    moved, given a radius, the mesh made again). The input 0.39 s to 0.10.
  - Several frames as steps of one CalculiX run, measured on the places
    input: one step 427 ms, ten 2.9 s -- each step factors the system
    again, the saving the 200 ms of reading the input and setting it up;
    and a moving patch's coupling is model data, all of them in the one
    input. Not taken.
  - Instead, the frames' runs at once: ccx's SPOOLES solver takes one CPU.
    solveFrames writes each frame's input, moves it into a folder of its
    own and starts ccx there (FEM's `ccx_process`, not waited for, one
    thread, its output to a file), `jobs` at once (4 by default, half the
    CPUs at most); the oldest collected when that many run, so the rows
    keep their order; where the stress is away from the loads is taken
    as the frame is written, before the next moves the couplings. A stop
    asked for leaves those started.
  - 40 frames of the places lever: 35.4 s, then 23.0 with the cache, 9.5
    with 4 at once -- the stresses, the away ones and the hold's forces
    the same to 9 digits each way.
  - Tests: TestDynamics' test_loads_to_fem_frames_jobs (3 at once against
    1, to the last digit; a stop at the fifth frame); FEM's
    test_mesh_sets_cache.
  - Not yet: see the next entry's.
- Built, D4a: the plot's value axis by its window.
  - The Results box's value axis was the whole run's: zoomed in on a
    bounce of 1.5 N after a landing of 528 N, the bounce was drawn flat.
  - Now by what its lines show in the window (`SimulationPlayback.
    shownRange()`, over peakSafe's points): those in it, and where a line
    crosses its edges -- M4 keeps each column's least and largest, so the
    range is the values' own at any width. The whole run's where nothing
    shows (a window inside a gap); and no finer than a thousandth of the
    whole run's, a steady value's round-off not drawn across the height.
    The labels at its top and bottom are that window's.
  - Test: TestSimulationResults' test_peak_safe (the bounce by its own
    range; an edge crossed halfway down the landing's fall, 264; a gap,
    none).
  - Not yet: see the next entry's.
- Built, D4a: a part of a rigid group over a gap to the others.
  - **Meeting the rest of its group nowhere, a part was held on the face
    nearest them first found** -- and along the edge nearest the other,
    a box's side faces are as near as the face across the gap. The group
    page's arm hung 1 mm under the rod was held on its side face, all 60
    mm of it: its largest stress 0.0044 MPa where, touching the rod, it
    is 0.275, and no point a section's depth off the hold. The rod was
    held on its end face.
  - Now each of the others within twice the least gap is moved across
    its gap to the part (along its nearest points' way), and the faces
    are those it would meet, as for parts that touch: the area they
    share, a round of it where it is part of a face. The load's `Gap`
    says how far (`groupGap()`), and To FEM's message says it: "It meets
    the rest of its rigid group nowhere, 1 mm off them".
  - The group page's video again with the arm 1 mm under the rod: 0.2732
    MPa at 160 ms, 0.1593 away from the loaded faces, against 0.275 and
    0.160 touching; the hold within 0.44% of the largest.
  - Part, from upstream (6132f25fdf): `Tools::fromPlacement` set the
    translation and then the rotation, which in OCCT replaces it -- every
    location it made was the rotation alone. FEM places a constraint's
    references with it (`Tools::getFeatureSubShape`): on a part turned
    and moved, a constraint's symbols were the face turned about the
    origin, the arm's hold drawn 100 mm off its analysis in the still;
    and a bearing's or a cylindrical transform's base point, which the
    input's *TRANSFORM takes, off the cylinder's axis. ShapeFix's Python
    methods taking a placement made the same location.
  - Tests: TestDynamics' test_loads_on_rigid_group_gap; FEM's
    test_placed_references.
  - Not yet: see the next entry's.
- Built, D4a: a run's start reactions solved at it.
  - **The start's reactions were taken back from the run's smallest
    steps**, a line through those after 32 and after 64 of h / 1024, and
    a contact begun in them was in them: the lever let go across two
    steel posts turned at 141 rad/s^2 at the start for the 146.7 of one
    with nothing under it, 3.6% under.
  - Now solved at the start (`solveReactions()`): [M Cq'; Cq 0] [a; -L]
    = [F; -g], F the start's forces -- a contact's as it stands, none
    where it only touches -- and g the constraints' acceleration with
    none, a motion's own and the velocities' turning: C's second
    difference along the start's velocities over h / 64, through Chrono's
    StateSolveCorrection with M alone. The system's accelerations and
    reactions are put back after it: HHT starts from them, and the
    run's frames are as without. The line through the smallest steps is
    the fallback where the solve fails.
  - Found by its test: the bodies' accelerations at the start, M^-1 (F +
    Cq' L), took Cq after the smallest steps. A rod driven from level at
    3 t + t^2 rad, turning at 3 rad/s, read 1.995 rad/s^2 for 2, its
    centre -449.29 mm/s^2 for -450. The solve gives F + Cq' L at the
    start itself.
  - Both now as the closed forms to 1e-6: the lever's 146.7 rad/s^2; the
    rod's centre -9 r along it and 2 r across, the hinge's force m (a -
    g), the motor's torque m g r less I alpha.
  - Tests: TestContact's test_start_with_contact (5% before, now 1e-6);
    TestDynamics' test_start_driven.
  - Not yet: a part's own stress at a frame through an impact is a static
    analysis of it (built, next).
- Built, D4a: a part's own vibration through the run.
  - **Each frame's analysis is the part as it stands**, balanced by its
    rigid inertia: right where the loads change slowly against its own
    periods, not through an impact. The places lever's landing, the
    block's 528 N over some 60 us, is over in under a thirtieth of the
    lever's first period held at its hinge (2.2 ms, 452 Hz): the frames
    said 64.5 MPa under the block at the force's peak and 0.03 MPa a
    millisecond later, where the lever, its own inertia taking the blow
    first, is at 9 to 22 MPa during it and rings at 30 to 50 after it.
  - Now To FEM's frames "With its own vibration" (`solveFrames(dynamic=
    True)`): the held part's modes (20 by default, a CalculiX frequency
    step on the same analysis), each frame solved as it stands, and the
    mode-acceleration method between them. The loads are linear between
    two frames, so the static response is the line between the frames'
    solutions, exactly; the rest is the modes', each y_i'' + 2 z w y_i' +
    w^2 y_i = -c_i'' - 2 z w c_i', c_i = phi_i^T M u the static
    solution's share of the mode -- c linear between frames, so y a damped
    sine from each frame on, in closed form (`_modalResponse()`): no
    steps. From rest at the first frame. M is the 10-node tetrahedra's
    consistent mass, built here (`_tet10Mass()`, the nodes by where they
    are); the density cancels. The modes that see no change respond
    statically, which the frames' solutions have.
  - The stress is sampled between the frames, 8 a period of the highest
    mode kept and at most 200 an interval; the series keeps the samples
    (SampleTimes, SampleVonMises, SampleVonMisesAway) and each frame's
    static stresses beside its own (StaticVonMises). The Results box
    draws the samples and the frames as they stand under them; the
    analysis is left at the largest sample, its result that field written
    over the nearest frame's.
  - Damping one ratio for every mode, 2% of critical by default, the
    dialog's. It matters through an impact: the landing's window peaked
    at 50.8 MPa undamped, 40.6 at 2%, 0.3 ms after the blow, its higher
    modes'.
  - Measured against CalculiX's own direct integration (*DYNAMIC, HHT, a
    step per frame interval, the loads ramped across it) through the
    landing: at 2.8 us increments, within a few percent of each other at
    every frame of the impact (22.3 MPa against 22.3, 32.3 against 31.5,
    35.8 against 36.0). CalculiX iterates Newton's method at each
    increment however linear the model (0.6 s each here): after the blow
    its 55 us increments fell behind the ringing, and 1.5 us through a
    window would be 17 minutes an impact. The modal way took 14 s for the
    window, 20 modes against 40 to 0.1%.
  - The places page's run again, every frame (327, 226 through impacts),
    in the manual's video (`assembly/simulation/loads-to-fem-vibration`):
    163 s, the frames' solves 92 of it; 25929 samples. The largest 40.6
    MPa at 64.18 ms, the lever's end at its hold, bending as it rings;
    away from the loaded faces 29.5 MPa at 63.98 ms, mid-span, where the
    frames as they stand had 13.6 at most (at a bounce, 121.8 ms) and
    64.6 under the block.
  - Found on the way, for that input: CalculiX adds a node's second
    *CLOAD in a step to its first (the posts pushed twice), and takes one
    amplitude a node's degree of freedom, which a distributing coupling
    spreading its six onto the same face nodes breaks.
  - Tests: TestDynamics' test_modal_response (the closed form against
    RK4, undamped and damped, 1.2e-9 of c), test_held_modes (the modes
    orthogonal through the mass to 1.3e-5, the first the clamped beam's
    to 0.15%, the falling rod's static state made of its modes but 9.3e-6
    of it), test_loads_to_fem_vibration (the pendulum, slow against its
    1.2 ms, within 1.2e-4 of its static stress; the samples, the result
    at the largest); TestGuiCommands' test_results_sampled.
  - Not yet: one way -- the run's contacts and joints are its rigid
    bodies', so the posts push as for a rigid lever and the block's
    impact is a rigid one's; the part held stiff at its joint's faces, as
    the static frames are -- a revolute's turn about its axis held too,
    so the lever's modes are a cantilever's (452 Hz first) where a pinned
    one's first would be some 4.4 times that, and its largest stress is
    at that hold; held as its joint holds it, the turn left free, its
    rigid mode left out and the frames solved by inertia relief, would
    take that away (built, next). Its rotation's own stiffening and its
    large motion's coupling to the modes left out; from rest at the first
    frame solved.
- Built, D4a: the vibration's modes held as the joint holds them.
  - **The modes were the part held stiff at its joint's faces**, a
    revolute's turn about its axis held with the rest: the places lever's
    were a cantilever's, its first in its swing's plane 452 Hz, where
    pinned it is 1958 Hz, and its largest stress, 40.6 MPa, was at that
    hold.
  - Now the modes' run holds the part as its joint does (`_holdAsJoint()`):
    a revolute leaves the turn about its axis free, a cylindrical that and
    the slide along it, a slider the slide, a ball every turn; a motion
    driving the joint holds what it drives, and a rigid group's hold or
    another kind of joint stays stiff. The hold's axes are the joint's,
    however it lies: FEM's rigid body constraint has Axes now (not
    upstream), which the CalculiX writer puts as a *TRANSFORM on its
    reference and rotation nodes -- CalculiX takes it for the rotation
    node's too, the same modes as the axis freed by hand, others for a
    wrong one. CalculiX finds the free motion's mode or leaves it out (an
    eigenvalue just under 0 is out of the range asked for), so one more
    mode is asked for each motion left free, and a mode more of the free
    motions than not, by the mass, is dropped (`_Modes(free=)`).
  - The frames stay as they were, held stiff: what holds the part there
    takes next to nothing, the frame balancing it, so its static state is
    the one held free but for a free motion, which no mode has a share of
    -- the stresses are the same, and so is each mode's share. No inertia
    relief is needed for it. What holds the part is still read off the
    stiff hold, along the global axes, for the check.
  - The places page's run again: its first mode 563.8 Hz, across the
    hinge's axis and held stiff so; the first in its swing's plane 1958
    Hz, the pinned lever's. 162 s for the 327 frames, 26168 samples. The
    largest stress 31.8 MPa at 64.02 ms, away from the loaded faces,
    where the lever bends between its hinge and the posts; the hinge end
    no longer the largest (40.6 MPa held stiff). As the frames stand,
    64.6 MPa under the block and 13.6 away, as before. Undamped, the
    landing's window peaks at 35.1 MPa (50.8 held stiff).
  - Tests: test_motionloads' test_rigid_coupling_axes (the bar held free
    to turn about z, the same modes along the global axes or turned 90
    deg about x; the clamped and the pinned beam's first); TestDynamics'
    test_held_modes_free (the rod's turn no mode's, a share of it 4.5e-6;
    the first across the hinge the clamped beam's to 0.15%, the first
    about it the pinned one's, 2.4% under it, shear; its static state
    made of its modes and the turn but 8.7e-7 of it);
    test_loads_to_fem_vibration now the rod free to turn, 1.4e-5 of its
    static stress (1.2e-4 held stiff).
  - Not yet: one way still, from rest, and the rotation's stiffening left
    out; a joint other than those (a distance, a gear, a cam) holds the
    part stiff in its modes.
- **D4b, flexible bodies: the plan (2026-11-06).** A part that bends in the
  run itself, its flex feeding back into the joints and the motion: two
  ways, where D4a's vibration is one. Open question 10's answer: modal
  reduction (`chrono_modal`), direct FEA bodies only for a specific need.
  - How the others do it. Adams takes a modal neutral file (MNF) made by
    component mode synthesis, Craig-Bampton modified, from Nastran or
    Ansys: interface nodes at the joints -- at a hole's centre, a spider
    (RBE2, or beams in Ansys) to the faces it bears on -- their static
    modes and the fixed-interface modes, orthonormalised
    (https://ansyshelp.ansys.com/public/Views/Secured/corp/v251/en/ans_substr/advmeth2402.html).
    MotionSolve's flexible body is OptiStruct's CMS (Craig-Bampton or
    Craig-Chang) in an H3D file, interface nodes by an ASET, small linear
    deformation only, the modes' coordinates added to the six of the
    rigid motion; a "Rigidify" switch runs it rigid with its inertia
    (https://help.altair.com/hwsolvers/ms/topics/solvers/ms/xml-format_34.htm,
    https://help.altair.com/hwdesktop/hwx/topics/motionview/optistruct_flexbody_generation.htm).
    The stresses are recovered after the run: each mode's stress field,
    weighed by the run's modal coordinates; stress takes more modes than
    shape (https://help.altair.com/hwsolvers/ms/topics/solvers/ms/intro_to_flex_bodies_intro_r.htm).
    RecurDyn has both, RFlex (CMS) and FFlex (full nodal), and says a modal
    body is for well-defined attachment points with no contact on it:
    a contact is a moving attachment point, whose static modes are not in
    the basis (https://functionbay.com/en/page/single/22). FFlex is for
    contact on a flexible part.
  - What Chrono 10 gives: `ChModalAssembly` (`chrono_modal`), a sub-assembly
    reduced by Herting (free-free modes) or Craig-Bampton (the boundary
    held), its boundary items kept -- bodies, nodes, links between them --
    and its internal ones (a `ChMesh`, internal links) replaced by modal
    coordinates on a floating frame, with the inertial coupling of its
    motion; gravity through the reduced mass; the internal nodes updated
    from the modes for display and stresses, a static correction for loads
    on them. Its eigensolvers are Spectra's (header only, MPL-2.0; its
    develop branch, whose Krylov-Schur solvers no release has); its K_II
    inverse a direct solver of our choice. The 10-node tetrahedron is corotational
    (`ChElementTetraCorot_10`), its stress at any point (GetStress); its
    mass lumped equally on the ten nodes, which for a 10-node tetrahedron
    is wrong (its corners take negative consistent mass): ours is a
    subclass with the consistent mass, D4a's `_tet10Mass()`.
  - The element: a Physics item, `FlexibleBody` -- `Component`, `Modes` (12
    by default), `Damping` (2% at the part's first mode, in proportion to
    its stiffness: step 2), `MeshSize`, and the
    mesh: its nodes (the component's coordinates) and 10-node tetrahedra, a
    Netgen mesh made for it as D4a's are, and each interface's nodes. An
    interface is each frame on the component that something holds -- a
    joint, a spring's or bushing's end, a load -- its nodes those of the
    faces D4a takes for it (`_facesOf()`); a vertex, the nearest. A study
    lists it in Elements, as every other.
  - The run: a dynamic run with a flexible body builds it as a
    `ChModalAssembly`, Craig-Bampton: a boundary body for each interface,
    at its frame, of negligible mass; its nodes tied to it rigidly (an
    RBE2, `ChLinkNodeFrame`, internal); the mesh internal. The joints,
    springs and loads on the component go on its interfaces' bodies. The
    component's pose reported is the floating frame's, carried from where
    the reduction found it; its "ModalCoordinates" a channel of width the
    modes, from which the deformation and the stresses are recovered after
    the run (each mode's displacements and stress field, sampled at the
    nodes). A kinematic run, a static one and a drag take the part rigid,
    with the mesh's mass.
  - Steps, each shipping:
    1. Build `chrono_modal` with Spectra beside the core (ChronoCore.cmake,
       an imported `ChronoModal`); the consistent-mass tetrahedron. Test
       (C++): a cantilever bar of 10-node tetrahedra in a ChModalAssembly,
       its first modes against the beam's closed form and the full mesh's.
    2. The neutral model's `Dynamics::flexibles` and the backend: a
       flexible body's interfaces, its joints on them, the floating frame
       as its pose, the modal coordinates as a channel. Tests: a cantilever
       held by a Fixed joint, settling under gravity to rho g A L^4 / 8 E I
       at its tip and ringing at its first frequency; a flexible pendulum,
       its swing the rigid one's to its stiffness and its bending the
       pinned-free beam's.
    3. The document: `FlexibleBody`, its command and task panel, the mesh
       made for it (kept, remade when the shape or the size changes), its
       interfaces from the study's joints and loads; the panel listing it.
    4. The results: the part shown deformed through playback (by a scale),
       its stresses coloured over it; To FEM's vibration taking the run's
       own modal coordinates, both ways, where D4a computes them one way.
    5. Contact on a flexible part: its contact taken at an interface of its
       own (the contact's faces, their static modes in the basis), as the
       modal bodies elsewhere allow at best; full nodal bodies, Chrono's
       `ChMesh` without reduction and `ChContactSurfaceMesh`, only when a
       case needs them.
  - Not in it: large deformation (the modes are linear about the shape at
    rest; a corotational mesh unreduced would be); a flexible part on the
    ground; the rotation's stiffening (geometric stiffness).
- Built, D4b step 1: Chrono's modal module, and a flexible body reduced.
  - **Spectra is a submodule**, `src/3rdParty/spectra` (Thundereal's fork
    of yixuan/spectra, its develop branch): Chrono's modal module includes
    `KrylovSchurGEigsSolver.h`, which no Spectra release has -- conda-forge's
    `spectra-cpp` 1.2.0 neither -- and Chrono's own build scripts clone
    develop. `ChronoCore.cmake` builds `Chrono_modal` beside the core when
    the submodule is there (an imported `ChronoModal`), and the Assembly
    compiles `Solver/ChronoFlexible.cpp` with it (`ASSEMBLY_WITH_CHRONO_MODAL`);
    without it the backend says it has no flexible bodies.
  - The neutral model's `Dynamics::flexibles` (`Flexible`): the body's
    10-node tetrahedra in its frame, E, nu and the density, the modes and
    the damping, and each interface's frame and nodes; scaled to SI with
    the rest (`scaled()`), compared by `sameMechanism()`.
  - `ChronoFlexible::build()` and `reduce()`: the mesh internal in a
    `ChModalAssembly`, our tetrahedron with the consistent mass
    (`ConsistentTet10`); a boundary body per interface, a millionth of the
    mesh's mass, its nodes tied to it by `ChLinkNodeFrame` (internal, an
    RBE2); Craig-Bampton; each mode of the reduced system damped at one
    ratio (`ModalRatio`). `Chrono::flexibleFrequencies()` reports the
    modes, their interfaces held.
  - **Found on the way, Chrono's modal module took every item as a body**,
    7 position and 6 velocity coordinates, and an xyz node -- every node
    of a mesh of tetrahedra -- has 3 and 3: in the matrices turned to the
    floating frame's axes every other node was turned wrongly and the last
    ones, past the final block of 6, had none, so K_II was singular and
    the eigensolver's LU failed and then solved on regardless, corrupting
    the heap; the gravity, the internal forces, the internal nodes'
    positions and velocities likewise. Each item's coordinates are now laid
    out by `Setup()` and those loops go by them. And the unsymmetric
    eigensolver, the one a modal assembly takes, reported an undamped
    mode's frequency as w^2 / 2 pi (the clamped bar's 209.6 Hz as 276137),
    its shift in the units of the damped problem's. And the reduction's
    sparse matrices were built a coefficient at a time into a reservation
    its own `setZero()` had taken back: 8.7 s on the test's bar, 0.4 now,
    with SparseLU for K_II (SparseQR, Chrono's default, 2.2 s). Our fork
    of Chrono, branch `thundereal`.
  - Chrono's tetrahedron lumps its mass equally on its ten nodes; on the
    test's bar, two elements across, its first two frequencies were 0.06%
    and 0.25% under the consistent mass's.
  - Test: FlexibleBody.cantileverModesAreTheClampedBeams (C++): a steel bar
    200 x 10 x 10 mm, 20 x 2 x 2 cells of six tetrahedra, held at one end
    by an interface on its end face's 25 nodes: 209.6 and 209.7 Hz against
    the clamped beam's 208.9, 1299.2 and 1300.1 against 1309 (shear).
- Built, D4b step 2: a flexible body in a dynamic run.
  - **The run builds it** (`buildFlexibles()`, at load, before the joints):
    every frame on the body an interface, its boundary body at the frame
    -- the frame's interface nodes, or the nearest node -- so the joints,
    springs, bushings, loads and motors on the part go on those bodies
    (`bodyOf()`, `localOf()`: the frame at its body's origin). The part's
    own ChBody stays out of the system, a proxy carrying the floating
    frame's pose, velocities and accelerations for what reads a body's
    (`syncFlexible()`): its pose reported, its "Acceleration",
    "AngularVelocity" and "AngularAcceleration". Its mass is the mesh's.
    Its "ModalCoordinates", a channel of the modes kept. A kinematic or a
    static run takes it rigid.
  - The floating frame's acceleration, which Chrono does not keep: the map
    its velocity is taken from (P_F Q_0), applied to the boundary's and the
    modes' accelerations (`GetFloatingFrameAccelerations()`, our Chrono).
  - **The damping in proportion to the stiffness**, R = (2 z / w) K: the
    ratio at the frequency given, `dampingFrequency` (0: the part's first
    mode, its interfaces held), more above it. Each of the reduced system's
    modes damped at a ratio -- step 1's -- is the free part's, its rigid
    motions undamped: 0.5 left the cantilever clamped by its joint ringing
    at 0.023 of critical, its mode mostly a rigid turn about the clamp. The
    stiffness damps deformation alone, whatever holds the part.
  - Newton's test a thousandth of the run's tolerance in position, as
    contact's (`tolerate()`): the modal assembly's Jacobian is approximate,
    its inertial terms linear, and at the default 1e-10 every step of a
    flexible pendulum ran its 20 iterations. And the mesh's nodes not moved
    with the modes as the run goes (SetInternalNodesUpdate(false)): each
    iteration moved them all. 15.9 s to 3.9 for the pendulum's 2000 steps.
  - Refused for now: a contact on a flexible part (step 5), a part's own
    initial velocity (it would start one interface), a flexible part fixed
    to the ground.
  - Tests (C++): FlexibleBody.cantileverBendsAndRingsInARun -- the bar fixed
    to the ground by a Fixed joint at one end, a gram fixed to the other:
    damped hard it settles 8.87 um down, the beam's 8.95; at 2% it rings at
    206.60 Hz against the cantilever's with its tip mass, 206.27, decaying
    at 0.0200; its modal coordinates reported. pendulumSwingsAsTheRigidBar
    -- hinged at one end, let go level: its centre within 2.4 um of the
    rigid bar's through the swing, 93 mm down by 0.2 s.
  - Not yet: the run's cost, 2 ms a step for the bar, most of it the modal
    assembly's own update at each iteration; stops' bounces leave a
    flexible part's interfaces as they were.
- Built, D4b step 3: the document's flexible body.
  - **`Assembly::FlexibleBody`**, a Physics item (`App/Physics.{h,cpp}`):
    `Component`, `Modes` (12), `Damping` (0.02, at `DampingFrequency`, 0:
    the first mode), `MeshSize` (0: a fortieth of the part's size), and
    what is made for it: `Nodes` (the component's coordinates),
    `Tetrahedra`, `InterfaceFrames` and `InterfaceNodes`, `MeshSignature`.
    Its command, Assembly_CreateFlexibleBody (`CommandCreateFlexibleBody.py`):
    the component selected, used by every Dynamic study; its icon a bar
    bent from its hold.
  - **Its mesh and interfaces are made before a run** (`FlexibleBodies.py`,
    `prepare()`; the Simulation panel calls it, `prepareStudy()`): Netgen
    through the Fem module, second order, in no document
    (`Fem.meshNetgen()`, since step 6; a hidden document of its own
    before); each joint on the component an interface, labelled as the model
    labels the joint's frame (its full name), its nodes those of the faces
    its reference stands for (`MotionLoads._facesOf()`, FemMesh
    `getNodesByFace()`); kept while the shape, the size and the joints'
    references are the same (`MeshSignature`). A force element's end on
    the part takes the nearest node, the run's default.
  - The model's builder (`AssemblyObject::addForces()`) turns it into the
    `Flexible` record once every frame is made: the nodes into the body's
    frame, the material the component's (`elasticityOf()`), its density its
    mass over its volume -- an override's mass too -- and each frame on the
    body its interface by its label. A component in a rigid group or fixed
    to the ground stays rigid, with a warning; so does one with no mesh.
  - **Found on the way: the session wrappers lost the mesh.** The joints'
    centres (`centredOnJoints()`) move each body's frame to its joints'
    centre, its frames and inertia with it but not its mesh: the
    pendulum's rod, its frame moved to its hinge at (0, 5, 5), had its mesh
    5 mm off and swung down 1.10% early, whatever the mesh or the step.
    And the rigid groups (`groupedRigidly()`) dropped the flexible
    records: a part with a block welded to it ran rigid. Now the mesh moves
    with its body's frame, and a flexible body is neither taken out nor
    grown into a group: what is welded to it is held at its interface.
  - And Netgen's tetrahedra turn the other way to Chrono's, whose
    stiffness takes the Jacobian's sign: each is turned (its corners 1 and
    2, and their edges' nodes, swapped) where it needs it. And
    `femmesh.Nodes` makes the whole dict at each read: read once, the
    mesh's 17 s are 0.8.
  - The reduction with the symmetric eigensolver (Spectra's Krylov-Schur,
    a real LU) where the modal assembly took the complex one only (our
    Chrono takes either now): the rod of 16000 coordinates 9.1 s to 5.3.
  - Tests: TestFlexibleBody -- test_mesh_and_interfaces (the rod's mesh in
    its coordinates, its interface every node of the hinge's face, kept,
    made again for another size); test_welded_block (a block welded to the
    rod's end held at an interface of its own, the rod still bending);
    test_flexible_pendulum (aluminium, let go level: its centre at the
    bottom in a quarter period within 0.2% of the closed form, the run
    reporting six modal coordinates). TestGuiCommands' test_flexible_body
    (the command idle until a component is picked, the body in the Dynamic
    study only, one undo). FlexibleBody.cantileverModesAreTheClampedBeams
    now with every other tetrahedron turned the other way, the same modes.
  - Not yet: the reduction is made again at each run (5 s for the rod; its
    modes kept since, step 5's last entry); a task panel of its own, the
    modes' frequencies shown before the run (step 6); the deformation and
    stresses (step 4).
- Built, D4b step 4 (first half): the deformation, drawn as the run plays.
  - **What the run reports for it**: each frame, beside the modal
    coordinates, its interfaces' own deformation ("InterfaceDeformation",
    six an interface, mm and rad: each boundary body's pose in the floating
    frame against its pose there at the start, as Chrono's
    GetLocalDeformations() has them); and once, before the first frame,
    its mode shapes (`Solver::ModeShapes`, a new TrajectorySink call):
    the reduction matrix's rows of the mesh's nodes, Psi, turned from the
    floating frame's axes into the body's, in mm per mm, rad or modal
    coordinate. A node's displacement in the body's frame is Psi times the
    two channels together; the body's pose is the floating frame's, so a
    node is where the pose puts it, displaced. The session wrappers pass
    them on (in the body's axes, which neither turns).
  - The document keeps them on the flexible body (`ModeShapes`,
    `ModeShapeColumns`, hidden: a list that size is saved as a file in the
    archive), from `keepFrames()`: in its component's axes, which are its
    body's.
  - **The player draws it bent** (`FlexibleBodies.Deformation`): the mesh's
    outer faces, each 6-node triangle as four, turned outward; at the time
    shown -- between two frames, the coordinates interpolated -- moved by
    the shapes and the coordinates, scaled up for the largest over the run
    to be a tenth of the part's size (or by the body's `BendingScale`,
    step 12), in place of the component, which
    hides while it is drawn and shows again when the panel closes. Save
    Animation's frames have it.
  - The panel's Elements lists the flexible bodies.
  - The manual's video (`assembly/simulation/flexible-bodies`): an
    aluminium strip 200 x 20 x 2 hinged at one end, let go level -- 31130
    nodes, the run of 0.3 s in 601 frames 90 s -- bending as it swings,
    7.5 um at most, 3 ms in, drawn 2664 times; the hinge's force ringing
    at the strip's 180 Hz as it starts.
  - Tests: FlexibleBody.cantileverBendsAndRingsInARun -- the shapes and the
    coordinates make the tip's node where the gram welded to it is, within
    1e-3 of the sag, and the mid-span node's sag the beam's, q x^2 (6 L^2 -
    4 L x + x^2) / 24 E I + P x^2 (3 L - x) / 6 E I, within 2%;
    TestFlexibleBody.test_cantilever_deformation (the rod fixed to the
    grounded block, flexible, not placed on the ground: its end, made of
    the shapes, sagging q L^4 / 8 E I within 3%, its hold not);
    TestGuiCommands' test_flexible_body_playback (the surface in the scene,
    the component hidden, moved at each frame, both undone on closing).
  - Not yet: the stresses (the second half); the run's cost on a fine mesh,
    90 s for the strip's 601 steps, much of it the modal assembly's own
    work at each step over the whole mesh.
- Built, D4b step 4 (second half): the stresses, coloured as the run plays.
  - **Recovered from the mode shapes, after the run** (`FlexibleBodies.
    modalStresses()`), as Adams and MotionSolve do: each coordinate's
    stress field -- the small strain of its shape, by the 10-node
    tetrahedron's shape functions' gradients at each of its nodes, linear
    elastic, each node's the mean of its tetrahedra's -- at the surface's
    nodes, then weighed by the run's coordinates at the time shown. A rigid
    turn's linear strain is none, so an interface's column that only
    carries the part is stress free. The material the run's
    (`getMassProperties()`' Young's modulus and Poisson's ratio). numpy;
    0.3 s for the rod's 2915 tetrahedra, kept while the shapes are the same
    so the player and the Results box share it.
  - **The player colours it** by its von Mises stress, blue to red as the
    Fem workbench's results, red the largest over the run of all the
    study's flexible bodies; a legend at the view's right (`Legend`) with
    the scale in MPa and how many times the bending is drawn, on a light
    backing of its own for any background. Painted by Qt: a widget over
    the view, and Save Animation paints it into its frames at the same
    place (`videoLayout()`, `videoFrame()`). Not in the scene, because
    with the bgfx backend on, nothing in the viewer's foreground root -- a
    view provider's front root, where Fem's colour bar goes -- reached a
    saved picture or the backend's own frame dump, not even the corner
    axis cross that feed was built for (a square of plain
    SoIndexedFaceSet, probed 2026-11-06); and a camera nested in the scene
    graph is not one the render cache takes. A picture saved by Save
    Picture is without it (has it since step 10).
  - **The Results box** has each flexible body's largest von Mises stress on
    its surface at each frame ("FlexibleVonMises", MPa), and the CSV and the
    spreadsheet with it.
  - Test: TestFlexibleBody.test_cantilever_stress -- the rod clamped to the
    grounded block, settled under gravity, 12 modes: its top and bottom
    sigma_xx the beam's q (L - x)^2 / 2 c / I within 1.3% from 10 to 70 mm
    off the hold (2% asked); its largest at the hold, 1.06 times the beam's
    there, the hold's corners; the channel each frame's largest.
    test_pendulum_stress -- the hinged rod let go level, 20 ms in, its
    ringing gone: its bending, half the difference of top and bottom (the
    swing's pull along it adds to both), a bar's turning about its pin
    under gravity, M = rho A g x (L - x)^2 / 4 L, concave up, within 1.7%
    at 24 modes (at 12, 4% at L / 3). TestGuiCommands'
    test_flexible_body_playback, the colours, the legend, the video's
    layout and the channel.
  - The manual's video (`assembly/simulation/flexible-stress`): the
    flexible-bodies page's strip, coloured, 0.203 MPa at most 3 ms in (the
    release's ringing, 1.73 times), then the swing's 0.1145 MPa at 75 ms
    against the turning bar's rho A g L^2 c / 27 I cos(theta), 0.1152.
- Built, D4b step 5: a contact on a flexible part.
  - **Through the faces it is touched on** (`FlexibleBody.ContactFaces`,
    faces of its component): one interface of their own, its nodes tied to
    its boundary body as a joint's are, which every contact shape on the
    part rides (each shape's frame where it is on that body,
    `frameLocal`). A contact's forces act on that body and bend the part
    through its static modes, implicitly, as a joint's do; the faces move
    together, so they are a patch -- a pad, a tip, a tooth's flank -- not
    a long face that bends. `FlexibleBodies.prepare()` makes the interface
    (labelled "Contact" in InterfaceFrames), less the nodes a joint's
    interface holds: two interfaces sharing nodes would hold each other. A
    contact on a flexible part without them was refused, saying so; it is
    a contact anywhere on the part now (step 8).
  - Why not the mesh's own nodes, as Adams' contact on a flexible body
    has it: the modal assembly takes a force on an internal node (its
    projection on the modes, `Psi^T f`), but only in the residual, and a
    metal contact's stiffness is a hundred times and more a gram's mass
    over a step of 0.1 ms squared -- Newton's iterations, without the
    coupling, run away on it (an estimate, not tried). The
    interface is the static modes RecurDyn's RFlex takes for a contact
    whose place is known; a contact anywhere on a part, its full nodal
    mesh (FFlex, Chrono's ChContactSurfaceMesh), when a case needs it.
    (Step 8 takes the mesh's own nodes after all, with that coupling.)
  - The contact interface's body at its nodes' centre, as a joint's is on
    its face: at the contact shape's frame -- a mesh's, the part's origin,
    the hinge's end of a strip 200 mm from the pad it was tied to -- the
    strip striking a post by Hertz's law diverged, where at the centre it
    ran (both before the floating frame below was set back).
  - **Found on the way: a substep taken again lost the floating frame.**
    The contact's substeps that run off are taken again from their start
    (`takeBack()`, the state scattered back), and a modal assembly's
    floating frame is not in the state: its update iterates from where it
    is, and a substep that ran off left it not finite -- as the strip left
    the post, every substep after one that diverged failed, down to a
    nanosecond ("the contact's integration diverged"). Each flexible
    part's floating frame is now set back with the state (our Chrono's
    `SetFloatingFrameOfReference()`), and a substep's state is taken as
    not finite by the whole system's, its modes too, not by the parts'
    bodies, a flexible one's a proxy that follows.
  - What a contact load reads of its bodies' masses -- the stiffness from
    an allowed penetration, Hunt and Crossley's speed, Johnson's line
    depth, its period for the substeps, a late contact's depth -- is the
    part's on a flexible side (`ContactLoad::carried`, `lighterMass()`),
    not the interface body's millionth of it. The part's proxy body has
    the mesh's mass too, for the run's lightest body.
  - Test: TestFlexibleBody.test_propped_cantilever -- the rod clamped to
    the grounded block, a pad under its free end resting on a grounded
    post, the pad's face its contact face: under its weight the post takes
    a propped cantilever's prop, q (6 L^2 - 4 L a + a^2) / 8 a and the pad's
    weight, 106.4 mN against 104.9 (1.4%), the clamp the rest, where a
    rigid rod's prop takes nothing (its Fixed joint holds it exactly, the
    penalty contact never presses); test_strike -- a strip 200 x 20 x 4
    hinged, a pad under its end, let go level onto a post 10 mm below, at
    an allowance of 1e-4 mm: it strikes, bounces and the run goes through
    (without the floating frame set back it diverges as it leaves), the
    blow softer than the rigid strip's (by Hertz's law, 69.7 N at most
    against 283 N); test_contact_needs_its_faces (test_contact_anywhere
    since step 8).
  - **Stops and joint friction on a flexible part** (2026-11-07): their
    substeps -- a penalty stop's period on its spring and damper, a
    friction's time to cross the stiction band -- read the bodies on the
    joint's two sides, and on a flexible part that is an interface's
    boundary body, whose mass reads 0 in the run (a millionth of the
    part's when built). A stop between the flexible rod's hinge and the
    ground set no substep: let go level onto an undamped stop of 10 N m
    per radian 30 deg down, the rod came back 0.275 deg above where it
    fell from, the rigid rod 0.022. Both now read the part's proxy
    (`carrier()`, `carrierOf()`, which `carriedMass()` shares), synced to
    the floating frame first; and the proxy takes the part's inertia at
    the mesh's mass -- it had Chrono's default, 1 kg m^2, which made that
    stop's period 1.99 s for its 19 ms, and a friction's mass about the
    pin 4e4 kg for 3.6. The rod now comes back to 0.025.
  - Tests: TestFlexibleBody.test_stop_bounce_on_flexible_part (the bounce
    above, the flexible within 0.1 deg of the rigid);
    test_stop_on_flexible_part (damped near critically, it rests where
    the rigid rod does, k (a - 30 deg) = m g L cos a, within 0.01 deg);
    test_friction_on_flexible_part (the hinge preloaded 3 N mm -- its pin's
    own friction on the hinge's reaction is smaller than the substeps'
    spread over 50 ms -- the swing slowed as the rigid one's, within
    0.05 deg at each frame).
  - The contact's rest correction (`meanAccelerations()`) leaves out a
    flexible side, as it leaves out a body on a joint: its interface body's
    accelerations are held by the modal assembly, which a change to them
    would not keep (it has columns there, but no mass, no means: not the
    run's bodies, its part's proxy is).
  - Not yet: a contact anywhere on a part (above) -- done, step 8.
  - **The reduction's cost** (2026-11-07), measured by a trace of
    `DoModalReduction()`'s steps on the pendulum's rod (16272 internal
    coordinates, 6 modes, 0.25 s run in 9.6 s, 6.8 of it the reduction):
    the full K, M and Cq assembled, 1.24 s; turned into the floating
    frame, 1.56 s; the eigenproblem, 1.94 s; the static and dynamic modes
    (K_II's LU and a solve each), 1.52 s. Two things taken off:
    - The damping matrix assembled again and turned, for a full_R_loc
      nothing reads (our Chrono, `ComputeLocalFullKMCqMatrices()`): its
      1.56 s are 0.88.
    - **The modes kept for the next run** (`KeptModesSolver`, the last four
      solved in the process): a set of the same size is taken when its
      pairs solve the problem, A v = lambda B v to 1e-6 -- 1.94 s to 0.10,
      the residuals' check -- not keyed by the record, so a part changed
      makes its own.
    - **A part started elsewhere takes them too** (2026-11-09): the
      floating frame's axes were the part's principal axes of inertia,
      their order and signs (and the square section's two) as the pose's
      rounding had them, and the problem is in those axes -- the rod
      started 25 deg down solved again, 8.9 s. They are the part's own now
      (our Chrono's `SetFloatingFrameAxes()`, from `ChronoFlexible::
      build()`), F's origin still the centre of mass: the same part
      anywhere has the same local matrices. Nothing read F's axes as
      principal ones -- the shapes, the interfaces' deformation and the
      stresses go through F whatever its axes.
    - The rod's second run of an unchanged study: its reduction 6.8 s to
      4.5, the whole 0.25 s run 9.6 to 7.2; started 25 deg down, 7.1 (8.9
      before its axes were its own). A mass-only element pass (the corotated K not made for the M)
      was tried and took nothing: the assembly's time is not the
      elements'.
    - **The whole reduction kept** (2026-11-09): our Chrono's
      `ChModalAssembly::GetReducedData()` gives what a reduction made --
      Psi, its static and dynamic modes, the reduced M, K and R -- and
      `DoModalReduction(const ReducedData&)` reduces with them, its M, K
      and Cq not assembled, its eigenproblem and its static and dynamic
      modes not solved. `ChronoFlexible::reduce()` keeps the last two,
      each by what it is made of in the part's frame (`Built::Key`): its
      tetrahedra, material, modes, damping and interfaces' nodes exactly,
      its nodes and interfaces' frames to 1e-9 of its size -- a part turned
      has them an ulp off (the rod started 25 deg down: 0.05 m as
      0.05000000000000001, found by a trace after the frames alone were
      let off, and it still missed). Its pose is not in it: F's axes are
      the part's. A part damped otherwise makes its own reduction, from
      the kept modes.
    - **The modal gravity by three columns** (our Chrono,
      `ComputeGravityMass()`): it was the whole local mass times the
      gravity on every item's translation at each residual, 1.5 million
      terms for the rod; it is the mass's three columns of a unit
      acceleration along F's axes times the gravity in them, which the
      kept data carry in place of the mass.
    - The rod's 0.25 s run of a study run before: 1.9 s, 2.0 at 12 modes
      (7.2 with the modes kept, 9.6 at first); started 25 deg down, 1.9; a
      1 ms run 0.46 s (4.4). Its first run, 4.4 s at 6 modes, 6.4 at 12.
    - **A fine mesh's steps** (2026-11-09): the manual's strip (31130
      nodes, 12 modes, 0.3 s in 601 frames) ran in 88.6 s, 31.7 run again
      -- its steps, at every residual, went over the whole mesh in our
      Chrono's `IntLoadResidual_F()`: each internal node cast twice to its
      type for its force, and the internal part's gravity made as a vector
      of 93390, turned and reduced by Psi_S and Psi_D. That gravity is
      constant in F's axes: reduced once at the reduction's end
      (`ComputeGravityReduced()`, [Psi_S; Psi_D]^T times the internal
      items' gravity columns), each residual takes it times the gravity in
      F's axes; each node's type is found once (`SetupItemCoords()`); a
      force on an internal node is still reduced at each residual, and
      the static correction mode, which takes the gravity among them,
      keeps its way. The strip 64.7 s, run again 6.5; the rod's 0.25 s run
      again 0.7 s (1.9).
    - **A fine mesh's reduction** (2026-11-09): the strip's, traced (a
      temporary trace, gone since): its full K, M and Cq assembled 8.5 s;
      turned into F's axes 32.6; the eigenproblem 10.5 (its shift's LU
      6.2, the iterations 3.8); K_II's LU for the static and dynamic modes
      6.3 and their solves 2.0. The turning was Chrono's general sparse
      products L^T K L, L^T M L and Cq L for an L that turns each item's
      translation by R_F and keeps the rest; it is now made block by block
      in one pass over the rows (our Chrono's `TurnToLocal()`, R_F^T B R_F
      for each 3x3 block of two translations), 0.17 s, the same 6.39
      million terms -- and for the rod turned about a skew axis (R_F not
      the identity) the same matrices within 1.2e-16 of their norm, the
      trace comparing the two. The strip's first run 64.7 s to 33.0; the
      rod's 1 ms first run 3.2 s to 3.0.
    - **K_II factored once** (2026-11-10): the eigenproblem's shift-and-invert
      and the static modes each factored the same internal stiffness (6 s
      apiece for the strip). Our Chrono's Craig-Bampton reduction factors
      K_IIc first and hands it to the Krylov-Schur eigensolver
      (`shift_inverse`), which takes it for a solve at a zero shift: its
      A = [-K_II -a Cq_II'; -a Cq_II 0] is -D K_IIc D, D scaling the
      multipliers by a over the static modes' own Cq scale. The modes are
      found about 0 Hz now, not 1e-5 (`reduce()`): a K_II the static modes
      solve with has no zero frequency to shift off, and the shift was
      (2 pi 1e-5)^2 = 4e-9 against the first mode's w^2. The strip's first run 33.0 s
      to 26.6; the rod's 1 ms first run 3.0 to 2.2 (12 modes 5.5 to 4.3).
    - **The full K and M** (2026-11-10), traced (a temporary trace, gone
      since): 7.2 s, 4.8 of them Chrono's sparsity learner, a list node per
      element term sorted row by row, for K, M and Cq each (2.13, 2.11,
      0.55); the mass pass 0.61, each tet10 making its corotated K to scale
      it by 0; pasting into K and M 0.71 each. Our Chrono's learner keeps a
      vector a row, sorted once (0.39, 0.63, 0.02), the same pattern; our
      tet10 asked for its mass alone skips its K (0.04). The strip's K and
      M 7.2 s to 2.7, its first run 26.6 to 22.4; the rod's 1 ms first run
      2.2 to 1.8 (12 modes 4.3 to 3.3).
  - Test: FlexibleBody.aPartsModesAreKeptForItsNextReduction -- the bar
    reduced again takes the kept reduction, the same frequencies to the
    bit; moved and turned, its interface's frame and a node an ulp off,
    takes it too; damped twice as much, a reduction of its own from the
    kept modes; twice as stiff, its own, sqrt(2) times.
    FlexibleBody.aRunAgainTakesTheKeptReduction -- the flexible pendulum
    run again takes the first run's, and swings the same: each frame,
    each channel to 1e-9, the mode shapes to the bit.
  - Not yet: the kept reductions across sessions (they are the process's;
    done, step 9);
    a part's first run, the rod's 1 ms run 1.8 s where the rerun's is 0.2,
    the strip's reduction about 16 of its 22.4 -- the
    eigenproblem's factorization (6 s, Eigen's SparseLU) and iterations
    (3.8), K_II's static and dynamic solves (2), pasting K and M (1.4).
  - Not yet: near a free end, where the stress is a hundredth of the root's,
    the modes left out take a fifth of it (6 modes, a third): stress takes
    more modes than shape, and a static correction for the loads on the
    mesh -- gravity, inertia -- would give it back (done, step 7); the
    stresses inside the
    part (the surface's only, where the largest is but for a load inside);
    To FEM's vibration from the run's own coordinates.
- Built, D4b step 6 (2026-11-10): the flexible body's task panel
  (`CommandCreateFlexibleBody.py`, `TaskFlexibleBody`), which the command
  opens on the body it makes and a double click opens on one
  (`ViewProviderFlexibleBody::doubleClicked()`).
  - **Part**: the component, and its material as the run takes it (the
    elastic material's name, E, Poisson's ratio, density; "assumed" where
    none is chosen). **Modes**: how many, the damping (in %), its
    frequency; **Find Modes** meshes the part if it must and reduces it as
    the first Dynamic study using it would, and lists the frequencies --
    the reduction kept, so the run after it takes it.
  - **Mesh**: the size, and the size its 0 stands for
    (`FlexibleBodies.defaultMeshSize()`); the mesh made, and whether it is
    of the part as it is now (`meshState()`: none, current, stale -- the
    signature `prepare()` keeps); **Mesh Now**; **Show the mesh**, its
    surface's edges over the part and each interface's nodes (orange a
    joint's or a force element's, green the contact faces'); what holds it,
    each interface by its joint's label and its count of nodes.
  - **Contact faces**, picked on the part in the 3D view (a gate lets only
    its faces through), removed or cleared: set by script alone before.
  - **The frequencies' entry**, `AssemblyObject.getFlexibleFrequencies(sim,
    flexible)`: the model built as the study's run builds it
    (`buildSimulation()`), its interfaces at that run's frames, then
    `Chrono::flexibleFrequencies()`. Refused while a run of the assembly is
    on: its model is the run's.
  - **Found on the way: meshing ends the panel's transaction.**
    `prepare()` makes the mesh in a document of its own, and the work there
    replaces the application's active transaction, committing the panel's:
    Cancel after Mesh Now undid nothing (the test's Modes stayed 4). First
    worked round in the panel (its transaction opened again after meshing,
    Cancel undoing back to where it began); then removed at the source: the
    one way to Netgen was `Fem::FemMeshShapeNetgenObject::execute()`, a
    document object's recompute, so the Fem module now has it without one --
    `FemMeshShapeNetgenObject::meshShape(mesh, shape, parameters)`, which
    `execute()` calls, and `Fem.meshNetgen(shape, MaxSize=..., ...)` for
    Python, a FemMesh whose `getNodesByFace()` takes the shape's own faces.
    `prepare()` meshes by it: no document, no copy of the shape, nothing
    of the application's transaction touched; the panel's workaround is
    gone.
  - Tests: TestGuiCommands -- test_flexible_body (the command opens the
    panel; OK keeps the body), test_flexible_body_cancelled (Cancel leaves
    none), test_flexible_body_panel (the rod's panel by a double click: the
    material, Modes, Damping and Mesh Size set, Mesh Now and the hinge's
    interface, the mesh drawn in the scene, a contact face picked and
    another part's refused, Find Modes' four frequencies lowest first,
    Cancel undoing it all); TestFlexibleBody test_mesh_in_no_document
    (`prepare()` makes no document and leaves an open transaction open,
    which aborting then undoes; `Fem.meshNetgen()` the Netgen object's
    mesh, its counts of nodes and of tetrahedra).
- Built, D4b step 7 (2026-11-10): the stress near a free end, by the
  inertia loads' residual modes (step 4's "not yet").
  - **What was missing.** A part's stress, recovered from its mode shapes,
    is only what its kept modes make of the loads on its mesh; the loads
    spread over it -- gravity, and the inertia of its own motion -- also
    load the modes left out, which a stress takes more of than a shape
    does. The clamped rod settled under gravity at 6 modes: 13% of the
    beam's stress missing 70 mm out, 21% at 80, 34% at 90 (at 12 modes 1%,
    7%, 20%); the hinged rod swinging, at 12 modes, 4% over at L / 3 and
    20% at 90 mm, which 24 modes took to 1 and 4.
  - **The mode-acceleration method, for the loads a rigid motion makes.**
    Their field over the mesh is g - a - alpha x r - w x (w x r) per unit
    mass, in the floating frame's axes, r from its origin: twelve fixed
    fields -- the three axes, the three e_k x r, and six of r's components
    the centripetal term takes -- times twelve coefficients the run has at
    each moment: g - a, -alpha, and w's products. Our Chrono's
    Craig-Bampton reduction makes, for each field b, the internal nodes'
    static displacement with the interfaces held, K_IIc^-1 {M_II b; 0}, by
    the factorization the static modes use, less what the kept modes take
    of it statically, Psi_D K_DD^-1 Psi_D^T M_II b (K_DD the reduced
    stiffness's modal block: what the reduced model at rest under the load
    has): `SetUseInertialResidual()`, `GetInertialResidualModes()`, in the
    kept reduction's data too. They are not in the reduced model: its run is
    as it was; they are the stress's and the drawn shape's, which take the
    residual's static share at each moment.
  - **What the run hands over**: twelve more columns in the flexible body's
    ModeShapes (mm per coefficient, SI), after its interfaces' and its
    modes', and a channel "InertialLoad" of the twelve coefficients,
    after "ModalCoordinates" -- `FlexibleBodies.Deformation` takes it as
    the columns' coordinates. The floating frame's accelerations at the
    start are not its motion's, the state not stepped yet: the start's
    coefficients leave them out, the gravity and w's products alone. Those
    the frame held there read 122 rad/s^2 for a clamped rod at rest, and a
    line through the smallest steps' (as a reaction's start) 83 m/s^2 along
    it, the reduced model's first steps settling.
  - The rod clamped, settled, its top and bottom against the beam's:
    within 0.4% from 10 to 50 mm, 1.1% at 70, 2.4% at 80 -- at 6 modes as
    at 12, to the digit: the modes no longer matter to it. What is left
    nearer the end, 9% at 90 mm and 29% at 95, is the part's against the
    beam's, a depth from a free end, where the beam's is not the solid's.
    The hinged rod at 12 modes: within 2.3% from 10 to 90 mm, as 24 modes
    with them; at 95 mm 16% under at either.
  - Cost: twelve solves by K_IIc's factorization at a reduction, and
    twelve columns over the mesh: the strip's ModeShapes 18 columns to 30.
  - Tests: TestFlexibleBody.test_cantilever_stress (6 modes now: the
    beam's out to 80 mm within 3%, without the residual modes' columns 13%
    and 21% under at 70 and 80; the coefficients the gravity at the start
    and the end); test_pendulum_stress (12 modes now, out to 90 mm, 3%);
    test_cantilever_deformation (24 columns); FlexibleBody.
    cantileverBendsAndRingsInARun (30 columns, the channel after the modal
    coordinates, the gravity at rest).
- Built, D4b step 8 (2026-11-10): a contact anywhere on a flexible part.
  - **Without the faces it is touched on** (`ContactFaces` empty), a contact
    on a flexible part presses its mesh where it touches, as Adams' contact
    on a flexible body does. The part's contact shapes ride its first
    interface's boundary body, the carrier, on which collision detection
    finds their points as it does any body's; each point found is put on
    the mesh's surface -- the tetrahedra's faces no other shares, the
    nearest one's corners and the point's weights on them, by a grid of
    them (`ChronoFlexible::Surface`) -- where it is with the part at rest.
    As the run goes each point is where the part's modes have it: its
    position, velocity and map T onto the part's reduced coordinates from
    our Chrono's `ChModalAssembly::GetInternalNodesMap()` -- the nodes'
    F + R_F (r0 + Psi_S e_B + Psi_D eta), and T the transpose of the map
    `IntLoadResidual_F()` takes the internal nodes' forces by, 3 x (6 per
    boundary + modes), whose product with the reduced velocities is the
    node's velocity.
  - **The coupling the step 5 entry missed** (`FlexibleContact`, a physics
    item per such part, in the system after its modal assembly and before
    the loads, so its update puts the points where the state has them before
    a load reads them): the points' forces go to the reduced coordinates as
    T^T f, not to the carrier, and their stiffness and damping along the
    normal with them, T^T (k n n') T and the blocks to the other side's
    body, -T^T k n n' J and its transpose, J = [I, -R_A [r]x] -- a KRM
    block over the reduced variables (`GetReducedVariables()`) and those
    bodies'. The other side's own block stays the load's numerical
    Jacobian; the load reads the point's place and velocity from the modes,
    not from the carrier, so moving the carrier in that Jacobian moves
    nothing and nothing is counted twice. k is the law's rate with depth,
    e times the spring over the depth, the share and the damping's factor
    held; c its rate with the depth's rate. Without these a force on the
    mesh is in the residual only, as a node's load is.
  - A flexible part touched anywhere is the load's B; the set's forces are
    on its first group's side, so a load made the other way round says so
    (`ContactLoad::swapped`) -- the first run read the prop's force
    reversed. Two flexible parts touching each other anywhere are refused:
    one needs its faces named.
  - The shapes are found on the carrier rigid, the part's deformation then
    taken in at each point: a contact shape that bends farther from the
    carrier than collision detection's envelope (2e-5 m) before it touches
    is found late, by the step that goes too deep (advance()), as a
    contact it does not see coming is. A metal part bends microns -- a
    stocky one: a strip does not, and step 11 looks for the surface
    farther ahead. Since step 12 the shape is the surface itself, moved
    by the modes.
  - The rod clamped, its pad resting on the post, the pad's face not named:
    the prop takes 106.6 mN for the propped cantilever's 104.9, 1.7% over
    (through the faces 1.4%), clamp and prop the weight within 0.1%, the run
    5.2 s. The strip struck on the post: its blow 132.9 N at most anywhere,
    94.1 through the pad's face, 409 N rigid -- the pad's own patch moves
    as one through the faces, and anywhere its nodes take the blow by the
    modes alone, stiffer.
  - Not in it: a stress near the contact from it (the modes and the
    inertia loads' residual modes recover the deformation, not a contact's
    local static share: no static correction for it); a cam or a profile
    contact on a flexible part.
  - Tests: TestFlexibleBody.test_propped_cantilever_anywhere (the prop
    within 3%, clamp and prop the weight within 0.1%),
    test_strike_anywhere (the run goes through, the blow under the rigid
    strip's), test_contact_anywhere (test_contact_needs_its_faces's case
    runs).
- Built, D4b step 9 (2026-11-11): a flexible part's reduction kept in the
  file.
  - **The last run's reduction is the flexible body's** (`Reduction`, a
    hidden float list in the Mesh group, written as a binary file of the
    FCStd), and a run hands it to the backend (`Flexible::reduction`),
    which takes it where the part is as it was -- its kept reductions
    (step 5's) seeded with it -- so a part's first run in a session goes
    as a rerun does. The rod's: a process that opens the file runs in
    0.54 s, one with the file's dropped makes the reduction again in
    2.31; the mode shapes the same to the bit either way.
  - Its numbers (`ChronoFlexible::reductionNumbers()`): a format (1), the
    key's hash as two 32-bit halves, the modes, the reduction's scaling,
    the frequencies, then each of Psi_S, Psi_D, Psi_S Lambda^-1,
    Psi_D Lambda^-1, M, K and R reduced, the gravity's M columns and
    Psi_R as rows, columns and data. Psi itself, [I 0; Psi_S Psi_D;
    Psi_S_L Psi_D_L], is made again of them, not kept. The rod's (6
    modes, 24 columns with the residual ones) is 443028 numbers, 3.5 MB
    of its 7.8 MB file.
  - The key's hash is FNV-1a over the key the kept reductions are found
    by: the exact numbers by their bits, the near ones (nodes and
    interface frames over the part's size) rounded at 1e-9, sameReduction()'s
    tolerance. One at a rounding's edge misses, and the reduction is
    made again; a file's from another part (another mesh, material,
    modes, damping or interface) is not taken. A new mesh (prepare())
    empties it.
  - **Found by the test: an interface held nothing in a document opened
    from a file of another name.** InterfaceFrames keeps each joint's
    full name, the document's in it; opened from kept.FCStd the document
    is "kept", no frame matched, the interface's nodes were none and the
    part ran free, with no word. The frames are matched by the joint's
    name in its document now (the contact faces' label has no document
    and is no joint's), and the mesh's signature takes that name too, so
    the mesh is not stale and not made again for it.
  - A Python joint converted to the C++ one on opening (ConvertJointsOnOpen)
    is another joint, of another name: the part's mesh is stale, and made
    again with its reduction when the study runs from its panel
    (prepareStudy()). A script's run before that has the part held by
    none of its nodes there, and says so now: a joint's frame on a
    flexible part its mesh has no nodes for is warned of.
  - Tests: TestFlexibleBody.test_reduction_kept_in_the_file (saved, opened
    under another name, the mesh current, the reduction as saved, the run
    the same mode shapes and coordinates to 1e-9; a new mesh empties it);
    FlexibleBody.aRunTakesTheReductionItsModelHands (this process's
    forgotten, a run handed the numbers takes them and swings the same,
    each frame and channel to 1e-9; a stiffer part's are not taken).
- Built, D4b step 10 (2026-11-11): the stress legend in Save Picture.
  - **A widget over the 3D view can ask to be in its picture**: a dynamic
    property `pictureOverlay`, true. `saveImage(..., overlays=True)`
    (keywords now, the old positional calls as they were) paints each
    such child of the viewer, shown, into the picture after it is taken
    -- `QWidget::render()` of its own painting, no background, where it
    is on the view: its centre at the picture's scale of the view on each
    axis, itself scaled by the smaller of the two. Save Picture
    (Std_ViewScreenShot) passes it, so a macro it records does too; the
    legend's widget sets the property. Save Animation paints its own
    frames' legend (`videoFrame()`) and does not pass it.
  - Painted after the capture, not into the scene, for step 4's reason:
    on the bgfx backend nothing in the viewer's foreground root reaches
    a saved picture. The backend's readback is at the view's size and
    scaled for a larger picture; the overlay is painted at the picture's,
    sharp at any size.
  - Test: TestGuiCommands.test_flexible_body_playback -- the picture at
    the view's size and at twice it, the legend's bar red at its top and
    blue at its bottom where it is on the view; without overlays, not.
- Built, D4b step 11 (2026-11-11): a flexible part bent off its carrier
  keeps its contact; a run is stopped inside a frame.
  - Found making the manual's video of step 8: an aluminium strip
    200 x 20 x 2 hinged at an end, let go level onto a grounded bar 10 mm
    across lying 10 mm under it, 120 mm out, no contact faces. The run got
    past the first blow (336 N at 47.5 ms) and then crawled, 0.4 ms in
    four minutes, the panel showing a run under way and Cancel doing
    nothing. Five faults, in the order they were found.
  - **A run is stopped inside a frame** (`TrajectorySink::cancelled()`, read
    at each substep of advance()): Cancel was seen only as a frame was
    given, and a contact's substeps can take minutes a frame. And a run
    that will not get anywhere says so: 100000 substeps kept in a row, each
    under 10 ns, fail the run with "the contact's integration stalled at
    ... s". The panel's poll outlives its assembly when a document closes
    under a run (`pollRun()` takes the ReferenceError).
  - **A flexible part's forces after a step taken back were the late
    points'.** A load's forces on a flexible part are kept by its points'
    numbers (`forcesOnB`) for `FlexibleContact` to put on the reduced
    coordinates. A step taken back from a contact found late left them as
    computed at the late points, and collision detection run again at the
    state before gave other points or none: the next step read 7.6 kN on a
    strip of 21 g as its start's (HHT's share of the old forces). The
    finder computes a flexible load's forces again at the points it has
    just found, and the two readers skip a number past the points.
  - **The surface's speed, not the carrier's**, sizes a substep and judges
    a contact found late (`ContactLoad::speedOnB()`): the strip came back
    onto the bar 0.2 mm off its carrier's shape and still bending.
  - **The carrier's shapes lag the surface, and were looked for too
    near.** The shapes ride the first interface rigid; after the first blow
    the strip's surface at the bar was 0.2 to 0.45 mm ahead of them, the
    contact was lost as the shapes left the bar by the run's reach (0.1 mm)
    and found again with the surface 0.18 mm in: 62 kN, where the blow is
    336 N. Step 8's "a metal part bends microns" holds for a stocky part,
    not for a strip. Now a flexible part's carrier is looked for ahead by
    `FlexibleContact::reach`, a hundredth of the diagonal of the part's
    nodes' box (the strip's 2 mm), as its collision model's envelope:
    - Chrono's Bullet shapes are grown by the model's envelope and its
      report takes the envelope back off each point, so the points are the
      true shapes' with a pair kept from that far off.
    - Our polyhedral features (box, hull, a mesh's triangles) are grown by
      the run's envelope (2e-5 m) only: grown by the wide one they stand as
      far sideways too, and a bar across the strip's 20 mm touched it
      along 21. They keep how far inside Bullet's shape they stand
      (`SatPolyhedron::inset`); the clip searches that much farther and
      gives its points as Bullet's shapes'.
    - The finder's own clips (a cylinder on a face or a mesh, the hollow
      rounds, a seat's ring) reach farther by the two shapes'
      `ShapeOn::search`.
    A point found ahead presses nothing until the surface is there: each
    point's depth is the surface's, by the modes.
  - **Points found ahead whose surface is not there are no contact's
    yet** (`FlexibleContact::trim()`, in the finder before the points'
    patches are made). Kept, they shared their load's stiffness and its
    patches' extent: step 8's propped cantilever, a rod 100 x 10 x 10
    resting a pad 1 mm proud on a post, had its whole underside found
    1 mm over the post's top (its reach 1.01 mm) and the post's force came
    5% light, 99.6 mN for 104.9. A point stays when its surface, by the
    modes, is within the run's own reach (0.1 mm) of the other body's
    point along the normal.
  - Limits. The surface must stay within the reach of the carrier's shape,
    and well within the part's thickness of it (the shape is the rigid
    part's: bent a thickness off, its other side is where the surface
    was). A contact still found deep, no shorter step finding it sooner,
    is said once a part: "the contact of ... on the flexible body ... was
    found ... mm in". The proper mend is collision shapes that follow
    the modes -- the surface's triangles placed by the nodes each step --
    and is step 12.
  - The strip on the bar now (mesh 8, 9052 nodes, 30 coordinates): the
    blow 337.5 N at 47.50 ms, three lighter ones within 0.2 ms (156, 42,
    13 N), then the strip stays on the bar 3.3 ms, pressing 0.2 to 6 N,
    bent 0.45 mm off its carrier's shape, and leaves at 51 ms; 76 ms run
    in 13.6 s. The manual's (mesh 3, 24296 nodes, 0.15 s): 336.2 N, 12
    strikes, 0.16 N at the end for the 0.18 N of its weight's moment
    about the hinge, 840 frames in 158 s.
  - Tests: TestFlexibleBody.test_bent_onto_bar_anywhere (the force never
    over the blow's, the strip pressing lightly a millisecond after, no
    late contact said; 62 kN before); test_propped_cantilever_anywhere
    holds the trim (106.6 mN as in step 8); TestContact.
    test_cancel_within_a_frame (a ball bouncing 1000 s, one frame,
    cancelled half a second in, done within 5 s). The strike with no
    contact faces is 129.6 N now, 132.9 in step 8.
- Built, D4b step 12 (2026-11-11): a flexible part touched anywhere is
  touched where it is, however far it has bent -- step 11's limit.
  - **The part's surface is its collision shape, and moves with the
    modes** (`FlexibleContact::makeMesh()`, `moveMesh()`). The triangles of
    `ChronoFlexible::Surface` -- the tetrahedra's faces no other shares,
    now wound to face out of the body, a collision triangle being one
    sided -- are one `ChCollisionShapeTriangleMesh` on the carrier, in
    place of every shape the part had there. As each collision detection
    starts the vertices go where the modes have their nodes
    (`ContactCollisionSystem::addMovingMesh()`, `moveMeshes()`): Bullet's
    triangles read the vertices as they are, and what it keeps of them is
    brought along -- each triangle's box in the carrier's tree, five
    envelopes wider when it has left the one it had, the carrier's own
    box, the triangles' features. Chrono's fork gathers the nodes' shapes
    once (`ChModalAssembly::GetInternalNodesShapes()`) and places them all
    by one product a step (`GetInternalNodesPos()`); the maps of step 8
    are asked only for the points found.
  - A point found on one of the mesh's triangles is kept by the triangle's
    nodes and its weights on them (`FlexibleContact::place()`), not looked
    up on the surface at rest: it is where the triangle is. Step 11's
    search ahead is gone with what it needed -- `FlexibleContact::reach`,
    `trim()`, `ShapeOn::search`, `SatPolyhedron::inset`, the "found ... mm
    in" warning: the envelope is the run's own (2e-5 m) again.
  - **A bar on a triangle its axis is not over got Bullet's point, and a
    force 14 times the blow.** The finder clips a cylinder against a
    mesh's triangle (`cylinderTrianglePoints()`); a pair it did not clip
    went on with Bullet's GJK point. On the strip's surface that was a
    triangle with a corner 0.8 mm from the bar's line of contact, its
    normal 8 deg off the direction to the axis, and the cylinder's support
    along that normal (`rollAlong()`) read a gap of 34 um as 13 um in:
    4.7 kN where the blow is 340 N. Now such a pair has the triangle's
    point nearest the cylinder's axis, the normal from it to the axis
    (`cylinderTriangleEdge()`, `nearestOfSegments()`): an edge along the
    axis gives the ends of what they share. The clip also flipped a
    triangle's normal to face the cylinder; with the triangles wound
    outward it gives no points for a cylinder whose middle is behind the
    triangle -- a bar across the strip's edge had the strip's side face
    through it, and its cap, 5 mm past that face, was a point 5 mm deep.
    Both hold for a rigid mesh under a cylinder too.
  - **The friction's rate was not in the part's matrix.**
    `FlexibleContact`'s block had each point's spring and damper along the
    normal only (step 8). The friction's slope with the sliding speed --
    mu 0.3 reached at 0.1 mm/s, 81 N s/mm under 18 N -- was in the residual
    alone, and the Newton iterations stopped unconverged on any substep
    past m / (mu' N): the strip between two bars (below) took no substep
    longer than 0.3 us under 18 N, each fourth try taken back, 98950 kept
    and 32587 taken back, 1643 s for its 0.1 s. Without friction the same
    run kept 5272 and took none back, in 44 s. The block now has, a point,
    k l n' and c l n' -- l the normal less mu along the sliding, the force
    that comes with the normal's -- and the friction's rate with the
    sliding velocity, mu N / v across the sliding and mu' N along it where
    mu rises (`ContactLoad::ForceOnB::line`, `drag`): 5274 kept, none
    unconverged, 43 s. Where mu falls from the static to the dynamic the
    rate along the sliding is left out, a damper that drives. Found by a
    step trace (kept and taken-back substeps a frame, by cause) after a
    softer contact law did not help: the largest step that converged went
    as 1 / N.
  - Numbers (mesh 8). The strip let go onto the bar: first touch at
    47.498 ms as in step 11, the blow 351 N (337.5), after it at most
    3.0 N, pressing in 12 of 13 frames. The strip between two bars: no
    gravity, its hinge turned 0.06 rad in 0.1 s onto a bar 120 mm out,
    0.05 mm under it; bent over that bar its end rises and 195 mm out
    meets a second bar 1 mm over it, 14 mm from where the hinge's end has
    it rigid (7 mm at the first). The second bar is met at 0.02899 rad for
    a beam's 0.02928; over the last fifth of the run the first presses
    17.54 N for the beam's 17.11 and the second 3.175 N for 3.145 -- the
    beam clamped at the hinge's turn, the contacts springs of 1000 N/mm.
    On the materials' law (Hertz) 17.63 N for 16.85 and 3.288 N for
    3.074, the second bar met with a blow of 17 N.
  - Limits.
    - The surface is the elements' faces, flat each: a round face facets
      as fine as the mesh, and a round's own law (a cylinder's line, a
      ball's point) needs the faces named (`ContactFaces`).
    - Two flexible parts touching each other anywhere are still refused.
    - A part the mesh could not be made for keeps its own shapes on the
      carrier, rigid, with no search ahead.
    - A contact that stands under a changing load runs at the contact's
      own substep, a fiftieth of the period of the part on the contact's
      spring: `resting()` lets a settled contact take the frame's step
      only while its depth stays within 2%, and a load a motion keeps
      raising never does. On the materials' law that is 1.4 us for the
      strip, 250 substeps a frame of 0.5 ms: the two bars' 0.1 s in 210 s,
      where the spring of 1000 N/mm (18 us) takes 43 s. Proposed, not
      built: judge a standing contact by how its depth follows the load --
      the depth's rate against the spring's own period -- not by its
      range, so a slowly loaded contact steps as a resting one does.
  - **How the bending is drawn is the flexible body's**
    (`FlexibleBody::BendingScale`, the panel's "Bending drawn" under After
    a run): 0, Automatic, is the scale that shows it, the largest over the
    run a tenth of the part's size -- as it is where that is a twentieth
    or more, never less than it is; a value draws it that many times, 1 as
    it is. The scale is of the bending about the part's floating frame,
    which lies along the bent part, not at its joint: the strip between
    the bars, 14 mm from its hinge's line, is 1.5 mm from its frame's and
    was drawn 13.6 times that -- off its mount and through both bars. A
    part bent onto others wants 1. `Deformation.scale` reads the property
    as it is; it is not the model's, so the run kept stays the study's and
    the mesh stays made.
  - Tests: TestFlexibleBody.test_bent_between_bars_anywhere (the second
    bar met within 10% of the beam's angle, the bars' forces within 10%
    and 15% of the beam's, neither over 1.5 times its last; 44 s; before,
    the second bar was never met and the first was 7 mm in);
    test_bent_onto_bar_anywhere, test_strike_anywhere (132.3 N, rigid
    409.1) and test_propped_cantilever_anywhere (106.3 mN for 104.9) hold
    on the moving mesh. The drawn scale: test_cantilever_deformation, and
    TestGuiCommands' flexible body panel.
  - The manual's page (`assembly/simulation/flexible-contact`) has the
    strip between the bars, drawn as it is, mesh 3 mm: the lower bar met
    at 7.7 ms, the upper at 48.9 ms, the hinge turned 0.0290 rad (a beam's
    0.0293); at the end 18.71 N and 3.51 N for a beam's 18.33 and 3.48,
    the strip's root at 114.5 MPa for a beam's 114.1; the run of 0.1 s,
    526 frames, 119 s.
- Built, D4b step 13 (2026-11-12): step 12's limits.
  - **A contact standing under a changing load steps as a resting one
    does.** `resting()` judges a load's depth over a window of one of its
    periods (or of the last substep, where that was longer), not over all
    of the time since it came to rest; the window's rate of depth sets the
    substep a settled load allows, `standingStep()`: the time its depth
    takes to move half the 2% `resting()` allows. Before, the range since
    rest grew past 2% under a load a motion kept raising, the load fell out
    of rest, the substep was taken back, and five periods were stepped at a
    fiftieth of one before it settled again, over and over. The strip
    between two bars (step 12), mesh 8: on the materials' law (Hertz) 0.1 s
    in 44.9 s, 209.7 s before (48.6 s on the step's last build); at 1000
    N/mm 36.5 s, 43.0 before (39.2). The
    forces as they were: over the last fifth 18.203 N and 3.410 N for
    18.204 and 3.413; the largest difference at any frame 0.095 N, at the
    upper bar's blow. TestContact 63 in 81 s (89.5 before).
  - **A round face touched anywhere is its elements' face, quadratic, not
    flat** (`ChronoFlexible::Surface`). The mesh is of the second order and
    Netgen (SMESH's quadratic helper) puts the edges' nodes on the face: on
    a rod R 5 at a 4 mm mesh every one is at R to 1e-6 mm, the faces'
    middles 0.198 mm in on their flat triangles, 0.0005 mm on the
    quadratic faces. So:
    - a face whose edges' nodes are off their edges' middles is four flat
      triangles through them for collision detection, a quarter of its
      sagitta; a flat face stays one (its quarters doubled the two bars'
      time and changed nothing);
    - a point found on one is placed on the face, by the six-node
      triangle's shape functions, and its map onto the reduced coordinates
      is the face's too -- each point six nodes, those of no weight left
      out (`refresh()`);
    - against a face of A (the point's normal A's face's own,
      `Point::face`), the point goes to the face's farthest point towards
      A, `Surface::farthest()`, A's point slid along with it: the flat
      triangle's nearest is a corner or an edge, where the face and the
      triangle are one, so placing the point alone changed nothing (the
      rod's force ran from 23 to 124 N). Only to a point where the face
      curves to A -- stationary inside it or along a curved edge, not a
      corner a straight way runs to -- and only as far in as the face
      stands out of its triangles (`Surface::bulge()`, 1.5 times). Both
      found by a trace: the plate's end, where Bullet took the end face for
      points of the rod sunk into the top past it, the normal along the
      rod, sent the farthest point to the face's end along the rod's
      straight, 0.11 mm in, 200 N for 46; and a normal leaning 1.3 deg
      there, 1.4 mm along, read 0.033 mm deep;
    - the carrier's envelope is wider by the bulge, and so is the reach
      a cylinder's points on a mesh triangle are taken within: a triangle
      is grown by its model's envelope alone (Chrono's Bullet, its
      thickness its margin), and a face pressing where its triangles clear
      the other part is found.
    The rod R 5 x 40 hinged on its axis at an end face, turned one
    revolution in 0.1 s on a plate 0.05 mm into its round, 1000 N/mm, no
    friction: over the turn (0.03 s on) the force from 42.5 to 46.7 N, its
    mean 44.9, in 6.4 s (mesh 4); 42.4 to 46.7 in 10.4 s (mesh 2). Its
    faces flat, as in step 12 (the same build, all its faces taken flat):
    0 to 104.5 N, the plate lost as each face rolled by, 23.0 s; at mesh
    2, 0 to 80 N, 100.5 s.
  - **The friction's fall stays out of the flexible part's matrix**, as
    measured: with mu' along the sliding where mu falls from the static to
    the dynamic, the strip between two bars ran at 1000 N/mm in 34.9 and
    35.7 s for 36.2 and 36.0 without, on the materials' law 47.1 s for
    44.7, its forces the same to the last digit both ways. The fall, from
    0.1 to 10.16 mm/s, is 7.5 s/m at its steepest against the rise's 4500
    (the default law); a damper that drives, kept out for no gain. (A
    switch read from the environment at each point cost the run three
    times its time: a getenv() per point per force, the stiff loads'
    Jacobians by differences.)
  - Limits.
    - Against a round (a cylinder's points on the mesh, or a mesh's round),
      the point stays on the face where the triangle has it: A's surface
      falls away from the plane square to the normal. A quarter of the
      flat face's sagitta, then.
    - A part that runs on past the edge of a box it presses into: Bullet
      takes the box's end face for some points of it, the normal along
      the part, a penalty contact's edge -- the rod on a plate ending under
      it pressed some 110 N for 46 at times. Not the faces'; flat faces
      have it too.
    - Two flexible parts touching each other anywhere are still refused:
      a load's side A on a mesh of its own, its forces on A's reduced
      coordinates, and a block coupling two parts' reduced variables, which
      `FlexibleContact`'s, one part's, is not; Bullet finds a moving mesh's
      triangles against another's.
  - Tests: TestFlexibleBody.test_round_anywhere (the rod: within a tenth
    of its mean over the turn, the mean under a rigid rod's 50 N; 9.6 s).
    The rest hold: TestFlexibleBody 19, TestDynamics 90, TestJointFriction
    4, TestContact 63; ctest 25; TestGuiCommands 76.
  - The manual's page (`assembly/simulation/flexible-contact`) has the rod
    turned on the plate, mesh 4: over the turn 42.1 to 46.7 N, the median
    45.0; the run of 0.1 s, 504 frames, 10.5 s.
- Built, D4b step 14 (2026-11-12): a flexible part past a box's edge.
  - **A point of a flexible part's surface in a box or a hull is under the
    face it is least deep under that faces the surface**
    (`leastDeepFace()`), not the face Bullet found it under. Bullet's face
    is the whole mesh triangle's way out of the solid: the rod of step 13
    on a plate from x = 5 to 35, its ends under the rod, had points 0.036
    mm under the top taken through the plate's end, 0.124 mm in along the
    rod (a trace, each point leaning off the plate's normal or deeper than
    0.07 mm). Over the turn the force ran from 35 to 126 N, its mean 54.9
    for the whole plate's 44.9, the run 45.5 s. Three steps, each found by
    the probe:
    - the least deep face of all: the triangle's point Bullet gives can be
      above the top and inside the end, so the face a point is outside of
      is its face, the curved face's point it goes to then under the top
      or not -- with only the points inside taken, 53.5 N and 114 at most;
    - of the faces within 45 deg of facing the surface -- the mesh
      triangle's outward normal, the other way: the end's points least
      deep through the end, 0.041 mm in and 0.049 under the top, made a
      patch of their own, its spring the set's, 40 N along the rod on top
      of the 46, at the plate's edge where it is the edge in the rod's
      round that presses -- 46.5 N, 62.8 at most;
    - only of the faces within the margin (0.1 mm, which the penetrations
      stay under) and the face's bulge of the point: without that the rod's
      end faces, their normal along the rod, took the plate's far ends,
      45 and 50 mm in, for theirs, and the rod on the whole plate pressed
      it at 18 kN on average, 52 kN at most, its 0.1 s in 2819 s.
    The rod past the plate's ends now 41.7 to 47.3 N over the turn, its
    mean 45.1, in 4.3 to 8.5 s; on the whole plate as before, 42.5 to 46.7
    (mesh 4) and 42.4 to 46.7 (mesh 2). The strip between two bars (step
    12): its forces as they were, to the digit.
  - Only a flexible part's points (`ShapeOn::flexible`), and only where A
    is a box or a hull with its features; a rigid mesh's are Bullet's as
    before.
  - Limits as step 13 left them, but for the box's edge: against a round
    the point stays on its quarter facet; two flexible parts touching
    anywhere are still refused.
  - Tests: TestFlexibleBody.test_round_past_edge (the rod on the plate
    from 5 to 35: within a tenth of its mean over the turn, the mean under
    50 N), sharing test_round_anywhere's rod (`roundTurned()`). The rest
    hold: TestFlexibleBody 20, TestDynamics 90, TestJointFriction 4,
    TestContact 63; ctest 25; TestGuiCommands 76.
- Built, D4b step 15 (2026-11-12): a flexible part's round on a rigid one.
  - **Against a sphere or a cylinder's side, a point of a flexible part's
    curved face is the face's point nearest the round's centre or axis**
    (`deepestOnFace()`), its normal out through it from there, turning with
    the part (`Point::face` 2); A's point the round's surface that way.
    Before, it kept the quarter triangle's normal and its place under the
    round: a rod touching a roller at a slant to its quarter reads a
    facet's R t^2 off, about the 0.05 mm it is pressed. The rod of step 13
    on a grounded roller R 5 along it, 0.05 mm into its round, 1000 N/mm:
    over the turn 31.9 to 45.8 N (mesh 4), 35.3 to 45.9 (mesh 2), the runs
    157 and 225 s. Four steps, each from a trace of every point (how it
    was placed, its depth, its angle round the rod, its normal):
    - the face's point nearest the axis's point at the point's own station
      (Gauss and Newton on the face's area coordinates): 37.2 to 45.9 N --
      a point whose nearest is in the next face stopped at its own face's
      edge and slid along it, a slanted edge, so that the way along the rod
      read as gap: normals 0.08 along the rod, 0.01 mm of 0.04;
    - measured square to the axis, the station held by a millionth: 5 to
      50 N -- a step along the axis all but unbounded, and the points fell
      into the faces' far corners, 1.4 mm clear;
    - the station held by a tenth, each step at most a fifth of the face and
      halved until nearer: 40.2 to 47.5 N, 13 s -- left, the points held at
      an edge, 0.02 mm deep where the faces beside them read 0.045, pulling
      their patch's depth down;
    - across the edge into the face there, three faces at most
      (`Surface::across()`, each face's neighbour across each edge; a flat
      face's edges' nodes at their middles): 42.0 to 46.7 N (mesh 4), 40.5
      to 46.2 (mesh 2), the runs 6.3 and 8.7 s. On the plate, 42.5 to 46.7.
  - `Surface::faceCoordinates()`, a triangle's point in its face's area
    coordinates, which `onFace()` takes now too.
  - Only a curved face's points against a round of A with no round of B
    (`Point::roundA`, not `roundB`); a flat face's triangle is its face.
    The plate, its edge and the strip between two bars as they were, to
    the digit.
  - Limits: two flexible parts touching each other anywhere are still
    refused (step 13's sizing). A round's cap, and a hollow round -- a bore
    round a flexible pin, whose deepest is farthest from its axis -- are as
    before; a hull's or a mesh's round face (`ShapeOn::rounds`) is taken as
    its whole sphere or cylinder, its angles' range not looked at.
  - Tests: TestFlexibleBody.test_round_on_roller (the rod on the roller:
    within a tenth of its mean over the turn, the mean under 50 N). The
    rest hold: TestFlexibleBody 21, TestDynamics 90, TestJointFriction 4,
    TestContact 63; ctest 25; TestGuiCommands 76.
- Built, D4b step 16 (2026-11-12): two flexible parts touching each other
  anywhere -- the last of step 12's limits; such a pair is no longer
  refused.
  - **A load's side A on a mesh of its own** (`ContactLoad::flexibleA`,
    `onMeshA`, `forcesOnA`). Of two flexible parts touched anywhere the
    set's first is A, the second B. A's points are placed on A's surface and
    follow its modes as B's do (`FlexibleContact::place()` and `refresh()`
    by side, `loadsA`); `ComputeQ()` takes A's point and speed from A's mesh
    and leaves A's force out of the bodies' load, and A's part takes it on
    its reduced coordinates, T_A' (-f) (`IntLoadResidual_F()`). The
    coupling is in B's block: each flexible A's reduced variables follow
    B's and the bodies' there (`setVariables()`, `Part`), and a point adds
    T_B' h T_B, -T_B' h T_A and its transpose, and T_A' h T_A
    (`LoadKRMMatrices()`). The readers of a point's speed -- the contact's
    substep, a contact found late, the patches' approach -- take A's from
    its surface too (`speedOnA()`).
  - That alone ran, and wrong: the rod of step 13 on its plate made
    flexible too, held all along its underside (mesh 4), took 826 s for its
    0.1 s, 706 frames, the force from 20.2 to 43.7 N, its mean 34.9, where
    the rigid plate has 42.5 to 46.7 in 6 s. At t = 0 it was right (44.66
    N), the terms above all there by a trace, and the sides swapped gave
    the same numbers. Three things, each from a trace of every point of
    every detection (its face, its triangles, its normal against each
    triangle's own, its depth):
    - **The curved face's point was not moved to where the face is deepest
      when the curved face is A's.** Of each detection's points 95% were on
      a curved face of the rod against a flat triangle of the plate, the
      normal the plate's (`Point::face` 2), and step 13's move
      (`Surface::farthest()`) was B's against a face of A alone: the rod's
      middle sat 55 to 90 um clear on its quarter triangles, its ends alone
      pressing. Now a flexible A's curved face against a flat face of B
      goes to its farthest towards B, B's point slid along with it and
      placed after (`placeOnFlexibles()`: each point's side that moves the
      other's first). Not against a curved face of another flexible part.
    - **One point in twenty had the rod's facet's own normal**, 10 to 17
      deg off the plate's and turning with the rod: the pair of triangles
      parted least along the rod's triangle where the plate's lay to one
      side of it. A point between a curved face of one part and a flat
      face of the other is the flat face's now, within 45 deg: its normal
      that face's own, the flat side's point under the curved side's on its
      plane (`flatFaces()`, before the patches).
    - **One detection in twenty was two patches, each with the set's
      spring**: the rows of the facets' corners either side of the rod's
      line, no quarter of the one sharing a corner with the other's -- 80
      to 95 N for 41. A curved face's quarter meets its neighbours by its
      face's corners (`FlexibleContact::faceKeys()`, `patchesOf()`), on a
      load of two flexible parts.
    With these the first 10 ms ran in 6.2 s for 24.9, always one patch, but
    from 33 to 47 N: the rod's middle still had no pressing point.
    - **A triangle gone through the other's surface was "apart" from it**
      (`satApart()`). The plain test of two triangles took each at its grown
      place alone -- grown by the envelope and, on a flexible part, its
      faces' bulge, 0.09 mm here -- so the rod's triangles along its line,
      0.05 mm into the plate, lay 0.04 mm past the plate's top and were
      proven apart, 41.6 um for a threshold of 6: of the 73 triangles of the
      rod with points at the first detection, 5 reached its line, all at
      its ends, and the 68 mid rod were the ones clear of the plate. A
      triangle reaches now from its grown place back through its own and
      the safe margin behind it (0.1 mm, `safeMargin`), the skin the
      penetrations stay in. A box or a hull is a solid and never had it:
      the rigid plate's points were on the line. The first detection then
      has 376 points, 220 pressing, for 127 and 8.
  - Three more, the pair's first limits, each from the same trace:
    - **A part past the edge of another's flat face.** A point under a
      flat face of one part, the normal that face's own, is under the face
      -- of that one and the flat ones it meets at a convex edge
      (`FlexibleContact::facesAbout()`, `edgeFaces`, as the part is at
      rest) -- it is least deep under that faces the other part's surface
      there, within 45 deg of that surface's triangle's normal the other
      way and within the margin and the surface's bulge; one outside
      through any of them, through the one it is farthest out of
      (`flatFaces()`): step 14's rule for a rigid box, of a mesh's faces.
      The rod on a plate from x = 5 to 35, both flexible, had a node 0.04
      mm past the plate's end and 0.05 under its top taken through the
      end, a patch of its own with the set's spring, 40 N along the rod:
      over the turn 35.2 to 130.2 N, its mean 55.8, in 195 s; now 39.6 to
      47.1, its mean 45.3, in 26 s (the rigid plate's 41.7 to 47.3). Where
      none of the faces faces the other's surface -- that one's triangle a
      side of its own, an edge on an edge -- the point is left as found:
      with the least deep of them all taken then, two strips crossed, both
      flexible, pressed 0.58 N sideways for 0.92 down.
    - **Two flat faces meeting at their edges** are that same rule's. A
      strip 200 x 20 x 2 turned 0.06 rad down onto the free end of a second
      strip 20 x 100 x 2 across under it, clamped at its far end, both
      flexible (mesh 8, 1000 N/mm, no friction): the force leaned 0.22 to
      0.69 of its size along the lower strip, a side face's points a patch
      of their own; now 0.027 at the end, the lower strip's own slope, one
      patch, 9.80 N for a rigid lower strip's 14.06.
    - **Two rounds.** A flexible part's shapes' convex cylinders and
      spheres are kept as they ride its carrier at rest
      (`FlexibleContact::rounds`), and each curved face of its surface
      whose nodes are all on one is that round's (`matchRounds()`). A
      point between two curved faces has B's go to its face's nearest A's
      round's axis or centre, as against a rigid round (step 15,
      `deepestOnFace()`), the round where A's mesh has it now
      (`roundAt()`): its radius its own, a cylinder's axis the round's at
      rest turned as the face's corners have turned, its centre where the
      face's six nodes are all the radius from it. A's goes against B's
      round where A's own face is of none. The other part's point, set on
      its round's surface, is placed there as set (`ContactLoad::given`):
      its offset from the face's point, not from the flat triangle's --
      from the triangle's the rod's points were up to 0.034 mm outside the
      rod, a roller's 0.044, and the depths read 0.05 to 0.094 mm for 0.05.
      The rod on a roller R 5 held at both ends, both flexible: on their
      quarter triangles the run did not end in 18 minutes; now 504 frames
      in 112 s, over the turn 43.47 to 45.26 N, its mean 44.29 (a rigid
      roller's 42.0 to 46.7).
    A face walked into from a curved one's edge (step 15) took a flat
    face's edge nodes for its first corner, which stands for them in
    `Surface`: they are at its edges' middles now.
  - Numbers (mesh 4, the rod R 5 x 40 turned once in 0.1 s, 0.05 mm into
    the plate, 1000 N/mm, no friction). Both flexible: 504 frames in 21.8
    s, over the turn 43.76 to 47.21 N, its mean 46.1. The set's groups
    swapped, the plate A and the rod B: the same to 0.04 N, 20.3 s. The
    rigid rod on the flexible plate 49.08 N throughout, as it was. Every
    probe of steps 12 to 15 as it was, frame for frame: the two bars on
    both laws, the roller and the plate at mesh 4 and 2, the plate's edge.
    (Later the same day the box ran the plate in 36 s, this commit and the
    one before it alike.)
  - Limits.
    - Two curved faces neither of a cylinder or a sphere of its part's
      shapes -- a tooth's flank, a torus -- stay on their quarter
      triangles, each facet's own normal; so do hollow rounds.
    - A point slid or set by the other part's is placed on the triangle
      it was found on, its offset from it rigid: past that triangle's edge
      it is not handed to the next one.
    - A flat face's neighbours are those across its own convex edges: at
      a corner where three faces meet, the third is not looked at.
    - Friction between two flexible parts is run, not checked: the two
      strips with a friction of 0.3 press 9.85 N at the end for 9.80
      without, as straight; the friction's own force is against no closed
      form yet.
    - Two rounds cost five times the plate, 112 s for 22: each point's
      round is found again at each detection.
    - `satApart()`'s skin is every mesh triangle's, a rigid mesh's too. A
      rigid mesh's flat faces are large triangles, and the pair's
      threshold goes by their size: TestContact.test_mesh_on_mesh rested
      before as it does now.
  - Tests: TestFlexibleBody.test_round_on_flexible_plate (the rod on the
    plate, both flexible: within a tenth of its mean over the turn, the
    mean under 50 N, the run under 120 s -- 46.1 N, from 43.76 to 47.21),
    test_round_past_flexible_edge (the plate from 5 to 35: within 15% of
    its mean), test_round_on_flexible_roller (over 0.03 to 0.045 s of the
    turn, within a tenth) and test_strip_on_flexible_strip (the two
    strips, to 0.035 s: 2.3 to 3.3 N at the end, rising, leaning under
    0.05 of it). The rest hold: TestFlexibleBody 25, TestDynamics 90,
    TestJointFriction 4, TestContact 63; ctest 25; TestGuiCommands 76.
- Built, D4b step 17 (2026-11-13): the friction on a flexible part
  touched anywhere against its closed form -- step 16's limit -- and what
  the check found: a point slid over a curved face is slid again at every
  update of a substep.
  - **The check.** Step 13's rod (R 5 x 40, mesh 4, turned once in 0.1 s
    by a motion, 0.05 mm into the plate, 1000 N/mm, no gravity) with a
    friction of 0.3 slides all the turn, at up to 470 mm/s: the motion
    turning it gives mu N R, the rod's radius the friction's arm. The
    friction's force was mu N in every case, 0.300; the motion's torque,
    over 0.03 to 0.08 s, as its share of N R:

    | rod | under it | before | now |
    |---|---|---|---|
    | rigid | rigid plate | 0.2998 | 0.2998 |
    | rigid | flexible plate | 0.2999 | 0.2999 |
    | flexible | rigid plate | 0.2838 | 0.2987 |
    | flexible | flexible plate | 0.2820 | 0.2972 |
    | flexible | rigid roller R 5 | 0.3102 | 0.2983 |
    | flexible | flexible roller R 5 | -- | 0.2978 |

    Not the friction, and not the pair of flexible parts: a flexible rod's
    torque was 5% under on a plate and 3% over on a roller, with a ripple
    of 7% on the roller (0.273 to 0.333), and with no friction at all the
    motion gave -2.18 N mm on the plate for none.
  - **A point placed at a substep's start is a point of the mesh.**
    `FlexibleContact::place()` slides a curved face's point to the face's
    farthest towards a flat face (step 13) or its deepest in a round (step
    15) once, at collision detection, and `refresh()` then moves it with
    the mesh. A rod turning carries it round, w h R past where the rod is
    deepest by the substep's end, 0.085 mm at 85 rad/s and 0.2 ms; the
    normal's force, still along the plate's normal, turns the rod on. With
    no friction, at output steps of 0.2, 0.1 and 0.05 ms: -2.18, -1.15 and
    -0.58 N mm, in proportion; with 0.3, 63.6, 65.5 and 66.2 for 67.2. A
    rigid round's point had the same and its cure (`rollAlong()`).
    Now each such point is slid again as the bodies are at every update
    (`FlexibleContact::follow()`, from `refresh()` at `Update()`), by what
    `place()` kept of it (`ContactLoad::OnMesh::slides`): towards a flat
    face, its face's nodes and the point found on its flat triangle
    (`towardsFlat()`), the normal turned with the face's body as the
    load's follow turns it; into a round, the face it was placed on, its
    area coordinates there, the station it is held at and the round
    (`deepestOnFace()`). The other part's point under one slid along a
    flat face stays under it where that part is rigid. The plate's cases
    were right with that alone: no friction -0.06 N mm, 0.2987 and 0.2972.
  - **On a round the first try never ended its first substep**, and then
    was wrong the other way, -2.25 N mm with no friction for +0.75. Three
    things, from a trace of every point's search:
    - `deepestOnFace()` held each point, by a tenth of a weight, at the
      station along the round's axis of the point it started from: found
      again at each of Newton's iterations from where it last was, a point
      on a rod lying along a roller crept along it each time. It is held
      at the station its placing took now (`OnMesh::station`), and started
      from where it was placed each time: a way of the state alone.
    - **Its steps were Gauss and Newton's, without the face's curve**, and
      such a step is the face's own radius short of the way: against a
      roller of the rod's radius it goes twice as far as the deepest, to as
      far beyond it and no nearer, and stops. The trace had the points in
      two sets, a substep's turn (0.09 mm) either side of the rod's line,
      some 0.2 to 0.6 mm off it -- at placing too, step 15's and step
      16's. The steps are Newton's now, with the face's second rates, which
      are the same all over a quadratic face (the Gauss and Newton's where
      those leave no way down). The roller without friction: -0.39 N mm,
      and 0.2981 with 0.3.
    - **And then 283 s for 8**: a fifth of the substeps did not converge
      (250 of 1417 in the first 26 ms; none on the plate) and were taken
      again an eighth as long. A point gone on into the next face at one
      of Newton's iterations and not at the next: the edge it leaves by
      was read off the step as halved twelve times, so the deepest was
      0.016 mm beyond the edge before the point went after it, and went
      that far at once. The edge is read off the step as taken now; and
      within a substep a point stays on the face it was placed on, stopped
      at its edges -- a substep's turn is a fiftieth of a face. No substep
      unconverged since, the run 7.5 s.
  - **A point set on its round by the other flexible part is on its face
    there** (`setGiven()`, step 16's second limit): its weights the face's
    at the point nearest where it is set, across into the faces beyond its
    edges at placing, its offset from that -- not the weights of where it
    was found, its offset all the way from there. On a rod turning on a
    flexible roller the roller's points go to their deepest in the rod and
    the rod's are set under them: those were still the rod's own points
    where they were found, carried round with it, and the torque 0.288 of
    N R. Both parts' points are slid before either part's are read
    (`refresh()`), whichever is updated first. 0.2978.
  - Numbers (mesh 4, steps of 0.2 ms, friction 0.3; N R the mean normal
    force by the rod's 5 mm). The flexible rod on the rigid plate: 44.81 N,
    66.94 N mm, 0.2987 (0.2845 to 0.3147 over the turn), in 7.3 s; on the
    flexible plate 46.02 N, 68.38 N mm, 0.2972, in 26 s; on the rigid
    roller 44.60 N, 66.52 N mm, 0.2983 (0.2895 to 0.3081), in 9.7 s; on
    the flexible roller 44.71 N, 66.57 N mm, 0.2978, in 182 s. With no
    friction, the motion's torque for none: -0.06 N mm on the plate, -0.11
    on the roller. Without friction, both flexible: the plate 43.75 to
    47.20 N in 21 s (step 16's 43.76 to 47.21), past its ends 39.59 to
    47.23 (39.62 to 47.14), the roller 44.09 to 45.37 in 136 s (43.47 to
    45.26 in 112), the two strips 9.80 N at the end as they were, in 159 s
    as they were. The strip between two bars
    as it was, frame for frame, on both laws (19.09 and 16.85 N, 18.73 and
    3.526): its faces are flat. The rod on the rigid roller 42.35 to 46.81
    N at mesh 4 and 41.59 to 46.93 at mesh 2 (42.0 to 46.7, 40.5 to 46.2),
    on the rigid plate 42.28 to 46.78 and 42.18 to 46.68, past its ends
    41.81 to 47.30.
  - Limits.
    - Within a substep a slid point stops at the edges of the face it was
      placed on, and one set by the other part at its own face's: the next
      detection places it on.
    - The flat part's point under a point slid along its face is moved
      with it where that part is rigid; a flexible flat face's stays the
      point of its mesh it was placed on.
    - Two rounds, both flexible, cost four to six times the plate: 136 s
      for 21 without friction, 182 for 26 with.
    - The friction is checked sliding; sticking, and the two strips, are
      against no closed form.
    - Step 16's others stand: two curved faces of no cylinder or sphere,
      hollow rounds, a corner of three flat faces.
  - Tests: TestFlexibleBody.test_friction_on_flexible_round,
    test_friction_on_roller and test_friction_between_flexible_parts (the
    rod with a friction of 0.3 on the rigid plate, the rigid roller and the
    flexible plate: the motion's torque 0.3 N R to 0.006 N R -- 0.2994,
    0.2985 and 0.2975 over 0.03 to 0.1 s); and every round's test of steps
    13 to 16 holds the motion's torque to none within the same (-0.0013 to
    0.0004 N R). The rest hold: TestFlexibleBody 28 (548 s), TestDynamics
    90, TestJointFriction 4, TestContact 63; ctest 25; TestGuiCommands 76.
- Built, D4b step 18 (2026-11-13): hollow rounds -- a pin in a bore, one
  of them flexible or both, step 16's limit -- and with them a round's
  points thinned to one a place.
  - **Where it stood.** Step 17's rod (R 5 x 40, mesh 4, turned once in
    0.1 s, friction 0.3) in a block 55 x 20 x 20 with a bore R 5.5 along
    it, the bore's wall 0.05 mm into the rod's underside. Both rigid: 50 N,
    0.2998 N R. The rod flexible in the rigid block: not over in 10
    minutes. The rod rigid in the block flexible, held all along its
    underside: 47.6 N steady and 0.3018 N R, in 430 s.
  - **A hollow round's deepest is the face's point farthest from its axis**
    (`deepestOnFace()`, which refused one): the same steps the other way,
    Newton's Hessian the other sign, the Gauss and Newton's still the
    metric where that leaves no way down; the normal still out of the axis
    through the point, and the load's the other way, from the hollow part
    in (`place()`, `follow()`). The mesh's faces on a bore are its round's
    too (`matchRounds()`; `roundAt()`'s centre out of the part there), so
    of two flexible parts the pin's face goes into the bore's round.
  - **3500 points on the one line.** A pair of triangles each gives its
    points; the rod's triangles a third of the way round it, against each
    of the bore's mesh's about them, all go to where the rod's face is
    deepest -- a roller has 130. Each substep took 0.4 s, none unconverged:
    the points, not the solver. The points slid into a round are thinned
    after they are placed (`thinOut()`): one place is a patch's points, slid
    on the same side, within a quarter of their face's first edge along the
    round's axis and a whole edge across it -- a ball's, a quarter all
    round. The places are gathered about their deepest points, and of a
    place's points the middle one by depth is kept. Not the first found:
    41.7 N for 44.5. Not the deepest: the same to a newton here, but it is
    the place's outlier. And not the points slid towards a flat face, which
    are held nowhere along a rod's line: thinned, the rod's force on a plate
    ran from 39.5 to 46.3 N for 42.3 to 46.8. About 100 points then, the
    solve a tenth of what it was.
  - **Thinned, the strays weigh.** Three kinds, each from a dump of the
    points kept:
    - A third of them were 1 to 3 mm off the line, stopped at a face's
      edge: `deepestOnFace()` read the edge it leaves by only once the
      steps along the edge had stopped getting nearer, and a point slid
      along its edge for its sixteen steps with the deepest across it. It
      is read at each step now -- on an edge, Newton's step out across it by
      a millionth of the face or more -- and the faces gone through are
      eight at most, for three: at a mesh's node the way round is that
      long.
    - A cylinder's end. A point found past it was refused, and the wall's
      points at a rod's two ends stayed on their flat triangles, which
      stand in from a hollow face: 0.08 and 0.13 mm deep for 0.05. It is
      held at the end's station now -- the rim of a pin's end on a bore's
      wall is its side's line there -- and taken as the end's within a
      twentieth of the radius: held by a tenth of a weight, a point is
      microns off its station where the face leans along the axis.
    - What is still left on a curved face's triangle, in a patch whose
      others were slid into the round, is left out (`thinOut()`): one such
      at a rod's rim, 0.7 mm off its triangle and 0.12 mm deep among
      forty-six at 0.05, had the rigid rod press the flexible bore 53.8 N.
  - **A slid point is on its part's round**, where its face is of one
    (`ontoRound()`, from `place()`): its offset the way from the face's
    point to the round the mesh has there (`roundAt()`), kept through the
    substep. A quadratic face through nodes on a round stands in from it
    between them. Measured on the rod's mesh (`faceerror.py`): its faces'
    corners span 26.5 deg, 38.8 at most, and the faces are 0.21 um inside
    the R 5 on the mean, 1.75 at most -- of the 50 pressed, 0.4%, and the
    rod's force on a plate rose 0.2%. Not the plate's ripple, then, which
    stays.
  - Numbers (mesh 4, steps of 0.2 ms, friction 0.3, over 0.03 to 0.08 s).

    | rod | under it | N | of N R | run | before |
    |---|---|---|---|---|---|
    | flexible | rigid plate | 44.92 (42.52 to 46.67) | 0.2987 | 8 s | 44.81, 0.2987 |
    | flexible | flexible plate | 46.13 (45.05 to 47.03) | 0.2973 | 42 s | 46.02, 0.2972 |
    | flexible | rigid roller | 45.64 (44.70 to 46.61) | 0.2974 | 6 s | 44.60, 0.2983, 10 s |
    | flexible | flexible roller | 45.15 (44.69 to 45.58) | 0.2982 | 59 s | 44.71, 0.2978, 182 s |
    | flexible | rigid bore | 45.53 (44.99 to 46.04) | 0.2988 | 140 s | not over in 600 s |
    | rigid | flexible bore | 49.98 (49.98 to 49.98) | 0.2999 | 31 s | 47.56, 0.3018, 440 s |
    | flexible | flexible bore | 45.50 (45.04 to 45.98) | 0.2984 | 276 s | -- |

    The flexible rod presses 45.2 to 45.6 N on a roller and in a bore, rigid
    or flexible; a rigid roller's ripple is 3.8% of the mean for 10.0 (44.88
    to 46.60 N over the turn, 44.41 to 46.55 at mesh 2). Without friction,
    both flexible: the plate 43.76 to 47.21 N, past its ends 39.51 to 47.25,
    the roller 44.71 to 45.54 in 49 s (step 17's 44.09 to 45.37 in 136), the
    two strips 9.80 N at the end as they were. The strip between two bars
    as it was, frame for frame.
  - Limits.
    - A flexible rod in a rigid bore is 140 s for a roller's 6: the rigid
      bore is a mesh, and every triangle of the rod is tried against each
      of its triangles near it at each detection, 0.07 s.
    - The points slid towards a flat face are not thinned: two flexible
      parts' line on a flat face has a point for each pair of triangles.
    - The flexible rod's force itself, 45 N of the rigid 50, is against no
      closed form: it is one within 1% on a plate's edge, a roller and a
      bore, rigid or flexible.
    - A partial round's reach about its axis (a D shaft's) is not looked at
      where a face's point is slid into it.
    - Step 17's stand: within a substep a slid point stops at its face's
      edges; sticking friction against no closed form.
  - Tests: TestFlexibleBody.test_round_in_bore (the flexible rod in the
    rigid bore over 0.03 to 0.045 s: 45.63 N, within a tenth, the motion's
    torque none to 0.006 N R) and test_rigid_round_in_flexible_bore (the
    rigid rod in the flexible bore with a friction of 0.3: 49.98 N steady,
    0.2987 N R). The rest hold: TestFlexibleBody 30 (587 s), TestDynamics
    90, TestJointFriction 4, TestContact 63; ctest 25; TestGuiCommands 76.
- Built, D4b step 19 (2026-11-13): a flexible part held by a part nothing
  joins to the ground.
  - **The scene.** A grounded pin R 5 x 30; a hanger 10 x 20 x 80 on it by
    a bore R 5.2, 12 mm under its top; a block 30 x 30 x 30 fixed under the
    hanger, 114 g the two. Gravity, the contact 1000 N/mm, the bore's wall
    0.01 mm over the pin. Rigid, the hanger lands at 5.1 N, bounces once
    and rests in 6 ms; let go 30 deg off the vertical it swings to -28.97
    and back to 26.68 in 0.6 s, a run of 2 s. Flexible, each landing was
    harder -- 4, 10, 26, 600 N -- and it left the pin.
  - **Not the contact.** With no contact asked of, the two falling: the
    block fell 19.63 um in 2 ms, g, and the flexible body's deformation at
    its interface grew as t^2, a turn about x of 5.7e-4 rad at 1.2 ms, 800
    rad/s^2; three of its modal coordinates with it, the rest at 1e-13.
    Its modes: 0.0015, 0.0026 and 0.0034 Hz, then 5.4 kHz. In the contact's
    run the bore, 68 mm over the interface, was 36.6 um off the pin's axis
    sideways by the first touch, the contact 0.95 mm round from the pin's
    top, a sixth of its force sideways.
  - **Its record had no nodes for the joint.** A flexible body's interfaces
    are the frames on its body, each with the nodes its mesh has for it
    (`FlexibleBody::InterfaceFrames`), and were listed with its record, in
    `addForces()`. The joints of parts nothing joins to the ground are put
    in the model after that, once a contact, a spring or the flexible body
    itself has made a part of theirs a body (`buildSimulation()`): the
    hanger's joint to the block came after its record, so the record had
    no interface for it, the backend took the frame as one with no nodes --
    tied at the node nearest -- and the hanger turned about that node, its
    first three modes. A flexible part hinged or fixed to the ground, or to
    a part that is, has its joint in the model from the start: every test
    before this one.
    The interfaces are listed once those joints are in
    (`AssemblyObject::flexibleInterfaces()`, from `buildSimulation()`
    before the masses).
  - **Find Modes with a contact set.** `Chrono::flexibleFrequencies()` took
    every frame on the body for an interface, a contact shape's too -- at
    the node nearest it -- where a run leaves those to the first interface
    or to the contact faces' (`ChronoSession::load()`): its modes were not
    the run's, and the hanger's reduction failed to factor (SparseLU), the
    process with it. With no interface at all it read storage not its own.
    It takes them as the run does now, and says "nothing holds the flexible
    body" of one held by nothing.
  - Numbers (mesh 4). Falling free, the hanger and the block both 19.627 um
    in 2 ms; its modal coordinates under 1e-10. Its modes 1382, 2644, 7450
    Hz, with the contact set or without. Let down straight: 5.15 N at the
    landing, one bounce of 2.5, then 1.128 N over 0.02 to 0.03 s for a
    weight of 1.116, in 9.7 s. Let go at 30 deg, a friction of 0.1: to
    -28.95 deg and back to 26.61 in 0.6 s, the rigid hanger's -28.97 and
    26.68; 843 frames in 209 s. Every probe of steps 12 to 18 as it was.
  - Limits.
    - The swing is 209 s for the rigid hanger's 2. Before the fix its
      substeps were the contact's own, 25 us: the contact was never at rest
      by the 2% its deepest point's depth, 1 um here, may move over its
      period. Not looked at again since.
    - Resting, the force on the pin ripples between 1.0 and 1.24 N about
      its 1.12.
    - A flexible part held by no joint at all is still refused.
  - Tests: TestFlexibleBody.test_held_by_a_free_part (the hanger let down
    straight for 0.03 s: its first mode over 1 kHz, the landing under 10 N,
    the force at rest its weight to 5% -- 1382 Hz, 5.15 N, 1.128 N of
    1.116). The rest hold: TestFlexibleBody 31 (604 s), TestDynamics 90,
    TestJointFriction 4, TestContact 63; ctest 25; TestGuiCommands 76.
- Built, D4b step 20 (2026-11-14): a contact on a flexible part's round
  at rest -- step 19's swing, 209 s for the rigid hanger's 2.
  - **Never at rest.** A contact rests while its deepest point's depth
    keeps within 2% of itself, or within the tolerance, over one of its
    periods (`resting()`), and a resting one steps over its period.
    The hanger's bore is 1.3 um into its pin, 1.1 N at 1000 N/mm; the
    deepest point, found again on the mesh's faces at every detection,
    wandered 0.3 um as the bore slid round the pin -- 1.22 to 2.19 um over
    the swing -- and 2% of it is 28 nm. Every substep was a fiftieth of the
    contact's period, 25 us: 4000 to 0.1 s, two thirds of each in collision
    detection.
  - **Within the faces' own accuracy.** On a flexible part's curved faces
    a contact rests within a hundredth of how far those stand out of their
    flat triangles too (`Surface::bulge()`): 0.9 um for the hanger, where
    step 18 measured a rod's quadratic faces 0.2 um off its round on the
    mean and 1.75 at most, of 90. A part of flat faces alone has none, and
    a contact deeper than fifty times that is within its 2% first: the
    probes of steps 12 to 18, 50 um deep, are as they were, frame for
    frame. With 0.1 um instead the swing took 191 s, and with a two
    hundredth of the bulge 85.
  - Numbers. The swing of 0.6 s: 4195 substeps for some 24000, 54 s for
    209; to -28.95 deg and back to 26.61 as before. The pin's force over
    it from 0.02 s on: mean 1.196 N, 0.79 to 1.74, from frame to frame
    0.109 N rms -- before, 1.208, 0.50 to 1.70 and 0.109; the rigid
    hanger's 1.154, 0.94 to 1.37 and 0.015. Let down straight, 1.112 N at
    rest for a weight of 1.116 (1.128).
  - Limits.
    - The force on a flexible bore sunk a micron is seven times as rough
      as on a rigid one, frame to frame: its mesh's accuracy, a third of
      that depth. A contact by the materials' law, stiffer, is sunk less
      and rougher still; there the bore is better named a contact face.
    - 54 s is still the rigid hanger's 2 many times: a resting contact on
      a flexible round steps by how fast its depth moves (`standingStep()`),
      which is that roughness too.
  - Tests: none new; TestFlexibleBody.test_held_by_a_free_part rests at
    1.112 N of 1.116. TestFlexibleBody 31 (593 s), TestDynamics 90,
    TestJointFriction 4, TestContact 63; ctest 25; TestGuiCommands 76.
- Built, D4b step 21 (2026-11-14): a flexible part no joint holds.
  - **Refused.** A flexible body is reduced about its interfaces -- the
    frames on its body, each a boundary body tied to its mesh's nodes
    there -- and has no motion as a whole but theirs: its coordinates are
    the interfaces' six each and the modes' (`ChModalAssembly`). One with no
    joint on it had no interface: "nothing holds the flexible body" (step
    19; before that the reduction read storage not its own).
  - **An interface of its own.** `ChronoFlexible::ownInterface()`: the
    tetrahedron nearest its mesh's middle -- the tetrahedra's, each by its
    volume -- its ten nodes tied to a boundary body at that tetrahedron's
    middle. Nothing is on that body: it is the part's motion as a whole,
    and the modes are the mesh's with that element held. Given in the run
    (`buildFlexibles()`) and in Find Modes (`Chrono::flexibleFrequencies()`)
    to a body with no frame but its contact shapes'. With contact faces
    named their interface is the one, as it was to be, and the part goes as
    those faces do, rigid.
  - Numbers. A bar 80 x 10 x 4, 8.64 g, across two grounded rollers R 5
    60 mm apart, 0.01 mm over them; 1000 N/mm, a friction of 0.1; mesh 4,
    2788 nodes.
    - Falling free, no contact asked of: 19.65 um in 2 ms for g t^2/2's
      19.61, its modal coordinates under 1e-19.
    - On the rollers: lands at 1.435 N (rigid 1.85), at rest by 17 ms,
      0.08473 N for a weight of 0.084729; 165 frames in 2.2 s (rigid 0.1
      s).
    - Its sag between the rollers at rest, its underside at midspan
      against its underside over them, for a beam's on two supports with
      its ends hanging over, w L^2 (5 L^2 - 24 a^2) / (384 E I): 0.0409 um
      of 0.0415. 2 mm thick (mesh 2) 0.1637 of 0.1660; 1 mm thick 0.657 of
      0.664 (mesh 4) and 0.656 (mesh 2). 1 to 1.5% under, all four.
    - Its modes by Find Modes: 1874, 2063, 2169 Hz.
    - Its underside named its contact face: lands at 1.844 N as the rigid
      bar does, sags 3e-11 um, its modes from 177 kHz.
    - 1 mm thick, let fall 5 mm: lands at 12.05 N (rigid 20.1), bent 73 um
      between the rollers at 32.4 ms, springs 0.77 mm off them, and is at
      rest by 0.15 s, 0.0212 N for 0.02118; 335 frames in 79 s.
  - Limits.
    - Find Modes shows such a part's modes about its middle element, not
      its free ones: the bar's 1874 Hz is half of it clamped at the middle
      (a beam's 2.06 kHz), where a free bar's first bending is 3.3 kHz by
      the beam's formula. The run does not hold it there: the interface's
      six coordinates go with those modes.
    - Falling free it is 0.04 um ahead of g t^2/2 at 2 ms and 0.10 at 5
      ms, a speed of 0.02 mm/s from the start; step 19's hanger and block,
      19.627 um, had a third of it. Step 22.
    - The strip 1 mm thick still rings at rest at 0.05 s, 0.0207 to 0.0217
      N: its 479 Hz mode.
  - Tests: TestFlexibleBody.test_held_by_nothing (the bar on the rollers
    for 0.05 s: no interface listed, its first mode over 1 kHz, the landing
    under 3 N, the force at rest its weight to 1%, its sag the beam's to
    3% -- 1874 Hz, 1.435 N, 0.08473 N, 0.0409 um of 0.0415). The rest
    hold: TestFlexibleBody 32 (615 s), TestDynamics 90, TestJointFriction
    4, TestContact 63; ctest 25; TestGuiCommands 76; every probe of steps
    12 to 18 as it was.
- Built, D4b step 22 (2026-11-14): a flexible body starts at the
  velocities it has.
  - **2 us of acceleration ahead.** Step 21's bar falling free was ahead of
    g t^2/2 by 0.0196 um at 1 ms, 0.0392 at 2 and 0.098 at 5: a speed of
    0.019613 mm/s from the start, g times 2e-6 s to its last digit. The
    start's assembly projects the velocities twice, each a step of 1e-6 s
    under the forces, and takes those 2e-6 s of acceleration back off the
    bodies (`assembleFull()`) -- off the rigid ones: a flexible
    body is none of those, its coordinates its interfaces' bodies and its
    modes'. Step 19's hanger, 41 g of the 114 falling, was 0.36 of it
    ahead: 19.627 um for 19.613.
  - Taken back off a flexible body's interface bodies and its modes' rates
    too.
  - Numbers. The bar falling free: 1.225831, 4.903325, 19.613300 and
    122.583125 um at 0.5, 1, 2 and 5 ms, g t^2/2 to the last digit. On the
    rollers it lands at 1.443 N for 1.435, and rests and sags as it did,
    0.08473 N and 0.0409 um. Step 19's hanger lands at 5.17 N for 5.15 and
    rests at 1.121 N for 1.112, of 1.116. The probes of steps 12 to 18
    are as they were, to the digits they print.
  - Tests: none new. TestFlexibleBody 32 (601 s), TestDynamics 90,
    TestJointFriction 4, TestContact 63; ctest 25; TestGuiCommands 76.
- Checked, D4b step 23 (2026-11-14): a flexible part sticks as a rigid
  one does. No change to the solver.
  - Step 17 checked the friction on a flexible part sliding, mu N; whether
    one is held was left open. A bar 40 x 10 x 4 no joint holds (step 21),
    4.32 g, lying on a grounded plate tilted 5 deg, tan 0.0875, touched
    anywhere, 1000 N/mm; 0.1 s.
  - A friction of 0.2: the plate presses it 0.042204 N, its weight's
    cosine, and holds it 0.0036923 N, its weight's sine, both to the last
    digit; it creeps down at 0.04582 mm/s, steadily -- the friction's slip,
    the rigid bar's 0.04582 too.
  - A friction of 0.05: 0.0021102 N, mu N, and it gains 366.24 mm/s^2 for
    g (sin a - mu cos a)'s 366.24; 1.832 mm down the slope at 0.1 s, the
    rigid bar's 1.8317.
  - 162 frames in 3.4 s, the rigid bar's 0.1 s.
  - Tests: TestFlexibleBody.test_sticks_on_a_slope (both frictions: the
    forces and the gain to 1%, the creep under 0.1 mm/s). TestFlexibleBody
    33 (611 s).

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

### 8.6 Playback speed, and plots that keep their peaks

An impact of steel on steel lasts some tens of microseconds. Output frames
milliseconds apart miss it, and a video at an output step a frame shows it
between two frames. Nothing surveyed (2026-11-04) slows down by itself:

- Abaqus writes history output (the X-Y plots) as often as every
  increment, and field output (contours, animations) seldom.
- LS-DYNA's binout and d3plot have intervals of their own. The d3plot
  interval can follow a load curve over time (LCDT), set before the run.
- RecurDyn animates at a uniform report step, and its plot data sit at
  the solver's own variable steps.
- Simscape Multibody plays at a speed set by hand, powers of 2 from 1/256
  to 256 of real time.
- Game engines' slow motion on a collision is scripted.
- Video research (ElasticPlay) sets the speed from the motion, and found
  users distrust a speed they cannot see.

What this fork does:

- **Frames through each impact** (`ImpactFrames`, on by default). After
  each substep, the contact loads' forces. A substep where one moved from
  the last kept is kept, with its values and poses, and so is the one
  before where they set off from a steady force, its values read too (on
  the line between its neighbours', its free body did not balance in
  moment). At each output frame
  those since the last are thinned by Douglas-Peucker: each load within 5%
  of the most it is at between a line's two ends, or 0.1% of the largest
  force so far. A peak is the farthest from its line, and kept; a bounce
  of a few newtons after a landing of hundreds keeps its own (5% of the
  load's largest so far thinned the block's bounces away). A load at a
  steady force keeps none.
- **Playback by the run's time** (`PlaybackSpeed`, `SimulationPlayback.
  Playback`):
  - Normal: an output step a video frame, the output times.
  - 1/2 to 1/256: that much slower.
  - Auto (the default): Normal, slowed where the frames come closer, so
    that each of them is shown for a video frame. Closeness is averaged
    over two frames either side, as an impact's frames crowd its peak.
  - How far Auto slows is weighted by how far the contact forces' total
    swings within an output step either side, over the most it swings in
    the run: the rate is Normal times (the frames' own rate over Normal)
    to that share. The largest swing gets the full slow-down, one a
    twentieth of it a few percent. Unweighted, a lever rattling on its
    posts at 59 N, between a block's bounces, slowed as much as the
    block's landing at 528 N. The speed never got back to Normal between
    rattles 3 to 5 ms apart, and the block was seen flying in slow motion
    from 64 to 95 ms (the user's eye).
  - Tried against it, the user's own rule: the forces' change a video
    frame limited, each gap between frames as long in the video as its
    change takes at the limit. In this run, with the largest swing over
    15, 30 or 60 video frames, the video was 6.0, 8.6 and 14.0 s; the
    weighted rule's, 5.4 s. The user chose the weighted one ("weighted
    looks the best").
  - `SlowMotion` (the study's; the panel's "Slow-mo" by Speed at Auto):
    video frames each frame takes where the forces swing the most, 1 by
    default. The rate there is the frames' own over it, and elsewhere as
    much less slow as they swing less: at 4, a rattle at a twentieth of
    the largest swing is 4^0.05, 7%, slower.
  - Auto's rate changes by at most EASE = 1.5 from one video frame to
    the next: the lower envelope of cones of slope fps (EASE - 1) / EASE
    about each gap's own rate.
  - A video frame's time between the run's frames is the parts' between
    them: each one's position on the line, its turn by slerp
    (`updateForTime()`).
  - The panel says how fast the run goes against real time at the frame
    shown. The video export goes the same way.
- **The plot goes by the video's time** (`Playback.window()`, the user's
  ask: "do not scale the whole timeline when zoomed in, scale only the
  slowed down time period"). It shows as much of the video as the whole
  run takes at Normal: the run at Normal's scale, a part Auto plays slower
  stretched as much as it is slower, the window following the time shown.
  A first cut narrowed the run's time shown by the speed, which zoomed the
  whole of it.
- **Its timeline is a movie player's slider** under it, on the same axis:
  - filled up to the time shown, with a handle on it;
  - ticks an even step of the run's time apart: spreading as the speed
    eases in, closing up as it eases out;
  - each slowed span shaded, with the run's time under it where the speed
    begins to ease in and where it is back at Normal (`slowSpans()`:
    from where it is a tenth slower, those that slow to half or less);
  - a click or a drag on it, or on the curves, goes to the time there,
    played on from there if playing;
  - + and - at the plot's top right, and the mouse wheel, zoom in and out
    about the time shown, out to the whole video.
- **Peak-safe drawing** (M4, Jugel et al. 2014). Of the values in each
  pixel's column, the first, the least, the largest and the last. A spike
  narrower than a pixel is drawn to its height with the whole run shown.
  A frame through an impact that a stress series did not solve is passed
  over, not a gap.
- **To FEM** solves every frame by default: an impact's are a handful,
  and with them the stress plotted follows it. Solved at its peak alone
  (the part's contact forces' local largest, a choice), the stress zoomed
  in was a line from the output frame before to the peak and on to the
  one after, an impact of 60 us drawn 5 ms wide.
- **A saved video shows the Results box and the time**, so that what
  the manual's videos show takes no script. Save Animation's menu (not
  upstream) holds two choices, both on by default and kept in the
  preferences (Mod/Assembly VideoResultsBox, VideoTime):
  - With the Results box: the box beside the 3D view, as tall as the
    view at most, at one scale for every frame, down to its value line
    (`videoBoxImage()`, its buttons left out) -- each frame shown through
    `showPlayed()`, so the plot, its slider and the value are that
    frame's. Only when the box shows.
  - With the time and speed: the panel's two labels' text over the
    view's top left, on a light backing, whatever the view's background.

  Both off, the frames are the 3D view's, as upstream's. The panel is
  left at the frame it was at, its labels and plot too. Found on the way:
  the panel widened and narrowed as it played, its frame, time and speed
  labels as wide as their text then, and the Results box with it -- a
  saved or grabbed box resized from frame to frame; the labels are now
  as wide as their widest (`fitPlayerLabels()`).


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
