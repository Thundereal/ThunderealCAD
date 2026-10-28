// SPDX-License-Identifier: LGPL-2.1-or-later

/****************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU      *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ****************************************************************************/

#ifndef PART_FeatureElement_H
#define PART_FeatureElement_H

#include <App/PropertyLinks.h>
#include <App/PropertyGeo.h>

#include "PartFeature.h"

namespace App
{
class AuxGroup;
}

namespace Part
{

/** An interface a container declares: one piece of its geometry
 *
 * An element lives in the Elements group of the container holding the
 * geometry (App::AuxGroupOwner::getElementsGroup()), for the container's
 * parents to connect to, as Assembly3's element is
 * (docs/AssemblyObjectModel.md sec 4). Its Target is a direct child of the
 * container and a subname into it: a face of a part, or an element of a
 * child container ("$$Elements.$Name."), which re-exports it.
 *
 * It keeps a copy of the geometry in Shape, in the container's coordinate
 * system, taken whenever the target recomputes, so that its users need not
 * load the target. Its label is unique among its siblings only, so
 * interchangeable containers can declare the same names.
 */
class PartExport Element : public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Part::Element);
    using inherited = Part::Feature;

public:
    Element();

    /// A direct child of the container, and a subname into it
    App::PropertyLinkSub Target;
    /// A placement of the element's frame relative to its geometry, for every user
    App::PropertyPlacement Offset;
    /// Keep the copy and stop following the target
    App::PropertyBool Detach;
    /** What Target resolves to, through a link to what it links, when not
     * the target itself: hidden, kept in step. A change reaches a dependent
     * only through objects that change, and those between (a part, an
     * Elements group, a link) do not: this is the dependency on the
     * geometry itself.
     */
    App::PropertyXLink _Resolved;
    /// The target is not loaded now (a link to a closed or missing
    /// document): the copy is kept, and serves its users until it is back
    App::PropertyBool Unresolved;
    /// A vertex placing the frame's origin on the geometry: a direct child of
    /// the container and the subname to the vertex. Offset follows it.
    App::PropertyLinkSub Vertex;
    /// Where Vertex was in the geometry's frame when Offset last followed it
    App::PropertyVector VertexPosition;

    App::DocumentObjectExecReturn *execute() override;
    void onChanged(const App::Property *prop) override;
    void onDocumentRestored() override;
    const char *getViewProviderName() const override {
        return "PartGui::ViewProviderElement";
    }
    bool allowDuplicateLabel() const override {
        return true;
    }
    void onBeforeChangeLabel(std::string &newLabel) override;
    /** As its Elements group: the element keeps its copy where the
     * container's children need not be restored, as a frozen assembly's
     */
    int canLoadPartial() const override;

    /// The Elements group holding this element, or null
    App::AuxGroup *getElementsGroup() const;
    /// The container declaring this element, or null
    App::DocumentObject *getContainer() const;

    /** Declare an element in a container
     *
     * @param container: the container, which makes its Elements group if it
     *                   has none
     * @param target: a direct child of the container
     * @param subname: the subname into the target
     * @param label: the label, made unique among the siblings
     */
    static Element *create(App::DocumentObject *container,
                           App::DocumentObject *target,
                           const char *subname,
                           const char *label = nullptr);

    /** Whether elements and element bindings are hidden, and shown only while
     * selected or preselected, as Assembly3's "Auto element visibility"
     *
     * On by default. A new element or element binding is made hidden while it
     * is on, visible while it is off. The setting is the Part preference
     * AutoElementVisibility.
     */
    static bool autoVisibility();
    static void setAutoVisibility(bool on);

    /** The placement of a whole object a subname names, in the coordinate
     * system of \a obj's container
     *
     * @return false when the subname names geometry (an element), or does
     *         not resolve. A whole object -- a link, a datum, an LCS, a body
     *         -- is the frame of its users whether it has a shape or not, as
     *         upstream's joint takes it.
     */
    static bool getWholeObjectPlacement(App::DocumentObject *obj,
                                        const char *subname,
                                        Base::Placement &placement);

    /** Whether an object stands for one not loaded now: a link to an object
     * of a document that is closed or missing, or an object a partially
     * loaded document created without restoring it
     */
    static bool isUnloaded(const App::DocumentObject *obj);
    /** Whether a link property's path stops at an object not loaded now
     * (isUnloaded()), where it would otherwise go on: the geometry is not
     * gone, only not here
     */
    static bool stopsUnloaded(const App::PropertyLinkSub &prop);

    /** Make Offset follow Vertex
     *
     * @param geometry: the target's geometry in the container's coordinate
     *                  system
     *
     * The vertex's move in the geometry's frame since VertexPosition moves
     * Offset's origin by as much; the rest of Offset, the user's, stays.
     * With no Vertex, the last position is taken back out.
     *
     * @return false when Vertex is set and is not a vertex now
     */
    bool updateVertexOffset(const TopoShape &geometry);
    /// The vertex in the container's coordinate system, when Vertex is one
    bool getVertexPoint(Base::Vector3d &point) const;

