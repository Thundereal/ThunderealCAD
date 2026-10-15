# Rename inventory — giving the fork its own name

Status: survey, 2026-08-07. No name chosen yet; no commercialization plan —
the motivation is identity: users should know whose software they run, where
to report problems, and what the browser viewer they were just linked to is
called. Decision so far (discussion 2026-08-07): **rename the product now,
keep the code relationship with upstream** — rebases are still cheap (38
commits replayed over 31 upstream with zero conflicts) and a hard fork is a
separate, later decision that becomes right only when rebase cost exceeds its
value.

The raw count — `FreeCAD` appears in ~4,500 source files — is misleading.
The governing principle splits it into a small "change" pile and a huge
"deliberately keep" pile:

> **Rename the product identity. Keep the formats and the APIs.**

## 0. The name: **Thundereal** (decided 2026-08-07)

The product is **Thundereal**; where context needs the category, the
marketed form is **"Thundereal CAD"** (website title, package
descriptions, search phrase). Rationale: the successful pattern in this
market is a distinctive name plus a descriptor in the tagline (Onshape,
Rhino, Blender), not a descriptive name; it matches the domain in hand
(`thundereal.com`, so product, share links and website reinforce each
other); and it audibly carries the "realthunder" lineage every FreeCAD
user already knows.

Collision check (2026-08-07 web survey): **clean** — no product, company
or brand named Thundereal; nearest neighbors (ThunderSoft, Thunderobot,
Thunderstone) are in unrelated categories. Zero SEO competition.

Ruled out: **ThunderCAD** — an active Dutch commercial product
(thundercad.nl, subscription add-on toolbox for Autodesk Inventor).
Same industry, same spelling, EU rights through use; a textbook
confusing-similarity case. The storm-noun+CAD pattern is generally
mined (LIGHTNINGCAD is a registered US trademark).

**Ownership** (established 2026-08-07): the brand belongs to
**Thundereal Technologies Pte. Ltd.** (Singapore) — the user's existing
company. Company: Thundereal Technologies; product: Thundereal;
marketed form: Thundereal CAD. The GitHub org `thundereal` is
registered under the company (corporate ToS); domain `thundereal.com`
in hand. An ACRA-registered company name + domain in use makes the
name claim solid; the optional belt-and-suspenders step is an IPOS
trademark filing (class 9/42) by the company. The LGPL code is not
company-owned — only the brand is.

Still to verify before the public announcement: conda/PyPI package-name
availability, a formal trademark search on "Thundereal" beyond the web
survey (IPOS/EUIPO/USPTO).

The `fc`/`FC_` prefixes sprinkled through code and env vars are *not* a
constraint — see §5.

## 1. The branding mechanism already exists

Upstream FreeCAD supports derivative branding, and the fork inherits it:

- `App::Application::Config()["ExeName"]` / `["ExeVendor"]` — set once in
  `src/Main/MainGui.cpp:186` (currently `"FreeCAD"`). Window titles, log
  file name (`<ExeName>.log`), console banners, crash messages, Qt
  `applicationName` (`src/Gui/Application.cpp:2278`) all read it.
- `src/App/Application.cpp:1465` explicitly replaces "FreeCAD" with the
  branded name in user-visible strings ("Due to branding stuff…").
- User config/data paths derive from `ExeName`/`ExeVendor`
  (`getUserAppDataDir`, `src/App/Application.cpp:1155`).

So the *mechanical core* of the rename is a handful of lines in `src/Main/`,
not a tree-wide sed. Everything else is assets, packaging, and migration.

## 2. Change — user-visible identity

