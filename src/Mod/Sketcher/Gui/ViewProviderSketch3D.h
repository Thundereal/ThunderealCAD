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
// geometry as the 2D sketch draws its edit geometry, and brings up the Sketcher's
// edit-mode toolbars from whatever workbench is active, as the 2D sketch does.

#include <memory>

#include <App/PropertyPythonObject.h>
#include <App/PropertyStandard.h>
#include <Mod/Part/Gui/ViewProvider.h>
#include <Mod/Sketcher/SketcherGlobal.h>

class SoSeparator;

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

    bool doubleClicked() override;
    void updateData(const App::Property* prop) override;

    /// In an edit of the sketch, not moving it in Transform.
    bool isEditingSketch() const;

    /// Redraw the edit geometry from the object.
    void draw();

    /// The edit scene's root, for tests and the tools: null outside an edit.
    SoSeparator* getEditRoot() const;

protected:
    bool setEdit(int ModNum) override;
    void unsetEdit(int ModNum) override;
    void setEditViewer(Gui::ViewerContext* viewer, int ModNum) override;
    void unsetEditViewer(Gui::ViewerContext* viewer) override;

private:
    struct EditData;
    std::unique_ptr<EditData> edit;
};

/// A 3D sketch is in edit in the document (not moving in Transform).
SketcherGuiExport bool isSketch3DInEdit(Gui::Document* doc);

}  // namespace SketcherGui
