# Fluid simulation (CFD): a native workbench on OpenFOAM

Status (2026-11-03): a design only. No code stands behind it. Work starts
after the assembly simulation work (`docs/AssemblyDynamics.md`) is done;
until then this document is the plan to start from.

Status (2026-11-21): the assembly simulation work is done (D4b closed,
16cead244a) and this starts. Still a design, revised on the user's
direction the same day: **several solver backends behind one interface**
(sec 10 surveys them, sec 11 is the design for it), and **the first
result a dam break in CAD parts** (sec 12), which moves the free surface
from F5 to the front. Secs 1 to 9 stand as written except where secs 10
to 13 say otherwise; sec 13 lists what the user has to rule on before
anything is installed or built.

## 1. What this is

Fluid flow and heat transfer through and around the parts of a model, set
up on the model's own faces, solved, and shown in the 3D view -- the job
SOLIDWORKS Flow Simulation, Simcenter FloEFD, Autodesk CFD and Creo Flow
Analysis do inside their CAD.

"Native" means built the way Assembly simulation is: C++ document objects,
a neutral model of plain data, a backend interface whose sessions can be
served by another process, results kept in the document and drawn by the
fork's renderer. It does not mean a solver of our own. The solver is
OpenFOAM, run as a separate program.

It does not extend the CfdOF addon. CfdOF is a Python workbench that writes
OpenFOAM cases and shows results in ParaView; what this fork needs from CFD
-- faces referenced through the element map, results in the view and the
browser, the solver out of process, motion from Assembly -- sits in the
parts CfdOF does not have, and in an architecture it would have to be
rewritten to fit. Its knowledge is reused instead (sec 3.2).

## 2. What exists today

On this box (checked 2026-11-03):
- VTK 9.6.2 in `.conda/freecad` (pinned), with `vtkOpenFOAMReader` and
  `vtkStreamTracer`: the env can read an OpenFOAM case and trace
  streamlines in process.
- The FEM workbench is built, with Netgen (LGPL) linked in; no gmsh,
  CalculiX or Elmer executables in the env.
- No OpenFOAM anywhere: not on Windows, not in the WSL distros (`Nobel`,
  Ubuntu 24.04, 8 cores; `Ubuntu`, stopped). Windows has 16 cores.
- CfdOF is not installed. A read of its source (master a90f60c, v1.37.3)
  is summarised in sec 3.

In the fork, the pieces this builds on:
- Assembly's solver interface (`Mod/Assembly/App/Solver/Backend.h`): "Every
  argument and result is plain data, so a session can be served by another
  process." The fluid backend is that pattern with the process boundary
  real from the start.
- Assembly's Simulation object, its kept results (a run played back after
  reopening), its sensors, plots, video and glTF export.
- The bgfx renderer and the scene streaming tier (`docs/RenderEngine.md`,
  `docs/SceneStreaming.md`): what draws results on desktop and in the
  browser.
- The element map (TNP): face references that survive a recompute.
- Content-addressed blobs in the document (`docs/FileBlobsManager.md`):
  where large result fields can live.
- The headless server and process boundaries planned in
  `docs/ComputeBoundaries.md` and `docs/RoadMap.md` workstream 2: a solve
  is a long-running job the server owns, the GUI a client of its progress
  and results.

## 3. Ground rules

### 3.1 Licences

- OpenFOAM (both the Foundation's and OpenCFD/ESI's) is GPL-3.0-or-later.
  Linking its libraries into FreeCAD (LGPL-2.1+) would make the combined
  program GPL. So OpenFOAM is only ever run as separate executables: the
  case is written as files, its utilities and solvers run as processes,
  their output read back as files. Writing OpenFOAM's file formats
  (dictionaries, `polyMesh`) ourselves is not linking. Shipping OpenFOAM's
  binaries beside the application is allowed with their source offered;
  decide that when packaging (sec 9).
- CfdOF is LGPL-3: its templates and its mapping of boundary conditions to
  dictionaries may be reused, with attribution.