| Item | Where | Notes |
|---|---|---|
| `ExeName` / `ExeVendor` | `src/Main/MainGui.cpp`, `MainCmd.cpp`, `MainPy.cpp`, `FreeCADGuiPy.cpp`, `Identity.h` | done: `ExeName` is `FREECAD_APP_NAME` -- Thundereal ON, FreeCAD OFF -- so the window title, the Qt application name, the version line, the log file name, the Start page's welcome, the splash's text and the file dialogs' "FreeCAD" filters follow it. `Identity.h` holds the rest of what the executables say about themselves: the console banner (its ASCII-art name), the license and credits lines ("Thundereal is based on FreeCAD, and wouldn't be possible without the FreeCAD community"), the About dialog's link and Help > Report a bug, both to `github.com/Thundereal/Thundereal`. The copyright line puts "The Thundereal developers 2026" in front of upstream's authors, who stay: they wrote the code. `ExeVendor` follows the name too, so Qt's window-state file moves with the user folders (section 3). The version: ON numbers the product by date, `2026.10.15dev` (`InitializeFreeCADBuildOptions.cmake`), while a document still states the FreeCAD release the code derives from, `PACKAGE_BASE_VERSION` (0.22), as the version that wrote it, with the product's name and version after the build -- `0.22R1234 (Thundereal 2026.10.15)`. Upstream's readers decide by that release how to read a colour's alpha and several features, so a date there would have every upstream reader misread this fork's files. The Windows version resource and the macOS bundle and its executable follow `FREECAD_APP_NAME` |
| Binary names `FreeCAD`, `FreeCADCmd` | `src/Main/CMakeLists.txt` (`SET_BIN_DIR`) | done: `Thundereal`, `ThunderealCmd` and `Thundereal.app`. Off Windows, `FreeCAD` and `FreeCADCmd` stay beside them as links, in the build tree and the installed bin, for the scripts, tests and add-ons that start them by name (`fc_keep_upstream_name`). Windows -- a portable zip -- has the new names only. The NSIS installer is not maintained: its `APP_NAME` is at once the exe path, the registry key and the Start menu name |
| CMake `project(FreeCAD)` | top-level `CMakeLists.txt:22` | cosmetic but sets several derived vars |
| Icons & splash | `src/Gui/Icons/freecad*.{svg,png,xpm}`, `freecadsplash.png`, `freecadabout.png` | icons done: the T-strike mark, drawn by `src/Tools/thundereal-icons.py` into the app icon (`thundereal*.svg`/`.png`/`.xpm`, Windows `.ico`, macOS `.icns`), the `.TStd` document icon, the empty-window watermark, the package-box label, the browser viewer's favicon and, in line art, the title-block logo of TechDraw's 12 ISO 7200 templates (TechDraw's build swaps it into a copy of each template). Upstream's artwork stays beside it under its own names, and `THUNDEREAL_IDENTITY` picks the set: the code asks for `freecad` as upstream does, and a generated `Branding.qrc` maps `:/icons/freecad.svg` and the package box's names to the picked files. `.FCStd` keeps FreeCAD's document icon -- the format is FreeCAD's -- and `.TStd` keeps its own under either setting. The browser viewer, its own CMake project, has a `THUNDEREAL_IDENTITY` of its own: ON writes `thundereal-favicon.html` into a copy of `shell.html`. The splash (1x and 2x) and the About picture are drawn by the same script, the wordmark set in Ubuntu Sans Bold as outlines, and `Branding.qrc` maps them to `freecadsplash*.png` and `freecadabout.png`; the splash then writes only the version, under the wordmark (`SplashInfoExeName`, `SplashInfoPosition`), and a development build's warning wraps onto more lines to stay left of the mark (`SplashWarningWidth`) |
| About dialog | `src/Gui/Splashscreen.cpp`, `AboutApplication.ui` | must credit lineage: "based on FreeCAD" (LGPL attribution is code-level; this is courtesy + clarity). Done: the name, the credits line, the Credits tab's "FreeCAD would not be possible without", and the picture, which says "Parametric 3D CAD, based on FreeCAD" under the wordmark |
| Desktop/AppStream | `src/XDGData/org.freecad.FreeCAD.desktop`, `org.freecad.FreeCAD.metainfo.xml.in` | done: the CMake switch `THUNDEREAL_IDENTITY` (ON) gives the app id `com.thundereal.Thundereal` and Thundereal's artwork (OFF: `org.freecad.FreeCAD` and upstream's artwork, its files byte for byte). It names the installed desktop entry, AppStream data, MIME package and hicolor app icons, and the running app's desktop file name (Wayland app_id). The sources keep upstream's file names. `Name=` and `<name>` follow `FREECAD_APP_NAME`; so do the AppStream data's developer (The Thundereal developers), the description's name ("Thundereal, based on FreeCAD, is ...") and its URLs: the website, the repository, its tracker, wiki and contributing guide. The screenshots are still upstream's |
| Help menu | `src/Gui/CommandStd.cpp` (`helpSiteUrl`), `Identity.h` (`FREECAD_HELP_SITES`) | done: ON gives the Website (thundereal.com), Forum (the repository's Discussions), Users and Python documentation, Help Website and FAQ (its wiki) and Donate (thundereal.com/#support) entries the program's own sites, and their texts the name. A URL in `BaseApp/Preferences/Websites` equal to upstream's default is the default an earlier build stored, not the user's choice, and is replaced; any other is kept. Help > Report a bug does the same with upstream's tracker. The per-command help (F1, the Help module) is still upstream's wiki |
| WASM viewer page | `src/Gui/Renderer/wasm/shell.html` title/strings (few sites) | the page strangers see first — highest identity leverage per line changed |
| Start page / workbench strings | every `tr()`, `.ui` and Python `translate()` string | done at run time, not in the sources: see "The name in text" below. No source string or `.ts` file changes, so every language keeps its translation and merges from upstream stay clean |
| Windows installer / macOS bundle ids | packaging repos | macOS done: `CFBundleIdentifier` (was empty) and the `.TStd` UTI (`com.thundereal.tstd`) follow `THUNDEREAL_IDENTITY`. Windows has no app id to set (no AppUserModelID in the tree) |

### The name in text

About 460 translatable strings, and some untranslated ones, say "FreeCAD".
Most mean this program ("restart FreeCAD", "FreeCAD's macro directory"); the
rest mean upstream and must keep the word. `App/BrandText.cpp` tells them
apart, and does nothing at all unless `ExeName` is other than FreeCAD:

- **Kept**: upstream's sites and people (FreeCAD wiki, forum, website, FAQ,
  team, community, users, development, documentation, GitHub), its versions (FreeCAD 0.19, FreeCAD version, an earlier
  version of FreeCAD, FreeCAD older than v0.15), a name, path or link
  (`FreeCAD.ActiveDocument`, `.FreeCAD/Mod`, `github.com/FreeCAD/...`,
  FreeCAD-VR), and whole strings that speak for upstream: the credits ("would
  not be possible without"), "at FreeCAD, we believe", "part of FreeCAD",
  "made with FreeCAD", "requires FreeCAD <version>".
- **Dropped**, where the word only says the thing is FreeCAD's and reads true
  without it for either program: the themes' names (FreeCAD Classic, Dark,
  Light become Classic, Dark, Light), two formats' (FreeCAD macro, FreeCAD Tool
  Library), and the add-ons ("Installing FreeCAD Addon" is "Installing Addon",
  "a FreeCAD Addon" is "an Addon"). A translation drops it with what joined it
  to its neighbour ("FreeCAD-Makro" is "Makro").
