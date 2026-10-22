# Header inclusion audit -- build localization on change

Tree: `build/win-relwithdebinfo-801` (Ninja, MSVC, branch AssemblyPort, built 2026-10-22).
Data: `.ninja_deps` (every object's real header set via /showIncludes), `.ninja_log` (last
duration per step), `build.ninja` (compile and link edges), `git log` of the last 12 months
for edit frequency. Nothing in the tree was edited or built. Scripts and raw outputs:
`scripts/header-audit/` (rerun with `sh scripts/header-audit/run_audit.sh`; the raw outputs land in `<build>/header-audit/`).

## 0. Totals and method

| | |
|---|---|
| compile steps (TUs) | **4015** (2200 under `src/` minus 3rdParty; the rest is 3rdParty, bgfx tools, tests) |
| compile CPU of a full build | **423.8 min** (fork TUs: 353.6 min) -> ~70.6 min wall at -j 6 |
| link steps fed by TUs | 148, 1.6 min |
| AUTOMOC/AUTOUIC steps with header deps | 566, 355 s |
| fork headers seen | 2097; 226 reach >= 100 TUs, 119 reach >= 500, 69 reach >= 1000 |
| PCH | **off** (`FREECAD_USE_PCH=OFF` in the cache; no `/Yu` in build.ninja). `PreCompiled.h` is a no-op without `_PreComp_` (FCConfig.h + pragmas) |
| compile time vs headers | fork TUs pull 8.1 MB of headers on average; t = 1.5 s + 1.0 s/MB fits with r2 0.72 -- most compile time is header parsing |
| churn-weighted rebuild cost | sum over headers of (edits in 12 months x rebuild CPU) = **2740 CPU-h** (~457 h wall at -j 6). Upper bound: it treats every commit as its own rebuild |

Rebuild cost of touching header H = sum of compile ms of every TU whose deps contain H, plus the
link steps those objects feed (static libs propagate one level; DLL import libs are restat=1 and
only relink consumers when exports change, so they are not counted), plus AUTOMOC steps whose
depfile lists H. Wall = CPU/6 + links + autogen/6. Durations come from builds run at -j 6, so they
already include contention.

PartFeature.h check: the model gives 467 TUs + 29 links + 28 AUTOMOC = **524 steps, 117 CPU-min,
~20 min wall** on this tree. The 2026-10-12 measurement was 610-640 steps / ~45 min; the gap is
probably a different tree or state (more targets reconfigured, a moc cascade, or the build-fcad
reconfigure that rebuilds 300-400 files by itself -- see the memory note on skipping the reconfigure).
Treat the model's numbers as the floor for a clean incremental build.

## 1. Top 60 headers by rebuild cost

"edits 12mo" = non-merge commits touching the header since 2025-10-22.

