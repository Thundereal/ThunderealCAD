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

#ifndef PARTGUI_ViewProviderElement_H
#define PARTGUI_ViewProviderElement_H

#include "ViewProvider.h"

class SoCoordinate3;
class SoSwitch;

namespace PartGui
{

/** The view provider of a Part::Element
 *
 * Drawn as Assembly3 draws its elements: a half transparent cyan copy of
 * the geometry, thick lines and points, on top of everything when the
 * element itself is selected. With auto element visibility (the default,
 * Part::Element::autoVisibility()) an element is hidden and so seen only
 * while it is selected or preselected: the on-top selection draws a hidden
 * object too.
 */
class PartGuiExport ViewProviderElement : public ViewProviderPart
{
    using inherited = ViewProviderPart;
    PROPERTY_HEADER_WITH_OVERRIDE(PartGui::ViewProviderElement);

public:
    ViewProviderElement();
    ~ViewProviderElement() override;

    void attach(App::DocumentObject *obj) override;
    void updateData(const App::Property *prop) override;
    void setupContextMenu(QMenu*, QObject*, const char*) override;

    /** Turn auto element visibility on or off
     *
     * Turned on, it hides every element and element link of every open
     * document, as Assembly3's "Auto element visibility" does. Turned off,
     * each keeps the visibility it has, and new ones are made visible.
     */
    static void setAutoVisibility(bool on);

protected:
    /// The color of the copy
    virtual App::Color defaultColor() const;
    void applyStyle();
    /** Where the frame's origin is put by a vertex, in the object's own
     * coordinates: the element's Vertex, or an element link's element's
     *
     * @return false when there is none
     */
    virtual bool getVertexMark(Base::Vector3d &point) const;
    /// Show the vertex mark where getVertexMark() says
    void updateVertexMark();

private:
    /// The vertex, a bigger point in its own color, in every mode
    SoSwitch *pcVertexMark;
    SoCoordinate3 *pcVertexCoord;
};

/// The view provider of a Part::ElementLink: the same, in red
class PartGuiExport ViewProviderElementLink : public ViewProviderElement
{
    using inherited = ViewProviderElement;
    PROPERTY_HEADER_WITH_OVERRIDE(PartGui::ViewProviderElementLink);

public:
    ViewProviderElementLink();

protected:
    App::Color defaultColor() const override;
    bool getVertexMark(Base::Vector3d &point) const override;
};

}  // namespace PartGui

#endif  // PARTGUI_ViewProviderElement_H