- VTK is BSD: linked in process, as FEM already does.
- HiSA (CfdOF's compressible solver) is GPL too: the same rule as OpenFOAM.
- Not legal advice; check before the first release that bundles anything.

### 3.2 What to take from CfdOF

Read, not run: its case templates per OpenFOAM version (`Data/Templates`),
how each boundary type becomes `U`, `p`, `T`, `k`, `omega`, `nut` entries,
its solver choice by physics (`getSolverName()`), its meshing settings for
cfMesh and snappyHexMesh (characteristic length, refinement, layers, the
point inside), and its dependency checker (which OpenFOAM, ParaView, MPI it
finds and where). Its gaps are this plan's targets (sec 4).

### 3.3 The repo's rules

As for Assembly simulation: every feature validated against a closed form
or a published benchmark, in a test; a video of each new simulation
feature in the manual repo (`assembly/simulation/...` has the pattern; this
gets `fluid/...`); ASCII; GUI tests through `scripts/gui-test.cmd`.

## 4. The target: what the CAD-embedded tools do

| | CfdOF today | FloEFD / SOLIDWORKS Flow | This plan |
|---|---|---|---|
| Incompressible flow, laminar and RANS | yes | yes | F1 |
| Heat in the fluid, buoyancy | yes | yes | F3 |
| Heat through solid parts (conjugate) | no | yes, with contact resistance | F3 |
| Radiation, solar load | no | yes | later |
| Fans, heat sinks, PCBs, compact models | no | yes (add-on modules) | later |
| Rotating parts | constant-speed rotating mesh only | yes | F4 |
| Motion from the assembly | no | no (FloEFD: rotation only) | F4 |
| Free surface (VOF) | isothermal | yes | F5 |
| Compressible, high Mach | yes (HiSA) | yes | F5 |
| Meshing from CAD without tuning | no: lengths, a point inside, layers | yes: immersed boundary Cartesian | F1 aim |
| Results in the CAD view | no: ParaView only | yes | F1 |
| Parametric studies, design points | no | yes | F5 |
| Faces kept through a recompute | no: `Face12` | yes | F1 |
| Assemblies built of Links | no: `Part::Feature` only | yes | F1 |

## 5. Architecture

### 5.1 The module

A new module, `Mod/Fluid` (App and Gui), not a part of FEM: FEM's objects
are built around a structural analysis container and its solvers, and the
FEM port is still being evaluated (`docs/FemPortEvaluation.md`). Shared
pieces (Netgen, VTK readers, result colour maps) are used from where they
are, not copied.

### 5.2 Document objects (C++, as Assembly's Physics items)

- `Fluid::Study` -- the container and the run, as `Assembly::Simulation`:
  analysis type (steady, transient), physics (incompressible,
  heat transfer, buoyancy, turbulence model), end time or iterations, output
  interval, the backend, and the run's status. Group extension holding the
  rest.
- `Fluid::Domain` -- what is fluid: internal (the cavity inside the
  selected solids, closed at the openings by lids the user picks or the
  study finds) or external (a box or cylinder around them, its size
  defaulted from the parts). The fluid volume is computed, not drawn: a
  Boolean of the enclosure against the parts, kept as the domain's shape.
- `Fluid::Boundary` -- one per condition: inlet (velocity, flow rate, mass
  flow, total pressure), outlet (pressure, outflow), wall (no slip, slip,
  moving, rough; thermal: temperature, heat flux, adiabatic, coefficient),
  opening, symmetry, periodic. Its faces by `PropertyLinkSubList` through
  the element map, so they survive a recompute; faces left unassigned are
  walls.
- `Fluid::Region` -- a volume with a role: porous, rotating, a heat source,
  an initial value.
- `Fluid::FluidMaterial` -- from the Materials system (density, viscosity,
  conductivity, specific heat; temperature tables where they exist); solids
  take their own material for conjugate heat.
- `Fluid::MeshControl` -- the base cell size (defaulted from the domain),
  refinement on faces and in regions, boundary layers.
- `Fluid::Monitor` -- what the run reports while it goes and keeps: force
  and moment on faces, pressure drop between faces, a point's value, a flow
  rate -- Assembly's sensors' counterpart, plotted live, able to stop a run
  at convergence.
- `Fluid::Result` -- the kept result: each boundary's fields on its faces,
  monitors' series, and the case reference to read the volume from on
  demand (slices, streamlines). Large arrays as content-addressed blobs;
  the volume field optionally kept too (it can be gigabytes).

### 5.3 The neutral model and the backend

As Assembly's `Solver::Model`: a plain-data description the study builds
from the document, in SI -- the fluid volume as a closed triangulation with
a patch id per triangle (one patch per boundary object, one per wall face
group), the regions likewise, the materials, the physics, the mesh
controls, the monitors -- and nothing that points back into the document.

`Fluid::Solver::Backend` opens sessions on a model: `mesh()`,
`run()`, progress (time or iteration, residuals, monitors' values),
`cancel()`, `results()` -- each call plain data, each long one
asynchronous. The OpenFOAM backend is the first; the interface leaves room
for a second (sec 8).

### 5.4 From the model to a mesh

Two routes, both written by us, both measured before one is the default:
- OpenFOAM's own meshers -- snappyHexMesh (hex-dominant, layers) or cfMesh
  (cut-cell, layers), run as processes: the model's surfaces written as
  one STL per patch, their dictionaries from the mesh controls. What the
  CAD-embedded tools' "no tuning" asks for is defaults derived from the
  geometry: the base size from the domain and the smallest gap, refinement
  at faces by their curvature and closeness, the point inside from the
  domain's own solid.
- Netgen in process (LGPL, already linked): a tetrahedral mesh, its dual
  for polyhedra, written as OpenFOAM's `polyMesh` by our writer. No extra
  executable, and the mesh is the model's own.

### 5.5 Running OpenFOAM

A launcher abstraction, one per place OpenFOAM can live:
- WSL: `wsl.exe -d <distro>` with the case's path translated; OpenFOAM
  from the Foundation's or ESI's apt repository on Ubuntu.
- Native Windows: blueCFD-Core (MSYS2), or ESI's MinGW build.
- A container: Docker or Podman, the case directory mounted.
- Linux natively, and later a remote machine or the headless server's own
  workers.

The backend runs the steps as processes (mesh, check, decompose, solve,
reconstruct), parses each solver's log as it is written -- residuals,
Courant number, time, the monitors' function object output -- and reports
them as progress. Cancel stops the process tree; a run can resume from its
last written time. Parallel runs by MPI decomposition (MS-MPI on Windows,
Open MPI elsewhere).

### 5.6 Results in the view

The case read with `vtkOpenFOAMReader` in process. What is drawn:
- each boundary's fields on its faces, coloured (pressure, velocity
  magnitude, temperature, wall shear, heat flux), on the parts themselves;
- cut planes through the volume, coloured, with vectors;
- streamlines from a seed line or face (`vtkStreamTracer`), coloured,
  animated for a transient run;
- iso-surfaces; probes and monitors as plots, like Assembly's.

Drawn by the bgfx renderer from the render cache, so they stream to the
browser viewer with the model. FEM's VTK post-processing pipeline (Coin)
is the fallback while the renderer path is built, not the destination.

### 5.7 Coupling with Assembly

- One way, motion into the flow: a rotating part's speed from its joint's
  motion (a rotating region, AMI); a part's whole motion from a kept
  Assembly run, written as tabulated rigid-body motion for a moving mesh.
- One way, forces out of the flow: the force and moment on each part,
  from its faces, as a load on the Assembly body (a time series for a
  transient run).
- Two way: OpenFOAM's rigid-body motion solver with Chrono stepping the
  mechanism, exchanging motion and forces each step -- through preCICE or
  our own exchange. Last, and only with a test that needs it.

### 5.8 Out of process and headless

The solver is already another process; the backend's session can be too.
A study's run is a job the headless server owns (ComputeBoundaries.md
boundary B): the GUI, the browser or a script starts it, watches its
progress and reads its results through the same plain-data calls. The case
directory lives with the server; results come to clients as the fields on
faces and slices the view asks for, not the volume.

