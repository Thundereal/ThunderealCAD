# Assembly: the object model in C++, elements, and aux groups

Status (2026-10-24): design, nothing built. Branch `AssemblyPort`. It
extends `docs/AssemblyAudit.md` (decision 8) and corrects the Element
verdict of `docs/AssemblyFromAsm3.md` A.1.

Sources:
- the Assembly3 wiki (https://github.com/realthunder/FreeCAD_assembly3/wiki,
  pages Concepts, Replacing Part, Constraints and Solvers, How to Handle Large
  Scale Assembly, Common Operations, Link; last commit `81461e9`, 2023-12-12);
- the Assembly3 clone at `D:\works\sw\FreeCAD_assembly3` (`773b977`);
  `asm3/...` below is under its `freecad/asm3/`;
- `PartDesign::AuxGroup` in this tree.

## 0. Decisions (user, 2026-10-24)

1. **The assembly object model is C++.** The container, elements, element
   links, joints, simulations, motions, Physics items and exploded steps are
   C++ document objects. Task panels and commands may stay Python. Upstream's
   Assembly commits stop being taken as they come for these objects: they
   become a source to port by hand (sec 8). This narrows audit decision 1.
2. **The container is `App::Part`, given group modes** like LinkGroup's
   (sec 2). `AssemblyObject` stays an `App::Part`, so its type name, Origin
   and BOM fields are kept.
3. **Ownership follows Body and its aux groups**: the container's `Group`
   owns its children flat, and aux groups show them sorted (sec 3).
4. **Elements and ElementLinks as Assembly3 has them** (sec 4): an Element
   is an interface declared by the container that holds the geometry, for
   its parents to connect to; an ElementLink is how a container uses an
   element of a direct child, or its own, to form a joint.
5. **The Elements group owns its children.** It is the label namespace
   `$Label` resolves in.
6. **An ElementLink stores the shape too**, unlike Assembly3's, so a joint
   is self-contained and a child need not be loaded (sec 4.4).
7. **Body and App::Part declare interfaces as well as Assembly** (sec 5),
   using the same aux group.
8. **Generalize `PartDesign::AuxGroup` for every container** (sec 6).
9. **Auto is Assembly's group mode by default** (sec 2); Insert Component's
   "always link" stays as an option of that command.
10. **A pick on a child that cannot be edited falls back** to a convenience
    element in the picking container, and says so (sec 4.1).
11. **`$$Name.` in a subname steps through the link property `Name`** of
    the object it is applied to (sec 6.3). The Elements group is reached as
    `$$Elements.`, held by a property as the Origin is.
12. **Which shape a Body's element takes is the user's choice** (sec 5.2):
    any child shape of the Body can be a target, with no machinery to favour
    or repair one kind.
13. **The Parts group is optional, off by default** (sec 3.1): components
    sit directly under the assembly unless the user asks for the group.
14. **A frozen assembly always has a Parts group**, which stores the
    compound shape of all its components (sec 3.1).
15. **A Body with SolidBodies shows each solid's elements in that
    SolidBody** (sec 5.3): they are grouped automatically by the solid that
    owns the feature they link to. The Body's Elements group still owns
    them all.
16. **The groups come first, right after Origin, and Parts is the last of
    them** (sec 3): Origin, Elements, Joints, Physics, Simulations, Views,
    then Parts when there is one, then any component not in a group.

## 1. The tree

