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
| `ExeName` / `ExeVendor` | `src/Main/MainGui.cpp`, `MainCmd.cpp`, `MainPy.cpp`, `FreeCADGuiPy.cpp`, `Identity.h` | done: `ExeName` is `FREECAD_APP_NAME` -- Thundereal ON, FreeCAD OFF -- so the window title, the Qt application name, the version line, the log file name, the Start page's welcome, the splash's text and the file dialogs' "FreeCAD" filters follow it. `Identity.h` holds the rest of what the executables say about themselves: the console banner (its ASCII-art name), the license and credits lines ("Thundereal is based on FreeCAD, and wouldn't be possible without the FreeCAD community"), the About dialog's link and Help > Report a bug, both to `github.com/Thundereal/Thundereal`. The copyright line stays upstream's: it names the authors of the code. `ExeVendor` stays FreeCAD, and the user folders do too (section 3). The Windows version resource and the macOS bundle name follow `FREECAD_APP_NAME`; the binaries keep their names |
| Binary names `FreeCAD`, `FreeCADCmd` | `src/Main/CMakeLists.txt` (`SET_BIN_DIR`) | keep old names as symlinks for one release cycle |
| CMake `project(FreeCAD)` | top-level `CMakeLists.txt:22` | cosmetic but sets several derived vars |
| Icons & splash | `src/Gui/Icons/freecad*.{svg,png,xpm}`, `freecadsplash.png`, `freecadabout.png` | icons done: the T-strike mark, drawn by `src/Tools/thundereal-icons.py` into the app icon (`thundereal*.svg`/`.png`/`.xpm`, Windows `.ico`, macOS `.icns`), the `.TStd` document icon, the empty-window watermark, the package-box label, the browser viewer's favicon and, in line art, the title-block logo of TechDraw's 12 ISO 7200 templates (TechDraw's build swaps it into a copy of each template). Upstream's artwork stays beside it under its own names, and `THUNDEREAL_IDENTITY` picks the set: the code asks for `freecad` as upstream does, and a generated `Branding.qrc` maps `:/icons/freecad.svg` and the package box's names to the picked files. `.FCStd` keeps FreeCAD's document icon -- the format is FreeCAD's -- and `.TStd` keeps its own under either setting. The browser viewer, its own CMake project, has a `THUNDEREAL_IDENTITY` of its own: ON writes `thundereal-favicon.html` into a copy of `shell.html`. Splash and About still to draw (they need a wordmark) |
| About dialog | `src/Gui/Splashscreen.cpp`, `AboutApplication.ui` | must credit lineage: "based on FreeCAD" (LGPL attribution is code-level; this is courtesy + clarity). Done in text: the name, the credits line and the Credits tab's "FreeCAD would not be possible without". The picture is still `freecadabout.png`, FreeCAD's wordmark, as is the splash -- they need a Thundereal wordmark |
| Desktop/AppStream | `src/XDGData/org.freecad.FreeCAD.desktop`, `org.freecad.FreeCAD.metainfo.xml.in` | done: the CMake switch `THUNDEREAL_IDENTITY` (ON) gives the app id `com.thundereal.Thundereal` and Thundereal's artwork (OFF: `org.freecad.FreeCAD` and upstream's artwork, its files byte for byte). It names the installed desktop entry, AppStream data, MIME package and hicolor app icons, and the running app's desktop file name (Wayland app_id). The sources keep upstream's file names. `Name=` and `<name>` follow `FREECAD_APP_NAME`; the AppStream description, developer name, screenshots and URLs are still upstream's copy |
| WASM viewer page | `src/Gui/Renderer/wasm/shell.html` title/strings (few sites) | the page strangers see first — highest identity leverage per line changed |
| Start page / workbench strings | every `tr()`, `.ui` and Python `translate()` string | done at run time, not in the sources: see "The name in text" below. No source string or `.ts` file changes, so every language keeps its translation and merges from upstream stay clean |
| Windows installer / macOS bundle ids | packaging repos | macOS done: `CFBundleIdentifier` (was empty) and the `.TStd` UTI (`com.thundereal.tstd`) follow `THUNDEREAL_IDENTITY`. Windows has no app id to set (no AppUserModelID in the tree) |

### The name in text

About 460 translatable strings, and some untranslated ones, say "FreeCAD".
Most mean this program ("restart FreeCAD", "FreeCAD's macro directory"); the
rest mean upstream and must keep the word. `App/BrandText.cpp` tells them
apart, and does nothing at all unless `ExeName` is other than FreeCAD:

- **Kept**: upstream's sites and people (FreeCAD wiki, forum, website, FAQ,
  team, community, users, development, documentation, GitHub), its add-ons
  (FreeCAD Addon), its versions (FreeCAD 0.19, FreeCAD version, an earlier
  version of FreeCAD, FreeCAD older than v0.15), a name, path or link
  (`FreeCAD.ActiveDocument`, `.FreeCAD/Mod`, `github.com/FreeCAD/...`,
  FreeCAD-VR), and whole strings that speak for upstream: the credits ("would
  not be possible without"), "at FreeCAD, we believe", "part of FreeCAD",
  "made with FreeCAD", "requires FreeCAD <version>", and the names of its
  themes (FreeCAD Classic/Dark/Light) and formats (FreeCAD macro, FreeCAD Tool
  Library).
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

Not covered: translators a Python add-on installs itself, untranslated
Python strings, and the exported files' provenance ("Created by FreeCAD
<https://www.freecad.org>" in OBJ/PLY/STL/3MF/SVG/STEP headers, G-code
comments, screenshots), which waits on which name and URL they should carry.

## 3. Change with migration — filesystem & OS integration

These rename *and* must keep reading the old locations, the same
backward-compatible pattern the fork already practices for document
properties:

- **User config/data** (not started -- `AppDataName`, set by `Identity.h`,
  keeps the folders FreeCAD's while `ExeName` is Thundereal; until this is
  done the program still shares `user.cfg`, macros and add-ons with an
  upstream FreeCAD on the same machine; Qt's window-state file is named by
  `ExeName`, so it is `FreeCAD/Thundereal.conf` now and the window size and
  position reset once): `~/.config/FreeCAD/`, `~/.local/share/FreeCAD/`,
  `%APPDATA%\FreeCAD` → new name, with first-run migration (copy, don't
  move; leave a marker). `user.cfg`/`system.cfg` contents are
  name-agnostic.
- **Cache**: `~/.cache/FreeCAD/` — safe to start fresh (it is transient by
  definition), no migration needed.
- **MIME/file association**: register the new app id as handler for the
  *unchanged* `.FCStd` type. Upstream's `org.freecad` MIME declaration
  stays recognized. `application/x-extension-tstd` (glob `*.tstd`) is
  declared beside it, with the thumbnailer and `.desktop` entry taking both;
  its icon is `thundereal-doc.svg`, with drawings of its own for 16 to 32 px.
- **Lock files / single-instance keys / crash-recovery folders** derive from
  `ExeName` (now Thundereal, so a document an upstream FreeCAD left after a
  crash is no longer offered for recovery here, nor the other way round, and
  the first run after the rename does not offer one left by the last run
  under the old name) -- old and new
  builds will not see each other's instances during a transition; acceptable.

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