| # | header | TUs | compile CPU min | links | autogen | steps | wall min @-j6 | edits 12mo |
|---|---|---|---|---|---|---|---|---|
| 1 | `<build>/src/QtCore.h` | 2168 | 365 | 111 | 104 | 2383 | 62.6 | 0 |
| 2 | `src/FCGlobal.h` | 2168 | 365 | 111 | 104 | 2383 | 62.6 | 1 |
| 3 | `src/FCConfig.h` | 2040 | 354 | 100 | 88 | 2228 | 60.7 | 2 |
| 4 | `src/Base/Type.h` | 1866 | 347 | 88 | 100 | 2054 | 59.3 | 4 |
| 5 | `src/Base/BaseClass.h` | 1864 | 347 | 87 | 100 | 2051 | 59.3 | 1 |
| 6 | `src/Base/TimeInfo.h` | 1848 | 346 | 89 | 100 | 2037 | 59.1 | 1 |
| 7 | `src/Base/FileInfo.h` | 1846 | 346 | 89 | 100 | 2035 | 59.1 | 1 |
| 8 | `src/Base/Exception.h` | 1836 | 345 | 87 | 100 | 2023 | 59.0 | 3 |
| 9 | `src/Base/Console.h` | 1773 | 341 | 86 | 96 | 1955 | 58.4 | 6 |
| 10 | `src/Base/Handle.h` | 1624 | 329 | 82 | 96 | 1802 | 56.2 | 0 |
| 11 | `src/CXX/WrapPython.h` | 1648 | 326 | 86 | 88 | 1822 | 55.7 | 0 |
| 12 | `src/CXX/Config.hxx` | 1646 | 326 | 86 | 88 | 1820 | 55.7 | 0 |
| 13 | `src/CXX/Python3/Config.hxx` | 1646 | 326 | 86 | 88 | 1820 | 55.7 | 0 |
| 14 | `src/CXX/Python3/IndirectPythonInterface.hxx` | 1646 | 326 | 86 | 88 | 1820 | 55.7 | 0 |
| 15 | `src/CXX/Python3/cxx_standard_exceptions.hxx` | 1646 | 326 | 86 | 88 | 1820 | 55.7 | 0 |
| 16 | `src/CXX/Python3/CxxDebug.hxx` | 1645 | 326 | 86 | 88 | 1819 | 55.7 | 0 |
| 17 | `src/CXX/Python3/Exception.hxx` | 1645 | 326 | 86 | 88 | 1819 | 55.7 | 0 |
| 18 | `src/CXX/Python3/Objects.hxx` | 1645 | 326 | 86 | 88 | 1819 | 55.7 | 0 |
| 19 | `src/CXX/Version.hxx` | 1645 | 326 | 86 | 88 | 1819 | 55.7 | 0 |
| 20 | `src/Base/Persistence.h` | 1594 | 323 | 76 | 88 | 1758 | 55.2 | 0 |
| 21 | `src/CXX/Objects.hxx` | 1613 | 322 | 80 | 88 | 1781 | 55.1 | 0 |
| 22 | `src/Base/Vector3D.h` | 1609 | 320 | 75 | 96 | 1780 | 54.8 | 2 |
| 23 | `src/Base/Observer.h` | 1555 | 319 | 82 | 96 | 1733 | 54.7 | 0 |
| 24 | `src/Base/Bitmask.h` | 1518 | 318 | 74 | 92 | 1684 | 54.5 | 0 |
| 25 | `src/Base/Matrix.h` | 1560 | 317 | 75 | 96 | 1731 | 54.3 | 0 |
| 26 | `src/App/StringHasher.h` | 1462 | 311 | 73 | 88 | 1623 | 53.3 | 8 |
| 27 | `src/Base/Parameter.h` | 1504 | 310 | 81 | 84 | 1669 | 53.1 | 2 |
| 28 | `src/Base/Rotation.h` | 1484 | 308 | 71 | 92 | 1647 | 52.6 | 0 |
| 29 | `src/Base/Placement.h` | 1473 | 307 | 71 | 92 | 1636 | 52.5 | 0 |
| 30 | `src/App/ElementNamingUtils.h` | 1417 | 306 | 72 | 56 | 1545 | 52.3 | 0 |
| 31 | `src/App/IndexedName.h` | 1416 | 306 | 72 | 56 | 1544 | 52.3 | 1 |
| 32 | `src/App/MappedName.h` | 1414 | 306 | 72 | 56 | 1542 | 52.3 | 0 |
| 33 | `src/Base/Swap.h` | 1397 | 302 | 77 | 84 | 1558 | 51.7 | 0 |
| 34 | `src/Base/Stream.h` | 1396 | 302 | 77 | 84 | 1557 | 51.7 | 1 |
| 35 | `src/Base/Unit.h` | 1373 | 299 | 70 | 88 | 1531 | 51.2 | 5 |
| 36 | `src/App/Application.h` | 1425 | 298 | 79 | 80 | 1584 | 51.1 | 12 |
| 37 | `src/App/Property.h` | 1351 | 297 | 70 | 84 | 1505 | 50.8 | 22 |
| 38 | `src/App/stx/any.hpp` | 1351 | 297 | 70 | 84 | 1505 | 50.9 | 0 |
| 39 | `src/Base/Tools2D.h` | 1405 | 295 | 70 | 92 | 1567 | 50.4 | 0 |
| 40 | `src/App/PropertyLinks.h` | 1324 | 294 | 69 | 84 | 1477 | 50.3 | 19 |
| 41 | `src/Base/ViewProj.h` | 1392 | 293 | 70 | 92 | 1554 | 50.2 | 0 |
| 42 | `src/Base/BoundBox.h` | 1385 | 293 | 70 | 92 | 1547 | 50.2 | 0 |
| 43 | `src/Base/Color.h` | 1328 | 292 | 71 | 84 | 1483 | 50.0 | 7 |
| 44 | `src/App/Color.h` | 1322 | 292 | 69 | 84 | 1475 | 49.9 | 3 |
| 45 | `src/App/MaterialAppearance.h` | 1312 | 290 | 69 | 84 | 1465 | 49.7 | 4 |
| 46 | `src/App/FileBlobManager.h` | 1265 | 285 | 65 | 84 | 1414 | 48.8 | 34 |
| 47 | `src/App/DynamicProperty.h` | 1254 | 282 | 64 | 84 | 1402 | 48.2 | 1 |
| 48 | `src/Base/COWData.h` | 1233 | 280 | 65 | 84 | 1382 | 47.9 | 2 |
| 49 | `src/App/AppearanceList.h` | 1228 | 279 | 64 | 84 | 1376 | 47.8 | 3 |
| 50 | `src/App/Enumeration.h` | 1228 | 279 | 64 | 84 | 1376 | 47.8 | 0 |
| 51 | `src/Base/Uuid.h` | 1228 | 279 | 65 | 84 | 1377 | 47.8 | 0 |
| 52 | `src/App/PropertyStandard.h` | 1227 | 279 | 64 | 84 | 1375 | 47.8 | 28 |
| 53 | `src/App/PropertyContainer.h` | 1222 | 278 | 63 | 84 | 1369 | 47.6 | 10 |
| 54 | `src/App/PropertyGeo.h` | 1224 | 277 | 67 | 84 | 1375 | 47.5 | 1 |
| 55 | `src/App/ExtensionContainer.h` | 1198 | 274 | 62 | 84 | 1344 | 47.0 | 2 |
| 56 | `src/Base/SmartPtrPy.h` | 1198 | 274 | 63 | 84 | 1345 | 46.9 | 0 |
| 57 | `src/boost_graph_adjacency_list.hpp` | 1191 | 274 | 62 | 80 | 1333 | 46.9 | 0 |
| 58 | `src/App/TransactionalObject.h` | 1195 | 274 | 62 | 84 | 1341 | 46.9 | 1 |
| 59 | `src/App/PropertyPythonObject.h` | 1192 | 273 | 62 | 84 | 1338 | 46.8 | 4 |
| 60 | `src/App/DocumentObject.h` | 1189 | 273 | 62 | 84 | 1335 | 46.8 | 17 |