```
Rotor                       Assembly : App::Part, GroupMode = Auto   (Rotor.FCStd)
  Origin
  Elements                  owns its elements; Rotor's interface and label namespace
    HubFace       -> (Dome, "Face12")                declared for Rotor's parents
    _Element      -> (Dome, "Edge7")                 convenience: Dome has no Elements
    ShaftEnd      -> (Axis, "$$Elements.$PinEnd.")    re-exported upward, renamed
  Joints                    aux view
    Revolute001             a joint owns its ElementLinks
      ElementLink     -> (Elements, "$_Element.")      Shape copy, Offset
      ElementLink001  -> (Axis, "$$Elements.$PinEnd.")  straight into the child's interface
  Physics                   aux view; shared items
    Spring001
      ElementLink     -> (Elements, "$HubFace.")
      ElementLink001  -> (Bracket, "$$Elements.$Bore.")  a Body's own interface
  Simulations               aux view
    Study001 [Dynamic]      owns its own items; Elements = the Physics items it uses
      Motion001             Joint = Revolute001 (a whole joint, plain link)
      Force001
        ElementLink   -> (Elements, "$_Element.")
  Views
    Exploded View           owns its steps
      Move                  Components = [Bracket]
  Dome                      direct child: a plain part in this document
  Axis                      owned App::Link -> Axis (an assembly in Axis.FCStd)
  Bracket                   owned App::Link -> Bracket.FCStd#Body

Axis                        Assembly in its part role, Freeze = true
  Origin
  Elements
    PinEnd        -> (Pin, "$$Elements.$End.")
  Parts                     aux view, the last group; required when frozen:
                            Shape = compound of the parts
    Pin                     owned App::Link -> Pin.FCStd#Pin (an App::Part)

Bracket                     PartDesign::Body with two solids (Bracket.FCStd)
  Origin
  Sketches  Datums  Misc    aux views, as today
  Elements                  owns every element; shows those of no solid
    Mount         -> Sketch, "Edge3"
  Solid                     PartDesign::SolidBody
    Elements                aux view: elements whose target is a feature of this solid
      Bore        -> Pad, "Face4"                 (sec 5.2)
    Pad  Pocket ...
  Solid001                  PartDesign::SolidBody
    Elements
      Tab         -> Pad001, "Face2"
    Pad001 ...
```

## 2. The container: App::Part with group modes

`App::Part` gets `GroupMode`. It decides what happens when an existing
object is added; objects the container creates itself (elements, joints,
studies) are always direct children.

