<p align="center">
  <a href="https://thundereal.org"><img src="src/Gui/Icons/thundereal-mark.svg" width="96" height="96" alt="Thundereal"></a>
</p>

<h1 align="center">Thundereal</h1>

<p align="center"><b>Parametric 3D CAD. Everything you dreamed of. And then some.</b></p>

<p align="center">
  <a href="https://thundereal.org">Website</a> &middot;
  <a href="https://github.com/Thundereal/ThunderealCAD/releases">Download</a> &middot;
  <a href="https://github.com/Thundereal/ThunderealCAD/discussions">Discussions</a> &middot;
  <a href="https://github.com/Thundereal/ThunderealCAD/wiki">Wiki</a> &middot;
  <a href="https://github.com/Thundereal/ThunderealCAD/issues">Report a bug</a>
</p>

Thundereal is FreeCAD rebuilt where it counts: a renderer that brings your
design to life while you are still building it, a geometry kernel that
finishes the shells, fillets and drafts others give up on, and a model that
remembers every step and branches like code.

Open source (LGPL-2.1+). Opens FreeCAD files. Linux, Windows and macOS, and
browser and mobile through sharing. The full tour is at
**[thundereal.org](https://thundereal.org)**.

## Rendering

The glossy product shots in CAD tutorials are made right here, in the window
you model in, and they stay live while you edit.

- **A real-time GPU engine** -- physically based materials, soft shadows,
  ambient occlusion, image-based lighting, glass, water and fire, on OpenGL
  or Vulkan.
- **Surface finish** -- screw threads that look cut, not painted, at their
  true pitch and profile; knurled, brushed, blasted and turned too, face by
  face, with no UV mapping.
- **MaterialX** -- the open material standard, with a node-graph editor for
  your own shaders.
- **Cycles** -- Blender's path tracer inside the 3D view for the final
  photoreal picture.

## A kernel that finishes the job

Thundereal ships its own fork of OpenCASCADE, fixed case by case against
models from real bug reports and checked against upstream on every fix.

- **Thickness** -- shells that fail or come out inside out upstream come out
  right.
- **Fillet** -- setback blends where fillets meet at a corner, and a smooth
  patch where the corner can't be solved exactly.
- **Draft** -- a new draft that can change topology, taken automatically
  wherever the classic draft would cross itself.

## A model with a memory

Every step is written into the document's transaction log, and on it sits
what git gives code.

- **Every step, kept** -- each edit, undo, recompute and save, with every
  value before and after, travelling inside the file.
- **Versions** -- the whole document as it was at a moment.
- **Branches and merge** -- try the risky idea on a branch of the same file;
  changes merge by what they touch.
- **Shape diff** -- see what a merge, a version or a step would change,
  drawn as shapes, before you take it.

## And then some

Progressive loading for large models, live sharing to a browser or phone
behind a sign-in door, Blender-style split views, one search box for
objects, commands and settings (press `/`), a sandbox for formulas and
Python features in files you were sent, and a built-in MCP server for your
AI assistant.

## Built on FreeCAD

Thundereal is a fork of [FreeCAD](https://www.freecad.org) and wouldn't be
possible without the FreeCAD community. It keeps FreeCAD's Python API, so
macros and most add-ons work unchanged. It grew out of the Link branch,
where `App::Link` and the first fix for topological naming started.

Thundereal is not FreeCAD: please report its bugs on
[Thundereal's tracker](https://github.com/Thundereal/ThunderealCAD/issues),
not upstream's.

## Building

Thundereal builds with CMake and Ninja against a conda environment, its own
forks of [OpenCASCADE](https://github.com/realthunder/OCCT) and
[Coin3D](https://github.com/realthunder/coin), and Qt 6.
[docs/DevEnvironment.md](docs/DevEnvironment.md) has the full recipe, and
[docs/Testing.md](docs/Testing.md) runs the test suites.

## Support the work

Thundereal is made in the open. If it saves you time, these keep it going:
[Patreon](https://www.patreon.com/thundereal) &middot;
[Liberapay](https://liberapay.com/realthunder/donate) &middot;
[PayPal](https://www.paypal.me/realthunder)

## License

[LGPL-2.1-or-later](LICENSE), the same as FreeCAD.
