# Assembly3, bundled for migration

The package `freecad.asm3` of the Assembly3 add-on
(https://github.com/realthunder/FreeCAD_assembly3), commit `773b977`
(version 0.12.3), kept here so that an Assembly3 document opens with its
objects working and can be migrated to the Assembly objects
(`docs/AssemblyObjectModel.md` sec 8) without the add-on installed.

It is not a workbench here and is on no search path.
`../AssemblyAsm3Bundle.py` makes `import freecad.asm3` find it when nothing
else does, so it loads only when a document names one of its proxies, and an
installed Assembly3 always takes precedence.

Left out: `init_gui.py` (the workbench), `install_prompt.py` (installs a
solver with pip), `translations/`.

Changed:
- `asm3/system.py`, `System.unknownType`: no prompt to install a solver.
- Two icons: a degree sign in their description metadata is written `deg`.

The code is under the license in `LICENSE` (GPL-3.0), as in the add-on.