| mode | adding an existing object | removing it |
|---|---|---|
| Normal (the default; today) | moved in, taken out of its old geometry group | moved back out |
| Auto (Assembly's default) | moved in, or an owned link when one of the cases below holds | owned link deleted; a direct child moved back out |
| Link | always an owned `App::Link` (`_LinkOwner` = the container) | owned link deleted |

Auto makes a link when the object is in another document, when another
geometry group owns it (a Part, a Body), when it is already a child (a second
instance), or when the pick is a subname or a sub-element. The first, third
and fourth are LinkGroup's rule (`App/Link.cpp:2429`); the second is the one
`App::Part` needs, because it cannot share a child the way a LinkGroup can.

For the record, LinkGroup's own modes (`App/Link.cpp:198`, `:2516`):
"Auto Delete" links only when needed and deletes whatever child is removed;
"Auto Link" always links and keeps the link on removal; "Auto Unlink" always
links and deletes only its own links. Link mode above is "Auto Unlink".

Rules:
- **Dependencies dragged along.** Adding an object today also moves the
  objects its coordinate system depends on
  (`recursiveCSRelevantLinks`, `App/GeoFeatureGroupExtension.cpp:177`). In
  Auto, if that would take anything out of another group, the whole add is
  a link instead.
- **A new link starts at the original's global placement**, so nothing jumps.
- **Link mode in one document** leaves the original where it was; it is
  hidden when it sits at the document's top level.
- **Replacing a component** is re-pointing its owned link (sec 4.5). A
  direct child is first turned into a link ("Replace with link").

Every reference inside the container ends at one of its own children: an
element links to a child (a direct part or an owned link) with a subname, a
joint links to its own element links. All are local links; only an owned
link's `LinkedObject` reaches outside, which is the App::Link's business. No
Hidden links and no cross-document links are needed in joints or elements.

## 3. Ownership: Body-style, with aux groups

- The container's `Group` owns its children flat: components, joints,
  Physics items, studies, exploded views, and its aux groups.
- Aux groups show subsets sorted by C++ type (Joints, Physics,
  Simulations, Views, and Parts when asked for, sec 3.1), as
  `PartDesign::AuxGroup` shows Sketches, Datums and Misc. They own nothing.
- **The groups come first** (decision 16). The container lists, in this
  order: Origin, Elements, Joints, Physics, Simulations, Views, then Parts
  when there is one, then whatever component is in no group (all of them,
  without a Parts group). The order is the container view provider's
  `claimChildren`, not positions in `Group`, so it holds however `Group` was
  reordered or filled, and a file written in another order shows the same.
  A group that does not exist is skipped; the Elements group is held by a
  property (sec 6.2) and is placed the same way.
- Three things own their children instead:
  - **the Elements group**: it is the label namespace (decision 5);
  - **a study** owns its own items (motions, loads, initial conditions,
    sensors, plots): they mean nothing outside it, and duplicating or
    deleting a study carries them;
  - **an exploded view** owns its steps, for the same reason.
- **A joint owns its ElementLinks**, as an Assembly3 constraint does.
- Physics items stay shared and flat; a study lists the ones it uses in its
  `Elements` link list (`docs/AssemblyDynamics.md` sec 5.1, which this
  changes only in that Physics becomes an aux view).

### 3.1 The Parts group, and a frozen assembly

- **Optional, off by default** (decision 13). Components sit directly under
  the container, as upstream shows them. The container's context menu adds
  or removes the Parts group, as Body's "Toggle auto auxiliary group" does
  for Sketches, Datums and Misc. The group's presence is the setting; there
  is no property to keep in step with it.
- **Mandatory for a frozen assembly** (decision 14). Freezing creates the
  Parts group when there is none, and removing it is refused while frozen;
  unfreezing leaves it, for the user to remove. A frozen assembly:
  - stores the compound of all its components, at their placements, in the
    Parts group's `Shape`, taken when it freezes and again on an explicit
    refresh;
  - skips solving;
  - answers `canLoadPartial` 1 on the assembly and the Parts group, so that
    in a parent document the components are created but not restored, and
    the stored compound is what is shown (Assembly3's Freeze,
    `docs/AssemblyFromAsm3.md` A.5).
- **Why the Parts group holds it.** The assembly is an `App::Part` and has
  no shape of its own. The Parts group is the one object that already
  stands for "all the components", it is restored when they are not, and
  its view provider can draw the compound in their place.
- **The class.** An `App::Part`'s Parts group is a plain App aux group.
  An assembly's is `Assembly::PartsGroup`, deriving from it and adding
  `Shape`, since App cannot hold shapes. Freeze is the assembly's alone.

## 4. Elements and ElementLinks

### 4.1 What they are

The Assembly3 concept, unchanged (wiki, Concepts / Element):

- **An Element is declared by the container that holds the geometry.** It
  links either to geometry of one of its own children (`Dome.Face12`), or to
  an element of a child container (`Axis.$$Elements.$PinEnd.`), which
  re-exports the lower interface upward.
- **Addressed by `$Label`.** Labels are unique among the siblings of one
  Elements group and may repeat across containers, so interchangeable parts
  share an interface by name.
- **One geometry per element**, read flexibly as a point, a line or a plane
  (wiki, Constraints and Solvers / Constraining Geometry Element).
- **An ElementLink is the use site.** A joint in container A holds
  ElementLinks. Each links straight to an element declared by one of A's
  direct children, through that child: `(Axis, "$$Elements.$PinEnd.")`.
  Only a child with no Elements group of its own (a plain feature such as
  `Dome`) gets a convenience element in A's own Elements group (`_Element`
  above), and the ElementLink links to that.
- **Not Assembly3's 0.9 rule.** Since 0.9 Assembly3 always links through a
  local element, so that a copy survives a missing lower element (release
  notes 0.9). Here the ElementLink keeps its own copy (sec 4.4), so that
  indirection is not needed.
- **A pick on a child that declares elements** creates the element in that
  child's Elements group when the face is not declared yet, as Assembly3
  does (wiki, Constraints and Solvers / Constraining Geometry Element);
  that edits the child's document. When the child cannot be edited
  (frozen, partially loaded, read-only document), the element is made as a
  convenience element in the picking container instead, and the report
  view says so (decision 10).
- **Offsets.** An ElementLink's `Offset` affects only its joint; an
  Element's `Offset` affects every user.
- **Repair is in one place.** When a face is renamed, only the deepest
  element is fixed; elements and joints above it stay as they are.
