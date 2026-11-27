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
medians, the lowest still at half. The same slice at 2.5 mm, one cell
wide (154560 cells, 0.6 s in 4017 s on 4 processes):

| slice's cells | 3 mm | 15 mm | 30 mm | front |
|---|---|---|---|---|
| 5 mm | 44.6 mbar | 58.9 | 45.2 | 1.72 sqrt(g H) |
| 2.5 mm | 54.4 | 64.1 | 45.9 | 1.73 |
| experiment's median | 91.4 | 57.5 | 43.8 | 1.56 |

The peak at 3 mm rises with the mesh and has not settled: +22% for cells
half the size, where the sensor at 30 mm moved 2%. It is a matter of
resolution at the floor's corner, at least in part; at 2.5 mm it is at
60% of the median and below the experiment's band. Not chased finer: a
user's case is a part in the flow, which the next test is. **Open**:
what is left at the corner once the mesh is fine enough (the air is
incompressible here, and nothing refines the mesh at a wall). The
water's volume in the mesh is within 1% of the solid's when the solid's
faces fall on cell faces (cells are in or out by their centres).

**Kleefsman's box** (SPHERIC test 2, the benchmark's own data,
`test_case_2_exp_data.xls`), through the workbench: the tank and its box
one CAD part, 20 mm cells (431520), 6 s of flow in 4164 s on 7
processes. Pressures on the box's face towards the water (P1 to P4,
lowest first) and on its top (P5 to P8), over the 6 s:

| | arrives (s) | peak (Pa) | impulse (Pa s) | rms (Pa) |
|---|---|---|---|---|
| P1 | 0.405 (exp 0.395) | 10644 (exp 11225) | 12701 (exp 13392, -5%) | 512 |
| P2 | 0.420 (0.415) | 8447 (7886) | 10478 (10958, -4%) | 384 |
| P3 | 0.440 (0.425) | 4613 (6492) | 8523 (9496, -10%) | 484 |
| P4 | 0.475 (0.435) | 2940 (7198) | 5199 (7092, -27%) | 779 |
| P5 | 0.995 (1.095) | 2826 (1984) | 2807 (2804, 0%) | 470 |
| P6 | 0.995 (1.065) | 2578 (1860) | 2820 (3027, -7%) | 390 |
| P7 | 1.115 (1.070) | 2497 (2024) | 2696 (3338, -19%) | 438 |
| P8 | 1.105 (1.065) | 3196 (2082) | 2696 (2933, -8%) | 503 |

Water heights, the rms difference over the 6 s as a share of the
experiment's peak: 10% behind the box (0.496 m), 8% (0.992 m), 10%
(1.488 m), 3% in the reservoir (2.638 m). The second wave, back from the
far wall, reaches the box at 4.8 s here and 4.7 s there.

- The lower half of the face is close in peak and impulse. P4, a cell
  from the box's top edge (20 mm cells, the sensor 20 mm down), misses
  the first peak: 2940 Pa for 7198. The top's sensors get the impulse
  within 19% but arrive early and spike where the experiment rises
  slowly. Both are what 8 cells across a box would give; not run finer
  yet.
- **The data's H1 and H3 columns are the figure's probes the other way
  round**: its H1 rises at 0.2 s and its H3 at 1 s, and water cannot be
  behind the box (0.496 m) before it is half a metre from the gate
  (1.488 m). They are compared that way.
- **The water started 10 mm high**: 0.55 m is 27.5 cells of 20 mm, and a
  cell is in or out by its centre, so the column was 0.56 m (2% more
  water). A fluid whose faces do not fall on cell faces is set to the
  nearest cell; partly filled cells at the start are not written yet.
- Heights come back in the document's length unit (mm), pressures in Pa.

Scripts and data: `~/.cache/fluid/kleefsman` (kleefsman.py, compare.py),
`~/.cache/fluid/proto` for the slices.

Known limits: a wall thinner than a cell is not resolved; a background
cell's centre exactly on a wall's face (walls half a cell thick, aligned
with the space's box) leaves the mesher an even choice; turbulence is
off; one liquid; no refinement at the walls; the water at the start is
whole cells.

- **Tests**: `Fluid_tests_run` 15 (the case's files, the surface and the
  tables read back); `TestFluidWorkbench` 30, two of them a dam break and
  a cancel run on OpenFOAM where there is one (`FLUID_OPENFOAM_PATH` in
  the gate here), skipped elsewhere.
- **The manual**: `fluid/study/dam-break`.

### 12.5 S3 as built: SPlisHSPlasH, the second backend (2026-11-21)

The same study on particles, and the same tests asked of it.

- **A program of ours runs it, not this process.** Sec 11.3 had
  SPlisHSPlasH stepped in process. It is built as C++11 against its own
  bundled Eigen 3.4, with its own definitions and vector instructions;
  FreeCAD's Eigen here is 5 and needs C++14, the solver's parts do not
  compile as C++17, and its simulation is one object a process. So
  `FluidSplish` (`src/Mod/Fluid/Runner/SplishRunner.cpp`) is compiled as
  the solver is and linked with it, and the backend runs it over a
  folder: `case.txt`, the particles as raw points, lines on its output
  for frames, monitors and errors (`SplishCase.h`). A run that dies takes
  no document with it, and nothing of the solver's is in FreeCAD's
  process. It needs no shell: Windows is not shut out by how it is run.
- **Driven with no scene file**: the fluid's particles, the walls'
  particles, DFSPH, the standard viscosity at the liquid's own value,
  gravity; the solver's step limit from the speeds, and a longest step
  of our own (below).
- **The walls are particles the workbench lays** (`Splish::boundaryOf`),
  one sheet behind every face the fluid can reach, of the parts and of
  the space's closed sides: a grid of 1.5 radii in each face's plane, a
  row along each crease. Two rules decide what stays: a particle is deep
  enough (no fluid at it nor within the distance of it in its plane),
  and fluid can rest against the wall in front of it or within the
  distance of there. So a sheet stops short of an edge the fluid goes
  round, and runs on past an edge where the face ends against another
  part.
- **The distance behind the face is 1.2 radii, measured**: with the
  sheet there the layer of still water against the wall has the rest
  density at the start (999.0 for 998.2); at 1.0 it is 12% over, and the
  first step throws the water off the wall at 2.6 m/s.
- **Parts that only touch**: a tank of five plates lost two thirds of a
  dam break's water in 1.5 s along its corners, where no part has a
  corner for a sheet to turn. With the second rule the plates' sheets
  meet behind the corner the parts make together, and it loses none.
- **The longest step follows the resolution**: no longer than gravity
  takes to move a particle a hundredth of the spacing. At the solver's
  own 5 ms a tank of still water at 5 mm flew apart (16000 particles,
  201 left): the solver's limit from the speeds says nothing of water at
  rest.
- **A frame is the first step at or past its time**, and says its time.
  Cutting a step short to meet the time exactly made pressure spikes of
  1e6 Pa: the solver's pressure is what undoes a step's compression,
  over the step's square.
- **Monitors.** A height is the particles in a column over the point, as
  a depth, the column without an edge (with one, the depth was 4% out by
  where the line stood among the particles). A pressure on a wall is the
  fluid's push on the wall's particles around the point, along the
  wall's normal, over their area -- the workbench finds the nearest face
  within the resolution and hands its normal over. The solver's own
  pressure at the particles against a wall read half as much again as
  the water's weight at a floor; the push reads the weight. A pressure
  in the open fluid is the solver's own, weighted by the kernel.
- **A particle that leaves the space is gone**: parked far off and held,
  since the solver cannot take one out.
- **Found, or said missing**: the preference `Mod/Fluid/SplishPath`, the
  environment's `FLUID_SPLISH_PATH`, then `bin/FluidSplish` beside
  FreeCAD. `Mod/Fluid/Threads` sets the threads (8 at most by default,
  S0's finding).
- **Built behind `BUILD_FLUID_SPLISHSPLASH`** (off by default), the
  solver found one of two ways. The package of `splishsplash-feedstock`,
  in the environment or named by `SPlisHSPlasH_DIR`: its CMake package
  carries the solver's definitions, its bundled Eigen and the vector
  instructions it was compiled with. Or a tree it was built in by hand:
  `FLUID_SPLISHSPLASH_SOURCE`, `FLUID_SPLISHSPLASH_BUILD`
  (`-DSPH_LIBS_ONLY=ON -DUSE_PYTHON_BINDINGS=OFF
  -DEIGEN3_INCLUDE_DIR=<source>/extern/eigen`, then `ninja SPlisHSPlasH`).
- **The feedstock** (ruled 2026-11-21: one of our own; none exists, on
  conda-forge or elsewhere) answers the two things that stood between
  the solver and a package. Its build fetches Discregrid, CompactNSearch
  and GenericParameters from GitHub as it builds: the recipe pins the
  three as sources and builds them first. It compiles with
  `-march=native`: the recipe sets `CI_BUILD` and asks for x86-64 with
  AVX2 and FMA. The recipe's build script was run by hand into a prefix
  of its own, the runner built against that package, and the workbench's
  35 tests pass with it. Not yet done: a run through `rattler-build`,
  Windows and macOS, a repository and a channel upload.

Still water, a tank 200 x 100 with 100 mm of it, through the workbench;
the mean over 0.2 to 0.5 s as a share of the water's weight above:

| spacing | depth | floor | wall, 80 mm down | in the water, 50 mm down |
|---|---|---|---|---|
| 10 mm (2000 particles) | 100% | 99% | 100% | 100% |
| 5 mm (16000) | 101% | 100% | 99% | 100% |
| 2.5 mm (128000, 179 s) | 101% | 100% | 100% | 100% |

Lobovsky's tank (sec 12.4's set-up), 1 s of flow:

| | SPlisHSPlasH 10 mm | OpenFOAM 10 mm | experiment |
|---|---|---|---|
| time taken | 28 s, 8 threads | 201 s, 7 processes | |
| front speed | 1.76 sqrt(g H) | 1.66 | 1.56; others' 1.3 to 1.7 |
| peak at 3 mm | 47.0 mbar | 40.5 | 91.4 median |
| peak at 15 mm | 56.5 | 42.2 | 57.5 |
| peak at 30 mm | 47.5 | 41.4 | 43.8 |
| peak at 80 mm | 15.8 | 18.7 | 30.2 |

At 5 mm (216000 particles; 832 s with two other runs on the box): front
1.76, peaks 74.8, 77.0, 45.6 and 16.5 mbar. The peak at 3 mm rises with
the resolution as OpenFOAM's does and is nearer the experiment at 5 mm
than OpenFOAM's slice at 2.5 (54.4); the one at 15 mm goes over the
median (the experiment's band was not looked up for it). The front is
fast on both solvers. The peaks are of one run each: a second run of a
smaller case moved its peak by 4%.

Kleefsman's box (sec 12.4's set-up), 20 mm, 6 s of flow in 591 s on 8
threads with two other runs on the box (OpenFOAM: 4164 s):

| | arrives (s) | peak (Pa) | impulse (Pa s) |
|---|---|---|---|
| P1 | 0.415 (exp 0.395) | 11274 (exp 11225) | 11476 (exp 13392, -14%) |
| P2 | 0.435 (0.415) | 7813 (7886) | 9900 (10958, -10%) |
| P3 | 0.455 (0.425) | 3943 (6492) | 7510 (9496, -21%) |
| P4 | 0.500 (0.435) | 1637 (7198) | 3114 (7092, -56%) |
| P5 | 1.480 (1.095) | 1351 (1984) | 1167 (2804, -58%) |
| P6 | 1.460 (1.065) | 2046 (1860) | 2286 (3027, -24%) |
| P7 | 1.480 (1.070) | 2571 (2024) | 2256 (3338, -32%) |
| P8 | 1.480 (1.065) | 2474 (2082) | 1801 (2933, -39%) |

Water heights, rms over the 6 s as a share of the experiment's peak: 9%,
7%, 8% and 5% (OpenFOAM: 10%, 8%, 10%, 3%). The face's lower half is as
good as OpenFOAM's in its peaks and lower in its impulse; the top of the
face and the box's top are worse: the water is on the top 0.4 s late and
a third to a half short in impulse. The box is 8 particles high.

Known limits, measured:

- **A wall is soft by about half the spacing.** A particle pressed on a
  wall rests with its middle near the wall's face (a quarter radius from
  it in still water, up to 0.12 of the spacing past it in a strike), not
  a radius from it.
