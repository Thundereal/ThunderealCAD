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

#pragma once

#include <string>
#include <vector>

#include <fastsignals/signal.h>

#include <Base/Placement.h>
#include <Mod/Assembly/AssemblyGlobal.h>

#include <Gui/ViewProviderDocumentObject.h>
#include <Gui/ViewProviderFeaturePython.h>


class SoGroup;
class SoPickStyle;
class SoSwitch;
class SoTransform;

namespace AssemblyGui
{

/** The view provider of a C++ joint (Assembly::Joint)
 *
 * The joint holds its element links, and a multiplied joint its instance
 * joints: it claims them in the tree. Claimed in C++, not by the joint's
 * Python view provider: a group's tree export asks every child for its
 * claims, and an assembly's children are its joints, so a Python call per
 * joint for each change of the assembly's Group made building joints
 * quadratic (docs/AssemblyAudit.md sec 8.2, Phase 2). The markers come with
 * the Python view provider of Assembly::JointPython, the proxy of
 * ViewProviderJointPython, whose claimChildren is not asked here; it claims
 * the same for a joint whose file states the generic Python view provider.
 *
 * It draws the markers too, a joint coordinate system at each side's frame
 * and at each pair of a multiplied joint's instances, in the display mode
 * "Wireframe". The Python view provider made three Coin subgraphs of its own
 * per joint, at every joint made and every document opened, and redrew them
 * through a Python call per joint each solve and drag step
 * (docs/AssemblyAudit.md, F5 and F8). Here every marker of every joint shares
 * one geometry, and a solve says it moved the frames by the joint's
 * signalFramesMoved.
 */
class AssemblyGuiExport ViewProviderJoint: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderJoint);

public:
    ViewProviderJoint();
    ~ViewProviderJoint() override;

    std::vector<App::DocumentObject*> claimChildren() const override;

    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;
    std::vector<std::string> getDisplayModes() const override;
    const char* getDefaultDisplayMode() const override;
    void setDisplayMode(const char* mode) override;

    /// Draw the markers where the joint's frames are now
    void redrawMarkers();
    /// Whether the markers can be picked, as the joint task sets it while it
    /// picks
    void setMarkersPickable(bool pickable);

private:
    /// One marker: a switch over the transform and the shared geometry
    SoSwitch* makeMarker();
    static void placeMarker(SoSwitch* marker, const Base::Placement* placement);

    SoGroup* pcMarkers = nullptr;
    SoPickStyle* pcPick = nullptr;
    /// The two sides, then each instance's two
    std::vector<SoSwitch*> markers;
    fastsignals::scoped_connection connFramesMoved;
};

using ViewProviderJointPython = Gui::ViewProviderFeaturePythonT<ViewProviderJoint>;

}  // namespace AssemblyGui