- **Elements are made for you.** A pick in the 3D view creates the elements
  and element links down the hierarchy; named, explicit elements are the
  practice for reuse.

### 4.2 Upstream's joint picks

Upstream picks two sub-elements for one side of a joint, a face and a vertex
(`[Component, ["Body.Face6", "Body.Vertex3"]]`); the vertex only places the
origin on the face. Here one side is one ElementLink to the face's element,
and the vertex becomes that ElementLink's `Offset`. A pick that already gives
a frame (a circular edge, a datum, an LCS) needs no offset.

### 4.3 Classes

C++ in the Part module (they hold shapes; `App::Part` sees them by type
name). Both are plain shape objects, not link objects (user, 2026-10-24):
they own their `Shape`, so nothing needs the geometry of what they point at
at display or solve time.

- `Part::Element`: `Target` (an `App::PropertyLinkSub`: a direct child of
  its own container, plus a subname into it), `Offset`, `Detach`, `Shape`
  (the copy),
  `Placement`.
- `Part::ElementLink`: `Element` (an `App::PropertyLinkSub`: a direct child
  of its container and the subname to an element in that child's Elements
  group, or the container's own Elements group and a convenience element),
  `Offset`, `Shape` (sec 4.4).
- **Why `PropertyLinkSub`, not `PropertyXLinkSub`.** Both targets are
  always in the same document, because an external part comes in as an
  owned `App::Link` child. `PropertyLinkSub` already has what is needed:
  shadow (mapped) names, label references and their update on relabel,
  `adjustLink`, and `updateElementReference` (`App/PropertyLinks.h:961-1070`).
  What the X adds -- another document, relative paths, the modification
  stamp, `AllowPartial`, version pinning -- belongs to the owned
  `App::Link`, and subnames resolve through it.
- **Copies follow their source.** A copy is taken as its source resolves in
  the element's container and is retaken whenever the source recomputes.
  That includes a component that deforms under motion: assembly solving
  recomputes anyway, and a component that only moved costs a relocation of
  the same `TShape`. On disk the shape store strips the top-level location
  (`docs/SharedShapeStorage.md` sec 11.4), so copies of one geometry at
  different places still dedup.
- Both keep `OnTopWhenSelected` on by default, as Assembly3's do.

What Assembly3 took from `LinkBaseExtension` for these two, and where it
goes instead:

| Assembly3 use | here |
|---|---|
| showing the referenced geometry in place (`ViewProviderLinkPython`, `asm3/assembly.py:250`) | a part view provider showing the copy, at the component's current placement |
| link navigation ("Select linked", "Select final"): `getLinkedViewProvider` (`Gui/CommandLink.cpp:923`) | the element view providers override `getLinkedViewProvider` to step to the target. `getLinkedObject` is NOT overridden: `Part::Feature::getTopoShape` follows it (`Part/App/PartFeature.cpp:1235`) and would bypass the copy, which `asm3/assembly.py:598` warns of too |
| a link-array ElementLink for constraint multiplication (`Count`, `PlacementList`, `ShowElement`; `asm3/assembly.py:1898-1903`, `:2210-2248`, `:2541`) | one joint per array instance, each with its own ElementLinks; per-instance offsets then need no second "expand" step |
| re-pointing by drag and drop | the view providers' own drop handlers, as Assembly3's are (`:2363`) |

Not tied to the extension, so kept as they are: `$Label` references and
their update on relabel (`PropertyLinkBase::updateLabelReferences`), the
adjustment of relative links on "Replace with link"
(`DocumentObject::adjustRelativeLinks`), import of links, version pinning
(on the owned `App::Link`, not on elements), and partial loading (sec 4.4).

### 4.4 Shapes on both, and partial loading

Assembly3 stores the copy on the Element only: `AsmElement.execute` keeps
the sub-shape at its part's placement (`asm3/assembly.py:880-912`), while
`AsmElementLink` is an `App::FeaturePython` with no shape (`:2274`) that
re-resolves its chain in every `execute` (`:1955`) and `getInfo` (`:2164`).
In a frozen assembly elements answer `canLoadPartial` 1 (`:628`) and the
constraint group 2 (`:3253`), so a parent can only solve against a child
that is frozen or loaded far enough for the chain to resolve.

Here the ElementLink also stores the shape:

- **It costs almost nothing.** In memory the copy shares the element's
  `TShape`. On disk the shape store keeps what is shared in memory once
  (`docs/SharedShapeStorage.md` sec 11.4; `docs/RoadMap.md`, workstream 2).
  To be checked
  with a round-trip probe when it is built.
- **A joint is self-contained.** The solver, the highlight and the joint's
  display read the ElementLinks alone.
- **A child need not be loaded at all to solve**, not only frozen. Partial
  loading is object- and property-level, not a link-extension feature:
  `DocumentObject::canLoadPartial` is written into the file's dependency
  table (`App/Document.cpp:3822`) and read back to create dependencies
  without restoring them (`:4063-4097`), and an XLink with `AllowPartial`
  opens its document partially and tolerates a missing target
  (`App/PropertyLinks.cpp:4255`, `:5082`). Joints and element links answer
  `canLoadPartial` 1; an element link into an unloaded child keeps its copy
  and reports itself unresolved, not broken.
- **Display is the container's business.** Showing an unloaded child needs
  the child container to keep a frozen shape of its own, in its Parts group
  (sec 3.1); the copies on elements only serve the solver, the highlight
  and the joints.
- **No hash of its own is needed to notice a change.** The link
  dependencies already trigger the recompute: in a session through the
  dependency graph, and across sessions through the owned link's
  `PropertyXLink`, which keeps the linked document's `LastModifiedDate` and
  touches the linking object when it differs (`App/PropertyLinks.cpp:4507`,
  `:5100`).

### 4.5 Repair and replacement

- **The repair order**, per element: the `$Label` path, then the
  topological-naming history (`Part.getRelatedElements` on the shadow name),
  then a geometric search against the element's copy (Assembly3 0.11's
  auto-fix, `asm3/assembly.py:733`). What stays unresolved is reported on
  the element, not on the joints.
