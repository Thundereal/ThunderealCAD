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

    App::DocumentObjectExecReturn *execute() override;
    void onChanged(const App::Property *prop) override;
    const char *getViewProviderName() const override {
        return "PartGui::ViewProviderElement";
    }
    bool allowDuplicateLabel() const override {
        return true;
    }
    void onBeforeChangeLabel(std::string &newLabel) override;

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

    /** Whether elements and element links are hidden, and shown only while
     * selected or preselected, as Assembly3's "Auto element visibility"
     *
     * On by default. A new element or element link is made hidden while it
     * is on, visible while it is off. The setting is the Part preference
     * AutoElementVisibility.
     */
    static bool autoVisibility();
    static void setAutoVisibility(bool on);
};

/** How a container uses an element, for a joint of its own
 *
 * Its Element is a direct child of the container and the subname to an
 * element in that child's Elements group ("$$Elements.$Name."), or the
 * container's own Elements group and one of its elements. It keeps a copy
 * of the element's geometry in Shape, in the container's coordinate system,
 * so that a joint is self-contained (docs/AssemblyObjectModel.md sec 4.4).
 */
class PartExport ElementLink : public Part::Feature
{
    PROPERTY_HEADER_WITH_OVERRIDE(Part::ElementLink);
    using inherited = Part::Feature;

public:
    ElementLink();

    /// A direct child of the container and the subname to one of its elements
    App::PropertyLinkSub Element;
    /// A placement of the frame relative to the element's, for this use only
    App::PropertyPlacement Offset;

    App::DocumentObjectExecReturn *execute() override;
    const char *getViewProviderName() const override {
        return "PartGui::ViewProviderElementLink";
    }

    /** Add an element link to a container, made its own child whatever its
     * GroupMode
     *
     * @param container: the container
     * @param obj: a direct child of the container, or its Elements group
     * @param subname: the path to an element of the child, or one in the group
     */
    static ElementLink *create(App::DocumentObject *container,
                               App::DocumentObject *obj,
                               const char *subname);
};

}  // namespace Part

#endif  // PART_FeatureElement_H
