# Fluid simulation (CFD): a native workbench on OpenFOAM

Status (2026-11-03): a design only. No code stands behind it. Work starts
after the assembly simulation work (`docs/AssemblyDynamics.md`) is done;
until then this document is the plan to start from.

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