- **Still water takes a quarter second to settle** at 10 mm, its surface
  moving 4 mm meanwhile (one particle 8.5).
- A wall thinner than 1.2 radii gets no particles; the walls' particles
  are found by asking, for each, whether places near it are inside a
  part -- every part's every triangle: slow for parts of many triangles.
- One liquid; no air; no moving walls yet; the pressure's peaks are of
  one run (the solver is not repeatable bit for bit across threads).
- The solver's own boundaries from a distance field (its default) are
  not used; they may hold a wall's place better and are untried.

- **Tests**: `Fluid_tests_run` 23 (was 15): the walls' particles of a
  closed space and of plates that touch with no hole anywhere, the
  fluid's particles, the case's files, the runner's lines.
  `TestFluidWorkbench` 35 (was 30). The dam break and the still water
  are one test each for every backend (`FluidTests/DamBreak.py`,
  `StillWater.py`): the backend's name and what its frames are is all a
  test class says. Plus the plates on SPlisHSPlasH.
- **The manual**: `fluid/study/particles`.

### 12.6 Particles drawn as water (2026-11-21)

A frame of particles was drawn as points. It is now drawn, by default, as
the closed surface around them, and that surface is a body of water to the
bgfx renderer.

- **The surface** (`App/ParticleSurface`, `Study.getSurface(index)`): each
  particle adds a weight `(1 - q^2)^3` to a grid of half the spacing, the
  sum scaled to 1 deep inside particles a spacing apart; the surface is
  where it is a half, which against a flat face of particles is half a
  spacing past their middles, the fluid's edge, whatever the reach. Cut by
  marching tetrahedra (six a cell about one diagonal, so neighbours agree
  and the surface is closed), a point for every grid line it crosses, a
  normal a point from its triangles by their area.
- **A long reach, and drops.** A reach of 1.4 spacings showed every
  particle of a moving fluid: on a block of them each a third of a
  spacing off its place the surface was 7% more than the block's and its
  top faced up by 0.90 of its area. With 2 spacings it is under 5% more
  and over 0.97. But a particle alone rises only to 0.2 with it, and a
  strand of three to 0.36: thrown water would be gone. So every particle
  the surface does not hold (the long field at its own place is under a
  half) is a drop, and adds a second, short field (reach 1.2 spacings),
  scaled so that a particle alone is a ball 0.45 of a spacing in radius:
  two a spacing apart run together, two 2.5 apart do not. Only such
  particles add it -- added wherever the long field was weak it swelled
  the body's own face. The crossing on a grid line is found on the cubic
  through the two nodes' values and slopes, not the straight line (a drop
  is a node or two wide, and came out a quarter too small), and a point's
  normal is the field's slope there, not its triangles': a drop of a few
  triangles is lit as a ball. Tried before this and dropped: a ball of 20
  or 80 triangles a particle, as water (thousands of bodies of water a
  few pixels each were a haze over the whole tank) and then as pale lit
  balls (the user: "doesn't even look like liquid").
- **Spray** (2026-11-22). With every particle in the surface the height
  of a splash was still a cloud of separate specks: at 5 mm the thrown
  water is one particle wide, further apart than drops run together,
  and no surface over single particles is a sheet. What other renderers
  do (Blender, Houdini; Ihmsen et al. 2012 triangulate "neglecting
  isolated particles") is treat such particles as a layer of their own.
  So `surfaceOf` takes `sprayBelow`: a particle whose long field at its
  own place is under that many times what a particle alone has (itself)
  is left out of the surface and handed back as spray. `SprayBelow` is
  2.5: one alone (1), a pair a spacing apart (1.42), three in a row are
  spray; a clump of four and more is a drop. In the dam break at 5 mm
  that is 14% of the particles at the height of the splash (13333 of
  96000 at 0.60 s) and next to none before and after. The view draws
  them as mist: unlit pale points one pixel in size, three quarters seen
  through. Left out and not drawn at all the body is as clean but the
  thrown water simply vanishes.
- **Cohesion in the solver, tried for the same end** (2026-11-22): the
  pull between particles of Akinci et al. 2013, `cohesion <k>` in the
  runner's case, from the preference `Mod/Fluid/Cohesion` (0, off). At
  the solver's own 0.05 the splash is as without; at 0.25 it gathers in
  clumps and strands, and 8% are spray where 14% were. But the peak
  pressure on the block moves from 3305 Pa to 3665 (0.05) and 3684
  (0.25), the level at the end from 33.9 mm to 33.0 and 30.4, the run
  from 225 s to 232 and 256: it is not the validated run, and the look
  did not need it once the spray was a layer. Kept as it is, off; a
  setting of the study when there is a use for it. The solver's own
  method is not used: it works out the wall's kernel even with the pull
  to the walls at nothing, a fourth root that is of a number a rounding
  below zero when a particle is at the kernel's edge, and one run in
  four at 10 mm was not-a-number from some step on (and ran its hour to
  the end and said Done). The runner has the force itself, less the wall
  part (twelve runs of twelve whole), and now stops with an error at the
  first step whose particles are not numbers.
- **The walls** (2026-11-25). The user, of the mechanism's video: "Water
  near surface looks discontinuously poping." Measured on the hinged
  bar's frames 10 ms apart, as height maps of the top on a 2.5 mm raster:
  2.5% of the cells within 5 mm of the tank's walls changed by more than
  2 mm from one frame to the next, 2% along the bar (up to 22 mm), 0.01%
  in the open water; and 21 to 28 particles of the still water were spray,
  all at the waterline against a wall or the bar. Beside a wall half of
  what is about a particle is the wall's, so the field read low there and
  hovered about the level: the surface dipped to the particles, and the
  ones at the top flipped between body, drop and spray as they jiggled.
  Now `surfaceOf` is given the walls, as points half a spacing apart with
  the normal out of the solid (`wallPointsOf`), and a particle within
  reach of a wall whose foot is on it has its mirror image across it in
  the field -- across two or three at once in a corner -- on the water's
  side of those walls alone, so nothing grows inside a solid or leaks
  through a thin one. `Study::wallPointsAt` takes them from the model
  once per results and spacing, a moved part's where the frame has it
  (`Study.getWallPoints(index)`). Against the tank's walls the cells
  changing by more than 2 mm went to 0.01%, the spray of still water from
  about 26 particles to 1; in a test tank the top along the walls came
  from 1.2 mm below the open top to 0.09, and the volume's worst change
  between frames of jiggled still water from 3.77 of a spacing's cube to
  1.23. Along the bar what is left is real at this spacing: near its tip
  its top face is within a millimetre of the water's level at 17 deg, so
  a film one particle thick comes and goes on it, and the line where the
  two meet moves three times as far as the water does. A foot "on" a
  wall was first one within three quarters of a spacing of a wall point,
  which takes in feet past the wall's edge: a particle out past the
  bar's end, bobbing a millimetre about the plane of its top, was
  mirrored down across the top whenever above it, and heaped water up
  to 4 mm high over the end -- there one frame, gone the next, with a
  glint on its side. The points are half a spacing apart on triangles
  of at most that side, so a foot on the wall is within 0.29 of a
  spacing of one: now the nearest point of each wall is taken, the
  mirror image is whole to 0.3 of a spacing and fades out by 0.45, and
  the water keeps half a spacing off a wall. In a test of that particle
  stepping a fifth of a spacing up across the plane, the volume's change
  went from 0.43 of a spacing's cube to 0.053. What is left at the tip
  is the water's own: the particles past the end sit 1 to 1.5 mm above
  the open water's top layer and bob a millimetre. 47 ms a frame
  for the bar's tank.
- **The spacing** is kept with the results (`spacing` in index.json, mm);
  results kept before it take the study's resolution as it is now.
- **Cost**, the dam break of sec 12.5 at 5 mm: 0.4 to 0.6 s a frame, 450000
  to 580000 points (0.75 s and a million points with every particle in
  the surface).
- **In the view** (`FluidPlayback.FrameView`): the surface with its
  normals under an `SoFCRenderMaterial` node saying `water TRUE`, found
  by name, so the workbench needs nothing of the renderer. Coin draws it
  as a smooth lit body. The bgfx renderer draws it as water when its
  water surface is on -- checked on screen, no renderer change needed.
  The spray is points of its own under the same root.
  The panel's "Particles as a surface" (preference
  `Mod/Fluid/ParticlesAsSurface`, on) switches back to points.
- **What the look needs of the renderer's settings**, found by trying:
  `View/RenderCache` 3 and `View/Render/Type` "bgfx - OpenGL";
  `WaterSurface`; `Volumetric` AND the view's `Render_Light`, without
  which the water has no depth tint at all (the tint by depth runs only
  with the volumetric pass, and that only with the scene light);
  `VolumetricDensity` small (1e-6: at 0, automatic, the air itself is
  hazy at this size); `WaterAbsorption` about 0.02 (the default 0.2 is
  for a pool seen in metres and makes 100 mm of water opaque blue);
  `WaterPlanarReflection` off (a splash has no plane to mirror about);
  `WaterWaveStrength` 0 (the ripples are a pool's, the surface here has
  its own shape). The workbench sets none of these: they are the user's.
  Tried further and kept for the manual's pictures: the shadow soft and
  light (the view's `RenderShadow_SmoothBorder` 60,
  `RenderShadow_Transparency` 0.75) with the light from nearly overhead
  (`Render_LightDirection` (-0.25, 0.35, -1)) -- the tank is drawn four
  fifths clear and still casts a whole shadow, which was the heaviest
  thing in the picture; the water (0.10, 0.45, 0.80) at absorption 0.012
  -- (0.40, 0.70, 0.90) at 0.008 and in-scatter 0.8 since 2026-11-25, below.
- **The colour by depth** (2026-11-25). The user: "tune down the water
  color and if possible, make color follow absorption law so that thuinner
  file is nearly transparent". The renderer's water shader did follow
  Beer-Lambert, over the column from the surface to the water's back face,
  but a film over a part was tinted as much as the open water: it laid a
  fifth of the water's colour on whatever the depth, and its back face is
  the FARTHEST of the water along the ray -- behind a part standing in
  the water, the pool's floor. The column now ends at the first opaque
  surface behind the water (the prepass depth, which holds opaque fills
  only), and the fixed tint is left to where the depth is not known
  (tests/gui/water-film-is-clear.py: a plate 2 under the surface of 56 of
  water moved a channel 0.411 of the 0.506 the open water did; now
  0.041). Tried at the hinged bar's frames, the same settings first, then
  less absorption at the old colour (the water went grey: less of it
  absorbed lets the dark floor and the shadow through, and the in-scatter
  fell with it), then paler colours: (0.40, 0.70, 0.90) at absorption
  0.008 and in-scatter 0.8 kept for the manual, light blue and the parts
  in it seen; (0.55, 0.80, 0.95) at 0.010 and 0.6 drifted grey-green.
  `FluidPlayback.WATER` is the new colour; the absorption and in-scatter
  stay the user's settings, which the manual's scripts set.
  Tried and dropped: the water as a glass body of index 1.33 (grey, no
  colour by depth), the tank as glass (the block behind it is displaced
  out of recognition), bloom and the sun's disc (no visible change).
- **Cycles** (tried 2026-11-21; `BUILD_CYCLES` was off in this box's
  tree and its three libraries not in the environment: installed as
  docs/DevEnvironment.md says, at the versions it names, nothing else
  moved). The path tracer leaves a body of water out, by design
  (docs/CyclesIntegration.md, "Skipped"), so the surface goes to it as
  glass: the same node saying `water FALSE glass TRUE glassIOR 1.33
  glassDensity 0.01`. With the colour and the tank of the renderer's
  picture the water came out dark and looked solid; a paler colour
  (0.55, 0.80, 0.95) and the tank nine tenths clear gave clear blue
  water. CPU only here (no nvcc): 1280 x 720, 64 samples, denoised, about
  22 s a frame of 700000 triangles; 256 samples 46 s. No shadow on the
  ground, and the water's floor is lit by the environment alone. The
  workbench does not switch the node itself: the manual's script does.
  It draws no points, so no mist: the water alone, which with the
  spray out of the surface is a clean body (11.6 s a frame at 64
  samples, a million triangles).
- **Open**: OpenFOAM's frames are an open sheet (the surface between water and
  air, not closed against the walls) and stay drawn as they were; the
  settings above could be offered as one switch.

### 12.7 S4 as built, step 1: a free part in the fluid (2026-11-23)