- **Replacing a component** (wiki, Replacing Part): re-point the owned link
  to another container that exports the same element labels, and re-solve.
  Elements of the replaced child that were not exported by label go through
  the repair order once.

## 5. Who declares elements

### 5.1 Assembly, App::Part, Body

All three can carry an Elements group (decision 7). An assembly in its part
role (no joints, only an interface) stays available, but wrapping each part
in an assembly is no longer needed: a Body or a Part declares its own.

- **Assembly and App::Part**: an element links to a child with a subname
  (`Box.Face3`), or to a child container's element. No cycle: the
  container depends on its Elements group, which depends on the children.
- **Body**: an element may link to a feature (`Sketch.Edge3`, a datum, an
  early feature), which is the wiki's advice for stable interfaces
  (How to Handle Large Scale Assembly). That is cycle-free like the above.

### 5.2 Which shape a Body's element takes

The user's choice (decision 12). The program provides the capability: an
element's `Target` can be any child shape of the Body -- a sketch, a datum,
a binder, an intermediate feature, a `SolidBody`.

- **Advice, not enforcement.** Pick a feature early in the history. The
  Body's shape and a `SolidBody`'s are moving targets that every later
  feature changes. This goes in the documentation and the command's tool
  tip, not into code.
- **The Body's own shape** cannot be a `Target` link: the Body holds its
  Elements group (sec 6.3), so the link would be a cycle. A user who wants
  it anyway binds it with an expression through `hiddenref()`, which records
  no dependency (`App/Expression.cpp:2777`). The element is then not touched
  when the Body changes; that trade is the user's.
- **No extra machinery.** No result elements kept up by the Body's
  `execute`, and no auto-fix aimed at moving targets. An element on a Body
  is repaired like any other (sec 4.5).

### 5.3 Elements of a Body with SolidBodies

A Body holding more than one solid has a `PartDesign::SolidBody` for each.
Its elements are then shown by solid (decision 15):

