// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
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
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoSeparator.h>

#include <App/Document.h>
#include <App/GeoFeatureGroupExtension.h>
#include <App/PropertyGeo.h>
#include <Base/Placement.h>
#include <Gui/BitmapFactory.h>
#include <Gui/Command.h>
#include <Mod/Part/App/FeatureElement.h>

#include <Mod/Assembly/App/Physics.h>

#include "ViewProviderPhysics.h"

using namespace AssemblyGui;

PROPERTY_SOURCE(AssemblyGui::ViewProviderMassOverride, Gui::ViewProviderDocumentObject)

QIcon ViewProviderMassOverride::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_MassProperties.svg");
}

PROPERTY_SOURCE(AssemblyGui::ViewProviderTorsionSpring, Gui::ViewProviderDocumentObject)

QIcon ViewProviderTorsionSpring::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_TorsionSpring.svg");
}

PROPERTY_SOURCE(AssemblyGui::ViewProviderSpringDamper, Gui::ViewProviderDocumentObject)

namespace
{

// Whether obj is the component or one of its containers up to the assembly
bool holds(const App::DocumentObject* assembly,
           App::DocumentObject* component,
           const App::DocumentObject* obj)
{
    for (auto* step = component; step && step != assembly;
         step = App::GeoFeatureGroupExtension::getGroupOfObject(step)) {
        if (step == obj) {
            return true;
        }
    }
    return false;
}

}  // namespace

ViewProviderSpringDamper::ViewProviderSpringDamper() = default;

ViewProviderSpringDamper::~ViewProviderSpringDamper()
{
    connChanged.disconnect();
}

void ViewProviderSpringDamper::attach(App::DocumentObject* obj)
{
    ViewProviderDocumentObject::attach(obj);
    auto* coil = new SoSeparator;
    auto* material = new SoMaterial;
    material->diffuseColor.setValue(0.2F, 0.45F, 0.85F);
    material->emissiveColor.setValue(0.1F, 0.25F, 0.5F);
    coil->addChild(material);
    auto* style = new SoDrawStyle;
    style->lineWidth = 2.0F;
    coil->addChild(style);
    coords = new SoCoordinate3;
    coil->addChild(coords);
    line = new SoLineSet;
    coil->addChild(line);
    addDisplayMaskMode(coil, "Coil");
    if (obj->getDocument()) {
        connChanged = obj->getDocument()->signalChangedObject.connect(
            [this](const App::DocumentObject& changed, const App::Property& prop) {
                onChangedObject(changed, prop);
            });
    }
    redraw();
}

std::vector<std::string> ViewProviderSpringDamper::getDisplayModes() const
{
    return {"Coil"};
}

void ViewProviderSpringDamper::setDisplayMode(const char* mode)
{
    setDisplayMaskMode("Coil");
    ViewProviderDocumentObject::setDisplayMode(mode);
}

QIcon ViewProviderSpringDamper::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_SpringDamper.svg");
}

std::vector<App::DocumentObject*> ViewProviderSpringDamper::claimChildren() const
{
    std::vector<App::DocumentObject*> children;
    if (auto* spring = freecad_cast<Assembly::SpringDamper*>(getObject())) {
        for (int side : {1, 2}) {
            if (auto* binding = spring->getBinding(side)) {
                children.push_back(binding);
            }
        }
    }
    return children;
}

bool ViewProviderSpringDamper::doubleClicked()
{
    auto* obj = getObject();
    if (!obj || !obj->isAttachedToDocument()) {
        return false;
    }
    Gui::Command::doCommand(Gui::Command::Gui,
                            "import CommandCreateSpring\n"
                            "CommandCreateSpring.editSpring("
                            "App.getDocument('%s').getObject('%s'))",
                            obj->getDocument()->getName(),
                            obj->getNameInDocument());
    return true;
}

void ViewProviderSpringDamper::updateData(const App::Property* prop)
{
    ViewProviderDocumentObject::updateData(prop);
    redraw();
}

void ViewProviderSpringDamper::onChangedObject(const App::DocumentObject& obj,
                                               const App::Property& prop)
{
    auto* spring = freecad_cast<Assembly::SpringDamper*>(getObject());
    if (!spring || !prop.isDerivedFrom<App::PropertyPlacement>()) {
        return;
    }
    auto* assembly = App::GeoFeatureGroupExtension::getGroupOfObject(spring);
    Base::Vector3d point;
    if (holds(assembly, spring->getEnd(1, point), &obj)
        || holds(assembly, spring->getEnd(2, point), &obj)) {
        redraw();
    }
}

void ViewProviderSpringDamper::redraw()
{
    auto* spring = freecad_cast<Assembly::SpringDamper*>(getObject());
    if (!spring || !coords) {
        return;
    }
    const Base::Vector3d a = spring->getEndPoint(1);
    const Base::Vector3d b = spring->getEndPoint(2);
    Base::Vector3d axis = b - a;
    const double length = axis.Length();
    std::vector<SbVec3f> points;
    if (length < 1e-9) {
        points = {SbVec3f(a.x, a.y, a.z), SbVec3f(b.x, b.y, b.z)};
    }
    else {
        axis = axis / length;
        // Two directions across the axis
        Base::Vector3d across =
            std::abs(axis.z) < 0.9 ? Base::Vector3d(0, 0, 1) : Base::Vector3d(1, 0, 0);
        across = (across - axis * axis.Dot(across)).Normalize();
        const Base::Vector3d other = axis.Cross(across);
        // Straight for a tenth at each end, a coil of eight turns between
        const double radius = std::clamp(0.08 * length, 0.5, 5.0);
        constexpr int turns = 8;
        constexpr int perTurn = 16;
        auto push = [&](const Base::Vector3d& p) {
            points.emplace_back(static_cast<float>(p.x),
                                static_cast<float>(p.y),
                                static_cast<float>(p.z));
        };
        push(a);
        for (int i = 0; i <= turns * perTurn; ++i) {
            const double u = static_cast<double>(i) / (turns * perTurn);
            const double angle = 2.0 * std::numbers::pi * turns * u;
            const double fade = std::min(1.0, std::min(u, 1.0 - u) * 8.0);
            push(a + axis * (length * (0.1 + 0.8 * u))
                 + (across * std::cos(angle) + other * std::sin(angle)) * (radius * fade));
        }
        push(b);
    }
    coords->point.setNum(static_cast<int>(points.size()));
    coords->point.setValues(0, static_cast<int>(points.size()), points.data());
    line->numVertices.setValue(static_cast<int>(points.size()));
}
