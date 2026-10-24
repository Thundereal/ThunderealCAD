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

#ifndef _PreComp_
# include <QAction>
# include <QMenu>
#endif

#include <App/Application.h>
#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <Gui/ActionFunction.h>
#include <Mod/Part/App/FeatureElement.h>

#include "ViewProviderElement.h"

using namespace PartGui;

PROPERTY_SOURCE(PartGui::ViewProviderElement, PartGui::ViewProviderPart)

ViewProviderElement::ViewProviderElement()
{
    applyStyle();
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

App::Color ViewProviderElementLink::defaultColor() const
{
    // Assembly3's element link color
    return App::Color(1.0f, 60.0f/255.0f, 60.0f/255.0f);
}