The top 60 are the foundation layer (FCGlobal/FCConfig, Base, CXX, App property/document
core): each one reaches 1200-2200 TUs and costs 47-63 min wall per touch, but most are almost
never edited. What costs time in practice is reach x edit frequency:

| header | edits 12mo | TUs | CPU min / edit | wall min / edit | CPU-h / 12mo |
|---|---|---|---|---|---|
| `src/App/Document.h` | 97 | 712 | 178 | 30.6 | 288 |
| `src/App/FileBlobManager.h` | 34 | 1265 | 285 | 48.8 | 161 |
| `src/App/PropertyStandard.h` | 28 | 1227 | 279 | 47.8 | 130 |
| `src/App/Property.h` | 22 | 1351 | 297 | 50.8 | 109 |
| `src/App/PropertyLinks.h` | 19 | 1324 | 294 | 50.3 | 93 |
| `src/Gui/Renderer/Renderer.h` | 214 | 123 | 25 | 4.6 | 90 |
| `src/App/DocumentObject.h` | 17 | 1189 | 273 | 46.8 | 77 |
| `src/Gui/ViewProvider.h` | 31 | 487 | 119 | 20.3 | 61 |
| `src/Gui/Application.h` | 30 | 491 | 122 | 20.8 | 61 |
| `src/App/Application.h` | 12 | 1425 | 298 | 51.1 | 60 |
| `src/Mod/Part/App/PartFeature.h` | 29 | 467 | 117 | 20.2 | 57 |
| `src/Base/Tools.h` | 13 | 1043 | 235 | 40.2 | 51 |
| `src/Gui/View3DInventorViewer.h` | 56 | 184 | 51 | 8.7 | 47 |
| `src/App/PropertyContainer.h` | 10 | 1222 | 278 | 47.6 | 46 |
| `src/Gui/Document.h` | 26 | 406 | 104 | 17.7 | 45 |
| `src/App/StringHasher.h` | 8 | 1462 | 311 | 53.3 | 42 |
| `src/App/PropertyExpressionEngine.h` | 9 | 1189 | 273 | 46.8 | 41 |
| `src/Gui/ViewProviderGeometryObject.h` | 31 | 309 | 76 | 13.1 | 39 |
| `src/Mod/Part/App/TopoShape.h` | 16 | 603 | 139 | 23.9 | 37 |
| `src/App/GeoFeature.h` | 11 | 856 | 198 | 34.0 | 36 |
| `src/Mod/Part/Gui/ViewProviderExt.h` | 53 | 148 | 39 | 6.8 | 35 |
| `src/Base/Console.h` | 6 | 1773 | 341 | 58.4 | 34 |
| `src/Base/Color.h` | 7 | 1328 | 292 | 50.0 | 34 |
| `src/Mod/Part/App/PropertyTopoShape.h` | 17 | 471 | 118 | 20.3 | 33 |
| `src/Gui/ViewProviderDocumentObject.h` | 18 | 438 | 106 | 18.2 | 32 |

The four headers in the question, for comparison:

| header | TUs | CPU min | links | autogen | steps | wall min | rank |
|---|---|---|---|---|---|---|---|
| `src/Base/Vector3D.h` | 1609 | 320 | 75 | 96 | 1780 | 54.8 | 22 |
| `src/Gui/Application.h` | 491 | 122 | 24 | 16 | 531 | 20.8 | 120 |
| `src/Gui/Document.h` | 406 | 104 | 20 | 20 | 446 | 17.7 | 132 |
| `src/App/GeoFeatureGroupExtension.h` | 163 | 45 | 18 | 12 | 193 | 8.0 | 184 |
| (`src/Mod/Part/App/PartFeature.h`) | 467 | 117 | 29 | 28 | 524 | 20.2 | 130 |
| (`src/App/Document.h`) | 712 | 178 | 56 | 40 | 808 | 30.6 | 81 |

## 2. Why they reach so many TUs

Method: per TU, the include graph was rebuilt from the `#include` lines of the TU and of every
header (fork and external), resolved only inside that TU's real header set (so stale or
conditional includes cannot invent edges; `_PreComp_`/non-Windows branches are skipped). For each
target header: its direct parents, and its **dominators** -- headers every include path to it
goes through, i.e. the umbrella that alone carries it into a TU. Unresolved cases: 0-3 TUs per
header. Full data: `analyze.json`.

### 2.1 The foundation chain (ranks 1-35)

Everything below is entered through a handful of edges; each line is "parent carries child into N
TUs" from the dominator data.

