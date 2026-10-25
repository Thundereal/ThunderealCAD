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

Built as step 2 (2026-10-24):

- `App::Part::GroupMode` (Normal, Auto, Link; Normal by default).
  `Part::prepareObjectsToAdd()` puts an owned `App::Link` in place of an
  object, called by `GeoFeatureGroupExtension::addObjects` and, before it
  relinks to the origin, by `OriginGroupExtension::addObjects`; an owned
  link of the part passes through, so the second call is harmless. Never
  linked: aux groups, the origin and its features, and the part's own
  containers.
- `Part::onObjectsRemoved()`, called by `removeObjects`, deletes the owned
  links taken out, in every mode. A link another group takes is a move,
  not a removal: the group taking it says so with
  `GeoFeatureGroupExtension::MovingObjectGuard`, and the link is disowned
  (`_LinkOwner` 0) and kept.
- Not yet: a pick with a subname or a sub-element (the add API takes whole
  objects; this comes with Insert Component), and Auto as Assembly's
  default, which comes with the C++ assembly objects (step 4) so that the
  current Python assembly code is not changed under it.
- Tests: `tests/src/App/PartGroupMode.cpp`, one case per rule above.

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

Built as step 2 (2026-10-24): `App::PartAuxGroup` (owner in `_Owner`), and
`App::Part` as an `AuxGroupOwner`. The Parts group takes every child no
other group of a role takes, so it needs no list of component types, and
an assembly's Joints or Physics group (step 4) narrows it by existing.
`Gui::ViewProviderPart::claimChildren` orders the groups
(`Part::getAuxGroupRank`), and the context menu's "Parts group" toggles
it. Checked in the GUI: Origin, Views, a user's group, Parts, in that
order whatever the order in `Group`; the components under Parts; a link
Auto mode adds goes to Parts.

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
and the vertex is the ElementLink's `Vertex`: a live link, which the
`Offset` follows (user, 2026-10-25). A pick that already gives a frame (a
circular edge, a datum, an LCS) needs no vertex.

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
  `Offset`, `Shape` (sec 4.4), and `Vertex` (user, 2026-10-25):
  - `Vertex` (a Global `PropertyLinkSub` to a vertex, a direct child of the
    container plus a subname) places the frame's origin on the element.
    `Offset` follows it: on each recompute the vertex's move in the
    element's frame since `VertexPosition` (hidden) moves `Offset`'s origin
    by as much, `Offset = T(v) * T(v_old)^-1 * Offset`, and the rest of
    `Offset`, the user's, stays. Clearing `Vertex` takes its share back out.
    A `Vertex` that is not a vertex is an error of the element link.
  - The view provider shows the vertex as a yellow point, 10 px, over the
    link's half transparent red (`PartGui::ViewProviderElementLink`).
- **The element frame** (`Part::findElementFrame`, `Part/App/ElementGeometry`,
  with the curve and surface recognition it reads) is Part's: `Offset` is
  relative to it, and the element link computes it. It was Assembly's
  until the vertex needed it; `Assembly/App/ElementGeometry.h` forwards.
- **Copies follow through what does not change.** In this build a change
  reaches a dependent only through objects that change, and the objects
  between an element and its geometry (an `App::Part`, an Elements group, a
  link) do not. Two things make the copies follow anyway:
  - an element and an element link keep a hidden `_Resolved` link
    (`PropertyXLink`, Global) on what their subname resolves to, kept in
    step when the link is set and on each recompute: the dependency on the
    geometry itself;
  - `App::AuxGroup::_GroupTouched` (an output, not saved) is touched when
    an element's shape changes, and the container's `_GroupTouched` with
    it, as a group's and a link's own do: the shape cache of a subname
    through the group or the container
    (`Part::PropertyShapeCache::slotChanged`) is cleared by any `*Touched`
    property, and a copy taken through it was stale before.