- **Every FreeCAD named**, in the Help commands whose site is the program's
  own (the Help menu row above), whatever the rules would say: "FreeCAD
  Forum", "Donate to FreeCAD development".
- **Rephrased**, in this fork's own text: "this FreeCAD" -- this build, as
  against upstream -- becomes the name ("only Thundereal reads"), and "other
  FreeCAD" becomes FreeCAD ("will not open in any FreeCAD").
- **Named**: every other FreeCAD.

Gui installs a translator per `.qm` file that applies this to what it
returns, plus a fallback, asked last, for English. A translation is changed
only when every FreeCAD in its English source is named, since which FreeCAD
is which cannot be told in another language; then every FreeCAD in it is,
hyphenated compounds included ("FreeCAD-Datei"). Untranslated text calls
`App::brandText()` where it is written (the first-run console message, the
schema 5 save warnings, the sandbox's errors). Tests:
`tests/src/App/BrandText.cpp`.

Not covered: translations a Python add-on loads with a `QTranslator` of its
own rather than through `FreeCADGui.addLanguagePath` (its English text still
passes the fallback), and untranslated Python strings.

The files the program writes say who made them by its name and the
repository's URL (`App::createdBy()`, `App::programUrl()`, the `ProgramUrl`
config key; FreeCAD and freecad.org when OFF): the mesh formats (OBJ, PLY,
3MF, Inventor, VRML, Asymptote), both STEP writers (originating system,
description and, in `Part.Shape.exportStep`, author and organisation), DXF,
the SVG view export, screenshots, TechDraw's SVG, the FEM and CAM writers
(G-code's "(Exported by ...)", Nastran, Z88, CalculiX, gmsh), the OpenSCAD,
OCA, Kuka, Collada, gbXML, JSON and BIM OBJ exporters and the material cards.
A standard document's zip comment and XML banner keep "FreeCAD Document"; a
schema 5 one says "Thundereal Document". A file that is neither is "Not a
Thundereal/FreeCAD document".

## 3. Change with migration — filesystem & OS integration

These rename *and* must keep reading the old locations, the same
backward-compatible pattern the fork already practices for document
properties:

- **User config/data** (done): the folders follow `ExeName` --
  `~/.config/Thundereal/`, `~/.local/share/Thundereal/`, `~/.cache/Thundereal/`,
  `%APPDATA%\Thundereal`. The first start with no `user.cfg` of its own copies
  FreeCAD's over (`migrateUserFiles` in `App/Application.cpp`): the data folder
  -- macros, add-ons, materials, preference packs, everything but logs and
  settings -- and the settings, a LinkStage3 release's `user.link.cfg` before a
  plain `user.cfg` (and `system.link.cfg` before `system.cfg`). A
  `user.link.cfg` has its colour convention converted on the way, as a vendor
  rename always did; a plain `user.cfg` may be in either, and is left as this
  build already read it. Paths in the copied settings that pointed into the
  old folders point into the new ones. The old folders are left alone, for an
  upstream FreeCAD still using them; `MigratedFrom.txt` in the new settings
  folder records the copy, so it happens once. Nothing is copied when
  `FREECAD_USER_HOME` or `FREECAD_USER_DATA` names the folders. Qt's
  window-state file is not copied, so the window size and position reset
  once. `AppDataName` (config key, or branding.xml) can still keep a renamed
  build in another name's folders.
- **Cache**: `~/.cache/FreeCAD/` — safe to start fresh (it is transient by
  definition), no migration needed.
- **MIME/file association**: register the new app id as handler for the
  *unchanged* `.FCStd` type. Upstream's `org.freecad` MIME declaration
  stays recognized. `application/x-extension-tstd` (glob `*.tstd`) is
  declared beside it, with the thumbnailer and `.desktop` entry taking both;
  its icon is `thundereal-doc.svg`, with drawings of its own for 16 to 32 px.
- **Lock files / single-instance keys / crash-recovery folders** derive from
  `ExeName` and live in the cache folder, which moved too: recovery looks for
  `Thundereal_<pid>.lock` and `Thundereal_Doc_*` in `~/.cache/Thundereal`. So a
  document an upstream FreeCAD left after a crash is no longer offered for
  recovery here, nor the other way round, and the first run after the rename
  does not offer one left by a crash of the last run under the old name (run
  that build once to recover it). A file opened from the desktop while both
  run goes to the program named in the desktop entry, not to whichever started
  first.

## 4. Change — ecosystem & packaging (outside this repo)

- **GitHub repo rename** (`realthunder/FreeCAD` → new): GitHub installs
  permanent redirects for git operations, web, API, raw and release URLs;
  existing clones keep working and forks/issues/stars move intact. The
  redirect survives until a new repo reuses the old name (we control that;
  keep the account name — account renames are the fragile variant). Update
  in-content URLs on our side: feedstock recipes, docs, submodule references
  in sibling repos, CI.
- **Feedstocks**: `freecad-rt-feedstock` package name; `coin3d-feedstock`,
  `pivy-feedstock` unaffected except source URLs. Old conda package name
  should get one final release whose description points at the new name.
- **Companion repos** (`realthunder/coin`, `realthunder/OCCT`): no rename
  needed — they are libraries, not products.
- **Website / wiki / release notes / issue templates**: the loud part.
  Issue templates are the actual fix for the misdirected-bug-report problem —
  state plainly "this is not upstream FreeCAD; report here."
- **Addon Manager**: point default sources at our own addon list if/when it
  diverges; today upstream's list works and can stay.

## 5. Keep — deliberately unchanged

The compatibility surface. Renaming any of these buys nothing and breaks
the ecosystem the product depends on:

- **`.FCStd` for the standard format (schema 4)** -- file exchange with
  FreeCAD users is a feature, and a schema 4 file is still written under
  `.FCStd`. **Revised 2026-10-14: schema 5 is `.TStd`.** The distinct format
  this section once declined already exists -- schema 5 renamed the root to
  `<FCDocument>` so that no other reader opens it -- and a schema 5 file named
  `.FCStd` told the OS, upstream and the user something false. The rules, as
  built on the `Branding` branch:
  - The extension follows the schema: 5 writes `.TStd` (backups `.TBak`), 4
    writes `.FCStd` (backups `.FCBak`). Opening goes by the content, never the
    extension, and accepts both.
  - A plain Save that would raise a standard file into schema 5 -- which is
    every first save of an opened `.FCStd`, since restore raises the schema --
    writes `Part.TStd` beside `Part.FCStd`, leaves the original untouched, and
    the document lives at `Part.TStd` from then on. The Gui says so in one
    dialog (`Document::getSaveFileName`).
  - Save As and Save a Copy name the file for the format chosen; the name
    field follows the Standard/Compact toggle as it is flipped. A Python
    `saveAs()` given an explicit name writes exactly that name.
  - Links are NOT redirected. Opening a missing `Part.FCStd` whose
    `Part.TStd` exists -- a link's target, a recent file -- fails as before,
    and the error names the sibling.
  - Why "Std": `FCStd` is "FreeCAD Standard", after OCAF's "Standard"
    document format, whose extension is `.std` (`MDTV-Standard` in OCCT's
    `resources/StdResource/Standard`); FreeCAD still opens a bare `.std` from
    the command line. `TStd` keeps the pattern for the brand initial.
- **Python modules `FreeCAD`, `FreeCADGui`, `Part`, …** — every workbench,
  addon and user macro imports these. This API *is* the ecosystem. (Upstream
  derivatives that rebranded all kept them.) The ~4,500-file grep count
  lives overwhelmingly here and in internal C++ naming.
- **Env vars** — 243 distinct `FC_*`/`FREECAD_*` names in the tree; scripts,
  harnesses, and user setups depend on them. `FC_` is name-neutral anyway.
- **Internal C++ namespaces, class prefixes (`SoFC*`), CMake target names,
  `fcviewer.*`, `fcscenediff`** — invisible to users; churning them is pure
  rebase-conflict liability against upstream.
- **Document XML tags and property names** — persistence compat, as ever.

## 6. Suggested phasing

1. **Name chosen + collision-checked** (trademark, domain, GitHub, conda).
   **DONE** for the choice and the informal web survey (§0); formal
   registry checks remain.
2. **Soft identity switch** — `ExeName`/vendor, About, icons/splash, viewer
   page, issue templates, README. One PR-sized change; the product stops
   introducing itself as FreeCAD. Old binary names symlinked.
3. **OS integration + migration** — desktop/metainfo ids, config-dir
   migration code, MIME handler. Needs one release of soak.
4. **Repo + packaging rename** — GitHub rename (redirects carry the old
   links), feedstock rename with a final pointer release under the old name,
   website/docs sweep.
5. **Never (until an actual hard fork is decided)**: formats, Python API,
   env vars, internal naming.

Rebase-friction note: phases 2–3 touch files upstream also touches
(`MainGui.cpp`, `Application.cpp`, XDG data) but only a few dozen lines;
the branding indirection keeps the diff small. The tree-wide churn that
would make rebases expensive is precisely what §5 declines to do.

## 7. Open questions

- Reverse-DNS id: settled as `com.thundereal.Thundereal` (three parts, as
  Flatpak requires, in upstream's `org.freecad.FreeCAD` shape).
- Whether the browser viewer carries the same name as the desktop product
  or its own sub-brand (one product, one name is the safer default).
- When (if ever) `LinkMerge`/`LinkVibe` branch names should follow — cosmetic,
  zero urgency, redirects make it cheap whenever.
