// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#pragma once

// The 3D sketch's view provider and edit mode (docs/Sketch3D.md section 7). Outside
// an edit it shows the sketch's wire as any Part shape; in an edit it draws the
// geometry as the 2D sketch draws its edit geometry, brings up the Sketcher's
// edit-mode toolbars from whatever workbench is active, and places points Fusion's
// way: a manipulator at the last point, its active plane where a click lands.

#include <memory>

#include <Inventor/SbVec2s.h>

#include <App/PropertyPythonObject.h>
#include <App/PropertyStandard.h>
#include <Base/Placement.h>
#include <Mod/Part/Gui/ViewProvider.h>
#include <Mod/Sketcher/SketcherGlobal.h>

class SoSeparator;
class SoDragger;

namespace Gui
{
class Document;
}

namespace Sketcher
{
class Sketch3DObject;
}

namespace SketcherGui
{

class SketcherGuiExport ViewProviderSketch3D: public PartGui::ViewProviderPart
{
    PROPERTY_HEADER_WITH_OVERRIDE(SketcherGui::ViewProviderSketch3D);

public:
    ViewProviderSketch3D();
    ~ViewProviderSketch3D() override;

    /// The workbench an edit switches to, as the 2D sketch's property of the same name.
    App::PropertyString EditingWorkbench;
    /// Hide what depends on the sketch while it is edited (a pipe along it).
    App::PropertyBool HideDependent;
    /// The visibility automation's state during an edit (Show.TempoVis).
    App::PropertyPythonObject TempoVis;

    Sketcher::Sketch3DObject* getSketch3DObject() const;
    PyObject* getPyObject() override;

    bool doubleClicked() override;
    void updateData(const App::Property* prop) override;

    /// In an edit of the sketch, not moving it in Transform.
    bool isEditingSketch() const;

    /// Redraw the edit geometry from the object.
    void draw();

    /// The edit scene's root, for tests and the tools: null outside an edit.
    SoSeparator* getEditRoot() const;

    /** @name The manipulator (docs/Sketch3D.md section 7)
     * Its frame is in the sketch's frame. The active plane is its XY, YZ or ZX; a
     * click lands there, on one of its axes when the pointer runs along it, or on the
     * plane's normal through a point when the height is locked.
     */
    //@{
    enum class Plane
    {
        XY,
        YZ,
        ZX
    };
    Base::Vector3d manipulatorOrigin() const;
    Base::Rotation manipulatorRotation() const;
    Plane activePlane() const;
    void setManipulator(const Base::Vector3d& origin, const Base::Rotation& rotation);
    /// XY -> YZ -> ZX (Sketcher_Sketch3DCyclePlane).
    void cycleWorkingPlane();
    /// Back to the sketch frame's orientation (Sketcher_Sketch3DResetManipulator).
    void resetManipulator();
    /// Keep the in-plane position, move along the normal (Sketcher_Sketch3DLockHeight).
    void toggleHeightLock();
    bool heightLocked() const;
    /// The point the height lock keeps the in-plane position of.
    Base::Vector3d heightBase() const;
    void setRotationHandlesShown(bool shown);
    bool rotationHandlesShown() const;
    /// The active plane as a placement in the sketch's frame (its XY is the plane).
    Base::Placement workingPlane() const;
    //@}

    /** @name Drawing tools (section 7: point, line, polyline, the two splines)
     * Started by the Sketcher's own commands when a 3D sketch is in edit. A click
     * places a point where pointAt() lands it, or snapped: to a sketch point
     * (coincident), a sketch curve (point on object), or the model element under the
     * pointer (made an external reference, then coincident or point on object). Escape
     * or a right click finishes what is placed; with nothing placed, it ends the tool.
     */
    //@{
    enum class Tool
    {
        None,
        Point,
        Line,
        Polyline,
        BSplineThrough,
        BSplineControl
    };
    void startTool(Tool tool);
    Tool activeTool() const;
    /// Commits what is placed (a polyline's chain, a spline's points); with nothing
    /// placed, ends the tool.
    void finishTool();
    /// Ends the tool, dropping a spline's points not yet made.
    void cancelTool();
    /// The number of points placed and not yet made into geometry.
    int placedCount() const;
    bool constructionMode() const;
    void setConstructionMode(bool construction);
    //@}

    /** @name Input, in view coordinates (Coin's: origin bottom left)
     * The mouse handlers and the Python binding come through these, so a test drives
     * what a click does.
     */
    //@{
    /// Where a click at pos lands, in the sketch's frame.
    bool pointAt(const SbVec2s& pos, Base::Vector3d& point) const;
    /// A point of the sketch's frame in view coordinates.
    bool toView(const Base::Vector3d& point, SbVec2s& pos) const;
    /// A left press: with a tool, places a point; without, picks a plane handle or
    /// moves the manipulator to a sketch point.
    bool press(const SbVec2s& pos);
    /// A right press: finishes the tool's placing, as Escape does.
    bool pressRight(const SbVec2s& pos);
    bool move(const SbVec2s& pos);
    //@}

protected:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
    void setEditViewer(Gui::ViewerContext* viewer, int ModNum) override;
    void unsetEditViewer(Gui::ViewerContext* viewer) override;
    bool keyPressed(bool pressed, int key) override;
    bool mouseMove(const SbVec2s& cursorPos, Gui::ViewerContext* viewer) override;
    bool mouseButtonPressed(int button,
                            bool pressed,
                            const SbVec2s& cursorPos,
                            const Gui::ViewerContext* viewer) override;

private:
    struct EditData;
    std::unique_ptr<EditData> edit;
    /// The tools make construction geometry (Sketcher_ToggleConstruction).
    bool construction {false};

    struct Snap;
    struct Placed;
    Base::Matrix4D editingTransform() const;
    void updateManipulator();
    bool place(const SbVec2s& pos, Placed& placed) const;
    /// The drawn sketch point within the pick radius of pos, nearest first, or -1.
    int pointNear(const SbVec2s& pos) const;
    /// The plane handle (0 XY, 1 YZ, 2 ZX) under pos, or -1.
    int planeHandleAt(const SbVec2s& pos) const;
    void addPlaced(const Placed& placed);
    /// The constraint that ties (geoId, pos) where the snap put it.
    void constrainTo(const Snap& snap, int geoId, int pos);
    void makeSpline();
    void updatePreview();
    void showToolStatus();
    static void draggerChanged(void* data, SoDragger* dragger);
};

/// A 3D sketch is in edit in the document (not moving in Transform).
SketcherGuiExport bool isSketch3DInEdit(Gui::Document* doc);
/// The 3D sketch view provider in edit in the active document, or null.
SketcherGuiExport ViewProviderSketch3D* activeSketch3DInEdit();

}  // namespace SketcherGui