- `FCGlobal.h` <- `FCConfig.h` <- `App/StringHasher.h` (1460) -- an App TNP header is how most
  TUs first get FCConfig.
- `Base/Exception.h` -> `Base/FileInfo.h` -> **`<boost/filesystem.hpp>`** + `TimeInfo.h`:
  `FileException` holds a `FileInfo` by value, so every one of 1836 Exception.h users parses
  boost.filesystem. FileInfo.h is the sole boost gateway for 194 TUs.
- `Base/Observer.h` -> `Console.h` + `Exception.h`, only for logging inside `Subject<>`'s inline
  template bodies (lines 126-188). Observer.h is the only route to Console.h for 397 TUs.
- `Base/Parameter.h` <- `App/Application.h` (1417) <- `App/ElementMap.h` (990, **unused include**)
  <- `App/ComplexGeoData.h` <- `GeoFeature.h` / `TopoShape.h`. Parameter.h is also the sole xerces
  gateway for 883 TUs (it includes `xercesc/util/XercesDefs.hpp`).
- `CXX/*` (PyCXX, 1645 TUs): `CXX/WrapPython.h` is the sole Python.h gateway for 1078 TUs;
  `CXX/Objects.hxx` is pulled by `App/StringHasher.h` (1460, which uses only `PyObject*`),
  `PropertyPythonObject.h` (1189), `App/Document.h` (705), `Selection.h` (620).
- `App/Property.h` -> `Base/Stream.h`, `stx/any.hpp`, `Exception.h`.
- `App/DocumentObject.h` (1189) -> `PropertyExpressionEngine.h` (member `ExpressionEngine`, needs
  the full type) -> `boost_graph_adjacency_list.hpp` + `boost/graph/topological_sort.hpp`: every
  DocumentObject user parses boost.graph for one private `using DiGraph`.
- `App/DocumentObject.h` -> `PropertyStandard.h` (1224) -> `AppearanceList.h`,
  `FileBlobManager.h`, `MaterialAppearance.h`, `COWData.h`, `Enumeration.h`, `Uuid.h`,
  `boost/filesystem/path.hpp`, `dynamic_bitset`. PropertyStandard.h is the fork's busiest property
  header and carries the fork-only `PropertyAppearance`/`PropertyAppearanceList` (lines 1105-1360),
  which are what need AppearanceList/FileBlobManager/MaterialAppearance.
- `App/FileBlobManager.h` (1265 TUs, 913 lines, 34 edits) arrives through AppearanceList.h (1225),
  PropertyStandard.h (1224), FileSet.h, PropertyFile.h, PropertyElementAppearance.h,
  MaterialXDocument.h, PropertyMaterial.h, PropertyTopoShape.h -- all of which need only the blob
  value types (`FileBlob`, `FileBlobHandle`, `BlobReferrer(Property)`) from its first ~340 lines.
  Only 33 .cpp files name `FileBlobManager` at all.
- `App/DocumentObject.h` -> `PropertyGeo.h` + `Base/Placement.h`, while it uses `Placement` only
  as a return type and `PropertyPlacement` only by pointer.

### 2.2 The upper layers

- **`Gui/Document.h`** (406): **266 .cpp include it directly**; the rest come through
  `ViewProviderFeaturePython.h` (dominates 119 TUs; it needs Gui::Document for one inline call,
  `getDocument()->toggleInSceneGraph(this)` at line 524), `PartDesign/Gui/ViewProvider.h` (40),
  `Fem/Gui/ViewProviderFemConstraint.h` (34) and `Sketcher/Gui/ViewProviderSketch.h` (9; it
  includes Gui/Document.h, App/Part.h and BodyBase.h and uses none of them).
  Gui/Document.h itself includes `App/Document.h` (it only needs `App::Document*`/`&`) and
  `MDIView.h` (only `MDIView*` plus `BaseView` for the `foreachView` template, which is in
  `View.h`). `MDIView.h` -> `ActiveObjectList.h` -> **`Tree.h`** (QTreeWidget, App/Application.h,
  DocumentObject.h, Parameter.h, DockWindow.h, Selection.h), and ActiveObjectList.h needs only
  `HighlightMode`, which lives in `TreeItemMode.h`. So every Gui/Document.h user parses the tree
  view widget.
- **`App/Document.h`** (712; 97 edits -- the single most expensive header in practice):
  **482 .cpp include it directly**, Gui/Document.h carries it alone into 185 TUs,
  ViewProviderFeaturePython.h into 92, ViewProviderPythonFeature.h 49, PartDesign/Gui/ViewProvider.h
  29, Fem 28. Edit classification (below) shows 76 of 97 edits changed public declarations, so a
  pimpl does not help; only reach does.
- **`Gui/Application.h`** (491): 396 .cpp include it directly, `Gui/Command.h` carries it alone
  into 84 more (Command.h needs it only for the `FCMD_SET_EDIT` macro body). The header declares
  **85 `static PyObject* s...` wrappers** plus `Methods[]`; their only referrer is the method table
  in `ApplicationPy.cpp`. Gui::Application already has a pimpl (`ApplicationP* d`).