## 6. Phases

Each phase ends with its tests green, its validation numbers in this
document ("Built:" and "Measured:" entries, as AssemblyDynamics.md keeps
them) and its video in the manual.

- **F0. The toolchain.** OpenFOAM running here: Foundation or ESI, WSL or
  native Windows, measured on one tutorial (the lid-driven cavity, then
  `pitzDaily`): install, run, time, parallel. The launcher for the chosen
  places; the VTK reader reading the result in FreeCAD's process; a
  decision record of which OpenFOAM the backend targets first.
- **F1. Steady incompressible internal flow.** Study, Domain (internal,
  with lids), Boundary (inlet, outlet, wall), FluidMaterial, MeshControl;
  the neutral model; the case writer for `simpleFoam` (laminar and k-omega
  SST); meshing by both routes (sec 5.4), compared; the runner with live
  residuals; the result on faces and on a cut plane in the view; Links and
  sub-assemblies taken as their shapes. Validation: laminar pipe pressure
  drop against Hagen-Poiseuille, dp = 8 mu L Q / (pi R^4), to 1% and its
  parabolic profile; turbulent pipe friction factor against Haaland's
  formula to 5%; the lid-driven cavity at Re 100 and 1000 against Ghia,
  Ghia and Shin (1982); a backward-facing step's reattachment against
  Armaly et al. (1983).
- **F2. External flow and forces.** An external domain; Monitor (force,
  moment, drag and lift coefficients, convergence stop); streamlines.
  Validation: a sphere's and a cylinder's drag coefficient at low Reynolds
  numbers against the published curves.
- **F3. Heat.** Heat transfer in the fluid and buoyancy
  (`buoyantSimpleFoam`, or the Foundation's `foamRun` solver modules);
  then heat through solid parts, conjugate, from their materials
  (multi-region). Validation: laminar pipe Nusselt numbers 3.66 (wall
  temperature) and 4.36 (wall heat flux) when fully developed; natural
  convection in a square cavity against de Vahl Davis (1983) at Ra 1e3 to
  1e6; a composite wall's conduction against its closed form.
- **F4. Transient and motion.** Transient runs (`pimpleFoam`), rotating
  regions from a joint's speed, motion from a kept Assembly run, forces
  back to Assembly as loads. Validation: vortex shedding behind a cylinder
  (Strouhal number about 0.165 at Re 100, Williamson 1996); a rotating region's torque
  against a closed-form Couette flow between turning cylinders.
- **F5. Further.** Free surface (VOF), compressible flow, radiation,
  compact models (fans, heat sinks), parametric studies as variants of
  the document, a second backend. Each only with a test case that needs
  it.

## 7. Testing

- Unit tests (C++, gtest) for the model builder, the case writer (its
  dictionaries against files kept in the test tree), the log parser and the
  `polyMesh` writer -- none need OpenFOAM.
- Validation tests (Python, `TestFluid*`) run the solver on small meshes
  and compare against the closed forms of sec 6. They skip, saying so, when
  no OpenFOAM is found -- as Assembly's GUI tests skip without a display --
  and a separate long suite holds the cases too slow for every run.
- Each case's mesh fine enough for its tolerance and no finer: the
  tolerance and the measured error both written in the test's docstring,
  as Assembly's tests do.

## 8. Second backends, later

Superseded 2026-11-21 by secs 10 and 11: several backends are designed
for from the start. As written on 2026-11-03:

Not planned, noted so the interface does not close them off:
- SU2 (LGPL-2.1): a finite volume solver that could be linked in process.
- A GPU lattice Boltzmann preview: seconds, not hours, for a first look
  while editing -- what Ansys Discovery offers. Research, not product.
- Chrono's FSI module (SPH; in the vendored source, not built; it needs
  CUDA as far as known -- unchecked): free surface sloshing coupled to
  Chrono bodies directly.