- **Ownership does not move.** The Body's Elements group owns every element
  and stays their label namespace, so `(Bracket, "$$Elements.$Bore.")` is
  the same path whichever solid `Bore` is shown in, and stays valid when a
  feature moves to another solid or the body goes back to one solid.
- **Each SolidBody shows an Elements aux view**: the elements whose
  `Target` is a feature of that solid (`SolidBody::hasFeature`). It is a
  Filter group whose source is the Body's Elements group rather than its
  owner's `Group` (sec 6.2), with the SolidBody as owner.
- **Kept automatically.** `Body::syncSolidBodies()`, which keeps the
  SolidBodies in step with the BaseFeature chains, also makes a solid's
  Elements view when the Body has an Elements group, and removes it with
  the solid. A view refreshes when the Elements group's children change,
  when an element's `Target` changes, and when a solid's `Group` changes.
- **The rest stay at Body level.** An element whose target is no feature of
  a solid (a sketch, a datum, a binder) is shown under the Body's Elements
  group, which in the tree lists only those. The Elements group's view
  provider leaves out of `claimChildren` what a solid's view shows.

## 6. Generalizing AuxGroup

### 6.1 What it is today

`PartDesign::AuxGroup` (`PartDesign/App/AuxGroup.{h,cpp}`, 257 lines;
`PartDesign/Gui/ViewProviderAuxGroup.{h,cpp}`, 269 lines) is a plain
`App::DocumentObject` with:
- `Group`: a link list, Hidden, ReadOnly, Output;
- `_Body`: a hidden link to its owner;
- `refresh()`, run on the owner's `Group.signalChanged`: filters the
  owner's `Group` by `isObjectAllowed`, or for a user group ("Add group")
  keeps its curated subset in the owner's order.

Generic already:
- the output `Group` computed from the owner's `Group`;
- the curated mode;
- `claimChildren`;
- visibility through the owner (`Body::isElementVisible` /
  `setElementVisible`, `PartDesign/App/Body.cpp:1275-1300`);
- drag, drop, reorder and delete delegated to the owner's view provider.

Tied to Body:
- `_Body` and `getBody()` cast to `PartDesign::Body`;
- `isObjectAllowed` names PartDesign and Part types;
- the kind comes from the object's name prefix (`Sketches`, `Datums`,
  `Misc`, else a user group), `AuxGroup.cpp:86-100`;
- the view provider touches `Body::_GroupTouched` and calls
  `Body::findBodyOf`;
- the icons;
- the unique-label setting lives in `Part::PartParams`
  (`AuxGroupUniqueLabel`), and its relabel button
  (`Part/Gui/DlgSettingsGeneral.cpp:63`) looks for `PartDesign::AuxGroup`
  by name;
- the groups are created by the Body's context menu
  (`PartDesign/Gui/ViewProviderBody.cpp:210-280`).

Nothing outside PartDesign, Part's preference page and Body uses it.

### 6.2 The generalization

- **`App::AuxGroup`** in `src/App`, and **`Gui::ViewProviderAuxGroup`** in
  `src/Gui`, take the generic parts above.
- **The owner answers through a small interface**, `App::AuxGroupOwner`,
  implemented by Body, `App::Part` (so Assembly) and anything else:
  - which children a group of a given role accepts;
  - what to touch when a group changes (Body: `_GroupTouched`);
  - the visibility of a group as one item.
  A mix-in, not virtuals on `GroupExtension`, so the change does not
  rebuild every file that includes `GroupExtension.h`.
- **`Role`**, a persisted property, names the kind (Sketches, Datums, Misc,
  Parts, Joints, Physics, Simulations, Views, Elements, or a user group).
  When it is empty, as in every existing file, the role comes from the name
  prefix as today.
- **Three modes**:
  - Filter: the output `Group` is computed from the owner's `Group` by role
    (Sketches, Datums, Misc, Parts, Joints, Physics, Simulations, Views);
    or from another group's children when the owner names that source, as
    a SolidBody's Elements view takes the Body's Elements group (sec 5.3);
  - Curated: the user's subset of the owner's children, as Body's "Add
    group" makes today;
  - Owning: the Elements group. Its `Group` is an ordinary link list, its
    children are not in the owner's `Group`, it resolves subnames to them
    including `$Label`, and it keeps their labels unique among siblings.
    The owner holds it through a link property named `Elements`, as an
    `OriginGroupExtension` holds its `Origin`
    (`App/OriginGroupExtension.cpp:69-89`), and paths step into it as
    `$$Elements.` (sec 6.3), so its name and label are free.
