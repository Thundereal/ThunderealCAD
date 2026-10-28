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

// The view providers of the assembly's Physics items (docs/AssemblyDynamics.md
// sec 9).

#pragma once

#include <fastsignals/signal.h>

#include <Gui/ViewProviderDocumentObject.h>

#include <Mod/Assembly/AssemblyGlobal.h>

class SoCoordinate3;
class SoLineSet;

namespace AssemblyGui
{

/// A mass override: its icon
class AssemblyGuiExport ViewProviderMassOverride: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderMassOverride);

public:
    QIcon getIcon() const override;
};

/** A spring and damper, drawn as a coil between its ends, in the assembly's
 * coordinates; redrawn as its ends' components move -- dragged, solved, or
 * played back.
 */
class AssemblyGuiExport ViewProviderSpringDamper: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderSpringDamper);

public:
    ViewProviderSpringDamper();
    ~ViewProviderSpringDamper() override;

    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* mode) override;
    QIcon getIcon() const override;

private:
    void redraw();
    void onChangedObject(const App::DocumentObject& obj, const App::Property& prop);

    SoCoordinate3* coords = nullptr;
    SoLineSet* line = nullptr;
    fastsignals::scoped_connection connChanged;
};

}  // namespace AssemblyGui