## 9. Open questions

- Which OpenFOAM first: the Foundation's (versions 11 to 13 reshaped its
  solvers into `foamRun` modules) or ESI's (`v2xxx`, the Windows builds).
  F0 decides on what runs here and what users can install.
- Packaging: bundle an OpenFOAM build (GPL: its source offered) or require
  an install and find it, as CfdOF does. The WSL route has no Windows
  installer to bundle.
- The fluid volume from dirty CAD: small gaps, touching faces, parts that
  overlap -- the Boolean that makes the domain has to report what it
  could not close.
- Result size: a volume field of millions of cells per time step. Keep
  faces and slices in the document, the volume in the case directory,
  referenced.
- Units and materials: fluid properties in the Materials system (density,
  dynamic viscosity, temperature tables) -- which cards exist, which to
  add.
- Whether the FEM workbench's VTK pipeline objects are worth sharing for
  results, or the renderer path makes them redundant.

## 10. The solvers surveyed (2026-11-21)

Read from the projects' own pages that day, and from the vendored Chrono's
source. "Checked" is read on the page or in the source; "summary" is seen
in a search result only; "unchecked" is said so. Nothing here is measured
on this box yet.

### 10.1 What each is

- **OpenFOAM, interFoam** (finite volume, volume of fluid). The free
  surface as a field on a mesh; pressures and forces on walls are what it
  is validated for (Kleefsman's case among them, summary). CPU, MPI. A
  separate program, GPL-3 (sec 3.1). ESI v2606 (2026-06) and the
  Foundation's 14 (2026-07), summary. Linux natively; Windows by WSL,
  Docker or ESI's MinGW build; macOS only in a VM or Docker (summary). On
  conda-forge for linux-64 only, OpenFOAM 2412 (summary). Needs a volume
  mesh of the fluid's space (sec 5.4). Bodies moved by the fluid: its own
  rigid-body mesh motion (small motions, no contact), or coupled to
  another program through preCICE.
- **Chrono FSI-SPH** (weakly compressible SPH, particles). In the vendored
  Chrono 10, not built. **CUDA only**: checked in
  `chrono_fsi/sph/CMakeLists.txt` ("requires CUDA") and on its install
  page (CUDA 12.8 or newer, an NVIDIA card, "cannot be built for MacOS").
  An AMD route (HIP) is in Chrono's development branch, not in 10.0. BSD-3,
  in process. Coupled to Chrono's bodies and finite elements as it comes:
  boundary markers from each body's collision shape, forces back each
  step. Periodic and inlet/outlet sides (summary). Validated on dam break
  and sloshing in its authors' papers (summary), none found against
  Lobovsky or Kleefsman.
- **Chrono FSI-TDPF** (potential flow, Cummins' equation). Floating bodies
  in waves from coefficients computed beforehand by a boundary element
  program. No free surface is resolved: no dam break, sloshing, jet or
  filling. CPU; needs HDF5 (checked in its CMakeLists). Out of this plan.
- **SPlisHSPlasH** (SPH: DFSPH, IISPH, PBF, WCSPH and others). MIT; a C++
  library with a Python package; CPU with OpenMP and AVX (its GPU part is
  the neighbour search alone, CUDA); Windows and Linux tested, macOS
  wheels seen (summary). Static and moving rigid boundaries, emitters,
  viscosity, surface tension. Its moving bodies are its authors' own
  library's; feeding it another solver's bodies is glue to write. Made
  for computer graphics: fast and robust, no engineering validation of
  its pressures found.
- **DualSPHysics** (weakly compressible SPH). LGPL-2.1; CPU (OpenMP) and
  CUDA builds; Windows and Linux, no macOS; v5.4 (2025-03). The best
  validated particle code found: dam break on an obstacle, sloshing,
  wave makers, open boundaries. A pipeline of programs over case files
  (GenCase, the solver, post tools). Its coupling with Chrono is to a
  patched Chrono 9 of its own, not ours.
- **preCICE** (a coupling library, LGPL-3). Official adapter for OpenFOAM;
  none for Chrono in its list, but Chrono's development branch has a
  `Chrono::PRECICE` module with an OpenFOAM demo -- after 10.0, not in a
  release (checked in Chrono's changelog).
- **Lattice Boltzmann**: FluidX3D's licence forbids commercial use --
  out. Palabos (AGPL or GPL, reports differ) and waLBerla (GPL-3) are
  frameworks cases are compiled against: separate programs, heavy to
  package; not pursued.
- **PySPH, Taichi, Warp**: research and demonstration frameworks; none a
  backend to ship.

### 10.2 Side by side

| | method | runs on | macOS | licence | how it is run | bodies moved by the fluid |
|---|---|---|---|---|---|---|
| OpenFOAM interFoam | VOF on a mesh | CPU, MPI | VM only | GPL-3 | separate program, case files | its own mesh motion, or preCICE |
| Chrono FSI-SPH | SPH | NVIDIA GPU only | no | BSD-3 | in process | Chrono's, as it comes |
| SPlisHSPlasH | SPH (DFSPH...) | CPU | likely | MIT | in process (library) | glue to write |
| DualSPHysics | SPH | CPU or NVIDIA GPU | no | LGPL-2.1 | separate programs, case files | its own Chrono 9 |

What follows from it:
- No one solver runs everywhere, is validated for pressure, and moves our
  Chrono bodies. The interface has to take both kinds: a library stepped
  in process and a program run over files.
- The only solver that runs on every machine we ship to is SPlisHSPlasH,
  and it is the one without engineering validation -- so it has to be
  validated here, against the same cases as the others.
- OpenFOAM is the reference for pressures and forces, and what secs 1 to
  7 already plan for the rest of CFD.
- Chrono FSI-SPH is the shortest way to parts and fluid pushing each
  other, for those with the card.

### 10.3 What to test a dam break against

- **Martin and Moyce (1952)**: the front's position against time, both
  scaled by the column's base. The table's numbers were not readable from
  the scan found; they are to be taken from the paper before a test is
  written, not from memory.
- **Lobovsky et al. (2014)** (arXiv 1308.0115, checked): tank 1610 x 600 x
  150 mm, gate 600 mm from the end wall, water 300 or 600 mm deep, 100
  runs each. The front's speed over sqrt(g H): 1.56 at 300 mm (others'
  experiments 1.3 to 1.7; Ritter's ideal 2). Pressures on the far wall at
  3, 15, 30 and 80 mm up: at 300 mm the median peaks 91.4, 57.5, 43.8 and
  30.2 mbar, with their 2.5% and 97.5% bands (the lowest sensor's 75 to
  131). The peak scatters run to run: a test holds the band and the
  impulse, not one peak.
- **Kleefsman et al. (2005), SPHERIC test 2**: tank 3.22 x 1 x 1 m, column
  1.228 m long and 0.55 high, a box 0.161 x 0.403 x 0.161 in its way;
  water heights at four probes and pressures at eight sensors on the box,
  the data published with the benchmark. A dam break on a part, which is
  the user's case.

## 11. Several backends behind one interface

Sec 5.3 has one interface with OpenFOAM first. This is what it takes for
the solvers of sec 10 to sit behind it.

### 11.1 The neutral model says what, never how

The study builds one plain-data model, SI, for every backend:
- the **walls**: each part as a closed triangulation with a patch id per
  triangle, and whether it is fixed, moved as given (a kept Assembly run)
  or moved by the fluid;
- the **fluid at the start**: closed volumes filled with it (a block of
  water is a solid the user picks or draws), and its material;
- the **space**: the box nothing leaves, or its open sides;
- **inlets and outlets** on faces (later; not for the dam break);
- gravity, the end time, the output interval;
- a **resolution**: one length, the size of what is to be resolved. A
  particle backend takes it as its spacing, a mesh backend as its base
  cell; nothing in the model names particles or cells;
- the **monitors** (sec 5.2): force on faces, a height at a line, a
  pressure at a point.

A backend that needs a volume mesh makes it from the walls itself (sec
5.4); one that needs boundary particles samples them itself.

### 11.2 Capabilities, asked not assumed

As Assembly's `Capabilities`: each backend says what it can do --
free surface, inlets, moving walls, bodies moved by the fluid, heat,
steady runs, bitwise repeatable -- and what it needs to run (a program
found on this machine and its version, a GPU of a kind). The panel lists
the backends that can run the study as set up and says why the others
cannot. A study keeps the backend and version its results came from.

### 11.3 Two ways to run, one session

- **In process**: a library stepped by us (Chrono FSI-SPH, SPlisHSPlasH).
- **A program**: a case written, processes run, logs and files read (sec
  5.5: OpenFOAM, DualSPHysics).

Both behind the same session calls -- `prepare()`, `run()` with progress,
`cancel()`, `results()` -- every argument and result plain data, so the
session itself can be served from another process (sec 5.8). A GPL
program is only ever on the far side of files and a process (sec 3.1).

### 11.4 Results: three kinds, each drawn one way

A backend returns what it has; the view draws by kind, not by backend:
- **particles** per frame (positions, optionally velocity and pressure):
  drawn as instanced spheres, or as a surface made from them;
- **a surface** per frame (the free surface as triangles; a VOF backend's
  half-full iso-surface, or a particle backend's own surface
  reconstruction);
- **fields on a mesh** (secs 5.6): faces, cut planes, streamlines.

And for every backend alike, the **monitors' series** -- which is what
the tests compare, so one validation test runs against every backend
that can run its case. Frames are kept as content-addressed blobs, read
lazily, as Assembly's results are; the renderer plays them back, and the
browser tier reads the same frames (Simulator.md stages 4 and 5).

### 11.5 Bodies moved by the fluid

The seam is already in the vendored Chrono and builds without CUDA: the
generic `Chrono_fsi` library (`ChFsiSystem`, `ChFsiInterfaceGeneric`,
depending on Chrono's core alone, checked in its CMakeLists). A fluid
solver is anything that implements `ChFsiFluidSystem`: it is handed the
bodies' states (`LoadSolidStates`), steps, and hands back the force and
torque on each (`StoreSolidForces`). So:
- Chrono FSI-SPH is one such, as it comes;
- an in-process backend of ours (SPlisHSPlasH) becomes one by a wrapper:
  poses in, forces out;
- a program (OpenFOAM) becomes one by a wrapper that exchanges the same
  two things each step over preCICE or files -- last, as sec 5.7 says.

The Assembly's Chrono backend owns the bodies in every case; a fluid
backend never owns a mechanism. One-way cases (walls fixed, or moved as a
kept run has them) need none of this, and come first.

## 12. The free-surface track, first

Ruled 2026-11-21: the first working result is a dam break in CAD parts.
These phases go ahead of F1 to F4; F0's toolchain work is shared.

- **S0. The toolchain, measured.** Each candidate backend run on its own
  dam break example on this box, outside FreeCAD: does it install here,
  how long a run takes at what resolution, what comes out. Decides the
  order of S2. No FreeCAD code.
- **S1. The frame of it.** `Mod/Fluid`: Study, the fluid at the start,
  the walls from the assembly's parts (Links and sub-assemblies as their
  shapes), a height and a pressure monitor; the neutral model (sec 11.1);
  the backend interface (secs 11.2 to 11.4); kept results; playback of
  particles or a surface in the 3D view on the Simulation panel's
  timeline. Tested with a backend of test data.
- **S2. The first backend, and the dam break.** A tank and a block of
  water made of parts; released; played back. Validation: the front's
  speed against Lobovsky's 1.56 sqrt(g H) and the others' range; the wall
  pressures' impulse and peaks within Lobovsky's bands; then Kleefsman's
  box (heights and pressures against the published data).
- **S3. The second backend**, on the same tests unchanged -- the proof
  that the interface is one.
- **S4. Parts moved by the fluid** (sec 11.5): a floating block at rest
  at its draught (Archimedes), then one dropped in; first on the backend
  where it is cheapest.
- **S5. Inlets**: a jet from a face onto a part, the force against the
  momentum's closed form, rho Q v.

### 12.1 S0 measured (2026-11-21)

All three candidates installed and ran a dam break of their own on this
box (i7-12700, 20 threads, RTX A2000 12 GB, WSL2), none needing sudo.
Everything is under `~/.cache/fluid`; no FreeCAD code. The cases differ
(each solver's own example), so the times say what a run costs, not which
solver is faster. Other sessions were running; these are not benchmarks.

| Backend | Install | Case | Size | Flow time | Wall time |
|---|---|---|---|---|---|
| OpenFOAM v2412, interFoam | conda-forge, own env, 1.1 GB | 2D damBreak tutorial | 2268 cells | 1 s | 2.9 s, 1 core |
| | | 3D, box in the flow, fixed mesh | 258048 cells | 1 s | 246 s, 8 processes |
| SPlisHSPlasH 2.19.1, DFSPH | PyPI wheel, 5.5 MB | DamBreakModel | 6859 particles | 2 s | about 3 s, 8 threads |
| | | the same, radius halved | 59319 particles | 2 s | 42 s, 8 threads |
| Chrono FSI-SPH (10.0.0) | conda CUDA 12.6 toolkit, 2.4 GB, then built from our submodule | demo_FSI-SPH_DamBreak | 16731 fluid + 13596 wall particles | 2 s | 70 s, GPU |

What went wrong on the way, each of which the workbench has to handle:

- **OpenFOAM's conda package runs serial only as installed.** Its
  libraries carry an RPATH of `sys-mpich`, the package ships the MPI
  layer in `lib/mpich-3.3`, so the dummy one loads and a parallel run
  stops with "The dummy Pstream library cannot be used in parallel
  mode". An RPATH beats `LD_LIBRARY_PATH`; a link `lib/sys-mpich ->
  mpich-3.3` fixes it. The download we offer must make that link (or
  preload the library), and the search must test a parallel run, not
  only find `interFoam`.
- **OpenFOAM writes a lot**: 2.1 GB for 50 frames of that 3D run with
  every field from 8 processes. The session must ask for the fields the
  study's results need and no more.
- **The 3D tutorial refines its mesh as it runs** (to about 180000
  cells from 32768) and took over 16 minutes of one core for 0.4 s of
  flow before it was stopped. Refinement stays off until asked for.
- **SPlisHSPlasH's wheel crashes on import here**: it bundles its own
  libGLX, libGLdispatch and libOpenGL beside the system's libGL, and the
  two dispatch libraries in one process jump through a null pointer.
  Pointing the bundled three at the system's made it run. In FreeCAD's
  process we would meet the same clash, so we build the library from
  source without its GUI rather than load the wheel.
- **SPlisHSPlasH on every thread is far slower than on a few**: the
  6859-particle run took 164 s on 20 threads with the box busy, and
  about 3 s on 8. The session sets the thread count; it never takes the
  default.
- **A resolution set after the scene is read gives nonsense** in
  SPlisHSPlasH (`--param particleRadius`: particles inside the walls, no
  end). The scene is written whole at the resolution wanted.
- **Chrono FSI-SPH built and ran first try**, CUDA found from the conda
  env by `-DCUDAToolkit_ROOT`; its demo steps at a fixed 1e-4 s, which is
  the 70 s.

The order of sec 13 stands. PyPI has SPlisHSPlasH wheels for Linux,
Windows and macOS arm64, which bears out "every machine".

### 12.2 S1 as built, step 1: the App side (2026-11-21)

`src/Mod/Fluid` (`BUILD_FLUID`, on by default), no Gui yet.

- **The model** (`App/Solver/Model.h`): sec 11.1 as plain structs in SI --
  walls and fluid volumes as triangulations, the space's box and its open
  sides, gravity, the times, one resolution, the monitors. `signature()`
  is a hash of all of it; `sampled()` gives the grid points inside a
  closed surface (crossings counted along z, so a cavity is left out) for
  the particle backends to start from.
- **The backend interface** (`Backend.h`, `Registry.h`): `Capabilities`
  (what the frames are -- particles, surface, fields -- and what it can
  do), `Availability` (can it run on this machine, why not, which
  version), a `Session` with `prepare()` and `run()` into a `FrameSink`
  that takes each frame as it is made and is asked whether to stop.
  `Registry::select()` gives the backend a study names, or for no name
  the first that is no test data and can run here -- never another in
  the place of one that is missing.
- **The backend of test data**, `FreeFall`: the fluid sampled at the
  resolution, every particle in free fall to the space's floor. Each
  frame has a closed form. Never chosen unless named.
- **The objects** (`Study.h`): `Fluid::Study` (a group: `Walls`,
  `TimeEnd`, `OutputInterval`, `Gravity`, `Resolution` -- 0 is a
  twentieth of the fluid's smallest side --, `Backend`, `Results`,
  `Status`), `Fluid::InitialFluid` (`Volume`, `Density`, `Viscosity`;
  water at 20 degrees by default) and `Fluid::Monitor` (`Kind`: Height or
  Pressure, `Point`). A wall or a volume is taken as the shape it shows
  where it stands, so a Link is a wall as its copy.
- **The run** is in a thread of its own: `start()`, `getProgress()`,
  `cancel()`, `finish()`; `run()` is the first and the last. A run that
  fails or is cancelled keeps nothing and leaves the results before.
- **Kept results** (`ResultsArchive.h`): a zip in the study's `Results`
  (a blob of the document), `index.json`, `times.npy`, `monitors.npy` and
  one `frames/NNNNNN.points.npy` per frame (float32, mm; a surface's
  frames also `.triangles.npy`), which numpy reads as they are. Unlike
  the Assembly's, a frame is written as the run makes it and read only
  when asked for (`getFrame(i)`): a run's frames do not fit in memory
  together. `isStale()` compares the kept signature with the study's
  model now. Objects are named without the document, so a file saved
  under another name finds its monitors.
- **Tests**: `Fluid_tests_run` (9) and `TestFluidWorkbench` (15), all
  against closed forms on `FreeFall`.

### 12.3 S1 as built, step 2: the workbench (2026-11-21)

In Python, with no Gui library of its own yet.

- **The workbench** (`InitGui.py`, `FluidCommands.py`): Create Study (the
  solids selected are its walls), Fluid at the Start (the solids selected
  are full of fluid; shown seen through), Height Monitor and Pressure
  Monitor (at the point picked), Run and Play Back (the study's panel).
  The study worked on is the one selected, a selected child's, or the
  document's only one -- never a guess between two.
- **The panel** (`TaskFluidStudy`): the solver -- every registered
  backend, one that cannot run here marked and its reason as the tool
  tip, and "the first that can run here" --, the resolution (0 shown as
  Automatic), the end time and the frame interval; Run, which becomes
  Cancel, with the frames and the time made so far; then the kept
  results: how many frames, from which backend, whether they are of the
  study as it was before a change; Play and the timeline; each monitor's
  value at the frame shown.
- **A frame in the view** (`FluidPlayback.py`): `Frames` reads a kept
  frame when it is shown; `FrameView` draws it by its kind -- particles
  as points of a size on the screen, a surface as triangles seen from
  both sides -- in one Coin node under the view's scene graph, replaced
  frame by frame. While frames show, the solids the fluid started in are
  hidden; closing the panel brings them back.
- **Tests**: `TestFluidWorkbench` 23 (was 15): the frames by time, the
  monitors' rows, the study chosen, and the drawn node's points and
  triangles; the workbench driven in the GUI by a script (commands,
  panel, run, play) under `~/.cache/fluid/gui`.

Not yet: particles drawn as spheres or as a surface made from them, a
plot of the monitors, saving the playback as a video, and drawing by the
renderer rather than Coin (so the frames reach the browser viewer). The
manual's page waits for S2: on the test data the water falls through the
tank's floor, which is nothing to show a user.

### 12.4 S2 as built: OpenFOAM, and the dam break (2026-11-21)

- **The case** (`App/Solver/OpenFoamCase.h`): `makeCase()` writes a model
  as interFoam's case, text only -- a background mesh of the space's box
  at the resolution (blockMesh), the walls' STL files and snappyHexMesh's
  dictionary cutting the mesh around them (no refinement, no layers yet),
  the fluid's volumes as STL and setFields filling the cells whose
  centres are inside them, water's properties from the first volume and
  air at 20 degrees, laminar, the space's open sides a total-pressure
  patch and the rest walls. The time step is the solver's, held to a
  Courant number of 0.5.
- **What is asked of the solver, and no more**: the free surface (the
  half-full iso-surface) at each output time, and the monitors at every
  step -- pressure by `probes`, height by `interfaceHeight`. The fields
  are kept for the last time only. The prototype's 1 s of Lobovsky's tank
  (144900 cells, 100 frames) left 73 MB in its case, where S0's tutorial
  left 2.1 GB.
- **The monitors have a time base of their own** (`FrameSink::sample()`,
  `samples.npy`, `MonitorTimes`): a solver reports them as often as it
  steps. Measured first: sampling at the frames (10 ms) or at every step
  (about 1 ms) gave the same peaks on Lobovsky's wall here, 39.4 against
  40.6 mbar, so the low peak below is not a sampling matter; the finer
  base is kept because sensors are compared at that rate.
- **The backend** (`OpenFoamBackend.h`): each step a process of its own
  under bash, in a process group that Cancel ends whole (mpirun and the
  solvers under it), in an environment without this program's library
  paths. While interFoam runs, the frames it has finished writing and the
  monitors' new rows are handed on, so the panel's progress moves and a
  cancelled run stops within the poll. A step that fails keeps its case
  and says where, with the solver's own error. Above some twenty thousand
  cells a process, up to 8 and half the cores, the case is decomposed and
  run under MPI (the preference `Mod/Fluid/Processes` sets it).
- **Found, never shipped** (ruling 4): the preference
  `Mod/Fluid/OpenFoamPath` (a folder or an `etc/bashrc`), the
  environment's `FLUID_OPENFOAM_PATH`, the environment's own PATH, the
  workbench's download folder, then `/usr/lib/openfoam`, `/opt` and
  `~/OpenFOAM`. A conda prefix is activated without conda. Only the ESI
  line (v2012 on) is taken: the Foundation's dictionaries differ. An
  install whose programs load the serial MPI stand-in is run on one
  process. Not found, the backend says where it looked, the panel shows
  it under the solver, and offers **Download OpenFOAM** (`FluidOpenFoam.py`,
  Linux): conda-forge's package into the workbench's own folder by a
  conda tool found here, or by micromamba fetched and checked against its
  published sum; then the package's parallel layer is mended (S0's
  finding) and the backends looked for again. Run for real here into a
  fresh profile: found at once, and the dam break test passed on it.
  Windows (WSL) and macOS (a container) are not driven yet.
- **`OpenTop`** on the study: the space's side against gravity is open to
  the air, by default.

Measured through the workbench, the tank a CAD part:

| | here | Lobovsky 2014 |
|---|---|---|
| front speed, 10 mm cells, 144900 cells | 1.66 sqrt(g H) | 1.56; others' 1.3 to 1.7 |
| peak at 3 mm | 40.5 mbar | 91.4 median, 75 to 131 |
| peak at 15 mm | 42.2 | 57.5 |
| peak at 30 mm | 41.4 | 43.8 |
| peak at 80 mm | 18.7 | 30.2 |

1 s of flow in 201 s on 7 processes. On a slice of the tank 20 mm wide
with 5 mm cells (the prototype's case, sides slipping) the peaks were
44.6, 58.9 and 45.2 mbar: the sensors at 15 and 30 mm at the experiment's
medians, the lowest still at half. **Open**: the peak at 3 mm, a cell or
less from the floor's corner at either size, rose little with the finer
mesh; whether it is resolution, the incompressible air, or the corner's
treatment is not known. The water's volume in the mesh is within 1% of
the solid's (cells are in or out by their centres).

Known limits: a wall thinner than a cell is not resolved; a background
cell's centre exactly on a wall's face (walls half a cell thick, aligned
with the space's box) leaves the mesher an even choice; turbulence is
off; one liquid; no refinement at the walls.

- **Tests**: `Fluid_tests_run` 15 (the case's files, the surface and the
  tables read back); `TestFluidWorkbench` 30, two of them a dam break and
  a cancel run on OpenFOAM where there is one (`FLUID_OPENFOAM_PATH` in
  the gate here), skipped elsewhere.
- **The manual**: `fluid/study/dam-break`.

## 13. For the user to rule (2026-11-21)

1. **The first backend (S2).** OpenFOAM: validated, CPU, already this
   document's plan, but a separate GPL program users install, no native
   macOS, and a mesh to make. SPlisHSPlasH: MIT, in process, every
   machine, but to be validated by us. Chrono FSI-SPH: nearest to the
   Assembly's dynamics, NVIDIA only. Proposed: S0 measures all three
   here, then OpenFOAM first (the reference the others are held to) and
   SPlisHSPlasH second (what every user can run).
2. **Installs S0 needs on this box**, none made yet: OpenFOAM (the
   conda-forge package into a separate env through the tunnel, or apt in
   WSL with sudo); the CUDA toolkit for Chrono FSI-SPH (several GB);
   SPlisHSPlasH from PyPI into a scratch env.
3. **A module of its own, `Mod/Fluid`** (sec 5.1), or the free-surface
   study as another kind of Assembly simulation, sharing its panel,
   timeline and results store. Proposed: `Mod/Fluid`, using Assembly's
   playback and results code where it can be shared.
4. **What users without OpenFOAM see**: the backend listed as not found,
   with how to install it (as CfdOF does), or OpenFOAM bundled (GPL:
   source offered). Sec 9 has this open already.


**Ruled 2026-11-21.** 1, 2 and 3 as proposed: S0 measures all three,
then OpenFOAM first and SPlisHSPlasH second, Chrono FSI-SPH after; the
three installs are allowed; the module is `Mod/Fluid`. 4: OpenFOAM is
never bundled -- the workbench searches for an install and, finding
none, offers to download one.