- **`PartDesign::AuxGroup` stays as a registered type**, derived from
  `App::AuxGroup`, and keeps its `_Body` property name. Old files restore
  unchanged, and an older build still reads the Body groups of a new file.
- **The unique-label setting** becomes an App parameter read by every aux
  group; the Part preference keeps its checkbox, pointed at it.

Built as step 1 (2026-10-24), with these differences from the plan above:

- **`App::AuxGroup` is abstract** (`App/AuxGroup.{h,cpp}`). The owner link
  property belongs to the derived class, which hands it over with
  `setOwnerProperty()`: `PartDesign::AuxGroup` keeps `_Body`, and a group for
  `App::Part` (step 2) will name its own. One class cannot register the
  property under two names, and a renamed property would cost an older
  build the owner of every Body group in a new file.
- **`App::AuxGroupOwner` has two calls**: `isAuxGroupAllowed(group, obj)`,
  the filter of a role, and `isAuxGroupChild(obj)`, what a curated group
  may take (by default, membership of the owner's `Group`; Body: everything
  `Body::findBodyOf` gives it). The touch needs no call: `_GroupTouched` is
  on `GroupExtension`, so the group touches its owner's. The visibility of a
  group as one item is the group's own (`isChildVisible()`,
  `setChildrenVisible()`).
- **`Role`** is in, and the Body's context menu sets it. A group with no
  `Role` reads it from its name through `getDefaultRole()`, which
  `PartDesign::AuxGroup` answers with the old prefixes.
- **`Gui::ViewProviderAuxGroup`** holds the drag, drop, reorder and delete
  logic; `PartDesignGui::ViewProviderAuxGroup` only adds the icons.
- **Not moved yet**: the unique-label setting stays in `Part::PartParams`,
  read by `PartDesign::AuxGroup::allowDuplicateLabel()`. It moves to App
  with the first owner outside PartDesign.
- Tests: `PartDesignTests/TestAuxGroup.py`. A file written by the staged
  older build (526afd96b3) loads in the new one with the same groups, and a
  new file loads in the older build with the same groups (it ignores
  `Role` and reads the names).

### 6.3 `$$Name.`: a subname step through a link property

Every path into a child's interface needs one step into its Elements group.
By object name it breaks on replacement: the group is `Elements` in one
document and `Elements001` in another. By label (`$Elements.`) it breaks on
a relabel or a translated label. A reserved key would need every resolver to
learn it, and could collide with names in existing files. The step is made
by property name instead:

- **`$$Name.` resolves to the object held by the link property `Name`** of
  the object the step is applied to, then continues from it:
  `Axis.$$Elements.$PinEnd.` is Axis, its `Elements` property's object, then
  the child labelled `PinEnd` there. Any single-object link property works
  (`PropertyLink` and its variants, `PropertyXLink`, `PropertyLinkSub`); a
  list property does not. It works on any object, not only containers.
- **Transform**: the property's object is taken as a child in the owner's
  coordinate system, as `OriginGroupExtension` does for its `Origin`.
- **Existing files keep their meaning.** Before, `$$X.` could only mean an
  object labelled `$X`. The step still means that when the object has no
  single-object link property `X`.

Implemented (2026-10-24) in `DocumentObject::resolvePropertyStep()` and
`getSubObjectByProperty()`, and in every place that reads a `$Label` step:

- **`DocumentObject::getSubObject`** resolves the step before the
  extensions are asked, since `GroupExtension`, `GeoFeatureGroupExtension`
  and `OriginGroupExtension` each read a leading `$` as a label. Every
  override that passes a path with a dot to the base reaches it
  (`Part::Feature`, `Mesh::Feature`, `PartDesign::Feature`, `Body`,
  `SolidBody`, `Split`, `SubShapeBinder`).