- **Why `PropertyLinkSub`, not `PropertyXLinkSub`.** Both targets are
  always in the same document, because an external part comes in as an
  owned `App::Link` child. `PropertyLinkSub` already has what is needed:
  shadow (mapped) names, label references and their update on relabel,
  `adjustLink`, and `updateElementReference` (`App/PropertyLinks.h:961-1070`).
  What the X adds -- another document, relative paths, the modification
  stamp, `AllowPartial`, version pinning -- belongs to the owned
  `App::Link`, and subnames resolve through it.
- **Global scope** (user, 2026-10-25). `Target` and `Element` copy geometry
  and place nothing, so neither is a coordinate-system link: with Global
  scope the link-scope check and the walks of what moves with what
  (`getCSOutList`, `getCSInList`, `getCSRelevantLinks`) never follow them,
  whatever the element links to.
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

Built as step 3a (2026-10-24), `Part/App/FeatureElement.{h,cpp}`:

- `Part::Element` and `Part::ElementLink`, both `Part::Feature`. Each
  `execute` takes the shape its link resolves to (`Feature::getTopoShape`
  on the link's object and subname) and keeps it with its location in
  `Placement`. `Detach` keeps the copy and skips the target. `Offset` is
  stored on both and composed by the frame computation of the joints
  (step 4); nothing reads it yet.
- `Element::create()` (Python: `Part.makeElement(container, target,
  subname, label)`) makes the container's Elements group if missing,
  puts the element in it first and then labels it, so the label is made
  unique among its siblings (`onBeforeChangeLabel`) and no wider: it allows
  duplicate labels in the document. A new element is hidden; its
  geometry is the target's. `Part.makeElementLink(container, obj,
  subname)` adds an ElementLink to the container's `Group` directly,
  whatever its GroupMode (a joint owns them from step 4).
- The Elements group is an `App::AuxGroup` of role "Elements", which makes
  it owning (`AuxGroup::isOwning()`): its `Group` is what it holds, it is
  no view of the owner's, and deleting it, or the container
  (`OriginGroupExtension::onExtendedUnsetupObject`), deletes the elements.
  `App::Part` and Body hold it in their `Elements` property (scope Child,
  as `Origin`) and make it through `AuxGroupOwner::getElementsGroup()`:
  an `App::PartAuxGroup` for a part, a `PartDesign::AuxGroup` for a body.
- **Coordinate system.** The group and its elements are in their
  container's, as an origin and its features are:
  `GeoFeatureGroupExtension::getGroupOfObject` answers the container for
  them, and `OriginGroupExtension::hasObject` counts them as the
  container's, which the Child-scope `Elements` property needs to pass the
  link-scope check (as `Origin` passes it). Their links are Global (sec
  4.3), so an element is never dragged into the container's `Group` with
  what it links to, and a child moved out does not take its elements
  along. (Until 2026-10-25 the links were Local and the coordinate-system
  walks carried exclusions for elements instead.)
- **Tree.** `ViewProviderOriginGroupExtension::extensionClaimChildren` puts
  the Elements group right after the origin, for a part and a body alike.
  `OriginGroupExtension::extensionGetSubObject` resolves the group by its
  name or label too, as it does the origin, so the path the tree makes
  (`Part.Elements001.Element.`) reaches an element.
- **Display, as Assembly3's** (`asm3/assembly.py:1287-1310`, `:2326-2343`,
  `asm3/gui.py:31-160`, `:712-745`):
  - `PartGui::ViewProviderElement`: the copy in half transparent cyan,
    lines and points 4 wide, `OnTopWhenSelected` "Object";
    `PartGui::ViewProviderElementLink`: the same in red.
  - The owning group draws its elements (`Gui::ViewProviderAuxGroup` gives
    an owning group a child root and claims its children in 3D), and the
    container claims the group in 3D, so an element is in the scene in the
    container's coordinate system and a selection path to it resolves.
  - **Auto element visibility** (the Part preference
    `AutoElementVisibility`, on by default, Assembly3's default too): new
    elements and element links are made hidden, and each is seen only
    while it is selected or preselected. Assembly3 does that by toggling
    `Visibility` from a selection observer, and stands down when the core
    draws the selection on top (`asm3/gui.py:78`). This fork's core always
    can: with `ShowSelectionOnTop`, on by default, the render manager draws
    a selection and the preselection highlight with the switch override
    (`SoFCSwitch::setOverrideSwitch`, `Gui/SoFCUnifiedSelection.cpp:930`,
    `Gui/Inventor/SoFCRenderCacheManager.cpp:1249`), which draws a hidden
    object too. So nothing toggles `Visibility`, and no document is
    modified by looking. Off: new ones are made visible, and each is shown
    and hidden by hand. The element's context menu has the switch;
    turning it on hides every element and element link, as Assembly3's
    command does.
  - Not yet: Assembly3's "Show element coordinate system" (a coordinate
    cross at the element's frame), and the pick commands.
- **The Elements task panel** (user, 2026-10-25), `Part_EditElements`,
  `Part/PartElements/TaskElements.py`. Tree and 3D selection are kept in
  step, so re-pointing an element by drag and drop means scrolling the tree
  back and forth; the panel lists the container's elements instead, and
  takes the 3D selection as it is:
  - Add declares an element on each selected geometry, Replace gives the
    element selected in the list the selected geometry, Remove deletes the
    selected ones; a label is edited in place (made unique among the
    siblings). One transaction: Cancel undoes it all.
  - A pick is stored as an element stores it: the last direct child of
    the container on the path (a body's feature, also when picked through
    a solid body) and the rest of the path, with the element's plain name
    (a selection's mapped name is the shape's it was picked on, the body's
    for a pick on the body's own shape). A pick of a child's element is
    stored whole, `$$Elements.$Label.`.
  - Opened from the command, from an element's context menu, and by a
    double-click or the context menu of the Elements group: core
    `Gui::ViewProviderAuxGroup` runs the command a module registered for
    the group's role (`setEditCommand`), so Gui names no Part command.
  - Tests: `parttests/ElementsPanelTest.py` (7, GUI, in `TestPartGui`):
    add, replace and remove, rename, Cancel and OK, a body's feature and
    its own shape, a child's element, the command from the group. The
    double-click and the context menus are not reachable from Python and
    were not exercised.
  - Verified: the look, the hidden default, both scene paths. The on-top
    drawing of a hidden selection is the core's and is read from the code:
    the render manager's on-top state is not reachable from Python, and a
    count of the old Coin on-top groups stays 0 in render-cache mode even
    for a visible object.
- A body's Elements group in an older build: that build has no
  `Part::Element` and refreshes a `PartDesign::AuxGroup` from the body's
  `Group`, which this one is not in, so it empties the group. Nothing an
  older build can do with elements is lost by that.
- Tests: `parttests/ElementTest.py` (13) and
  `PartDesignTests/TestBodyElement.py` (4): the group and its property,
  `$$Elements.` with the group relabelled, copies at the right place and
  following their target, `Detach`, labels unique among siblings only,
  re-export through an owned link, an ElementLink to a child's element and
  to a convenience element, relabel updating the ElementLink, deletion,
  save and restore, and a body's element recomputing with no cycle.

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
- **A body with no SolidBody** (one solid, or `AddSolidBody` off) has no
  views, and its Elements group shows every element.

Built as step 3b (2026-10-24):

- `SolidBody` is an `App::AuxGroupOwner`. Its view is an
  `App::PartAuxGroup` of role "Elements" in the body's `Group`, right
  behind the solid body, held by the solid body's hidden `_ElementsView`.
  The link is what the tree needs: a child claimed by a sibling leaves the
  parent's level only if the sibling links to it (`filterLinksByScope`).
  Being hidden, it adds no dependency.
- `AuxGroupOwner` gains `getAuxGroupSource()` (what a group shows a part
  of; the owner's `Group` by default, the body's Elements group for a
  solid body), `hasAuxGroup()` (whether the group is still the owner's)
  and `onElementsChanged()`. A group of role "Elements" is owning only if
  its owner holds it (`getElementsGroup()`); any other is a view.
- `Body::syncElementViews()` makes, refreshes and removes the views. It
  runs at the end of `syncSolidBodies()`, when the Elements group is made
  or goes, and on `onElementsChanged()`, which the owning group calls when
  its children change and an element calls when its `Target` changes. A
  solid body refreshes its view when its features change, and takes it
  along when it goes.
- Tests: `PartDesignTests/TestBodyElement.py`, `TestSolidBodyElements`
  (8): no views without elements, each solid showing its elements, an
  element of no solid staying with the body, a retarget moving the
  element, views going with their solid body and with the Elements group,
  the whole body removed, and undo and redo. Checked in the GUI: the body
  lists Origin, its Elements group (the datum's element only), then each
  solid body, which lists its Elements view and then its features.

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

### 7.1 The joint (step 4a)

- **`Assembly::Joint`**, an `App::DocumentObject` with
  `App::SuppressibleExtension` (`Suppressed`, as the Python joint has).
  `Assembly::JointPython` lets a Python view provider and task panel attach
  to it, so the joint's markers (`JointObject.ViewProviderJoint`) keep
  working while the data is C++.
- **Properties, upstream's names where the meaning is upstream's**:
  `JointType` (the same enumeration and order as `JointObject.JointTypes`),
  `Distance`, `Distance2`, `Angle`, the limits (`EnableLengthMin` ...
  `AngleMax`), and `Placement1` / `Placement2`, the two joint coordinate
  systems relative to each side's component, as the Python joint's are
  relative to their object, output only (the markers draw them; nothing
  reads them back). `ElementLink1` / `ElementLink2`
  replace `Reference1/2`, `Offset1/2` and `Detach1/2`: each is a
  `PropertyLink` to a `Part::ElementLink`, whose own `Offset` is the
  side's whole offset (user, 2026-10-25): the picked vertex's share (the
  link's `Vertex`, which it follows, sec 4.3) and the user's offset in one
  placement. The joint task's offset and rotation edit it (step 4b); an
  upstream joint converts as sec 8 says. The frame of a side is the
  element's frame, then the offsets of the elements down the chain (the
  deepest first), then the element link's `Offset`.
- **Ownership: flat, as everything in the container** (sec 3). The element
  links are direct children of the assembly, so they are in its coordinate
  system and link to their siblings, with no special case in
  `GeoFeatureGroupExtension` (an object in a plain group inside a Part is a
  direct child of the Part too, which is what makes that work). The joint
  holds them: it deletes them when it is deleted, its view provider claims
  them, and the assembly's tree leaves out the element links a joint holds.
- **A side resolves to** (`Assembly::JointSide`): the moving component (a
  direct child of the assembly: the element link's `Element` object, or,
  for a convenience element in the assembly's own Elements group, that
  element's `Target` object); the element's shape in the assembly's
  coordinate system (resolved live through the component, the element
  link's stored copy when that fails, sec 4.4); and the frame on it.
- **The frame** is `UtilsAssembly.findPlacement`'s, ported to C++ on a
  shape (`Assembly::findElementFrame`): the same rules for a vertex, a
  line, a circle, another planar curve, a plane, a cylinder, a cone, a
  sphere, a torus, and a whole object. The vertex upstream picks with a face
  is the element link's `Vertex` (sec 4.2), so the frame needs no second
  sub-element. (`Part::findElementFrame` since the vertex; sec 4.3.)
- **The builder reads sides, not properties.** `AssemblyObject` asks
  `JointSide` for a joint's component, element and frame. A Python joint
  answers from `Reference1/2` and `Placement1/2` as today, so both kinds
  solve side by side until the converter (step 4c) retires the Python one.
- **Making one from picks** (`AssemblyObject.makeJoint(type, pick1,
  pick2)`, a pick being `(component, subname)` as a selection gives it):
  - the component declares elements (an `App::Part`, a Body, an assembly,
    or a link to one): the element is found or made in its Elements group
    and the link goes `(component, "$$Elements.$Label.")`;
  - otherwise a convenience element `_Element` is made in the assembly's
    own Elements group and the link goes `(Elements, "$_Element.")`;
  - a second sub-element in the pick (upstream's face plus vertex) becomes
    the element link's `Vertex`, and its `Offset` follows it.
  Grounding and rigid groups stay as they are in this step: they name whole
  components, not geometry.

Built as step 4a (2026-10-25):

- `Assembly::Joint` and `Assembly::JointPython` (`App/Joint.h`), with
  `Assembly::findElementFrame` and `Assembly::JointSide`;
  `AssemblyObject.makeJoint(type, pick1, pick2)` in Python, and on the
  joint `getComponent(side)`, `getFrame(side)` (the frame in the
  assembly's coordinate system, resolved now), `setSide(side, component,
  subname, vertex)` and `clearSide(side)`. An element link a side lets go of
  is deleted unless another joint uses it, and with it the convenience
  element it used when no other element link does.
- **The reference helpers answer for both kinds** of joint. They keep the
  Python joint's property names ("Reference1", "Reference2") as the names
  of the sides (`typedJointSide`): `getMovingPartFromRef` gives the side's
  component, `getObjFromJointRef` and `getLinkedObjFromRef` its element
  link, `getElementFromProp` names the element link's copy ("Face1"), so
  the Distance classification, the radii and the refusals read the copy.
  Branches of their own: the side placement, the marker
  (`handleOneSideOfJoint`), rack and pinion, `swapJCS`, the current value,
  and the joint lists (`getJoints`, the malformed check, `JointGroup`). The
  by-name readers of the type, the distances, the angle, the limits and
  `Suppressed` read the C++ joint's properties unchanged.
- **A copy goes stale when a solve moves its component.** A solve writes
  placements and purges the touch (`setNewPlacements`), so nothing
  recomputes after it. Two things follow:
  - a side is resolved through a convenience element's target, not
    through the element's copy, which is where the component was;
  - the copies that follow a component in the assembly's coordinate
    system (its element links, the convenience elements on it) are moved
    with it where the solve writes its placement
    (`Joint::relocateCopies`): a relocation of the same shape, as sec 4.3
    has it. `Placement1/2` are relative to the components for the same
    reason.
- Found on the way: an Elements group made the part's link-scope check
  warn ("Link(s) to object(s) 'Elements' go out of the allowed scope") on
  every recompute, for a plain `App::Part` too, since step 3.
  `OriginGroupExtension::hasObject` now counts the Elements group and its
  elements as the container's, as it counts the origin.
- Not yet: cam joints (`CamJoint.cpp` reads `Reference1`), C++ joints in a
  flexible sub-assembly (`AssemblyLink` mirrors joints by property name),
  the markers (4b), and a whole-object side on `App::Line` (upstream's
  axis roles).
- Tests: `AssemblyTests/TestElementJoint.py` (15): the sides for plain
  parts and for a part that declares elements, reuse of an element, the
  frames of a face, a circle and a cylinder, a vertex as the offset and
  a user's offset composed with it; Fixed, Revolute (the same pose as the Python
  joint from the same start) and Distance solved; malformed when both
  sides are one part; the sides from Python; deleting the joint deletes
  its element links and an unused convenience element; saved and
  restored.

## 8. Upstream files and commits

- **Opening upstream 1.0 and 1.1 files**: a converter runs on restore and
  replaces upstream's Python joints, groups, simulations and steps with
  these objects. Each joint side becomes an element in the container that
  holds the geometry plus an ElementLink with the vertex as its `Offset`
  (sec 4.2). This is the importer of `docs/AssemblyFromAsm3.md` B pointed at
  upstream's files; Assembly3 files map 1:1 onto these objects.
  - **A side converts to one offset** (user, 2026-10-25: the joint's
    offsets fold into the element link's). Upstream's side is a
    reference `[component, [element, vertex]]`, `PlacementN` (the frame
    relative to the referenced object, `findPlacement`'s or, with
    `DetachN`, the user's own) and `OffsetN`. The converter takes upstream's
    frame as it is, `G = global(referenced object) * PlacementN *
    OffsetN`, makes the element link from `(component, element)`, sets its
    `Vertex` to upstream's vertex, `VertexPosition = F^-1 * vertex`, and
    `Offset = F^-1 * G`, with `F` the element's frame (`findElementFrame`)
    in the same coordinate system. So every joint keeps its frames
    exactly, whatever upstream's rules, the offsets and the detaching made
    them, a re-solve moves nothing, and the vertex stays live: when the
    part changes the origin follows it, as upstream's does.
  - Not set for a side whose frame upstream does not take from the vertex:
    Distance and cam joints (`ignoreVertex`), a detached side (`DetachN`,
    fixed on upstream's side too, and so relative to the element here),
    and a pick whose vertex is the element itself (a vertex, or a circle's
    centre).
- **Opening Assembly3 files** (user, 2026-10-25): Assembly3's own code is
  carried in the tree to do the migration, so the add-on need not be
  installed, and it loads lazily.
  - `src/Mod/Assembly/Assembly3/asm3` is the add-on's `freecad.asm3` at
    `773b977`, without its workbench (`init_gui.py`), its pip prompt and
    its translations. `Assembly3/README.md` lists the few changes: no
    solver prompt, no auto-solve, and the module-level GUI calls guarded
    so that it imports without a GUI.
  - It is on no search path, so FreeCAD's startup scan of the `freecad`
    namespace does not see it. `AssemblyAsm3Bundle.install()`, from the
    module's `Init.py`, appends a finder to the end of `sys.meta_path`
    that answers `import freecad.asm3` with the copy. An installed
    Assembly3 is found by the normal finders first and wins.
  - So nothing of it is imported until a document restores an Assembly3
    proxy. The copy is under a Mod directory, which is what the proxy
    restore checks before importing a module a document names
    (`App/PropertyPythonObject.cpp`, `proxyModuleAllowed`).
  - Its value to the migration: Assembly3's restore code runs, so the
    three generations of element references (`docs/AssemblyFromAsm3.md`
    B.4) are already normalized by Assembly3 itself, and its objects answer
    `getElementSubname`, `getInfo` and the rest. The converter reads them
    through that API rather than re-deriving them from raw properties.
  - The code is GPL-3.0, as the add-on is (`Assembly3/LICENSE`).
- **Saving for upstream** needs an exporter, built only if wanted.
- **Upstream's later Assembly commits** are read and ported by hand for the
  converted objects. Task panels and commands that stay Python can still be
  taken as they come where they apply.

## 9. Build order

1. `App::AuxGroup` and `Gui::ViewProviderAuxGroup`, with Body moved onto
   them; no behaviour change. Tests: the existing PartDesign suites, and an
   old Body file with Sketches, Datums and Misc round-tripped. Done
   2026-10-24.
2. `GroupMode` on `App::Part`, with gtests for each case of sec 2, and
   its aux groups, the Parts group optional (sec 3.1). Done 2026-10-24.
3. `Part::Element` and `Part::ElementLink`, the Owning aux group, and
   Elements on `App::Part` and Body (done 2026-10-24, sec 4.3), with the
   SolidBody views (done 2026-10-24, sec 5.3).
4. The C++ assembly objects, the solver builder on them, and the
   converters. In parts, each kept green against the existing suites:
   - 4a. `Assembly::Joint` and the builder reading joint sides (sec 7.1),
     with the Python joint still solving beside it.
   - 4b. The joint task panel and commands making C++ joints from picks,
     with the markers on them; grounding and rigid groups as C++ objects.
   - 4c. The upstream converter on restore (sec 8), which retires the
     Python joint and the builder's property reads.
   - 4d. Simulation, motion, exploded view and snapshot as C++ objects,
     with the same converter.
   - 4e. Auto as Assembly's group mode; Joints, Simulations and Views as
     aux groups of the assembly in place of `JointGroup`, `SimulationGroup`
     and `ViewGroup`.
   - 4f. The Assembly3 converter, on the bundled Assembly3 code.
   Physics items come with the dynamics phase (`docs/AssemblyDynamics.md`),
   none exist yet.
5. Partial loading through element links (sec 4.4), Freeze with the Parts
   group's compound (sec 3.1), and repair (sec 4.5).

## 10. Open questions

None. The last one, a Body's element on its own result, is settled by
decision 12 (sec 5.2).