private:
    void touchGroups();
    /** Find the geometry of Target or Vertex again
     * (docs/AssemblyObjectModel.md sec 4.5): where its name names nothing
     * now, by the naming history, then by a search for its copy; where the
     * component is replaced, by the search alone. A path through labels is
     * the link's own.
     *
     * @return true when the link was changed
     */
    bool repair(App::PropertyLinkSub &prop, const TopoShape &copy, bool replaced);
    /// The vertex where the copy says it was, or null
    TopoShape vertexCopyShape() const;
};

/** How a container uses an element, for a joint of its own
 *
 * Its Element is a direct child of the container and the subname to an
 * element in that child's Elements group ("$$Elements.$Name."), or the
 * container's own Elements group and one of its elements. It keeps a copy
 * of the element's geometry in Shape, in the container's coordinate system,
 * so that a joint is self-contained (docs/AssemblyObjectModel.md sec 4.4).
 */
class PartExport ElementBinding : public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Part::ElementBinding);
    using inherited = Part::Feature;

public:
    ElementBinding();

    /// A direct child of the container and the subname to one of its elements
    App::PropertyLinkSub Element;
    /// A placement of the frame relative to the element's, for this use only
    App::PropertyPlacement Offset;
    /** What Element resolves to, when not the target itself: hidden, kept
     * in step. A change reaches a dependent only through objects that
     * change, and those between (a part, an Elements group, a link) do not:
     * this is the dependency on the geometry itself.
     */
    App::PropertyXLink _Resolved;
    /// The element is not loaded now (a link to a closed or missing
    /// document): the copy is kept, and the joint is solved on it
    App::PropertyBool Unresolved;

    App::DocumentObjectExecReturn *execute() override;

    /** The element whose vertex places this link's origin: the first down
     * the chain of elements (the one linked, what it re-exports) that has a
     * Vertex, or null. The link has no vertex of its own, so that what it
     * refers to is always an element, repaired in one place.
     */
    const Part::Element *getVertexElement() const;

    const char *getViewProviderName() const override {
        return "PartGui::ViewProviderElementBinding";
    }

    /** Add an element binding to a container, made its own child whatever its
     * GroupMode
     *
     * @param container: the container
     * @param obj: a direct child of the container, or its Elements group
     * @param subname: the path to an element of the child, or one in the group
     */
    static ElementBinding *create(App::DocumentObject *container,
                               App::DocumentObject *obj,
                               const char *subname);

protected:
    void onChanged(const App::Property *prop) override;
    void onDocumentRestored() override;
    void handleChangedPropertyName(Base::XMLReader &reader,
                                   const char *typeName,
                                   const char *propName) override;
};

}  // namespace Part

#endif  // PART_FeatureElement_H