Decided here, not ruled: S4 comes in two steps. Step 1 is a part with no
joint -- it floats, sinks, is pushed -- and the particle backend moves it
itself, inside its runner. Step 2 is a part of a mechanism, where Chrono
owns the bodies and the fluid backend exchanges poses and forces with it
(sec 11.5). Step 1 needs none of the exchange and gives the tests their
closed form (Archimedes), so it is first; nothing in it stands in the way
of step 2, which replaces the runner's own integration by Chrono's.

- **The object**: `Fluid::FreeBody`, a child of the study: `Part` (a
  link) and `Density`. A part is a wall or a free body, not both; no
  part, no density, or a part also in `Walls` is an error of the model.
  Command `Fluid_AddBody`: the density from the part's material if it has
  one, else asked for; the part is taken out of `Walls`.
- **The model**: `WallMotion::Free`, with `MassProps` (mass, centre,
  inertia about the centre, from the shape by `BRepGProp`), and
  `freeWalls(model)`. The signature takes the mass properties to nine
  digits: a shape read back from a file gives values that differ in the
  last places, and the kept results were stale after every reload.
- **The capability** (sec 11.2): `freeBodies`. A study with a free body on
  a backend without it does not start: "<backend> cannot move a part by
  the fluid". OpenFOAM has it off (a moving mesh is its own piece of
  work); SPlisHSPlasH on; the test backend on (a free wall falls
  g t^2 / 2 down to the floor of the space).
- **Results**: each frame carries a `Motion` for each free body
  (translation and rotation from where the part stood at the start), kept
  as `bodies.npy` (frames x bodies x 7, mm and a quaternion) beside the
  particles. Python: `getResults()["Bodies"]`, `getFrame(i)["Bodies"]`
  (placements), `getBodies(i)`. Playback sets the parts' own `Placement`
  for the frame shown and puts them back on leaving, before a new run and
  before other results are loaded.
