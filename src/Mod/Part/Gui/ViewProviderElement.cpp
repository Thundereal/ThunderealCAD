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

#include "PreCompiled.h"

#include <cstring>

#ifndef _PreComp_
# include <QAction>
# include <QMenu>
#endif

#include <App/Application.h>
#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoPointSet.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSwitch.h>

#include <Gui/ActionFunction.h>
#include <Gui/Application.h>
#include <Gui/Command.h>
#include <Mod/Part/App/ElementGeometry.h>
#include <Mod/Part/App/FeatureElement.h>

#include "ViewProviderElement.h"

using namespace PartGui;

PROPERTY_SOURCE(PartGui::ViewProviderElement, PartGui::ViewProviderPart)

ViewProviderElement::ViewProviderElement()
{
    applyStyle();

    pcVertexMark = new SoSwitch;
    pcVertexMark->ref();
    pcVertexMark->whichChild = SO_SWITCH_NONE;
    auto sep = new SoSeparator;
    auto light = new SoLightModel;
    light->model = SoLightModel::BASE_COLOR;
    sep->addChild(light);
    auto material = new SoMaterial;
    // Yellow, opaque, against the half transparent copy
    material->diffuseColor.setValue(1.0f, 1.0f, 0.0f);
    material->transparency = 0.0f;
    sep->addChild(material);
    auto style = new SoDrawStyle;
    style->pointSize = 10.0f;
    sep->addChild(style);
    pcVertexCoord = new SoCoordinate3;
    sep->addChild(pcVertexCoord);
    sep->addChild(new SoPointSet);
    pcVertexMark->addChild(sep);
}

ViewProviderElement::~ViewProviderElement()
{
    pcVertexMark->unref();
}

void ViewProviderElement::attach(App::DocumentObject *obj)
{
    inherited::attach(obj);
    for (SoGroup *root : {pFaceEdgeRoot.get(), pFaceRoot.get(), pEdgeRoot.get(), pVertexRoot.get()}) {
        if (root)
            root->addChild(pcVertexMark);
    }
    updateVertexMark();
}

void ViewProviderElement::updateData(const App::Property *prop)
{
    inherited::updateData(prop);
    auto obj = getObject();
    if (!obj || !prop)
        return;
    const char *name = prop->getName();
    if (name && (strcmp(name, "Shape") == 0 || strcmp(name, "Vertex") == 0
                 || strcmp(name, "VertexPosition") == 0))
        updateVertexMark();
}

bool ViewProviderElement::getVertexMark(Base::Vector3d &point) const
{
    auto element = Base::freecad_dynamic_cast<Part::Element>(getObject());
    if (!element || !element->Vertex.getValue() || element->Shape.getShape().isNull())
        return false;
    // VertexPosition is in the frame of the geometry, drawn under the
    // element's own placement
    Part::findElementFrame(element->Shape.getShape())
        .multVec(element->VertexPosition.getValue(), point);
    element->Placement.getValue().inverse().multVec(point, point);
    return true;
}

void ViewProviderElement::updateVertexMark()
{
    Base::Vector3d point;
    if (!getVertexMark(point)) {
        pcVertexMark->whichChild = SO_SWITCH_NONE;
        return;
    }
    pcVertexCoord->point.setValue(float(point.x), float(point.y), float(point.z));
    pcVertexMark->whichChild = 0;
}

App::Color ViewProviderElement::defaultColor() const
{
    // Assembly3's element color
    return App::Color(60.0f/255.0f, 1.0f, 1.0f);
}

void ViewProviderElement::applyStyle()
{
    auto color = defaultColor();
    ShapeColor.setValue(color);
    LineColor.setValue(color);
    PointColor.setValue(color);
    Transparency.setValue(50);
    LineWidth.setValue(4);
    PointSize.setValue(4);
    // On top when the element itself is selected, which is also what shows a
    // hidden element while it is selected
    OnTopWhenSelected.setValue(2);
}

void ViewProviderElement::setupContextMenu(QMenu *menu, QObject *receiver, const char *member)
{
    if (getObject()->isDerivedFrom<Part::Element>()) {
        if (auto cmd = Gui::Application::Instance->commandManager().getCommandByName(
                "Part_EditElements"))
            cmd->addTo(menu);
    }
    auto func = new Gui::ActionFunction(menu);
    QAction *act = menu->addAction(QObject::tr("Auto element visibility"));
    act->setCheckable(true);
    act->setChecked(Part::Element::autoVisibility());
    act->setToolTip(QObject::tr("Hide the elements and show each one only while it is\n"
                                "selected or preselected"));
    func->toggle(act, [](bool checked) {
        ViewProviderElement::setAutoVisibility(checked);
    });
    inherited::setupContextMenu(menu, receiver, member);
}

void ViewProviderElement::setAutoVisibility(bool on)
{
    Part::Element::setAutoVisibility(on);
    if (!on)
        return;
    for (auto doc : App::GetApplication().getDocuments()) {
        for (auto obj : doc->getObjects()) {
            if (!obj->isDerivedFrom<Part::Element>() && !obj->isDerivedFrom<Part::ElementLink>())
                continue;
            if (obj->Visibility.getValue()) {
                // Visibility is view state: no undo step, no modified document
                bool touched = obj->isTouched();
                obj->Visibility.setValue(false);
                if (!touched)
                    obj->purgeTouched();
            }
        }
    }
}

// ------------------------------------------------------------------------

PROPERTY_SOURCE(PartGui::ViewProviderElementLink, PartGui::ViewProviderElement)

ViewProviderElementLink::ViewProviderElementLink()
{
    applyStyle();
}

bool ViewProviderElementLink::getVertexMark(Base::Vector3d &point) const
{
    // The link has no vertex of its own: its element's, at the same place in
    // the frame of the copy, which is the element's moved
    auto link = Base::freecad_dynamic_cast<Part::ElementLink>(getObject());
    auto element = link ? link->getVertexElement() : nullptr;
    if (!element || link->Shape.getShape().isNull())
        return false;
    Part::findElementFrame(link->Shape.getShape())
        .multVec(element->VertexPosition.getValue(), point);
    link->Placement.getValue().inverse().multVec(point, point);
    return true;
}

App::Color ViewProviderElementLink::defaultColor() const
{
    // Assembly3's element link color
    return App::Color(1.0f, 60.0f/255.0f, 60.0f/255.0f);
}
