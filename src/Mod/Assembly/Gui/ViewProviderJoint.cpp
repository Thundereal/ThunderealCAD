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

#include "PreCompiled.h"

#include <cmath>
#include <cstring>
#include <numbers>

#include <Inventor/nodes/SoAnnotation.h>
#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoDrawStyle.h>
#include <Inventor/nodes/SoFaceSet.h>
#include <Inventor/nodes/SoLineSet.h>
#include <Inventor/nodes/SoMaterial.h>
#include <Inventor/nodes/SoPickStyle.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoSwitch.h>
#include <Inventor/nodes/SoTransform.h>

#include <App/Application.h>
#include <Base/Parameter.h>
#include <Gui/SoAxisCrossKit.h>
#include <Gui/SoFCSelection.h>
#include <Mod/Assembly/App/Joint.h>
#include <Mod/Part/App/FeatureElement.h>

#include "ViewProviderJoint.h"

using namespace AssemblyGui;

namespace
{

// As SoSwitchMarker.py draws a joint coordinate system: a translucent disc,
// the X and Y axes from half way, the Z axis whole, kept a size on screen
constexpr float AxisThickness = 3.0F;
constexpr float ScaleFactor = 40.0F;

SoBaseColor* axisColor(const char* name, unsigned long fallback)
{
    auto params = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/View"
    );
    const unsigned long packed = params->GetUnsigned(name, fallback);
    auto color = new SoBaseColor;
    color->rgb.setValue(
        static_cast<float>((packed >> 24) & 0xff) / 255.0F,
        static_cast<float>((packed >> 16) & 0xff) / 255.0F,
        static_cast<float>((packed >> 8) & 0xff) / 255.0F
    );
    return color;
}

SoNode* scaled(SoNode* shape)
{
    auto scale = new Gui::SoShapeScale;
    scale->setPart("shape", shape);
    scale->scaleFactor = ScaleFactor;
    return scale;
}

SoNode* axis(SoDrawStyle* style, SoBaseColor* color, const SbVec3f& from, const SbVec3f& to)
{
    auto coords = new SoCoordinate3;
    coords->point.set1Value(0, from);
    coords->point.set1Value(1, to);
    auto line = new SoLineSet;
    line->numVertices.setValue(2);
    auto sep = new SoAnnotation;
    sep->addChild(style);
    sep->addChild(color);
    sep->addChild(coords);
    sep->addChild(line);
    return scaled(sep);
}

SoNode* disc(float size, int count)
{
    auto coords = new SoCoordinate3;
    for (int i = 0; i < count; ++i) {
        const double angle = static_cast<double>(i) / count * 2.0 * std::numbers::pi;
        coords->point.set1Value(
            i,
            static_cast<float>(std::cos(angle) * size),
            static_cast<float>(std::sin(angle) * size),
            0.0F
        );
    }
    auto face = new SoFaceSet;
    face->numVertices.setValue(count);
    auto style = new SoDrawStyle;
    style->style = SoDrawStyle::FILLED;
    auto material = new SoMaterial;
    material->diffuseColor.setValue(0.5F, 0.5F, 0.5F);
    material->ambientColor.setValue(0.5F, 0.5F, 0.5F);
    material->specularColor.setValue(0.5F, 0.5F, 0.5F);
    material->emissiveColor.setValue(0.5F, 0.5F, 0.5F);
    material->transparency.setValue(0.3F);
    auto sep = new SoAnnotation;
    sep->addChild(style);
    sep->addChild(material);
    sep->addChild(coords);
    sep->addChild(face);
    return scaled(sep);
}

/// The geometry every marker of every joint shows: made once, kept for good
SoSeparator* markerGeometry()
{
    static SoSeparator* geometry = nullptr;
    if (!geometry) {
        geometry = new SoSeparator;
        geometry->ref();
        auto style = new SoDrawStyle;
        style->style = SoDrawStyle::LINES;
        style->lineWidth = AxisThickness;
        geometry->addChild(disc(0.4F, 15));
        geometry->addChild(
            axis(style, axisColor("AxisXColor", 0xCC333300), SbVec3f(0.5F, 0, 0), SbVec3f(1, 0, 0))
        );
        geometry->addChild(
            axis(style, axisColor("AxisYColor", 0x33CC3300), SbVec3f(0, 0.5F, 0), SbVec3f(0, 1, 0))
        );
        geometry->addChild(
            axis(style, axisColor("AxisZColor", 0x3333CC00), SbVec3f(0, 0, 0), SbVec3f(0, 0, 1))
        );
    }
    return geometry;
}

}  // namespace

PROPERTY_SOURCE(AssemblyGui::ViewProviderJoint, Gui::ViewProviderDocumentObject)

ViewProviderJoint::ViewProviderJoint()
{
    pcMarkers = new Gui::SoFCSelection;
    pcMarkers->ref();
    pcPick = new SoPickStyle;
    pcPick->ref();
    pcPick->style = SoPickStyle::SHAPE_ON_TOP;
}

ViewProviderJoint::~ViewProviderJoint()
{
    pcPick->unref();
    pcMarkers->unref();
}