- **`Mod/Part/App/PartFeature.h`** (467) carries a whole module: `PropertyMaterial.h` ->
  `Materials.h` -> `App/Application.h`, `MaterialXDocument.h`, `Model.h`, `MaterialValue.h` ->
  **`Gui/MetaTypes.h`** (an App-library header including a Gui header) -> `DocumentObserver.h`,
  Placement/Matrix/Quantity, QMetaType. PropertyMaterial.h already forward-declares `Material`
  and holds only `std::shared_ptr<const Material>`; the Materials.h include is unneeded.
  PartFeature.h dominates Gui/MetaTypes.h in 401 TUs and Base/Reader.h (xerces SAX) in 367.
- **`Base/Vector3D.h`** (1609): 49 .cpp include it directly; it arrives through Matrix.h (1558),
  Rotation.h, Placement.h, Tools2D.h, ViewProj.h, PropertyGeo.h (1221) and Gui/MetaTypes.h (655).
  No single umbrella dominates more than ~100 TUs: it is a vocabulary type that the TUs really use
  (the include-what-you-use pass below re-adds it to almost every TU it is cut from).
- **`App/GeoFeatureGroupExtension.h`** (163): `OriginGroupExtension.h` (125) <- `App/Part.h`,
  `Part/App/BodyBase.h` <- `PartDesign/App/Body.h`; `App/Datums.h` (63) <- `Origin.h`,
  `OriginFeature.h`; `ViewProviderSketch.h` (24, unused). Its reach is mostly legitimate.
- `Gui/View3DInventorViewer.h` (184, 56 edits): 115 direct; `SoFCUnifiedSelection.h` carries it
  into 59 TUs but only uses `View3DInventorViewer*`. It drags `ViewerContext.h` (32 edits, a base
  class -- unavoidable), Quarter and Coin.
- Renderer: `Gui/Renderer/Renderer.h` (123 TUs, 214 edits, 4085 lines, std-only includes) is cheap
  per edit (25 CPU-min, 4.6 min wall) but edited constantly. 92 of its 214 edits touched only the
  ~30 `*Config` structs at the top. `BGFXRendererP.h` (10979 lines) reaches 11 TUs -- contained.

### 2.3 Heavy third-party families (who lets them in)

