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

#include <cstdint>
#include <vector>

#include <Inventor/SbColor.h>
#include <Inventor/SbVec3f.h>
#include <fastsignals/signal.h>

#include <App/PropertyUnits.h>
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

/// A torsion spring: its icon; its joint shows where it is
class AssemblyGuiExport ViewProviderTorsionSpring: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderTorsionSpring);

public:
    QIcon getIcon() const override;
};

/** A spring and damper, drawn as a coil between its ends, in the assembly's
 * coordinates; redrawn as its ends' components move -- dragged, solved, or
 * played back. It claims the element bindings the spring owns, and a
 * double click opens the spring's task panel (CommandCreateSpring.py).
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
    std::vector<App::DocumentObject*> claimChildren() const override;
    bool doubleClicked() override;

private:
    void redraw();
    void onChangedObject(const App::DocumentObject& obj, const App::Property& prop);

    SoCoordinate3* coords = nullptr;
    SoLineSet* line = nullptr;
    fastsignals::scoped_connection connChanged;
};

/** A Physics item at a frame (Assembly::FramedItem), drawn as lines in the
 * assembly's coordinates and redrawn as its component moves. It claims the
 * element binding the item owns, and a double click opens the item's task
 * panel.
 */
class AssemblyGuiExport ViewProviderFramedItem: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderFramedItem);

public:
    ViewProviderFramedItem() = default;
    ~ViewProviderFramedItem() override;

    void attach(App::DocumentObject* obj) override;
    void updateData(const App::Property* prop) override;
    std::vector<std::string> getDisplayModes() const override;
    void setDisplayMode(const char* mode) override;
    std::vector<App::DocumentObject*> claimChildren() const override;
    bool doubleClicked() override;

protected:
    /// The lines, in the assembly's coordinates: their points, and the
    /// number of points of each
    virtual void makeLines(std::vector<SbVec3f>& points, std::vector<int32_t>& counts) const;
    virtual SbColor lineColor() const;
    /// The Python function opening the task panel, module.function, given
    /// the object
    virtual const char* editCall() const;
    void redraw();

private:
    void onChangedObject(const App::DocumentObject& obj, const App::Property& prop);

    SoCoordinate3* coords = nullptr;
    SoLineSet* line = nullptr;
    fastsignals::scoped_connection connChanged;
};

/** An applied load: a force as an arrow from or to the point it acts at, on
 * the frame's outer side (a face's outward normal), a torque as a curled
 * arrow about its axis (CommandCreateLoad.py's panel).
 */
class AssemblyGuiExport ViewProviderAppliedLoad: public ViewProviderFramedItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderAppliedLoad);

public:
    ViewProviderAppliedLoad();

    /// The arrow's length
    App::PropertyLength ArrowLength;

    void onChanged(const App::Property* prop) override;
    QIcon getIcon() const override;

protected:
    void makeLines(std::vector<SbVec3f>& points, std::vector<int32_t>& counts) const override;
    const char* editCall() const override;
};

/** A bushing: a ring of rubber in the frame's xy plane, about its z
 * (CommandCreateBushing.py's panel).
 */
class AssemblyGuiExport ViewProviderBushing: public ViewProviderFramedItem
{
    PROPERTY_HEADER_WITH_OVERRIDE(AssemblyGui::ViewProviderBushing);

public:
    ViewProviderBushing();

    /// The ring's outer diameter
    App::PropertyLength Size;

    void onChanged(const App::Property* prop) override;
    QIcon getIcon() const override;

protected:
    void makeLines(std::vector<SbVec3f>& points, std::vector<int32_t>& counts) const override;
    SbColor lineColor() const override;
    const char* editCall() const override;
};

}  // namespace AssemblyGui