void ViewProviderJoint::attach(App::DocumentObject* obj)
{
    Gui::ViewProviderDocumentObject::attach(obj);
    addDisplayMaskMode(pcMarkers, "Wireframe");
    if (auto joint = Base::freecad_dynamic_cast<Assembly::Joint>(obj)) {
        connFramesMoved = joint->signalFramesMoved.connect([this]() { redrawMarkers(); });
    }
    redrawMarkers();
}

std::vector<std::string> ViewProviderJoint::getDisplayModes() const
{
    auto modes = Gui::ViewProviderDocumentObject::getDisplayModes();
    modes.emplace_back("Wireframe");
    return modes;
}

const char* ViewProviderJoint::getDefaultDisplayMode() const
{
    return "Wireframe";
}

void ViewProviderJoint::setDisplayMode(const char* mode)
{
    if (mode && std::strcmp(mode, "Wireframe") == 0) {
        setDisplayMaskMode("Wireframe");
    }
    Gui::ViewProviderDocumentObject::setDisplayMode(mode);
}

void ViewProviderJoint::updateData(const App::Property* prop)
{
    Gui::ViewProviderDocumentObject::updateData(prop);
    auto joint = Base::freecad_dynamic_cast<Assembly::Joint>(getObject());
    if (joint
        && (prop == &joint->Placement1 || prop == &joint->Placement2 || prop == &joint->Binding1
            || prop == &joint->Binding2 || prop == &joint->_Instances)) {
        redrawMarkers();
    }
}

SoSwitch* ViewProviderJoint::makeMarker()
{
    auto sw = new SoSwitch;
    sw->whichChild = SO_SWITCH_NONE;
    auto jcs = new SoAnnotation;
    jcs->addChild(new SoTransform);
    jcs->addChild(pcPick);
    jcs->addChild(markerGeometry());
    sw->addChild(jcs);
    pcMarkers->addChild(sw);
    return sw;
}

void ViewProviderJoint::placeMarker(SoSwitch* marker, const Base::Placement* placement)
{
    if (!placement) {
        marker->whichChild = SO_SWITCH_NONE;
        return;
    }
    auto jcs = static_cast<SoAnnotation*>(marker->getChild(0));
    auto transform = static_cast<SoTransform*>(jcs->getChild(0));
    const Base::Vector3d& pos = placement->getPosition();
    double q0 {}, q1 {}, q2 {}, q3 {};
    placement->getRotation().getValue(q0, q1, q2, q3);
    transform->translation.setValue(
        static_cast<float>(pos.x),
        static_cast<float>(pos.y),
        static_cast<float>(pos.z)
    );
    transform->rotation.setValue(
        static_cast<float>(q0),
        static_cast<float>(q1),
        static_cast<float>(q2),
        static_cast<float>(q3)
    );
    marker->whichChild = SO_SWITCH_ALL;
}

void ViewProviderJoint::redrawMarkers()
{
    auto joint = Base::freecad_dynamic_cast<Assembly::Joint>(getObject());
    if (!joint || !pcMarkers) {
        return;
    }
    // The frames, in the assembly's coordinate system, under which the
    // markers are: the two sides, then each instance's
    std::vector<std::pair<bool, Base::Placement>> frames;
    for (int side : {1, 2}) {
        const bool set = joint->getBinding(side) != nullptr;
        frames.emplace_back(set, set ? joint->getFrame(side) : Base::Placement());
    }
    for (auto obj : joint->_Instances.getValues()) {
        auto instance = Base::freecad_dynamic_cast<Assembly::Joint>(obj);
        for (int side : {1, 2}) {
            if (instance) {
                frames.emplace_back(true, instance->getFrame(side));
            }
            else {
                frames.emplace_back(false, Base::Placement());
            }
        }
    }
    while (markers.size() < frames.size()) {
        markers.push_back(makeMarker());
    }
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const bool shown = i < frames.size() && frames[i].first;
        placeMarker(markers[i], shown ? &frames[i].second : nullptr);
    }
}

void ViewProviderJoint::setMarkersPickable(bool pickable)
{
    pcPick->style = pickable ? SoPickStyle::SHAPE_ON_TOP : SoPickStyle::UNPICKABLE;
}

std::vector<App::DocumentObject*> ViewProviderJoint::claimChildren() const
{
    std::vector<App::DocumentObject*> children;
    auto joint = Base::freecad_dynamic_cast<Assembly::Joint>(getObject());
    if (!joint) {
        return children;
    }
    for (int side : {1, 2}) {
        if (auto link = joint->getBinding(side)) {
            children.push_back(link);
        }
    }
    const auto& instances = joint->_Instances.getValues();
    children.insert(children.end(), instances.begin(), instances.end());
    return children;
}

namespace Gui
{
/// @cond DOXERR
PROPERTY_SOURCE_TEMPLATE(AssemblyGui::ViewProviderJointPython, AssemblyGui::ViewProviderJoint)
/// @endcond

// Not asked of the proxy: the joint's Python view provider claims the same
// for a view provider of the generic type, as a file saved before this one
// has, and the call is what this view provider is for not making
template<>
std::vector<App::DocumentObject*>
ViewProviderFeaturePythonT<AssemblyGui::ViewProviderJoint>::claimChildren() const
{
    return AssemblyGui::ViewProviderJoint::claimChildren();
}

// explicit template instantiation
template class AssemblyGuiExport ViewProviderFeaturePythonT<AssemblyGui::ViewProviderJoint>;
}  // namespace Gui