| family | TUs | .cpp includes it itself | mean TU s with / without | main gateway (TUs carried alone) |
|---|---|---|---|---|
| Qt | 1826 | 798 | 11.4 / 2.1 | `App/StringHasher.h`, `IndexedName.h`, `MappedName.h` (QByteArray/QHash in the TNP core), `Base/Tools.h` (QString), `Gui/MetaTypes.h` |
| Python | 1654 | 20 | 11.8 / 2.5 | `CXX/WrapPython.h` sole gateway for 1078 TUs (via StringHasher.h, PropertyPythonObject.h, Document.h, Base/Interpreter.h) |
| boost | 1874 | 232 | -- | `Base/FileInfo.h` (filesystem, sole for 194), `PropertyExpressionEngine.h` (graph, 1186), `MappedName.h`, `DynamicProperty.h` (multi_index) |
| xerces | 1542 | 10 | -- | `Base/Parameter.h` sole for 883, `Base/Reader.h` (SAX handler base class) 625 |
| OCCT | 762 | 385 | 12.4 / 4.9 | `TopoShape.h` (601), `PropertyTopoShape.h`, `PartFeature.h` (umbrella for 71), TechDraw `Geometry.h`/`DrawUtil.h` |
| Coin | 640 | 250 | 13.3 / 5.0 | `Gui/ViewProviderDocumentObject.h` (sole for 94; includes `Inventor/SoType.h`), `ViewProviderGeometryObject.h`, `ViewerContext.h`, Quarter |
| bgfx | 1055 (mostly bgfx's own) | 1034 | -- | contained in `Gui/Renderer/`; `BGFXRendererP.h` 11 |

Observations for the direction of the project: Python and Qt reach 1650-1830 TUs because the App
core (StringHasher/MappedName/IndexedName, Property*, Document) carries them -- 1650-1830 of the
2200 fork TUs parse Python.h and QtCore whether or not they touch Python or Qt. That is the main obstacle to
a headless geometry worker that does not link Qt/Python. OCCT is better contained (762 TUs, half of
which include it themselves), Coin is contained to Gui (640), bgfx is fully contained. No fork
header includes a generated `*Py.h` except `Material/Gui/MaterialTreeWidget.h`
(`MaterialFilterPy.h`); generated headers reach 7-400 TUs only through each other and PyImp files,
so editing a `*Py.xml` is cheap (Gui/DocumentPy.h: 7 TUs, 2 CPU-min).

### 2.4 What kind of edits the hot headers get

`churn_classify.py` buckets every changed line by where it sits in the new file. A commit is
"avoidable" when its lines are all comments, private members, inline bodies, includes or Python
wrapper declarations -- with a pimpl, out-of-line bodies and file-local wrappers it would not have
touched the header.

| header | commits | public | protected | signal | py wrapper | private/body/comment only | avoidable |
|---|---|---|---|---|---|---|---|
| Gui/Application.h | 30 | 13 | 0 | 1 | **16** | 0 | **53%** |
| App/Application.h | 12 | 7 | 0 | 2 | 3 | 0 | 25% |
| App/Document.h | 97 | 76 | 13 | 2 | 0 | 6 | 6% |
| Gui/Document.h | 26 | 18 | 1 | 2 | 0 | 5 | 19% |
| App/FileBlobManager.h | 34 | 31 | 0 | 0 | 0 | 3 | 9% (but 17 touched only the manager class, see P2) |
| PropertyTopoShape.h | 17 | 12 | 0 | 0 | 0 | 5 | 29% |
| Renderer.h | 214 | 205 | 0 | 0 | 0 | 9 | 4% (92 touched only config structs) |
| BGFXRendererP.h | 167 | 134 | 0 | 0 | 0 | 33 | 20% |
| RenderParams.h | 104 | 84 | 0 | 0 | 0 | 20 comment-only | 19% |

Conclusion: for the App core, a pimpl would save almost nothing -- the edits are new public API.
The wins are (a) shrinking reach and (b) splitting headers along the line where edits actually
land (Python wrappers, the blob manager class, renderer config structs). Note also that
comment-only edits rebuild everything too: 6 of App/Document.h's 97 commits were comment-only
(6 x 30 min wall).

## 3. Proposals, ranked by measured saving

Each proposal was simulated on the real per-TU graphs (`include_graph.py whatif`): the edge is
cut, reachability recomputed, and then -- the include-what-you-use correction -- every TU whose own
.cpp/.h names something a lost header declares (its classes, free functions, macros, enumerators,
distinctive member names; Qt/OCCT/Coin headers by class name) gets that header back, as it would
after fixing the compile errors. So the numbers are what remains after the fallout is repaired;
they are estimates, conservative for heavily-named headers (`Document` matches both App and Gui).
"Saving" = 12-month churn-weighted rebuild CPU saved over all headers whose reach shrinks; wall
~ CPU/6. "Per edit" = the cost of touching the named header before -> after.

| # | change | reach change (TUs) | per-edit cost of that header | CPU-h / 12 mo saved (wall h) | risk |
|---|---|---|---|---|---|
| **P1** | **Gui/Document.h diet**: include `View.h` not `MDIView.h`; forward-declare `App::Document`; `ViewProviderFeaturePython.h` moves its one `getDocument()->toggleInSceneGraph` call out of line and drops `Gui/Document.h`; `ViewProviderSketch.h` drops `App/Part.h`, `Gui/Document.h`, `BodyBase.h`; remove stale `#include <Gui/Document.h>` from .cpp (C01) | Gui/Document.h 406 -> 244; App/Document.h 712 -> 587; MDIView.h 459 -> 181; Tree.h 467 -> 222 | Gui/Document.h 104 -> 67 CPU-min; App/Document.h 178 -> 149 | **86** (14) | low-medium: upstream headers, one-line include edits; .cpp that relied on the transitive App/Document.h need it added (254 TUs matched `getDocument()->` and were kept) |
| **P2** | **Split `App/FileBlobManager.h`**: blob value types (`FileBlob`, `FileBlobHandle`, `BlobReferrer`, `BlobReferrerProperty`, `BlobRecorder`, lines 1-340) into `App/FileBlob.h`; the `FileBlobManager` class stays. Properties include FileBlob.h | manager header 1265 -> **49** | manager edits 285 -> 13 CPU-min (48.8 -> 2.5 min wall) | **77** (13): 17 of 34 edits touched only the manager class | low: fork-owned code, no upstream counterpart |
| **P3** | **`PropertyMaterial.h` drops `Materials.h` and `Base/Reader.h`** (it already forward-declares `Material`; `XMLReader` only by reference). With `MaterialValue.h` / `QuantitySpinBox.h` including `Base/Quantity.h` instead of `Gui/MetaTypes.h` (C02) | Materials.h 515 -> 57; MetaTypes.h 658 -> 64; Reader.h 628 -> 355; Writer.h 652 -> 387 | Materials.h 126 -> 11 CPU-min | **62** (10); also -274 MB parsed per full build | low: Material module diverged already; MetaTypes removal also fixes an App->Gui layering violation (upstream has it too) |
| **P4** | **Move `PropertyAppearance` / `PropertyAppearanceList` out of `PropertyStandard.h`** into `App/PropertyAppearance.h` (the 15 headers that use them include it) | AppearanceList.h 1228 -> 664; FileBlobManager.h 1265 -> 1003; MaterialAppearance.h 1312 -> 1089; COWData.h 1233 -> 830 | PropertyStandard.h itself keeps its 1227 TUs, but stops dragging AppearanceList/COWData/blob code into ~560 of them; appearance-class edits then reach only users of the 15 headers that name them (not measured) | **46** (8) | low-medium: the appearance properties are fork-only; PropertyStandard.h is upstream's file, but the block is fork-added, so the split shrinks the diff against upstream |
| **P5** | **Python wrappers out of `Gui/Application.h`**: the 85 `static PyObject* sX` + `Methods[]` become file-local in `ApplicationPy.cpp` (as `6cac2592a1` already did for doCommandEval/doCommandSkip). Same for App/Application.h's 39 | none (frequency, not reach) | 16 of 30 edits avoided | **33** (5.5) | medium merge cost: upstream adds wrapper declarations to this block often; each port then resolves a conflict by dropping the line. Zero-cost variant: leave upstream's existing ones, make every fork-added one file-local (convention) |
| **P6** | remove stale `#include <App/Document.h>` from .cpp files that name nothing in it (I01) | 712 -> 661 | 178 -> 165 | 23 (4) | none (.cpp only) |
| **P7** | `ActiveObjectList.h` includes `TreeItemMode.h`, not `Tree.h` (P01) | Tree.h 467 -> 49; DockWindow.h 470 -> 57; Selection.h 589 -> 485 | Tree.h 120 -> 15 CPU-min | 21 (3.5) | low: one line in an upstream header; .cpp that used TreeWidget through it add the include |
| **P8** | `SoFCUnifiedSelection.h` forward-declares `View3DInventorViewer` (P13) | View3DInventorViewer.h 184 -> 145; ViewerContext.h 194 -> 165 | viewer 51 -> 41 CPU-min | 20 (3.3) | low |
| **P9** | `hash_combine` and the small math templates out of `Base/Tools.h` into a dependency-free header; `TopoShape.h` and `InventorBase.h` include that (P14) | Tools.h 1043 -> 659 | 235 -> 159 | 17 (2.8) | low-medium: Tools.h is upstream's; add a new header and keep Tools.h including it |
| **P10** | Split `Gui/Renderer/Renderer.h`: the ~30 `*Config` structs into `RenderConfig.h` | config edits reach 123 -> ~72 TUs | 25 -> 17 CPU-min | ~13 (2) | none (fork-only) -- small because the renderer TUs that name configs are most of them |
| P11 | `Command.h` forward-declares `Gui::Application` (callers of `FCMD_SET_EDIT` include it) | Gui/Application.h 491 -> 446 | 122 -> 112 | 5.6 | low |
| P12 | `App/ElementMap.h` drops its unused `Application.h` | App/Application.h 1425 -> 1288; Parameter.h 1504 -> 1407 | 298 -> 276 | 5.4 | very low |
| P13 | `DocumentObject.h` forward-declares `Placement`/`PropertyPlacement`, drops `PropertyGeo.h` | PropertyGeo.h 1224 -> 925; Placement.h 1473 -> 1300; Vector3D 1609 -> 1574 | 277 -> 210 | 5.0 | medium: upstream core header, many .cpp rely on it transitively |
| P14 | `Base/Observer.h`: Subject's logging/catch via an out-of-line helper; drop `Console.h`/`Exception.h` | Console.h 1773 -> 1530 | 341 -> 306 | 4.8; -163 MB parsed | low |
| P15 | `Base/Exception.h`: `FileException` keeps a path string, forward-declares `FileInfo` | FileInfo.h 1846 -> 1428 | 346 -> 305 | 1.3; **-325 MB parsed** (boost.filesystem out of 418 TUs) | medium: ABI of FileException changes (rebuild-all anyway); upstream file |
| P16 | `StringHasher.h` drops `CXX/Objects.hxx` (uses only `PyObject*`) | Objects.hxx 1613 -> 1454 | -- | ~0 now (CXX never edited) | low; a step toward Python-free App core |

**All reach proposals together (C03 = P1, P3, P4, P7-P9, P11-P16)**: 269.6 CPU-h / 12 months
saved (10% of the 2740 total), 114 headers' reach shrinks, and a full build parses 1064 MB less
(about 18 CPU-min, 4% -- the compile-speed effect is small; the win is localization). Adding P2,
P5 and P10: **~390 CPU-h / year, ~65 h of wall time at -j 6**.

Not proposed: a pimpl for App::Document / DocumentObject / Property (edits are public API, see
2.4); breaking the `PropertyExpressionEngine` member out of DocumentObject.h (API break across the
tree). Worth considering later for the headless worker: a Qt-free / Python-free App core
(StringHasher/MappedName using std types; `CXX/Objects.hxx` out of App headers) -- large, and it
collides with upstream's TNP code.

Upstream-merge note: every edit to an upstream header (Gui/Document.h, MDIView.h,
ActiveObjectList.h, Command.h, Exception.h, Observer.h, ElementMap.h, DocumentObject.h, Tools.h,
Application.h) makes later ports that touch the same include block conflict, and ported .cpp code
that relied on the removed transitive include fails to compile until it adds the include. Both
are mechanical but recurring. The fork-owned splits (P2, P4's block, P3, P10) have no such cost and
are the best first steps. Of the upstream ones, P1 and P7 give the most per line changed.

## 4. The four headers from the port

**Gui/Application.h** -- 491 TUs, 122 CPU-min, 531 steps, ~21 min wall. The reach is mostly real:
396 .cpp include it directly for `Application::Instance`; Command.h adds 45 that only need a
macro (P11). The member the port needed was a Python function, and that is the problem: Python
wrappers are declared in the header (85 of them), so every new `FreeCADGui.xxx` costs a 491-TU
rebuild -- 16 of the last 30 edits to this header were only that. Minimal change: wrappers are
file-local functions in ApplicationPy.cpp (what `6cac2592a1` did; make it the rule, P5). Private
state goes to the existing `ApplicationP` pimpl. Only genuinely public API needs the header.

**Gui/Document.h** -- 406 TUs, 104 CPU-min, 446 steps, ~18 min wall. Two thirds is direct .cpp
includes (266), so no single cut makes it cheap; the rest is ViewProviderFeaturePython.h (one
inline call) and three module VP headers. Upstream's change was a public member
(`TreeRootObjects`), and putting it in `DocumentPyImp.cpp` was the right minimal move -- the
generated DocumentPy.h reaches 7 TUs (2 CPU-min). In general, Python-facing helpers of
Gui::Document belong in DocumentPyImp.cpp (or a free function in a small header such as a new
`Gui/DocumentTools.h`); private helpers belong in `DocumentP` (already a pimpl). P1 takes the
header to 244 TUs (67 CPU-min, ~11 min wall) and stops it dragging Tree.h, App/Document.h and
MDIView.h into everything that includes it.

**Base/Vector3D.h** -- 1609 TUs, 320 CPU-min, 1780 steps, ~55 min wall. Its reach is legitimate:
it is used by Matrix/Rotation/Placement/BoundBox/PropertyGeo and the TUs name Vector3d
themselves; the best combination of cuts (P3, P13, MetaTypes) only takes it to ~1530. A
`static constexpr` member like `UnitZ` must live in the class, so there is no cheap way to add it.
It is edited about twice a year: keep spelling `Base::Vector3d(0, 0, 1)` as `9c50dd3838` did, or
batch such additions into one deliberate full rebuild (e.g. right after an upstream merge, which
rebuilds everything anyway).

**App/GeoFeatureGroupExtension.h** -- 163 TUs, 45 CPU-min, 193 steps, ~8 min wall. Not actually
expensive: the `using GroupExtension::addObject;` would have cost about 8 minutes of wall time.
It is reached through OriginGroupExtension.h (App/Part.h, BodyBase.h -> PartDesign Body.h) and
Datums.h; ViewProviderSketch.h adds 13 TUs with includes it does not use (P1 removes them).
Recommendation: make such one-line changes directly; reserve workarounds for headers above
~400 TUs / 15 min.

For reference, `Mod/Part/App/PartFeature.h` (467 TUs, ~20 min) costs that much partly because of
what it drags *with* it: P3 removes the whole Material module, MetaTypes and xerces SAX from its
461 TUs, which makes Materials.h/Model.h/MaterialValue.h edits ~10x cheaper; PartFeature.h's own
reach is legitimate (Part features).

## 5. Tooling to keep it from regressing

Scripts (in `scripts/header-audit/`), Python 3 from `.conda\freecad\python.exe`, no
third-party modules:

- `run_audit.sh [BUILD] [OUT] [--baseline old/ranking.csv]` -- dumps `ninja -t deps`, computes
  12-month churn from git, writes `ranking.csv/.txt` and `analyze.json`. Takes about a minute.
- `hdrmodel.py` -- loads build.ninja / .ninja_log / deps into a cached model (TUs, links, AUTOMOC).
- `header_cost.py` -- the ranking (reach, CPU, links, autogen steps, wall at -j N, churn-weighted
  cost). `--baseline` flags headers whose reach grew by more than `--grow` TUs since a saved
  ranking: run after each build/merge, fail review on a surprise jump.
- `include_graph.py analyze` -- direct parents, dominators (umbrellas), third-party gateways.
  `include_graph.py whatif --spec proposals.spec` -- simulate cutting/adding include edges with
  the include-what-you-use correction (`declnames.py`).
- `churn_classify.py` -- what kind of lines past edits changed (public/private/body/py/comment).
- `parse_cost.py` -- fits compile seconds per MB of headers (1.0 s/MB here).
- `renderer_split.py` -- the Renderer.h config-split estimate.
- `proposals.spec`, `proposals_blob.spec` -- the what-ifs above.

Suggested adoption: copy the scripts to `scripts/header-audit/` and run `header_cost.py
--baseline` after merges (a reach jump on a top-100 header is a review item). A rule of thumb for
review, measurable with the ranking: a new `#include` in a header that reaches >300 TUs needs a
reason; prefer forward declarations; Python wrappers and private helpers never go in headers.
Complementary tools: include-what-you-use (needs clang + `CMAKE_EXPORT_COMPILE_COMMANDS`; use its
Qt/boost mapping files; run per directory and review by hand, not wholesale); on this MSVC box,
`vcperf` (C++ Build Insights) or `/d1reportTime` give per-header parse cost, and `/showIncludes`
on one TU shows a chain; with clang, `-ftime-trace` + ClangBuildAnalyzer. Keep PCH off for
development (it is): with PCH, any header in a target's `_PreComp_` list rebuilds the whole
target -- the lists today are external-only (QtAll.h, InventorAll.h, OpenCascadeAll.h), keep them
that way if PCH is ever turned on for release builds.

Caveats: costs come from the latest log entry per step (one build's timing); the include-graph
reconstruction approximates the preprocessor (unknown `#if` branches are taken, but only edges to
headers in the TU's real set count); the IWYU correction is name-based and misses uses through
members of unnamed types (the `getDocument()->` regex covers the main one); churn counts commits,
not builds.
