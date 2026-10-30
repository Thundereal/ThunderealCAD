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
#include <string>
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

PROPERTY_SOURCE(AssemblyGui::ViewProviderFramedItem, Gui::ViewProviderDocumentObject)

ViewProviderFramedItem::~ViewProviderFramedItem()
{
    connChanged.disconnect();
}

void ViewProviderFramedItem::attach(App::DocumentObject* obj)
{
    ViewProviderDocumentObject::attach(obj);
    auto* lines = new SoSeparator;
    auto* material = new SoMaterial;
    const SbColor color = lineColor();
    material->diffuseColor.setValue(color);
    material->emissiveColor.setValue(color * 0.6F);
    lines->addChild(material);
    auto* style = new SoDrawStyle;
    style->lineWidth = 3.0F;
    lines->addChild(style);
    coords = new SoCoordinate3;
    lines->addChild(coords);
    line = new SoLineSet;
    lines->addChild(line);
    addDisplayMaskMode(lines, "Lines");
    if (obj->getDocument()) {
        connChanged = obj->getDocument()->signalChangedObject.connect(
            [this](const App::DocumentObject& changed, const App::Property& prop) {
                onChangedObject(changed, prop);
            });
    }
    redraw();
}

std::vector<std::string> ViewProviderFramedItem::getDisplayModes() const
{
    return {"Lines"};
}

void ViewProviderFramedItem::setDisplayMode(const char* mode)
{
    setDisplayMaskMode("Lines");
    ViewProviderDocumentObject::setDisplayMode(mode);
}

std::vector<App::DocumentObject*> ViewProviderFramedItem::claimChildren() const
{
    std::vector<App::DocumentObject*> children;
    if (auto* item = freecad_cast<Assembly::FramedItem*>(getObject())) {
        if (auto* binding = item->getBinding()) {
            children.push_back(binding);
        }
    }
    return children;
}

const char* ViewProviderFramedItem::editCall() const
{
    return nullptr;
}

bool ViewProviderFramedItem::doubleClicked()
{
    auto* obj = getObject();
    const char* call = editCall();
    if (!obj || !obj->isAttachedToDocument() || !call) {
        return false;
    }
    const std::string function = call;
    Gui::Command::doCommand(Gui::Command::Gui,
                            "import %s\n%s(App.getDocument('%s').getObject('%s'))",
                            function.substr(0, function.find('.')).c_str(),
                            call,
                            obj->getDocument()->getName(),
                            obj->getNameInDocument());
    return true;
}

void ViewProviderFramedItem::updateData(const App::Property* prop)
{
    ViewProviderDocumentObject::updateData(prop);
    redraw();
}

void ViewProviderFramedItem::onChangedObject(const App::DocumentObject& obj,
                                             const App::Property& prop)
{
    auto* item = freecad_cast<Assembly::FramedItem*>(getObject());
    if (!item || !prop.isDerivedFrom<App::PropertyPlacement>()) {
        return;
    }
    auto* assembly = App::GeoFeatureGroupExtension::getGroupOfObject(item);
    Base::Placement frame;
    if (holds(assembly, item->getEnd(frame), &obj)) {
        redraw();
    }
}

SbColor ViewProviderFramedItem::lineColor() const
{
    return {0.85F, 0.15F, 0.1F};
}

void ViewProviderFramedItem::makeLines(std::vector<SbVec3f>& /*points*/,
                                       std::vector<int32_t>& /*counts*/) const
{}

void ViewProviderFramedItem::redraw()
{
    if (!coords || !getObject()) {
        return;
    }
    std::vector<SbVec3f> points;
    std::vector<int32_t> counts;
    makeLines(points, counts);
    coords->point.setNum(static_cast<int>(points.size()));
    coords->point.setValues(0, static_cast<int>(points.size()), points.data());
    line->numVertices.setNum(static_cast<int>(counts.size()));
    line->numVertices.setValues(0, static_cast<int>(counts.size()), counts.data());
}

PROPERTY_SOURCE(AssemblyGui::ViewProviderAppliedLoad, AssemblyGui::ViewProviderFramedItem)

ViewProviderAppliedLoad::ViewProviderAppliedLoad()
{
    ADD_PROPERTY_TYPE(ArrowLength, (20.0), "Display", App::Prop_None, "The arrow's length");
}

QIcon ViewProviderAppliedLoad::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_AppliedLoad.svg");
}

const char* ViewProviderAppliedLoad::editCall() const
{
    return "CommandCreateLoad.editLoad";
}

void ViewProviderAppliedLoad::onChanged(const App::Property* prop)
{
    if (prop == &ArrowLength) {
        redraw();
    }
    ViewProviderFramedItem::onChanged(prop);
}