- **`FeaturePythonT::getSubObject`** resolves it before the proxy's own
  `getSubObject`, and **`SketchObject::getSubObject`**, which reads paths
  with a dot only against its `Exports`, resolves it there.
- **Peeks skip it.** `GeoFeatureGroupExtension` and `Body` look one step
  ahead for a sibling to collapse; a `$$` step names a property, never a
  sibling.
- **A link** resolves a step through its own property as it does its linked
  object, and passes any other `$$` step to the linked object, which is how
  `Axis.$$Elements.` reaches through an owned link. `getElementIndex` does
  not read a step through its own property as an element.
- **Label references.** `getLabelReferences` still registers `$Elements`
  from `$$Elements.`, which is right for the old meaning and harmless
  otherwise. `updateLabelReference` now matches a label only at the start
  of a step, so relabelling an object labelled `Elements` no longer finds
  `$Elements.` inside `$$Elements.`.
- **Replace and export.** `tryReplaceLink` leaves a step through a property
  alone (the property follows the replacement), and `exportSubName` keeps
  it instead of writing the export name of the object it gives.
- **`SubObjectT::normalize`** keeps the step: it is already the stable form,
  and the object it gives may be no child to name.
- **Expressions** write a `$$Name` step back as `<<$$Name>>`, so it reads
  back as itself and not as the label `$Name`.
- **Gui**: `LinkView::linkGetDetailPath` matches a `$$` step by the object
  it gives, and `ViewProviderLink::getDetailPath` treats a step through the
  link's own property as the linked object when that is what it holds.
- Tests: `tests/src/App/PropertyStep.cpp`.

Still to do when the commands are built: **picks are rewritten into this
form.** A selection in the 3D view or the tree carries object names
(`Axis.Elements001.PinEnd.`); the commands that make elements and element
links store `$$Elements.` and `$PinEnd.` instead.

## 7. Joints, Physics items and studies

- A joint owns two ElementLinks (one per side) instead of `Reference1/2`;
  its other properties follow upstream's joint (type, offsets, limits,
  motion), as typed C++ properties.
- Physics items that pick geometry (springs, bushings, cam contacts) own
  ElementLinks the same way. Study items that pick geometry (forces,
  initial conditions on a part) do too.
- A motion links to its joint with a plain link; an exploded step to its
  components with a plain link list.
- The solver builder reads typed objects, not property names: today
  `Assembly/App/AssemblyObject.cpp` reads the Python objects through 33
  `getPropertyByName("...")` calls, which is how an object with a motion's
  property names came to be taken for a motion.

## 8. Upstream files and commits

- **Opening upstream 1.0 and 1.1 files**: a converter runs on restore and
  replaces upstream's Python joints, groups, simulations and steps with
  these objects. Each joint side becomes an element in the container that
  holds the geometry plus an ElementLink with the vertex as its `Offset`
  (sec 4.2). This is the importer of `docs/AssemblyFromAsm3.md` B pointed at
  upstream's files; Assembly3 files map 1:1 onto these objects.
- **Saving for upstream** needs an exporter, built only if wanted.
- **Upstream's later Assembly commits** are read and ported by hand for the
  converted objects. Task panels and commands that stay Python can still be
  taken as they come where they apply.

## 9. Build order

1. `App::AuxGroup` and `Gui::ViewProviderAuxGroup`, with Body moved onto
   them; no behaviour change. Tests: the existing PartDesign suites, and an
   old Body file with Sketches, Datums and Misc round-tripped.
2. `GroupMode` on `App::Part`, with gtests for each case of sec 2, and
   its aux groups, the Parts group optional (sec 3.1).
3. `Part::Element` and `Part::ElementLink`, the Owning aux group, and
   Elements on `App::Part` and Body, with the SolidBody views (sec 5.3).
4. The C++ assembly objects (joint, simulation, motion, Physics items,
   exploded view), the solver builder on them, and the upstream converter.
5. Partial loading through element links (sec 4.4), Freeze with the Parts
   group's compound (sec 3.1), and repair (sec 4.5).

## 10. Open questions

None. The last one, a Body's element on its own result, is settled by
decision 12 (sec 5.2).