- **SPlisHSPlasH**: the body is sampled as the walls are (a sheet behind
  every face, a particle at each corner) and written to the case as
  `body` lines with its mass properties. In the runner it is a dynamic
  rigid body of ours: after each step of the solver the force and torque
  on its particles are read (Akinci's boundary handling gives them), the
  body is stepped (semi-implicit Euler) and its particles moved.
- **Against the walls**: SPlisHSPlasH has no contact between bodies, so
  the runner has a stand-in until Chrono does it: a spring and damper
  where the body's particles come within reach of a fixed wall's, and a
  drag of the water film in a band beyond that. It holds the body ONE
  PARTICLE'S WIDTH off the wall, the layer of water staying between them.
  Letting the faces touch was tried first and dropped: one landing in
  five flipped the block and threw 118 to 213 particles out of the tank;
  with the gap, twelve landings in twelve were calm and lost none.

Measured (SPlisHSPlasH, a block 60 x 60 x 40 in a tank 120 x 120, water
60 deep, 5 mm):

| case | expected | measured |
|---|---|---|
| half the water's density, put at its draught | underside at 39.96 | 39.24 in the mean after 0.5 s (0.7 deep, 3.6% of the draught) |
| the same let go 10 above the water | 44.97 (the water rises by what it pushes aside) | 44.78 in the mean after 1 s, still moving 2 each way |
| twice the water's density | on the floor | 4.8 to 4.95 above it (the gap), leaning up to 2.6 deg |

At 10 mm the floating block was 6 to 10 off its draught: a body needs
some eight particles across its least side, and the study does not check
that yet.

Tests: `FluidTests/Floating.py`, written for every backend that has the
capability, as the dam break is; the C++ tests cover the model, the
archive, the case writer and the test backend's fall. Scores: C++ 28 ->
31, Python 37 -> 50.

**Limits, all of step 1**: a body rests a particle's width off a wall; no
friction against the walls; two free bodies do not meet each other; no
joints. All four go with step 2, Chrono's contact and mechanism.

### 12.8 S5 as built: inlets, and the force on a part (2026-11-23)

The fluid comes in through a flat face, and a monitor reports what the
fluid presses a part with. On the particle backend and the test backend;
OpenFOAM has neither yet, and says so in place of running.

- **The object**: `Fluid::Inlet`, a child of the study: `Face` (one flat
  face of a part, a link with its sub-element), `Velocity` (the same all
  over the face, square to it, out of the solid the face is of),
  `Reversed`, `Density` and `Viscosity` (water's), `StartTime` and
  `EndTime` (an end not after the start is the run's). No face, an edge,
  no speed and a face that is not flat are errors of the model. Command
  `Fluid_AddInlet`: on the faces picked, the speed asked for.
- **The force**: a third kind of `Fluid::Monitor`, `Force`, with `Part`
  (one of the study's walls) and `Direction`: the fluid's force on the
  part along the direction, N. Command `Fluid_AddForceMonitor`, on the
  walls selected; the direction is the way the study's first inlet sends
  its fluid, or against gravity.
- **The space**: `OpenSides` on the study, beside `OpenTop`: the space's
  other sides are open too, the one gravity points to with them, and
  what leaves is gone -- what a jet in the open needs. Without gravity no
  side is the top and `OpenSides` is all six.
- **The model**: `Inlet` (the face as triangles, direction, speed,
  material, start, end) in `Model::inlets`; `MonitorKind::Force` with the
  wall's place and a direction. A study needs fluid at the start or an
  inlet, and may have both. The liquid is the first volume's, or with none the
  first inlet's (`liquidOf`). The signature takes the inlets only when
  there are any, so the kept results of every study without one stay
  theirs.
- **The capabilities** (sec 11.2): `inlets` and `forces`. The test
  backend sends a layer of points from the face each time the last has
  gone the resolution, each in free flight, and reports as the force the
  momentum the inlets bring a second.
- **SPlisHSPlasH**:
  - The face is sampled as one layer of particles, a grid of the
    resolution in the face's plane along its longest edge and centred on
    it (`inletOf`), the places whose middles are on the face; a face of
    less than half a particle's is too small. The case has an `inlet`
    line and `inlet.<k>.points`.
  - The runner puts a layer on the face each time the last has gone a
    particle's width, placed as far on as it is late, and carries the
    fluid within a kernel's width before the face at the inlet's speed
    whatever presses on it, so that what is past that has fluid behind it
    and not a void. Particles come from the ones that left the space
    first, then from ones kept for the inlets when the fluid was made
    (as many as the inlets can send in the run).
  - The solver's own emitters were not used: they are rectangles and
    circles only, and they have a fault that was found by doing the same
    -- a particle is left where it is for the step in which it comes out
    of the carried stretch, the layer behind runs into it, and the jet
    comes apart from there on (measured: its front a half faster than it
    was sent, the jet half as wide again before the plate, the force on
    the plate 30% over). Moved in that step too, the jet stays the grid it
    was sent as, at the speed it was sent.
  - A wall a force is asked of is a body to the solver, one that is
    never moved: the solver adds up the fluid's push on a body's
    particles and not on a fixed wall's. Its particles go in
    `forced.<k>.points`, out of `boundary.points` (`boundaryOf` says
    whose each particle is). This is the accounting the floating block
    was checked by (sec 12.7). A pressure monitor finds such a wall as
    any other.

Measured (a jet 20 x 20 mm out of a nozzle onto a plate 120 x 120 mm
square to it, 100 mm on, no gravity, the space open; the force's mean
over time from 0.15 to 0.3 s against density x area x speed^2):

| spacing | speed | expected | measured | over |
|---|---|---|---|---|
| 5 mm | 2 m/s | 1.597 N | 1.720 N | 7.7% |
| 2.5 mm | 2 m/s | 1.597 N | 1.733 N | 8.5% |
| 1.25 mm | 2 m/s | 1.597 N | 1.688 N | 5.7% |
| 2.5 mm | 4 m/s | 6.388 N | 6.923 N | 8.4% |
| 2.5 mm | 1 m/s | 0.399 N | 0.431 N | 8% (the runner alone) |

It goes with the speed's square, starts when the jet arrives (0.049 s
for 100 mm at 2 m/s), and reads 6 to 9% over. The closed form is of a jet
that leaves along the plate; some 200 of 4900 particles were found
behind the plate's face plane and off the jet's axis, so some of the
water comes back off the plate, which presses more. Not traced further.
The pressure where the jet's axis meets the plate read 1340 Pa, two
thirds of density x speed^2 / 2 (1996 Pa): a monitor's patch is three
spacings across, of a jet eight across.

A tap (40 x 40 mm, 1 m/s for 1.2 s) over a tank 200 x 200 mm at 5 mm:
15424 particles sent, 111 of them out over the rim in the first splash;
the level 0.8 s after the tap is shut 46.5 mm, where what is in the tank
makes 47.9 (still moving a millimetre each way).

Tests: `FluidTests/Jet.py` for every backend that has the capabilities;
`TestInlets` and the C++ tests on the model, the layer, the case's files
and the test backend. Scores: C++ 31 -> 35, Python 50 -> 60.

**Open**:
- OpenFOAM: an inlet is a patch of the mesh with a fixed velocity, and
  the force a function object over a part's patch. Neither is built.
- The flow through a face that is not whole spacings is that of the
  particles on it, not of its area: a round face 20 mm across at 5 mm is
  12 particles, 300 mm^2 for 314. The study does not say so yet.
- The force is on a wall held still. On a part the fluid moves the
  solver has it too, and it is not reported.
- A sheet thinner than a particle, as a jet makes on a plate, is spray
  to the surface drawn around the particles: such a run is looked at as
  particles.
- One speed over the face: no profile, no flow rate given in its place,
  no pressure inlet, no outlet other than the space's open sides.

### 12.9 S4 step 2, planned: parts of a mechanism in the fluid (2026-11-24)

Ruled 2026-11-24, to be built before F1 goes on. Step 1's part is free:
the runner moves it itself. A part held by joints -- a flap on a hinge, a
float on an arm, a wheel on its axle -- is the Assembly's, moved by its
Chrono backend with the joints, contacts, motors and springs it has
there; the fluid adds its push and takes the part's motion (sec 11.5: a
fluid backend never owns a mechanism). What is there to build on, and
what follows from it:

- The fluid is another process (FluidSplish, sec 12.5), the mechanism a
  thread of ours. They exchange, each step, the parts' poses and
  velocities one way and a force and torque on each the other, over the
  runner's pipes.
- The Assembly's run is one call, `Session::simulate()`, and inside it
  Chrono's steps are taken in substeps that are taken back -- a contact
  found late, a sensor's trip located by bisection. A fluid cannot be
  taken back. So the exchange is by pieces of the run, not by Chrono's
  substeps: over a piece the fluid's force on each part is held as it
  was found for that piece, and whatever Chrono does inside the piece it
  does under that force. The fluid is asked for a piece once; a piece
  taken again gets the same answer.
- The piece is the fluid's own step (tenths of a millisecond at 5 mm):
  that is the coupling step 1 has, the part integrated on the fluid's
  step from the fluid's force of that step, and it held for a block half
  the water's density. Longer pieces are not tried until that works.

The steps, each with its tests:

1. **The Assembly takes forces from outside.** `SimulationSpec` gains a
   source of outside loads: the bodies it pushes, the longest piece it
   allows, and a call that is given those bodies' poses and velocities at
   a time and a piece's length, and answers with a force and a torque on
   each for that piece. The Chrono backend cuts its run into such pieces
   and holds the answer on the bodies through each. Nothing of the fluid
   in it: tested with a source that is a formula (a constant force on a
   free body, its closed form; a spring's law on a hinged one).
2. **The runner moved from outside.** FluidSplish, told so by its case,
   moves no part itself: for each piece it reads the parts' states from
   its input, steps the fluid through the piece with the parts moving as
   told, and writes the mean force and torque on each. `SplishBackend`
   gets the session's other end. Tested with motions that are scripted:
   a block held under water (its buoyancy), a block pushed through it.
3. **The study and the mechanism together.** A study names an Assembly
   simulation; its free parts that are that assembly's are the
   mechanism's. The study's run starts the runner and the Assembly's run
   with the runner as its source of loads; both results are kept, on the
   same frames. Validation: a flap on a hinge under still water against
   the hydrostatic moment's closed form; a float on a hinged arm at rest
   at the angle where its buoyancy's moment is its weight's; then one let
   go, as S4's block was. A command, the manual's page and its video.

Open before it is built: whether a light part (a float a tenth of the
water's density) stays stable when its motion and the fluid's force are
a step apart -- step 1 was not tried below a half; the Assembly's
results and the fluid's being two kept things for one run.

**As built (2026-11-24), the three steps but for what is listed last.**

- **The Assembly** (`Solver/Backend.h`): `OutsideLoads` and
  `SimulationSpec::outside`; a body's state is its frame's pose, that
  frame's origin's velocity and the angular velocity, the answer a force
  at that origin and a torque, in the model's units. The Chrono backend
  asks at the start of each stretch of its run not asked for yet, from
  the bodies' states then, and holds the answer on the bodies (an
  accumulator each) until the next: a substep taken back and stepped
  again is within what was asked. `AssemblyObject::startSimulation(sim,
  PartLoads*)` is the same by parts, which it finds among the run's
  bodies.
- **The model** (`Model.h`): a wall's motion `Driven`, moved from outside
  as the run goes; `anyDriven()`. A study's moved parts are all driven or
  all the fluid's own.
- **The runner** (`driven 1` in its case): it prints `ask 0 <longest>`
  and waits; told `move <until>` and each wall's centre, turn, velocity
  and angular velocity, it puts the walls there, steps the fluid past
  `until` with the walls going on as told between its steps, and prints
  `pushed <time>` with the mean force and torque (about the centre) on
  each over the steps it took; then waits again, until `end`. A step is
  the solver's own to choose, so the fluid is up to a step ahead of what
  it was asked; asked for a stretch it has passed, it says the same
  again. The stand-in contact of step 1 is not used: a mechanism's
  contacts are the Assembly's.
- **Between them**: `WallDriver`, what a session asks of whoever moves
  its driven walls; `WallExchange`, the two threads' meeting, either
  side's end letting the other go; in `Study.cpp` the bridge that is a
  `PartLoads` to the Assembly and the mover to the fluid, turning body
  frames in mm and mN into walls' centres in SI and back, the torque
  taken from the centre to the frame's origin.
- **The study**: `Mechanism`, a link to an Assembly's simulation. With it
  the study's `FreeBody` parts are that mechanism's; its run starts the
  runner and the Assembly's simulation together, ends when the
  mechanism's does, and keeps both results -- the fluid's in the study,
  the Assembly's in its simulation, as each does alone. The piece is the
  runner's longest step: no longer than gravity takes to move a particle
  a hundredth of the spacing from rest, 2.3 ms at 5 mm and 1.6 ms at 2.5
  (an earlier draft of this said 0.3 ms, which was wrong).

Measured (`FluidTests/Mechanism.py`, S4's tank and block, the block on a
vertical slider of an assembly, 5 mm):

| Case | Result | Closed form |
|---|---|---|
| Half the water's density, put at its draught | bobs 2.1 mm down and 0.3 up, half that by 1 s; 0.7 mm deep in the mean | 39.96 mm |
| A tenth of the water's density | within 1.1 mm for 1 s | 55.99 mm |
| Nine tenths | within 1.3 mm | 23.94 mm |
| Half, let go 10 mm above the water | under its draught and back, the mean after 1 s within 3 mm | 44.97 mm |

The fluid's frames and the Assembly's are of one motion to 0.1 mm; the
run of 1 s takes 8 s. The light float answers the first open question
for this case: no growth seen. The spring test of step 1 says what the
lag costs where there is nothing to damp it, 2% a period at a tenth of
a millisecond for a period of 0.1 s.

A turning part, tried (`~/.cache/fluid/s4/arm.py`, not yet a test): a
bar 100 x 20 x 20 at half the water's density, hinged at one end 20 mm
above the water and let go level. Where its buoyancy's moment about the
hinge is its weight's, the water's level risen by what it pushes aside:
16.45 deg below level. At 5 mm it settles at 17.5 to 18.0 deg, 1 to 1.5
deg low, the same put there at the start; at 2.5 mm it does not settle --
the swing grows, about a tenth each swing, as step 1's spring test says
a force a piece behind the motion does. Shorter pieces at 2.5 mm were
being tried when the host hung (sec 12.10) and are not measured.

**The turning part as a test (2026-11-24).** `FluidTests/Mechanism.py`
has the bar of the trial above, on a revolute of an assembly, against the
closed form (`barAtRest`: the section clipped at the water's level, the
level risen by what the section pushes aside, bisected for the angle where
the moments meet). What it showed first: the bar rests low for every
density, by more than the block on its slider is deep, and by more again
when heavier -- not the lag, and not a share of the buoyancy lost (3.4%
less moves the half-density bar 0.3 deg, not 1). One cause fits all of it:
the fluid sees a part a skin thinner than it is. A skin of 0.3 mm is the
block's 0.7 mm; the bar, 20 thick, feels the same skin more:

| Density (of the water's) | Closed form | Measured, 5 mm, 4 threads | Skin that fits |
|---|---|---|---|
| 0.3, let go level | 13.08 deg | 13.61, swinging 0.67 | 0.33 mm |
| 0.5, let go level | 16.45 | 17.42, swinging 0.25 | 0.43 |
| 0.5, put at rest | 17.14 (less water) | 18.13, swinging 0.15 | 0.43 |
| 0.7, let go level (3 s) | 21.07 | 23.80 | 0.38 |

The heavier bar is turned the furthest by the same skin: it is mostly
under, where a little less buoyancy takes a lot more turn to make up. Put at rest, the bar
starts in the water, where the fluid is not seeded: 96 particles fewer,
the level 0.83 mm lower, a rest 0.7 deg deeper -- the closed form takes
the water the fluid has, from its count. The tests hold each case between
the closed form for a skin of 0.25 mm and one of 0.6 (a twentieth and
an eighth of the spacing). A torque taken to the wrong point would put it
degrees out: the shift from the bar's centre to its frame's origin is the
buoyancy times some 50 mm, as large as the moment about the hinge. The fluid's frame is its first step at or past the frame's
time, so for a turning part the two runs agree to what the bar turns in
3 ms more than 0.1 deg (0.23 deg at 0.05 s, the bar falling at near 200
deg/s). The four cases take 100 s.

The skin is not the coupling's: S4's block let go alone, no mechanism,
is as deep (sec 12.7). Most likely it is how SPlisHSPlasH's boundary
meets the fluid at 5 mm; whether it shrinks with the spacing waits for
runs finer than 5 mm (sec 12.10).

**The command and the panel (2026-11-24).** `Fluid_Mechanism`, "Run with
a Mechanism": the simulation selected, or the one of the assembly
selected, or the document's only one, else picked from a list; one that
is not Dynamic is offered to be made so, since a Kinematic run moves the
parts by the joints' motions alone. The panel has a Mechanism row when
the document has an Assembly simulation. Both go through
`FluidPlayback.linkMechanism`, which also sets the simulation's end and
output step to the study's: the run ends with the earlier of the two, so
they are one. `Study::start` refuses, saying why, a mechanism whose
simulation is not Dynamic (it ran before, and moved nothing), a part the
fluid moves that is not the assembly's (the run did not start, and said
only that), and one grounded in it. The manual's page is
fluid/study/mechanism.

The study's links -- its walls, its mechanism, each part's, volume's and
face's -- are global now: an assembly is an App::Part, and a plain link
into one warned at every recompute that it left its scope. A file saved
with the plain links keeps them: `PropertyContainer`'s default
`handleChangedPropertyType`, which dropped any value whose type had
changed, now reads a link saved as another scope variant of its kind
(Child, Global, Hidden of PropertyLink, -List, -Sub, -SubList; not
PropertyXLink, which writes another format). Without it all seven of a
test file's links came back empty, without a word.

**The two results as one (2026-11-25).** The panel played the fluid's
frames and moved the parts the fluid pushes by them; a part of the
mechanism the fluid does not touch (an arm, a counterweight, a link
fixed to a float) stood still. With a mechanism, `FluidPlayback.
MovedParts` now loads the simulation's kept run (`loadSimulationResults`)
and puts the whole assembly where that run has it at each frame's time
(`updateForTime`), the Assembly's own parts' placements saved and put
back as its panel does -- when the run ends where the fluid's does, to a
frame; otherwise, or with no mechanism, the fluid's frames as before.
From the file too: the simulation keeps its run (KeepResults Motion, the
default). The Assembly's own panel still does not draw the water.

**The lag, at 5 mm (2026-11-25).** A partitioned coupling like this one
-- the force of a stretch from the fluid moved along the parts' motion at
its start, held through it -- is unstable in theory where the water that
moves with a part outweighs the part. Not seen at 5 mm, on 4 threads:
the hinged bar at 0.3, 0.2 and 0.15 of the water's density settles, the
lighter the sooner (swinging 0.55, 0.33 and 0.24 deg at the end, 0.5 to
0.7 deg low as the skin above says); a paddle 45 x 60 x 10 at 1.5 times
the water's density swinging broadside under water, some four times its
mass of water moving with it, creeps to hanging, overdamped. The water at
5 mm damps far more than the lag feeds. The growth seen at 2.5 mm is for
the attended run of sec 12.10 to settle: first whether it follows the
stretch (the runner's longest step divided, a diagnostic knob kept out of
the tree), then, if it does, the remedies by cost -- the walls on a
parabola over a stretch (the accelerations in the move line), an
added-mass correction, shorter stretches.

**The 2.5 mm runs, and a quarter of the step (2026-11-26).** The
manual's hinged bar (the pointer on it adds 0.08 deg: 16.52 by the closed
form) run 2 s at 2.5 mm, 55296 particles, on 4 threads, the last 30% of
its angle:

| The run | Longest step | Last 30% | Wall time |
|---|---|---|---|
| as it was | 1.6 ms | 14.37 to 21.93 deg, growing | 448 s |
| the pressure solver's cap 500, not 100 | 1.6 ms | 12.66 to 22.44 | 1004 s |
| the longest step halved | 0.8 ms | 16.75 to 17.21 | 616 s |
| quartered | 0.4 ms | 16.74 to 17.18 | 437 s |
| 5 mm, as it was | 2.3 ms | 17.30 to 17.68 | 25 s |
| 5 mm, quartered | 0.6 ms | 17.22 to 17.63 | 29 s |

The runner's trace had DFSPH at its default cap of 100 iterations on 93%
of the lines, the water not brought back to its density; let it go to
500 and it converged (median 175, at 500 on 0.1%) and swung no less --
not the cause. The longest step is the stretch the mechanism holds its
push over and the fluid's own longest step at once, and halving it was
enough: the growth is the lag, fed faster than the finer water damps.
Which of the two halves matters was separated after (the slice of the
test below, 2 s): the fluid's step a quarter and the stretch whole swung
12.45 to 18.48 deg, as wide as before -- it is the stretch, the lag, and
not the fluid's step. Shorter costs nothing
here: the pressure solver needs fewer iterations on a shorter step (at
its cap 93%, 43%, 0% of the lines), so a quarter ran as fast as the whole.

Built: a run with a mechanism moving a part takes a quarter of the
longest step (`SplishRunner.cpp`), which leaves the 5 mm results where
they were (17.38 deg for the half-density bar, 0.19 either way; 13.62,
0.40 at three tenths; 23.80, 0.36 at seven tenths). The test that would
have caught it, `test_a_bar_on_a_hinge_at_half_the_spacing`, runs the bar
at 2.5 mm in a slice of the tank 40 across (18432 particles, 2.5 min);
on the whole step it swung 3.68 deg about its mean, now 0.53 (0.25
either way from 1.8 s on). It rests at 15.35 deg against 14.92 by the
closed form: a skin of 0.22 mm, half the 5 mm runs' 0.42 -- the whole
tank's 16.95 against 16.45 is 0.24. So the skin shrinks with the
spacing, and the rest converges on the closed form: 1.0, then 0.43 deg
over. The scratch is `~/.cache/fluid/s4/` (narrow.py, run25*, the
analytic check `~/.cache/fluid/analytic/hinged_bar.py`).

### 12.10 The host's hard hangs, and what the runs are held to (2026-11-24)

The Windows host hung hard twice, 2026-11-22 about 18:32 and 2026-11-24
about 06:22, the first since July 2025: no crash code, no dump, nothing
in its logs of why. Ruled out: the RTX A2000's PCIe advisory error
(logged at every boot since April 2025), standby (the box has only S3,
sleep is off; the screen had been off), the network's drops, memory,
heat, the disks. Common to both: they came 1.5 to 4 minutes after a
rebuild of FluidSplish and its runs began -- SPlisHSPlasH built with
AVX (FMA throughout), on 8 threads, unattended. Not proved.

Ruled by the user, for now: the runner on 4 threads, 5 mm the finest.
`FLUID_SPLISH_THREADS` before the preference `Mod/Fluid/Threads` sets
the runner's thread count for every run started under it, the tests'
too; the jet test (`FluidTests/Jet.py`) is at 5 mm, its bounds from
measurements there -- the force 7.5 to 8% over, single steps 0.60 to
1.84 times the mean's, the plate's centre 0.39 to 0.40 of the stagnation
pressure. An attended run to look for the cause is to come.

2026-11-25 the user asked for 2.5 mm. Four such runs (7 to 17 min, 4
threads) were watched by a logger whose lines survive a hang -- Windows'
own counters from typeperf, which runs outside the VM; the VM's load,
memory and pressure stalls; its kernel messages; the runner's own trace
(`FLUID_RUNNER_LOG`): none hung. Package power 31 to 91 W, the thermal
zone 69 to 88, no throttle reason, the passive limit 100% throughout, no
memory or IO pressure. 8 threads is still to be tried, attended.

2026-11-26 a third, about 12:46, a few minutes into a 2.5 mm run on 4
threads: so the thread count does not prevent it, and what the three
have in common is the 2.5 mm run, not 8 threads. The same test then ran
8 times in a row without one, the package at a steady 65 W, the
processor at full performance, no throttling. Looked into and found
harmless: the WHEA 17 PCI Express events, one at every boot since 2025,
a configuration read of a function the NVIDIA card does not have. Not
known: the temperatures (no counter is readable without elevation; the
HP BIOS sensors need it, HWiNFO does not start on this box, the BIOS is
locked) and the power supply. The cause is not known.

Agreed with the user, 2026-11-26: 2.5 mm runs and the gate go on
unattended, with two logs kept for every fluid run and read first after
a stop -- a one-second log of Windows' power counters, each line synced
to both sides (`~/.cache/fluid/hang/powerlog.py`), and the runner's
trace named by `FLUID_RUNNER_LOG`. The trace is not in the tree; it is
kept as a patch beside the logger. A scheduled task on the host brings
the VM up at boot without a login.

### 12.11 A run's account of its energy (2026-11-26)

The 2.5 mm bar that swung ever wider ended its run well, and its frames
looked like water. Nothing said not to believe it. Now the run keeps an
account of its energy, the results hold it, and the panel says when it
does not hold.

**What is kept.** With each frame the runner says (`energy`, J) the
fluid's kinetic energy; its potential energy, heights taken from under
the space so that a particle that leaves takes energy with it; the work
the fluid has done so far on the walls it moves, each step's force and
torque along the wall's motion; and of a mechanism's parts two sums made
at each hand-over. A part goes through a stretch by the fluid's mean push
of the stretch before, and the fluid meanwhile gives another: the
difference, along the way the part went, is work the one side did and
the other never felt. *Created* is that with its sign, *exchanged* the
stretch's work without one. The results keep it as `energy.npy`, and
`getResults()["Energy"]` gives the columns and what is made of them
(`Fluid::judge`).

**What is made of it**, past the first quarter of the run's time:

- *The fluid's budget.* Kinetic and potential energy and the work
  together cannot rise in a run nothing feeds; the solver only loses.
  The most it rose above its lowest before, as a share of the fluid's
  greatest kinetic energy: over 0.1, the run was coming apart. Not judged
  of a run with inlets, which bring energy the account does not have
  (the jet that stops would be over the limit, at 0.12, with nothing wrong).
- *The hand-over.* What was created as a share of what was exchanged,
  when more went over than a hundredth of the fluid's greatest kinetic
  energy: over 0.2, the lag is feeding the motion. Energy the lag takes
  out is damping, and nothing to warn of.

The first quarter is left out because a run's start is not its fault:
fluid seeded against a part is pressed apart in the first steps (the bar
put where it rests: the budget up by as much as the fluid's kinetic
energy ever was, in the first 50 ms, and never again), and a part's
first stretch goes by a push of none.

**Measured** (4 threads; the share created past the first quarter):

| The run | Created of exchanged | The budget rose |
|---|---|---|
| the bar at 2.5 mm on the whole step (swinging 12.5 to 18.5 deg) | 0.30, and growing to the end | never |
| the bar at 5 mm on the whole step (15.3 to 16.6) | 0.08 | never |
| the bar at 2.5 mm on a quarter | 0.005 | never |
| the bar at 5 mm on a quarter; lighter; heavier; put where it rests | 0.003 to 0.006 | never |
| a float let go above the water, on a slider | 0.014 | never |
| a float a tenth of the water's density on a slider | 0.10 | never |
| the dam break, the plates, still water, the floating blocks | no mechanism | never |

The fluid's own budget never rose in the run that swung wider: the lag
is no part of the fluid, and the budget cannot see it. Only the
hand-over does. The light float at 0.10 is the nearest to the limit of
the runs that settle, as the theory has it (the water that moves with it
outweighs it).

**What it does not say.** A run on particles too coarse for the question
keeps its account in order: the 5 mm bar rests a degree off the closed
form with nothing found. And there is no remedy in it: the step is not
made shorter by what it finds, which the user ruled out for now (the
energy budget alone, of the guards offered).

The test: `test_the_lag_is_seen_in_the_runs_energy` runs the slice at
2.5 mm on the whole step again (`FLUID_SPLISH_STRETCH=4`, the runner's
switch for it) and has the hand-over over its limit and the budget not;
every other run of the Splish tests is held to both (`energyHolds`).

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

## 14. The steady-flow track

Secs 5 and 6 as written on 2026-11-03, built after the free-surface
track on what that track left: the module, the neutral model, the
backend interface, the OpenFOAM launcher and its download, kept results.

### 14.1 F0 measured (2026-11-23)

Most of F0 was settled by S0 and S2 (secs 12.1, 12.4). What was left,
measured on this box with the install S2 uses (scratch
`~/.cache/fluid/f0`, `run.sh` and `read.py`; other sessions running, so
no benchmark):

| Case | Cells | Solver | Ran to | Wall time | Memory | On disk |
|---|---|---|---|---|---|---|
| Lid-driven cavity (tutorial) | 400 | icoFoam, 1 core | 0.5 s, 100 steps | 0.08 s | 47 MB | 0.5 MB |
| pitzDaily (tutorial, k-epsilon) | 12225 | simpleFoam, 1 core | converged, 281 iterations | 3.7 s | 132 MB | 7.5 MB |
| the same | 12225 | simpleFoam, 4 processes (scotch) | converged, 288 iterations | 2.2 s | 153 MB | 13 MB |

- **Which OpenFOAM** (sec 9's first question): ESI's, v2412, the
  conda-forge package -- what S2 runs and what the workbench's download
  installs. Its solvers keep their names (`simpleFoam`, `pimpleFoam`,
  `buoyantSimpleFoam`, all present), so the case writers of F1 to F4 are
  written for those; the Foundation's `foamRun` modules are not a target.
- **Parallel** works after sec 12.1's link, serial and 4 processes giving
  the same flow (the pressure's range on the lower wall -8.21 to 12.16
  against -8.14 to 12.27, each stopped by its own residuals). A case this
  small gains little; the study decides the process count from the cell
  count, as S2 does.
- **The launcher**: the one S2 built (the install's `etc/bashrc` in a
  shell, the steps as processes). Linux only still; Windows through WSL
  and macOS through a container remain as sec 12.4 left them, since
  neither can be tried from this box's Linux side.
- **VTK's reader in FreeCAD's process**: `vtkOpenFOAMReader` (VTK 9.6.2,
  through the Python modules FreeCADCmd already loads) read all three
  cases as they stand, the reconstructed parallel one too: the internal
  mesh and every patch as blocks, each with its fields (`U`, `p`, and
  `k`, `epsilon`, `nut`), 0.01 s for the cavity and 0.06 s for pitzDaily.
- **How F1 reads results, decided here**: not through that reader.
  `Mod/Fluid`'s C++ links no VTK, and S2 already has OpenFOAM write what
  the view draws (the free surface, by a `surfaces` function object) as
  legacy VTK files our own small parser reads. F1 asks the same function
  object for each patch's fields and for the cut planes, and reads them
  the same way: no new dependency, nothing of the volume read into our
  process, and the files are the "faces and slices" sec 9 wants kept.
  The reader stays the way to look at a whole volume later (streamlines,
  iso-surfaces), from Python, where it is known to work.

### 14.2 The downloaded OpenFOAM computes viscous flow wrongly (2026-11-23)

Found while trying F1's first validation outside FreeCAD (a laminar pipe,
scratch `~/.cache/fluid/f1/proto/pipe.py`): **conda-forge's
`openfoam-2412-h946d6a9_1`, the package S2 runs on and the workbench's
download installs, has a miscompiled viscous stress term.** Every solver
that takes its stress from the turbulence library is affected --
`simpleFoam`, `pimpleFoam`, and `interFoam` with them; `icoFoam`, which
writes its own Laplacian, is not.

Measured, the same fluid (1000 kg/m^3, 0.1 Pa s) at 0.1 m/s, Re 10, the
pressure drop over 50 mm of developed flow:

| Case | Solver | Result | Closed form | Off by |
|---|---|---|---|---|
| Pipe, 10 mm, along x, snappyHexMesh, 20 cells across | simpleFoam | 200.0 Pa | 160.0 | +25.0% |
| the same with 3 wall layers | simpleFoam | 200.2 | 160.0 | +25.1% |
| Square duct, 10 mm, along x, blockMesh 20 x 20 | simpleFoam | 175.1 | 142.3 | +23.1% |
| the same | pimpleFoam to a steady state | 175.1 | 142.3 | +23.1% |
| the same | icoFoam to a steady state | 140.9 | 142.3 | -1.0% |
| the same duct turned along y, and along z | simpleFoam | 140.9 | 142.3 | -1.0% |
| the duct along x, **the stress term rebuilt and preloaded** | simpleFoam | 140.9 | 142.3 | -1.0% |
| the pipe along x, 20 across, no layers, term rebuilt | simpleFoam | 160.3 | 160.0 | +0.2% |
| the pipe, 10 across, no layers, term rebuilt | simpleFoam | 161.8 | 160.0 | +1.1% |

What is known:

- The excess does not move with the mesh, the mesher, wall layers, the
  solver's settings, the convection scheme, the speed or the viscosity
  (it is linear in both): it is not an error of resolution.
- It depends on the direction of the flow in the model's axes: wrong
  along x, right along y and z. The velocity carries a cell-to-cell
  checkerboard along x, and the duct's profile differs between its two
  cross directions in a problem that is symmetric.
- Asked directly (`repro/h2.C`, a program calling the shipped library's
  `divDevReff` on a case's velocity): the explicit part of the viscous
  term, `- div(nuEff dev2(T(grad U)))`, has a wrong x component in 42000
  of the duct's 80000 cells -- every even cell and 2000 odd ones -- and
  right y and z components everywhere.
- **The cause** (chased 2026-11-23 on the user's word, `repro/g.C` and
  `k.C`): OpenFOAM's own source, brought out by conda-forge's flags.
  `T()` of a temporary tensor field reuses the temporary's storage
  (`UNARY_FUNCTION(tensor, tensor, T, transform)` in
  `GeometricTensorField.C`, through `reuseTmpGeometricField`) and
  transposes it in place, the result and the argument one field. Every
  list's data pointer is declared `__restrict__` (`UList<T>::v_`, in
  `UList.H`), and the loops' own pointers again (`List_ACCESS`,
  `List_CONST_ACCESS` in `ListLoopM.H`): a promise that what one points
  at is reached through no other, which here it is. gcc may then do
  anything, and its SLP vectoriser does: in every other cell the
  transposed zx keeps its old value in place of xz's.
  The stress term is `dev2(T(fvc::grad(U)))`, a temporary.
- It shows with `-O2 -ftree-vectorize`, which is what the package was
  built with: the recipe appends conda's flags after OpenFOAM's own
  `-O3`, and the last `-O` wins. With those two flags it is wrong by
  conda-forge's gcc 13.3.0-2 and by Ubuntu's 13.3.0 alike (40000 of
  80000 cells for `T()` alone, 42000 for the whole term -- the shipped
  library's count exactly). It is right with `-O2` alone, with
  OpenFOAM's `-O3` alone, with `-fno-tree-slp-vectorize`, and with
  `__restrict__` taken off the list's pointer -- off the loops' alone it
  stays wrong. So a build by OpenFOAM's own rules hides
  it, and the first note of this section had the rebuild right for the
  wrong reason: it put `-O3` last.
- Every in-place operation on a reused temporary whose result's
  components depend on other components of the input stands on the same
  promise -- `dev`, `dev2`, `symm`, `inv` and the rest -- whether or not
  this compiler happens to break them. The mend that covers them all is
  to take `__restrict__` off the list's pointer and the loops';
  `-fno-tree-slp-vectorize` only stops this compiler finding it.
- ESI has not mended it: `UList.H`, `ListLoopM.H`, `tensorField.C` and
  `GeometricTensorField.C` are the same in v2506 and v2512 (their
  tarballs, from dl.openfoam.com).
- **Our package** (ruled 2026-11-23): `Thundereal/openfoam-feedstock`,
  conda-forge's recipe for v2412 with that patch
  (`0001-no-restrict-on-list-data.patch`), the parallel link of sec 12.1
  made in the build, the libraries pinned to those conda-forge's own
  build was made against, ESI's tarball as the source (the archive
  address of conda-forge's recipe now asks for a login), and
  `test/viscous.sh` as the recipe's test: the same duct along x, y and
  z, serial and under MPI, the three within 1% -- 3 s, and on
  conda-forge's package it says 23.7%.
- conda-forge has only this version, in two builds of the same day.

What it means for what is built:

- S2's dam break ran with the faulty term. Water's viscosity is a
  hundred thousandth of the fluid's here and the flow is driven by its
  weight, so the term should matter little there, and the validation
  against Lobovsky and Kleefsman stands as measured -- but that is
  reasoning: it was not run again with the term rebuilt.
- F1 cannot be validated, or trusted, on this install as it comes: a
  pipe along x is a quarter out. Wherever viscosity matters the fault
  does.
- With the term right, snappyHexMesh without layers meets F1's 1% on the
  pipe at 20 cells across the diameter.

Ruled 2026-11-23: a package of our own (above), after the cause was
chased; and the backend tests an install for the fault as it tests for
a parallel run, and refuses one that has it, saying why. Reporting it
upstream was not ruled on.

**The check, as built.** `OpenFoam::ductCase(axis)` is the recipe's test
as a case of ours: a square duct 10 mm across and 100 long, 50 x 10 x 10
cells, laminar, `simpleFoam`, the pressure probed at two points of its
axis. `OpenFoamBackend::viscousFault()` runs it along x and along y on
every OpenFOAM the search finds (about 2 s), and one whose two losses
differ by more than a hundredth is not used: the search goes on to the
next place, and with no other the backend is not available and its
reason says which install was refused and why. What could not be run is
not refused. The verdict is kept in the user's cache folder
(`Fluid/openfoam-viscous.txt`) by the stress library's path, size and
date, so an install is put to it once. `FluidApp.checkOpenFoam(folder)`
asks it of a folder. Download OpenFOAM now takes `openfoam=2412=*_100`
from our channel (what it runs on from conda-forge), which is our build
and no other.

**Our package, built and tried (2026-11-23).** The feedstock's first CI
run took 3 h 40 on GitHub's runner and passed the recipe's tests, the
ducts along x, y and z losing 0.137072 each, serial and under MPI;
`openfoam-2412-h7ecdde7_100` is on the realthunder channel. Installed
here as the download does it (`~/.cache/fluid/env-openfoam2`): the duct
test the same, the backend's check passes, the pipe of the table above
at +0.21% of Hagen-Poiseuille at 20 cells across and +1.14% at 10, and
the workbench's tests that run OpenFOAM pass on it as they did.

One thing the trial found: where the machine's own conda settings say
strict channel priority, mamba and micromamba refuse a build that is in
one channel when the package's name is in two, whichever channel is
named first (mamba 2.5.0; conda itself does not). The download's command
says `--channel-priority flexible` to those two.

### 14.3 F1 as built, step 1: the fluid's space (2026-11-23)

What a steady flow is to the model, on the App side; no backend runs one
yet (each says so: "cannot run a steady flow"), and there is no command
for it. Built on S5's inlet rather than on sec 5.2's `Domain` and
`Boundary` objects: the study already has walls and an inlet that is a
face of one of them, and a lid is a wall like any other.

- **The study**: `Analysis`, "Free surface" (as every study was) or
  "Steady flow", and `Iterations` (2000), the steps a steady flow is
  given to settle in.
- **`Fluid::Outlet`**, a child of the study: `Face` (one flat face of a
  part) and `Pressure` (over the surroundings', 0). The inlet is S5's;
  its times mean nothing to a steady flow. Fluid at the start and parts
  the fluid moves are errors in a steady study, an outlet in a free
  surface's.
- **The space the fluid fills is found, not drawn** (sec 5.2): a box
  around the walls, the walls cut out of it, and of the solids left the
  ones that do not reach the box are the closed spaces inside. The one
  the inlets' and outlets' faces lie on is the fluid's. The openings are
  closed by parts the user makes -- lids, walls of the study -- and an
  inlet or an outlet is the lid's face on the fluid's side: the fluid
  goes out of the solid the face is of, as in S5. An opening without a
  lid leaves no closed space, and the error says so; faces on two spaces
  are an error too.
- **The model** (`Model.h`): `analysis`, `outlets`, `iterations`, and
  `domain`, the space's closed triangulation with a patch per triangle
  -- 0 a wall, then each inlet, then each outlet -- by which of the
  crossed faces a point of the space's face lies on. The model's space
  is the fluid's box; the resolution left to the study is a twentieth of
  its smallest side. `volumeOf()` and `patchAreas()` for whoever checks
  it. The signature takes all of it in a steady model only, so kept
  results of the studies before stay theirs.
- **Round faces are cut finer here than a wall's**: flat pieces a
  hundredth of the resolution off the face (a wall's are a quarter),
  since a mesh is drawn onto these triangles and a pipe's pressure drop
  goes with the fourth power of the radius they make. The space is
  meshed whole and each face's triangles taken from it, so neighbours
  share their edges' points.

Tests: `TestSteady` (a tube with two lids: the bore's volume and the
three patches' areas to 0.3%, the resolution, the errors, kept with the
document), `FluidModel.aSteadyFlowsParts`.

Open: the lids the study could find itself (sec 5.2); an inlet by flow
rate or pressure; the rest of F1 -- the case for `simpleFoam`, the run,
the result on faces -- which waited on sec 14.2 and is sec 14.4.

### 14.4 F1 as built, step 2: the flow run, and its pressure on the faces (2026-11-27)

A steady flow runs on OpenFOAM, from the workbench, and its results are
kept and drawn. Laminar only.

- **The case** (`OpenFoamCase.cpp`, `steadyCase`): `simpleFoam`. The
  model's domain goes out as a surface per patch -- `wall`, `inlet<i>`,
  `outlet<j>` -- and `snappyHexMesh` meshes the inside of them from a
  background box a cell and a half wider than the fluid's, cells of the
  model's resolution, no layers. An inlet is a fixed velocity, an outlet
  a fixed pressure (over the density, as the solver has it), a wall no
  slip. Schemes and solvers are the ones the pipe of sec 14.2 was
  measured with; the run stops when the residuals are under 1e-7 (p) and
  1e-8 (U), or at the study's `Iterations`. Above one process it is
  decomposed and run under MPI as a free surface's is.
- **What is written back**: the pressure on every patch (the `surfaces`
  function object, VTK) each fortieth of the steps and when it stops;
  each opening's mean pressure and what goes through it
  (`surfaceFieldValue`), and the pressure monitors (`probes`), each step.
- **Frames of fields** (`ResultKind::Fields`, till now a name only): a
  frame is the faces the fluid touches as triangles with the pressure at
  each point (`Frame::values`, Pa) and each opening's mean pressure and
  flow (`Frame::openings`, the inlets then the outlets; m^3/s, in through
  an inlet, out through an outlet). A frame's time is the step. The first
  frame is the model's domain with no pressure on it. The archive keeps
  `frames/N.values.npy` and `openings.npy`; its index says `steady`,
  `settled` (it stopped short of its steps) and the openings.
  `getResults()` has `Steady`, `Settled` and `Openings`; `getFrame()` has
  `Values`.
- **Who runs it**: OpenFOAM says `steady`. A study with no solver named
  takes the first that runs its kind of flow (`Registry::select`), so a
  steady study is not handed to the particle solver registered before
  it. A height monitor in a steady study is refused by the case, a force
  monitor too (not built).
- **The workbench**: `Fluid_AddOutlet` (the faces picked; it makes the
  study one of a steady flow); the panel has "Study of" (free surface or
  steady flow) and, for a steady flow, "Steps at most" in place of the
  times. A field is drawn on its faces in the colours of the pressure
  (`FrameView.showField`, a Turbo ramp), the scale under the slider from
  the last frame's least to its most, the walls hidden while it is
  shown -- it is drawn on their faces from the inside. The table lists
  each opening's mean pressure and flow under the monitors. A flow that
  ran out of steps is said so in orange, as the energy's doubts are.

Measured, the tube of `TestSteady` (10 mm inside, 100 long), 0.1 m/s in,
1000 kg/m^3 and 0.1 Pa s (Reynolds number 10), the pressure lost between
two monitors on the axis at 40 and 90 mm against Hagen-Poiseuille's at
the flow there was:

| Resolution | Cells across | Lost, Pa | Closed form | Off | Flow off the speed x face | Steps | Time |
|---|---|---|---|---|---|---|---|
| 1 mm | 10 | 160.879 | 160.840 | +0.02% | +0.53% | 109 | 1.6 s |
| 0.5 mm (the study's own) | 20 | 160.320 | 160.127 | +0.12% | +0.08% | 103 | 7.6 s |
| 0.25 mm | 40 | 160.196 | 160.013 | +0.11% | +0.01% | 2000, not settled | 435 s |

Sec 6 asks 1%. The flow is off the speed times the face because the
mesh's inlet is of flat cells' faces snapped to the round. At 0.25 mm the
residuals never got under the limits and the run went its 2000 steps; the
answer had long stopped moving, and the panel says the flow had not
settled. A looser stop for a fine mesh is open.

A block with a bore 10 mm across that parts into branches 8 and 5 mm
across, each 28 long (the manual's): of 7.86 ml/s in, 85.7% leaves by the
wide one and 14.3% by the narrow, 269 Pa lost, settled in 113 steps, 7 s.
Two pipes of one length under one pressure would share as their
diameters' fourth powers, 86.8% to 13.2%; the branches here do not start
from one pressure -- the narrow one is further along the bore.

Tests: `OpenFoamCase.filesOfASteadyFlow`, `whatCannotBeASteadyFlow`,
`aFacesPressureReadBack`; `FluidArchive.aFieldOnFacesAndTheOpenings`;
`FluidRegistry` (the unnamed choice); `FluidTests/Pipe.py` run as
`TestOpenFoamPipe` (the pipe to 1%, what comes in goes out, the faces'
pressure against the monitors, kept with the document, the unnamed
solver); `TestColors`, `TestSteadyDoubts`,
`TestFrameView.test_a_field_is_coloured_by_its_values`.

Open, the rest of F1: turbulence (k-omega SST) and its pipe against
Haaland (built since, sec 14.6); the velocity (built since, sec 14.5); the lid-driven cavity and the
backward-facing step; meshing by the second route (sec 5.4); residuals
shown as the run goes; an inlet by flow rate or pressure; the force on a
part; lids the study finds itself; a warning when the Reynolds number
says the flow is not laminar (built since, sec 14.6).

### 14.5 F1 as built, step 3: the flow inside, and a cut through it (2026-11-27)

The velocity is kept and shown: the fluid inside at every cell of the
solver's, and a flat cut through it drawn in the colours of its speed or
its pressure, with arrows.

- **What is written back**: a `sets` function object of type
  `cellCentre`, once, when the solver ends (`writeControl onEnd`): a row a
  cell, its centre, p and U, each cell's own value. It is one file under
  MPI too. `OpenFoam::readInside` reads it; the backend hands it over
  with the run's last frame only (`Frame::centres`, `velocities`,
  `pressures`). Of every frame it would be 16 bytes a cell a frame, 320
  MB for a tube at 40 cells across that runs its 2000 steps; and a
  steady flow's frames on the way are no flow that ever was.
- **Kept** as `frames/N.inside.npy`, (cells, 7) float32: the centre in
  mm, the velocity in m/s, the pressure in Pa. The index lists the frames
  that have it (`inside`). `getResults()["Inside"]`, `getInside(i)`.
- **The cut** (`App/Section.cpp`, `Fluid::cut`; `getSection(i, base,
  normal, step)`): where the plane passes through the frame's own faces
  is the cut's outline, a piece a triangle; a grid on the plane, half a
  cell apart, is inside the fluid between an odd crossing of the outline
  along its row and the next. The value at a grid point is of the cells
  whose centres are within a cell of it, the nearer the more (Shepard's
  weights, ((R - d) / (R d))^2); with none so near, the nearest about.
  The squares with four corners inside are the cut's triangles. It works
  on nothing of OpenFOAM's: the centres, and faces that close the fluid
  in.
- **Drawn** (`FluidPlayback.CutView`): the triangles unlit in the ramp's
  colours, the outline, and arrows (`arrowsOf`) at every few grid points,
  the fastest as long as the arrows are apart, some thirty along the
  cut's longer side. The faces on the viewer's side of the plane are left
  out (`behind`, whole triangles, in numpy: no clip plane, which not every
  renderer has) and the rest drawn plain. The panel has "Cut through the
  fluid": across X, Y or Z, how far along the fluid's box, speed or
  pressure, arrows or not; the scale is of all the fluid inside, so it
  stays as the cut moves. At a frame without the inside it says so and
  draws the faces' pressure as before.

Measured, the tube of sec 14.4 at the study's own 0.5 mm (61000 cells,
104 steps): between 40 and 90 mm along, the velocity along the pipe at
every cell against the parabola 2 U (1 - r^2 / R^2), U the mean speed of
the flow there was: furthest off by 0.46% of the speed on the axis, 0.19%
in the mean; on the axis 0.19993 m/s against 0.20016. A cut across at 60
mm, 1245 points: within 4 mm of the axis furthest off by 5.3%, 2.2% in
the mean. That is the blending between cells, not the solver: the cells
have it to half a percent. A value fitted to the cells about a point,
not blended from them, would do better and is open.

Tests: `OpenFoamCase.theFluidInsideReadBack` and the `inside` lines in
`filesOfASteadyFlow`; `FluidArchive.aFieldOnFacesAndTheOpenings` (the
inside of the last frame only); `FluidSection.aCutAcrossABox`,
`aSlantedCutAndOnesThatFindNothing`;
`Pipe.test_the_flow_inside_is_poiseuilles` (the cells to 3%, the cut to
6%, its area, its outline) and the inside kept with the document;
`TestCuts`, `TestFrameView.test_a_cut_is_drawn_with_its_outline_and_arrows`.
The panel's controls have no test of their own: the manual's script
drives them (`fluid/study/flow-inside/video.py`).

Open: the cut's edge is a stair of half cells inside the outline; a
fitted value at a point (built since, sec 14.7); lines of the flow
(tracers, sec 14.7); the speed at an
opening's face; a cut of any slant from the panel (the script takes any
plane); the inside of the frames on the way.

### 14.6 F1 as built, step 4: turbulent flow, and what says whether to believe it (2026-11-27)

A steady flow is run in layers, as before, or turbulent; and the results
say when the choice, or the mesh at the walls, is against them.

- **The choice**: the study's `Turbulence`, `Laminar` (as files made
  before have it) or `k-omega SST`; the panel's "Flow" for a steady
  study. The model's `turbulence`; a laminar model's signature is what it
  was.
- **The case**: `simulationType RAS`, `kOmegaSST`; `k` and `omega`
  carried upwind; the walls by wall functions (`kqRWallFunction`,
  `omegaWallFunction`, `nutkWallFunction`) -- the mesh has no layers at
  its walls, so the wall's own layer is not resolved and cannot be. What
  comes in at an inlet is taken as stirred a twentieth of its speed, in
  eddies 7% of the inlet's width across; the fluid starts as the first
  inlet's, and what flows back in at an outlet is that too. It stops
  when the residuals are under 1e-5 (p) and 1e-6 (U, k, omega): the
  pipes below had not moved by a hundredth of a percent in their last
  hundred steps.
- **An inlet's width and Reynolds number** (`Solver::widthOf`,
  `reynolds`): the width across a round face of the inlet's area, the
  area of a steady flow that of the inlet's part of the fluid's space --
  the face picked is often wider than the opening (a lid's whole face
  over a bore). `buildModel()` has each inlet's `Reynolds`; the results
  keep the greatest (`Reynolds`).
- **The wall's units**: the `yPlus` function object says, at the end, how
  far the first cell at the wall is from it in the wall's own units, the
  least, the most and the mean. Kept with the results (`WallUnits`).
- **What the panel says** (`FluidPlayback.flowDoubts`, with the other
  doubts in orange): a laminar study whose Reynolds number is over 2300;
  a turbulent one under it; a turbulent one whose wall cells are under 30
  wall units in the mean, or over 1000, with the resolution that would
  put it right.

Measured, the pipe of sec 14.4 made 300 mm long, water (1000 kg/m^3,
0.001 Pa s), the pressure lost between monitors on the axis at 150 and
270 mm as a friction factor at the flow there was, against Haaland's for
a smooth pipe, 1 / sqrt(f) = -1.8 log10(6.9 / Re):

| Reynolds | Resolution | f | Haaland | Off | Wall units, least..most, mean | Steps | Time | The panel |
|---|---|---|---|---|---|---|---|---|
| 10000 | 1 mm | 0.02726 | 0.03084 | -11.6% | 6..29, 20 | 189 | 12 s | too fine |
| 10000 | 0.5 mm | 0.02823 | 0.03088 | -8.6% | 4..16, 11 | 179 | 40 s | too fine |
| 30000 | 1 mm | 0.02269 | 0.02329 | -2.6% | 11..81, 56 | 192 | 13 s | - |
| 30000 | 0.5 mm | 0.02106 | 0.02331 | -9.7% | 6..40, 28 | 185 | 40 s | too fine |
| 100000 | 1 mm | 0.01809 | 0.01781 | +1.6% | 28..238, 166 | 195 | 11 s | - |
| 100000 | 0.5 mm | 0.01716 | 0.01782 | -3.7% | 12..119, 85 | 188 | 36 s | - |
| 1000000 | 1 mm | 0.01224 | 0.01158 | +5.8% | 272..1915, 1370 | 198 | 11 s | too coarse |

Sec 6 asks 5%. It is met where the wall cells are in the wall model's
range, and missed by up to 12% where they are not -- and every case that
misses is one the panel speaks against. A finer mesh is not a better one
here: at 30000 half the cell takes the answer from 2.6% off to 9.7%.
The wall units spread wide about their mean because the cells at a round
wall are of a square mesh snapped to it. The pipe at 10000 has no good
resolution with this mesh: coarse enough for the wall model is 3 cells
across the bore. Layers of cells at the walls, their thickness chosen
for the flow, are what mend it, and are open (tried since, sec 14.8:
not so simply, and part of this row's error is the pipe's length). Outside FreeCAD the same
cases with `nutUSpaldingWallFunction` were no better (+10.5% at 10000,
+4.8% at 100000 on 1 mm).

Tests: `OpenFoamCase.filesOfATurbulentFlow`, `theWallUnitsReadBack`;
`FluidModel.inletsAndForces` (the width, the Reynolds number, of a
steady flow by the fluid's space), `aSteadyFlowsParts` (the signature);
`FluidArchive.aFieldOnFacesAndTheOpenings` (the model's name, the
Reynolds number and the wall units kept); `TestFlowDoubts`;
`Pipe.test_a_turbulent_pipe_loses_what_haaland_says` (Reynolds 30000 on
1 mm, to 5%, nothing against it). The panel's "Flow" has no test of its
own: the manual's script drives it (`fluid/study/turbulent-flow/video.py`).

Corrected the same day: the two sentences on the Reynolds number said
which way the pressure was off (a laminar run over 2300 "loses more
pressure than is shown"). That is a pipe's case, where the loss is the
wall's friction. A block with a bore that parts in two, water at 6 m/s,
showed 50 kPa lost run in layers and 38 kPa run turbulent, neither
settled. The sentences now say the pressures are not the flow's own, and
no direction.

Open: layers of cells at the walls; a rough wall; the eddies that come
in at an inlet as the user gives them; other models; the Reynolds number
of a flow that is not a pipe's -- 2300 is a pipe's, and an opening's
width is not always what the flow is as wide as.

### 14.7 F1 as built, step 5: tracers in the flow, and a truer value at a point (2026-11-27)

On the branch `FluidDynamics`. A steady flow is one state and nothing in
it moves; tracers are what is put in it to be seen moving.

- **`Fluid::trace`** (`App/Tracers.cpp`): a tracer from each seed,
  followed through the frame's fluid inside by the flow half a step on
  (the midpoint rule), a point each interval -- even times, so that
  playing them is a point a tick and the fast parts of the flow are seen
  fast. With no interval given, one that takes the fastest of the fluid
  half a cell a point. A path ends where no cell is near (the tracer has
  left by an outlet) or the fluid stands still, or at the most points
  asked.
- **A value at a point** (`App/InsideCells.h`, shared with the cut): a
  plane fitted by least squares through the cells within a cell and a
  half, the nearer the more, held within what those cells have; with too
  few for a fit, their mean by nearness as before. Sec 14.5's "a fitted
  value at a point" is this.
- **Python**: `study.getPaths(i, seeds, interval, most)`;
  `FluidPlayback.Frames.paths` -> `Paths` (`at(tick, every)`: the
  tracers there are at a tick, one let go at each seed each so many
  ticks; `lines()`); `seedsAt` (the first cells inside an inlet's plane,
  thinned); `facingAway` (the faces on the other side of the fluid);
  `TracerView` (points in the colours of the speed, the paths as faint
  lines).
- **The panel**: "Tracers in the flow", for results that keep the fluid
  inside: 80 seeds over the inlets, one tracer let go at each every 40
  ticks, 25 ticks a second; the faces towards the viewer left out, the
  scale the speed's. With a cut, the cut under them.

Measured, the pipe of sec 14.5 (0.5 mm, 61000 cells):

| | Before the fit | With it |
|---|---|---|
| A cut across at 60 mm, within 4 mm of the axis, against the parabola: furthest, mean | 5.3%, 2.2% | 0.68%, 0.51% |
| The same within 4.75 mm | - | 0.71%, 0.52% |

A tracer let go half a mm off the axis reaches the outlet, 100 mm on, in
0.518 s; at twice the mean speed all the way it would be 0.500 s, and the
flow takes some 6 mm to get its shape. The block of sec 14.4: of 80
tracers 66 leave by the wide branch and 11 by the narrow (82.5% and
13.8%; the flow parts 85.7% and 14.3%), 3 are still inside at 6000
points, in the slow fluid at the walls.

Tests: `FluidTracers.carriedAlongAStraightFlow`,
`roundATurnAndBackToTheStart` (a turn of 200 points keeps its radius to
2%), `whatIsNotFollowed`; `TestTracers`,
`TestFrameView.test_tracers_are_drawn_with_their_paths`; in
`Pipe.test_the_flow_inside_is_poiseuilles` the tracers from the inlet (to
the outlet, in time, none back or through the wall). The panel's box has
no test of its own: the manual's script drives it.

Open: the tracers are let go at the inlets only; a place picked in the
view; a backward-facing step, in layers and turbulent, as the case to
show them on and to check against Armaly et al.

### 14.8 Layers of cells at the walls: tried, not built (2026-11-27)

Sec 14.6 named layers as what would mend a turbulent flow whose wall
cells are out of the wall model's range. Tried outside FreeCAD on the
pipe, `snappyHexMesh`'s `addLayers`, 30 runs; the friction factor
against Haaland's, the wall units in the mean:

| Reynolds | Cells | Layers (count, growth, last as a share of the cell) | Wall units | Off |
|---|---|---|---|---|
| 10000 | 1 mm | 1, -, 0.3 | 9 | -1.0% (short pipe) |
| 10000 | 0.5 mm | 1, -, 0.3 | 5 | +6.1% (short pipe) |
| 30000 | 0.5 mm | 1, -, 0.5 | 18 | -16.0% |
| 10000 | 0.5 mm | 8, 1.3, 0.6 | 1 | -10.6% (short pipe) |
| 10000 | 1 mm, 800 long | 8, 1.3, 0.6 | 1 | -1.4% |
| 10000 | 1 mm, 800 long | 10, 1.4, 0.5 | 0.4 | +3.1% |
| 10000 | 1 mm, 800 long | 14, 1.25, 0.5 | 0.4 | +4.3% |
| 30000 | 0.5 mm | 8, 1.3, 0.6 | 4 | -1.3% |
| 30000 | 0.5 mm | 10, 1.4, 0.5 | 1 | -9.3% |
| 30000 | 1 mm | 5 or 8, 1.2 or 1.3, 0.4 or 0.6 (8 runs) | 3..12 | -8.4% to +13.1% |
| 100000 | 1 mm | the same 8 | 9..35 | -4.4% to +10.0% |
| 1000000 | 1 mm | 10, 1.4, 0.5 | 28 | +6.6%, not settled in 3000 steps |

What they say:

- **The pipe at 10000 was too short.** Its flow has not its shape 15 to
  27 widths in: with no layers the 800 mm pipe, measured 40 to 72 widths
  in, is 6.7% under where the 300 mm one is 11.6% under. Some 5% of sec
  14.6's worst row was the test's pipe, not the solver.
- **Between 5 and 30 wall units is worse than either side**, whichever
  wall function: 5% to 16% off. A thin layer or two puts the first cell
  there. Under some 2, or over 30, it is within some 4%.
- **No setting held across the cases.** Two reasonable ones at 30000
  differ by 8 points. On 1 mm cells the layers went on over half the
  wall only, whatever was asked, and 18 thin ones not at all. Eight
  layers cost 5 to 8 times the run.

So an automatic mode cannot be built on this yet, and it is not built.
The warning of sec 14.6 stands: it speaks against the cases that are
off. What would be worth trying next is a mesh made for its walls
(sec 5.4's second route) rather than layers pushed into a snapped one.

### 14.9 A step down in a channel's floor, against Armaly (2026-11-27)

A second measured flow beside the pipe, and the first with fluid that
turns back. Armaly, Durst, Pereira and Schoenung (J. Fluid Mech. 127,
1983) measured a channel 5.2 mm high that a step of 4.9 mm in its floor
makes 10.1: behind the step the fluid at the floor runs backwards, and
how far along it turns forwards again grows with the Reynolds number
while the flow is in layers. Their Reynolds number is on the mean speed
in and twice the height before the step. Nothing was built for it: it
is the steady flow of secs 14.4 to 14.7 on another shape, and a test.

The channel in a block, 100 mm of it before the step and 150 after,
water, 0.5 mm cells. Where the flow at the floor turns forwards, in step
heights, in the channel's middle:

| Reynolds | Channel 20 wide | Channel 40 wide | Time, 20 and 40 wide |
|---|---|---|---|
| 100 | 2.78 | 2.98 | 31 s, 589 s |
| 200 | 4.74 (4.79 with 100 mm before the step where that had 60) | 5.06 | 33 s, 136 s |
| 400 | - | 8.66 | -, 179 s |
| 800 | 14.37 | 14.38 | 81 s, 727 s |
| 8000, k-omega SST | 5.41 to 5.54 | 6.98 | 395 s, 1071 s |
| 40000, k-omega SST, 1 mm cells | 4.09 | 4.79 | 98 s, 112 s |

- **In layers it is as measured.** The one number found in print is 7.94
  at a Reynolds number of 389 (cited from the paper by later work); the
  figure's others, some 3 at 100, 5 at 200 and 14 at 800, are from
  memory of it and to be checked against the paper. Later work puts the
  flow as no longer flat across the channel above some 400, and a
  two-dimensional computation falls short of the measurement there; a
  channel with side walls, as here, does not.
- **The side walls are in it.** Across the channel the place moves: at
  800, 12.7 step heights a quarter in from a side wall and 13.8 a tenth
  in, where the middle has 14.4. The measured channel was far wider than
  ours.
- **1 mm cells give 4.59 at 200** in the channel 20 wide, in 7 s: five
  cells to the step, and 3% short of the 0.5 mm cells. That is the
  suite's test (FluidTests/Step.py), which takes 4.2 to 5.4.
- **Turbulent it is rough.** Measured turbulent steps turn forwards 6 to
  8 step heights behind. Ours do at 4 to 7 by the cells and the
  channel's width, are not the same on the channel's two sides, and at
  8000 the run does not settle in its 2000 steps and the cells at the
  walls are 7 wall units out, which the panel says. With cells in the
  wall model's range (40000 on 1 mm) the panel is silent and the place
  is 4.1 to 4.8: the wall model has no law for fluid that turns back,
  so its silence is not a pass here. Sec 14.6's warning is about the
  friction of a flow along a wall; this is a limit of it.
- **Tracers let go in the turned-back fluid** are carried towards the
  step, round, and in the end out: the flow is not flat across the
  channel, so its rings are not closed. The panel lets tracers go at the
  inlets only, and none of those enters; the manual's pictures let them
  go there from a script (Frames.paths).
- **The Reynolds number the panel goes by** is on the inlet's width as
  sec 14.6 has it (221 where Armaly's is 200).

### 14.10 Tracers let go where the user clicks (2026-11-27)

Sec 14.9's step showed what sec 14.7's tracers lack: let go at the
inlets, they go where most of the fluid goes, and fluid that turns back
gets few. The panel now lets more go at a click.

- **Let go at a click** (a button that stays down, under the tracers'
  box) takes the left clicks in the 3D view while it is down, by an
  event callback on the view that is taken off again when the button
  comes up, the box is unticked or the panel closes. A click is a line
  of sight: from the camera through the pointer in perspective, along
  the view's direction otherwise.
- **With no cut shown** the tracers are let go along the line through
  the fluid: `FluidPlayback.seedsAlong` takes the cells whose centres
  the line passes within half a side of (or, the line running between
  cells, within their half diagonal), one place at each cell's depth,
  thinned evenly to 12.
- **With a cut shown** they are let go where the line meets the cut's
  plane (`sightOnPlane`): `seedsAbout` gives the place and one a cell
  from it each way in the plane, those of the five that are in the
  fluid.
- **At the inlets only** takes the picked ones away. They are the
  panel's, not the document's: new results start without them.
- The paths are followed again for all the seeds at each click (some
  100 paths of up to 6000 points: not felt).
- **No more than 15 tracers on a path at once** (`Paths.at`'s `most`):
  a tracer let go each 40 ticks on a path of 6000 points -- one round
  and round behind the step -- made a solid line of 150. On a path
  longer than 15 times 40 points they are now let go less often. This
  changes the inlets' tracers too, where they crawl along a wall.

Tests: `TestTracers.test_seeds_along_a_line_of_sight`,
`test_seeds_at_a_place_on_a_plane`, and the cap in
`test_the_tracers_at_a_tick`. The panel's part has no test in the suite;
the manual's script (fluid/study/tracers/pick.py) sends the view real
clicks of the mouse and reads what the panel did: three clicks behind
the step each let go at 12 places, a click off the fluid at none and
says so, a click on the cut at 5.

Not done: a click picks no depth without a cut; the places are not kept;
no way to take away one click's tracers and keep another's.

### 14.11 A flow through time, and the eddies behind a cylinder (2026-11-28)

The start of F4. A third kind of study, **Flow in time**
(`Analysis::Unsteady`): the one fluid of a steady flow in the same closed
space, from rest, followed through time -- for what does not settle. And
faces the fluid slides along, which the case it is tested on needs.

- **The case** (`steadyCase`, the same writer): `pimpleFoam`, second
  order in time (`backward`), the convection `linearUpwind` and not held
  bounded, two pressure corrections a step. The step is the solver's own,
  no longer than the fluid crosses a cell in (`maxCo 1`) nor than the
  frames are apart; the first a fifth of a cell at the fastest inlet's
  speed. It runs to the study's `TimeEnd` and writes each
  `OutputInterval`. An outlet lets fluid back in at rest (`inletOutlet`):
  an eddy that reaches it draws some. With the eddies modelled
  (`Turbulence`) it is refused, not run yet. A height or force monitor is
  refused as of a steady flow.
- **Frames**: `ResultKind::Fields` as a steady flow's, but a frame's time
  is seconds and the archive's `steady` is not set. The fluid inside
  (sec 14.5) is written with every frame, not once at the end, and every
  frame after the start's keeps it: 28 bytes a cell a frame before the
  archive packs them. Measured 1.3 MB a frame at 59000 cells, 311 MB for
  240 frames in the document. The cells' centres are the same in every
  frame and are kept in every one: open.
- **Faces the fluid slides along** (`Study.Slides`, faces of the walls;
  `Fluid_AddSlide` puts the picked faces among them or takes them out):
  held by nothing, not passed. A plane the flow is the same on both sides
  of, or a wall too far off to slow it. They are the domain's patch after
  the last outlet's (`slidePatch`), one patch `slide` in the case. A
  steady flow takes them too. The patch is a plain one with `slip` on the
  velocity: as one of the mesher's `symmetry` type it was not drawn onto
  its surface -- a channel 3 cells deep came out 2 deep, and two thirds
  of the flow came in. `buildModel()` has the area as `Domain.Slides`.
- **The panel**: "Study of" has the third kind, with the end time and
  the frames' interval in place of the steps. The frames play from the
  start, ten a second where the run's own time would be slower. A cut
  (sec 14.5) is of the frame shown.
- **Tracers through time** (`FluidPlayback.Streaks`): one let go at each
  seed at every frame, each carried from a frame to the next by the flow
  of the frame it leaves, in steps of half a cell at the fastest
  (`getPaths` of that frame, sec 14.7) -- what smoke let out at the seeds
  would draw. Found frame by frame when first asked for and kept. The
  panel's tracers are these for a flow through time: they move as the
  frames do, with no paths drawn; a quarter of the seeds at the inlets
  (20), or the lines fill the view; clicks add seeds as in sec 14.10. A
  frame's flow is held still for the time to the next: frames far apart
  against the flow's changes carry the tracers wrong.
- **Fixed on the way**: the model's triangles were the shape's own, which
  the 3D view keeps as fine as it last drew them. With a round wall the
  model was another after the view had drawn it, and results just kept
  read "of the study as it was before a change" in the panel. The model
  now meshes a copy of each shape (`meshOf`, `spacesInside`).

Measured: a cylinder 10 mm across in water at 10 mm/s (a Reynolds number
of 100), its axis across a channel the fluid slides along on all four
faces, cells of 1 mm (10 across the cylinder). The eddies leave its two
sides in turn; how often is the Strouhal number, the cylinder's width
over the speed and the time between two eddies of one side. In a stream
without walls it is measured at 0.164 to 0.165 (Williamson; from memory,
not checked against the paper); a channel's faces speed the fluid past
the cylinder and raise it.

| Channel (long x high), cylinder's share of the height | Cells | Strouhal number | Run |
|---|---|---|---|
| 250 x 120 mm, 8.3% | 59000 | 0.171 (by hand, from the lift, 16 periods) | 208 s for 200 s of flow, 4 processes |
| 200 x 100 mm, 10% (the manual's) | 40000 | 0.177 | 400 s for 110 s, beside the panel |
| 150 x 80 mm, 12.5% (the test's) | 23000 | 0.178 | 155 s for 64 s, 1 process |

By hand the drag coefficient was 1.46 and the lift swung by 0.41 of the
stream's pressure on the cylinder's front; no monitor of the study
reports a force of a flow that fills its space yet. From rest the wake
is two still eddies for some 20 s, wavers, and sheds fully by 45 s.

Not measured: finer cells, and a channel wide enough to compare with the
open stream's number -- both were started and stopped. In the channel 240
high the grid fell on the cylinder so that one cell came out 14 times
longer than thick, and the step followed that cell: 0.005 s against
0.058, twelve times the steps. How the grid falls on a round body sets
the cost of the run, and nothing tells the user. Open.

Tests: `OpenFoamCase.filesOfAFlowThroughTime`,
`facesTheFluidSlidesAlong`; `FluidTests/Shedding.py` run as
`TestOpenFoamShedding` (the slid area, a frame each half second each with
the fluid inside, in equal to the speed times the face and to out, no
swing at first and one of 0.4 of the stream late, a Strouhal number of
0.16 to 0.20, tracers swept to both sides of the wake, kept with the
document); `TestTracers.test_tracers_carried_through_time`;
`TestSteady.test_the_signature_is_not_of_how_finely_a_wall_was_drawn`.
The panel's part has no test of its own: the manual's script drives it
(`fluid/study/vortex-street/video.py`).

Open: the force on a part (lift and drag are what this flow is measured
by); the cells' centres kept once; what the step is, shown as it runs;
going on from a kept run; turbulence through time; the tracers' lines
drawn; the fluid's spin as a colour for the cut; rotating parts (the
rest of F4).