void ViewProviderAppliedLoad::makeLines(std::vector<SbVec3f>& points,
                                        std::vector<int32_t>& counts) const
{
    auto* load = freecad_cast<Assembly::AppliedLoad*>(getObject());
    if (!load) {
        return;
    }
    const Base::Vector3d at = load->getEndPlacement().getPosition();
    const Base::Vector3d axis = load->getDirectionNow();
    auto push = [&](const Base::Vector3d& p) {
        points.emplace_back(static_cast<float>(p.x),
                            static_cast<float>(p.y),
                            static_cast<float>(p.z));
    };
    const double length = std::max(ArrowLength.getValue(), 1e-3);
    if (axis.Length() < 0.5) {
        // No direction: a dot's worth of a line
        push(at);
        push(at);
        counts.push_back(2);
    }
    else {
        Base::Vector3d across =
            std::abs(axis.z) < 0.9 ? Base::Vector3d(0, 0, 1) : Base::Vector3d(1, 0, 0);
        across = (across - axis * axis.Dot(across)).Normalize();
        const Base::Vector3d other = axis.Cross(across);
        const double head = 0.25 * length;
        // The head's four barbs back from a tip, against a direction
        auto barbs = [&](const Base::Vector3d& tip,
                         const Base::Vector3d& along,
                         const Base::Vector3d& side1,
                         const Base::Vector3d& side2) {
            for (const Base::Vector3d& side : {side1, side2, side1 * -1.0, side2 * -1.0}) {
                push(tip);
                push(tip - along * head + side * (0.4 * head));
                counts.push_back(2);
            }
        };
        if (!load->isTorque()) {
            // On the frame's outer side (its z, a face's outward normal), so
            // that a face does not hide it: a pull starts at the point it
            // acts at, a push ends there
            const Base::Vector3d outward =
                load->getEndPlacement().getRotation().multVec(Base::Vector3d(0, 0, 1));
            const Base::Vector3d tail = axis.Dot(outward) >= 0.0 ? at : at - axis * length;
            push(tail);
            push(tail + axis * length);
            counts.push_back(2);
            barbs(tail + axis * length, axis, across, other);
        }
        else {
            // The axis, and three quarters of a turn about it, the head
            // turning the positive way (right hand)
            push(at - axis * (0.5 * length));
            push(at + axis * (0.5 * length));
            counts.push_back(2);
            const double radius = 0.35 * length;
            constexpr int steps = 36;
            const double sweep = 1.5 * std::numbers::pi;
            for (int i = 0; i <= steps; ++i) {
                const double angle = sweep * i / steps;
                push(at + (across * std::cos(angle) + other * std::sin(angle)) * radius);
            }
            counts.push_back(steps + 1);
            const Base::Vector3d tip =
                at + (across * std::cos(sweep) + other * std::sin(sweep)) * radius;
            const Base::Vector3d tangent = across * -std::sin(sweep) + other * std::cos(sweep);
            barbs(tip, tangent, axis, tangent.Cross(axis));
        }
    }
}

PROPERTY_SOURCE(AssemblyGui::ViewProviderBushing, AssemblyGui::ViewProviderFramedItem)

ViewProviderBushing::ViewProviderBushing()
{
    ADD_PROPERTY_TYPE(Size, (12.0), "Display", App::Prop_None, "The ring's outer diameter");
}

QIcon ViewProviderBushing::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_Bushing.svg");
}

SbColor ViewProviderBushing::lineColor() const
{
    return {0.1F, 0.55F, 0.2F};
}

const char* ViewProviderBushing::editCall() const
{
    return "CommandCreateBushing.editBushing";
}

void ViewProviderBushing::onChanged(const App::Property* prop)
{
    if (prop == &Size) {
        redraw();
    }
    ViewProviderFramedItem::onChanged(prop);
}

void ViewProviderBushing::makeLines(std::vector<SbVec3f>& points,
                                    std::vector<int32_t>& counts) const
{
    auto* bushing = freecad_cast<Assembly::Bushing*>(getObject());
    if (!bushing) {
        return;
    }
    const Base::Placement frame = bushing->getEndPlacement();
    auto push = [&](double x, double y, double z) {
        Base::Vector3d p;
        frame.multVec(Base::Vector3d(x, y, z), p);
        points.emplace_back(static_cast<float>(p.x),
                            static_cast<float>(p.y),
                            static_cast<float>(p.z));
    };
    // The outer and inner rings, the spokes of rubber between them, and the
    // axis through the sleeve
    const double outer = 0.5 * std::max(Size.getValue(), 1e-3);
    const double inner = 0.45 * outer;
    constexpr int steps = 48;
    for (double radius : {outer, inner}) {
        for (int i = 0; i <= steps; ++i) {
            const double angle = 2.0 * std::numbers::pi * i / steps;
            push(radius * std::cos(angle), radius * std::sin(angle), 0.0);
        }
        counts.push_back(steps + 1);
    }
    constexpr int spokes = 8;
    for (int i = 0; i < spokes; ++i) {
        const double angle = 2.0 * std::numbers::pi * (i + 0.5) / spokes;
        push(inner * std::cos(angle), inner * std::sin(angle), 0.0);
        push(outer * std::cos(angle), outer * std::sin(angle), 0.0);
        counts.push_back(2);
    }
    push(0.0, 0.0, -0.5 * outer);
    push(0.0, 0.0, 0.5 * outer);
    counts.push_back(2);
}

PROPERTY_SOURCE(AssemblyGui::ViewProviderCamContact, Gui::ViewProviderDocumentObject)

QIcon ViewProviderCamContact::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_CamContact.svg");
}

PROPERTY_SOURCE(AssemblyGui::ViewProviderContactSet, Gui::ViewProviderDocumentObject)

QIcon ViewProviderContactSet::getIcon() const
{
    return Gui::BitmapFactory().pixmap("Assembly_Contact.svg");
}

bool ViewProviderContactSet::doubleClicked()
{
    auto* obj = getObject();
    if (!obj || !obj->isAttachedToDocument()) {
        return false;
    }
    Gui::Command::doCommand(Gui::Command::Gui,
                            "import CommandCreateContact\n"
                            "CommandCreateContact.editContactSet("
                            "App.getDocument('%s').getObject('%s'))",
                            obj->getDocument()->getName(),
                            obj->getNameInDocument());
    return true;
}
